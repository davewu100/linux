#!/usr/bin/env python3
"""
k-serial Python Client

A Python interface to k-serial for querying cgroup fields.
This demonstrates how easy it is to use k-serial from any language.
"""

import struct
import os
import sys
from dataclasses import dataclass
from typing import List, Dict, Any

# Constants (must match kernel UAPI)
KS_MAX_FIELDS = 16
KS_FIELD_NAME_LEN = 32
KS_MAX_OUTPUT_SIZE = 4096
KS_PROCFS_PATH = "/proc/kserial"

# Struct formats
# struct ks_schema { u32 nr_fields; char field_names[16][32]; }
SCHEMA_FMT = f"I{KS_MAX_FIELDS * KS_FIELD_NAME_LEN}s"

# struct ks_result { u32 total_len; u8 data[4096]; }
RESULT_HEADER_FMT = "I"

# struct ks_tlv { u16 field_id; u16 len; u8 data[]; }
TLV_HEADER_FMT = "HH"
TLV_HEADER_SIZE = struct.calcsize(TLV_HEADER_FMT)


@dataclass
class KSerialResult:
    """Parsed k-serial query result"""
    fields: Dict[str, Any]
    raw_data: bytes
    total_len: int


class KSerialClient:
    """Client for querying cgroup fields via k-serial"""
    
    def __init__(self, procfs_path: str = KS_PROCFS_PATH):
        self.procfs_path = procfs_path
    
    def query(self, field_names: List[str]) -> KSerialResult:
        """
        Query specified fields from current process's cgroup.
        
        Args:
            field_names: List of field names to query (max 16)
        
        Returns:
            KSerialResult with parsed field values
        
        Raises:
            ValueError: Invalid input
            OSError: Kernel communication error
        """
        if not field_names:
            raise ValueError("Must specify at least one field")
        
        if len(field_names) > KS_MAX_FIELDS:
            raise ValueError(f"Too many fields (max {KS_MAX_FIELDS})")
        
        # Build schema
        schema = self._build_schema(field_names)
        
        # Query kernel
        result_bytes = self._query_kernel(schema)
        
        # Parse result
        return self._parse_result(result_bytes, field_names)
    
    def _build_schema(self, field_names: List[str]) -> bytes:
        """Build binary schema structure"""
        # Pack field names into fixed-size array
        field_data = b""
        for name in field_names:
            if len(name) >= KS_FIELD_NAME_LEN:
                raise ValueError(f"Field name too long: {name}")
            # Null-terminated, padded to KS_FIELD_NAME_LEN
            field_bytes = name.encode('utf-8').ljust(KS_FIELD_NAME_LEN, b'\x00')
            field_data += field_bytes
        
        # Pad remaining slots
        padding = (KS_MAX_FIELDS - len(field_names)) * KS_FIELD_NAME_LEN
        field_data += b'\x00' * padding
        
        # Pack schema: nr_fields + field_names array
        schema = struct.pack(SCHEMA_FMT, len(field_names), field_data)
        return schema
    
    def _query_kernel(self, schema: bytes) -> bytes:
        """Send schema to kernel and read result"""
        try:
            with open(self.procfs_path, 'r+b', buffering=0) as f:
                # Write schema
                f.write(schema)
                f.flush()
                
                # Read result
                f.seek(0)
                result = f.read()
                
                return result
        
        except FileNotFoundError:
            raise OSError(
                f"Cannot open {self.procfs_path}. "
                "Is the k-serial kernel module loaded?"
            )
        except PermissionError:
            raise OSError(
                f"Permission denied accessing {self.procfs_path}. "
                "Try running with sudo?"
            )
    
    def _parse_result(self, data: bytes, field_names: List[str]) -> KSerialResult:
        """Parse TLV-encoded result"""
        if len(data) < 4:
            raise ValueError("Invalid result: too short")
        
        # Parse total_len
        total_len = struct.unpack(RESULT_HEADER_FMT, data[:4])[0]
        
        # Parse TLV entries
        fields = {}
        offset = 4  # Skip total_len field
        
        while offset < 4 + total_len:
            if offset + TLV_HEADER_SIZE > len(data):
                break
            
            # Parse TLV header
            field_id, value_len = struct.unpack(
                TLV_HEADER_FMT,
                data[offset:offset + TLV_HEADER_SIZE]
            )
            offset += TLV_HEADER_SIZE
            
            # Validate field_id
            if field_id >= len(field_names):
                raise ValueError(f"Invalid field_id: {field_id}")
            
            # Extract value
            value_bytes = data[offset:offset + value_len]
            offset += value_len
            
            # Decode based on size (assume unsigned integer)
            if value_len == 1:
                value = struct.unpack('B', value_bytes)[0]
            elif value_len == 2:
                value = struct.unpack('H', value_bytes)[0]
            elif value_len == 4:
                value = struct.unpack('I', value_bytes)[0]
            elif value_len == 8:
                value = struct.unpack('Q', value_bytes)[0]
            else:
                # Unknown size, keep as bytes
                value = value_bytes
            
            fields[field_names[field_id]] = value
        
        return KSerialResult(
            fields=fields,
            raw_data=data,
            total_len=total_len
        )


def print_result(result: KSerialResult, field_names: List[str]):
    """Pretty-print query result"""
    print("\n=== k-serial Query Results ===")
    print(f"Total data: {result.total_len} bytes\n")
    
    for name in field_names:
        if name in result.fields:
            value = result.fields[name]
            print(f"  {name:25s} = {value}")
        else:
            print(f"  {name:25s} = (not found)")
    
    print()


def main():
    import argparse
    
    parser = argparse.ArgumentParser(
        description="k-serial: Query cgroup fields using BTF-based reflection",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s level
  %(prog)s level nr_descendants max_depth

Whitelisted fields:
  - level                  Cgroup depth in hierarchy
  - max_depth              Maximum depth allowed
  - nr_descendants         Number of descendant cgroups
  - nr_dying_descendants   Number of dying descendants
  - max_descendants        Maximum descendants allowed
        """
    )
    
    parser.add_argument(
        'fields',
        nargs='+',
        help='Field names to query'
    )
    
    parser.add_argument(
        '--json',
        action='store_true',
        help='Output as JSON'
    )
    
    parser.add_argument(
        '--raw',
        action='store_true',
        help='Show raw TLV data (debug)'
    )
    
    args = parser.parse_args()
    
    # Create client and query
    client = KSerialClient()
    
    try:
        print(f"Querying {len(args.fields)} field(s) from current cgroup:")
        for i, name in enumerate(args.fields):
            print(f"  [{i}] {name}")
        
        result = client.query(args.fields)
        
        if args.json:
            import json
            print(json.dumps(result.fields, indent=2))
        elif args.raw:
            print(f"\nRaw TLV data ({result.total_len} bytes):")
            print(result.raw_data[4:4+result.total_len].hex())
            print()
            print_result(result, args.fields)
        else:
            print_result(result, args.fields)
        
        return 0
    
    except Exception as e:
        print(f"\nERROR: {e}", file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
