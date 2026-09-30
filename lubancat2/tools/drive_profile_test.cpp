// drive_profile_test.cpp —— 驱动器档案/识别匹配纯逻辑自测（不需要 IgH、不需要硬件）
//
// 覆盖：
//   1) 注册表：内置 sv630/generic 已注册，按名可查；
//   2) 精确匹配：vid/pid/rev/SII 名称全中 -> EXACT；
//   3) 坑位防护：rev 不符 / SII 名称不符 -> ID_ONLY（不自动采用，降级提示）；
//   4) 克隆/新型号：vid 对 pid 不对 -> 回退 generic 且给提示；
//   5) 名字解析：auto / 具体名 / 未知名；
//   6) 档位内容：SV630 档位1 = 3Rx+2Tx，对象与槽位一一对应。
#include "../src/motion/drive_profile.h"

#include <cstdio>
#include <cstring>

using namespace kx;

static int g_fail = 0;

static void check(bool cond, const char* what) {
    std::printf("  %s %s\n", cond ? "[ OK ]" : "[FAIL]", what);
    if (!cond) ++g_fail;
}

static void test_registry() {
    std::printf("== 1) 注册表 ==\n");
    ProfileRegistry& reg = ProfileRegistry::instance();
    check(reg.size() >= 2, "至少注册了 2 份档案");
    check(reg.find_by_name("sv630") != nullptr, "按名找到 sv630");
    check(reg.find_by_name("SV630") != nullptr, "档案名大小写不敏感");
    check(reg.find_by_name("generic") != nullptr, "按名找到 generic");
    check(reg.find_by_name("no_such") == nullptr, "未知名返回 nullptr");
    check(reg.fallback() != nullptr && std::strcmp(reg.fallback()->name, "generic") == 0,
          "兜底档案是 generic");

    const DriveProfile* sv = reg.find_by_name("sv630");
    if (!sv) return;
    check(sv->vendor_id == 0x00100000 && sv->product_code == 0x000c0112,
          "SV630 vid/pid = 0x00100000/0x000c0112");
    check(sv->revision == 0x00010000, "SV630 rev = 0x00010000");
    check(sv->n_pdos == 3, "SV630 有 3 个 PDO 档位");
    check(sv->default_gear == 1, "SV630 默认档位 1");
    check(sv->param_objs.profile_vel == 0x6081 && sv->param_objs.profile_acc == 0x6083 &&
          sv->param_objs.profile_dec == 0x6084, "轮廓参数对象 6081/6083/6084");
    check(sv->halt.halt_option_index == 0x605D, "停机语义用 605D");
    check(sv->halt.error_code_index == 0x203F && sv->halt.error_code_bits == 32,
          "唯一错误码用 203F(Uint32)");
    check(sv->supports_pv == true, "支持 PV（60ff 走 SDO）");
}

static void test_gear_layout() {
    std::printf("== 2) 档位内容 ==\n");
    const DriveProfile* sv = ProfileRegistry::instance().find_by_name("sv630");
    if (!sv) return;

    const PdoProfile& g1 = sv->pdos[0];
    check(g1.n_rx == 3 && g1.n_tx == 2, "档位1 = 3Rx + 2Tx");
    check(g1.entries[0].index == 0x6040 && g1.entries[0].bits == 16, "Rx 首条 6040:16");
    check(g1.entries[1].index == 0x607a && g1.entries[1].bits == 32, "Rx 次条 607a:32");
    check(g1.entries[2].index == 0x6060 && g1.entries[2].bits == 8,  "Rx 三条 6060:8");
    check(g1.entries[3].index == 0x6041 && g1.entries[3].bits == 16, "Tx 首条 6041:16");
    check(g1.entries[4].index == 0x6064 && g1.entries[4].bits == 32, "Tx 次条 6064:32");

    // 槽位必须唯一且都在合法范围
    bool slot_seen[SLOT_COUNT] = {false};
    bool dup = false, bad = false;
    for (unsigned i = 0; i < g1.n_slots; ++i) {
        const uint8_t s = g1.slots[i].slot;
        if (s >= SLOT_COUNT) bad = true;
        else if (slot_seen[s]) dup = true;
        else slot_seen[s] = true;
    }
    check(!dup, "槽位无重复");
    check(!bad, "槽位都在 0..7");
    check(g1.slots[0].slot == SLOT_CTRL_WORD && g1.slots[0].index == 0x6040,
          "slots[0] = 6040 -> ctrl_word");

    const PdoProfile& g3 = sv->pdos[2];
    check(g3.n_rx == 4 && g3.n_tx == 4, "档位3 = 4Rx + 4Tx（含 60ff）");
    check(g3.slots[3].index == 0x60ff && g3.slots[3].slot == SLOT_TARGET_VEL,
          "档位3 含 60ff -> target_vel");
}

static void test_exact_match() {
    std::printf("== 3) 精确匹配 ==\n");
    MatchResult r = profile_match(0x00100000, 0x000c0112, 0x00010000, "SV630_1Axis_03716");
    check(r.kind == ProfileMatch::EXACT, "vid/pid/rev/名称全中 -> EXACT");
    check(r.profile && std::strcmp(r.profile->name, "sv630") == 0, "命中 sv630");
    check(!r.rev_mismatch && !r.name_mismatch, "无 mismatch 标记");
    std::printf("       理由: %s\n", r.detail);

    // SII 名称的另一候选（InoSV630N）
    MatchResult r2 = profile_match(0x00100000, 0x000c0112, 0x00010000, "InoSV630N");
    check(r2.kind == ProfileMatch::EXACT, "SII 名称候选 InoSV630N 也认");
}

static void test_pitfalls() {
    std::printf("== 4) 坑位防护 ==\n");

    // (a) 同 vid/pid 但 rev 不同（换固件）—— 不自动采用
    MatchResult r = profile_match(0x00100000, 0x000c0112, 0x00090099, "SV630_1Axis_03716");
    check(r.kind == ProfileMatch::ID_ONLY, "rev 不符 -> ID_ONLY（降级，不自动采用）");
    check(r.rev_mismatch, "rev_mismatch 标记已置");
    std::printf("       理由: %s\n", r.detail);

    // (b) 克隆：vid/pid 相同但 SII 名称不同
    MatchResult r2 = profile_match(0x00100000, 0x000c0112, 0x00010000, "ClonEx_Servo_9000");
    check(r2.kind == ProfileMatch::ID_ONLY, "SII 名称不符 -> ID_ONLY");
    check(r2.name_mismatch, "name_mismatch 标记已置");

    // (c) 同厂新型号：vid 对、pid 不对 -> generic 兜底 + 提示
    MatchResult r3 = profile_match(0x00100000, 0x000c0199, 0x00010000, "SV670N");
    check(r3.kind == ProfileMatch::GENERIC, "pid 不认 -> GENERIC 兜底");
    check(r3.profile && std::strcmp(r3.profile->name, "generic") == 0, "兜底档案是 generic");
    std::printf("       理由: %s\n", r3.detail);

    // (d) 完全陌生厂商 -> 也是 GENERIC（不静默猜具体型号）
    MatchResult r4 = profile_match(0x00abcdef, 0x12345678, 0x00000001, nullptr);
    check(r4.kind == ProfileMatch::GENERIC, "陌生厂商 -> GENERIC（绝不静默猜）");
}

static void test_resolve_name() {
    std::printf("== 5) 名字解析 ==\n");
    bool is_auto = false, unknown = false;

    const DriveProfile* p1 = profile_resolve_name("auto", &is_auto, &unknown);
    check(p1 == nullptr && is_auto && !unknown, "auto -> 交给总线识别");

    is_auto = unknown = false;
    const DriveProfile* p2 = profile_resolve_name("sv630", &is_auto, &unknown);
    check(p2 && !is_auto && !unknown, "sv630 -> 直接给档案指针");

    is_auto = unknown = false;
    const DriveProfile* p3 = profile_resolve_name("typo_name", &is_auto, &unknown);
    check(p3 == nullptr && !is_auto && unknown, "未知名 -> unknown=true（调用方须报错）");

    is_auto = unknown = false;
    const DriveProfile* p4 = profile_resolve_name("", &is_auto, &unknown);
    check(p4 == nullptr && !unknown, "空串 -> 不过问（返回 nullptr 不报未知）");
}

static void test_describe() {
    std::printf("== 6) 描述输出 ==\n");
    char buf[320];
    const DriveProfile* sv = ProfileRegistry::instance().find_by_name("sv630");
    if (!sv) return;
    const int n = profile_describe(*sv, buf, sizeof(buf));
    check(n > 0, "profile_describe 有输出");
    std::printf("       %s\n", buf);
}

static void sink_count(const char* line, void* user) {
    (void)line;
    ++*(int*)user;
}

int main() {
    std::printf("==== drive_profile_test ====\n");
    test_registry();
    test_gear_layout();
    test_exact_match();
    test_pitfalls();
    test_resolve_name();
    test_describe();

    int lines = 0;
    profile_dump_all(sink_count, &lines);
    std::printf("== 7) dump_all 输出 %d 行 ==\n", lines);
    check(lines > 0, "profile_dump_all 有输出");

    std::printf("============================\n");
    if (g_fail == 0) std::printf("ALL PASS\n");
    else             std::printf("%d 项失败\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
