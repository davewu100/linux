#!/bin/bash
# Helper script to commit the boot hang fix

echo "=== Atomic Counter Boot Hang Fix - Commit Helper ==="
echo ""
echo "This will create a git commit with the boot hang fixes."
echo ""

# Show current changes
echo "Files to be committed:"
git diff --name-only kernel/cgroup/atomic.c mm/memcontrol-atomic.c
echo ""

# Show stats
echo "Change statistics:"
git diff --stat kernel/cgroup/atomic.c mm/memcontrol-atomic.c
echo ""

# Ask for confirmation
read -p "Create commit? (y/n): " confirm
if [ "$confirm" != "y" ]; then
    echo "Aborted."
    exit 1
fi

# Stage the files
git add kernel/cgroup/atomic.c mm/memcontrol-atomic.c

# Commit with the message
git commit -F BOOT_HANG_FIX_COMMIT_MSG.txt

echo ""
echo "Commit created successfully!"
echo ""
echo "To view the commit:"
echo "  git show HEAD"
echo ""
echo "To amend the commit message:"
echo "  git commit --amend"
echo ""
echo "To create a patch file:"
echo "  git format-patch HEAD~1"
