#!/bin/bash
#
# Git commit script for hierarchical atomic counter implementation
#

set -e

cd /home/jianyuew/repo/tmp/linux

echo "========================================="
echo "Committing Hierarchical Atomic Counter"
echo "========================================="
echo ""

# 显示改动的文件
echo "Modified files:"
git status --short include/linux/cgroup-atomic.h \
                   include/linux/memcontrol.h \
                   kernel/cgroup/atomic.c \
                   mm/memcontrol-atomic.c \
    2>/dev/null || echo "  (git status unavailable)"

echo ""

# 显示统计
echo "Code statistics:"
echo "  kernel/cgroup/atomic.c: $(wc -l < kernel/cgroup/atomic.c) lines (was ~540 lines)"
echo "  mm/memcontrol-atomic.c: $(wc -l < mm/memcontrol-atomic.c) lines"
echo ""

# 暂存文件
echo "Staging files..."
git add include/linux/cgroup-atomic.h \
        include/linux/memcontrol.h \
        kernel/cgroup/atomic.c \
        mm/memcontrol-atomic.c \
    2>/dev/null || {
    echo "Warning: git add failed, are you in a git repository?"
    exit 1
}

echo "✓ Files staged"
echo ""

# 提交
echo "Creating commit..."
git commit -F COMMIT_MSG_HIERARCHICAL.txt || {
    echo "Error: git commit failed"
    exit 1
}

echo ""
echo "✓ Commit created successfully!"
echo ""

# 显示 commit 信息
echo "========================================="
echo "Commit details:"
echo "========================================="
git log -1 --stat

echo ""
echo "To view the commit:"
echo "  git show HEAD"
echo ""
echo "To create a patch:"
echo "  git format-patch -1 HEAD -o ."
echo ""
