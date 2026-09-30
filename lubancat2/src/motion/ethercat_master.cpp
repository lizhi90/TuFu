// ethercat_master.cpp —— IgH EtherLab (ecrt) 主站封装实现
#include "ethercat_master.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>

#include <ecrt.h>

namespace kx {

// ---------------------------------------------------------------------------
// 档案 -> ecrt 结构的适配（本文件里唯一与 ecrt 打交道的地方）
//
// PDO 布局与「哪个对象绑到哪个偏移槽位」现在都来自 DriveProfile（见 drive_profile.h），
// 这里是纯机械转换，不再含任何厂商常量。
// ---------------------------------------------------------------------------
constexpr unsigned kMaxPdoEntries = 16;

// 取档位：want 为 0 时用档案默认档位；越界则夹到合法范围
const PdoProfile* pick_gear(const DriveProfile& p, int want_1based) {
    if (p.n_pdos == 0) return nullptr;
    int g = want_1based;
    if (g < 1) g = p.default_gear;
    if (g < 1 || (unsigned)g > p.n_pdos) g = 1;
    return &p.pdos[g - 1];
}

// PdoEntry[] -> ec_pdo_entry_info_t[]（布局一致，显式逐字段拷贝更稳妥）
unsigned to_ecrt_entries(const PdoProfile& g, ec_pdo_entry_info_t* out, unsigned cap) {
    unsigned n = g.n_rx + g.n_tx;
    if (n > cap) n = cap;
    for (unsigned i = 0; i < n; ++i) {
        out[i].index      = g.entries[i].index;
        out[i].subindex   = g.entries[i].subindex;
        out[i].bit_length = g.entries[i].bits;
    }
    return n;
}

// 醒目告警（档案不是精确命中时用；绝不静默）
void warn_profile(const char* tag, const char* detail) {
    std::fprintf(stderr,
                 "\n"
                 "  ############################################################\n"
                 "  # [%s] 驱动器档案未精确匹配\n"
                 "  #   %s\n"
                 "  #   如非刻意，请核对从站型号，或用 ECAT_PROFILE=<档案名> 指定。\n"
                 "  ############################################################\n\n",
                 tag, detail);
}

// ---------------------------------------------------------------------------

EthercatMaster::~EthercatMaster() { release(); }

void EthercatMaster::release() {
    if (master_) {
        ecrt_release_master(static_cast<ec_master_t*>(master_));
        master_ = nullptr;
    }
    domain_         = nullptr;
    domain_data_    = nullptr;
    active_         = false;
    axis_count_     = 0;
    manual_dc_sync_ = false;
    for (int i = 0; i < kAxisMax; ++i) {
        slave_cfg_[i]      = nullptr;
        slave_position_[i] = 0;
        profile_[i]        = nullptr;
        pdo_off_[i]        = AxisPdoOffset{};
        for (int j = 0; j < SLOT_COUNT; ++j) offsets_[i][j] = 0;
    }
}

bool EthercatMaster::read_slave_identity(void* master, uint16_t position,
                                         EcatSlaveIdentity* out) {
    if (!master || !out) return false;
    *out = EcatSlaveIdentity{};

    ec_slave_info_t info;
    std::memset(&info, 0, sizeof(info));
    // 注意：IgH 1.6 的 ecrt_master_get_slave 按「环上绝对位置」索引（含所有从站）。
    if (ecrt_master_get_slave(static_cast<ec_master_t*>(master), position, &info) != 0) {
        return false;
    }
    out->valid        = true;
    out->alias        = info.alias;
    out->position     = info.position;
    out->vendor_id    = info.vendor_id;
    out->product_code = info.product_code;
    out->revision     = info.revision_number;
    out->serial       = info.serial_number;
    out->al_state     = (uint8_t)info.al_state;
    // IgH 的 name 字段就是 SII 里读到的 OrderName/ProductName（可能为空）
    std::snprintf(out->name, sizeof(out->name), "%s", info.name);
    return true;
}

bool EthercatMaster::probe_identity(unsigned master_index, uint16_t position,
                                    EcatSlaveIdentity* out) {
    auto* master = ecrt_request_master(master_index);
    if (!master) {
        std::fprintf(stderr, "[ecat] probe: ecrt_request_master(%u) 失败\n", master_index);
        return false;
    }
    const bool ok = read_slave_identity(master, position, out);
    ecrt_release_master(master);
    if (!ok) {
        std::fprintf(stderr, "[ecat] probe: 读不到位置 %u 的从站身份（环上无此从站？）\n", position);
    }
    return ok;
}

int EthercatMaster::scan_bus(const unsigned* axis_positions, unsigned n_axis,
                             BusInfo* out) const {
    if (!out) return 0;
    *out = BusInfo{};
    if (!master_) return 0;

    // 当前主站视角：link_up / 响应从站数 / AL 位图 + 各轴从站 AL
    BusState bs;
    read_state(&bs);

    out->link_up          = bs.link_up ? 1 : 0;              // D10 主站健康
    out->slaves_responding = (int)bs.slaves_responding;
    out->master_al        = (int)bs.master_al;
    out->slave_al         = (int)bs.slave_al;
    out->slave_online     = bs.slave_online ? 1 : 0;
    out->slave_op         = bs.slave_op ? 1 : 0;

    const unsigned total = bs.slaves_responding;
    const int cnt = (int)(total < (unsigned)kBusNodeMax ? total : (unsigned)kBusNodeMax);
    out->node_count = cnt;

    for (int i = 0; i < cnt; ++i) {
        BusNodeInfo& n = out->node[i];
        // 该节点是否属于某一轴（轴从站位置匹配）
        int axis_idx = -1;
        for (unsigned a = 0; a < n_axis; ++a) {
            if (axis_positions && (uint16_t)i == (uint16_t)axis_positions[a]) {
                axis_idx = (int)a;
                break;
            }
        }
        // 逐节点读 SII 身份取 AL 状态（非阻塞 ioctl）；读不到时退回该轴的主站视角 AL
        EcatSlaveIdentity id;
        const bool have_id = read_slave_identity(master_, (uint16_t)i, &id) && id.valid;
        if (have_id) {
            n.status = (int)id.al_state;
        } else if (axis_idx >= 0 && axis_idx < bs.axis_count) {
            n.status = bs.axis_al[axis_idx];
        }
        // 只有配置的轴从站带 1 个轴；IO/AD-DA 计数在本控制器恒 0
        if (axis_idx >= 0) n.axis_count = 1;

        // D10：从站明细（身份/在线/AL/轴映射）——通讯状态面板
        n.axis     = axis_idx;
        n.al_state = n.status;
        n.online   = (axis_idx >= 0 && axis_idx < bs.axis_count)
                         ? (bs.axis_online[axis_idx] ? 1 : 0)
                         : (have_id ? 1 : 0);
        if (have_id) {
            n.vendor_id    = id.vendor_id;
            n.product_code = id.product_code;
            n.revision     = id.revision;
            std::strncpy(n.name, id.name, sizeof(n.name) - 1);
        }
    }
    return cnt;
}

bool EthercatMaster::init(const EthercatConfig& cfg_in, AxisPdoOffset* out_off,
                          const DriveProfile** out_profile) {
    EthercatConfig cfg = cfg_in;   // 身份可能被总线实读值覆盖，故用可写副本

    unsigned n_axis = cfg.axis_count;
    if (n_axis < 1) n_axis = 1;
    if (n_axis > (unsigned)kAxisMax) n_axis = (unsigned)kAxisMax;

    auto* master = ecrt_request_master(cfg.master_index);
    if (!master) {
        std::fprintf(stderr, "[ecat] ecrt_request_master(%u) 失败：主站是否存在/已加载?\n",
                     cfg.master_index);
        return false;
    }
    master_     = master;
    axis_count_ = n_axis;
    for (unsigned i = 0; i < n_axis; ++i) {
        slave_position_[i] = (uint16_t)cfg.slave_position[i];
    }

    // ---- 0) 逐轴读 SII 身份 + 决定各轴用哪份驱动器档案 ----
    uint32_t vid[kAxisMax], pid[kAxisMax];
    for (unsigned i = 0; i < n_axis; ++i) {
        vid[i] = cfg.vendor_id;
        pid[i] = cfg.product_code;

        EcatSlaveIdentity ident;
        const bool have_ident = read_slave_identity(master, slave_position_[i], &ident);
        const DriveProfile* prof = cfg.profile;   // nullptr = 按身份自动匹配

        if (have_ident) {
            std::printf("[ecat] 轴%u 从站#%u 身份: vid=0x%08x pid=0x%08x rev=0x%08x alias=%u name='%s'\n",
                        i, ident.position, ident.vendor_id, ident.product_code, ident.revision,
                        ident.alias, ident.name);
            if (cfg.auto_identity) {
                // 用实读身份覆盖配置默认值：IgH 的 attach 要求与 SII 精确相等
                if (ident.vendor_id != vid[i] || ident.product_code != pid[i]) {
                    std::printf("[ecat] 轴%u 用实读身份覆盖配置 vid/pid"
                                "（0x%08x/0x%08x -> 0x%08x/0x%08x）\n",
                                i, vid[i], pid[i], ident.vendor_id, ident.product_code);
                }
                vid[i] = ident.vendor_id;
                pid[i] = ident.product_code;
            }
            if (!prof) {
                const MatchResult mr = profile_match(ident.vendor_id, ident.product_code,
                                                     ident.revision,
                                                     ident.name[0] ? ident.name : nullptr);
                prof = mr.profile;
                if (mr.kind == ProfileMatch::EXACT) {
                    std::printf("[ecat] 轴%u 自动识别: %s\n", i, mr.detail);
                } else if (mr.kind == ProfileMatch::ID_ONLY) {
                    warn_profile("auto", mr.detail);
                } else if (mr.kind == ProfileMatch::GENERIC) {
                    warn_profile("auto", mr.detail);
                }
            }
        } else {
            std::fprintf(stderr, "[ecat] 警告: 轴%u 读不到从站 SII 身份（位置 %u）\n",
                         i, slave_position_[i]);
            if (!prof) {
                prof = ProfileRegistry::instance().fallback();
                if (prof) warn_profile("no-sii", "读不到 SII 身份，已回退通用档案（映射大概率不符，务必人工确认）");
            }
        }

        if (!prof) {
            std::fprintf(stderr,
                         "[ecat] 轴%u 无法确定驱动器档案（无匹配且无 generic 兜底），拒绝猜测映射，退出\n", i);
            release();
            return false;
        }

        // 用户明确点名/自动选中后，与实读身份做一次交叉校验（不阻止，只告警）
        if (have_ident && prof->vendor_id != 0 &&
            (prof->vendor_id != ident.vendor_id || prof->product_code != ident.product_code)) {
            char msg[224];
            std::snprintf(msg, sizeof(msg),
                          "轴%u 指定档案 '%s' 的 vid/pid=0x%08x/0x%08x 与总线实读 0x%08x/0x%08x 不符",
                          i, prof->name, prof->vendor_id, prof->product_code,
                          ident.vendor_id, ident.product_code);
            if (cfg.profile_explicit) warn_profile("override", msg);
            else                      warn_profile("mismatch", msg);
        }
        profile_[i] = prof;
    }
    if (out_profile) *out_profile = axis_profile(0);

    auto* domain = ecrt_master_create_domain(master);
    if (!domain) {
        std::fprintf(stderr, "[ecat] ecrt_master_create_domain 失败\n");
        release();
        return false;
    }
    domain_ = domain;

    // ---- 1) 逐轴：从站配置 + PDO 映射 + DC + 看门狗 ----
    const PdoProfile* gears[kAxisMax] = {nullptr};
    for (unsigned i = 0; i < n_axis; ++i) {
        const DriveProfile* prof = profile_[i];

        auto* sc = ecrt_master_slave_config(master, cfg.slave_alias, slave_position_[i],
                                            vid[i], pid[i]);
        if (!sc) {
            std::fprintf(stderr,
                         "[ecat] 轴%u slave_config(alias=%u pos=%u vid=0x%08x pid=0x%08x) 失败\n",
                         i, cfg.slave_alias, slave_position_[i], vid[i], pid[i]);
            release();
            return false;
        }
        slave_cfg_[i] = sc;

        // PDO 映射：档位来自档案（explicit_profile>0 可临时覆盖）
        const PdoProfile* gear = pick_gear(*prof, cfg.explicit_profile);
        if (!gear) {
            std::fprintf(stderr, "[ecat] 轴%u 档案 '%s' 没有任何 PDO 档位\n", i, prof->name);
            release();
            return false;
        }
        gears[i] = gear;
        const int gear_no = (int)(gear - prof->pdos) + 1;

        bool use_explicit = cfg.use_explicit_pdo;
        if (!use_explicit && !prof->allow_sii_default_pdo) {
            // 档案明说「不能靠 SII 默认」（如 SV630：IgH 未加载 SII 默认 PDO，注册阶段就会失败）
            std::fprintf(stderr,
                         "[ecat] 轴%u 档案 '%s' 不支持走 SII 默认 PDO，自动改用显式映射（档位%d）\n",
                         i, prof->name, gear_no);
            use_explicit = true;
        }

        if (use_explicit) {
            // 注意：entries 须在 ecrt_slave_config_pdos 返回前保持有效
            ec_pdo_entry_info_t entries[kMaxPdoEntries];
            const unsigned n = to_ecrt_entries(*gear, entries, kMaxPdoEntries);
            if (n < gear->n_rx + gear->n_tx) {
                std::fprintf(stderr, "[ecat] 轴%u 档位条目过多（%u > %u），已截断\n",
                             i, gear->n_rx + gear->n_tx, kMaxPdoEntries);
            }
            ec_pdo_info_t pdos[2] = {
                {0x1600, gear->n_rx, entries},
                {0x1a00, gear->n_tx, entries + gear->n_rx},
            };
            ec_sync_info_t syncs[5] = {
                {0,    EC_DIR_OUTPUT,  0, nullptr,  EC_WD_DISABLE},  // SM0 out (mailbox)
                {1,    EC_DIR_INPUT,   0, nullptr,  EC_WD_DISABLE},  // SM1 in  (mailbox)
                {2,    EC_DIR_OUTPUT,  1, &pdos[0], EC_WD_ENABLE},   // SM2 out (RxPDO)
                {3,    EC_DIR_INPUT,   1, &pdos[1], EC_WD_DISABLE},  // SM3 in  (TxPDO)
                {0xff, EC_DIR_INVALID, 0, nullptr,  EC_WD_DEFAULT},  // 结束标记
            };
            if (ecrt_slave_config_pdos(sc, EC_END, syncs) != 0) {
                std::fprintf(stderr,
                             "[ecat] 轴%u ecrt_slave_config_pdos 失败（档案 %s 档位%d: %s）\n",
                             i, prof->name, gear_no, gear->name);
                release();
                return false;
            }
            std::printf("[ecat] 轴%u 显式 PDO 映射 档案=%s 档位%d: %s（Rx %u 条 / Tx %u 条）\n",
                        i, prof->name, gear_no, gear->name, gear->n_rx, gear->n_tx);
        } else {
            std::printf("[ecat] 轴%u 使用从站 SII 默认 PDO 映射（档案 %s；实测易在注册阶段失败）\n",
                        i, prof->name);
        }

        // 分布式时钟
        if (cfg.dc_assign != 0) {
            if (ecrt_slave_config_dc(sc, cfg.dc_assign, cfg.cycle_ns, cfg.dc_shift_ns, 0, 0) != 0) {
                std::fprintf(stderr, "[ecat] 轴%u ecrt_slave_config_dc(assign=0x%04x) 失败\n",
                             i, cfg.dc_assign);
                release();
                return false;
            }
            std::printf("[ecat] 轴%u DC 已配置 assign=0x%04x cycle=%uns shift=%uns\n",
                        i, cfg.dc_assign, cfg.cycle_ns, cfg.dc_shift_ns);
        } else if (i == 0) {
            std::printf("[ecat] DC 未启用（assign=0，基础链路验证用）\n");
        }

        // 从站看门狗（可选；0/0 = 用从站默认）
        if (cfg.wd_divider > 0 || cfg.wd_intervals > 0) {
            if (ecrt_slave_config_watchdog(sc, cfg.wd_divider, cfg.wd_intervals) != 0) {
                std::fprintf(stderr, "[ecat] 轴%u ecrt_slave_config_watchdog 失败（继续）\n", i);
            }
        }
    }

    // ---- 2) 逐轴逐槽绑定 PDO 偏移：未映射槽位先置无效哨兵，注册成功的才被 IgH 覆盖 ----
    for (unsigned i = 0; i < n_axis; ++i) {
        for (int j = 0; j < SLOT_COUNT; ++j) offsets_[i][j] = kOffsetInvalid;
    }

    // 注册表来自各轴档案档位的 slots（对象 -> 槽位）；末项全零作为结束标志
    ec_pdo_entry_reg_t regs[(unsigned)kAxisMax * SLOT_COUNT + 1];
    std::memset(regs, 0, sizeof(regs));
    unsigned k = 0;
    for (unsigned i = 0; i < n_axis; ++i) {
        const PdoProfile* gear = gears[i];
        const unsigned nspec = gear->n_slots < SLOT_COUNT ? gear->n_slots : SLOT_COUNT;
        for (unsigned j = 0; j < nspec; ++j) {
            regs[k].alias        = cfg.slave_alias;
            regs[k].position     = slave_position_[i];
            regs[k].vendor_id    = vid[i];
            regs[k].product_code = pid[i];
            regs[k].index        = gear->slots[j].index;
            regs[k].subindex     = gear->slots[j].subindex;
            regs[k].offset       = &offsets_[i][gear->slots[j].slot];
            regs[k].bit_position = nullptr;
            ++k;
        }
    }

    if (ecrt_domain_reg_pdo_entry_list(domain, regs) != 0) {
        std::fprintf(stderr,
                     "[ecat] 注册 PDO 条目失败（共 %u 条 / %u 轴）：映射与从站实际不符。\n"
                     "       请用 deploy/23-dump-pdo.sh 核对 cstruct；若为同 vid/pid 的不同固件，\n"
                     "       可试 ECAT_EXPLICIT_PROFILE=2/3，或为该型号新增一份档案。\n",
                     k, n_axis);
        release();
        return false;
    }

    // 激活（从站开始 PREOP->OP；OP 失败不阻塞，状态由状态字体现）
    if (ecrt_master_activate(master) != 0) {
        std::fprintf(stderr, "[ecat] ecrt_master_activate 失败\n");
        release();
        return false;
    }
    active_ = true;

    domain_data_ = ecrt_domain_data(domain);
    if (!domain_data_) {
        std::fprintf(stderr, "[ecat] ecrt_domain_data 返回空\n");
        release();
        return false;
    }

    // 导出各轴偏移（轴 0 同时写入 out_off，兼容单轴调用）
    auto show = [](unsigned int o, char* buf, size_t n) {
        if (o == kOffsetInvalid) std::snprintf(buf, n, "n/a");
        else                     std::snprintf(buf, n, "%u", o);
    };
    for (unsigned i = 0; i < n_axis; ++i) {
        AxisPdoOffset& o = pdo_off_[i];
        o.ctrl_word   = offsets_[i][SLOT_CTRL_WORD];
        o.target_pos  = offsets_[i][SLOT_TARGET_POS];
        o.mode        = offsets_[i][SLOT_MODE];
        o.target_vel  = offsets_[i][SLOT_TARGET_VEL];
        o.status_word = offsets_[i][SLOT_STATUS_WORD];
        o.pos_actual  = offsets_[i][SLOT_POS_ACTUAL];
        o.pos_demand  = offsets_[i][SLOT_POS_DEMAND];
        o.mode_disp   = offsets_[i][SLOT_MODE_DISP];

        char b_cw[16], b_tp[16], b_md[16], b_tv[16], b_sw[16], b_pa[16], b_pd[16], b_mi[16];
        show(o.ctrl_word,  b_cw, sizeof(b_cw));
        show(o.target_pos, b_tp, sizeof(b_tp));
        show(o.mode,       b_md, sizeof(b_md));
        show(o.target_vel, b_tv, sizeof(b_tv));
        show(o.status_word,b_sw, sizeof(b_sw));
        show(o.pos_actual, b_pa, sizeof(b_pa));
        show(o.pos_demand, b_pd, sizeof(b_pd));
        show(o.mode_disp,  b_mi, sizeof(b_mi));
        std::printf("[ecat] 轴%u PDO 偏移: cw=%s tp=%s mode=%s tv=%s sw=%s pa=%s pd=%s md=%s\n",
                    i, b_cw, b_tp, b_md, b_tv, b_sw, b_pa, b_pd, b_mi);
    }
    if (out_off) *out_off = pdo_off_[0];
    std::printf("[ecat] 已激活（%u 轴）\n", n_axis);

    // 选参考时钟（第一个支持 DC 的从站）
    if (cfg.dc_assign != 0) {
        if (cfg.select_ref_clock) {
            ecrt_master_select_reference_clock(master,
                static_cast<ec_slave_config_t*>(slave_cfg_[0]));
            manual_dc_sync_ = false;   // 主站自动同步从站时钟
            std::printf("[ecat] 已选定参考时钟（轴0；主站自动同步从站时钟）\n");
        } else {
            manual_dc_sync_ = true;    // 应用在 send() 中手动同步
            std::printf("[ecat] 未选参考时钟：send() 内将调用 sync_reference/slave_clocks\n");
        }
    }
    return true;
}

void EthercatMaster::set_app_time(uint64_t app_time_ns) {
    if (!master_) return;
    ecrt_master_application_time(static_cast<ec_master_t*>(master_), app_time_ns);
}

void EthercatMaster::receive() {
    if (!master_ || !domain_) return;
    ecrt_master_receive(static_cast<ec_master_t*>(master_));
    ecrt_domain_process(static_cast<ec_domain_t*>(domain_));
}

void EthercatMaster::send() {
    if (!master_ || !domain_) return;
    auto* master = static_cast<ec_master_t*>(master_);
    if (manual_dc_sync_) {
        ecrt_master_sync_reference_clock(master);
        ecrt_master_sync_slave_clocks(master);
    }
    ecrt_domain_queue(static_cast<ec_domain_t*>(domain_));
    ecrt_master_send(master);
}

// ---- 多轴查询 ----
const AxisPdoOffset& EthercatMaster::pdo_offset(int axis) const {
    if (axis < 0 || axis >= (int)axis_count_) return pdo_off_[0];
    return pdo_off_[axis];
}

const DriveProfile* EthercatMaster::axis_profile(int axis) const {
    if (axis < 0 || axis >= (int)axis_count_) return nullptr;
    return profile_[axis];
}

void EthercatMaster::read_state(BusState* out) const {
    if (!out) return;
    *out = BusState{};
    if (!master_) return;

    ec_master_state_t ms;
    std::memset(&ms, 0, sizeof(ms));
    ecrt_master_state(static_cast<ec_master_t*>(master_), &ms);
    out->link_up           = ms.link_up;
    out->slaves_responding = ms.slaves_responding;
    out->master_al         = ms.al_states;
    out->axis_count        = (int)axis_count_;

    if (axis_count_ == 0) return;

    bool all_online = true;
    bool all_op     = true;
    unsigned worst  = 0xFFu;   // AL 状态数值越小越靠前/越差（INIT=1 < PREOP=2 < SAFEOP=4 < OP=8）
    for (unsigned i = 0; i < axis_count_; ++i) {
        int al = 0;
        bool online = false;
        if (slave_cfg_[i]) {
            ec_slave_config_state_t ss;
            std::memset(&ss, 0, sizeof(ss));
            ecrt_slave_config_state(static_cast<ec_slave_config_t*>(slave_cfg_[i]), &ss);
            al     = (int)ss.al_state;
            online = (ss.online != 0);
        }
        out->axis_al[i]     = al;
        out->axis_online[i] = online;
        all_online = all_online && online;
        all_op     = all_op && (al == 0x08);   // EC_AL_STATE_OP
        if ((unsigned)al < worst) worst = (unsigned)al;
    }
    if (worst == 0xFFu) worst = 0;
    out->slave_al     = worst;
    out->slave_online = all_online;
    out->slave_op     = all_op;
}

bool EthercatMaster::sdo_download_u32_on(int axis, uint16_t index, uint8_t subindex,
                                         uint32_t value, size_t size_bits, uint32_t* abort_code) {
    if (!master_ || axis < 0 || axis >= (int)axis_count_) return false;
    uint8_t buf[4];
    const size_t nbytes = (size_bits + 7) / 8;
    std::memcpy(buf, &value, nbytes > 4 ? 4 : nbytes);
    uint32_t abort = 0;
    const int rc = ecrt_master_sdo_download(static_cast<ec_master_t*>(master_),
                                            slave_position_[axis],
                                            index, subindex, buf, nbytes, &abort);
    if (abort_code) *abort_code = abort;
    if (rc != 0) {
        std::fprintf(stderr, "[ecat] 轴%d SDO 下载失败 0x%04x:%02x size=%zu abort=0x%08x\n",
                     axis, index, subindex, size_bits, abort);
    }
    return rc == 0;
}

bool EthercatMaster::sdo_upload_u32_on(int axis, uint16_t index, uint8_t subindex,
                                       uint32_t* value, size_t size_bits, uint32_t* abort_code) {
    if (!master_ || !value || axis < 0 || axis >= (int)axis_count_) return false;
    uint8_t buf[4] = {0, 0, 0, 0};
    const size_t nbytes = (size_bits + 7) / 8;
    size_t out_size = nbytes > 4 ? 4 : nbytes;
    uint32_t abort = 0;
    const int rc = ecrt_master_sdo_upload(static_cast<ec_master_t*>(master_),
                                          slave_position_[axis],
                                          index, subindex, buf, sizeof(buf), &out_size, &abort);
    if (abort_code) *abort_code = abort;
    if (rc == 0) {
        uint32_t v = 0;
        std::memcpy(&v, buf, out_size > 4 ? 4 : out_size);
        *value = v;
    }
    return rc == 0;
}

uint8_t EthercatMaster::read_u8(unsigned int off) const {
    return domain_data_ ? domain_data_[off] : 0;
}
uint16_t EthercatMaster::read_u16(unsigned int off) const {
    return domain_data_ ? (uint16_t)(domain_data_[off] | (domain_data_[off + 1] << 8)) : 0;
}
int32_t EthercatMaster::read_s32(unsigned int off) const {
    if (!domain_data_) return 0;
    uint32_t v = (uint32_t)domain_data_[off]
               | ((uint32_t)domain_data_[off + 1] << 8)
               | ((uint32_t)domain_data_[off + 2] << 16)
               | ((uint32_t)domain_data_[off + 3] << 24);
    return (int32_t)v;
}
void EthercatMaster::write_u8(unsigned int off, uint8_t v) {
    if (domain_data_) domain_data_[off] = v;
}
void EthercatMaster::write_u16(unsigned int off, uint16_t v) {
    if (!domain_data_) return;
    domain_data_[off]     = (uint8_t)(v & 0xff);
    domain_data_[off + 1] = (uint8_t)(v >> 8);
}
void EthercatMaster::write_s32(unsigned int off, int32_t v) {
    if (!domain_data_) return;
    uint32_t u = (uint32_t)v;
    domain_data_[off]     = (uint8_t)(u & 0xff);
    domain_data_[off + 1] = (uint8_t)((u >> 8) & 0xff);
    domain_data_[off + 2] = (uint8_t)((u >> 16) & 0xff);
    domain_data_[off + 3] = (uint8_t)((u >> 24) & 0xff);
}

} // namespace kx
