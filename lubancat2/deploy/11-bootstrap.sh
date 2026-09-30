#!/bin/bash
# 11-bootstrap.sh —— 在鲁班猫2 上「一键」完成实时化持久配置
# 前置：deploy/push.sh 已把脚本推到 /opt/kine-x/deploy
# 内容：① 网口分离(eth0=EtherCAT/eth1=业务) ② 内核启动参数 ③ 开机自动调优服务
# 说明：②需重启生效；重启后 ③ 会自动重新应用 governor/中断亲和/限流。
set -u
D="${DEPLOY_DIR:-/opt/kine-x/deploy}"
sep() { echo; echo "########## $* ##########"; }

sep "① 网口分离"
bash "$D/08-net-separate.sh" apply

sep "② 内核启动参数"
bash "$D/06-cmdline.sh" apply

sep "③ 开机自动调优服务"
bash "$D/09-tune-service.sh" install

sep "④ 汇总"
bash "$D/05-tune-runtime.sh" status
echo
echo "下一步：sudo reboot   （重启后执行 deploy/12-verify.sh 复核）"
sep "11-bootstrap DONE"
