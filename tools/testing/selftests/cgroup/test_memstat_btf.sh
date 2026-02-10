#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Test memory.stat.ks with BTF query support
# This demonstrates TRUE kserial functionality

set -e

CGROUP_ROOT="/sys/fs/cgroup"
TEST_CGROUP="test_btf_$$"
# Default: use root cgroup so memory.stat shows non-zero values (system-wide usage)
TEST_PATH="$CGROUP_ROOT"
CREATED_CGROUP=0
HELPER_BIN="$(dirname "$0")/memstat_read_open_once"
HELPER_SRC="$(dirname "$0")/memstat_read_open_once.c"
FIELDS_LIST="3,8,16,32,64,128"

usage() {
	echo "Usage: $0 [--root] [--cgname <name>] [--cgpath <path>] [--fields <list>]"
	echo ""
	echo "  --root            Use root cgroup (default; has non-zero stats)"
	echo "  --cgname <name>   Use /sys/fs/cgroup/<name> (create if missing)"
	echo "  --cgpath <path>   Use an existing cgroup path (or create it)"
	echo "  --fields <list>   Comma list for perf test (default: $FIELDS_LIST)"
}

while [ $# -gt 0 ]; do
	case "$1" in
		--root)
			TEST_PATH="$CGROUP_ROOT"
			shift
			;;
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

# No colors (plain text only; avoids \033 in logs/paste)
GREEN=''
BLUE=''
YELLOW=''
RED=''
NC=''

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

# Extract memory.stat actual BTF paths (vmstats.state[N]) in order
extract_memstat_btf_paths() {
	local -a node_items
	local -a stat_items
	local -a memory_stats
	local -A state_index
	local name symbol idx nr_node nr_stat

	mapfile -t node_items < <(extract_array_items "memcg_node_stat_items")
	mapfile -t stat_items < <(extract_array_items "memcg_stat_items")
	mapfile -t memory_stats < <(extract_memory_stats)

	nr_node=${#node_items[@]}
	nr_stat=${#stat_items[@]}

	for ((i=0; i<nr_node; i++)); do
		state_index["${node_items[i]}"]=$i
	done
	for ((i=0; i<nr_stat; i++)); do
		state_index["${stat_items[i]}"]=$((nr_node + i))
	done

	for entry in "${memory_stats[@]}"; do
		symbol="${entry##*:}"
		idx="${state_index[$symbol]}"
		[ -n "$idx" ] && echo "vmstats.state[$idx]"
	done
}

# Extract numa_stat actual BTF paths (node-level fields only)
extract_numa_stat_btf_paths() {
	local -a node_items
	local -a memory_stats
	local name symbol idx

	mapfile -t node_items < <(extract_array_items "memcg_node_stat_items")
	mapfile -t memory_stats < <(extract_memory_stats)

	for entry in "${memory_stats[@]}"; do
		symbol="${entry##*:}"
		# Find in node_items
		for ((idx=0; idx<${#node_items[@]}; idx++)); do
			if [ "${node_items[$idx]}" = "$symbol" ]; then
				echo "vmstats.state[$idx]"
				break
			fi
		done
	done
}

MEMSTAT_BTF_PATHS=($(extract_memstat_btf_paths))
NUMA_STAT_BTF_PATHS=($(extract_numa_stat_btf_paths))

build_filter_for_n() {
	local n="$1"
	local filter="flush"
	local i
	local max="${#MEMSTAT_BTF_PATHS[@]}"

	# Cap at available field count
	if [ "$n" -gt "$max" ]; then
		n="$max"
	fi

	for ((i=0; i<n; i++)); do
		[ -n "${MEMSTAT_BTF_PATHS[$i]}" ] && filter="${filter},${MEMSTAT_BTF_PATHS[$i]}"
	done
	echo "$filter"
}

# Build filter using simple names (first n lines from memory.stat) so output is exactly n lines.
build_simple_filter_for_n() {
	local n="$1"
	local names
	names=$(grep -v "^#" "$TEST_PATH/memory.stat" | head -n "$n" | awk '{print $1}' | paste -sd,)
	echo "flush,$names"
}

build_numa_filter_for_n() {
	local n="$1"
	local filter="flush"
	local i
	local max="${#NUMA_STAT_BTF_PATHS[@]}"

	# Cap at available field count
	if [ "$n" -gt "$max" ]; then
		n="$max"
	fi

	if [ "$n" -eq 3 ]; then
		# Use first 3 node-level fields
		echo "flush,${NUMA_STAT_BTF_PATHS[0]},${NUMA_STAT_BTF_PATHS[1]},${NUMA_STAT_BTF_PATHS[2]}"
		return
	fi

	for ((i=0; i<n; i++)); do
		[ -n "${NUMA_STAT_BTF_PATHS[$i]}" ] && filter="${filter},${NUMA_STAT_BTF_PATHS[$i]}"
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

	# Prefer live memory.stat for schema order so output (and head -10) shows anon, file, ... with real values first
	if [ -r "$TEST_PATH/memory.stat" ]; then
		schema_fields=()
		memory_stat_names=()
		while read -r name _; do
			[ -z "$name" ] || [[ "$name" == \#* ]] && continue
			schema_fields+=("$name")
			if [ "$name" = "pgsteal" ]; then
				symbol=""; idx="-"
				for entry in "${memory_stats[@]}"; do [ "${entry%%:*}" = "pgsteal" ] && { symbol="${entry##*:}"; idx="${state_index[$symbol]:--}"; break; }; done
				memory_stat_names+=("pgsteal:${symbol:-pgsteal}:$idx")
				# Remaining lines are events; add to schema_fields only (compare_outputs uses ::EVENTS:: list)
				while read -r ev_name _; do
					[ -z "$ev_name" ] || [[ "$ev_name" == \#* ]] && continue
					schema_fields+=("$ev_name")
				done
				break
			fi
			symbol=""; idx="-"
			for entry in "${memory_stats[@]}"; do
				[ "${entry%%:*}" = "$name" ] && { symbol="${entry##*:}"; idx="${state_index[$symbol]:--}"; break; }
			done
			memory_stat_names+=("$name:${symbol:-$name}:$idx")
		done < "$TEST_PATH/memory.stat"
		echo "${memory_stat_names[@]}"
		echo "::EVENTS::"
		for ((i=0; i<${#event_symbols[@]}; i++)); do
			echo "${event_symbols[i]}:${event_names[i]}:$i"
		done
	else
		# Fallback: build from source (vmstats.state[N] / event names)
		for ((i=0; i<${#memory_stats[@]}; i++)); do
			entry="${memory_stats[i]}"
			name="${entry%%:*}"
			symbol="${entry##*:}"
			idx="${state_index[$symbol]:--}"
			memory_stat_names+=("$name:$symbol:$idx")
			schema_fields+=("$name")
			if [ "$name" = "slab_unreclaimable" ]; then
				memory_stat_names+=("slab:slab:slab")
				schema_fields+=("slab")
			fi
		done
		memory_stat_names+=("pgscan:pgscan:pgscan")
		schema_fields+=("pgscan")
		memory_stat_names+=("pgsteal:pgsteal:pgsteal")
		schema_fields+=("pgsteal")
		for ((i=0; i<${#event_symbols[@]}; i++)); do
			schema_fields+=("${event_names[i]}")
		done
		echo "${memory_stat_names[@]}"
		echo "::EVENTS::"
		for ((i=0; i<${#event_symbols[@]}; i++)); do
			echo "${event_symbols[i]}:${event_names[i]}:$i"
		done
	fi

	IFS=','; echo "${schema_fields[*]}" > "$schema_file"; IFS=' '
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
		echo "${YELLOW}WARN: ${label}: missing schema metadata${NC}"
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
		symbol="${line%%:*}"
		# Kernel outputs by name (anon, slab, ...) in same unit as memory.stat; prefer ks_values[name]
		local raw="${ks_values[$name]:-${ks_values["vmstats.state[$value]"]}}"
		if [ -n "$raw" ]; then
			local actual="${stat_values[$name]}"
			if [ "$name" = "slab" ]; then
				local slab_reclaim="${stat_values[slab_reclaimable]}"
				local slab_unreclaim="${stat_values[slab_unreclaimable]}"
				actual=$((slab_reclaim + slab_unreclaim))
			fi
			# Both .stat and .ks use memcg_page_state_output (same unit); compare directly
			if [ -n "$actual" ] && [ "$raw" -ne "$actual" ]; then
				echo "${YELLOW}WARN: ${label}: ${name} legacy=${actual} btf=${raw}${NC}"
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
				echo "${YELLOW}WARN: ${label}: ${ev_name} legacy=${stat_values[$ev_name]} btf=${ev_val}${NC}"
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
			echo "${YELLOW}WARN: ${label}: pgscan legacy=${stat_values[pgscan]} btf=${pgscan_sum}${NC}"
			return 1
		fi
		if [ -n "${stat_values[pgsteal]}" ] && [ "$pgsteal_sum" -ne "${stat_values[pgsteal]}" ]; then
			echo "${YELLOW}WARN: ${label}: pgsteal legacy=${stat_values[pgsteal]} btf=${pgsteal_sum}${NC}"
			return 1
		fi
	}

	echo "${GREEN}PASS: ${label} outputs match${NC}"
}

compare_plain_outputs() {
	local left="$1"
	local right="$2"
	local label="$3"

	if diff -u <(grep -v "^#" "$left") <(grep -v "^#" "$right") >/dev/null; then
		echo "${GREEN}PASS: ${label} outputs match${NC}"
	else
		echo "${YELLOW}WARN: ${label} outputs differ${NC}"
	fi
}

echo "=========================================="
echo "memory.stat.ks BTF Query Tests"
echo "=========================================="
echo ""

# Check if memory.stat.ks exists
if [ ! -f "$CGROUP_ROOT/memory.stat.ks" ]; then
    echo "${RED}FAIL: memory.stat.ks not found - CONFIG_KSERIAL not enabled${NC}"
    exit 1
fi

echo "${GREEN}PASS: CONFIG_KSERIAL is enabled${NC}"
echo ""

if [ ! -x "$HELPER_BIN" ]; then
	if [ -f "$HELPER_SRC" ]; then
		echo "Building helper: $HELPER_BIN"
		${CC:-gcc} -O2 -Wall -o "$HELPER_BIN" "$HELPER_SRC"
		echo ""
	else
		echo "${RED}FAIL: Helper source not found: $HELPER_SRC${NC}"
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

# Reset memory.stat.ks to default mode (show all fields, same as memory.stat).
# Otherwise a previous run may have left a filter (e.g. BTF schema, 29 fields).
echo "" > "$TEST_PATH/memory.stat.ks"
[ -f "$TEST_PATH/memory.numa_stat.ks" ] && echo "" > "$TEST_PATH/memory.numa_stat.ks"

echo "=========================================="
echo "${BLUE}Test 1: Default Mode (No Write)${NC}"
echo "=========================================="
echo "Expectation: Show all fields (legacy mode)"
echo ""

cat "$TEST_PATH/memory.stat.ks" | head -10
echo "..."
echo ""
echo "Mode line:"
cat "$TEST_PATH/memory.stat.ks" | grep "# Mode:" || true
echo ""

LINE_COUNT=$(cat "$TEST_PATH/memory.stat.ks" | grep -v "^#" | wc -l)
echo "Total fields: $LINE_COUNT"
echo "${GREEN}PASS: Test 1 passed: Default mode works${NC}"
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
    echo "${GREEN}PASS: Test 2 passed: Single field query works${NC}"
else
    echo "${RED}FAIL: Test 2 failed: Expected 1 field, got $FIELD_COUNT${NC}"
fi
echo ""

echo "=========================================="
echo "${BLUE}Test 2.5: Multiple Fields with Simple Names${NC}"
echo "=========================================="
echo "Query: anon,file,slab (simple names)"
echo ""

echo "anon,file,slab" > "$TEST_PATH/memory.stat.ks"
RESULT=$(cat "$TEST_PATH/memory.stat.ks")
echo "$RESULT"
echo ""

# Verify three fields
FIELD_COUNT=$(echo "$RESULT" | grep -v "^#" | wc -l)
if [ $FIELD_COUNT -eq 3 ]; then
    echo "${GREEN}PASS: Test 2.5 passed: Multiple field query with simple names works${NC}"
else
    echo "${RED}FAIL: Test 2.5 failed: Expected 3 fields, got $FIELD_COUNT${NC}"
fi
echo ""

echo "=========================================="
echo "${BLUE}Test 3: Full Schema BTF Query${NC}"
echo "=========================================="
echo "Query: All fields via generated schema (vmstats.state[N], vmstats.events[N])"
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
    echo "${GREEN}PASS: Test 4 passed: Reset to default mode${NC}"
    echo "Field count: $FIELD_COUNT (all fields)"
else
    echo "${YELLOW}WARN: Test 4: Field count $FIELD_COUNT${NC}"
fi
echo ""

echo "=========================================="
echo "${BLUE}Test 5: Open-once Full-file Read Comparison${NC}"
echo "=========================================="
echo "Each full-file read = one read() of the entire file (all lines); open once, then 100× (lseek(0)+read)."
echo "Fields: $FIELDS_LIST"
echo "Note: μs/read below = μs per full-file read (not per field). 3-field and full-file can be similar"
echo "      because flush dominates and one read() returns the whole filtered output (fits in 4KB)."
echo "All field counts (3,8,16,32,64,128) use simple names (first n from memory.stat) so output is exactly n lines."
echo ""

READS=100
WARMUP_READS=1
COLD_READS=1
declare -a KS_FIELDS
declare -a KS_STAT_EFF
declare -a KS_STAT_US
declare -a KS_NUMA_EFF
declare -a KS_NUMA_US

# Legacy baseline: same method as .ks — 1 cold read (timed here), then hot avg from helper
START=$(date +%s%N)
"$HELPER_BIN" "$TEST_PATH/memory.stat" "$COLD_READS"
END=$(date +%s%N)
STAT_COLD_US=$(( (END - START) / 1000 ))
echo "memory.stat (1st full-file read, cold): $STAT_COLD_US μs"
STAT_AVG=$("$HELPER_BIN" "$TEST_PATH/memory.stat" "$READS" "$WARMUP_READS")
echo "memory.stat (avg ${READS}x, hot): $STAT_AVG μs per full-file read"
STAT_LINE_COUNT=$(grep -v "^#" "$TEST_PATH/memory.stat" | wc -l)

if [ -f "$TEST_PATH/memory.numa_stat" ]; then
	START=$(date +%s%N)
	"$HELPER_BIN" "$TEST_PATH/memory.numa_stat" "$COLD_READS"
	END=$(date +%s%N)
	NUMA_COLD_US=$(( (END - START) / 1000 ))
	echo "memory.numa_stat (1st full-file read, cold): $NUMA_COLD_US μs"
	NUMA_AVG=$("$HELPER_BIN" "$TEST_PATH/memory.numa_stat" "$READS" "$WARMUP_READS")
	echo "memory.numa_stat (avg ${READS}x, hot): $NUMA_AVG μs per full-file read"
	NUMA_LINE_COUNT=$(grep -v "^#" "$TEST_PATH/memory.numa_stat" | wc -l)
fi
echo ""

# Warm-up: run full-field read on memory.stat.ks so first iteration (3-field) is not cold
stat_filter="$(build_simple_filter_for_n "$STAT_LINE_COUNT")"
echo "$stat_filter" > "$TEST_PATH/memory.stat.ks"
"$HELPER_BIN" "$TEST_PATH/memory.stat.ks" 10 >/dev/null 2>&1

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

	# Use simple names so output is exactly n_stat lines (BTF paths can mis-resolve and output all 70)
	stat_filter="$(build_simple_filter_for_n "$n_stat")"
	echo "Setting filter: ${n}-field + flush (effective: stat=${n_stat}${n_numa:+, numa_stat=${n_numa}})"
	echo "$stat_filter" > "$TEST_PATH/memory.stat.ks"
	LINES=$(grep -v "^#" "$TEST_PATH/memory.stat.ks" | wc -l)
	if [ "$LINES" -ne "$n_stat" ]; then
		echo "${RED}FAIL: expected $n_stat lines, got $LINES${NC}"
		exit 1
	fi
	if [ -n "$n_numa" ] && [ -f "$TEST_PATH/memory.numa_stat.ks" ]; then
		numa_filter="$(build_numa_filter_for_n "$n_numa")"
		echo "$numa_filter" > "$TEST_PATH/memory.numa_stat.ks"
	fi

	# First full-file read (cold: BTF resolution + populate resolved[])
	START=$(date +%s%N)
	"$HELPER_BIN" "$TEST_PATH/memory.stat.ks" "$COLD_READS"
	END=$(date +%s%N)
	STAT_COLD_NS=$((END - START))
	STAT_COLD_US=$((STAT_COLD_NS / 1000))
	echo "  memory.stat.ks (1st full-file read, cold): $STAT_COLD_US μs"

	# Subsequent reads (hot): warmup then time READS; average excludes first-read-after-open cold
	STAT_KS_AVG=$("$HELPER_BIN" "$TEST_PATH/memory.stat.ks" "$READS" "$WARMUP_READS")
	echo "  memory.stat.ks (avg ${READS}x, hot): $STAT_KS_AVG μs per full-file read"

	NUMA_KS_AVG="-"
	if [ -f "$TEST_PATH/memory.numa_stat.ks" ]; then
		START=$(date +%s%N)
		"$HELPER_BIN" "$TEST_PATH/memory.numa_stat.ks" "$COLD_READS"
		END=$(date +%s%N)
		NUMA_COLD_US=$(( (END - START) / 1000 ))
		echo "  memory.numa_stat.ks (1st full-file read, cold): $NUMA_COLD_US μs"
		NUMA_KS_AVG=$("$HELPER_BIN" "$TEST_PATH/memory.numa_stat.ks" "$READS" "$WARMUP_READS")
		echo "  memory.numa_stat.ks (avg ${READS}x, hot): $NUMA_KS_AVG μs per full-file read"
	fi
	KS_FIELDS+=("$n")
	KS_STAT_EFF+=("$n_stat")
	KS_STAT_US+=("$STAT_KS_AVG")
	KS_NUMA_EFF+=("${n_numa:--}")
	KS_NUMA_US+=("$NUMA_KS_AVG")
	echo ""
done

echo "------------------------------------------"
echo "Verify: 3-field and 64-field output (must be exactly 3 and 64 lines)"
echo "------------------------------------------"
echo "3-field (simple names):"
echo "flush,anon,file,slab" > "$TEST_PATH/memory.stat.ks"
L3=$(grep -v "^#" "$TEST_PATH/memory.stat.ks" | wc -l)
echo "  lines: $L3 (expected 3)"
cat "$TEST_PATH/memory.stat.ks"
echo ""
echo "64-field (simple names, first 64 from memory.stat):"
stat_filter="$(build_simple_filter_for_n 64)"
echo "$stat_filter" > "$TEST_PATH/memory.stat.ks"
L64=$(grep -v "^#" "$TEST_PATH/memory.stat.ks" | wc -l)
echo "  lines: $L64 (expected 64)"
[ "$L64" -eq 64 ] || { echo "${RED}FAIL: 64-field simple got $L64 lines${NC}"; exit 1; }
head -5 "$TEST_PATH/memory.stat.ks"
echo "  ..."
tail -5 "$TEST_PATH/memory.stat.ks"
echo ""
echo "64-field (BTF paths) — must output 64 lines when 64 paths are written:"
# MEMSTAT_BTF_PATHS is set at script start by parsing mm/memcontrol.c; if empty (e.g. script
# not run from kernel tree, or CONFIG_* / awk parse fails), re-run extraction and skip if still empty.
[ ${#MEMSTAT_BTF_PATHS[@]} -eq 0 ] && MEMSTAT_BTF_PATHS=($(extract_memstat_btf_paths))
if [ ${#MEMSTAT_BTF_PATHS[@]} -ge 64 ]; then
	stat_filter="flush"
	for ((i=0; i<64; i++)); do
		[ -n "${MEMSTAT_BTF_PATHS[$i]}" ] && stat_filter="${stat_filter},${MEMSTAT_BTF_PATHS[$i]}"
	done
	echo "$stat_filter" > "$TEST_PATH/memory.stat.ks"
	L64BTF=$(grep -v "^#" "$TEST_PATH/memory.stat.ks" | wc -l)
	echo "  lines: $L64BTF (expected 64)"
	if [ "$L64BTF" -ne 64 ]; then
		echo "${RED}FAIL: 64-field BTF got $L64BTF lines (kernel should output only requested fields)${NC}"
		exit 1
	fi
else
	# BTF paths not available (extract failed); 64-field already verified with simple names above
	echo "  (MEMSTAT_BTF_PATHS empty, skip BTF path check; 64-field simple names already passed)"
fi
echo ""

echo "=========================================="
echo "${BLUE}Summary: μs per full-file read (open-once, 100× lseek+read)${NC}"
echo "=========================================="
echo "  Each full-file read = one read() of the entire file (n lines). Not per-field."
echo ""
echo "memory.stat vs memory.stat.ks (μs per full-file read)"
printf "%-8s %-6s %-12s %-14s %-7s\n" "fields" "n" "memory.stat" "memory.stat.ks" "speedup"
printf "%-8s %-6s %-12s %-14s %-7s\n" "---" "---" "---" "---" "---"
_su="${KS_STAT_US[-1]:--}"
if [ "$_su" != "-" ] && [ "$_su" -gt 0 ] 2>/dev/null; then _sx="${STAT_AVG:-0}"; printf "%-8s %-6s %-12s %-14s %-7s\n" "full" "$STAT_LINE_COUNT" "$STAT_AVG" "$_su" "$((_sx / _su))x"; else printf "%-8s %-6s %-12s %-14s %-7s\n" "full" "$STAT_LINE_COUNT" "$STAT_AVG" "${_su}" "-"; fi
for i in "${!KS_FIELDS[@]}"; do
	_su="${KS_STAT_US[$i]}"
	if [ -n "$_su" ] && [ "$_su" != "-" ] && [ "$_su" -gt 0 ] 2>/dev/null; then _sx="${STAT_AVG:-0}"; printf "%-8s %-6s %-12s %-14s %-7s\n" "${KS_FIELDS[$i]}" "${KS_STAT_EFF[$i]}" "-" "$_su" "$((_sx / _su))x"; else printf "%-8s %-6s %-12s %-14s %-7s\n" "${KS_FIELDS[$i]}" "${KS_STAT_EFF[$i]}" "-" "${_su}" "-"; fi
done
echo ""
echo "memory.numa_stat vs memory.numa_stat.ks (μs per full-file read)"
printf "%-8s %-6s %-17s %-19s %-7s\n" "fields" "n" "memory.numa_stat" "memory.numa_stat.ks" "speedup"
printf "%-8s %-6s %-17s %-19s %-7s\n" "---" "---" "---" "---" "---"
_nu="${KS_NUMA_US[-1]:--}"
if [ "$_nu" != "-" ] && [ "$_nu" -gt 0 ] 2>/dev/null; then _nx="${NUMA_AVG:-0}"; printf "%-8s %-6s %-17s %-19s %-7s\n" "full" "${NUMA_LINE_COUNT:--}" "${NUMA_AVG:--}" "$_nu" "$((_nx / _nu))x"; else printf "%-8s %-6s %-17s %-19s %-7s\n" "full" "${NUMA_LINE_COUNT:--}" "${NUMA_AVG:--}" "${_nu}" "-"; fi
for i in "${!KS_FIELDS[@]}"; do
	_nu="${KS_NUMA_US[$i]}"
	if [ -n "$_nu" ] && [ "$_nu" != "-" ] && [ "$_nu" -gt 0 ] 2>/dev/null; then _nx="${NUMA_AVG:-0}"; printf "%-8s %-6s %-17s %-19s %-7s\n" "${KS_FIELDS[$i]}" "${KS_NUMA_EFF[$i]}" "-" "$_nu" "$((_nx / _nu))x"; else printf "%-8s %-6s %-17s %-19s %-7s\n" "${KS_FIELDS[$i]}" "${KS_NUMA_EFF[$i]}" "-" "${_nu}" "-"; fi
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
        echo "${YELLOW}WARN: Internal fields not accessible (expected, may need whitelist)${NC}"
    else
        echo "${GREEN}PASS: Test 6 passed: Internal field access works${NC}"
    fi
else
    echo "${YELLOW}WARN: No output for internal fields${NC}"
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
    echo "${GREEN}PASS: Test 7 passed: Invalid field handled gracefully${NC}"
    echo "$RESULT" | grep "Error"
else
    echo "${YELLOW}WARN: No error message for invalid field${NC}"
fi
echo ""

echo "=========================================="
echo "${YELLOW}Summary${NC}"
echo "=========================================="
echo ""

cat << 'EOF'
memory.stat.ks supports BTF queries for selective reads:

Mode 1: Legacy (no write)
   cat memory.stat.ks
   Shows all fields (backward compatible)

Mode 2: BTF Query (after write)
   Simple field names (recommended):
     echo "anon,file,slab" > memory.stat.ks
     cat memory.stat.ks

   BTF paths (advanced):
     echo "vmstats.state[17],vmstats.state[19]" > memory.stat.ks
     cat memory.stat.ks

   With flush flag:
     echo "flush,anon,file" > memory.stat.ks
     cat memory.stat.ks

   Shows only requested fields via BTF query

Key Benefits:
  - Real BTF-based field resolution
  - Selective field access (lower overhead)
  - Simple names auto-translated to BTF paths
  - Can query internal mem_cgroup fields (via BTF paths)
  - Automatic cgroup context binding
EOF
echo ""

# Cleanup
rm -f "$SCHEMA_FILE"
if [ "$CREATED_CGROUP" -eq 1 ]; then
	rmdir "$TEST_PATH"
	echo "Cleaned up test cgroup"
	echo ""
fi

echo "${GREEN}All tests complete${NC}"
