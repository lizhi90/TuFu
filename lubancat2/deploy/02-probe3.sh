#!/bin/bash
# 02-probe3.sh —— 第三轮：确认「真正的启动参数入口」与「能否装包/取内核源码」
sep() { echo; echo "===== $* ====="; }

sep "挂载点（/boot 在哪个分区）"
mount | grep -E 'mmcblk|/boot' || echo "(无匹配)"

sep "U-Boot 引导脚本 boot.cmd（判断 extlinux 是否生效）"
cat /boot/boot.cmd 2>/dev/null | head -60

sep "boot.scr 是否引用 extlinux"
sudo strings /boot/boot.scr 2>/dev/null | grep -iE 'extlinux|boot_prefixes|sysboot|Image' | head -20 || echo "(读不到)"

sep "/boot/uEnv 内容"
ls -l /boot/uEnv/ 2>/dev/null
for f in /boot/uEnv/*; do [ -f "$f" ] && { echo "--- $f ---"; cat "$f"; }; done

sep "apt 网络可达性（HTTP 层，ICMP 常被禁）"
if command -v curl >/dev/null 2>&1; then
  timeout 8 curl -sI -m 6 http://mirrors.ustc.edu.cn/debian/dists/bookworm/Release | head -3 || echo "curl 失败"
elif command -v wget >/dev/null 2>&1; then
  timeout 8 wget -qS -T 6 -O /dev/null http://mirrors.ustc.edu.cn/debian/dists/bookworm/Release 2>&1 | head -3 || echo "wget 失败"
else
  echo "(无 curl/wget)"
fi
echo "-- 默认路由 --"
ip route

sep "apt 列表状态"
ls /var/lib/apt/lists/ 2>/dev/null | head -8
echo "lists 文件数 = $(ls /var/lib/apt/lists/ 2>/dev/null | wc -l)"

sep "能否装 rt-tests / 取内核源码"
apt-cache policy rt-tests 2>/dev/null | head -5
apt-cache search 'linux-source' 2>/dev/null | head -5
apt-cache search 'embedfire' 2>/dev/null | head -10
apt-cache search 'linux-headers' 2>/dev/null | grep -i rk356 | head -5

sep "embedfire 源内容"
cat /etc/apt/sources.list.d/embedfire-*.list 2>/dev/null

sep "已有可用工具"
for t in gcc make git gzip xz tar python3 curl wget; do
  command -v $t >/dev/null 2>&1 && echo "  $t: $(command -v $t)" || echo "  $t: 缺失"
done

sep "PROBE3 DONE"
