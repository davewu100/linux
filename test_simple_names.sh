#!/bin/bash
# Quick test for simple field name support

CGROUP="/sys/fs/cgroup"

echo "Testing simple field name translation:"
echo "========================================"
echo ""

# Test 1: Simple field names
echo "Test 1: Using simple field names"
echo "anon,file,slab" > "$CGROUP/memory.stat.ks"
echo "Result:"
cat "$CGROUP/memory.stat.ks"
echo ""

# Test 2: With flush
echo "Test 2: With flush flag"
echo "flush,anon,file" > "$CGROUP/memory.stat.ks"
echo "Result:"
cat "$CGROUP/memory.stat.ks"
echo ""

# Test 3: Mixed simple and BTF paths
echo "Test 3: Mixed simple names and BTF paths"
echo "anon,css.id,file" > "$CGROUP/memory.stat.ks"
echo "Result:"
cat "$CGROUP/memory.stat.ks"
echo ""

# Test 4: Reset
echo "Test 4: Reset to default"
echo "" > "$CGROUP/memory.stat.ks"
echo "Field count: $(cat "$CGROUP/memory.stat.ks" | wc -l)"
echo ""

echo "All tests completed!"
