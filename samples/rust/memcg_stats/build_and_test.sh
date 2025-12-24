#!/bin/bash
# Build and test TLV to JSON converter

set -e

echo "Building TLV to JSON converter..."
cargo build --release

echo "Running tests..."
cargo test

echo "Testing with sample TLV data..."
# Create sample TLV data for testing
# TLV entry: type=1 (anon), length=8, value=12345
echo -ne '\x00\x01\x00\x08\x00\x00\x00\x00\x00\x00\x30\x39' > test.tlv

echo "Sample TLV data created. You can now run:"
echo "./target/release/tlv_to_json test.tlv"
echo "Or with real kernel data:"
echo "./target/release/tlv_to_json /sys/fs/cgroup/memory/memory.stat_bin"

echo "Build complete!"
