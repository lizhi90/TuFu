// ethercat_master.h —— IgH EtherLab (ecrt) 主站封装：初始化 / PDO / DC / OP
// 说明：
//   * 单主站、单域、**多从站**（每轴一个 SV630），周期 1ms。
//   * 不依赖 ESI/XML：PDO 用「显式映射档位」（见 EthercatConfig::explicit_profile）。
//     实测（docs/planA/07 §4、deploy/23-dump-pdo.sh）：
//       - use_explicit_pdo=false（SII 默认）在注册阶段就会失败（IgH 未加载 SII 默认 PDO）；
//       - SV630 实际默认 PDO 为 SM2=0x1600{6040,607a,6060} / SM3=0x1a00{6041,6064}；
//         把 TxPDO 扩到 4 条会被从站拒绝（AL 0x001E Invalid input configuration）。
//   * 若某对象未映射，其偏移被置为 kOffsetInvalid，读前必须先判断。
//   * 本类只负责"搬运数据"，不解释 CiA402 语义（那是 cia402.* 的职责）。
//   * 多轴：轴 i 对应 EthercatConfig::slave_position[i] 处从站；各轴 PDO 偏移独立，
//     但共用同一个 domain（域内字节偏移由 IgH 分配，互不重叠）。每轴档案可不同（按各自身份匹配）。
#pragma once

#include <cstdint>
#include <cstddef>

#include "common/axis_limits.h"    // kAxisMax
#include "common/state.h"          // BusInfo：总线扫描快照（供脚本 NODE_* 查询）
#include "motion/drive_profile.h"

namespace kx {

// 显式 PDO 映射档位（use_explicit_pdo=true 时生效）
enum : int {
    PDO_PROFILE_MIN      = 1,  // SV630 实测默认：Rx{6040,607a,6060} Tx{6041,6064}
    PDO_PROFILE_MODEDISP = 2,  // Min + Tx 增 6061(模式回显)
    PDO_PROFILE_EXTENDED = 3,  // + Rx 60ff / Tx 6062,6061（需从站支持）
};

// 单个 PDO 条目的偏移（相对 domain 数据区起始），单位：字节
struct AxisPdoOffset {
    // RxPDO（主站 -> 从站）
    unsigned int ctrl_word   = 0;   // 0x6040:00 u16
    unsigned int target_pos  = 0;   // 0x607a:00 s32
    unsigned int mode        = 0;   // 0x6060:00 u8
    unsigned int target_vel  = 0;   // 0x60ff:00 s32
    // TxPDO（从站 -> 主站）
    unsigned int status_word = 0;   // 0x6041:00 u16
    unsigned int pos_actual  = 0;   // 0x6064:00 s32
    unsigned int pos_demand  = 0;   // 0x6062:00 s32
    unsigned int mode_disp   = 0;   // 0x6061:00 u8
};

// 从站身份（读自 SII；不需要进 OP，也没有 ESI 文件也能读）
struct EcatSlaveIdentity {
    bool     valid        = false;
    uint16_t alias        = 0;
    uint16_t position     = 0;
    uint32_t vendor_id    = 0;
    uint32_t product_code = 0;
    uint32_t revision     = 0;
    uint32_t serial       = 0;
    uint8_t  al_state     = 0;
    char     name[64]     = {0};   // SII OrderName / ProductName
};

// EtherCAT 主站配置的默认从站位置（轴 i -> 环上第 i 个从站）
struct EthercatConfig {
    unsigned int master_index   = 0;        // 主站序号（ecrt_request_master 参数）
    unsigned int axis_count     = 1;        // 参与循环的轴数（1..kAxisMax）
    unsigned int slave_alias    = 0;
    // 每轴从站位置（下标=轴号）。轴 i 默认对应环上第 i 个从站；
    // 由 main 从 EcatCfg::slave_position[] 拷入（支持 ECAT_SLAVE_POS_<n> 单轴覆盖）。
    unsigned int slave_position[kAxisMax];
    EthercatConfig() { for (int i = 0; i < kAxisMax; ++i) slave_position[i] = (unsigned int)i; }

    // IgH 的 ec_slave_config_attach() 按 vendor/product 精确匹配 SII，0 不是通配符；
    // 填 0 会导致 attach 失败、SM 长度退回 SII 默认，最终 AL 0x001E(Invalid input config)。
    // 若 auto_identity=true，init() 会用每个从站实读身份覆盖这两个值。
    uint32_t     vendor_id      = 0x00100000;  // 汇川 SV630
    uint32_t     product_code   = 0x000c0112;

    // ---- 驱动器档案（本模块的核心扩展点）----
    // nullptr  = 由 init() 读 SII 身份后自动匹配（推荐）
    // 非空指针 = 用户显式指定档案（ECAT_PROFILE=<name> / --profile-name），应用于所有轴
    const DriveProfile* profile = nullptr;
    bool         auto_identity     = true;   // 用总线实读 vid/pid 覆盖上面的默认值
    bool         profile_explicit  = false;  // true=用户明确点名，身份不符也照用（记警告）

    uint32_t     cycle_ns       = 1000000;  // 应用周期 1ms
    uint32_t     dc_assign      = 0x0300;   // SYNC0；试 0x0700 / 0 见文档 03
    uint32_t     dc_shift_ns    = 500000;   // SYNC0 偏移 = 周期一半
    bool         select_ref_clock = true;   // 选第一个 DC 从站为参考时钟

    bool         use_explicit_pdo = false;  // true=用代码内显式 PDO 映射（见 explicit_profile）
    int          explicit_profile = PDO_PROFILE_MIN;  // 0=用档案默认档位；否则强制该档位
    // 从站看门狗：divider/intervals（IgH 语义，单位见从站手册；两者都为 0 = 不配置）
    uint16_t     wd_divider   = 0;
    uint16_t     wd_intervals = 0;
};

class EthercatMaster {
public:
    // 未映射对象的偏移哨兵值；读/写前必须判断（否则越界读 domain）
    static const unsigned int kOffsetInvalid = 0xFFFFFFFFu;

    EthercatMaster() = default;
    ~EthercatMaster();
    EthercatMaster(const EthercatMaster&) = delete;
    EthercatMaster& operator=(const EthercatMaster&) = delete;

    // 申请主站 -> （逐轴）读身份(可选) -> 匹配档案 -> 从站配置(PDO/DC/WD) -> 绑定偏移
    //   -> 激活(进 OP)
    // 成功后每轴 PDO 偏移经 pdo_offset(i) 查询；轴 0 的偏移/档案同时写入 out_off / out_profile
    // （兼容单轴调用）。档案匹配失败（GENERIC/NONE）时：GENERIC 会打印醒目告警后继续，NONE 直接失败。
    bool init(const EthercatConfig& cfg, AxisPdoOffset* out_off = nullptr,
              const DriveProfile** out_profile = nullptr);

    // ---- 多轴查询（init 成功后有效）----
    unsigned axis_count() const { return axis_count_; }
    const AxisPdoOffset& pdo_offset(int axis) const;    // axis 越界 -> 返回轴 0 的副本（安全）
    const DriveProfile*  axis_profile(int axis) const;  // axis 越界 -> nullptr

    // 实际采用的档案（轴 0；init 成功后有效）
    const DriveProfile* profile() const { return axis_profile(0); }

    // ---- 从站身份读取（SII；不需要进 OP，也不需要 ESI 文件）----
    // 读已申请主站上的从站身份；master 为 ec_master_t*（传 nullptr 返回 false）。
    static bool read_slave_identity(void* master, uint16_t position, EcatSlaveIdentity* out);
    // 独立探测：临时申请主站 -> 读身份 -> 释放。用于「先识别、后决定策略」的场景
    // （例如调试软件连上就显示型号；或先看身份再决定是否值得初始化）。
    static bool probe_identity(unsigned master_index, uint16_t position, EcatSlaveIdentity* out);

    // ---- 总线扫描（供脚本 NODE_*/ETHERCAT 查询）----
    // 把当前实际上线的从站写入 out：node_count = 响应的从站数；逐节点读 SII 身份取 AL 状态。
    // axis_positions[0..n_axis-1] 为各轴从站位置：命中的节点 axis_count=1，其余为 0；
    // 无 IO/模拟量从站，故 NODE_IO/NODE_AIO/IN/OUT/AD/DA 计数恒 0。
    // 未申请主站或总线无响应从站时 node_count=0（脚本侧 NODE_* 即返回 0）。
    // 返回写入的节点数。注意：内部含主站 ioctl，调用频率应远低于 RT 周期（见 main.cpp 100ms 发布）。
    int scan_bus(const unsigned* axis_positions, unsigned n_axis, BusInfo* out) const;

    // 1ms 循环三步曲（由 RT 线程调用）
    void receive();          // ecrt_master_receive + domain_process
    void send();             // DC 同步(如需要) + domain_queue + ecrt_master_send
    void set_app_time(uint64_t app_time_ns);  // 喂 DC 应用时间（CLOCK_MONOTONIC 绝对值）

    // 总线健康状态（由 ecrt_master_state / 各轴 ecrt_slave_config_state 聚合）
    struct BusState {
        bool     link_up      = false;
        unsigned slaves_responding = 0;
        unsigned master_al    = 0;     // 主站视角 AL 状态
        int      axis_count   = 0;     // 参与聚合的轴数
        // 各轴从站 AL 状态 / 在线；ok() 要求全部轴在线且 OP
        int      axis_al[kAxisMax]     = {0};
        bool     axis_online[kAxisMax] = {false};
        unsigned slave_al     = 0;     // 聚合（取“最差”轴的 AL 状态，便于日志观察）
        bool     slave_online = false; // 全部轴在线
        bool     slave_op     = false; // 全部轴 OP(0x08)
        bool     ok() const { return link_up && slaves_responding > 0 && slave_online && slave_op; }
    };
    // 注意：应在 receive() 之后调用
    void read_state(BusState* out) const;

    // SDO 访问（会阻塞，**只能从非实时线程调用**）
    // 默认作用于轴 0（兼容旧调用）；_on 版本显式指定轴。
    // size 取 8/16/32 位；返回 false 时 abort_code 给出 SDO 中止码
    bool sdo_download_u32(uint16_t index, uint8_t subindex, uint32_t value,
                          size_t size_bits, uint32_t* abort_code = nullptr) {
        return sdo_download_u32_on(0, index, subindex, value, size_bits, abort_code);
    }
    bool sdo_upload_u32(uint16_t index, uint8_t subindex, uint32_t* value,
                        size_t size_bits, uint32_t* abort_code = nullptr) {
        return sdo_upload_u32_on(0, index, subindex, value, size_bits, abort_code);
    }
    bool sdo_download_u32_on(int axis, uint16_t index, uint8_t subindex, uint32_t value,
                             size_t size_bits, uint32_t* abort_code = nullptr);
    bool sdo_upload_u32_on(int axis, uint16_t index, uint8_t subindex, uint32_t* value,
                           size_t size_bits, uint32_t* abort_code = nullptr);

    // 访问域数据区
    uint8_t* data() const { return domain_data_; }
    unsigned int off(size_t i) const { return offsets_[0][i]; }   // 轴 0 的槽位偏移（兼容）

    // 读写辅助（小端，与 IgH PDO 字节序一致）
    uint8_t  read_u8 (unsigned int off) const;
    uint16_t read_u16(unsigned int off) const;
    int32_t  read_s32(unsigned int off) const;
    void     write_u8 (unsigned int off, uint8_t  v);
    void     write_u16(unsigned int off, uint16_t v);
    void     write_s32(unsigned int off, int32_t  v);

    bool activated() const { return master_ && active_; }

private:
    void release();

    void*        master_      = nullptr;  // ec_master_t*
    void*        domain_      = nullptr;  // ec_domain_t*
    uint8_t*     domain_data_ = nullptr;
    unsigned     axis_count_  = 0;        // 实际的轴数（<= cfg.axis_count 且 <= kAxisMax）
    void*        slave_cfg_[kAxisMax] = {nullptr};   // ec_slave_config_t*
    uint16_t     slave_position_[kAxisMax] = {0};    // SDO 寻址用（每轴）
    AxisPdoOffset pdo_off_[kAxisMax];                // 每轴 PDO 偏移
    const DriveProfile* profile_[kAxisMax] = {nullptr};  // 每轴实际采用的档案
    bool         active_      = false;
    bool         manual_dc_sync_ = false;  // 未选参考时钟时：由应用同步参考/从站时钟
    // 便于按序号取偏移：0=ctrl_word ... 7=mode_disp（每轴一份）
    unsigned int offsets_[kAxisMax][SLOT_COUNT] = {{0}};
};

} // namespace kx
