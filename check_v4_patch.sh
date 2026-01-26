#!/bin/bash

echo "=== V4 Patch 检查 ==="
echo

echo "1. 检查 patch 文件是否存在..."
if [ -f v4-0000-cover-letter.patch ] && [ -f v4-0001-mm-optimize-stat-output-for-11-sys-time-reduce.patch ]; then
    echo "   ✅ Patch 文件存在"
else
    echo "   ❌ Patch 文件缺失"
    exit 1
fi

echo
echo "2. 检查 In-Reply-To 头部..."
if grep -q "In-Reply-To: <87ec59f7-2d76-4c7a-a2b0-57bc4e801d1d@gmail.com>" v4-0000-cover-letter.patch; then
    echo "   ✅ In-Reply-To 正确"
else
    echo "   ❌ In-Reply-To 缺失或错误"
fi

echo
echo "3. 检查 References 头部..."
if grep -q "References: <20260122114242.72139-1-wujianyue000@gmail.com>" v4-0000-cover-letter.patch; then
    echo "   ✅ References 正确"
else
    echo "   ⚠️  References 可能缺失"
fi

echo
echo "4. 检查 Suggested-by tag..."
if grep -q "Suggested-by: JP Kobryn" v4-0001-*.patch; then
    echo "   ✅ Suggested-by 存在"
else
    echo "   ❌ Suggested-by 缺失"
fi

echo
echo "5. 检查 Acked-by tag..."
if grep -q "Acked-by: Shakeel Butt" v4-0001-*.patch; then
    echo "   ✅ Acked-by 存在"
else
    echo "   ❌ Acked-by 缺失"
fi

echo
echo "6. 统计改动..."
grep "files changed" v4-0001-*.patch | head -1

echo
echo "7. Cover letter 关键内容预览..."
echo "---"
sed -n '/Changes in v4:/,/Performance improvement/p' v4-0000-cover-letter.patch | head -10
echo "---"

echo
echo "✅ 检查完成！可以发送 patch 了。"
echo
echo "发送命令："
echo "git send-email --to akpm@linux-foundation.org \\"
echo "               --cc shakeel.butt@linux.dev \\"
echo "               --cc hannes@cmpxchg.org \\"
echo "               --cc mhocko@kernel.org \\"
echo "               --cc roman.gushchin@linux.dev \\"
echo "               --cc muchun.song@linux.dev \\"
echo "               --cc linux-mm@kvack.org \\"
echo "               --cc cgroups@vger.kernel.org \\"
echo "               --cc linux-kernel@vger.kernel.org \\"
echo "               --cc inwardvessel@gmail.com \\"
echo "               v4-*.patch"
