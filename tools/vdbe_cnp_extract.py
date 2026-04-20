#!/usr/bin/env python3
"""
Copy-and-Patch stencil extractor.

Parses a relocatable object file (compiled from vdbe_cnp_stubs.c) using
pyelftools and emits vdbe_cnp_stencils.h — a C header containing the raw
machine code bytes and relocation metadata for each stencil.

Usage:
    python3 tools/vdbe_cnp_extract.py --input vdbe_cnp_stubs.o \
        --output src/box/sql/generated/vdbe_cnp_stencils.h \
        --opcodes-header src/box/sql/opcodes.h
"""

import argparse
import os
import struct
import sys

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection


# Mapping from HOLE_* symbol names to C enum values
HOLE_MAP = {
    "HOLE_P1": "CNP_HOLE_P1",
    "HOLE_P2": "CNP_HOLE_P2",
    "HOLE_P3": "CNP_HOLE_P3",
    "HOLE_P4": "CNP_HOLE_P4",
    "HOLE_P5": "CNP_HOLE_P5",
    "HOLE_NEXT": "CNP_HOLE_NEXT",
    "HOLE_BRANCH": "CNP_HOLE_BRANCH",
    "HOLE_ERROR_EXIT": "CNP_HOLE_ERROR_EXIT",
    "HOLE_HANDLER": "CNP_HOLE_HANDLER",
}

# x86_64 relocation type constants
R_X86_64_64 = 1
R_X86_64_PC32 = 2
R_X86_64_PLT32 = 4
R_X86_64_32S = 11


def parse_opcodes_header(path):
    """Parse opcodes.h to get opcode name -> number mapping."""
    opcodes = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line.startswith("#define OP_"):
                parts = line.split()
                if len(parts) >= 3:
                    name = parts[1]  # e.g. OP_Goto
                    try:
                        num = int(parts[2])
                        opcodes[name] = num
                    except ValueError:
                        pass
    return opcodes


def extract_stencils(obj_path, opcodes_header_path):
    """
    Extract stencil data from the .o file.

    Returns a list of dicts:
      { 'name': 'OP_Goto', 'opcode_num': 9,
        'bytes': b'...', 'holes': [...] }
    """
    opcode_nums = parse_opcodes_header(opcodes_header_path)

    with open(obj_path, "rb") as f:
        elf = ELFFile(f)

        # Get .text section
        text_section = elf.get_section_by_name(".text")
        if text_section is None:
            print("ERROR: No .text section found", file=sys.stderr)
            sys.exit(1)
        text_data = text_section.data()
        text_offset = text_section["sh_offset"]

        # Get symbol table
        symtab = elf.get_section_by_name(".symtab")
        if symtab is None:
            print("ERROR: No .symtab found", file=sys.stderr)
            sys.exit(1)

        # Build symbol index: name -> (value_in_text, size)
        # For cnp_OP_* symbols, value is offset within .text
        stencil_syms = {}
        all_syms = {}
        for sym in symtab.iter_symbols():
            all_syms[sym.name] = sym
            if sym.name.startswith("cnp_OP_"):
                op_name = sym.name[4:]  # strip "cnp_" prefix
                stencil_syms[op_name] = {
                    "offset": sym["st_value"],
                    "size": sym["st_size"],
                }

        # Get relocations for .text
        rela_text = None
        for section in elf.iter_sections():
            if isinstance(section, RelocationSection):
                if section.name in (".rela.text", ".rel.text"):
                    rela_text = section
                    break

        relocations = []
        if rela_text is not None:
            for rel in rela_text.iter_relocations():
                sym = symtab.get_symbol(rel["r_info_sym"])
                addend = rel.entry.get("r_addend", 0) if hasattr(rel, "entry") else 0
                relocations.append({
                    "offset": rel["r_offset"],
                    "type": rel["r_info_type"],
                    "addend": addend,
                    "sym_name": sym.name,
                })

        # Extract each stencil
        stencils = []
        for op_name, info in sorted(stencil_syms.items(),
                                     key=lambda x: x[1]["offset"]):
            start = info["offset"]
            size = info["size"]
            if size == 0:
                # Try to infer size from next symbol or section end
                # Find the next symbol after this one
                next_start = len(text_data)
                for other_name, other_info in stencil_syms.items():
                    if other_info["offset"] > start:
                        next_start = min(next_start, other_info["offset"])
                size = next_start - start

            raw_bytes = text_data[start:start + size]

            # Find relocations within this function's range
            holes = []
            for rel in relocations:
                if start <= rel["offset"] < start + size:
                    sym_name = rel["sym_name"]
                    rel_offset = rel["offset"] - start  # relative to stencil

                    if sym_name in HOLE_MAP:
                        kind = HOLE_MAP[sym_name]
                    else:
                        # Unknown relocation — skip with warning
                        print(f"  WARNING: unknown reloc target "
                              f"'{sym_name}' in {op_name} at offset "
                              f"{rel_offset}", file=sys.stderr)
                        continue

                    holes.append({
                        "offset": rel_offset,
                        "kind": kind,
                        "addend": rel["addend"],
                        "reloc_type": rel["type"],
                        "sym_name": sym_name,
                    })

            opcode_num = opcode_nums.get(op_name, -1)
            stencils.append({
                "name": op_name,
                "opcode_num": opcode_num,
                "bytes": raw_bytes,
                "holes": holes,
            })

            print(f"  {op_name} (opcode {opcode_num}): "
                  f"{len(raw_bytes)} bytes, {len(holes)} holes")

        return stencils


def emit_header(stencils, output_path, max_opcode):
    """Emit the C header with stencil data."""
    with open(output_path, "w") as out:
        out.write("/*\n")
        out.write(" * Auto-generated by tools/vdbe_cnp_extract.py — DO NOT EDIT\n")
        out.write(" *\n")
        out.write(" * Copy-and-Patch stencil data extracted from compiled object file.\n")
        out.write(" */\n\n")
        out.write("#ifndef VDBE_CNP_STENCILS_H\n")
        out.write("#define VDBE_CNP_STENCILS_H\n\n")
        out.write("#include <stdint.h>\n\n")

        # Enums
        out.write("enum cnp_hole_kind {\n")
        out.write("    CNP_HOLE_P1,\n")
        out.write("    CNP_HOLE_P2,\n")
        out.write("    CNP_HOLE_P3,\n")
        out.write("    CNP_HOLE_P4,\n")
        out.write("    CNP_HOLE_P5,\n")
        out.write("    CNP_HOLE_NEXT,\n")
        out.write("    CNP_HOLE_BRANCH,\n")
        out.write("    CNP_HOLE_ERROR_EXIT,\n")
        out.write("    CNP_HOLE_HANDLER,\n")
        out.write("};\n\n")

        # Relocation type constants
        out.write("/* x86_64 ELF relocation types used by stencils */\n")
        out.write(f"#define CNP_R_X86_64_64    {R_X86_64_64}\n")
        out.write(f"#define CNP_R_X86_64_PC32  {R_X86_64_PC32}\n")
        out.write(f"#define CNP_R_X86_64_PLT32 {R_X86_64_PLT32}\n")
        out.write(f"#define CNP_R_X86_64_32S   {R_X86_64_32S}\n\n")

        # Structs
        out.write("struct cnp_hole {\n")
        out.write("    uint32_t offset;\n")
        out.write("    enum cnp_hole_kind kind;\n")
        out.write("    int addend;\n")
        out.write("    uint8_t reloc_type;\n")
        out.write("};\n\n")

        out.write("struct cnp_stencil {\n")
        out.write("    const uint8_t *bytes;\n")
        out.write("    uint32_t size;\n")
        out.write("    const struct cnp_hole *holes;\n")
        out.write("    uint32_t num_holes;\n")
        out.write("};\n\n")

        # Emit byte arrays and hole arrays for each stencil
        for s in stencils:
            cname = s["name"].replace("OP_", "").lower()

            # Byte array
            out.write(f"static const uint8_t cnp_bytes_{cname}[] = {{")
            for i, b in enumerate(s["bytes"]):
                if i % 12 == 0:
                    out.write("\n    ")
                out.write(f"0x{b:02x}, ")
            out.write("\n};\n\n")

            # Hole array
            if s["holes"]:
                out.write(f"static const struct cnp_hole "
                          f"cnp_holes_{cname}[] = {{\n")
                for h in s["holes"]:
                    out.write(f"    {{ .offset = {h['offset']}, "
                              f".kind = {h['kind']}, "
                              f".addend = {h['addend']}, "
                              f".reloc_type = {h['reloc_type']} }},\n")
                out.write("};\n\n")

        # Stencil table indexed by opcode number
        # We need a sentinel for "no stencil" entries
        out.write("/* Sentinel for opcodes without stencils */\n")
        out.write("static const struct cnp_stencil cnp_stencil_none = "
                  "{ NULL, 0, NULL, 0 };\n\n")

        out.write(f"#define CNP_MAX_OPCODE {max_opcode}\n\n")

        out.write("static const struct cnp_stencil "
                  "cnp_stencils[CNP_MAX_OPCODE + 1] = {\n")

        # Build a lookup by opcode number
        by_opcode = {s["opcode_num"]: s for s in stencils}

        for i in range(max_opcode + 1):
            if i in by_opcode:
                s = by_opcode[i]
                cname = s["name"].replace("OP_", "").lower()
                holes_ref = f"cnp_holes_{cname}" if s["holes"] else "NULL"
                num_holes = len(s["holes"])
                out.write(f"    [{i}] = {{ cnp_bytes_{cname}, "
                          f"sizeof(cnp_bytes_{cname}), "
                          f"{holes_ref}, {num_holes} }}, "
                          f"/* {s['name']} */\n")

        out.write("};\n\n")
        out.write("#endif /* VDBE_CNP_STENCILS_H */\n")

    print(f"Generated {output_path}")


def main():
    parser = argparse.ArgumentParser(
        description="Extract CnP stencils from compiled object file"
    )
    parser.add_argument("--input", required=True,
                        help="Input .o file (vdbe_cnp_stubs.o)")
    parser.add_argument("--output", required=True,
                        help="Output C header (vdbe_cnp_stencils.h)")
    parser.add_argument("--opcodes-header", required=True,
                        help="Path to opcodes.h for opcode numbering")
    args = parser.parse_args()

    print(f"Extracting stencils from {args.input}...")
    stencils = extract_stencils(args.input, args.opcodes_header)

    if not stencils:
        print("ERROR: No stencils found!", file=sys.stderr)
        sys.exit(1)

    # Find max opcode number
    max_opcode = max(s["opcode_num"] for s in stencils)

    print(f"\nExtracted {len(stencils)} stencils "
          f"(max opcode = {max_opcode})")
    emit_header(stencils, args.output, max_opcode)


if __name__ == "__main__":
    main()
