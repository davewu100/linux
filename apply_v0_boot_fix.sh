#!/bin/bash
# Apply boot fix to v0 if needed

echo "=== Atomic Counter v0 Boot Fix ==="
echo ""
echo "This adds mem_cgroup_online() check to v0 implementation"
echo ""

# Check current branch
if [ "$(git branch --show-current)" != "atomic_counter_impl_v0" ]; then
    echo "ERROR: Not on atomic_counter_impl_v0 branch"
    echo "Current branch: $(git branch --show-current)"
    exit 1
fi

# Apply the patch
if patch -p1 < v0_boot_fix_if_needed.patch; then
    echo "Patch applied successfully!"
    echo ""
    echo "Next steps:"
    echo "  1. make -j$(nproc)"
    echo "  2. sudo make modules_install install"
    echo "  3. sudo reboot"
else
    echo "Failed to apply patch"
    exit 1
fi
