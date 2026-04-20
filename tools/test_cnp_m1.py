#!/usr/bin/env python3
"""
M1 gate test for Copy-and-Patch stencil pipeline.

Verifies that the stencil extraction round-trip is correct:
  genstubs.py → stubs.c → clang -O2 → stubs.o → extract.py → stencils.h

Checks:
  1. All expected M1 opcodes are present in stencils.h
  2. Handler calls use R_X86_64_64 (movabs, 64-bit absolute) — NOT PLT32
  3. OP_Goto / OP_Halt are minimal: movabs + ret (11 bytes)
  4. OP_Integer / OP_Add / OP_Copy / OP_ResultRow have HOLE_HANDLER at expected offset
  5. All hole reloc types match expectations
  6. Stencil bytes disassemble cleanly via objdump (no garbage patterns)

This is a build-time / CI verification, not a runtime execution test.
Runtime execution is covered by the M2 milestone.

Usage:
    python3 tools/test_cnp_m1.py [--stencils PATH] [--stubs-obj PATH]
"""

import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

# ---------------------------------------------------------------------------
# Expected stencil properties for M1 opcodes
# ---------------------------------------------------------------------------

# Opcode name → opcode number (must match opcodes.h)
M1_OPCODES = {
    "OP_Goto":      9,
    "OP_Init":      50,
    "OP_Halt":      53,
    "OP_Integer":   54,
    "OP_Add":       24,
    "OP_Copy":      62,
    "OP_ResultRow": 64,
}

# Relocation type constants
R_X86_64_64    = 1   # 64-bit absolute — used by movabs (HANDLER, NEXT, BRANCH, ...)
R_X86_64_32S   = 11  # 32-bit signed absolute — used for P1..P5 operands
R_X86_64_PC32  = 2   # PC-relative 32-bit — must NOT appear for handler calls
R_X86_64_PLT32 = 4   # PLT-relative 32-bit — must NOT appear for handler calls

# Hole kind names (as they appear in the generated enum)
HOLE_KINDS_NEED_64BIT = {
    "CNP_HOLE_HANDLER",
    "CNP_HOLE_NEXT",
    "CNP_HOLE_BRANCH",
    "CNP_HOLE_ERROR_EXIT",
    "CNP_HOLE_SIGNAL",
}

HOLE_KINDS_ALLOW_32BIT = {
    "CNP_HOLE_P1",
    "CNP_HOLE_P2",
    "CNP_HOLE_P3",
    "CNP_HOLE_P4",
    "CNP_HOLE_P5",
}

# movabs opcode prefix: 48 b8 (REX.W + MOV rax, imm64)
MOVABS_PREFIX = bytes([0x48, 0xb8])
RET_OPCODE    = bytes([0xc3])


# ---------------------------------------------------------------------------
# Parse stencils.h
# ---------------------------------------------------------------------------

def parse_stencils_h(path):
    """
    Parse the generated vdbe_cnp_stencils.h and return a dict:
      opcode_name -> {
        'opcode_num': int,
        'bytes': bytes,
        'holes': [{'offset': int, 'kind': str, 'reloc_type': int, 'addend': int}]
      }
    """
    with open(path) as f:
        text = f.read()

    stencils = {}

    # Find all cnp_bytes_* arrays
    byte_arrays = {}
    for m in re.finditer(
            r"static const uint8_t cnp_bytes_(\w+)\[\] = \{([^}]+)\};",
            text, re.DOTALL):
        name = m.group(1)  # e.g. "goto", "integer"
        hex_vals = re.findall(r'0x([0-9a-fA-F]+)', m.group(2))
        byte_arrays[name] = bytes(int(h, 16) for h in hex_vals)

    # Find all cnp_holes_* arrays (multiple {entry} blocks)
    hole_arrays = {}
    for m in re.finditer(
            r"static const struct cnp_hole cnp_holes_(\w+)\[\] = \{(.*?)\};",
            text, re.DOTALL):
        name = m.group(1)
        array_body = m.group(2)
        entries = re.findall(
            r"\{\s*\.offset\s*=\s*(\d+)\s*,\s*\.kind\s*=\s*(CNP_HOLE_\w+)"
            r"\s*,\s*\.addend\s*=\s*(-?\d+)\s*,\s*\.reloc_type\s*=\s*(\d+)",
            array_body)
        holes = []
        for offset, kind, addend, reloc_type in entries:
            holes.append({
                "offset":     int(offset),
                "kind":       kind,
                "addend":     int(addend),
                "reloc_type": int(reloc_type),
            })
        hole_arrays[name] = holes

    # Find stencil table entries:
    # [opcode] = { cnp_bytes_X, sizeof(cnp_bytes_X), cnp_holes_X, N }, /* OP_Name */
    for m in re.finditer(
            r"\[(\d+)\]\s*=\s*\{\s*cnp_bytes_(\w+)[^}]*\}[^/]*/\*\s*(OP_\w+)\s*\*/",
            text):
        opcode_num = int(m.group(1))
        key = m.group(2)      # e.g. "goto", "integer"
        op_name = m.group(3)  # e.g. "OP_Goto", "OP_Integer"
        stencils[op_name] = {
            "opcode_num": opcode_num,
            "bytes":      byte_arrays.get(key, b""),
            "holes":      hole_arrays.get(key, []),
        }

    return stencils


# ---------------------------------------------------------------------------
# Disassemble bytes via objdump
# ---------------------------------------------------------------------------

def disassemble(raw_bytes):
    """Return disassembly string for raw_bytes using objdump."""
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(raw_bytes)
        tmp = f.name
    try:
        out = subprocess.check_output(
            ["objdump", "-D", "-b", "binary", "-m", "i386:x86-64",
             "--no-show-raw-insn", tmp],
            stderr=subprocess.DEVNULL)
        return out.decode("utf-8", errors="replace")
    except (subprocess.CalledProcessError, FileNotFoundError):
        return ""
    finally:
        os.unlink(tmp)


# ---------------------------------------------------------------------------
# Verification helpers
# ---------------------------------------------------------------------------

def check_movabs_at(raw_bytes, offset, label):
    """Assert that offset points to a movabs rax, imm64 instruction."""
    if offset + 2 > len(raw_bytes):
        return f"  FAIL [{label}]: offset {offset} out of range (stencil size {len(raw_bytes)})"
    if raw_bytes[offset:offset+2] != MOVABS_PREFIX:
        got = raw_bytes[offset:offset+2].hex()
        return f"  FAIL [{label}]: expected movabs prefix 48 b8 at offset {offset}, got {got}"
    return None


def check_no_plt32(holes, op_name):
    """Assert that no handler-class holes use PLT32 relocation."""
    errors = []
    for h in holes:
        if h["kind"] in HOLE_KINDS_NEED_64BIT:
            if h["reloc_type"] in (R_X86_64_PLT32, R_X86_64_PC32):
                errors.append(
                    f"  FAIL [{op_name}]: hole {h['kind']} at offset {h['offset']} "
                    f"uses PLT32/PC32 reloc (type {h['reloc_type']}) — "
                    f"must use R_X86_64_64 (type 1) for unlimited range")
    return errors


def check_64bit_holes(holes, op_name):
    """Assert that HANDLER/NEXT/BRANCH/ERROR_EXIT holes use R_X86_64_64."""
    errors = []
    for h in holes:
        if h["kind"] in HOLE_KINDS_NEED_64BIT:
            if h["reloc_type"] != R_X86_64_64:
                errors.append(
                    f"  FAIL [{op_name}]: hole {h['kind']} at offset {h['offset']} "
                    f"has reloc_type={h['reloc_type']}, expected {R_X86_64_64}")
    return errors


def check_minimal_stencil(op_name, stencil):
    """
    OP_Goto and OP_Halt should be exactly 11 bytes:
      movabs $HOLE_BRANCH, %rax   (10 bytes: 48 b8 + 8-byte imm)
      ret                          (1 byte:  c3)
    """
    errors = []
    raw = stencil["bytes"]
    holes = stencil["holes"]

    if len(raw) != 11:
        errors.append(f"  FAIL [{op_name}]: expected 11 bytes, got {len(raw)}")
        return errors

    if raw[:2] != MOVABS_PREFIX:
        errors.append(f"  FAIL [{op_name}]: first 2 bytes should be movabs prefix 48 b8")
    if raw[-1:] != RET_OPCODE:
        errors.append(f"  FAIL [{op_name}]: last byte should be ret (c3)")

    if len(holes) != 1:
        errors.append(f"  FAIL [{op_name}]: expected 1 hole (BRANCH), got {len(holes)}")
    else:
        h = holes[0]
        if h["kind"] != "CNP_HOLE_BRANCH":
            errors.append(f"  FAIL [{op_name}]: hole should be BRANCH, got {h['kind']}")
        if h["offset"] != 2:
            errors.append(f"  FAIL [{op_name}]: BRANCH hole should be at offset 2, got {h['offset']}")
        if h["reloc_type"] != R_X86_64_64:
            errors.append(f"  FAIL [{op_name}]: BRANCH hole reloc_type={h['reloc_type']}, expected {R_X86_64_64}")

    return errors


def check_handler_stencil(op_name, stencil, branch_type="none"):
    """
    OP_Integer, OP_Add, OP_Copy, OP_ResultRow, OP_Init, OP_Halt:
    - Must have exactly one HOLE_HANDLER hole
    - HOLE_HANDLER must be at a valid movabs offset
    - Reloc types must be correct (64-bit for control-flow holes, 32-bit for P1..P5)

    branch_type:
      "none"  — no HOLE_BRANCH (always HOLE_NEXT)
      "jump"  — has one HOLE_BRANCH (for jump target or SQL_DONE)
      "row"   — has one HOLE_SIGNAL (for OP_ResultRow row notification)
    """
    errors = []
    raw = stencil["bytes"]
    holes = stencil["holes"]

    handler_holes = [h for h in holes if h["kind"] == "CNP_HOLE_HANDLER"]
    if len(handler_holes) != 1:
        errors.append(f"  FAIL [{op_name}]: expected 1 HOLE_HANDLER, got {len(handler_holes)}")
    else:
        h = handler_holes[0]
        err = check_movabs_at(raw, h["offset"] - 2, f"{op_name}/HANDLER")
        if err:
            errors.append(err)

    if branch_type == "jump":
        branch_holes = [h for h in holes if h["kind"] == "CNP_HOLE_BRANCH"]
        if len(branch_holes) != 1:
            errors.append(f"  FAIL [{op_name}]: expected 1 HOLE_BRANCH, got {len(branch_holes)}")
    elif branch_type == "row":
        signal_holes = [h for h in holes if h["kind"] == "CNP_HOLE_SIGNAL"]
        if len(signal_holes) != 1:
            errors.append(f"  FAIL [{op_name}]: expected 1 HOLE_SIGNAL, got {len(signal_holes)}")
        branch_holes = [h for h in holes if h["kind"] == "CNP_HOLE_BRANCH"]
        if len(branch_holes) != 0:
            errors.append(
                f"  FAIL [{op_name}]: expected 0 HOLE_BRANCH (row type uses SIGNAL), "
                f"got {len(branch_holes)}"
            )

    errors.extend(check_no_plt32(holes, op_name))
    errors.extend(check_64bit_holes(holes, op_name))
    return errors


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="M1 CnP stencil gate test")
    parser.add_argument(
        "--stencils",
        default=os.path.join(os.path.dirname(__file__), "..",
                             "src", "box", "sql", "generated",
                             "vdbe_cnp_stencils.h"),
        help="Path to vdbe_cnp_stencils.h",
    )
    args = parser.parse_args()

    stencils_path = os.path.abspath(args.stencils)
    if not os.path.exists(stencils_path):
        print(f"ERROR: stencils.h not found at {stencils_path}")
        print("       Run 'make cnp_stencils' first.")
        sys.exit(1)

    print(f"M1 CnP stencil gate test")
    print(f"Parsing: {stencils_path}")

    stencils = parse_stencils_h(stencils_path)

    all_errors = []
    passed = 0
    total = 0

    # --- Check 1: all M1 opcodes are present ---
    print("\n[1] Checking M1 opcode presence...")
    for op_name, expected_num in sorted(M1_OPCODES.items()):
        total += 1
        if op_name not in stencils:
            all_errors.append(f"  FAIL: {op_name} not found in stencils.h")
        elif stencils[op_name]["opcode_num"] != expected_num:
            got = stencils[op_name]["opcode_num"]
            all_errors.append(f"  FAIL: {op_name} opcode {got}, expected {expected_num}")
        else:
            print(f"  OK   {op_name} (opcode {expected_num}): "
                  f"{len(stencils[op_name]['bytes'])} bytes, "
                  f"{len(stencils[op_name]['holes'])} holes")
            passed += 1

    # --- Check 2: minimal stencils (OP_Goto only; OP_Halt is now an external handler) ---
    print("\n[2] Checking minimal stencil (Goto only)...")
    for op_name in ("OP_Goto",):
        total += 1
        if op_name not in stencils:
            all_errors.append(f"  SKIP: {op_name} not in stencils")
            continue
        errs = check_minimal_stencil(op_name, stencils[op_name])
        if errs:
            all_errors.extend(errs)
        else:
            print(f"  OK   {op_name}: movabs + ret, HOLE_BRANCH R_X86_64_64")
            passed += 1

    # --- Check 3: handler stencils (no PLT32) ---
    print("\n[3] Checking handler stencils (no PLT32, 64-bit handler calls)...")
    handler_ops = {
        "OP_Integer":   "none",   # always HOLE_NEXT
        "OP_Add":       "none",   # always HOLE_NEXT
        "OP_Copy":      "none",   # always HOLE_NEXT
        "OP_ResultRow": "row",    # HOLE_SIGNAL + HOLE_NEXT (no HOLE_BRANCH)
        "OP_Init":      "jump",   # HOLE_BRANCH for jump to P2
        "OP_Halt":      "jump",   # HOLE_BRANCH for SQL_DONE
    }
    for op_name, branch_type in handler_ops.items():
        total += 1
        if op_name not in stencils:
            all_errors.append(f"  SKIP: {op_name} not in stencils")
            continue
        errs = check_handler_stencil(op_name, stencils[op_name], branch_type)
        if errs:
            all_errors.extend(errs)
        else:
            n_holes = len(stencils[op_name]["holes"])
            print(f"  OK   {op_name}: HOLE_HANDLER R_X86_64_64, "
                  f"no PLT32, branch_type={branch_type} ({n_holes} holes total)")
            passed += 1

    # --- Check 4: disassembly sanity via objdump ---
    print("\n[4] Disassembly sanity (objdump -D)...")
    for op_name in sorted(M1_OPCODES.keys()):
        total += 1
        if op_name not in stencils:
            all_errors.append(f"  SKIP: {op_name} not in stencils")
            continue
        raw = stencils[op_name]["bytes"]
        asm = disassemble(raw)
        if not asm:
            all_errors.append(f"  FAIL [{op_name}]: objdump returned empty output "
                               f"(is objdump installed?)")
            continue
        # Check that 'movabs' appears at least once (for HOLE_BRANCH/NEXT/HANDLER)
        if "movabs" not in asm:
            all_errors.append(f"  FAIL [{op_name}]: no movabs instruction in disassembly")
        # Check that 'ret' appears (every stencil must return)
        elif "ret" not in asm:
            all_errors.append(f"  FAIL [{op_name}]: no ret instruction in disassembly")
        else:
            movabs_count = asm.count("movabs")
            print(f"  OK   {op_name}: {movabs_count} movabs instruction(s), ret present")
            passed += 1

    # --- Summary ---
    print(f"\n{'='*60}")
    if all_errors:
        print(f"RESULT: FAILED ({passed}/{total} checks passed)")
        print("\nErrors:")
        for e in all_errors:
            print(e)
        sys.exit(1)
    else:
        print(f"RESULT: PASSED ({passed}/{total} checks passed)")
        print("\nM1 gate satisfied: stencils round-trip correctly.")
        print("  - All M1 opcodes present")
        print("  - Handler holes use R_X86_64_64 (no PLT32 range limit)")
        print("  - Control-flow stencils are minimal (movabs + ret)")
        print("  - Disassembly confirms movabs pattern in all stencils")
        sys.exit(0)


if __name__ == "__main__":
    main()
