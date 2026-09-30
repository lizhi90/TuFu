// esi_tool.cpp —— ESI/XML 导入命令行工具（调试软件的「导入」后端雏形）
//
// 用法：
//   esi_tool <file.esi|.xml>                      # 打印身份 + PDO 布局
//   esi_tool <file> --check <档案名> [--gear N]   # 与已注册档案对比
//   esi_tool <file> --snippet [名字]              # 输出候选档案代码片段
//   esi_tool --list-profiles                      # 列出内置档案
//
// 例：
//   ./esi_tool /tmp/SV630.xml --check sv630
//   ./esi_tool /tmp/SV630.xml --snippet sv630_new
#include "../src/motion/esi_parser.h"
#include "../src/motion/drive_profile.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace kx;

static void usage(const char* a0) {
    std::printf(
        "用法: %s <file.esi|.xml> [--check <档案名> [--gear N]] [--snippet [名字]]\n"
        "      %s --list-profiles\n", a0, a0);
}

static void print_device(const EsiDevice& d) {
    std::printf("---- ESI 设备信息 ----\n");
    std::printf("厂商    : %s (0x%08x)\n", d.vendor_name.c_str(), d.vendor_id);
    std::printf("型号    : %s / Type=%s\n", d.device_name.c_str(), d.device_type.c_str());
    std::printf("标识    : ProductCode=0x%08x Revision=0x%08x\n", d.product_code, d.revision);
    std::printf("SM      : %zu 个\n", d.sms.size());
    for (const EsiSm& s : d.sms) {
        std::printf("  SM%u (%s)", s.index, s.is_input ? "Inputs" : "Outputs");
        for (uint16_t p : s.pdo_indexes) std::printf(" -> 0x%04x", p);
        std::printf("\n");
    }
    std::printf("PDO     : %zu 个（Rx %u 条 / Tx %u 条）\n",
                d.pdos.size(), d.count_rx_entries(), d.count_tx_entries());
    for (const EsiPdo& p : d.pdos) {
        std::printf("  0x%04x %-6s %s (%zu 条)\n", p.index, p.is_rx ? "RxPdo" : "TxPdo",
                    p.name.c_str(), p.entries.size());
        for (const EsiEntry& e : p.entries)
            std::printf("      0x%04x:%02x %2u bits  %s\n", e.index, e.subindex,
                        (unsigned)e.bits, e.name.c_str());
    }
    std::printf("----------------------\n");
}

static void sink(const char* line, void*) { std::printf("  %s\n", line); }

int main(int argc, char** argv) {
    if (argc < 2) { usage(argv[0]); return 2; }
    if (!std::strcmp(argv[1], "--list-profiles")) {
        std::printf("已注册驱动器档案（共 %zu 份）:\n", ProfileRegistry::instance().size());
        profile_dump_all(sink, nullptr);
        return 0;
    }

    const char* file = argv[1];
    std::string check_name;
    std::string snippet_name;
    bool do_snippet = false;
    int gear = 0;

    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--check") && i + 1 < argc) check_name = argv[++i];
        else if (!std::strcmp(argv[i], "--gear") && i + 1 < argc) gear = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--snippet")) {
            do_snippet = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') snippet_name = argv[++i];
        }
        else if (!std::strcmp(argv[i], "-h") || !std::strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        else { std::fprintf(stderr, "未知参数: %s\n", argv[i]); usage(argv[0]); return 2; }
    }

    EsiDevice dev;
    if (!esi_parse_file(file, &dev)) {
        std::fprintf(stderr, "[esi] 解析失败: %s\n", dev.error.c_str());
        return 1;
    }
    print_device(dev);

    if (!check_name.empty()) {
        const DriveProfile* p = ProfileRegistry::instance().find_by_name(check_name.c_str());
        if (!p) {
            std::fprintf(stderr, "[esi] 未知档案 '%s'（--list-profiles 查看）\n", check_name.c_str());
            return 2;
        }
        const EsiCheckReport r = esi_check_profile(dev, *p, gear);
        std::printf("%s", esi_check_to_text(r).c_str());
        return r.all_ok() ? 0 : 3;
    }

    if (do_snippet) {
        std::printf("%s", esi_to_profile_snippet(dev, snippet_name.c_str()).c_str());
    }
    return 0;
}
