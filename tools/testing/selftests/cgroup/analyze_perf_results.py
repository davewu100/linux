#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
Analyze and visualize memcg performance test results

This script processes CSV output from test_memcg_perf and generates
comparison reports and plots.
"""

import csv
import sys
import argparse
from pathlib import Path
from collections import defaultdict
import statistics


def load_csv(filepath):
    """Load performance data from CSV file"""
    data = {}
    with open(filepath, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            func_name = row['Function']
            data[func_name] = {
                'calls': int(row['Calls']),
                'total_us': float(row['Total_us']),
                'avg_us': float(row['Avg_us']),
                'min_us': float(row['Min_us']),
                'max_us': float(row['Max_us']),
            }
    return data


def compare_two_runs(file1, file2, label1="Run 1", label2="Run 2"):
    """Compare two performance runs"""
    data1 = load_csv(file1)
    data2 = load_csv(file2)
    
    print(f"\n{'='*80}")
    print(f"Performance Comparison: {label1} vs {label2}")
    print(f"{'='*80}\n")
    
    # Find common functions
    common_funcs = set(data1.keys()) & set(data2.keys())
    
    if not common_funcs:
        print("ERROR: No common functions found between the two runs")
        return
    
    # Print header
    print(f"{'Function':<30} {label1:>12} {label2:>12} {'Diff':>12} {'% Change':>10}")
    print(f"{'-'*80}")
    
    improvements = []
    regressions = []
    
    for func in sorted(common_funcs):
        avg1 = data1[func]['avg_us']
        avg2 = data2[func]['avg_us']
        diff = avg2 - avg1
        pct_change = (diff / avg1) * 100 if avg1 > 0 else 0
        
        # Color coding (for terminals that support it)
        if diff < 0:
            improvements.append((func, abs(pct_change)))
            diff_str = f"\033[92m{diff:+10.2f}\033[0m"  # Green
            pct_str = f"\033[92m{pct_change:+9.1f}%\033[0m"
        elif diff > 0:
            regressions.append((func, pct_change))
            diff_str = f"\033[91m{diff:+10.2f}\033[0m"  # Red
            pct_str = f"\033[91m{pct_change:+9.1f}%\033[0m"
        else:
            diff_str = f"{diff:+10.2f}"
            pct_str = f"{pct_change:+9.1f}%"
        
        print(f"{func:<30} {avg1:>10.2f}us {avg2:>10.2f}us {diff_str} {pct_str}")
    
    # Summary
    print(f"\n{'='*80}")
    print("Summary:")
    print(f"  Improvements: {len(improvements)}")
    print(f"  Regressions:  {len(regressions)}")
    
    if improvements:
        best_improvement = max(improvements, key=lambda x: x[1])
        print(f"  Best improvement: {best_improvement[0]} (-{best_improvement[1]:.1f}%)")
    
    if regressions:
        worst_regression = max(regressions, key=lambda x: x[1])
        print(f"  Worst regression: {worst_regression[0]} (+{worst_regression[1]:.1f}%)")
    
    print(f"{'='*80}\n")


def analyze_multiple_runs(directory, pattern="run_*.csv"):
    """Analyze multiple test runs in a directory"""
    csv_files = sorted(Path(directory).glob(pattern))
    
    if not csv_files:
        print(f"ERROR: No CSV files matching '{pattern}' found in {directory}")
        return
    
    print(f"\n{'='*80}")
    print(f"Multi-Run Analysis: {directory}")
    print(f"Number of runs: {len(csv_files)}")
    print(f"{'='*80}\n")
    
    # Collect data from all runs
    all_data = defaultdict(list)
    for csv_file in csv_files:
        data = load_csv(csv_file)
        for func_name, metrics in data.items():
            all_data[func_name].append(metrics['avg_us'])
    
    # Calculate statistics
    print(f"{'Function':<30} {'Mean':>10} {'StdDev':>10} {'Min':>10} {'Max':>10} {'CV%':>8}")
    print(f"{'-'*80}")
    
    for func_name in sorted(all_data.keys()):
        values = all_data[func_name]
        mean = statistics.mean(values)
        stddev = statistics.stdev(values) if len(values) > 1 else 0
        min_val = min(values)
        max_val = max(values)
        cv = (stddev / mean * 100) if mean > 0 else 0  # Coefficient of variation
        
        print(f"{func_name:<30} {mean:>10.2f} {stddev:>10.2f} {min_val:>10.2f} "
              f"{max_val:>10.2f} {cv:>7.1f}%")
    
    print(f"\n{'='*80}")
    print("Notes:")
    print("  - All times in microseconds (us)")
    print("  - CV% = Coefficient of Variation (StdDev/Mean * 100)")
    print("  - Lower CV% indicates more consistent performance")
    print(f"{'='*80}\n")


def generate_report(csv_file):
    """Generate a simple report from a single CSV file"""
    data = load_csv(csv_file)
    
    print(f"\n{'='*80}")
    print(f"Performance Report: {csv_file}")
    print(f"{'='*80}\n")
    
    print(f"{'Function':<30} {'Calls':>8} {'Avg (us)':>12} {'Min (us)':>12} {'Max (us)':>12}")
    print(f"{'-'*80}")
    
    for func_name in sorted(data.keys()):
        metrics = data[func_name]
        print(f"{func_name:<30} {metrics['calls']:>8} {metrics['avg_us']:>12.2f} "
              f"{metrics['min_us']:>12.2f} {metrics['max_us']:>12.2f}")
    
    print(f"{'='*80}\n")


def main():
    parser = argparse.ArgumentParser(
        description='Analyze memcg performance test results',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Generate report from single run
  %(prog)s report memcg_perf_results.csv
  
  # Compare two runs (e.g., atomic vs rstat)
  %(prog)s compare results_atomic.csv results_rstat.csv
  
  # Analyze multiple runs in a directory
  %(prog)s multi-run perf_results/atomic_20240121_120000/
        """
    )
    
    subparsers = parser.add_subparsers(dest='command', help='Command to execute')
    
    # Report command
    report_parser = subparsers.add_parser('report', help='Generate report from single CSV')
    report_parser.add_argument('csv_file', help='CSV file to analyze')
    
    # Compare command
    compare_parser = subparsers.add_parser('compare', help='Compare two runs')
    compare_parser.add_argument('file1', help='First CSV file')
    compare_parser.add_argument('file2', help='Second CSV file')
    compare_parser.add_argument('--label1', default='Run 1', help='Label for first run')
    compare_parser.add_argument('--label2', default='Run 2', help='Label for second run')
    
    # Multi-run command
    multi_parser = subparsers.add_parser('multi-run', help='Analyze multiple runs')
    multi_parser.add_argument('directory', help='Directory containing CSV files')
    multi_parser.add_argument('--pattern', default='run_*.csv', help='File pattern to match')
    
    args = parser.parse_args()
    
    if not args.command:
        parser.print_help()
        return 1
    
    try:
        if args.command == 'report':
            generate_report(args.csv_file)
        elif args.command == 'compare':
            compare_two_runs(args.file1, args.file2, args.label1, args.label2)
        elif args.command == 'multi-run':
            analyze_multiple_runs(args.directory, args.pattern)
    except FileNotFoundError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    except Exception as e:
        print(f"ERROR: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        return 1
    
    return 0


if __name__ == '__main__':
    sys.exit(main())
