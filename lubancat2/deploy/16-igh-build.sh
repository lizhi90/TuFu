#!/bin/bash
# 16-igh-build.sh [源码目录] —— 在鲁班猫2 板上编译安装 IgH EtherCAT Master（generic 驱动）
# 前置：
#   1) deploy/15-igh-deps.sh        装 build-essential/autoconf/libtool/...
#   2) deploy/push-igh.sh           把 IgH 源码传到 /opt/kine-x/ethercat
# 说明：内核模块 ec_master.ko / ec_generic.ko 必须用与运行内核一致的
#       /lib/modules/$(uname -r)/build 编译，所以本步骤只能在板上做。
set -u
SRC="${1:-/opt/kine-x/ethercat}"
DESTDIR_PREFIX="${PREFIX:-/usr/local}"
sep() { echo; echo "===== $* ====="; }

cd "$SRC" 2>/dev/null || { echo "✘ 源码目录不存在: $SRC（先跑 deploy/push-igh.sh）"; exit 1; }
KREL="$(uname -r)"
KBUILD="/lib/modules/$KREL/build"

sep "环境"
echo "  kernel  : $KREL"
echo "  kbuild  : $KBUILD -> $(readlink -f "$KBUILD")"
echo "  gcc     : $(gcc --version | head -1)"
echo "  nproc   : $(nproc)"
echo "  源码    : $SRC"
[ -f "$KBUILD/Module.symvers" ] && echo "  ✔ Module.symvers 存在" || echo "  ✘ 缺 Module.symvers"

sep "bootstrap"
if [ -x ./configure ] && [ -f Makefile.in ]; then
  echo "  已生成 configure，跳过"
else
  ./bootstrap 2>&1 | tail -8 || { echo "✘ bootstrap 失败"; exit 1; }
fi

if [ -f Makefile ] && [ -f config.status ] && [ "${FORCE_CONFIGURE:-0}" != "1" ]; then
  sep "configure"
  echo "  已存在 Makefile/config.status，跳过重复配置（如需重配：FORCE_CONFIGURE=1）"
else
  sep "configure"
  ./configure \
    --prefix="$DESTDIR_PREFIX" \
    --with-linux-dir="$KBUILD" \
    --enable-generic \
    --disable-eoe \
    --disable-8139too --disable-e100 --disable-e1000 --disable-e1000e \
    --disable-genet --disable-macb --disable-igb --disable-igc \
    --disable-r8169 --disable-stmmac-pci --disable-dwmac-intel \
    --disable-ccat --disable-rtdm \
    --disable-initd \
    --with-systemdsystemunitdir=/lib/systemd/system \
    --enable-tool --enable-userlib \
    2>&1 | tail -45
  [ -f Makefile ] || { echo "✘ configure 失败"; exit 1; }
fi

# 注意：IgH 的内核模块不在 `all` 目标里（master/devices 的 all 为空），
#       必须显式 `make modules`，否则拿不到 ec_master.ko / ec_generic.ko。
sep "make all（-j$(nproc)）"
make -j"$(nproc)" all 2>&1 | tail -15

sep "make modules（-j$(nproc)）"
make -j"$(nproc)" modules 2>&1 | tail -25
[ -f master/ec_master.ko ] || { echo "✘ make modules 未产出 ec_master.ko"; }

sep "产物核对"
ok=0
for f in master/ec_master.ko devices/ec_generic.ko; do
  if [ -f "$f" ]; then echo "  ✔ $f ($(du -h "$f" | cut -f1))"; else echo "  ✘ 缺 $f"; ok=1; fi
done
[ -x tool/ethercat ] && echo "  ✔ tool/ethercat" || { echo "  ✘ 缺 tool/ethercat"; ok=1; }
[ "$ok" = 0 ] || { echo "✘ 有产物缺失，中止安装"; exit 1; }

sep "安装"
sudo make modules_install 2>&1 | tail -6
sudo make install 2>&1 | tail -6
sudo depmod -a
sudo ldconfig            # 关键：libethercat.so 装在 /usr/local/lib，不刷新缓存会出现 "not found"
echo "  depmod / ldconfig 完成"

sep "安装结果"
ls -l "$DESTDIR_PREFIX/bin/ethercat" 2>/dev/null
ls -l "$DESTDIR_PREFIX"/lib/libethercat.so* 2>/dev/null | head -3
ls -l "/lib/modules/$KREL/ethercat/" 2>/dev/null
echo "  --- 配置文件 ---"
ls -l /etc/ethercat.conf "$DESTDIR_PREFIX/etc/ethercat.conf" 2>/dev/null
echo "  --- systemd 单元 ---"
ls -l /lib/systemd/system/ethercat.service 2>/dev/null || echo "  (未安装 systemd 单元)"

sep "试加载内核模块"
if lsmod | grep -q '^ec_master'; then
  echo "  ec_master 已在运行"
else
  sudo modprobe ec_master && echo "  ✔ modprobe ec_master 成功" || echo "  ✘ modprobe ec_master 失败（看 dmesg）"
fi
lsmod | grep -E '^ec_' || echo "  (无 ec_* 模块)"
echo "  --- dmesg 尾部 ---"
sudo dmesg | tail -25 | grep -iE 'ethercat|ec_master|ec_generic' || echo "  (dmesg 无相关输出)"
# 重要：这里只是验证模块能加载，随后必须卸载。
# 若留着无参加载的 ec_master，之后 ethercatctl 用 main_devices=<MAC> 再 modprobe 会因
# 「模块已加载」而被忽略，导致 /dev/EtherCAT0 不创建。正式启动请用 deploy/17-config-master.sh。
echo "  --- 卸载试加载的模块（交给 ethercatctl 正规加载）---"
sudo rmmod ec_generic 2>/dev/null || true
sudo rmmod ec_master 2>/dev/null || true
lsmod | grep -E '^ec_' || echo "  ✔ 已卸载干净"

sep "16-igh-build DONE"
