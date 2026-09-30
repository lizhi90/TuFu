// config_test.cpp —— 增量5 纯逻辑自测（不需要 IgH / 不需要硬件）
//
// 覆盖：
//   1) 缺省值：文件不存在时全部使用默认值且不致命；
//   2) 真实 config/app.conf 能解析，关键项与文档一致；
//   3) 容错：注释、行内注释、hex、多余空白、未知键、格式错误项均不影响其他项；
//   4) 带 BOM/CRLF 的首行也能正确解析。
#include "../src/common/config.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace kx;

static int g_fail = 0;

static void check(bool cond, const char* what) {
    std::printf("  %s %s\n", cond ? "[ OK ]" : "[FAIL]", what);
    if (!cond) ++g_fail;
}

static void write_file(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

static void test_defaults() {
    std::printf("== 1) 缺省值（文件不存在）==\n");
    AppConfig cfg;
    const bool ok = cfg.load("/tmp/kx_no_such_config_12345.conf");
    check(ok == false, "文件不存在返回 false（不致命）");
    check(cfg.axis[0].inc_per_mm == 0.0, "INC_PER_MM 无默认（0=未设置，由脚本 UNITS 运行时设置）");
    check(cfg.ecat.cycle_ns == 1000000, "周期默认 1ms");
    check(cfg.ecat.dc_assign == 0x0300, "DC assign 默认 0x0300");
    check(cfg.script.enable == false, "脚本默认关闭");
    check(cfg.script.wait_bus == true && cfg.script.wait_bus_ms == 15000, "脚本等总线默认开/15s");
    check(cfg.script.max_steps == 5000000, "脚本步数预算默认 5e6");
}

static void test_real_conf() {
    std::printf("== 2) 真实 config/app.conf ==\n");
    // ctest 从 build/ 运行，故同时尝试两个相对路径
    const char* paths[] = {"config/app.conf", "../config/app.conf"};
    AppConfig cfg;
    bool ok = false;
    for (const char* p : paths) {
        if (cfg.load(p)) { ok = true; break; }
    }
    check(ok, "config/app.conf 可解析");
    if (!ok) return;

    check(cfg.axis[0].inc_per_mm == 0.0, "app.conf 未再给出厂脉冲当量（unset）");
    check(cfg.axis[0].def_speed == 100.0, "MV_SPEED = 100");
    check(cfg.axis[0].def_accel == 500.0, "MV_ACCEL = 500");
    check(cfg.axis[0].spd_max == 3276.7, "MV_SPD_MAX = 3276.7");
    check(cfg.axis[0].tol_mm == 0.05, "MV_TOL = 0.05");
    check(cfg.axis[0].timeout_ms == 10000, "MV_TIMEOUT_MS = 10000");
    check(cfg.ecat.dc_assign == 0x0300, "ECAT_DC_ASSIGN = 0x0300（hex 解析）");
    check(cfg.ecat.dc_shift_ns == 500000, "ECAT_DC_SHIFT_NS = 500000");
    check(cfg.ecat.cycle_ns == 1000000, "ECAT_CYCLE_NS = 1000000");
    // 出厂 app.conf 启用自动脚本线程（开机主文件优先；未设主文件且 SCRIPT_FILE 为空时不会真的跑）
    check(cfg.script.enable == true && cfg.script.file.empty(), "SCRIPT_ENABLE=1 / SCRIPT_FILE 空");
    check(cfg.script.wait_bus_ms == 15000, "SCRIPT_WAIT_BUS_MS = 15000");
    check(cfg.debug == false, "DBG = 0");
    check(cfg.log_ms == 3000, "LOG_MS = 3000");
}

static void test_tricky() {
    std::printf("== 3) 容错解析 ==\n");
    const std::string path = "/tmp/kx_tricky.conf";
    // 注意：把 BOM 放在首行，CRLF 结尾，混入未知键与格式错误项
    write_file(path,
        "\xEF\xBB\xBFINC_PER_MM = 20000.5   # 行内注释\r\n"
        "\r\n"
        "   MV_SPEED=250   \r\n"
        "ECAT_DC_ASSIGN = 0x0700\r\n"
        "ECAT_EXPLICIT_PDO = 1\r\n"
        "SCRIPT_ENGINE = lua\r\n"
        "LOG_MS = 1234\r\n"
        "UNKNOWN_KEY = 9\r\n"          // 未知：告警但不影响
        "MV_TOL = not_a_number\r\n"    // 格式错误：保留默认
        "NOEQUALS_LINE\r\n"
        "MV_TIMEOUT_MS = 12345\r\n");
    AppConfig cfg;
    const bool ok = cfg.load(path);
    check(ok, "含杂质文件仍返回 true");

    check(cfg.axis[0].inc_per_mm > 20000.4 && cfg.axis[0].inc_per_mm < 20000.6, "BOM+CRLF 首行解析");
    check(cfg.axis[0].def_speed == 250.0, "行首尾空白被裁剪");
    check(cfg.ecat.dc_assign == 0x0700, "hex 覆盖生效");
    check(cfg.ecat.use_explicit_pdo == true, "布尔 = 1");
    check(cfg.script.engine == "lua", "字符串值解析");
    check(cfg.log_ms == 1234, "整数值解析");
    check(cfg.axis[0].tol_mm == 0.05, "格式错误项保留默认值");
    check(cfg.axis[0].timeout_ms == 12345, "错误项后续行仍被解析（顺序无关）");

    // 词法别名：SECONDS 之类不识别；再验证 MV_TIMEOUT 别名
    write_file(path, "MV_TIMEOUT = 7777\n");
    AppConfig c2;
    c2.load(path);
    check(c2.axis[0].timeout_ms == 7777, "MV_TIMEOUT 别名");

    std::remove(path.c_str());
}

int main() {
    std::printf("==== config_test（增量5）====\n");
    test_defaults();
    test_real_conf();
    test_tricky();
    std::printf("=============================\n");
    if (g_fail == 0) std::printf("ALL PASS\n");
    else             std::printf("%d 项失败\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
