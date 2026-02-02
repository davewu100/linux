#!/usr/bin/env python3
"""
kserial - Universal kernel struct query tool with PID support

Features:
  - Query current process (default)
  - Query specific process by PID
  - Array range reads
  - Block read optimization
"""

import sys
import os
import struct
import re
import argparse
from typing import List, Tuple, Optional

# k-serial UAPI constants
KS_MAX_FIELDS = 128
KS_FIELD_NAME_LEN = 64
KS_MAX_OUTPUT_SIZE = 4096

# Flags
KS_FLAG_ALLOW_NULL = 0x01
KS_FLAG_BLOCK_READ = 0x02
KS_FLAG_RAW_OFFSET = 0x04

class KSerialSchema:
    """Schema structure for k-serial queries"""
    def __init__(self):
        self.nr_fields = 0
        self.flags = 0
        self.struct_name = ""
        self.target_pid = 0        # 0 = current process
        self.reserved = [0, 0, 0]
        self.block_offset = 0
        self.block_size = 0
        self.array_start = 0
        self.array_count = 0
        self.field_names = [""] * KS_MAX_FIELDS
    
    def pack(self) -> bytes:
        """Pack schema into binary format"""
        # Format: nr_fields, flags, struct_name(64), target_pid, reserved[3],
        #         block_offset, block_size, array_start, array_count, field_names[16x64]
        fmt = "II64sI3IIIII" + "64s" * KS_MAX_FIELDS
        
        struct_name_bytes = self.struct_name.encode('utf-8')[:63] + b'\x00'
        field_bytes = [f.encode('utf-8')[:63] + b'\x00' for f in self.field_names]
        
        return struct.pack(fmt,
            self.nr_fields,
            self.flags,
            struct_name_bytes,
            self.target_pid,
            *self.reserved,
            self.block_offset,
            self.block_size,
            self.array_start,
            self.array_count,
            *field_bytes
        )

def parse_array_range(field: str) -> Tuple[str, Optional[int], Optional[int], bool]:
    """
    Parse array range syntax
    
    Returns: (base_name, start_idx, count, is_range)
    
    Examples:
        "state[0..49]" -> ("state", 0, 50, True)
        "state[*]"     -> ("state", 0, -1, True)
        "state[5]"     -> ("state[5]", None, None, False)
        "state"        -> ("state", None, None, False)
    """
    # Check for range syntax: field[start..end]
    match = re.match(r'^(\w+(?:\.\w+)*)\[(\d+)\.\.(\d+)\]$', field)
    if match:
        base = match.group(1)
        start = int(match.group(2))
        end = int(match.group(3))
        return (base, start, end - start + 1, True)
    
    # Check for wildcard: field[*]
    match = re.match(r'^(\w+(?:\.\w+)*)\[\*\]$', field)
    if match:
        base = match.group(1)
        return (base, 0, 0xFFFFFFFF, True)  # Special: read all
    
    # Not a range, return as-is
    return (field, None, None, False)

def query_kserial(schema: KSerialSchema) -> bytes:
    """Query /dev/kserial with schema"""
    try:
        with open('/dev/kserial', 'r+b') as f:
            # Write schema
            f.write(schema.pack())
            f.flush()
            
            # Read result
            result = f.read(KS_MAX_OUTPUT_SIZE)
            return result
    except FileNotFoundError:
        print("Error: /dev/kserial not found", file=sys.stderr)
        print("Is the k-serial module loaded?", file=sys.stderr)
        sys.exit(1)
    except PermissionError:
        print("Error: Permission denied", file=sys.stderr)
        print("Try: sudo kserial ...", file=sys.stderr)
        sys.exit(1)
    except OSError as e:
        if e.errno == 3:  # ESRCH
            print(f"Error: Process not found (PID {schema.target_pid})", file=sys.stderr)
        elif e.errno == 1:  # EPERM
            print(f"Error: Permission denied for PID {schema.target_pid}", file=sys.stderr)
            print("You can only query processes owned by the same user or use sudo", file=sys.stderr)
        else:
            print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

def parse_tlv_result(data: bytes, elem_size: int = 8) -> List[int]:
    """
    Parse TLV result from kernel
    
    Args:
        data: Raw bytes from kernel
        elem_size: Size of each element (default 8 for u64)
    
    Returns:
        List of integer values
    """
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
        
        # Parse TLV header
        field_id, tlv_len = struct.unpack('HH', data[offset:offset+4])
        offset += 4
        
        # Parse TLV data
        if tlv_len == 8:
            # Single u64 value
            value = struct.unpack('Q', data[offset:offset+8])[0]
            values.append(value)
            offset += 8
        elif tlv_len % elem_size == 0:
            # Array of values
            num_elements = tlv_len // elem_size
            fmt = 'Q' * num_elements if elem_size == 8 else 'I' * num_elements
            array_values = struct.unpack(fmt, data[offset:offset+tlv_len])
            values.extend(array_values)
            offset += tlv_len
        else:
            # Unknown format, skip
            offset += tlv_len
    
    return values

def main():
    parser = argparse.ArgumentParser(
        description='Query kernel struct fields using k-serial',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
Examples:
  # Query current process
  kserial task_struct pid comm state

  # Query specific process
  kserial task_struct pid comm state --pid=1234

  # Query memory stats of another process
  kserial mem_cgroup vmstats.state[9] vmstats.state[10] --pid=5678

  # Block read with PID
  kserial mem_cgroup vmstats.state[0..49] --pid=1234
  kserial mem_cgroup vmstats.state[*] --pid=1234
        '''
    )
    
    parser.add_argument('struct_type', 
                        help='Struct type (cgroup, mem_cgroup, task_struct)')
    parser.add_argument('fields', nargs='+',
                        help='Field names or ranges (e.g., pid, state[0..9])')
    parser.add_argument('--pid', type=int, default=0,
                        help='Target process PID (0=current process, default: 0)')
    parser.add_argument('-v', '--verbose', action='store_true',
                        help='Verbose output')
    
    args = parser.parse_args()
    
    # Build schema
    schema = KSerialSchema()
    schema.struct_name = args.struct_type
    schema.target_pid = args.pid
    
    # Check if first field is a block read
    base, start, count, is_range = parse_array_range(args.fields[0])
    
    if is_range:
        # Block read mode
        schema.flags |= KS_FLAG_BLOCK_READ
        schema.nr_fields = 1
        schema.field_names[0] = base
        schema.array_start = start if start is not None else 0
        schema.array_count = count if count is not None else 0xFFFFFFFF
        
        if args.verbose:
            print(f"Block read mode: {base}[{start}..{start+count-1 if count != 0xFFFFFFFF else '*'}]")
        
        # Query kernel
        result = query_kserial(schema)
        values = parse_tlv_result(result)
        
        # Print results
        pid_str = f" (PID {args.pid})" if args.pid > 0 else ""
        if count == 0xFFFFFFFF:
            print(f"{base}[*]{pid_str}: {len(values)} elements")
        else:
            print(f"{base}[{start}..{start+count-1}]{pid_str}:")
        
        for i, val in enumerate(values):
            print(f"  [{start+i}] = {val}")
    else:
        # Normal field query
        schema.nr_fields = len(args.fields)
        for i, field in enumerate(args.fields):
            schema.field_names[i] = field
        
        if args.verbose:
            print(f"Querying {schema.nr_fields} fields from {args.struct_type}")
            if args.pid > 0:
                print(f"Target PID: {args.pid}")
        
        # Query kernel
        result = query_kserial(schema)
        values = parse_tlv_result(result)
        
        # Print results
        pid_str = f" (PID {args.pid})" if args.pid > 0 else ""
        if pid_str:
            print(f"Results for{pid_str}:")
        
        for field, val in zip(args.fields, values):
            print(f"  {field}: {val}")

if __name__ == '__main__':
    main()
