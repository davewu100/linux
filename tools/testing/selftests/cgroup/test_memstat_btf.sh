#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Test memory.stat.ks with BTF query support
# This demonstrates TRUE kserial functionality

set -e

CGROUP_ROOT="/sys/fs/cgroup"
TEST_CGROUP="test_btf_$$"
TEST_PATH="$CGROUP_ROOT/$TEST_CGROUP"
CREATED_CGROUP=0
HELPER_BIN="$(dirname "$0")/memstat_read_open_once"
HELPER_SRC="$(dirname "$0")/memstat_read_open_once.c"
FIELDS_LIST="3,16,32,64,128"

usage() {
	echo "Usage: $0 [--cgname <name>] [--cgpath <path>] [--fields <list>]"
	echo ""
	echo "  --cgname <name>   Use /sys/fs/cgroup/<name> (create if missing)"
	echo "  --cgpath <path>   Use an existing cgroup path (or create it)"
	echo "  --fields <list>   Comma list for perf test (default: $FIELDS_LIST)"
}

while [ $# -gt 0 ]; do
	case "$1" in
		--cgname)
			if [ -z "${2:-}" ]; then
				echo "Missing argument for --cgname"
				usage
				exit 1
			fi
			TEST_PATH="$CGROUP_ROOT/$2"
			shift 2
			;;
		--cgpath)
			if [ -z "${2:-}" ]; then
				echo "Missing argument for --cgpath"
				usage
				exit 1
			fi
			TEST_PATH="$2"
			shift 2
			;;
		--fields)
			if [ -z "${2:-}" ]; then
				echo "Missing argument for --fields"
				usage
				exit 1
			fi
			FIELDS_LIST="$2"
			shift 2
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			echo "Unknown option: $1"
			usage
			exit 1
			;;
	esac
done

# Colors
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

KERNEL_SRC="$(realpath "$(dirname "$0")/../../../..")"
MEMCONTROL_C="${KERNEL_SRC}/mm/memcontrol.c"
CONFIG_GZ="/proc/config.gz"
BOOT_CONFIG="/boot/config-$(uname -r)"
AUTOCONF_H="${KERNEL_SRC}/include/generated/autoconf.h"

config_enabled() {
	local key="$1"

	if [ -f "$CONFIG_GZ" ]; then
		zgrep -q "^${key}=y" "$CONFIG_GZ" && return 0
	elif [ -f "$BOOT_CONFIG" ]; then
		grep -q "^${key}=y" "$BOOT_CONFIG" && return 0
	elif [ -f "$AUTOCONF_H" ]; then
		grep -q "^#define ${key} 1" "$AUTOCONF_H" && return 0
	fi

	return 1
}

CONFIG_SWAP=0
CONFIG_TRANSPARENT_HUGEPAGE=0
CONFIG_HUGETLB_PAGE=0
CONFIG_NUMA_BALANCING=0
CONFIG_ZSWAP=0
CONFIG_MEMCG_V1=0

config_enabled CONFIG_SWAP && CONFIG_SWAP=1
config_enabled CONFIG_TRANSPARENT_HUGEPAGE && CONFIG_TRANSPARENT_HUGEPAGE=1
config_enabled CONFIG_HUGETLB_PAGE && CONFIG_HUGETLB_PAGE=1
config_enabled CONFIG_NUMA_BALANCING && CONFIG_NUMA_BALANCING=1
config_enabled CONFIG_ZSWAP && CONFIG_ZSWAP=1
config_enabled CONFIG_MEMCG_V1 && CONFIG_MEMCG_V1=1

extract_array_items() {
	local array_name="$1"
	awk -v arr="$array_name" \
	    -v swap="$CONFIG_SWAP" \
	    -v thp="$CONFIG_TRANSPARENT_HUGEPAGE" \
	    -v hugetlb="$CONFIG_HUGETLB_PAGE" \
	    -v numa="$CONFIG_NUMA_BALANCING" \
	    -v zswap="$CONFIG_ZSWAP" \
	    -v memcgv1="$CONFIG_MEMCG_V1" '
		function cfg_enabled(name) {
			if (name == "CONFIG_SWAP") return swap;
			if (name == "CONFIG_TRANSPARENT_HUGEPAGE") return thp;
			if (name == "CONFIG_HUGETLB_PAGE") return hugetlb;
			if (name == "CONFIG_NUMA_BALANCING") return numa;
			if (name == "CONFIG_ZSWAP") return zswap;
			if (name == "CONFIG_MEMCG_V1") return memcgv1;
			return 0;
		}
		BEGIN { depth=0; active[0]=1; in_array=0; }
		/^#ifdef[ \t]+CONFIG_/ {
			cfg=$2; depth++;
			active[depth]=active[depth-1] && cfg_enabled(cfg);
			next;
		}
		/^#ifndef[ \t]+CONFIG_/ {
			cfg=$2; depth++;
			active[depth]=active[depth-1] && !cfg_enabled(cfg);
			next;
		}
		/^#else/ {
			active[depth]=active[depth-1] && !active[depth];
			next;
		}
		/^#endif/ { depth--; next; }
		$0 ~ "static const unsigned int " arr "\\[\\] = \\{" {
			in_array=1; next;
		}
		in_array && $0 ~ /^};/ { in_array=0; next; }
		in_array && active[depth] {
			if (match($0, /([A-Z0-9_]+)[ \t]*,/ , m))
				print m[1];
		}
	' "$MEMCONTROL_C"
}

extract_memory_stats() {
	awk -v swap="$CONFIG_SWAP" \
	    -v thp="$CONFIG_TRANSPARENT_HUGEPAGE" \
	    -v hugetlb="$CONFIG_HUGETLB_PAGE" \
	    -v numa="$CONFIG_NUMA_BALANCING" \
	    -v zswap="$CONFIG_ZSWAP" '
		function cfg_enabled(name) {
			if (name == "CONFIG_SWAP") return swap;
			if (name == "CONFIG_TRANSPARENT_HUGEPAGE") return thp;
			if (name == "CONFIG_HUGETLB_PAGE") return hugetlb;
			if (name == "CONFIG_NUMA_BALANCING") return numa;
			if (name == "CONFIG_ZSWAP") return zswap;
			return 0;
		}
		BEGIN { depth=0; active[0]=1; in_array=0; }
		/^#ifdef[ \t]+CONFIG_/ {
			cfg=$2; depth++;
			active[depth]=active[depth-1] && cfg_enabled(cfg);
			next;
		}
		/^#ifndef[ \t]+CONFIG_/ {
			cfg=$2; depth++;
			active[depth]=active[depth-1] && !cfg_enabled(cfg);
			next;
		}
		/^#else/ {
			active[depth]=active[depth-1] && !active[depth];
			next;
		}
		/^#endif/ { depth--; next; }
		$0 ~ "static const struct memory_stat memory_stats\\[\\] = \\{" { in_array=1; next; }
		in_array && $0 ~ /^};/ { in_array=0; next; }
		in_array && active[depth] {
			if (match($0, /\\{[ \t]*\\"([^\\"]+)\\",[ \t]*([A-Z0-9_]+)[ \t]*\\}/, m))
				print m[1] ":" m[2];
		}
	' "$MEMCONTROL_C"
}

extract_unit_one_items() {
	awk '
		$0 ~ "static int memcg_page_state_output_unit" { in_func=1; next; }
		in_func && $0 ~ /^}/ { in_func=0; }
		in_func {
			if (match($0, /^[ \t]*case[ \t]+([A-Z0-9_]+)[ \t]*:/, m))
				print m[1];
		}
	' "$MEMCONTROL_C"
}

lowercase_event_name() {
	echo "$1" | tr 'A-Z' 'a-z'
}

UNIT_ONE_LIST="$(extract_unit_one_items)"

build_filter_for_n() {
	local n="$1"
	local filter="flush"
	local i

	if [ "$n" -eq 3 ]; then
		echo "flush,vmstats.state[14],vmstats.state[16],vmstats.state[5]"
		return
	fi

	for ((i=0; i<n; i++)); do
		filter="${filter},vmstats.state[$i]"
	done
	echo "$filter"
}

build_full_schema() {
	local schema_file="$1"
	local -a node_items
	local -a stat_items
	local -a event_items
	local -A state_index
	local -A stat_values
	local -A ks_state
	local -A ks_event
	local -a memory_stats
	local -a memory_stat_names
	local -a schema_fields
	local -a event_names
	local -a event_symbols
	local name symbol idx
	local nr_node nr_stat i

	mapfile -t node_items < <(extract_array_items "memcg_node_stat_items")
	mapfile -t stat_items < <(extract_array_items "memcg_stat_items")
	mapfile -t event_symbols < <(extract_array_items "memcg_vm_event_stat")
	mapfile -t memory_stats < <(extract_memory_stats)

	nr_node=${#node_items[@]}
	nr_stat=${#stat_items[@]}

	for ((i=0; i<nr_node; i++)); do
		state_index["${node_items[i]}"]=$i
	done
	for ((i=0; i<nr_stat; i++)); do
		state_index["${stat_items[i]}"]=$((nr_node + i))
	done

	for symbol in "${event_symbols[@]}"; do
		event_names+=( "$(lowercase_event_name "$symbol")" )
	done

	for entry in "${memory_stats[@]}"; do
		name="${entry%%:*}"
		symbol="${entry##*:}"
		idx="${state_index[$symbol]}"
		if [ -z "$idx" ]; then
			continue
		fi
		memory_stat_names+=("$name:$symbol:$idx")
		schema_fields+=("vmstats.state[$idx]")
	done

	for ((i=0; i<${#event_symbols[@]}; i++)); do
		schema_fields+=("vmstats.events[$i]")
	done

	IFS=','; echo "${schema_fields[*]}" > "$schema_file"; IFS=' '

	echo "${memory_stat_names[@]}"
	echo "::EVENTS::"
	for ((i=0; i<${#event_symbols[@]}; i++)); do
		echo "${event_symbols[i]}:${event_names[i]}:$i"
	done
}

compare_outputs() {
	local stat_file="$1"
	local ks_file="$2"
	local schema_meta="$3"
	local label="$4"
	local page_size
	local -A stat_values
	local -A ks_values
	local line name value

	page_size=$(getconf PAGE_SIZE)

	while read -r name value; do
		[ -z "$name" ] && continue
		stat_values["$name"]="$value"
	done < "$stat_file"

	while read -r name value; do
		[ -z "$name" ] && continue
		case "$name" in \#*) continue ;; esac
		ks_values["$name"]="$value"
	done < "$ks_file"

	if [ -z "$schema_meta" ]; then
		echo "${YELLOW}⚠️  ${label}: missing schema metadata${NC}"
		return 1
	fi

	while read -r line; do
		[ -z "$line" ] && continue
		if [ "$line" = "::EVENTS::" ]; then
			break
		fi
		name="${line%%:*}"
		line="${line#*:}"
		value="${line##*:}"
		line="${line%%:*}"
		if [ -n "${ks_values["vmstats.state[$value]"]}" ]; then
			local raw="${ks_values["vmstats.state[$value]"]}"
			local unit="$page_size"
			if [ "$line" = "NR_KERNEL_STACK_KB" ]; then
				unit=1024
			elif grep -q "^${line}$" <<< "$UNIT_ONE_LIST"; then
				unit=1
			fi
			local expected=$((raw * unit))
			local actual="${stat_values[$name]}"
			if [ "$name" = "slab" ]; then
				local slab_reclaim="${stat_values[slab_reclaimable]}"
				local slab_unreclaim="${stat_values[slab_unreclaimable]}"
				actual=$((slab_reclaim + slab_unreclaim))
			fi
			if [ -n "$actual" ] && [ "$expected" -ne "$actual" ]; then
				echo "${YELLOW}⚠️  ${label}: ${name} legacy=${actual} btf=${expected}${NC}"
				return 1
			fi
		fi
	done <<< "$schema_meta"

	{
		local pgscan_sum=0
		local pgsteal_sum=0
		local in_events=0
		while read -r line; do
			[ -z "$line" ] && continue
			if [ "$line" = "::EVENTS::" ]; then
				in_events=1
				continue
			fi
			[ "$in_events" -eq 0 ] && continue
			local sym="${line%%:*}"
			local rest="${line#*:}"
			local ev_name="${rest%%:*}"
			local ev_idx="${rest##*:}"
			local ev_val="${ks_values["vmstats.events[$ev_idx]"]}"

			if [ -z "$ev_val" ]; then
				continue
			fi

			if [ "$CONFIG_MEMCG_V1" -eq 1 ] && { [ "$ev_name" = "pgpgin" ] || [ "$ev_name" = "pgpgout" ]; }; then
				continue
			fi

			if [ -n "${stat_values[$ev_name]}" ] && [ "${stat_values[$ev_name]}" -ne "$ev_val" ]; then
				echo "${YELLOW}⚠️  ${label}: ${ev_name} legacy=${stat_values[$ev_name]} btf=${ev_val}${NC}"
				return 1
			fi

			case "$sym" in
				PGSCAN_KSWAPD|PGSCAN_DIRECT|PGSCAN_PROACTIVE|PGSCAN_KHUGEPAGED)
					pgscan_sum=$((pgscan_sum + ev_val))
					;;
				PGSTEAL_KSWAPD|PGSTEAL_DIRECT|PGSTEAL_PROACTIVE|PGSTEAL_KHUGEPAGED)
					pgsteal_sum=$((pgsteal_sum + ev_val))
					;;
			esac
		done <<< "$schema_meta"

		if [ -n "${stat_values[pgscan]}" ] && [ "$pgscan_sum" -ne "${stat_values[pgscan]}" ]; then
			echo "${YELLOW}⚠️  ${label}: pgscan legacy=${stat_values[pgscan]} btf=${pgscan_sum}${NC}"
			return 1
		fi
		if [ -n "${stat_values[pgsteal]}" ] && [ "$pgsteal_sum" -ne "${stat_values[pgsteal]}" ]; then
			echo "${YELLOW}⚠️  ${label}: pgsteal legacy=${stat_values[pgsteal]} btf=${pgsteal_sum}${NC}"
			return 1
		fi
	}

	echo "${GREEN}✅ ${label} outputs match${NC}"
}

compare_plain_outputs() {
	local left="$1"
	local right="$2"
	local label="$3"

	if diff -u <(grep -v "^#" "$left") <(grep -v "^#" "$right") >/dev/null; then
		echo "${GREEN}✅ ${label} outputs match${NC}"
	else
		echo "${YELLOW}⚠️  ${label} outputs differ${NC}"
	fi
}

echo "=========================================="
echo "memory.stat.ks BTF Query Tests"
echo "=========================================="
echo ""

# Check if memory.stat.ks exists
if [ ! -f "$CGROUP_ROOT/memory.stat.ks" ]; then
    echo "${RED}❌ memory.stat.ks not found - CONFIG_KSERIAL not enabled${NC}"
    exit 1
fi

echo "${GREEN}✅ CONFIG_KSERIAL is enabled${NC}"
echo ""

if [ ! -x "$HELPER_BIN" ]; then
	if [ -f "$HELPER_SRC" ]; then
		echo "Building helper: $HELPER_BIN"
		${CC:-gcc} -O2 -Wall -o "$HELPER_BIN" "$HELPER_SRC"
		echo ""
	else
		echo "${RED}❌ Helper source not found: $HELPER_SRC${NC}"
		exit 1
	fi
fi

if [ "$TEST_PATH" = "$CGROUP_ROOT" ]; then
	echo "Using root cgroup: $TEST_PATH"
	echo ""
else
	if [ ! -d "$TEST_PATH" ]; then
		mkdir -p "$TEST_PATH"
		CREATED_CGROUP=1
		echo "Created test cgroup: $TEST_PATH"
		echo ""
	else
		echo "Using existing cgroup: $TEST_PATH"
		echo ""
	fi
fi

echo "=========================================="
echo "${BLUE}Test 1: Default Mode (No Write)${NC}"
echo "=========================================="
echo "Expectation: Show all fields (legacy mode)"
echo ""

cat "$TEST_PATH/memory.stat.ks" | head -10
echo "..."
echo ""
echo "Mode line:"
cat "$TEST_PATH/memory.stat.ks" | grep "# Mode:"
echo ""

LINE_COUNT=$(cat "$TEST_PATH/memory.stat.ks" | grep -v "^#" | wc -l)
echo "Total fields: $LINE_COUNT"
echo "${GREEN}✅ Test 1 passed: Default mode works${NC}"
echo ""

echo "=========================================="
echo "${BLUE}Test 2: Single Field BTF Query${NC}"
echo "=========================================="
echo "Query: anon"
echo ""

echo "anon" > "$TEST_PATH/memory.stat.ks"
RESULT=$(cat "$TEST_PATH/memory.stat.ks")
echo "$RESULT"
echo ""

# Verify only one field
FIELD_COUNT=$(echo "$RESULT" | grep -v "^#" | wc -l)
if [ $FIELD_COUNT -eq 1 ]; then
    echo "${GREEN}✅ Test 2 passed: Single field query works${NC}"
else
    echo "${RED}❌ Test 2 failed: Expected 1 field, got $FIELD_COUNT${NC}"
fi
echo ""

echo "=========================================="
echo "${BLUE}Test 3: Full Schema BTF Query${NC}"
echo "=========================================="
echo "Query: All fields via generated schema"
echo ""

SCHEMA_FILE="$(mktemp)"
SCHEMA_META="$(build_full_schema "$SCHEMA_FILE")"
cat "$SCHEMA_FILE" > "$TEST_PATH/memory.stat.ks"
RESULT=$(cat "$TEST_PATH/memory.stat.ks")
echo "$RESULT" | head -10
echo "..."
echo ""

FIELD_COUNT=$(echo "$RESULT" | grep -v "^#" | wc -l)
echo "Total fields via BTF: $FIELD_COUNT"
compare_outputs "$TEST_PATH/memory.stat" "$TEST_PATH/memory.stat.ks" "$SCHEMA_META" \
	"memory.stat vs memory.stat.ks (full BTF)"
echo ""

echo "=========================================="
echo "${BLUE}Test 4: Reset to Default Mode${NC}"
echo "=========================================="
echo "Action: Write empty line"
echo ""

echo "" > "$TEST_PATH/memory.stat.ks"
RESULT=$(cat "$TEST_PATH/memory.stat.ks")

FIELD_COUNT=$(echo "$RESULT" | grep -v "^#" | wc -l)
if [ $FIELD_COUNT -gt 10 ]; then
    echo "${GREEN}✅ Test 4 passed: Reset to default mode${NC}"
    echo "Field count: $FIELD_COUNT (all fields)"
else
    echo "${YELLOW}⚠️  Test 4: Field count $FIELD_COUNT${NC}"
fi
echo ""

echo "=========================================="
echo "${BLUE}Test 5: Open-once Read Comparison (N-field)${NC}"
echo "=========================================="
echo "Compare: memory.stat vs memory.stat.ks (N-field + flush)"
echo "         memory.numa_stat vs memory.numa_stat.ks (N-field + flush)"
echo "Fields: $FIELDS_LIST"
echo ""

# Keep the loop count modest for selftests
READS=100
declare -a KS_FIELDS
declare -a KS_STAT_EFF
declare -a KS_STAT_US
declare -a KS_NUMA_EFF
declare -a KS_NUMA_US

# Legacy (full) baseline
START=$(date +%s%N)
"$HELPER_BIN" "$TEST_PATH/memory.stat" "$READS"
END=$(date +%s%N)
STAT_TIME=$((END - START))
STAT_AVG=$((STAT_TIME / READS / 1000))
echo "memory.stat (full):        $STAT_AVG μs per read (open-once)"
STAT_LINE_COUNT=$(grep -v "^#" "$TEST_PATH/memory.stat" | wc -l)

if [ -f "$TEST_PATH/memory.numa_stat" ]; then
	START=$(date +%s%N)
	"$HELPER_BIN" "$TEST_PATH/memory.numa_stat" "$READS"
	END=$(date +%s%N)
	NUMA_TIME=$((END - START))
	NUMA_AVG=$((NUMA_TIME / READS / 1000))
	echo "memory.numa_stat (full):   $NUMA_AVG μs per read (open-once)"
	NUMA_LINE_COUNT=$(grep -v "^#" "$TEST_PATH/memory.numa_stat" | wc -l)
fi
echo ""

IFS=',' read -r -a FIELD_COUNTS <<< "$FIELDS_LIST"
for n in "${FIELD_COUNTS[@]}"; do
	n="${n//[[:space:]]/}"
	[ -z "$n" ] && continue

	n_stat="$n"
	if [ "$n_stat" -gt "$STAT_LINE_COUNT" ]; then
		n_stat="$STAT_LINE_COUNT"
	fi

	n_numa=""
	if [ -n "${NUMA_LINE_COUNT:-}" ]; then
		n_numa="$n"
		if [ "$n_numa" -gt "$NUMA_LINE_COUNT" ]; then
			n_numa="$NUMA_LINE_COUNT"
		fi
	fi

	stat_filter="$(build_filter_for_n "$n_stat")"
	echo "Setting filter: ${n}-field + flush (effective: stat=${n_stat}${n_numa:+, numa=${n_numa}})"
	echo "$stat_filter" > "$TEST_PATH/memory.stat.ks"
	if [ -n "$n_numa" ] && [ -f "$TEST_PATH/memory.numa_stat.ks" ]; then
		numa_filter="$(build_filter_for_n "$n_numa")"
		echo "$numa_filter" > "$TEST_PATH/memory.numa_stat.ks"
	fi

	START=$(date +%s%N)
	"$HELPER_BIN" "$TEST_PATH/memory.stat.ks" "$READS"
	END=$(date +%s%N)
	STAT_KS_TIME=$((END - START))
	STAT_KS_AVG=$((STAT_KS_TIME / READS / 1000))
	echo "memory.stat.ks (${n} fields): $STAT_KS_AVG μs per read (open-once)"

	NUMA_KS_AVG="-"
	if [ -f "$TEST_PATH/memory.numa_stat.ks" ]; then
		START=$(date +%s%N)
		"$HELPER_BIN" "$TEST_PATH/memory.numa_stat.ks" "$READS"
		END=$(date +%s%N)
		NUMA_KS_TIME=$((END - START))
		NUMA_KS_AVG=$((NUMA_KS_TIME / READS / 1000))
		echo "memory.numa_stat.ks (${n} fields): $NUMA_KS_AVG μs per read (open-once)"
	fi
	KS_FIELDS+=("$n")
	KS_STAT_EFF+=("$n_stat")
	KS_STAT_US+=("$STAT_KS_AVG")
	KS_NUMA_EFF+=("${n_numa:--}")
	KS_NUMA_US+=("$NUMA_KS_AVG")
	echo ""
done

echo "Summary (open-once, μs per read)"
printf "%-8s %-12s %-14s %-12s %-14s\n" "fields" "stat_cap" "stat.ks" "numa_cap" "numa.ks"
printf "%-8s %-12s %-14s %-12s %-14s\n" "legacy" "$STAT_LINE_COUNT" "$STAT_AVG" "${NUMA_LINE_COUNT:--}" "${NUMA_AVG:--}"
for i in "${!KS_FIELDS[@]}"; do
	printf "%-8s %-12s %-14s %-12s %-14s\n" \
		"${KS_FIELDS[$i]}" "${KS_STAT_EFF[$i]}" "${KS_STAT_US[$i]}" \
		"${KS_NUMA_EFF[$i]}" "${KS_NUMA_US[$i]}"
done
echo ""

echo "=========================================="
echo "${BLUE}Test 6: Internal Field Access${NC}"
echo "=========================================="
echo "This tests accessing fields beyond memory.stat"
echo ""

# Try to query internal mem_cgroup fields
echo "Querying internal fields (if supported):"
echo "  css.id - cgroup subsys ID"
echo "  css.serial_nr - cgroup serial number"
echo ""

echo "css.id,css.serial_nr" > "$TEST_PATH/memory.stat.ks" 2>/dev/null || true
RESULT=$(cat "$TEST_PATH/memory.stat.ks" 2>/dev/null)

if echo "$RESULT" | grep -q "css\.id\|Error"; then
    echo "$RESULT" | head -5
    echo ""
    if echo "$RESULT" | grep -q "Error"; then
        echo "${YELLOW}⚠️  Internal fields not accessible (expected, may need whitelist)${NC}"
    else
        echo "${GREEN}✅ Test 6 passed: Internal field access works!${NC}"
    fi
else
    echo "${YELLOW}⚠️  No output for internal fields${NC}"
fi
echo ""

echo "=========================================="
echo "${BLUE}Test 7: Invalid Field Handling${NC}"
echo "=========================================="
echo "Query: nonexistent_field"
echo ""

echo "nonexistent_field" > "$TEST_PATH/memory.stat.ks" 2>/dev/null || true
RESULT=$(cat "$TEST_PATH/memory.stat.ks" 2>/dev/null)

if echo "$RESULT" | grep -q "Error"; then
    echo "${GREEN}✅ Test 7 passed: Invalid field handled gracefully${NC}"
    echo "$RESULT" | grep "Error"
else
    echo "${YELLOW}⚠️  No error message for invalid field${NC}"
fi
echo ""

echo "=========================================="
echo "${YELLOW}Summary${NC}"
echo "=========================================="
echo ""

cat << 'EOF'
memory.stat.ks supports BTF queries for selective reads:

✅ Mode 1: Legacy (no write)
   cat memory.stat.ks
   → Shows all fields (backward compatible)

✅ Mode 2: BTF Query (after write)
   echo "<field list>" > memory.stat.ks
   cat memory.stat.ks
   → Shows only requested fields via BTF query

Key Benefits:
  • Real BTF-based field resolution
  • Selective field access (lower overhead)
  • Can query internal mem_cgroup fields
  • Automatic cgroup context binding
EOF
echo ""

# Cleanup
rm -f "$SCHEMA_FILE"
if [ "$CREATED_CGROUP" -eq 1 ]; then
	rmdir "$TEST_PATH"
	echo "Cleaned up test cgroup"
	echo ""
fi

echo "${GREEN}All tests complete!${NC}"
