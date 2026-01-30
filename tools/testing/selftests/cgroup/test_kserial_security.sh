#!/bin/bash
# kserial security test script
# Test kserial security mechanisms (integer overflow, bounds checking, format validation, etc.)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== kserial Security Test ==="
echo ""

# 1. Check if /dev/kserial exists
if [ ! -e /dev/kserial ]; then
    echo "Error: /dev/kserial not found"
    echo ""
    echo "Please load kserial modules first. Use one of the following methods:"
    echo ""
    echo "Method 1: Use quick load script"
    echo "  cd $SCRIPT_DIR"
    echo "  sudo ./quick_load_kserial.sh"
    echo ""
    echo "Method 2: Use load script"
    echo "  cd $SCRIPT_DIR"
    echo "  sudo ./load_kserial_module.sh"
    echo ""
    echo "Method 3: Manual load (in order)"
    echo "  cd $SCRIPT_DIR/../.."
    echo "  sudo insmod kernel/kserial_block.ko"
    echo "  sudo insmod kernel/kserial.ko"
    echo "  sudo insmod kernel/kserial_procfs.ko"
    echo ""
    echo "Verify modules are loaded:"
    echo "  lsmod | grep kserial"
    echo "  ls -la /dev/kserial"
    exit 1
fi

# 2. Compile test program
echo "1. Compiling test program..."
if [ ! -f test_kserial_security ]; then
    if [ -f Makefile.kserial ]; then
        make -f Makefile.kserial test_kserial_security
    else
        gcc -Wall -Wextra -O2 -o test_kserial_security test_kserial_security.c
    fi
    if [ $? -ne 0 ]; then
        echo "Compilation failed"
        exit 1
    fi
    echo "✓ Compilation successful"
else
    echo "✓ Test program already exists"
fi

echo ""
echo "2. Running security tests..."
echo ""

# 3. Run tests (requires root privileges)
if [ "$EUID" -ne 0 ]; then
    echo "Note: Root privileges required to run tests"
    echo "Run with sudo:"
    echo "  sudo $0"
    echo ""
    echo "Or run directly:"
    echo "  sudo ./test_kserial_security"
    exit 1
fi

# Run tests
./test_kserial_security
TEST_RESULT=$?

echo ""
if [ $TEST_RESULT -eq 0 ]; then
    echo "=== All security tests PASSED ==="
    exit 0
else
    echo "=== Security tests FAILED - Vulnerabilities detected! ==="
    exit 1
fi
