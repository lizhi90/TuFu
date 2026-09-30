// esi_parser_test.cpp —— ESI/XML 解析纯逻辑自测（不需要 IgH、不需要硬件）
//
// 覆盖：
//   1) 数字解析（#x / 0x / 十进制 / 非法）；
//   2) EtherCATInfo 全流程解析（Vendor/Device/Type/Sm/RxPdo/TxPdo/Entry）；
//   3) 自闭合标签、注释、属性写法；
//   4) 与 SV630 档案对比（一致 / 不一致两种）；
//   5) 生成候选档案片段。
#include "../src/motion/esi_parser.h"
#include "../src/motion/drive_profile.h"

#include <cstdio>
#include <string>

using namespace kx;

static int g_fail = 0;

static void check(bool cond, const char* what) {
    std::printf("  %s %s\n", cond ? "[ OK ]" : "[FAIL]", what);
    if (!cond) ++g_fail;
}

// 仿 SV630 的 EtherCATInfo 片段（含注释与字符串属性/元素两种写法）
static const char* kEsi =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<EtherCATInfo>\n"
    "  <!-- 厂商信息 -->\n"
    "  <Vendor>\n"
    "    <Id>#x00100000</Id>\n"
    "    <Name>Inovance</Name>\n"
    "  </Vendor>\n"
    "  <Descriptions>\n"
    "    <Devices>\n"
    "      <Device>\n"
    "        <Type ProductCode=\"#x000c0112\" RevisionNo=\"#x00010000\">SV630</Type>\n"
    "        <Name>SV630_1Axis_03716</Name>\n"
    "        <Sm><Index>#x2</Index><Name>SM2 Outputs</Name><Pdo><Index>#x1600</Index></Pdo></Sm>\n"
    "        <Sm><Index>#x3</Index><Name>SM3 Inputs</Name><Pdo><Index>#x1a00</Index></Pdo></Sm>\n"
    "        <RxPdo Fixed=\"1\" Mandatory=\"1\" Sm=\"2\">\n"
    "          <Index>#x1600</Index>\n"
    "          <Name>RxPDO</Name>\n"
    "          <Entry><Index>#x6040</Index><SubIndex>0</SubIndex><BitLen>16</BitLen><Name>Controlword</Name></Entry>\n"
    "          <Entry><Index>#x607a</Index><SubIndex>0</SubIndex><BitLen>32</BitLen><Name>Target position</Name></Entry>\n"
    "          <Entry><Index>#x6060</Index><SubIndex>0</SubIndex><BitLen>8</BitLen><Name>Modes of operation</Name></Entry>\n"
    "        </RxPdo>\n"
    "        <TxPdo Fixed=\"1\" Mandatory=\"1\" Sm=\"3\">\n"
    "          <Index>#x1a00</Index>\n"
    "          <Name>TxPDO</Name>\n"
    "          <Entry><Index>#x6041</Index><SubIndex>0</SubIndex><BitLen>16</BitLen><Name>Statusword</Name></Entry>\n"
    "          <Entry><Index>#x6064</Index><SubIndex>0</SubIndex><BitLen>32</BitLen><Name>Position actual value</Name></Entry>\n"
    "        </TxPdo>\n"
    "      </Device>\n"
    "    </Devices>\n"
    "  </Descriptions>\n"
    "</EtherCATInfo>\n";

static void test_numbers() {
    std::printf("== 1) 数字解析 ==\n");
    uint32_t v = 0;
    check(esi_parse_u32("#x000c0112", &v) && v == 0x000c0112, "#x 十六进制");
    check(esi_parse_u32("0x0300", &v) && v == 0x0300, "0x 十六进制");
    check(esi_parse_u32(" 1600 ", &v) && v == 1600, "十进制 + 空白");
    check(!esi_parse_u32("abc", &v), "非法输入返回 false");
    check(!esi_parse_u32("", &v), "空串返回 false");
}

static void test_parse() {
    std::printf("== 2) 全流程解析 ==\n");
    EsiDevice dev;
    const bool ok = esi_parse_string(kEsi, &dev);
    check(ok && dev.valid, "解析成功");
    if (!ok) { std::printf("       error=%s\n", dev.error.c_str()); return; }

    check(dev.vendor_id == 0x00100000, "Vendor Id = 0x00100000");
    check(dev.vendor_name == "Inovance", "Vendor Name = Inovance");
    check(dev.product_code == 0x000c0112, "ProductCode = 0x000c0112");
    check(dev.revision == 0x00010000, "RevisionNo = 0x00010000");
    check(dev.device_type == "SV630", "Type 文本 = SV630");
    check(dev.device_name == "SV630_1Axis_03716", "Device Name");

    check(dev.sms.size() == 2, "解析出 2 个 SM");
    if (dev.sms.size() == 2) {
        check(dev.sms[0].index == 2 && !dev.sms[0].is_input, "SM2 = Outputs");
        check(dev.sms[1].index == 3 && dev.sms[1].is_input, "SM3 = Inputs");
        check(!dev.sms[0].pdo_indexes.empty() && dev.sms[0].pdo_indexes[0] == 0x1600,
              "SM2 默认分配 0x1600");
    }

    check(dev.pdos.size() == 2, "解析出 2 个 PDO");
    const EsiPdo* rx = dev.find_pdo(0x1600);
    const EsiPdo* tx = dev.find_pdo(0x1a00);
    check(rx && rx->is_rx && rx->entries.size() == 3, "RxPdo 0x1600 有 3 条");
    check(tx && !tx->is_rx && tx->entries.size() == 2, "TxPdo 0x1a00 有 2 条");
    if (rx && rx->entries.size() == 3) {
        check(rx->entries[0].index == 0x6040 && rx->entries[0].bits == 16, "Rx[0]=6040:16");
        check(rx->entries[1].index == 0x607a && rx->entries[1].bits == 32, "Rx[1]=607a:32");
        check(rx->entries[2].index == 0x6060 && rx->entries[2].bits == 8,  "Rx[2]=6060:8");
        check(rx->entries[0].name == "Controlword", "条目带名称");
    }
    check(dev.count_rx_entries() == 3 && dev.count_tx_entries() == 2, "Rx/Tx 计数");
    check(dev.find_entry(0x6064, 0) != nullptr, "按对象索引查条目");
}

static void test_check_profile() {
    std::printf("== 3) 与 SV630 档案对比 ==\n");
    EsiDevice dev;
    esi_parse_string(kEsi, &dev);

    const DriveProfile* sv = ProfileRegistry::instance().find_by_name("sv630");
    if (!sv || !dev.valid) { check(false, "前置数据缺失"); return; }

    EsiCheckReport r = esi_check_profile(dev, *sv);
    check(r.vid_ok && r.pid_ok, "vid/pid 一致");
    check(r.rev_ok, "rev 一致");
    check(r.pdo_matches, "档案档位1 的条目都能在 ESI 找到");
    check(r.all_ok(), "整体 all_ok");
    check(r.gear_checked == 1, "默认比对档位1");
    std::printf("%s", esi_check_to_text(r).c_str());

    // 故意改 product code -> 应报不一致
    EsiDevice bad = dev;
    bad.product_code = 0x000c0199;
    EsiCheckReport r2 = esi_check_profile(bad, *sv);
    check(!r2.pid_ok && !r2.all_ok(), "pid 不同 -> all_ok=false");

    // 拿 generic 档案对比：generic 的 5 个对象也在 ESI 里 -> PDO 应能对上，但 pid 必不同
    const DriveProfile* gen = ProfileRegistry::instance().find_by_name("generic");
    if (gen) {
        EsiCheckReport r3 = esi_check_profile(dev, *gen);
        check(!r3.pid_ok, "generic 档案 pid 不匹配（预期）");
        check(r3.pdo_matches, "generic 的 5 个标准对象在 ESI 中仍能找到");
    }
}

static void test_missing_entry() {
    std::printf("== 4) 档案含 ESI 不存在条目 ==\n");
    // 构造一份「含 60ff」的档位（对应 SV630 档位3），ESI 里没有 60ff -> 应报危险
    const DriveProfile* sv = ProfileRegistry::instance().find_by_name("sv630");
    EsiDevice dev;
    esi_parse_string(kEsi, &dev);
    if (!sv || !dev.valid) return;

    EsiCheckReport r = esi_check_profile(dev, *sv, /*gear_1based=*/3);
    check(!r.pdo_matches, "档位3（含 60ff）与 ESI 不一致 -> 报警");
    bool has_missing = false;
    for (const std::string& s : r.findings)
        if (s.find("60ff") != std::string::npos || s.find("找不到") != std::string::npos) has_missing = true;
    check(has_missing, "findings 指出了缺失条目");
    std::printf("%s", esi_check_to_text(r).c_str());
}

static void test_snippet() {
    std::printf("== 5) 生成候选档案片段 ==\n");
    EsiDevice dev;
    esi_parse_string(kEsi, &dev);
    if (!dev.valid) return;
    const std::string s = esi_to_profile_snippet(dev, "sv630_candidate");
    check(s.find("kEntries_sv630_candidate") != std::string::npos, "片段含条目数组名");
    check(s.find("0x6040") != std::string::npos, "片段含 6040");
    check(s.find("0x100000") != std::string::npos, "片段含 vendor id");
    std::printf("---- snippet ----\n%s-----------------\n", s.c_str());
}

static void test_bad_input() {
    std::printf("== 6) 非法输入 ==\n");
    EsiDevice dev;
    check(!esi_parse_string("<NotEthercat></NotEthercat>", &dev), "非 EtherCATInfo -> false");
    check(!dev.error.empty(), "给出错误原因");
    check(!esi_parse_file("/tmp/kx_no_such_esi_xyz.xml", &dev), "文件不存在 -> false");
}

int main() {
    std::printf("==== esi_parser_test ====\n");
    test_numbers();
    test_parse();
    test_check_profile();
    test_missing_entry();
    test_snippet();
    test_bad_input();
    std::printf("=========================\n");
    if (g_fail == 0) std::printf("ALL PASS\n");
    else             std::printf("%d 项失败\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
