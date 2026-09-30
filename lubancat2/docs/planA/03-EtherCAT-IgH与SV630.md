# 03 EtherCAT(IgH) 与汇川 SV630

> **核心结论：IgH 不需要 ESI/XML**。ESI 是 TwinCAT 等配置工具用的；IgH 直接读从站
> SII EEPROM（`ethercat sdos/pdos/cstruct`）即可拿到 PDO/SDO 信息。所以"没有 ESI"不阻塞方案 A。
> 只有在需要**改写 PDO 映射**时，才用 `ecrt_slave_config_pdos()` 在代码里手工指定。

---

## 1. IgH 三个概念

| 概念 | 说明 | 对应原 ZBasic |
|------|------|---------------|
| **Master** | 一个网卡 = 一个主站 | 控制器内置 |
| **Domain** | 一组的 PDO 数据区（可多个，本项目 1 个） | — |
| **Slave Config** | 单从站的 PDO/DC/Watchdog 配置 | `ATYPE/units/DRIVE_PROFILE` |

数据流：`ecrt_domain_data()` → `RxPDO(6040/607A/6060...)` 写；`TxPDO(6041/6064...)` 读。

---

## 2. 先只用命令行把 SV630 摸清（不写代码）

> ✅ **前置已满足（M1 完成）**：IgH 1.6.13 已装好、主站绑定 `eth0`（`generic` 驱动），
> **SV630 已接入 `eth0` 并成功进 `OP`**，下列命令均已实跑（结果见 §7）。
> 装机过程与三个踩坑见 `02-软件栈与环境搭建.md` §3.5。

```bash
# 0) 主站自检（已通过 ✅）
sudo systemctl start ethercat
ls -l /dev/EtherCAT0               # 应存在
sudo ethercat master               # Master0 / Phase: Idle / Main: 8e:56:ce:3f:af:2b (attached)
sudo ethercat slaves               # 接上 SV630 后显示 OP

# 1) 接上 SV630 后（以下命令已实跑）
ethercat slaves -v                 # ✅ Vendor 0x00100000 / Product 0x000c0112 / Rev 0x00010000 / OP
ethercat sdos  0                   # SDO Info 未使能 → 改用自研 tools/ecat_sdo.cpp 读对象
ethercat pdos  0                   # 默认 PDO 映射（0x1600 Rx / 0x1A00 Tx）
ethercat cstruct                   # 可粘贴到 C 的 PDO 配置结构体 ★（⚠️ SII 默认长度 12/28 ≠ 真实映射）
```

**`ethercat cstruct` 是最重要的一步**：它输出 `ec_pdo_entry_info_t` /
`ec_pdo_info_t` / `ec_sync_info_t` 数组，可直接粘进 `ethercat_master.cpp`，
无需 ESI、无需手写映射。

### 读/写 SDO 验证（OP 前用 PREOP）

> ⚠️ SV630 的 SII 报 **Enable SDO Info: no** → `ethercat upload/download` 的交互式读法受限，
> 本工程改用自研 CLI `tools/ecat_sdo.cpp`（`ecrt_master_sdo_upload/download`）：
> 无参数运行时打印内置对象清单实读值；`--set 0xIDX:SUB:BITS=VALUE` 写入并自动回读校验（见 `07` §6.4）。

```bash
# 实读内置清单（605A/605D/605C/6085/603F/203F/200E 等）—— deploy/41-m2-sdo-stop.sh
ecat_sdo
# 写对象并回读校验（例：模式复位为 CSP）
ecat_sdo --set 0x6060:00:8=8       # → 回读 0x6061=8，见 deploy/43-m2-restore-mode.sh
```

### 从站状态机

```bash
ethercat states 0 OP      # 尝试进 OP（需 DC 配置正确，否则报 AL 错误）
ethercat states 0 PREOP
ethercat master           # 看 AL Status Code 排错
```

---

## 3. SV630 关键 CiA402 对象（✅ 已实读核定）

| 对象 | 名称 | 用在哪 |
|------|------|--------|
| `0x6040` | Controlword 控制字 | 使能/急停/清错 |
| `0x6041` | Statusword 状态字 | bit0 ready / bit3 fault / bit10 target reached |
| `0x6060` | Modes of operation | 8=CSP / 1=PP / 3=PV / 6=HM |
| `0x6061` | Modes of operation display | 回读当前模式 |
| `0x607A` | Target position | 目标位置(脉冲/inc) |
| `0x6064` | Position actual value | 实际位置(inc) → 换算 mm = MPOS |
| `0x6062` | Position demand value | 指令位置(inc) → 换算 mm = DPOS |
| `0x6081` | Profile velocity | 速度 |
| `0x6083` / `0x6084` | Profile acceleration / deceleration | 加/减速 |
| `0x607F` | Max profile velocity | 速度上限（实读 26214400） |
| `0x60FF` | Target velocity | PV 模式速度（**PDO 未映射 → 走 SDO**） |
| `0x605A` | Quick stop option code | `6040.bit2=0` 停机方式（实读 2） |
| **`0x605D`** | **Halt option code** | **决定 `6040.bit8`(halt) 停机方式（实读 1=减速停）** |
| `0x605C` | Disable operation option | 伺服 OFF 停机方式（实读 0） |
| `0x603F` | Error code (Uint16) | 报警码（**会重复，仅供粗判**） |
| **`0x203F`** | **Error code (Uint32)** | **唯一错误码，作 `MV_ERRMASK` 映射依据** |

---

## 4. PDO 映射策略

**优先方案：用 SII 默认映射**（`ethercat cstruct` 输出直接使用），因为：
- 无需 ESI、无需改写 Slave 的 EEPROM；
- 汇川默认 RxPDO 一般已含 `6040 / 607A / 6060 / (6072)`，TxPDO 含 `6041 / 6064 / 6061`，
  单轴定位足够。

**备选方案：代码内自定义映射**（默认 PDO 缺字段或需要精简时）：

```cpp
static ec_pdo_entry_info_t slave_pdo_entries[] = {
    {0x6040, 0x00, 16},   // controlword
    {0x607a, 0x00, 32},   // target position
    {0x6060, 0x00, 8 },   // modes of operation
    {0x6041, 0x00, 16},   // statusword
    {0x6064, 0x00, 32},   // position actual
};
static ec_pdo_info_t slave_pdos[] = {
    {0x1600, 3, slave_pdo_entries + 0},   // RxPDO
    {0x1a00, 2, slave_pdo_entries + 3},   // TxPDO
};
static ec_sync_info_t slave_syncs[] = {
    {0, EC_DIR_OUTPUT, 0, NULL, EC_WD_ENABLE},
    {1, EC_DIR_INPUT , 0, NULL, EC_WD_DISABLE},
    {2, EC_DIR_OUTPUT, 1, slave_pdos + 0, EC_WD_ENABLE},
    {3, EC_DIR_INPUT , 1, slave_pdos + 1, EC_WD_DISABLE},
    {0xff}
};
ecrt_slave_config_pdos(sc, EC_END, slave_syncs);
```

---

## 5. DC（分布式时钟）配置

单轴 1ms 周期，推荐 SYNC0 周期=1ms、偏移=0.5ms：

```cpp
// assign_activate = 0x0300 (SYNC0)；周期 1ms；SYNC0 shift = 0.5ms
ecrt_slave_config_dc(sc, 0x0300, 1000000, 500000, 0, 0);
```

- ✅ 实机确认：SV630 支持 DC 64bit，`assign_activate=0x0300`（SYNC0，周期 1ms）即可稳定进 OP
  （`0x1c32:01=2` SYNC0、`:02=1000000`；`0x1c32:20=1` 要求必须 SYNC0，见 §7.3/§7.5）。
- 应用侧同样要跑 1ms 周期并调用 `ecrt_master_application_time()` / `ecrt_master_sync_reference_clock()`
  / `ecrt_master_sync_slave_clocks()`。
- 用 `ethercat master` 观察 "DC" 相关状态与 `Reference Clock` 是否选定。

---

## 6. 上电到 OP 的时序（对应原任务0）

```
1) ecrt_request_master(0)
2) ecrt_master_create_domain
3) ecrt_master_slave_config(0,0,0)            // 从站0
4) ecrt_slave_config_pdos(...)                // 默认或以 cstruct 为准
5) ecrt_slave_config_dc(...)                  // DC
6) ecrt_slave_config_watchdog(...)            // 驱动器看门狗(可选)
7) ecrt_domain_reg_pdo_entry_list(...)        // 绑定偏移
8) ecrt_master_activate(master)               // 进入运行，从站自动 PREOP->OP
9) 循环: ecrt_master_receive -> domain_process -> 读/写 -> queue -> send
```

对应关系：

| 原 ZBasic | IgH |
|-----------|-----|
| `slot_scan(0)` / `NODE_COUNT` | `ethercat slaves` / `ecrt_master_slave_config` |
| `SLOT_START(0)` | `ecrt_master_activate` |
| `DRIVE_CONTROLWORD` | RxPDO 的 `0x6040` 映射数据 |
| `AXIS_ENABLE/wdog` | CiA402 使能序列（见 04） |
| `MPOS/DPOS/IDLE` | TxPDO `0x6064/0x6062/0x6041.bit10` |
| `RAPIDSTOP(2)` | `0x6040.bit8(halt)` 或 `0x605A` |

---

## 7. ✅ 实测结论（M1 已完成，全部落定）

实测环境：鲁班猫2（RK3568 / Debian 12 / 内核 6.1.99-rk356x，非 RT）+ IgH EtherLab **1.6.13**
（`/usr/local` 前缀，`generic` 驱动，`eth0` 专用口）。验证脚本：`deploy/30-safeop-trace.sh`（SM/FMMU 追踪）、
`deploy/31-config-api-trace.sh`（配置 API 追踪）、`deploy/32-op-verify.sh`（**在线轮询 AL + 过程数据**）。

### 7.1 从站标识与能力

| 项 | 实测值 |
|----|--------|
| Vendor ID | `0x00100000` |
| Product Code | `0x000c0112` |
| Revision | `0x00010000` |
| 名称 | `SV630_1Axis_03716` / `InoSV630N` |
| 协议 | EoE, CoE；**Enable SDO Info: no**（→ `ethercat upload` 不可用，改自研 `tools/ecat_sdo.cpp`） |
| Enable SafeOp | no |
| Enable PDO Assign / Configuration | yes |
| DC | 支持，64 bit |

### 7.2 真实 PDO 映射（SDO 实读，非 SII 默认）

| SM | 对象 | 内容 | 总长 |
|----|------|------|------|
| SM2 (out, ctrl `0x64`) | `0x1600` | `0x6040:16` + `0x607a:32` + `0x6060:8` | **7 B** |
| SM3 (in, ctrl `0x20`) | `0x1a00` | `0x6041:16` + `0x6064:32` | **6 B** |

> ⚠️ SII 里登记的 SM2/SM3 默认长度是 **12 / 28**（与真实映射不符），
> 这就是"用 SII 默认映射"必然失败的根因之一。

### 7.3 同步与模式参数

| 对象 | 值 | 说明 |
|------|-----|------|
| `0x1c32:01` | `2` | DC SYNC0 |
| `0x1c32:02` | `1000000` | 周期 1 ms |
| `0x1c32:04` | `6` | 最小周期 |
| `0x1c32:20` | `1` | **SM2 同步错误（故意置位：从站要求 SYNC0 才允许 OP）** |
| `0x1c33:*` | 同构，`:20=0` | SM3 侧无此要求 |
| `0x6060` / `0x6061` | `8` / `8` | **CSP**（Cyclic Synchronous Position） |
| `0x603f` | `0xe08` | 最近错误码（历史遗留，未清） |
| `0x1000` | `0x00020192` | 设备类型 |

### 7.4 ★ 关键坑：`vendor_id/product_code` 必须精确匹配（AL `0x001E` 的根因）

- IgH `ec_slave_config_attach()`（`master/slave_config.c`）按 **vendor_id + product_code 精确相等** 匹配；
  **`0` 不是通配符**（除非编译时开 `EC_IDENT_WILDCARDS`）。
- 若配置里填 `0/0`：`attach` 失败 → `slave->config == NULL` →
  `ec_fsm_slave_config_enter_pdo_sync()` 走 `sync->default_length`（= SII 的 12/28），
  我们代码里 `ecrt_slave_config_pdos()` 写入的映射被**静默忽略** →
  从站报 **AL `0x001E` Invalid input configuration**，永远停在 PREOP，过程数据全 0。
- **表象极具误导性**：应用层日志显示 `ecrt_slave_config_pdo_mapping_add 0x1600:6040/607A/6060`
  全部成功（`deploy/31` 已证实），但内核写下的 SM 页仍是 12/28。

**修复**（已提交）：

```cpp
// src/common/config.h / src/motion/ethercat_master.h
uint32_t vendor_id    = 0x00100000;   // 汇川 SV630（0 会导致 attach 失败！）
uint32_t product_code = 0x000c0112;
```

```ini
# config/app.conf
ECAT_VENDOR           = 0x00100000
ECAT_PRODUCT          = 0x000c0112
ECAT_EXPLICIT_PDO     = 1
ECAT_EXPLICIT_PROFILE = 1
```

### 7.5 ✅ 复测结果（修复后）

`deploy/32-op-verify.sh 1 0x0300 12`（档位1 + DC）：

```
t=1s  PREOP
t=2s  SAFEOP
t=3s  OP      ← 进 OP 成功
...
t=7s  OP
[probe] t= 9.300s sw=0x1650 mode=n/a pos_actual=170 pos_demand=n/a 抖动=3
[probe] t=12.000s sw=0x1650 mode=n/a pos_actual=170 pos_demand=n/a 抖动=3
[probe] 结束：共 12000 周期，滞后周期=3
```

- 内核 SM 页已与真实映射一致：**`SM2: Size 7` / `SM3: Size 6`**（修复前是 12 / 28）。
- `0x6041` 状态字 = `0x1650` → CiA402 **Switch on disabled + Voltage enabled + Remote**
  （`ctrl_word` 固定 0 不放使能时的正常值）。
- `0x6064` 实际位置 = `170~174`（真实编码器反馈，非 0）。

`deploy/32-op-verify.sh 2 0x0300 12`（档位2 = +`0x6061` 模式回显）：

```
[ecat] PDO 偏移: cw=0 tp=2 mode=6 tv=n/a sw=7 pa=9 pd=n/a md=13
[probe] t=12.000s sw=0x1650 mode=8 pos_actual=172 pos_demand=n/a 抖动=0
```

- `mode=8` → `0x6061` 回显 **CSP**，与 `0x6060=8` 一致。
- **抖动=0**：非 RT 内核上 1 ms 周期零丢拍（12000/12000）。

### 7.6 观测纪律（重要）

**运行期间不要用 `ethercat slaves -v` 轮询**：读 SII 会抢走 PDI 的 SII 访问权，
把 OP 中的从站打回 PREOP，造成"假掉线"。测 AL 状态请用不带 `-v` 的 `ethercat slaves`
（`deploy/32-op-verify.sh` 已按此实现）。

### 7.7 ✅ M2 实测结论（原"待补测"已闭合）

- [x] 位置单位：`0x6091:01/02=1`（电子齿轮 1:1）；`0x6092/0x608F/0x6093/0x6094/0x60C2` **均不存在**（abort）→
  无法由对象反推用户单位，**保持 `INC_PER_MM=14043.41`**（=旧 `PULSE_EQUIV`，见 `04` §1.1）。
- [x] 使能序列 `0x80→0x06→0x07→0x0F`：`6041` 依次 `0x1650→0x1631→0x1633→0x1637`，**95ms 完成**；
  PP 定位 `+5mm` 实测 `4.9999mm`，`bit4/bit12/bit10` 到位判据成立（见 `04` §8）。
- [x] DC 依赖程度：`0x1c32:20=1` 明确**要求 SYNC0**，故本项目固定用 DC（未走"关 DC"分支）；
  `--dc 0` 仅用于激活自检。

### 7.8 停机 / 错误码（手册对照定案）

| 触发 | 决定参数 | 实读默认 | 结论 |
|------|----------|----------|------|
| `6040.bit8`（halt，旧 `RAPIDSTOP(2)`） | **`0x605D`** | `1` | 按**减速停机**（非急停），与 `0x605A` 无关 |
| `6040.bit2=0`（撤 Enable operation） | `0x605A` | `2` | 按减速曲线停 |
| 伺服 OFF | `0x605C` | `0` | 立即断使能 |
| 唯一错误码 | **`0x203F`**(Uint32) | `0` | `0x603F`(Uint16) **会重复**，不可作唯一键 |

> 急停响应实测：v0=1mm/s → **9.0ms**；v0=5mm/s → **13.0ms**（`deploy/42-m2-estop-time.sh`，
> 见 `07` §8.4 与手册 md §17.6）。
