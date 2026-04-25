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


FRAG_KIND_NAMES = {
    0: "CNP_FRAG_NONE",
    1: "CNP_FRAG_FALLTHROUGH",
    2: "CNP_FRAG_JUMP_P2",
    3: "CNP_FRAG_ROW",
    4: "CNP_FRAG_TERMINAL",
}


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


def entry_text_offset(entry_rels, entry_base, field_off, entry_bytes):
    rel = entry_rels.get(entry_base + field_off)
    if rel is not None:
        if rel["sym_name"] != ".text" and rel["sym_section"] != ".text":
            raise RuntimeError(
                f"unexpected fragment-entry relocation target {rel['sym_name']}"
            )
        return rel["addend"]
    return struct.unpack_from("<I", entry_bytes, entry_base + field_off)[0]


def extract_frame_info(text, func_size):
    """Extract the real function prologue and epilogue templates.

    The generated fragment source is one function whose .text starts with a
    normal stack-frame setup and ends with the matching teardown. Fragment
    bodies live in the middle, so we must not treat everything before the first
    fragment as "prologue".
    """
    i = 0
    if not text.startswith(bytes([0x55, 0x48, 0x89, 0xE5])):
        raise RuntimeError("unexpected function prologue in fragment object")
    i = 4

    while i < len(text):
        if text[i:i + 2] in (
            bytes([0x41, 0x54]),
            bytes([0x41, 0x55]),
            bytes([0x41, 0x56]),
            bytes([0x41, 0x57]),
        ):
            i += 2
            continue
        if text[i] in (0x53, 0x56, 0x57):
            i += 1
            continue
        break

    if text[i:i + 3] == bytes([0x48, 0x81, 0xEC]):
        prologue_end = i + 7
    elif text[i:i + 3] == bytes([0x48, 0x83, 0xEC]):
        prologue_end = i + 4
    else:
        raise RuntimeError("could not locate stack allocation in prologue")
    prologue = bytes(text[:prologue_end])

    retq_pos = None
    for i in range(min(func_size, len(text)) - 1, -1, -1):
        if text[i] == 0xC3:
            retq_pos = i
            break
    if retq_pos is None:
        raise RuntimeError("could not locate function retq in .text")

    epilogue_start = None
    for i in range(retq_pos - 3, -1, -1):
        if text[i:i + 3] == bytes([0x48, 0x81, 0xC4]):
            epilogue_start = i
            break
        if text[i:i + 3] == bytes([0x48, 0x83, 0xC4]):
            epilogue_start = i
            break
    if epilogue_start is None:
        raise RuntimeError("could not locate stack teardown in epilogue")

    epilogue = bytes(text[epilogue_start:retq_pos + 1])
    print(f"  prologue: {len(prologue)} bytes at text[0..{prologue_end:#x}]")
    print(f"  epilogue: {len(epilogue)} bytes at text[{epilogue_start:#x}]")
    return prologue, epilogue


def extract_fragments(obj_path, opcodes_header_path):
    opcode_names = parse_opcodes_header(opcodes_header_path)
    with open(obj_path, "rb") as f:
        elf = ELFFile(f)
        text_section = elf.get_section_by_name(".text")
        rodata_section = (elf.get_section_by_name(".rodata") or
                          elf.get_section_by_name(".data.rel.ro"))
        symtab = elf.get_section_by_name(".symtab")
        if text_section is None or rodata_section is None or symtab is None:
            missing = []
            if text_section is None:
                missing.append(".text")
            if rodata_section is None:
                missing.append(".rodata/.data.rel.ro")
            if symtab is None:
                missing.append(".symtab")
            raise RuntimeError(
                f"required ELF sections missing: {', '.join(missing)}"
            )

        text = text_section.data()
        rodata = rodata_section.data()

        entries_sym = find_local_symbol(symtab, "cnp_fragment_entries.entr")
        if entries_sym is None:
            raise RuntimeError("cnp_fragment_entries.entries symbol not found")

        text_rels = load_relocations(elf, symtab, (".rela.text", ".rel.text"))
        entry_rels = relocation_map(
            load_relocations(
                elf,
                symtab,
                (
                    f".rela{rodata_section.name}",
                    f".rel{rodata_section.name}",
                ),
            ),
            entries_sym["st_value"],
            entries_sym["st_size"],
        )

        entry_off = entries_sym["st_value"]
        entry_size = entries_sym["st_size"]
        if entry_size % 20 != 0:
            raise RuntimeError(f"unexpected fragment entry size block: {entry_size}")
        count = entry_size // 20
        entry_bytes = rodata[entry_off:entry_off + entry_size]

        fragments = []
        for i in range(count):
            base = i * 20
            opcode = struct.unpack_from("<i", entry_bytes, base)[0]
            begin = entry_text_offset(entry_rels, base, 4, entry_bytes)
            dispatch = entry_text_offset(entry_rels, base, 8, entry_bytes)
            end = entry_text_offset(entry_rels, base, 12, entry_bytes)
            kind_num = struct.unpack_from("<i", entry_bytes, base + 16)[0]
            if begin < end and text[begin] == 0x00:
                begin += 1
                dispatch += 1
                end += 1
            if not (0 <= begin <= dispatch <= end <= len(text)):
                raise RuntimeError(
                    f"invalid fragment bounds for opcode {opcode}: "
                    f"begin={begin} dispatch={dispatch} end={end}"
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

            name = opcode_names.get(opcode, f"OP_{opcode}")
            frag = {
                "opcode": opcode,
                "name": name,
                "kind": kind_num,
                "begin": begin,
                "dispatch_offset": dispatch - begin,
                "bytes": text[begin:end],
                "relocs": frag_relocs,
            }
            fragments.append(frag)
            print(
                f"  {name} (opcode {opcode}): {len(frag['bytes'])} bytes, "
                f"dispatch+{frag['dispatch_offset']}, {len(frag_relocs)} relocs"
            )

        frag_base = min(frag["begin"] for frag in fragments)
        print(f"  fragment code starts at .text offset {frag_base:#x}")
        func_size = symtab.get_symbol_by_name("cnp_fragment_entries")[0]["st_size"]
        prologue, epilogue = extract_frame_info(text, func_size)

        return fragments, prologue, epilogue


def _emit_byte_array(out, name, data):
    out.write(f"static const uint8_t {name}[] = {{")
    for i, b in enumerate(data):
        if i % 12 == 0:
            out.write("\n    ")
        out.write(f"0x{b:02x}, ")
    out.write("\n};\n\n")


def emit_header(fragments, prologue, epilogue, output_path):
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
        _emit_byte_array(out, "cnp_fragment_epilogue_bytes", epilogue)
        out.write(f"#define CNP_FRAGMENT_PROLOGUE_SIZE {len(prologue)}u\n")
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
        out.write("struct cnp_fragment {\n")
        out.write("    const uint8_t *bytes;\n")
        out.write("    uint32_t size;\n")
        out.write("    uint32_t dispatch_offset;\n")
        out.write("    enum cnp_fragment_kind kind;\n")
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
                  "{ NULL, 0, 0, CNP_FRAG_NONE, NULL, 0 };\n\n")
        out.write("static const struct cnp_fragment "
                  "cnp_fragments[CNP_FRAG_MAX_OPCODE + 1] = {\n")
        for frag in fragments:
            cname = frag["name"].replace("OP_", "").lower()
            reloc_ref = f"cnp_fragment_relocs_{cname}" if frag["relocs"] else "NULL"
            out.write(
                "    [%d] = { cnp_fragment_bytes_%s, sizeof(cnp_fragment_bytes_%s), "
                "%d, %s, %s, %d }, /* %s */\n"
                % (
                    frag["opcode"],
                    cname,
                    cname,
                    frag["dispatch_offset"],
                    FRAG_KIND_NAMES.get(frag["kind"], "CNP_FRAG_NONE"),
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
    fragments, prologue, epilogue = extract_fragments(args.input, args.opcodes_header)
    if not fragments:
        print("ERROR: No fragments found!", file=sys.stderr)
        sys.exit(1)
    emit_header(fragments, prologue, epilogue, args.output)


if __name__ == "__main__":
    main()
