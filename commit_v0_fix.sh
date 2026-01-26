#!/bin/bash
# Commit the v0 boot safety fixes

echo "=== Committing v0 Boot Safety Fixes ==="
echo ""

# Check we're on the right branch
if [ "$(git branch --show-current)" != "atomic_counter_impl_v0" ]; then
    echo "ERROR: Not on atomic_counter_impl_v0 branch"
    echo "Current branch: $(git branch --show-current)"
    exit 1
fi

# Show what will be committed
echo "Files to be committed:"
git diff --name-only kernel/cgroup/atomic.c mm/memcontrol-atomic.c
echo ""

echo "Change statistics:"
git diff --stat kernel/cgroup/atomic.c mm/memcontrol-atomic.c
echo ""

# Confirm
read -p "Create commit? (y/n): " confirm
if [ "$confirm" != "y" ]; then
    echo "Aborted."
    exit 1
fi

# Stage and commit
git add kernel/cgroup/atomic.c mm/memcontrol-atomic.c
git commit -F V0_COMMIT_MSG.txt

echo ""
echo "✅ Commit created successfully!"
echo ""
echo "Next steps:"
echo "  1. View commit: git show HEAD"
echo "  2. Create patch: git format-patch -1 HEAD"
echo "  3. Build kernel: make -j\$(nproc)"
echo "  4. Install: sudo make modules_install install"
echo "  5. Reboot and test!"
