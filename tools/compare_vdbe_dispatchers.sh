#!/bin/bash
#
# VDBE Dispatcher Comparison Script
# Created: 2026-02-04
# Phase 5.9 / Pre-Phase 5.10
#
# PURPOSE:
# --------
# This script automates the comparison between original and generated
# VDBE dispatchers using the micro-benchmark. It:
#
# 1. Runs benchmark with generated dispatcher
# 2. Runs benchmark with original dispatcher
# 3. Compares results and generates report
#
# REQUIREMENTS:
# -------------
# - Single Tarantool binary with runtime dispatcher selection
# - vdbe_micro_benchmark.lua in tools/ directory
# - Python 3 with json module (for analysis script)
#
# USAGE:
# ------
#   ./tools/compare_vdbe_dispatchers.sh [tarantool_binary]
#
# If tarantool_binary is not specified, uses ./build/src/tarantool
#
# OUTPUT:
# -------
# - generated_results.json - Generated dispatcher results
# - original_results.json  - Original dispatcher results
# - comparison_report.txt  - Human-readable comparison
#

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BENCHMARK_SCRIPT="$SCRIPT_DIR/vdbe_micro_benchmark.lua"
RESULTS_DIR="${RESULTS_DIR:-/tmp/vdbe_benchmark_$$}"

# Get binary path
if [ $# -ge 1 ]; then
    TARANTOOL_BIN="$1"
else
    TARANTOOL_BIN="$PROJECT_ROOT/build/src/tarantool"
fi

# Validate
if [ ! -f "$TARANTOOL_BIN" ]; then
    echo -e "${RED}ERROR: Tarantool binary not found: $TARANTOOL_BIN${NC}"
    echo "Usage: $0 [path/to/tarantool]"
    exit 1
fi

if [ ! -f "$BENCHMARK_SCRIPT" ]; then
    echo -e "${RED}ERROR: Benchmark script not found: $BENCHMARK_SCRIPT${NC}"
    exit 1
fi

# Create results directory
mkdir -p "$RESULTS_DIR"
cd "$RESULTS_DIR"

echo -e "${BLUE}========================================================================${NC}"
echo -e "${BLUE}VDBE DISPATCHER COMPARISON${NC}"
echo -e "${BLUE}========================================================================${NC}"
echo "Tarantool binary: $TARANTOOL_BIN"
echo "Results directory: $RESULTS_DIR"
echo "Timestamp: $(date '+%Y-%m-%d %H:%M:%S')"
echo ""

# Function to extract JSON from output
extract_json() {
    local input_file="$1"
    local output_file="$2"
    
    # Extract everything after "JSON OUTPUT" line until end
    sed -n '/JSON OUTPUT/,$ p' "$input_file" | \
        sed '1,/^====/ d' | \
        grep -v '^====' > "$output_file"
}

# Function to run benchmark
run_benchmark() {
    local mode="$1"
    local output_file="$2"
    
    echo -e "${GREEN}Running benchmark with ${mode^^} dispatcher...${NC}"
    
    VDBE_DISPATCHER="$mode" "$TARANTOOL_BIN" \
        -e "dofile('$BENCHMARK_SCRIPT')" \
        > "$output_file" 2>&1
    
    if [ $? -ne 0 ]; then
        echo -e "${RED}ERROR: Benchmark failed for $mode dispatcher${NC}"
        cat "$output_file"
        return 1
    fi
    
    echo -e "${GREEN}✓ Completed $mode dispatcher benchmark${NC}"
}

# Run benchmarks
echo -e "\n${YELLOW}Step 1: Running generated dispatcher benchmark${NC}"
run_benchmark "generated" "generated_output.txt"

echo -e "\n${YELLOW}Step 2: Running original dispatcher benchmark${NC}"
run_benchmark "old" "original_output.txt"

# Extract JSON results
echo -e "\n${YELLOW}Step 3: Extracting results${NC}"
extract_json "generated_output.txt" "generated_results.json"
extract_json "original_output.txt" "original_results.json"

# Verify JSON files
if [ ! -s "generated_results.json" ] || [ ! -s "original_results.json" ]; then
    echo -e "${RED}ERROR: Failed to extract JSON results${NC}"
    exit 1
fi

echo -e "${GREEN}✓ Results extracted${NC}"

# Generate comparison report
echo -e "\n${YELLOW}Step 4: Generating comparison report${NC}"

cat > comparison_report.txt << 'EOF'
================================================================================
VDBE DISPATCHER PERFORMANCE COMPARISON REPORT
================================================================================

EOF

echo "Timestamp: $(date '+%Y-%m-%d %H:%M:%S')" >> comparison_report.txt
echo "Tarantool: $TARANTOOL_BIN" >> comparison_report.txt
echo "" >> comparison_report.txt

# Use Python to analyze JSON and create report
python3 << 'PYTHON_SCRIPT' >> comparison_report.txt
import json
import sys

def load_results(filename):
    with open(filename, 'r') as f:
        return json.load(f)

def format_time(seconds):
    if seconds < 0.001:
        return f"{seconds*1000000:.1f} μs"
    elif seconds < 1.0:
        return f"{seconds*1000:.1f} ms"
    else:
        return f"{seconds:.3f} s"

def compare_results(generated, original):
    print("TEST COMPARISON")
    print("=" * 80)
    print(f"{'Test Name':<30} {'Generated':<15} {'Original':<15} {'Diff':>10}")
    print("-" * 80)
    
    total_gen = 0
    total_orig = 0
    
    for g_test in generated['tests']:
        # Find matching test in original
        o_test = None
        for t in original['tests']:
            if t['name'] == g_test['name']:
                o_test = t
                break
        
        if not o_test:
            continue
        
        g_time = g_test['stats']['mean']
        o_time = o_test['stats']['mean']
        diff_pct = ((g_time - o_time) / o_time) * 100
        
        total_gen += g_time
        total_orig += o_time
        
        indicator = "⚡" if diff_pct < -5 else "🐌" if diff_pct > 5 else "≈"
        
        print(f"{g_test['name']:<30} {format_time(g_time):<15} {format_time(o_time):<15} {diff_pct:>9.1f}% {indicator}")
    
    print("-" * 80)
    overall_diff = ((total_gen - total_orig) / total_orig) * 100
    print(f"{'TOTAL':<30} {format_time(total_gen):<15} {format_time(total_orig):<15} {overall_diff:>9.1f}%")
    print()
    
    # Summary
    print("\nSUMMARY")
    print("=" * 80)
    if overall_diff < -2:
        print(f"✅ Generated dispatcher is {abs(overall_diff):.1f}% FASTER")
    elif overall_diff > 2:
        print(f"⚠️  Generated dispatcher is {overall_diff:.1f}% SLOWER")
    else:
        print(f"✓ Generated dispatcher performance is equivalent (within ±2%)")
    
    print(f"\nTotal time - Generated: {format_time(total_gen)}")
    print(f"Total time - Original:  {format_time(total_orig)}")
    print(f"Difference: {format_time(abs(total_gen - total_orig))} ({overall_diff:+.1f}%)")
    
    # Category breakdown
    print("\n\nBY CATEGORY")
    print("=" * 80)
    categories = {}
    
    for g_test in generated['tests']:
        cat = g_test['category']
        if cat not in categories:
            categories[cat] = {'gen': 0, 'orig': 0, 'count': 0}
        
        categories[cat]['gen'] += g_test['stats']['mean']
        categories[cat]['count'] += 1
        
        # Find original
        for o_test in original['tests']:
            if o_test['name'] == g_test['name']:
                categories[cat]['orig'] += o_test['stats']['mean']
                break
    
    print(f"{'Category':<20} {'Generated':<15} {'Original':<15} {'Diff':>10}")
    print("-" * 80)
    
    for cat in sorted(categories.keys()):
        data = categories[cat]
        diff_pct = ((data['gen'] - data['orig']) / data['orig']) * 100
        print(f"{cat:<20} {format_time(data['gen']):<15} {format_time(data['orig']):<15} {diff_pct:>9.1f}%")

try:
    generated = load_results('generated_results.json')
    original = load_results('original_results.json')
    compare_results(generated, original)
except Exception as e:
    print(f"ERROR: Failed to generate report: {e}", file=sys.stderr)
    sys.exit(1)
PYTHON_SCRIPT

if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ Comparison report generated${NC}"
else
    echo -e "${YELLOW}⚠ Could not generate detailed report (Python analysis failed)${NC}"
    echo "Raw results are available in:"
    echo "  - generated_output.txt"
    echo "  - original_output.txt"
fi

# Display report
echo -e "\n${BLUE}========================================================================${NC}"
cat comparison_report.txt
echo -e "${BLUE}========================================================================${NC}"

# Summary
echo -e "\n${GREEN}BENCHMARK COMPLETE${NC}"
echo "Results saved to: $RESULTS_DIR"
echo ""
echo "Files:"
echo "  - generated_output.txt   - Full output from generated dispatcher"
echo "  - original_output.txt    - Full output from original dispatcher"
echo "  - generated_results.json - Structured results (generated)"
echo "  - original_results.json  - Structured results (original)"
echo "  - comparison_report.txt  - Performance comparison report"
echo ""
echo -e "${BLUE}To view the full report:${NC}"
echo "  cat $RESULTS_DIR/comparison_report.txt"
