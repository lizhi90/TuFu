// ecat_probe.cpp —— 增量2 自检工具：只上总线、跑 1ms 循环、打印 PDO 实读值
// 目的：M1/M2 在板子上验证 PDO 偏移、DC 是否进 OP、状态字/位置是否在变。
// 不做使能（保持 ctrl_word=0），安全观察。
//
// 用法：
//   sudo ./ecat_probe [秒数] [--explicit] [--profile N] [--dc 0x0300] [--vid .. --pid ..]
// 例：
//   sudo ./ecat_probe 10                     # 默认映射 + DC 0x0300，跑 10s
//   sudo ./ecat_probe 10 --explicit          # 显式 PDO（档位1: SV630 默认 3+2）
//   sudo ./ecat_probe 10 --profile 2 --dc 0  # 档位2（+模式回显），关 DC
//   sudo ./ecat_probe 10 --extended --dc 0   # 档位3（扩展），关 DC
#include "../src/motion/ethercat_master.h"
#include "../src/motion/rt_util.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

using namespace kx;

static void usage(const char* a0) {
    std::printf("用法: %s [秒数=10] [--explicit] [--profile N] [--extended] "
                "[--dc 0x0300] [--vid 0x..] [--pid 0x..] [--pos N]\n"
                "      --explicit     显式 PDO 映射（默认档位1: SV630 3Rx+2Tx）\n"
                "      --profile N    档位 1=SV630默认 2=+模式回显 3=扩展\n"
                "      --extended     等价 --profile 3\n", a0);
}

int main(int argc, char** argv) {
    int    seconds      = 10;
    bool   explicit_pdo = false;
    int    profile      = 1;
    uint32_t dc         = 0x0300;
    uint32_t vid        = 0x00100000;   // 默认 SV630；--vid 可覆盖
    uint32_t pid        = 0x000c0112;   // 默认 SV630；--pid 可覆盖
    uint32_t position   = 0;

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--explicit")) explicit_pdo = true;
        else if (!std::strcmp(argv[i], "--extended")) { explicit_pdo = true; profile = 3; }
        else if (!std::strcmp(argv[i], "--profile") && i + 1 < argc) { profile = std::atoi(argv[++i]); explicit_pdo = true; }
        else if (!std::strcmp(argv[i], "--dc")   && i + 1 < argc) dc = (uint32_t)std::strtoul(argv[++i], nullptr, 0);
        else if (!std::strcmp(argv[i], "--vid")  && i + 1 < argc) vid = (uint32_t)std::strtoul(argv[++i], nullptr, 0);
        else if (!std::strcmp(argv[i], "--pid")  && i + 1 < argc) pid = (uint32_t)std::strtoul(argv[++i], nullptr, 0);
        else if (!std::strcmp(argv[i], "--pos")  && i + 1 < argc) position = (uint32_t)std::strtoul(argv[++i], nullptr, 0);
        else if (!std::strcmp(argv[i], "-h") || !std::strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        else if (argv[i][0] != '-') seconds = std::atoi(argv[i]);
    }

    // 绑核 CPU3 + SCHED_FIFO 80（非 root 时仅告警，继续跑）
    rt_setup(3, 80);
    DmaLatencyGuard dma;

    EthercatConfig cfg;
    cfg.master_index     = 0;
    ::memset(cfg.slave_position, 0, sizeof(cfg.slave_position));   // 单轴观察工具：轴 0 -> 环上第 0 个从站
    cfg.slave_position[0] = position;
    cfg.vendor_id        = vid;
    cfg.product_code     = pid;
    cfg.cycle_ns         = 1000000;
    cfg.dc_assign        = dc;
    cfg.dc_shift_ns      = 500000;
    cfg.use_explicit_pdo = explicit_pdo;
    cfg.explicit_profile = profile;
    cfg.select_ref_clock = (dc != 0);
    cfg.wd_divider       = 0;
    cfg.wd_intervals     = 0;

    EthercatMaster ec;
    AxisPdoOffset off;
    try {
        if (!ec.init(cfg, &off)) {
            std::fprintf(stderr, "[probe] 初始化失败，见上方日志\n");
            return 1;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[probe] 异常: %s\n", e.what());
        return 1;
    }

    std::printf("[probe] 进入 1ms 循环 %d 秒（不放使能，ctrl_word 固定 0）...\n", seconds);
    long long t0     = mono_ns();
    long long wake   = t0 + cfg.cycle_ns;
    long long period = cfg.cycle_ns;
    long long count  = 0;
    long long over   = 0;

    while (true) {
        sleep_until(wake);

        long long now = mono_ns();
        if (dc != 0) {
            // DC 应用时间：CLOCK_MONOTONIC 绝对值（与 IgH dc_user 示例一致）
            ec.set_app_time((uint64_t)now);
        }
        ec.receive();

        // 保持控制字为 0（不使能），仅观察 TxPDO
        if (off.ctrl_word != EthercatMaster::kOffsetInvalid)
            ec.write_u16(off.ctrl_word, 0x0000);

        ec.send();

        if (now - wake > period) ++over;
        ++count;

        if (count % 100 == 0) {
            const unsigned int BAD = EthercatMaster::kOffsetInvalid;
            auto u16 = [&](unsigned int o) -> unsigned { return o == BAD ? 0u : ec.read_u16(o); };
            auto s32 = [&](unsigned int o) -> long  { return o == BAD ? 0L : (long)ec.read_s32(o); };
            char pds[12], mds[12];
            if (off.pos_demand == BAD) std::snprintf(pds, sizeof pds, "n/a");
            else                       std::snprintf(pds, sizeof pds, "%ld", s32(off.pos_demand));
            if (off.mode_disp  == BAD) std::snprintf(mds, sizeof mds, "n/a");
            else                       std::snprintf(mds, sizeof mds, "%u",  u16(off.mode_disp));
            std::printf("[probe] t=%6.3fs sw=0x%04x mode=%s pos_actual=%ld pos_demand=%s 抖动=%lld\n",
                        (double)(now - t0) / 1e9, u16(off.status_word), mds,
                        s32(off.pos_actual), pds, over);
        }

        wake += period;
        if (now - t0 >= (long long)seconds * 1000000000LL) break;
    }

    std::printf("[probe] 结束：共 %lld 周期，滞后周期=%lld\n", count, over);
    return 0;
}
