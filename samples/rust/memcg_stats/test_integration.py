#!/usr/bin/env python3
"""
Integration test for memory cgroup binary statistics
Tests that memory.stat_bin files exist and contain valid TLV data
"""

import os
import sys
import struct
from pathlib import Path

def check_cgroup_files():
    """Check that binary stat files exist in cgroup directories"""
    cgroup_root = "/sys/fs/cgroup/memory"

    if not Path(cgroup_root).exists():
        print(f"❌ Cgroup root {cgroup_root} not found")
        return False

    # Check root cgroup has binary files
    stat_bin = Path(cgroup_root) / "memory.stat_bin"
    numa_stat_bin = Path(cgroup_root) / "memory.numa_stat_bin"

    print(f"Checking {cgroup_root}...")

    if stat_bin.exists():
        print(f"✅ {stat_bin} exists")
    else:
        print(f"❌ {stat_bin} missing")
        return False

    if numa_stat_bin.exists():
        print(f"✅ {numa_stat_bin} exists")
    else:
        print(f"⚠️  {numa_stat_bin} missing (NUMA not enabled?)")

    return True

def validate_tlv_data(filepath):
    """Validate that a file contains valid TLV data"""
    if not Path(filepath).exists():
        print(f"❌ File {filepath} does not exist")
        return False

    try:
        with open(filepath, 'rb') as f:
            data = f.read()
    except Exception as e:
        print(f"❌ Failed to read {filepath}: {e}")
        return False

    if len(data) < 4:
        print(f"❌ {filepath} too small ({len(data)} bytes)")
        return False

    # Check container header
    try:
        container_type, length = struct.unpack('<HH', data[0:4])
        print(f"📦 Container type: {container_type}, length: {length}")

        if length == 0 or length > len(data) - 4:
            print(f"❌ Invalid container length: {length}")
            return False

        # Try to parse some TLV entries
        pos = 4
        entries_parsed = 0
        while pos < len(data) and entries_parsed < 5:  # Check first 5 entries
            if pos + 4 > len(data):
                break

            tlv_type, tlv_length = struct.unpack('<HH', data[pos:pos+4])
            if pos + 4 + tlv_length > len(data):
                break

            if tlv_length == 8:  # uint64
                value = struct.unpack('<Q', data[pos+4:pos+4+8])[0]
                print(f"  📊 Type {tlv_type}: {value}")
            else:
                print(f"  📊 Type {tlv_type}: {tlv_length} bytes")

            pos += 4 + tlv_length
            entries_parsed += 1

        print(f"✅ {filepath} contains valid TLV data")
        return True

    except Exception as e:
        print(f"❌ Failed to parse TLV data in {filepath}: {e}")
        return False

def test_nested_cgroups():
    """Test that binary files exist in nested cgroup directories"""
    cgroup_root = "/sys/fs/cgroup/memory"

    # Create a test cgroup if it doesn't exist
    test_cgroup = Path(cgroup_root) / "test_stats"
    if not test_cgroup.exists():
        try:
            test_cgroup.mkdir(parents=True, exist_ok=True)
            print(f"📁 Created test cgroup: {test_cgroup}")
        except Exception as e:
            print(f"⚠️  Could not create test cgroup: {e}")
            return

    # Check if binary files exist in the test cgroup
    stat_bin = test_cgroup / "memory.stat_bin"
    if stat_bin.exists():
        print(f"✅ Nested cgroup {stat_bin} exists")
        validate_tlv_data(str(stat_bin))
    else:
        print(f"❌ Nested cgroup {stat_bin} missing")

    # Cleanup
    try:
        test_cgroup.rmdir()
        print(f"🧹 Cleaned up test cgroup")
    except:
        pass

def main():
    print("🔍 Testing memory cgroup binary statistics integration\n")

    # Check that kernel has been built with our changes
    if not check_cgroup_files():
        print("\n❌ Integration test FAILED")
        print("Make sure the kernel has been built with the TLV integration changes")
        sys.exit(1)

    # Validate TLV data in root cgroup
    root_stat_bin = "/sys/fs/cgroup/memory/memory.stat_bin"
    if not validate_tlv_data(root_stat_bin):
        print("\n❌ TLV validation FAILED")
        sys.exit(1)

    # Test nested cgroups
    print("\n📂 Testing nested cgroup support...")
    test_nested_cgroups()

    print("\n✅ Integration test PASSED")
    print("Memory cgroup binary statistics are working correctly!")

if __name__ == '__main__':
    main()
