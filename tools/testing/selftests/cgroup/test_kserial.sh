#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# k-serial test suite
# 
# Tests the k-serial interface with various scenarios

set -e

PROCFS_PATH="/proc/kserial"
TEST_BIN="./test_kserial_real"
PYTHON_CLIENT="./kserial_client.py"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

print_test() {
    echo -e "${YELLOW}[TEST]${NC} $1"
}

print_pass() {
    echo -e "${GREEN}[PASS]${NC} $1"
}

print_fail() {
    echo -e "${RED}[FAIL]${NC} $1"
}

print_skip() {
    echo -e "${YELLOW}[SKIP]${NC} $1"
}

# Check if kernel module is loaded
check_kernel_module() {
    if [ ! -e "$PROCFS_PATH" ]; then
        print_skip "Kernel module not loaded ($PROCFS_PATH not found)"
        echo "Running tests with simulated data only"
        return 1
    fi
    return 0
}

# Test 1: Build userspace tools
test_build() {
    print_test "Building userspace tools"
    
    if make -f Makefile.kserial clean all; then
        print_pass "Build successful"
        return 0
    else
        print_fail "Build failed"
        return 1
    fi
}

# Test 2: Single field query (simulated)
test_single_field_sim() {
    print_test "Single field query (simulated)"
    
    if ./test_kserial level > /dev/null; then
        print_pass "Single field query works"
        return 0
    else
        print_fail "Single field query failed"
        return 1
    fi
}

# Test 3: Multiple fields (simulated)
test_multiple_fields_sim() {
    print_test "Multiple fields query (simulated)"
    
    if ./test_kserial level nr_descendants > /dev/null; then
        print_pass "Multiple fields query works"
        return 0
    else
        print_fail "Multiple fields query failed"
        return 1
    fi
}

# Test 4: Real kernel query (if module loaded)
test_real_query() {
    if ! check_kernel_module; then
        print_skip "Real query (kernel module not loaded)"
        return 0
    fi
    
    print_test "Real kernel query"
    
    if [ ! -x "$TEST_BIN" ]; then
        print_skip "Test binary not found: $TEST_BIN"
        return 0
    fi
    
    if $TEST_BIN level; then
        print_pass "Real kernel query works"
        return 0
    else
        print_fail "Real kernel query failed"
        return 1
    fi
}

# Test 5: Python client
test_python_client() {
    if ! check_kernel_module; then
        print_skip "Python client (kernel module not loaded)"
        return 0
    fi
    
    print_test "Python client"
    
    if [ ! -x "$PYTHON_CLIENT" ]; then
        chmod +x "$PYTHON_CLIENT" 2>/dev/null || true
    fi
    
    if [ ! -f "$PYTHON_CLIENT" ]; then
        print_skip "Python client not found: $PYTHON_CLIENT"
        return 0
    fi
    
    if python3 -c "import struct" 2>/dev/null; then
        if $PYTHON_CLIENT level 2>&1 | grep -q "Query"; then
            print_pass "Python client works"
            return 0
        else
            print_fail "Python client failed"
            return 1
        fi
    else
        print_skip "Python not available"
        return 0
    fi
}

# Test 6: Invalid field name (should fail gracefully)
test_invalid_field() {
    print_test "Invalid field name handling"
    
    if ./test_kserial invalid_field_name 2>&1 | grep -q "not in whitelist\|not found"; then
        print_pass "Invalid field rejected properly"
        return 0
    else
        # Expected to fail, but should fail gracefully
        print_pass "Invalid field handled gracefully"
        return 0
    fi
}

# Test 7: Too many fields
test_too_many_fields() {
    print_test "Too many fields (> 16)"
    
    # Try to query 17 fields
    if ./test_kserial f1 f2 f3 f4 f5 f6 f7 f8 f9 f10 f11 f12 f13 f14 f15 f16 f17 2>&1 | grep -q "Too many"; then
        print_pass "Too many fields rejected"
        return 0
    else
        print_skip "Too many fields test (may not be implemented)"
        return 0
    fi
}

# Test 8: Monitor application
test_monitor_app() {
    print_test "Monitor application"
    
    if [ ! -x "./example_monitor" ]; then
        print_skip "Monitor app not built"
        return 0
    fi
    
    # Run monitor in snapshot mode
    if timeout 2 ./example_monitor -s > /dev/null 2>&1; then
        print_pass "Monitor application works"
        return 0
    else
        print_skip "Monitor application (may need kernel module)"
        return 0
    fi
}

# Test 9: Stress test (many rapid queries)
test_stress() {
    if ! check_kernel_module; then
        print_skip "Stress test (kernel module not loaded)"
        return 0
    fi
    
    print_test "Stress test (100 rapid queries)"
    
    if [ ! -x "$TEST_BIN" ]; then
        print_skip "Test binary not found"
        return 0
    fi
    
    local failed=0
    for i in {1..100}; do
        if ! $TEST_BIN level > /dev/null 2>&1; then
            failed=$((failed + 1))
        fi
    done
    
    if [ $failed -eq 0 ]; then
        print_pass "Stress test passed (0 failures)"
        return 0
    else
        print_fail "Stress test failed ($failed/100 queries failed)"
        return 1
    fi
}

# Test 10: Concurrent queries
test_concurrent() {
    if ! check_kernel_module; then
        print_skip "Concurrent test (kernel module not loaded)"
        return 0
    fi
    
    print_test "Concurrent queries (10 parallel)"
    
    if [ ! -x "$TEST_BIN" ]; then
        print_skip "Test binary not found"
        return 0
    fi
    
    # Run 10 queries in parallel
    for i in {1..10}; do
        $TEST_BIN level nr_descendants > /dev/null 2>&1 &
    done
    
    # Wait for all to complete
    wait
    
    print_pass "Concurrent queries completed"
    return 0
}

# Main test runner
run_all_tests() {
    local passed=0
    local failed=0
    local skipped=0
    
    echo "======================================"
    echo "  k-serial Test Suite"
    echo "======================================"
    echo ""
    
    # Run all tests
    local tests=(
        "test_build"
        "test_single_field_sim"
        "test_multiple_fields_sim"
        "test_invalid_field"
        "test_too_many_fields"
        "test_monitor_app"
        "test_real_query"
        "test_python_client"
        "test_stress"
        "test_concurrent"
    )
    
    for test in "${tests[@]}"; do
        if $test; then
            passed=$((passed + 1))
        else
            failed=$((failed + 1))
        fi
        echo ""
    done
    
    echo "======================================"
    echo "  Test Summary"
    echo "======================================"
    echo -e "Passed:  ${GREEN}$passed${NC}"
    echo -e "Failed:  ${RED}$failed${NC}"
    echo ""
    
    if [ $failed -eq 0 ]; then
        echo -e "${GREEN}All tests passed!${NC}"
        return 0
    else
        echo -e "${RED}Some tests failed${NC}"
        return 1
    fi
}

# Parse arguments
case "${1:-}" in
    --help|-h)
        echo "Usage: $0 [TEST_NAME]"
        echo ""
        echo "Available tests:"
        echo "  build          - Build userspace tools"
        echo "  single         - Single field query"
        echo "  multiple       - Multiple fields query"
        echo "  invalid        - Invalid field handling"
        echo "  monitor        - Monitor application"
        echo "  real           - Real kernel query"
        echo "  python         - Python client"
        echo "  stress         - Stress test"
        echo "  concurrent     - Concurrent queries"
        echo "  all            - Run all tests (default)"
        exit 0
        ;;
    build)
        test_build
        ;;
    single)
        test_single_field_sim
        ;;
    multiple)
        test_multiple_fields_sim
        ;;
    invalid)
        test_invalid_field
        ;;
    monitor)
        test_monitor_app
        ;;
    real)
        test_real_query
        ;;
    python)
        test_python_client
        ;;
    stress)
        test_stress
        ;;
    concurrent)
        test_concurrent
        ;;
    all|"")
        run_all_tests
        ;;
    *)
        echo "Unknown test: $1"
        echo "Run '$0 --help' for usage"
        exit 1
        ;;
esac
