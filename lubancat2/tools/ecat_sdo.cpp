// ecat_sdo.cpp —— 主站空闲态下用 SDO 直读从站对象字典（诊断 PDO 配置 / 0x001E）
// 为什么不用 `ethercat upload`：SV630 关闭了 CoE SDO Information（Enable SDO Info: no），
// 该命令无法自动判类型；本工具直接指定位宽上传，绕开 SDO Info。
//
// 用法：
//   sudo ./ecat_sdo                     # 打印内置诊断清单（PDO 分配/映射/SM 参数/6060/6061）
//   sudo ./ecat_sdo --slave 0
//   sudo ./ecat_sdo --get 0x1a00:00:8   # 任意读：index:subindex:bits
//   sudo ./ecat_sdo --quiet --get 0x6041:00:16
//   sudo ./ecat_sdo --set 0x6060:00:8=8 # 任意写（写入后自动回读校验）
//   sudo ./ecat_sdo --get 0x6061:00:8 --set 0x6060:00:8=8   # 先写全部，再统一回读
//
// 注意：运行期间不能有别的应用占用主站（先确保 ecat_probe / kine-x 已退出）。
// ⚠ 写入会改驱动器对象（H0E.01=3 时可能写 EEPROM）；只改明确知道的参数，改完记得回读。
#include <ecrt.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

struct Req {
    uint16_t    idx;
    uint8_t     sub;
    unsigned    bits;
    const char* name;
};

struct SetReq {
    uint16_t idx;
    uint8_t  sub;
    unsigned bits;
    uint64_t val;
};

// 内置诊断清单：先看 PDO 分配与映射，再看 SM 同步参数
const Req kList[] = {
    {0x1C12, 0x00,  8, "SM2 PDO 分配条数 (0x1C12:00)"},
    {0x1C12, 0x01, 16, "SM2 PDO 分配 [1]    (0x1C12:01)"},
    {0x1C12, 0x02, 16, "SM2 PDO 分配 [2]    (0x1C12:02)"},
    {0x1C13, 0x00,  8, "SM3 PDO 分配条数 (0x1C13:00)"},
    {0x1C13, 0x01, 16, "SM3 PDO 分配 [1]    (0x1C13:01)"},
    {0x1C13, 0x02, 16, "SM3 PDO 分配 [2]    (0x1C13:02)"},
    {0x1600, 0x00,  8, "RxPDO 0x1600 条目数"},
    {0x1600, 0x01, 32, "RxPDO 0x1600 [1]"},
    {0x1600, 0x02, 32, "RxPDO 0x1600 [2]"},
    {0x1600, 0x03, 32, "RxPDO 0x1600 [3]"},
    {0x1600, 0x04, 32, "RxPDO 0x1600 [4]"},
    {0x1600, 0x05, 32, "RxPDO 0x1600 [5]"},
    {0x1A00, 0x00,  8, "TxPDO 0x1A00 条目数"},
    {0x1A00, 0x01, 32, "TxPDO 0x1A00 [1]"},
    {0x1A00, 0x02, 32, "TxPDO 0x1A00 [2]"},
    {0x1A00, 0x03, 32, "TxPDO 0x1A00 [3]"},
    {0x1A00, 0x04, 32, "TxPDO 0x1A00 [4]"},
    {0x1A00, 0x05, 32, "TxPDO 0x1A00 [5]"},
    {0x1C32, 0x01, 16, "SM2 同步模式 (0x1C32:01)"},
    {0x1C32, 0x02, 32, "SM2 周期时间 (0x1C32:02)"},
    {0x1C32, 0x04, 16, "SM2 支持的同步模式 (0x1C32:04)"},
    {0x1C32, 0x20,  1, "SM2 同步错误 (0x1C32:20)"},
    {0x1C33, 0x01, 16, "SM3 同步模式 (0x1C33:01)"},
    {0x1C33, 0x02, 32, "SM3 周期时间 (0x1C33:02)"},
    {0x1C33, 0x04, 16, "SM3 支持的同步模式 (0x1C33:04)"},
    {0x1C33, 0x20,  1, "SM3 同步错误 (0x1C33:20)"},
    {0x1000, 0x00, 32, "设备类型 (0x1000)"},
    {0x6060, 0x00,  8, "模式设定 0x6060"},
    {0x6061, 0x00,  8, "模式回显 0x6061"},
    {0x603F, 0x00, 16, "错误码 0x603F"},
};

void usage(const char* a0) {
    std::printf(
        "用法: %s [--slave N] [--get 0xIDX:SUB:BITS] [--set 0xIDX:SUB:BITS=VALUE] [--quiet]...\n"
        "  不带 --get/--set 时打印内置诊断清单；两者均可重复多个。\n"
        "  --set 写入后会自动回读一次做校验。\n"
        "  --quiet  只打印成功的项\n", a0);
}

} // namespace

int main(int argc, char** argv) {
    uint16_t slave = 0;
    bool     quiet = false;
    std::vector<Req>    extra;
    std::vector<SetReq> sets;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strcmp(a, "--slave") && i + 1 < argc) {
            slave = (uint16_t)std::strtoul(argv[++i], nullptr, 0);
        } else if (!std::strcmp(a, "--quiet")) {
            quiet = true;
        } else if (!std::strcmp(a, "--get") && i + 1 < argc) {
            unsigned idx = 0, sub = 0, bits = 16;
            const char* s = argv[++i];
            if (std::sscanf(s, "%x:%x:%u", &idx, &sub, &bits) < 2) {
                std::fprintf(stderr, "  --get 格式错误: %s（应为 0xIDX:SUB:BITS）\n", s);
                return 2;
            }
            extra.push_back({(uint16_t)idx, (uint8_t)sub, bits, ""});
        } else if (!std::strcmp(a, "--set") && i + 1 < argc) {
            unsigned idx = 0, sub = 0, bits = 16;
            unsigned long long val = 0;
            const char* s = argv[++i];
            if (std::sscanf(s, "%x:%x:%u=%llu", &idx, &sub, &bits, &val) < 4) {
                std::fprintf(stderr, "  --set 格式错误: %s（应为 0xIDX:SUB:BITS=VALUE）\n", s);
                return 2;
            }
            sets.push_back({(uint16_t)idx, (uint8_t)sub, bits, (uint64_t)val});
        } else if (!std::strcmp(a, "-h") || !std::strcmp(a, "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "未知参数: %s\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    ec_master_t* master = ecrt_request_master(0);
    if (!master) {
        std::fprintf(stderr, "[sdo] ecrt_request_master(0) 失败：主站未运行或已被其他应用占用\n");
        return 1;
    }

    auto upload = [&](const Req& r) {
        uint8_t  buf[8] = {0};
        size_t   got    = 0;
        uint32_t abort  = 0;
        const size_t want = (r.bits + 7) / 8;
        const int rc = ecrt_master_sdo_upload(master, slave, r.idx, r.sub, buf, sizeof(buf),
                                              &got, &abort);
        if (rc != 0) {
            if (!quiet) {
                std::printf("  0x%04x:%02x (%2u bit)  ✘ 读取失败 abort=0x%08x%s%s\n",
                            r.idx, r.sub, r.bits, abort,
                            r.name && *r.name ? "  " : "", r.name ? r.name : "");
            }
            return;
        }
        unsigned long long v = 0;
        for (size_t k = 0; k < got && k < 8; ++k) v |= (unsigned long long)buf[k] << (8 * k);
        if (want <= 2) {
            std::printf("  0x%04x:%02x = %llu  (0x%llx)  %s\n",
                        r.idx, r.sub, v, v, r.name ? r.name : "");
        } else {
            std::printf("  0x%04x:%02x = %llu  (0x%08llx)  %s\n",
                        r.idx, r.sub, v, v, r.name ? r.name : "");
        }
    };

    auto download = [&](const SetReq& r) {
        uint8_t  buf[8] = {0};
        const size_t n  = (r.bits + 7) / 8;
        for (size_t k = 0; k < n && k < 8; ++k) buf[k] = (uint8_t)(r.val >> (8 * k));
        uint32_t abort = 0;
        const int rc = ecrt_master_sdo_download(master, slave, r.idx, r.sub, buf, n, &abort);
        if (rc != 0) {
            std::printf("  0x%04x:%02x <- %llu (%2u bit)  ✘ 写入失败 abort=0x%08x\n",
                        r.idx, r.sub, (unsigned long long)r.val, r.bits, abort);
        } else {
            std::printf("  0x%04x:%02x <- %llu (0x%llx) (%2u bit)  ✔ 写入成功\n",
                        r.idx, r.sub, (unsigned long long)r.val,
                        (unsigned long long)r.val, r.bits);
        }
    };

    std::printf("---- SDO 诊断（从站 %u）----\n", slave);
    if (sets.empty() && extra.empty()) {
        for (const Req& r : kList) upload(r);
    } else {
        for (const SetReq& r : sets) {
            download(r);
            upload({r.idx, r.sub, r.bits, "回读校验"});   // 写后自动回读
        }
        for (const Req& r : extra) upload(r);
    }
    std::printf("---- 结束 ----\n");

    ecrt_release_master(master);
    return 0;
}
