// SPDX-License-Identifier: GPL-2.0
// func_profiler.c - Lightweight function profiling implementation

#include "func_profiler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

// Global state (simplified version without shared memory for selftests)
static profiler_state_t g_state = {0};

// CPU frequency detection for x86/x64 RDTSC conversion
static double get_cpu_frequency_ghz(void) {
    static double cached_frequency = 0.0;
    static bool frequency_detected = false;

    if (frequency_detected)
        return cached_frequency;

    FILE* cpuinfo = fopen("/proc/cpuinfo", "r");
    if (!cpuinfo) {
        cached_frequency = 2.0; // Default fallback
        frequency_detected = true;
        return cached_frequency;
    }

    char line[256];
    while (fgets(line, sizeof(line), cpuinfo)) {
        if (strstr(line, "cpu MHz")) {
            char* colon = strchr(line, ':');
            if (colon) {
                double freq_mhz = atof(colon + 1);
                cached_frequency = freq_mhz / 1000.0;
                frequency_detected = true;
                fclose(cpuinfo);
                return cached_frequency;
            }
        }
    }

    fclose(cpuinfo);
    cached_frequency = 2.0; // Default fallback
    frequency_detected = true;
    return cached_frequency;
}

static double get_cycles_to_microseconds(void) {
    static double conversion_factor = -1.0;

    if (conversion_factor < 0.0) {
        double cpu_freq_ghz = get_cpu_frequency_ghz();
        conversion_factor = 1.0 / (cpu_freq_ghz * 1000.0);
    }
    return conversion_factor;
}

void func_profiler_init(void) {
    memset(&g_state, 0, sizeof(g_state));
    g_state.enabled = false;
    
    for (int i = 0; i < MAX_PROFILED_FUNCTIONS; i++) {
        g_state.data[i].number = -1;
        g_state.data[i].call_count = 0;
        g_state.data[i].call_duration_ns = 0;
        g_state.data[i].min_duration_ns = UINT64_MAX;
        g_state.data[i].max_duration_ns = 0;
        memset(g_state.data[i].function_name, 0, MAX_FUNCTION_NAME_LEN);
    }
}

void func_profiler_cleanup(void) {
    // Nothing to clean up in simplified version
}

void func_profiler_reset(void) {
    for (int i = 0; i < MAX_PROFILED_FUNCTIONS; i++) {
        if (g_state.data[i].number != -1) {
            g_state.data[i].call_count = 0;
            g_state.data[i].call_duration_ns = 0;
            g_state.data[i].min_duration_ns = UINT64_MAX;
            g_state.data[i].max_duration_ns = 0;
        }
    }
}

void func_profiler_add_data(int number, uint64_t duration) {
    if (!g_state.enabled)
        return;

    // Find the function slot by ID
    int slot_index = -1;
    for (int i = 0; i < MAX_PROFILED_FUNCTIONS; i++) {
        if (g_state.data[i].number == number) {
            slot_index = i;
            break;
        }
    }

    if (slot_index == -1)
        return;

    profiling_data_t* data_slot = &g_state.data[slot_index];
    
    data_slot->call_count++;
    data_slot->call_duration_ns += duration;

    if (duration < data_slot->min_duration_ns)
        data_slot->min_duration_ns = duration;

    if (duration > data_slot->max_duration_ns)
        data_slot->max_duration_ns = duration;
}

bool func_profiler_register_function(int function_id, const char* function_name) {
    // Check if function is already registered
    for (int i = 0; i < MAX_PROFILED_FUNCTIONS; i++) {
        if (g_state.data[i].number == function_id)
            return true;
    }

    // Find an empty slot
    for (int i = 0; i < MAX_PROFILED_FUNCTIONS; i++) {
        if (g_state.data[i].number == -1) {
            g_state.data[i].number = function_id;
            g_state.data[i].call_count = 0;
            g_state.data[i].call_duration_ns = 0;
            g_state.data[i].min_duration_ns = UINT64_MAX;
            g_state.data[i].max_duration_ns = 0;

            snprintf(g_state.data[i].function_name, MAX_FUNCTION_NAME_LEN,
                    "%s", function_name);
            g_state.data[i].function_name[MAX_FUNCTION_NAME_LEN - 1] = '\0';

            g_state.function_count++;
            return true;
        }
    }

    fprintf(stderr, "Cannot register function %d - no available slots\n", function_id);
    return false;
}

void func_profiler_print_stats(void) {
    printf("\n===== Cgroup Performance Profiling Statistics =====\n");

    for (int i = 0; i < MAX_PROFILED_FUNCTIONS; i++) {
        profiling_data_t* item = &g_state.data[i];

        if (item->number == -1 || item->call_count == 0)
            continue;

        const char* func_name = (strlen(item->function_name) > 0) ?
                               item->function_name : "unknown_function";

#if defined(__x86_64__) || defined(__i386__)
        // Convert cycles to microseconds for x86/x64
        double cycles_to_us = get_cycles_to_microseconds();
        double total_us = (double)item->call_duration_ns * cycles_to_us;
        double avg_duration_us = total_us / item->call_count;
        double min_duration_us = (item->min_duration_ns != UINT64_MAX) ?
                                (double)item->min_duration_ns * cycles_to_us : 0.0;
        double max_duration_us = (double)item->max_duration_ns * cycles_to_us;

        printf("[ID:%d] %-30s: Calls: %6d, Total: %10.2f us, "
               "Avg: %8.2f us, Min: %8.2f us, Max: %8.2f us\n",
                item->number, func_name, item->call_count,
                total_us, avg_duration_us, min_duration_us, max_duration_us);
#else
        // Convert nanoseconds to microseconds for other architectures
        double total_us = (double)item->call_duration_ns / 1000.0;
        double avg_duration_us = total_us / item->call_count;
        double min_duration_us = (item->min_duration_ns != UINT64_MAX) ?
                                (double)item->min_duration_ns / 1000.0 : 0.0;
        double max_duration_us = (double)item->max_duration_ns / 1000.0;

        printf("[ID:%d] %-30s: Calls: %6d, Total: %10.2f us, "
               "Avg: %8.2f us, Min: %8.2f us, Max: %8.2f us\n",
                item->number, func_name, item->call_count,
                total_us, avg_duration_us, min_duration_us, max_duration_us);
#endif
    }

    printf("===== End Profiling Statistics =====\n\n");
}

void func_profiler_export_csv(const char *filename) {
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        fprintf(stderr, "Failed to open %s: %s\n", filename, strerror(errno));
        return;
    }

    fprintf(fp, "ID,Function,Calls,Total_us,Avg_us,Min_us,Max_us\n");

#if defined(__x86_64__) || defined(__i386__)
    double cycles_to_us = get_cycles_to_microseconds();
#endif

    for (int i = 0; i < MAX_PROFILED_FUNCTIONS; i++) {
        profiling_data_t* item = &g_state.data[i];

        if (item->number == -1 || item->call_count == 0)
            continue;

        const char* func_name = (strlen(item->function_name) > 0) ?
                               item->function_name : "unknown_function";

#if defined(__x86_64__) || defined(__i386__)
        double total_us = (double)item->call_duration_ns * cycles_to_us;
        double avg_us = total_us / item->call_count;
        double min_us = (item->min_duration_ns != UINT64_MAX) ?
                        (double)item->min_duration_ns * cycles_to_us : 0.0;
        double max_us = (double)item->max_duration_ns * cycles_to_us;
#else
        double total_us = (double)item->call_duration_ns / 1000.0;
        double avg_us = total_us / item->call_count;
        double min_us = (item->min_duration_ns != UINT64_MAX) ?
                        (double)item->min_duration_ns / 1000.0 : 0.0;
        double max_us = (double)item->max_duration_ns / 1000.0;
#endif

        fprintf(fp, "%d,%s,%d,%.2f,%.2f,%.2f,%.2f\n",
                item->number, func_name, item->call_count,
                total_us, avg_us, min_us, max_us);
    }

    fclose(fp);
    printf("Performance data exported to %s\n", filename);
}

void func_profiler_enable(void) {
    g_state.enabled = true;
}

void func_profiler_disable(void) {
    g_state.enabled = false;
}

bool func_profiler_is_enabled(void) {
    return g_state.enabled;
}
