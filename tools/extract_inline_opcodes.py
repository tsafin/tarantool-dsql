#!/usr/bin/env python3
"""
extract_inline_opcodes.py

Tool to extract inline opcode implementations from vdbe.c and merge them
into opcodes.yaml for the DSL-based code generator (Phase 5.2).

Usage:
  python3 tools/extract_inline_opcodes.py \
    --vdbe src/box/sql/vdbe.c \
    --opcodes tools/vdbe_dsl/opcodes.yaml \
    --output tools/vdbe_dsl/opcodes_with_inline.yaml

This extracts all EXECUTE() blocks from vdbe.c and adds the code as
inline_code field to corresponding opcodes in the YAML file.
"""
import re
import sys
import argparse
import textwrap
from pathlib import Path

try:
    import yaml
    _HAS_YAML = True
except ImportError:
    _HAS_YAML = False


def extract_opcodes_from_vdbe(vdbe_path):
    """
    Extract all EXECUTE(opcode) blocks from vdbe.c.
    Returns a dict mapping opcode names to code strings.

    Handles:
    - Normal EXECUTE(OP_Name, (params)): { code }
    - Fall-through cases: EXECUTE(OP_A, (params)): EXECUTE(OP_B, (params)): { code }
    """
    with open(vdbe_path, 'r') as f:
        content = f.read()

    opcodes = {}

    # First, extract all EXECUTE blocks with their code
    # Pattern: EXECUTE(OP_Name,(params)): { ... }
    pattern = r'EXECUTE\(([A-Za-z0-9_]+),\([^)]*\)\):\s*(\{[^}]*(?:\{[^}]*\}[^}]*)*\})?'

    # Find all EXECUTE statements and group fall-throughs
    execute_pattern = r'EXECUTE\(([A-Za-z0-9_]+),\([^)]*\)\):\s*'
    lines = content.split('\n')

    # Build a map of which opcodes fall through and to which opcodes
    fallthrough_map = {}
    i = 0
    while i < len(lines):
        line = lines[i]
        if 'EXECUTE(' in line:
            # Extract opcode name
            m = re.search(r'EXECUTE\(([A-Za-z0-9_]+),\([^)]*\)\):', line)
            if m:
                op_name = m.group(1)
                # Check if the next line (after trimming whitespace) is another EXECUTE
                j = i + 1
                while j < len(lines) and not lines[j].strip():
                    j += 1

                if j < len(lines) and 'EXECUTE(' in lines[j]:
                    # This is a fall-through case
                    m_next = re.search(r'EXECUTE\(([A-Za-z0-9_]+),\([^)]*\)\):', lines[j])
                    if m_next:
                        next_op = m_next.group(1)
                        fallthrough_map[op_name] = next_op
                        print(f"  Fall-through detected: {op_name} -> {next_op}")
        i += 1

    # Now extract the code blocks
    pattern = r'EXECUTE\(([A-Za-z0-9_]+),\([^)]*\)\):\s*\{([^}]*(?:\{[^}]*\}[^}]*)*)\}'

    for match in re.finditer(pattern, content, re.DOTALL):
        opcode_name = match.group(1)
        code_block = match.group(2)

        # Clean up the code block - remove leading/trailing whitespace
        code_block = code_block.strip()

        # Normalize indentation: find minimum indent and remove it
        lines = code_block.split('\n')
        if lines:
            # Find minimum indentation
            min_indent = float('inf')
            for line in lines:
                if line.strip():  # ignore blank lines
                    indent = len(line) - len(line.lstrip())
                    min_indent = min(min_indent, indent)

            if min_indent == float('inf'):
                min_indent = 0

            # Remove the minimum indentation from all lines
            normalized_lines = []
            for line in lines:
                if line.strip():  # non-empty lines
                    normalized_lines.append(line[min_indent:])
                else:
                    normalized_lines.append('')

            code_block = '\n'.join(normalized_lines).rstrip()

        opcodes[opcode_name] = code_block

    # Handle fall-throughs: assign the code from the target opcode
    for src, dst in fallthrough_map.items():
        if dst in opcodes and src not in opcodes:
            opcodes[src] = opcodes[dst]
            print(f"  Assigned {dst} code to {src}")

    return opcodes


def load_yaml(path):
    """Load YAML file."""
    if not _HAS_YAML:
        raise RuntimeError('PyYAML is required for this tool. Install it with: pip install pyyaml')

    with open(path, 'r') as f:
        return yaml.safe_load(f)


def save_yaml(data, path):
    """Save YAML file with custom formatting."""
    if not _HAS_YAML:
        raise RuntimeError('PyYAML is required for this tool')

    with open(path, 'w') as f:
        yaml.dump(data, f, default_flow_style=False, sort_keys=False,
                 allow_unicode=True, width=120)


def merge_inline_codes(opcodes_yaml_list, extracted_opcodes):
    """
    Merge extracted inline code into opcodes YAML list.
    For each inline opcode, add the inline_code field.
    """
    # Special cases that don't have EXECUTE() blocks
    special_cases = {
        'OP_Noop': 'DISPATCH();',
    }

    for opcode in opcodes_yaml_list:
        if opcode.get('handler_type') == 'inline':
            opcode_name = opcode['name']
            if opcode_name in extracted_opcodes:
                # Add the inline code
                code = extracted_opcodes[opcode_name]

                # For YAML, we need to preserve the code block format
                # Use a literal scalar (|) for multiline strings
                opcode['inline_code'] = code

                print(f"✓ Merged {opcode_name}: {len(code)} bytes")
            elif opcode_name in special_cases:
                # Special handling for certain opcodes
                code = special_cases[opcode_name]
                opcode['inline_code'] = code
                print(f"✓ Special case {opcode_name}: {len(code)} bytes")
            else:
                print(f"⚠ No inline code found for {opcode_name}")

    return opcodes_yaml_list


def main():
    parser = argparse.ArgumentParser(
        description='Extract inline opcodes from vdbe.c and merge into opcodes.yaml'
    )
    parser.add_argument('--vdbe', required=True, help='Path to vdbe.c')
    parser.add_argument('--opcodes', required=True, help='Path to opcodes.yaml')
    parser.add_argument('--output', required=True, help='Output YAML file path')

    args = parser.parse_args()

    print("Extracting inline opcodes from vdbe.c...")
    extracted = extract_opcodes_from_vdbe(args.vdbe)
    print(f"Found {len(extracted)} EXECUTE() blocks")

    print("\nLoading opcodes.yaml...")
    opcodes_list = load_yaml(args.opcodes)

    print(f"Merging inline code into {len(opcodes_list)} opcode definitions...")
    merged = merge_inline_codes(opcodes_list, extracted)

    print(f"\nSaving merged opcodes to {args.output}...")
    save_yaml(merged, args.output)

    print("Done!")

    # Print statistics
    inline_count = sum(1 for op in merged if op.get('handler_type') == 'inline')
    with_code = sum(1 for op in merged if op.get('handler_type') == 'inline' and 'inline_code' in op)

    print(f"\nStatistics:")
    print(f"  Total inline opcodes: {inline_count}")
    print(f"  With inline_code: {with_code}")
    print(f"  Missing inline_code: {inline_count - with_code}")


if __name__ == '__main__':
    main()
