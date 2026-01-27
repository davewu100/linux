#!/usr/bin/env python3
"""
kserial - Simplified enhanced kernel struct query tool

Core features only:
  1. Field aliases (@anon instead of vmstats.state[9])
  2. Human-readable output (2.73 GB instead of bytes)
  3. Multiple output formats (JSON, CSV)
  4. Batch query (--pids, --pgrep)
  5. Watch mode (--watch)
"""

import sys
import os
import struct
import re
import argparse
import json
import time
import subprocess
from typing import List, Tuple, Optional, Dict

# k-serial UAPI constants
KS_MAX_FIELDS = 16
KS_FIELD_NAME_LEN = 64
KS_MAX_OUTPUT_SIZE = 4096

# Simple built-in aliases (no config file needed)
ALIASES = {
    'mem_cgroup': {
        'anon': 'vmstats.state[9]',
        'file': 'vmstats.state[10]',
        'kernel': 'vmstats.state[40]',
        'kernel_stack': 'vmstats.state[20]',
        'pagetables': 'vmstats.state[25]',
        'shmem': 'vmstats.state[15]',
    },
    'task_struct': {
        'proc_id': 'pid',
        'proc_name': 'comm',
        'proc_state': 'state',
    },
}

class KSerialSchema:
    def __init__(self):
        self.nr_fields = 0
        self.flags = 0
        self.struct_name = ""
        self.target_pid = 0
        self.reserved = [0, 0, 0]
        self.block_offset = 0
        self.block_size = 0
        self.array_start = 0
        self.array_count = 0
        self.field_names = [""] * KS_MAX_FIELDS
    
    def pack(self) -> bytes:
        fmt = "II64sI3IIIII" + "64s" * KS_MAX_FIELDS
        struct_name_bytes = self.struct_name.encode('utf-8')[:63] + b'\x00'
        field_bytes = [f.encode('utf-8')[:63] + b'\x00' for f in self.field_names]
        return struct.pack(fmt, self.nr_fields, self.flags, struct_name_bytes,
                         self.target_pid, *self.reserved, self.block_offset,
                         self.block_size, self.array_start, self.array_count,
                         *field_bytes)

def resolve_alias(struct_name: str, field: str) -> str:
    """Resolve alias to actual field path"""
    if field.startswith('@'):
        field = field[1:]
        if struct_name in ALIASES and field in ALIASES[struct_name]:
            return ALIASES[struct_name][field]
    return field

def format_bytes(value: int) -> str:
    """Format bytes to human-readable size"""
    for unit in ['B', 'KB', 'MB', 'GB', 'TB']:
        if value < 1024:
            return f"{value:.2f} {unit}"
        value /= 1024
    return f"{value:.2f} PB"

def query_kserial(schema: KSerialSchema) -> bytes:
    """Query /proc/kserial with schema"""
    try:
        with open('/proc/kserial', 'r+b') as f:
            f.write(schema.pack())
            f.flush()
            return f.read(KS_MAX_OUTPUT_SIZE)
    except FileNotFoundError:
        print("Error: /proc/kserial not found. Is k-serial module loaded?", file=sys.stderr)
        sys.exit(1)
    except PermissionError:
        print("Error: Permission denied. Try: sudo kserial ...", file=sys.stderr)
        sys.exit(1)
    except OSError as e:
        if e.errno == 3:
            print(f"Error: Process not found (PID {schema.target_pid})", file=sys.stderr)
        elif e.errno == 1:
            print(f"Error: Permission denied for PID {schema.target_pid}", file=sys.stderr)
        else:
            print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

def parse_tlv_result(data: bytes) -> List[int]:
    """Parse TLV result from kernel"""
    if len(data) < 4:
        return []
    total_len = struct.unpack('I', data[:4])[0]
    if total_len == 0:
        return []
    
    values = []
    offset = 4
    while offset < len(data) and offset < 4 + total_len:
        if offset + 4 > len(data):
            break
        field_id, tlv_len = struct.unpack('HH', data[offset:offset+4])
        offset += 4
        
        if tlv_len == 8:
            value = struct.unpack('Q', data[offset:offset+8])[0]
            values.append(value)
            offset += 8
        else:
            offset += tlv_len
    return values

def get_pids_by_name(name: str) -> List[int]:
    """Get PIDs by process name"""
    try:
        output = subprocess.check_output(['pgrep', name], text=True)
        return [int(pid) for pid in output.strip().split('\n') if pid]
    except subprocess.CalledProcessError:
        return []

def query_fields(struct_name: str, fields: List[str], pid: int = 0, human: bool = False) -> Dict[str, int]:
    """Query fields for a single PID"""
    schema = KSerialSchema()
    schema.struct_name = struct_name
    schema.target_pid = pid
    schema.nr_fields = len(fields)
    
    for i, field in enumerate(fields):
        schema.field_names[i] = field
    
    result = query_kserial(schema)
    values = parse_tlv_result(result)
    return dict(zip(fields, values))

def watch_mode(struct_name: str, fields: List[str], pid: int, interval: float, human: bool):
    """Continuous monitoring mode"""
    try:
        while True:
            os.system('clear')
            print(f"=== Watching PID {pid} ===")
            print(f"Time: {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
            
            try:
                results = query_fields(struct_name, fields, pid, human)
                for field, value in results.items():
                    display = format_bytes(value) if human else str(value)
                    print(f"  {field}: {display}")
            except Exception as e:
                print(f"Error: {e}")
            
            time.sleep(interval)
    except KeyboardInterrupt:
        print("\nStopped.")

def main():
    parser = argparse.ArgumentParser(
        description='Simplified kserial with aliases, batch, and watch',
        epilog='''
Examples:
  # Using aliases
  kserial mem_cgroup @anon @file @kernel
  
  # Human-readable
  kserial mem_cgroup @anon --human-readable
  
  # Batch query
  kserial mem_cgroup @anon --pids=1234,5678
  kserial mem_cgroup @anon --pgrep=nginx
  
  # Watch mode
  kserial mem_cgroup @anon --pid=1234 --watch
        '''
    )
    
    parser.add_argument('struct_type', help='Struct type (mem_cgroup, task_struct)')
    parser.add_argument('fields', nargs='+', help='Field names or aliases (@anon)')
    parser.add_argument('--pid', type=int, default=0, help='Target PID')
    parser.add_argument('--pids', help='Multiple PIDs (comma-separated)')
    parser.add_argument('--pgrep', help='Process name')
    parser.add_argument('--human-readable', '-H', action='store_true', help='Human-readable sizes')
    parser.add_argument('--format', choices=['default', 'json', 'csv'], default='default')
    parser.add_argument('--watch', '-w', action='store_true', help='Watch mode')
    parser.add_argument('--interval', type=float, default=1.0, help='Update interval (seconds)')
    
    args = parser.parse_args()
    
    # Resolve aliases
    resolved_fields = [resolve_alias(args.struct_type, f) for f in args.fields]
    display_fields = [f.lstrip('@') for f in args.fields]
    
    # Determine target PIDs
    if args.pids:
        target_pids = [int(p) for p in args.pids.split(',')]
    elif args.pgrep:
        target_pids = get_pids_by_name(args.pgrep)
        if not target_pids:
            print(f"Error: No processes found matching '{args.pgrep}'", file=sys.stderr)
            sys.exit(1)
    else:
        target_pids = [args.pid]
    
    # Watch mode
    if args.watch:
        if len(target_pids) != 1:
            print("Error: Watch mode supports only one PID", file=sys.stderr)
            sys.exit(1)
        watch_mode(args.struct_type, resolved_fields, target_pids[0], 
                  args.interval, args.human_readable)
        return
    
    # Query
    if len(target_pids) == 1:
        # Single PID
        results = query_fields(args.struct_type, resolved_fields, target_pids[0], args.human_readable)
        
        if args.format == 'json':
            output = {display_fields[i]: v for i, v in enumerate(results.values())}
            print(json.dumps(output))
        elif args.format == 'csv':
            print('field,value')
            for field, value in zip(display_fields, results.values()):
                display = format_bytes(value) if args.human_readable else str(value)
                print(f"{field},{display}")
        else:
            for field, value in zip(display_fields, results.values()):
                display = format_bytes(value) if args.human_readable else str(value)
                print(f"{field}: {display}")
    else:
        # Multiple PIDs
        all_results = {}
        for pid in target_pids:
            try:
                results = query_fields(args.struct_type, resolved_fields, pid, args.human_readable)
                all_results[f"PID_{pid}"] = {display_fields[i]: v for i, v in enumerate(results.values())}
            except:
                all_results[f"PID_{pid}"] = None
        
        if args.format == 'json':
            print(json.dumps(all_results))
        else:
            for pid_key, values in all_results.items():
                if values:
                    print(f"\n=== {pid_key} ===")
                    for field, value in values.items():
                        display = format_bytes(value) if args.human_readable else str(value)
                        print(f"  {field}: {display}")

if __name__ == '__main__':
    main()
