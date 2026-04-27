#!/usr/bin/env python3
"""
Extract threaded CnP fragment metadata from vdbe_cnp_fragments.o.

This is separate from the wrapper-stencil extractor. It understands the
`cnp_fragment_entries()` metadata table emitted by tools/vdbe_cnp_genfrags.py
and produces a C header with fragment byte ranges and generic relocations.
"""

import argparse
import struct
import sys

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection


R_X86_64_64 = 1
R_X86_64_PC32 = 2
R_X86_64_PLT32 = 4
R_X86_64_32 = 10
R_X86_64_32S = 11

X86_PUSH_RBP_PROLOGUE = bytes([0x55, 0x48, 0x89, 0xE5])
X86_PUSH_REX_PREFIX = 0x41
X86_PUSH_R12_OPCODE = 0x54
X86_PUSH_R13_OPCODE = 0x55
X86_PUSH_R14_OPCODE = 0x56
X86_PUSH_R15_OPCODE = 0x57
X86_PUSH_RBX_OPCODE = 0x53
X86_PUSH_RSI_OPCODE = 0x56
X86_PUSH_RDI_OPCODE = 0x57
X86_SUB_RSP_IMM32 = bytes([0x48, 0x81, 0xEC])
X86_SUB_RSP_IMM8 = bytes([0x48, 0x83, 0xEC])
X86_ADD_RSP_IMM32 = bytes([0x48, 0x81, 0xC4])
X86_ADD_RSP_IMM8 = bytes([0x48, 0x83, 0xC4])
X86_RETQ_OPCODE = 0xC3


FRAG_KIND_NAMES = {
    0: "CNP_FRAG_NONE",
    1: "CNP_FRAG_FALLTHROUGH",
    2: "CNP_FRAG_JUMP_P2",
    3: "CNP_FRAG_ROW",
    4: "CNP_FRAG_TERMINAL",
}

PILOT_FRAGMENTS = [
    {"name": "OP_Integer", "kind_num": 1, "tail_fallthrough": True},
    {"name": "OP_Bool", "kind_num": 1, "tail_fallthrough": True},
    {"name": "OP_Int64", "kind_num": 1, "tail_fallthrough": True},
    {"name": "OP_Init", "kind_num": 2, "tail_fallthrough": False},
    {"name": "OP_Add", "kind_num": 1, "tail_fallthrough": True},
    {"name": "OP_Subtract", "kind_num": 1, "tail_fallthrough": True},
    {"name": "OP_Multiply", "kind_num": 1, "tail_fallthrough": True},
    {"name": "OP_Divide", "kind_num": 1, "tail_fallthrough": True},
    {"name": "OP_Remainder", "kind_num": 1, "tail_fallthrough": True},
    {"name": "OP_Goto", "kind_num": 2, "tail_fallthrough": False},
    {"name": "OP_IfNot", "kind_num": 2, "tail_fallthrough": True},
    {"name": "OP_Once", "kind_num": 2, "tail_fallthrough": True},
    {"name": "OP_Halt", "kind_num": 4, "tail_fallthrough": False},
    {"name": "OP_ResultRow", "kind_num": 3, "tail_fallthrough": True},
]


def parse_opcodes_header(path):
    opcodes = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line.startswith("#define OP_"):
                parts = line.split()
                if len(parts) >= 3:
                    try:
                        opcodes[int(parts[2])] = parts[1]
                    except ValueError:
                        pass
    return opcodes


def find_local_symbol(symtab, prefix):
    for sym in symtab.iter_symbols():
        if sym.name.startswith(prefix):
            return sym
    return None


def find_symbol(symtab, name):
    for sym in symtab.iter_symbols():
        if sym.name == name:
            return sym
    return None


def collect_fragment_symbols(symtab):
    out = {}
    prefix = "cnp_frag_sym_OP_"
    suffixes = ("_begin", "_dispatch", "_transfer", "_end")
    for sym in symtab.iter_symbols():
        name = sym.name
        if not name.startswith(prefix):
            continue
        for suffix in suffixes:
            if name.endswith(suffix):
                frag_name = name[len("cnp_frag_sym_"):-len(suffix)]
                out.setdefault(frag_name, {})[suffix[1:]] = sym["st_value"]
                break
    return out


def load_relocations(elf, symtab, section_names):
    rels = []
    for section in elf.iter_sections():
        if isinstance(section, RelocationSection) and section.name in section_names:
            for rel in section.iter_relocations():
                sym = symtab.get_symbol(rel["r_info_sym"])
                addend = rel.entry.get("r_addend", 0) if hasattr(rel, "entry") else 0
                sym_name = sym.name
                sym_type = sym["st_info"]["type"]
                sym_section = None
                if sym_type == "STT_SECTION":
                    target_section = elf.get_section(sym["st_shndx"])
                    sym_section = None if target_section is None else target_section.name
                rels.append({
                    "offset": rel["r_offset"],
                    "type": rel["r_info_type"],
                    "addend": addend,
                    "sym_name": sym_name,
                    "sym_type": sym_type,
                    "sym_section": sym_section,
                })
    return rels


def relocation_map(rels, start, size):
    out = {}
    for rel in rels:
        if start <= rel["offset"] < start + size:
            out[rel["offset"] - start] = rel
    return out


def entry_text_offset(entry_rels, entry_base, field_off, entry_bytes,
                      frag_base_text_off):
    rel = entry_rels.get(entry_base + field_off)
    if rel is not None:
        if rel["sym_name"] != ".text" and rel["sym_section"] != ".text":
            raise RuntimeError(
                f"unexpected fragment-entry relocation target {rel['sym_name']}"
            )
        return frag_base_text_off + rel["addend"]
    return frag_base_text_off + struct.unpack_from(
        "<I", entry_bytes, entry_base + field_off
    )[0]


def extract_frame_info(text, func_size, init_start=None):
    """Extract the real function prologue and epilogue templates.

    The generated fragment source is one function whose .text starts with a
    normal stack-frame setup and ends with the matching teardown. Fragment
    bodies live in the middle, so we must not treat everything before the first
    fragment as "prologue".
    """
    i = 0
    if not text.startswith(X86_PUSH_RBP_PROLOGUE):
        raise RuntimeError("unexpected function prologue in fragment object")
    i = 4

    while i < len(text):
        if text[i:i + 2] in (
            bytes([X86_PUSH_REX_PREFIX, X86_PUSH_R12_OPCODE]),
            bytes([X86_PUSH_REX_PREFIX, X86_PUSH_R13_OPCODE]),
            bytes([X86_PUSH_REX_PREFIX, X86_PUSH_R14_OPCODE]),
            bytes([X86_PUSH_REX_PREFIX, X86_PUSH_R15_OPCODE]),
        ):
            i += 2
            continue
        if text[i] in (0x50, X86_PUSH_RBX_OPCODE, X86_PUSH_RSI_OPCODE,
                       X86_PUSH_RDI_OPCODE):
            i += 1
            continue
        break

    if init_start is not None and init_start > i:
        prologue_end = init_start
    elif text[i:i + 3] == X86_SUB_RSP_IMM32:
        prologue_end = i + 7
    elif text[i:i + 3] == X86_SUB_RSP_IMM8:
        prologue_end = i + 4
    elif i >= 4:
        prologue_end = i
    else:
        raise RuntimeError("could not locate stack allocation in prologue")
    prologue = bytes(text[:prologue_end])

    retq_pos = None
    for i in range(min(func_size, len(text)) - 1, -1, -1):
        if text[i] == X86_RETQ_OPCODE:
            retq_pos = i
            break
    if retq_pos is None:
        print("  epilogue: none (fragment entry does not return)")
        return prologue, b""

    epilogue_start = None
    for i in range(retq_pos - 3, -1, -1):
        if text[i:i + 3] == X86_ADD_RSP_IMM32:
            epilogue_start = i
            break
        if text[i:i + 3] == X86_ADD_RSP_IMM8:
            epilogue_start = i
            break
    if epilogue_start is None:
        print("  epilogue: none (no explicit stack teardown in fragment entry)")
        return prologue, b""

    epilogue = bytes(text[epilogue_start:retq_pos + 1])
    print(f"  prologue: {len(prologue)} bytes at text[0..{prologue_end:#x}]")
    print(f"  epilogue: {len(epilogue)} bytes at text[{epilogue_start:#x}]")
    return prologue, epilogue


def extract_fragments(obj_path, opcodes_header_path):
    opcode_names = parse_opcodes_header(opcodes_header_path)
    opcode_by_name = {name: opcode for opcode, name in opcode_names.items()}
    with open(obj_path, "rb") as f:
        elf = ELFFile(f)
        text_section = (elf.get_section_by_name(".ltext") or
                        elf.get_section_by_name(".text"))
        symtab = elf.get_section_by_name(".symtab")
        if text_section is None or symtab is None:
            missing = []
            if text_section is None:
                missing.append(".text/.ltext")
            if symtab is None:
                missing.append(".symtab")
            raise RuntimeError(
                f"required ELF sections missing: {', '.join(missing)}"
            )

        text = text_section.data()

        text_rels = load_relocations(
            elf, symtab,
            (f".rela{text_section.name}", f".rel{text_section.name}")
        )
        frag_base_sym = find_symbol(symtab, "cnp_frag_base_label")
        frag_base_text_off = 0 if frag_base_sym is None else frag_base_sym["st_value"]
        frag_symbols = collect_fragment_symbols(symtab)
        if not frag_symbols:
            raise RuntimeError("no cnp_frag_sym_OP_* symbols found")

        fragments = []
        for frag_spec in PILOT_FRAGMENTS:
            name = frag_spec["name"]
            opcode = opcode_by_name.get(name)
            sym = frag_symbols.get(name)
            if opcode is None or sym is None:
                continue
            begin = sym["begin"]
            dispatch = sym["dispatch"]
            transfer = sym["transfer"]
            end = sym["end"]
            kind_num = frag_spec["kind_num"]
            tail_fallthrough = frag_spec["tail_fallthrough"]
            if begin < end and text[begin] == 0x00:
                begin += 1
                dispatch += 1
                transfer += 1
                end += 1
            if not (0 <= begin <= dispatch <= transfer <= end <= len(text)):
                raise RuntimeError(
                    f"invalid fragment bounds for opcode {opcode}: "
                    f"begin={begin} dispatch={dispatch} transfer={transfer} "
                    f"end={end}"
                )

            frag_relocs = []
            for rel in text_rels:
                if begin <= rel["offset"] < end:
                    frag_relocs.append({
                        "offset": rel["offset"] - begin,
                        "type": rel["type"],
                        "addend": rel["addend"],
                        "sym_name": rel["sym_name"],
                    })

            frag = {
                "opcode": opcode,
                "name": name,
                "kind": kind_num,
                "tail_fallthrough": tail_fallthrough,
                "begin": begin,
                "dispatch_offset": dispatch - begin,
                "transfer_offset": transfer - begin,
                "bytes": text[begin:end],
                "relocs": frag_relocs,
            }
            fragments.append(frag)
            print(
                f"  {name} (opcode {opcode}): {len(frag['bytes'])} bytes, "
                f"dispatch+{frag['dispatch_offset']}, "
                f"transfer+{frag['transfer_offset']}, {len(frag_relocs)} relocs"
            )

        first_begin = min(frag["begin"] for frag in fragments)
        frag_base = first_begin if frag_base_sym is None else frag_base_sym["st_value"]
        raw_init = text[frag_base:first_begin]

        # Truncate init_bytes at jmpq *rax (ff e0) — code after is dead
        # (the probe_opcode dispatch loop never runs in JIT context).
        jmpq_pos = None
        for i in range(len(raw_init) - 1):
            if raw_init[i] == 0xff and raw_init[i + 1] == 0xe0:
                jmpq_pos = i + 2
                break
        if jmpq_pos is None:
            raise RuntimeError("could not find jmpq *rax in init region")
        init_bytes = raw_init[:jmpq_pos]

        init_relocs = []
        for rel in text_rels:
            if frag_base <= rel["offset"] < frag_base + jmpq_pos:
                init_relocs.append({
                    "offset": rel["offset"] - frag_base,
                    "type": rel["type"],
                    "addend": rel["addend"],
                    "sym_name": rel["sym_name"],
                })
        print(f"  fragment code starts at .text offset {frag_base:#x}")
        print(f"  init region: {len(init_bytes)} bytes (truncated at jmpq), "
              f"{len(init_relocs)} relocs")
        func_size = symtab.get_symbol_by_name("cnp_fragment_entry")[0]["st_size"]
        prologue, epilogue = extract_frame_info(text, func_size, frag_base)

        return fragments, prologue, init_bytes, init_relocs, epilogue


def _emit_byte_array(out, name, data):
    out.write(f"static const uint8_t {name}[] = {{")
    for i, b in enumerate(data):
        if i % 12 == 0:
            out.write("\n    ")
        out.write(f"0x{b:02x}, ")
    out.write("\n};\n\n")


def emit_header(fragments, prologue, init_bytes, init_relocs, epilogue,
                output_path):
    max_opcode = max(f["opcode"] for f in fragments)
    with open(output_path, "w", encoding="utf-8") as out:
        out.write("/*\n")
        out.write(" * Auto-generated by tools/vdbe_cnp_extract_fragments.py.\n")
        out.write(" * Threaded CnP fragment metadata for pilot extraction.\n")
        out.write(" */\n\n")
        out.write("#ifndef VDBE_CNP_FRAGMENTS_H\n")
        out.write("#define VDBE_CNP_FRAGMENTS_H\n\n")
        out.write("#include <stdint.h>\n\n")
        _emit_byte_array(out, "cnp_fragment_prologue_bytes", prologue)
        _emit_byte_array(out, "cnp_fragment_init_bytes", init_bytes)
        _emit_byte_array(out, "cnp_fragment_epilogue_bytes", epilogue)
        out.write(f"#define CNP_FRAGMENT_PROLOGUE_SIZE {len(prologue)}u\n")
        out.write(f"#define CNP_FRAGMENT_INIT_SIZE {len(init_bytes)}u\n")
        out.write(f"#define CNP_FRAGMENT_EPILOGUE_SIZE {len(epilogue)}u\n\n")
        out.write("enum cnp_fragment_kind {\n")
        for num in sorted(FRAG_KIND_NAMES):
            out.write(f"    {FRAG_KIND_NAMES[num]} = {num},\n")
        out.write("};\n\n")
        out.write("struct cnp_fragment_reloc {\n")
        out.write("    uint32_t offset;\n")
        out.write("    uint8_t reloc_type;\n")
        out.write("    int32_t addend;\n")
        out.write("    const char *symbol_name;\n")
        out.write("};\n\n")
        if init_relocs:
            out.write("static const struct cnp_fragment_reloc "
                      "cnp_fragment_init_relocs[] = {\n")
            for rel in init_relocs:
                out.write(
                    "    { .offset = %d, .reloc_type = %d, .addend = %d, "
                    '.symbol_name = "%s" },\n'
                    % (rel["offset"], rel["type"], rel["addend"], rel["sym_name"])
                )
            out.write("};\n\n")
        out.write(f"#define CNP_FRAGMENT_INIT_NUM_RELOCS {len(init_relocs)}u\n\n")
        out.write("struct cnp_fragment {\n")
        out.write("    const uint8_t *bytes;\n")
        out.write("    uint32_t size;\n")
        out.write("    uint32_t dispatch_offset;\n")
        out.write("    uint32_t transfer_offset;\n")
        out.write("    enum cnp_fragment_kind kind;\n")
        out.write("    int tail_fallthrough;\n")
        out.write("    const struct cnp_fragment_reloc *relocs;\n")
        out.write("    uint32_t num_relocs;\n")
        out.write("};\n\n")
        out.write(f"#define CNP_FRAG_MAX_OPCODE {max_opcode}\n\n")

        for frag in fragments:
            cname = frag["name"].replace("OP_", "").lower()
            _emit_byte_array(out, f"cnp_fragment_bytes_{cname}", frag["bytes"])
            if frag["relocs"]:
                out.write(
                    f"static const struct cnp_fragment_reloc "
                    f"cnp_fragment_relocs_{cname}[] = {{\n"
                )
                for rel in frag["relocs"]:
                    out.write(
                        "    { .offset = %d, .reloc_type = %d, .addend = %d, "
                        '.symbol_name = "%s" },\n'
                        % (rel["offset"], rel["type"], rel["addend"], rel["sym_name"])
                    )
                out.write("};\n\n")

        out.write("static const struct cnp_fragment cnp_fragment_none = "
                  "{ NULL, 0, 0, 0, CNP_FRAG_NONE, 0, NULL, 0 };\n\n")
        out.write("static const struct cnp_fragment "
                  "cnp_fragments[CNP_FRAG_MAX_OPCODE + 1] = {\n")
        for frag in fragments:
            cname = frag["name"].replace("OP_", "").lower()
            reloc_ref = f"cnp_fragment_relocs_{cname}" if frag["relocs"] else "NULL"
            out.write(
                "    [%d] = { cnp_fragment_bytes_%s, sizeof(cnp_fragment_bytes_%s), "
                "%d, %d, %s, %d, %s, %d }, /* %s */\n"
                % (
                    frag["opcode"],
                    cname,
                    cname,
                    frag["dispatch_offset"],
                    frag["transfer_offset"],
                    FRAG_KIND_NAMES.get(frag["kind"], "CNP_FRAG_NONE"),
                    1 if frag["tail_fallthrough"] else 0,
                    reloc_ref,
                    len(frag["relocs"]),
                    frag["name"],
                )
            )
        out.write("};\n\n")
        out.write("#endif /* VDBE_CNP_FRAGMENTS_H */\n")

    print(f"Generated {output_path}")


def main():
    parser = argparse.ArgumentParser(
        description="Extract threaded CnP fragments from compiled object file"
    )
    parser.add_argument("--input", required=True, help="Input .o file")
    parser.add_argument("--output", required=True, help="Output C header")
    parser.add_argument("--opcodes-header", required=True, help="Path to opcodes.h")
    args = parser.parse_args()

    print(f"Extracting threaded fragments from {args.input}...")
    fragments, prologue, init_bytes, init_relocs, epilogue = extract_fragments(
        args.input, args.opcodes_header
    )
    if not fragments:
        print("ERROR: No fragments found!", file=sys.stderr)
        sys.exit(1)
    emit_header(fragments, prologue, init_bytes, init_relocs, epilogue, args.output)


if __name__ == "__main__":
    main()
