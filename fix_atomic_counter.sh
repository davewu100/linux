#!/bin/bash
# Fix atomic counter boot hang issue
# This script disables CONFIG_MEMCG_ATOMIC_COUNTER

cd /home/jianyuew/repo/tmp/linux

echo "=== Disabling MEMCG_ATOMIC_COUNTER ==="
sed -i 's/CONFIG_MEMCG_ATOMIC_COUNTER=y/# CONFIG_MEMCG_ATOMIC_COUNTER is not set/' .config

echo ""
echo "=== Verifying configuration ==="
grep -E "CONFIG_MEMCG_ATOMIC_COUNTER" .config || echo "Config successfully disabled"

echo ""
echo "=== Next steps ==="
echo "1. Run: make oldconfig"
echo "2. Rebuild kernel: make -j\$(nproc)"
echo "3. Install: sudo make modules_install install"
echo "4. Reboot to the new kernel"
echo ""
echo "Configuration updated successfully!"
