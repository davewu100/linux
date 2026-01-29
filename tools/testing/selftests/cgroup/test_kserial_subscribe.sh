#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Test kserial subscribe-publish mode

set -e

echo "================================"
echo "kserial Subscribe-Publish Test"
echo "================================"
echo

# Check if kserial is available
if [ ! -e /proc/kserial ]; then
    echo "ERROR: /proc/kserial not found"
    echo "Please load kserial kernel modules first:"
    echo "  sudo insmod kernel/kserial.ko"
    echo "  sudo insmod kernel/kserial_procfs.ko"
    exit 1
fi

# Compile example if needed
if [ ! -f kserial_subscribe_example ]; then
    echo "Compiling example..."
    gcc -O2 -o kserial_subscribe_example kserial_subscribe_example.c
    echo
fi

# Run example
echo "Running subscribe-publish examples..."
echo
sudo ./kserial_subscribe_example

echo
echo "Test completed!"
