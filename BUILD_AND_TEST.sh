#!/bin/bash
# Build and test the fixed v0 kernel

echo "=== v0 启动保护修复 - 编译和测试 ==="
echo ""
echo "当前 commit: $(git log -1 --oneline)"
echo "分支: $(git branch --show-current)"
echo ""

# Step 1: 编译
echo "步骤 1/3: 编译内核..."
read -p "开始编译? (y/n): " compile
if [ "$compile" = "y" ]; then
    echo "编译中，这可能需要几分钟..."
    time make -j$(nproc)
    
    if [ $? -eq 0 ]; then
        echo "✅ 编译成功!"
    else
        echo "❌ 编译失败，请检查错误信息"
        exit 1
    fi
else
    echo "跳过编译"
fi

# Step 2: 安装
echo ""
echo "步骤 2/3: 安装内核..."
read -p "安装内核? 需要 sudo 权限 (y/n): " install
if [ "$install" = "y" ]; then
    echo "安装中..."
    sudo make modules_install install
    
    if [ $? -eq 0 ]; then
        echo "✅ 安装成功!"
    else
        echo "❌ 安装失败"
        exit 1
    fi
else
    echo "跳过安装"
    exit 0
fi

# Step 3: 重启
echo ""
echo "步骤 3/3: 重启系统测试..."
echo ""
echo "⚠️  重要提示:"
echo "  - 系统将重启到新内核"
echo "  - 观察是否能正常启动到登录界面"
echo "  - 如果卡住，可以从 GRUB 选择旧内核"
echo ""
read -p "现在重启? (y/n): " reboot
if [ "$reboot" = "y" ]; then
    echo "3 秒后重启..."
    sleep 3
    sudo reboot
else
    echo ""
    echo "稍后手动重启: sudo reboot"
fi
