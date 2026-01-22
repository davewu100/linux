# Memory Cgroup Performance Testing

## Overview

This test suite measures the performance of reading cgroup memory statistics files
(`memory.stat` and `memory.numa_stat`), helping to evaluate the performance impact
of different memory cgroup implementations (atomic counter vs rstat).

## Files

- `func_profiler.h` - Lightweight profiling library header
- `func_profiler.c` - Profiling library implementation
- `test_memcg_perf.c` - Main test program
- `README_MEMCG_PERF.md` - This file

## Building

From the selftests/cgroup directory:

```bash
make test_memcg_perf
```

Or build all cgroup tests:

```bash
make
```

## Running Tests

### Basic usage:

```bash
sudo ./test_memcg_perf
```

### Running specific scenarios:

The test includes 7 different test scenarios:

1. **Sequential reads** - 1000 iterations of reading memory.stat
2. **NUMA stat reads** - 1000 iterations of reading memory.numa_stat
3. **Mixed workload** - Alternating between memory.stat and memory.numa_stat
4. **Granular operations** - Breaking down open, read, and close overhead
5. **FD reuse** - Comparing FD reuse vs repeated open/close
6. **Concurrent reads** - Multi-process concurrent reading
7. **Burst reads** - Burst pattern (10 bursts of 100 reads each)

### Output

The test provides detailed statistics for each operation:

```
===== Cgroup Performance Profiling Statistics =====
[ID:0] read_memory_stat_once          : Calls:   1000, Total:   12500.00 us, Avg:    12.50 us, Min:     8.20 us, Max:    45.30 us
[ID:1] read_memory_numa_stat_once     : Calls:   1000, Total:   18700.00 us, Avg:    18.70 us, Min:    12.40 us, Max:    67.80 us
...
===== End Profiling Statistics =====
```

A CSV file `memcg_perf_results.csv` is also generated for further analysis:

```csv
ID,Function,Calls,Total_us,Avg_us,Min_us,Max_us
0,read_memory_stat_once,1000,12500.00,12.50,8.20,45.30
1,read_memory_numa_stat_once,1000,18700.00,18.70,12.40,67.80
...
```

## Comparing Implementations

To compare atomic counter vs rstat performance:

### Method 1: Boot-time selection (recommended)

```bash
# Test with atomic counter
sudo reboot # boot with memcg_atomic_counter=1
sudo ./test_memcg_perf
cp memcg_perf_results.csv results_atomic.csv

# Test with rstat
sudo reboot # boot with memcg_rstat_counter=1
sudo ./test_memcg_perf
cp memcg_perf_results.csv results_rstat.csv

# Compare results
diff results_atomic.csv results_rstat.csv
```

### Method 2: Rebuild kernel

Build two kernels:
1. With `CONFIG_MEMCG_ATOMIC_COUNTER=y`
2. With `CONFIG_MEMCG_RSTAT_COUNTER=y`

Run the test on each and compare results.

## Understanding Results

### Key metrics:

- **Total time**: Total time spent in the function across all calls
- **Average time**: Mean execution time per call
- **Min time**: Fastest execution
- **Max time**: Slowest execution (often indicates cache effects or contention)

### What to look for:

1. **Average latency**: Lower is better
2. **Max latency**: High max indicates cache misses or lock contention
3. **Consistency**: Smaller difference between min and max indicates more predictable performance

### Example interpretation:

```
read_memory_stat_once: Avg: 12.50 us, Min: 8.20 us, Max: 45.30 us
```

- Average read takes 12.5 microseconds
- Best case (cache hit): 8.2 microseconds
- Worst case (cache miss or contention): 45.3 microseconds

## Advanced Usage

### Custom iterations:

Edit `TEST_ITERATIONS` in `test_memcg_perf.c`:

```c
#define TEST_ITERATIONS 10000  // Run 10,000 iterations instead of 1,000
```

### Profiling your own functions:

```c
#include "func_profiler.h"

void my_custom_test(void) {
    PROFILE_FUNC();  // Auto-profile this function
    
    // Your code here
}

int main() {
    func_profiler_init();
    PROFILE_ENABLE();
    
    for (int i = 0; i < 1000; i++)
        my_custom_test();
    
    PROFILE_PRINT_STATS();
    func_profiler_cleanup();
}
```

## Troubleshooting

### "Cannot access memory.stat"

Make sure:
1. You're running as root or have appropriate permissions
2. Cgroup v2 is mounted (check `/sys/fs/cgroup`)
3. Memory controller is enabled

### "NUMA stat not available"

This is normal on systems without NUMA or with only one node. The test will
skip NUMA-related tests automatically.

### Unexpected high latency

Check for:
1. System load (run `top` or `htop`)
2. Other processes accessing cgroup files
3. CPU frequency scaling (disable with `cpupower frequency-set -g performance`)

## Performance Tips

For more accurate measurements:

1. **Disable CPU frequency scaling**:
   ```bash
   sudo cpupower frequency-set -g performance
   ```

2. **Disable turbo boost**:
   ```bash
   echo 0 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo
   ```

3. **Run on isolated CPU**:
   ```bash
   taskset -c 1 ./test_memcg_perf
   ```

4. **Repeat tests multiple times**:
   ```bash
   for i in {1..10}; do
       sudo ./test_memcg_perf >> results_$i.txt
   done
   ```

## Integration with kselftest Framework

Run as part of the kselftest suite:

```bash
cd tools/testing/selftests
sudo make run_tests -C cgroup TARGETS=test_memcg_perf
```

## Contributing

When adding new test scenarios:

1. Add your test function with `PROFILE_FUNC()` or `PROFILE_FUNC_NAMED()`
2. Call it from `main()` with appropriate iterations
3. Document expected behavior and results

## References

- Cgroup documentation: `Documentation/admin-guide/cgroup-v2.rst`
- Memory cgroup: `Documentation/admin-guide/cgroup-v2.rst` (memory controller)
