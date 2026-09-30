#!/bin/bash
# d6-dir-check.sh —— 检查 D6 脚本目录层级与服务身份（板上执行）
ls -d /userdata 2>&1
ls -ld /userdata/kine-x 2>&1
ls -ld /userdata/kine-x/scripts 2>&1
echo "--- 服务身份 ---"
ps -o user=,pid=,cmd= -p "$(systemctl show kine-x -p MainPID --value)"
echo "--- 手动逐级建目录测试 ---"
sudo mkdir -p /userdata/kine-x/scripts 2>&1 && echo "mkdir -p OK"
ls -ld /userdata/kine-x/scripts 2>&1
sudo chown -R "$(id -un):$(id -gn)" /userdata/kine-x 2>/dev/null
ls -ld /userdata/kine-x /userdata/kine-x/scripts
