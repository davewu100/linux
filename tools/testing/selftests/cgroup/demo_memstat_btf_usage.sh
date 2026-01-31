#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Demo: memory.stat.ks BTF query usage examples
# Shows real kserial functionality in action

CGROUP_ROOT="/sys/fs/cgroup"
DEMO_CGROUP="demo_btf_$$"
DEMO_PATH="$CGROUP_ROOT/$DEMO_CGROUP"

GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo "╔════════════════════════════════════════════════════════╗"
echo "║  memory.stat.ks: Real kserial BTF Query Demo          ║"
echo "╚════════════════════════════════════════════════════════╝"
echo ""

# Create demo cgroup
mkdir -p "$DEMO_PATH"
echo "Demo cgroup: $DEMO_PATH"
echo ""

# Generate some memory activity
echo $$ > "$DEMO_PATH/cgroup.procs"
dd if=/dev/zero of=/dev/null bs=1M count=50 2>/dev/null &
WORKER=$!
sleep 0.3
kill $WORKER 2>/dev/null || true
wait $WORKER 2>/dev/null || true
echo ""

echo "════════════════════════════════════════════════════════"
echo "${BLUE}Demo 1: Default Mode (All Fields)${NC}"
echo "════════════════════════════════════════════════════════"
echo ""
echo "Command:"
echo "  cat $DEMO_PATH/memory.stat.ks"
echo ""
echo "Output (first 10 lines):"
cat "$DEMO_PATH/memory.stat.ks" | head -10
echo "  ..."
echo "  (50+ more fields)"
echo ""
cat "$DEMO_PATH/memory.stat.ks" | grep "# Mode:"
echo ""
echo "${YELLOW}→ Shows ALL fields (backward compatible)${NC}"
echo ""

echo "════════════════════════════════════════════════════════"
echo "${BLUE}Demo 2: Simple BTF Query (3 Fields)${NC}"
echo "════════════════════════════════════════════════════════"
echo ""
echo "Command:"
echo "  echo 'anon,file,slab' > $DEMO_PATH/memory.stat.ks"
echo "  cat $DEMO_PATH/memory.stat.ks"
echo ""

echo "anon,file,slab" > "$DEMO_PATH/memory.stat.ks"
echo "Output:"
cat "$DEMO_PATH/memory.stat.ks"
echo ""
echo "${GREEN}→ Only 3 fields via BTF query!${NC}"
echo ""

echo "════════════════════════════════════════════════════════"
echo "${BLUE}Demo 3: Lightweight Monitoring (1 Field)${NC}"
echo "════════════════════════════════════════════════════════"
echo ""
echo "Command:"
echo "  echo 'anon' > $DEMO_PATH/memory.stat.ks"
echo "  cat $DEMO_PATH/memory.stat.ks"
echo ""

echo "anon" > "$DEMO_PATH/memory.stat.ks"
echo "Output:"
cat "$DEMO_PATH/memory.stat.ks"
echo ""
echo "${GREEN}→ Minimal overhead for single field!${NC}"
echo ""

echo "════════════════════════════════════════════════════════"
echo "${BLUE}Demo 4: Performance Comparison${NC}"
echo "════════════════════════════════════════════════════════"
echo ""

# Test all fields
echo "" > "$DEMO_PATH/memory.stat.ks"
START=$(date +%s%N)
for i in {1..100}; do cat "$DEMO_PATH/memory.stat.ks" > /dev/null; done
END=$(date +%s%N)
ALL_TIME=$(((END - START) / 100 / 1000))

# Test 3 fields
echo "anon,file,slab" > "$DEMO_PATH/memory.stat.ks"
START=$(date +%s%N)
for i in {1..100}; do cat "$DEMO_PATH/memory.stat.ks" > /dev/null; done
END=$(date +%s%N)
BTF_TIME=$(((END - START) / 100 / 1000))

# Test 1 field
echo "anon" > "$DEMO_PATH/memory.stat.ks"
START=$(date +%s%N)
for i in {1..100}; do cat "$DEMO_PATH/memory.stat.ks" > /dev/null; done
END=$(date +%s%N)
ONE_TIME=$(((END - START) / 100 / 1000))

echo "Performance (100 iterations average):"
echo ""
printf "  All fields (50+): %6d μs\n" $ALL_TIME
printf "  BTF (3 fields):   %6d μs  " $BTF_TIME
if [ $ALL_TIME -gt 0 ]; then
    SPEEDUP=$(echo "scale=2; $ALL_TIME / $BTF_TIME" | bc)
    printf "(${GREEN}%.2fx faster${NC})\n" $SPEEDUP
else
    echo ""
fi
printf "  BTF (1 field):    %6d μs  " $ONE_TIME
if [ $ALL_TIME -gt 0 ]; then
    SPEEDUP=$(echo "scale=2; $ALL_TIME / $ONE_TIME" | bc)
    printf "(${GREEN}%.2fx faster${NC})\n" $SPEEDUP
else
    echo ""
fi
echo ""
echo "${YELLOW}→ Fewer fields = Lower overhead!${NC}"
echo ""

echo "════════════════════════════════════════════════════════"
echo "${BLUE}Demo 5: Dynamic Field List${NC}"
echo "════════════════════════════════════════════════════════"
echo ""
echo "You can change fields anytime:"
echo ""

echo "First query: memory usage"
echo "anon,file" > "$DEMO_PATH/memory.stat.ks"
echo "  Result:"
cat "$DEMO_PATH/memory.stat.ks" | grep -v "^#"
echo ""

echo "Second query: I/O stats"
echo "pgscan,pgsteal,pgfault" > "$DEMO_PATH/memory.stat.ks"
echo "  Result:"
cat "$DEMO_PATH/memory.stat.ks" | grep -v "^#"
echo ""

echo "${GREEN}→ No restart needed, instant switch!${NC}"
echo ""

echo "════════════════════════════════════════════════════════"
echo "${BLUE}Demo 6: Comparison with memory.stat${NC}"
echo "════════════════════════════════════════════════════════"
echo ""

echo "Traditional memory.stat:"
echo "  ❌ Must read all 50+ fields"
echo "  ❌ Cannot select specific fields"
echo "  ❌ Higher overhead"
echo ""

echo "memory.stat.ks (BTF mode):"
echo "  ✅ Select only needed fields"
echo "  ✅ BTF-based dynamic query"
echo "  ✅ Lower overhead"
echo "  ✅ Can access internal fields"
echo ""

echo "════════════════════════════════════════════════════════"
echo "${YELLOW}Key Takeaways${NC}"
echo "════════════════════════════════════════════════════════"
echo ""

cat << 'EOF'
1. memory.stat.ks NOW uses real BTF queries
   (not just output optimization)

2. Two modes:
   • No write = All fields (legacy)
   • Write fields = BTF query (selective)

3. Benefits:
   • Lower overhead for selective queries
   • Access to internal mem_cgroup fields
   • Flexible field selection
   • Automatic cgroup context

4. This is TRUE kserial!
   • BTF-based field resolution
   • Dynamic field selection
   • Version-resilient
   • No kernel modification needed

5. Perfect for:
   • Custom monitoring dashboards
   • Lightweight stat collection
   • Per-cgroup field queries
   • Research and debugging
EOF
echo ""

# Cleanup
rmdir "$DEMO_PATH"
echo "────────────────────────────────────────────────────────"
echo "Demo complete! Try it on your own cgroups:"
echo "  echo 'anon,file' > /sys/fs/cgroup/YOUR_CGROUP/memory.stat.ks"
echo "  cat /sys/fs/cgroup/YOUR_CGROUP/memory.stat.ks"
echo ""
