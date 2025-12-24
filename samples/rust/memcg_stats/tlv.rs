// SPDX-License-Identifier: GPL-2.0
//
// TLV parsing for userspace serde JSON serialization
// This module provides functions to parse kernel TLV v1 format and convert to structured data

// TLV type constants (matching kernel definitions)
pub const TLV_TYPE_MEMORY_STATS_CONTAINER: u16 = 0xFFFE;
pub const TLV_TYPE_NUMA_STATS_CONTAINER: u16 = 0xFFFF;
