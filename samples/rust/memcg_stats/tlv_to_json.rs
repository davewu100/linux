// SPDX-License-Identifier: GPL-2.0
//
// Example program demonstrating TLV to JSON conversion using serde
// This shows how userspace applications can parse kernel TLV data and serialize to JSON

use std::fs;
use std::io::Read;

mod tlv;

#[derive(Debug, serde::Serialize, serde::Deserialize)]
struct MemoryStats {
    anon: u64,
    file: u64,
    kernel: u64,
    kernel_stack: u64,
    pagetables: u64,
    sec_pagetables: u64,
    percpu: u64,
    sock: u64,
    vmalloc: u64,
    shmem: u64,
    zswap: u64,
    zswapped: u64,
    file_mapped: u64,
    file_dirty: u64,
    file_writeback: u64,
    swapcached: u64,
    anon_thp: u64,
    file_thp: u64,
    shmem_thp: u64,
    inactive_anon: u64,
    active_anon: u64,
    inactive_file: u64,
    active_file: u64,
    unevictable: u64,
    slab_reclaimable: u64,
    slab_unreclaimable: u64,
    hugetlb: u64,
    slab: u64,
    workingset_refault_anon: u64,
    workingset_refault_file: u64,
    workingset_activate_anon: u64,
    workingset_activate_file: u64,
    workingset_restore_anon: u64,
    workingset_restore_file: u64,
    workingset_nodereclaim: u64,
    pgscan_kswapd: u64,
    pgscan_direct: u64,
    pgscan_khugepaged: u64,
    pgscan_proactive: u64,
    pgsteal_kswapd: u64,
    pgsteal_direct: u64,
    pgsteal_khugepaged: u64,
    pgsteal_proactive: u64,
    pgfault: u64,
    pgmajfault: u64,
    pgrefill: u64,
    pgactivate: u64,
    pgdeactivate: u64,
    pglazyfree: u64,
    pglazyfreed: u64,
    swpin_zero: u64,
    swpout_zero: u64,
    zswpin: u64,
    zswpout: u64,
    zswpwb: u64,
    thp_fault_alloc: u64,
    thp_collapse_alloc: u64,
    thp_swout: u64,
    thp_swout_fallback: u64,
    numa_page_migrate: u64,
    numa_pte_updates: u64,
    numa_hint_faults: u64,
}

impl Default for MemoryStats {
    fn default() -> Self {
        Self {
            anon: 0, file: 0, kernel: 0, kernel_stack: 0, pagetables: 0,
            sec_pagetables: 0, percpu: 0, sock: 0, vmalloc: 0, shmem: 0,
            zswap: 0, zswapped: 0, file_mapped: 0, file_dirty: 0, file_writeback: 0,
            swapcached: 0, anon_thp: 0, file_thp: 0, shmem_thp: 0,
            inactive_anon: 0, active_anon: 0, inactive_file: 0, active_file: 0,
            unevictable: 0, slab_reclaimable: 0, slab_unreclaimable: 0, hugetlb: 0,
            slab: 0,
            workingset_refault_anon: 0, workingset_refault_file: 0,
            workingset_activate_anon: 0, workingset_activate_file: 0,
            workingset_restore_anon: 0, workingset_restore_file: 0,
            workingset_nodereclaim: 0,
            pgscan_kswapd: 0, pgscan_direct: 0, pgscan_khugepaged: 0, pgscan_proactive: 0,
            pgsteal_kswapd: 0, pgsteal_direct: 0, pgsteal_khugepaged: 0, pgsteal_proactive: 0,
            pgfault: 0, pgmajfault: 0, pgrefill: 0, pgactivate: 0, pgdeactivate: 0,
            pglazyfree: 0, pglazyfreed: 0,
            swpin_zero: 0, swpout_zero: 0, zswpin: 0, zswpout: 0, zswpwb: 0,
            thp_fault_alloc: 0, thp_collapse_alloc: 0, thp_swout: 0, thp_swout_fallback: 0,
            numa_page_migrate: 0, numa_pte_updates: 0, numa_hint_faults: 0,
        }
    }
}

// TLV type mappings (matching kernel definitions)
// Note: We use numeric literals in match statements for clarity

// TLV format from kernel: Type(2) + Length(2) + Value(8) = 12 bytes total
// No need for separate structs since we parse manually

fn parse_tlv_to_memory_stats(data: &[u8]) -> Result<MemoryStats, Box<dyn std::error::Error>> {
    let mut stats = MemoryStats::default();
    let mut pos = 0;

    const TLV_ENTRY_SIZE: usize = 12; // Type(2) + Length(2) + Value(8)

    // Skip container header if present (4 bytes: Type + Length)
    if data.len() >= 4 {
        let container_type = u16::from_be_bytes([data[0], data[1]]);
        if container_type == tlv::TLV_TYPE_MEMORY_STATS_CONTAINER ||
           container_type == tlv::TLV_TYPE_NUMA_STATS_CONTAINER {
            pos = 4; // Skip container header
        }
    }

    while pos + TLV_ENTRY_SIZE <= data.len() {
        // Parse big-endian values manually
        let tlv_type = u16::from_be_bytes([data[pos], data[pos + 1]]);
        let _length = u16::from_be_bytes([data[pos + 2], data[pos + 3]]);
        let value = u64::from_be_bytes([
            data[pos + 4], data[pos + 5], data[pos + 6], data[pos + 7],
            data[pos + 8], data[pos + 9], data[pos + 10], data[pos + 11]
        ]);

        // Map TLV type to memory stats field
        match tlv_type {
            1 => stats.anon = value,
            2 => stats.file = value,
            3 => stats.kernel = value,
            4 => stats.kernel_stack = value,
            5 => stats.pagetables = value,
            6 => stats.sec_pagetables = value,
            7 => stats.percpu = value,
            8 => stats.sock = value,
            9 => stats.vmalloc = value,
            10 => stats.shmem = value,
            11 => stats.zswap = value,
            12 => stats.zswapped = value,
            13 => stats.file_mapped = value,
            14 => stats.file_dirty = value,
            15 => stats.file_writeback = value,
            16 => stats.swapcached = value,
            17 => stats.anon_thp = value,
            18 => stats.file_thp = value,
            19 => stats.shmem_thp = value,
            20 => stats.inactive_anon = value,
            21 => stats.active_anon = value,
            22 => stats.inactive_file = value,
            23 => stats.active_file = value,
            24 => stats.unevictable = value,
            25 => stats.slab_reclaimable = value,
            26 => stats.slab_unreclaimable = value,
            27 => stats.hugetlb = value,
            28 => stats.slab = value, // combined slab
            // VM events (200+)
            200 => stats.pgscan_kswapd = value,
            201 => stats.pgscan_direct = value,
            202 => stats.pgscan_khugepaged = value,
            203 => stats.pgscan_proactive = value,
            204 => stats.pgsteal_kswapd = value,
            205 => stats.pgsteal_direct = value,
            206 => stats.pgsteal_khugepaged = value,
            207 => stats.pgsteal_proactive = value,
            208 => stats.pgfault = value,
            209 => stats.pgmajfault = value,
            210 => stats.pgrefill = value,
            211 => stats.pgactivate = value,
            212 => stats.pgdeactivate = value,
            213 => stats.pglazyfree = value,
            214 => stats.pglazyfreed = value,
            // Additional events...
            _ => {
                // Unknown type, skip
            }
        }

        pos += TLV_ENTRY_SIZE;
    }

    Ok(stats)
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = std::env::args().collect();

    if args.len() != 2 {
        eprintln!("Usage: {} <tlv_file>", args[0]);
        eprintln!("Example: {} /sys/fs/cgroup/memory/memory.stat_bin", args[0]);
        std::process::exit(1);
    }

    let filename = &args[1];

    // Read TLV binary data
    let mut file = fs::File::open(filename)?;
    let mut data = Vec::new();
    file.read_to_end(&mut data)?;

    // Parse TLV to structured data
    let stats = parse_tlv_to_memory_stats(&data)?;

    // Serialize to JSON using serde
    let json = serde_json::to_string_pretty(&stats)?;
    println!("{}", json);

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_parse_single_entry() {
        // TLV entry: type=1 (anon), length=8, value=12345
        let data = [
            0x00, 0x01, // type = 1 (anon)
            0x00, 0x08, // length = 8
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x39, // value = 12345
        ];

        let stats = parse_tlv_to_memory_stats(&data).unwrap();
        assert_eq!(stats.anon, 12345);
        assert_eq!(stats.file, 0); // Other fields should be default
    }

    #[test]
    fn test_parse_multiple_entries() {
        let data = [
            // Entry 1: anon = 1000
            0x00, 0x01, 0x00, 0x08,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xE8,
            // Entry 2: file = 2000
            0x00, 0x02, 0x00, 0x08,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xD0,
        ];

        let stats = parse_tlv_to_memory_stats(&data).unwrap();
        assert_eq!(stats.anon, 1000);
        assert_eq!(stats.file, 2000);
    }
}
