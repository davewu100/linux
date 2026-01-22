// SPDX-License-Identifier: GPL-2.0
// func_profiler.h - Lightweight function profiling tool

#ifndef FUNC_PROFILER_H
#define FUNC_PROFILER_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#define MAX_PROFILED_FUNCTIONS 32
#define MAX_FUNCTION_NAME_LEN 64

// Profiling data structure
typedef struct {
    int number;
    int call_count;
    uint64_t call_duration_ns;
    uint64_t min_duration_ns;
    uint64_t max_duration_ns;
    char function_name[MAX_FUNCTION_NAME_LEN];
} profiling_data_t;

// Global profiler state
typedef struct {
    bool enabled;
    int function_count;
    profiling_data_t data[MAX_PROFILED_FUNCTIONS];
} profiler_state_t;

// Context structure for profiling a function call
typedef struct {
    int number;
    uint64_t start_time;
} profiling_context_t;

// RDTSC wrapper for x86/x64
static inline uint64_t rdtsc_profiler(void) {
#if defined(__x86_64__) || defined(__i386__)
    uint32_t lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#else
    // Fallback to clock_gettime for non-x86 architectures
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
#endif
}

// Public API functions
void func_profiler_init(void);
void func_profiler_cleanup(void);
void func_profiler_add_data(int number, uint64_t duration);
bool func_profiler_register_function(int function_id, const char* function_name);
void func_profiler_print_stats(void);
void func_profiler_export_csv(const char *filename);
void func_profiler_enable(void);
void func_profiler_disable(void);
bool func_profiler_is_enabled(void);
void func_profiler_reset(void);

// Cleanup function called automatically when context goes out of scope
static inline void profiling_cleanup_handler(profiling_context_t* ctx) {
    if (ctx && ctx->start_time > 0) {
        uint64_t end_time = rdtsc_profiler();
        func_profiler_add_data(ctx->number, end_time - ctx->start_time);
    }
}

// Initialize profiling context
static inline void profiling_init_context(profiling_context_t* ctx, int func_id) {
    ctx->number = func_id;
    ctx->start_time = rdtsc_profiler();
}

// Main profiling macros
#ifdef DISABLE_PROFILER
    #define PROFILE_FUNC() ((void)0)
    #define PROFILE_FUNC_NAMED(name) ((void)0)
    #define PROFILE_FUNC_ID(func_id) ((void)0)
    #define PROFILE_PRINT_STATS() ((void)0)
    #define PROFILE_ENABLE() ((void)0)
    #define PROFILE_DISABLE() ((void)0)
    #define PROFILE_RESET() ((void)0)
#else
    // Helper macros for token pasting
    #define PROFILE_CONCAT_(a, b) a##b
    #define PROFILE_CONCAT(a, b) PROFILE_CONCAT_(a, b)

    // Implementation macro with explicit ID
    #define PROFILE_FUNC_IMPL(unique_id, func_name) \
        enum { PROFILE_CONCAT(__prof_id_, unique_id) = unique_id }; \
        static bool PROFILE_CONCAT(__prof_reg_, unique_id) = false; \
        if (!PROFILE_CONCAT(__prof_reg_, unique_id)) { \
            func_profiler_register_function(PROFILE_CONCAT(__prof_id_, unique_id), func_name); \
            PROFILE_CONCAT(__prof_reg_, unique_id) = true; \
        } \
        profiling_context_t __prof_ctx __attribute__((cleanup(profiling_cleanup_handler))) = {0}; \
        profiling_init_context(&__prof_ctx, PROFILE_CONCAT(__prof_id_, unique_id))

    // Auto-generate ID and auto-register with function name
    #define PROFILE_FUNC() \
        PROFILE_FUNC_IMPL(__COUNTER__, __FUNCTION__)

    // Auto-generate ID and register with custom name
    #define PROFILE_FUNC_NAMED(name) \
        PROFILE_FUNC_IMPL(__COUNTER__, name)

    // Manual ID control (for comparing across runs)
    #define PROFILE_FUNC_ID(func_id) \
        PROFILE_FUNC_IMPL(func_id, __FUNCTION__)

    #define PROFILE_PRINT_STATS() \
        func_profiler_print_stats()

    #define PROFILE_ENABLE() \
        func_profiler_enable()

    #define PROFILE_DISABLE() \
        func_profiler_disable()

    #define PROFILE_RESET() \
        func_profiler_reset()
#endif

#endif // FUNC_PROFILER_H
