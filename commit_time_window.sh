#!/bin/bash
set -e

echo "========================================="
echo "Committing Time-Window Cache Implementation"
echo "========================================="
echo ""

# 显示改动
echo "Modified files:"
git status --short include/linux/cgroup-atomic.h \
                   include/linux/memcontrol.h \
                   kernel/cgroup/atomic.c \
                   mm/memcontrol-atomic.c 2>/dev/null || echo "(git not available)"

echo ""
echo "Code statistics:"
echo "  kernel/cgroup/atomic.c: $(wc -l < kernel/cgroup/atomic.c) lines"
echo "  mm/memcontrol-atomic.c: $(wc -l < mm/memcontrol-atomic.c) lines"
echo ""

# 提交
git add include/linux/cgroup-atomic.h \
        include/linux/memcontrol.h \
        kernel/cgroup/atomic.c \
        mm/memcontrol-atomic.c 2>/dev/null || {
    echo "Warning: git add failed"
    exit 1
}

git commit -F COMMIT_MSG_TIME_WINDOW.txt || {
    echo "Error: git commit failed"
    exit 1
}

echo ""
echo "✓ Commit created successfully!"
echo ""
git log -1 --stat
