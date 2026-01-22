#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Script to compare memcg atomic counter vs rstat performance
#
# This script helps automate the comparison between different
# memory cgroup implementations.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RESULTS_DIR="${SCRIPT_DIR}/perf_results"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

check_root() {
    if [ "$EUID" -ne 0 ]; then
        log_error "This script must be run as root"
        exit 1
    fi
}

check_binary() {
    if [ ! -x "${SCRIPT_DIR}/test_memcg_perf" ]; then
        log_error "test_memcg_perf not found or not executable"
        log_info "Run 'make test_memcg_perf' first"
        exit 1
    fi
}

detect_implementation() {
    local impl="unknown"
    local config_file=""
    
    # Find kernel config file (try multiple locations and formats)
    if [ -f /proc/config.gz ]; then
        config_file="/proc/config.gz"
    elif [ -f "/boot/config-$(uname -r)" ]; then
        config_file="/boot/config-$(uname -r)"
    else
        # Try to find the latest config file in /boot
        config_file=$(ls -t /boot/config* 2>/dev/null | head -n1)
    fi
    
    if [ -z "$config_file" ]; then
        log_warn "Kernel config file not found, cannot detect implementation"
        echo "$impl"
        return
    fi
    
    # Check config based on file format
    if [[ "$config_file" == *.gz ]]; then
        # Compressed config
        if zgrep -q "^CONFIG_MEMCG_ATOMIC_COUNTER=y" "$config_file" 2>/dev/null; then
            impl="atomic"
        elif zgrep -q "^CONFIG_MEMCG_RSTAT_COUNTER=y" "$config_file" 2>/dev/null; then
            impl="rstat"
        fi
    else
        # Uncompressed config
        if grep -q "^CONFIG_MEMCG_ATOMIC_COUNTER=y" "$config_file" 2>/dev/null; then
            impl="atomic"
        elif grep -q "^CONFIG_MEMCG_RSTAT_COUNTER=y" "$config_file" 2>/dev/null; then
            impl="rstat"
        fi
    fi
    
    echo "$impl"
}

setup_performance_mode() {
    log_info "Setting up performance mode..."
    
    # Disable CPU frequency scaling if available
    if command -v cpupower &> /dev/null; then
        cpupower frequency-set -g performance &> /dev/null || log_warn "Failed to set CPU governor"
    fi
    
    # Disable turbo boost on Intel
    if [ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
        echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo || log_warn "Failed to disable turbo boost"
    fi
    
    # Drop caches
    sync
    echo 3 > /proc/sys/vm/drop_caches
    
    log_info "Performance mode configured"
}

restore_system_settings() {
    log_info "Restoring system settings..."
    
    # Re-enable CPU frequency scaling
    if command -v cpupower &> /dev/null; then
        cpupower frequency-set -g schedutil &> /dev/null || true
    fi
    
    # Re-enable turbo boost
    if [ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
        echo 0 > /sys/devices/system/cpu/intel_pstate/no_turbo || true
    fi
}

run_single_test() {
    local run_number=$1
    local output_file=$2
    
    log_info "Running test iteration $run_number..."
    
    # Drop caches before each run
    sync
    echo 3 > /proc/sys/vm/drop_caches
    
    # Short sleep to let system stabilize
    sleep 1
    
    # Run the test
    "${SCRIPT_DIR}/test_memcg_perf" > "$output_file" 2>&1
    
    return $?
}

run_multiple_tests() {
    local impl=$1
    local num_runs=$2
    local output_dir="${RESULTS_DIR}/${impl}_${TIMESTAMP}"
    
    mkdir -p "$output_dir"
    
    log_info "Running $num_runs test iterations for $impl implementation..."
    
    for i in $(seq 1 $num_runs); do
        local output_file="${output_dir}/run_${i}.txt"
        local csv_file="${output_dir}/run_${i}.csv"
        
        if run_single_test "$i" "$output_file"; then
            # Move CSV file
            if [ -f "${SCRIPT_DIR}/memcg_perf_results.csv" ]; then
                mv "${SCRIPT_DIR}/memcg_perf_results.csv" "$csv_file"
            fi
            log_info "Run $i completed successfully"
        else
            log_error "Run $i failed"
        fi
    done
    
    log_info "All runs completed. Results saved to $output_dir"
}

generate_summary() {
    local impl=$1
    local output_dir="${RESULTS_DIR}/${impl}_${TIMESTAMP}"
    
    log_info "Generating summary for $impl..."
    
    # Extract average times from all runs
    python3 <<EOF
import csv
import glob
import statistics
from collections import defaultdict

csv_files = glob.glob("${output_dir}/run_*.csv")
if not csv_files:
    print("No CSV files found")
    exit(1)

# Collect data
data = defaultdict(list)
for csv_file in csv_files:
    with open(csv_file, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            func_name = row['Function']
            avg_us = float(row['Avg_us'])
            data[func_name].append(avg_us)

# Calculate statistics
print("\n" + "="*60)
print(f"Summary for ${impl} implementation (${TIMESTAMP})")
print("="*60)
print(f"Number of runs: {len(csv_files)}")
print()
print(f"{'Function':<35} {'Mean':>10} {'StdDev':>10} {'Min':>10} {'Max':>10}")
print("-"*60)

for func_name in sorted(data.keys()):
    values = data[func_name]
    mean = statistics.mean(values)
    stddev = statistics.stdev(values) if len(values) > 1 else 0
    min_val = min(values)
    max_val = max(values)
    print(f"{func_name:<35} {mean:>10.2f} {stddev:>10.2f} {min_val:>10.2f} {max_val:>10.2f}")

print("="*60)
print("All times in microseconds (us)")
print()
EOF
}

main() {
    check_root
    check_binary
    
    # Parse arguments
    NUM_RUNS=${1:-5}
    
    log_info "Memory Cgroup Performance Comparison Tool"
    log_info "=========================================="
    
    # Detect current implementation
    IMPL=$(detect_implementation)
    log_info "Detected implementation: $IMPL"
    
    if [ "$IMPL" = "unknown" ]; then
        log_warn "Could not detect memory cgroup implementation"
        log_warn "Proceeding anyway, results will be labeled as 'unknown'"
    fi
    
    # Setup
    mkdir -p "$RESULTS_DIR"
    setup_performance_mode
    
    # Run tests
    run_multiple_tests "$IMPL" "$NUM_RUNS"
    
    # Generate summary
    if command -v python3 &> /dev/null; then
        generate_summary "$IMPL"
    else
        log_warn "Python3 not found, skipping summary generation"
    fi
    
    # Cleanup
    restore_system_settings
    
    log_info "Testing complete!"
    log_info "Results saved to ${RESULTS_DIR}/${IMPL}_${TIMESTAMP}/"
    log_info ""
    log_info "To compare with another implementation:"
    log_info "  1. Reboot with different kernel/boot parameters"
    log_info "  2. Run this script again"
    log_info "  3. Compare the summary reports"
}

# Handle Ctrl+C
trap 'log_warn "Interrupted!"; restore_system_settings; exit 130' INT TERM

main "$@"
