// config.cpp —— config/app.conf 解析实现
//
// 设计要点：
//   * key 大小写不敏感（内部统一转大写比较），value 去首尾空白，支持行内 '#' 注释；
//   * 不认识/写错的项只告警，不影响启动（与旧 ZBasic 常量缺省行为一致）；
//   * 支持 0x 前缀的十六进制整数（如 ECAT_DC_ASSIGN = 0x0300）。
#include "config.h"

#include "script/engine_rule.h"   // 脚本引擎规则：只允许一种脚本语言（basic | lua）

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace kx {
namespace {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::string upper(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

// 解析整数：支持十进制与 0x 十六进制；失败返回 false
bool parse_i64(const std::string& v, long long* out) {
    if (v.empty()) return false;
    char* end = nullptr;
    errno = 0;
    long long x = std::strtoll(v.c_str(), &end, 0);   // base 0 => 自动识别 0x
    if (end == v.c_str() || *end != '\0' || errno != 0) return false;
    *out = x;
    return true;
}

bool parse_f64(const std::string& v, double* out) {
    if (v.empty()) return false;
    char* end = nullptr;
    errno = 0;
    double x = std::strtod(v.c_str(), &end);
    if (end == v.c_str() || *end != '\0' || errno != 0) return false;
    *out = x;
    return true;
}

bool parse_bool(const std::string& raw, bool* out) {
    const std::string v = upper(raw);
    if (v == "1" || v == "TRUE" || v == "ON" || v == "YES") { *out = true;  return true; }
    if (v == "0" || v == "FALSE" || v == "OFF" || v == "NO" || v.empty()) { *out = false; return true; }
    return false;
}

// 解析键名尾部的单轴后缀 "_<d>"（d = 0..kAxisMax-1）：
//   "AXIS1_INC_PER_MM" -> base="AXIS_INC_PER_MM", axis=1
// 无合法后缀时 axis 置 -1、base = 原键名（表示「作用于全部轴」）。
bool split_axis_suffix(const std::string& key, std::string* base, int* axis) {
    *axis = -1;
    *base = key;
    const size_t us = key.rfind('_');
    if (us == std::string::npos || us + 2 != key.size()) return false;
    if (!std::isdigit((unsigned char)key[us + 1])) return false;
    const int a = key[us + 1] - '0';
    if (a >= kAxisMax) return false;
    *axis = a;
    *base = key.substr(0, us);
    return true;
}

} // namespace

bool AppConfig::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "[cfg] 打不开 %s，全部使用默认值\n", path.c_str());
        return false;
    }

    std::string line;
    int lineno = 0;
    int unknown = 0;
    int bad = 0;

    while (std::getline(in, line)) {
        ++lineno;
        // 去掉 BOM / CR
        if (lineno == 1 && line.size() >= 3 &&
            (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
            (unsigned char)line[2] == 0xBF) {
            line.erase(0, 3);
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();

        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);

        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            if (!trim(line).empty())
                std::fprintf(stderr, "[cfg] %s:%d 忽略无 '=' 的行\n", path.c_str(), lineno);
            continue;
        }

        std::string key = upper(trim(line.substr(0, eq)));
        const std::string val = trim(line.substr(eq + 1));
        if (key.empty()) continue;

        // 单轴后缀：AXIS1_INC_PER_MM -> base=AXIS_INC_PER_MM, key_axis=1；无后缀则 key_axis=-1
        std::string key_base;
        int         key_axis = -1;
        split_axis_suffix(key, &key_base, &key_axis);
        // 对「轴参数」键：有后缀只改该轴，否则改全部轴（保持单轴配置的向后兼容）
        auto for_axes = [&](auto&& fn) {
            if (key_axis >= 0) fn(axis[key_axis]);
            else for (int a = 0; a < kAxisMax; ++a) fn(axis[a]);
        };

        long long i = 0;
        double    d = 0.0;
        bool      b = false;

        auto need_int = [&]() {
            if (parse_i64(val, &i)) return true;
            std::fprintf(stderr, "[cfg] %s:%d %s=\"%s\" 不是整数\n", path.c_str(), lineno, key.c_str(), val.c_str());
            ++bad;
            return false;
        };
        auto need_dbl = [&]() {
            if (parse_f64(val, &d)) return true;
            std::fprintf(stderr, "[cfg] %s:%d %s=\"%s\" 不是数字\n", path.c_str(), lineno, key.c_str(), val.c_str());
            ++bad;
            return false;
        };
        auto need_bool = [&]() {
            if (parse_bool(val, &b)) return true;
            std::fprintf(stderr, "[cfg] %s:%d %s=\"%s\" 不是布尔(0/1)\n", path.c_str(), lineno, key.c_str(), val.c_str());
            ++bad;
            return false;
        };

        // ---------------- 轴 / 单位 ----------------
        if (key == "AXIS_COUNT")            { if (need_int()) axis_count = (int)i; }
        // M3：MOTION_MODE = pp|csp（或 0|1）；缺省 pp。脚本 MOTION_MODE(m) 运行时可切。
        else if (key == "MOTION_MODE") {
            std::string v = val;
            for (auto& ch : v) ch = (char)std::tolower((unsigned char)ch);
            if      (v == "pp"  || v == "0") motion_mode_csp = 0;
            else if (v == "csp" || v == "1") motion_mode_csp = 1;
            else {
                std::fprintf(stderr, "[cfg] %s:%d MOTION_MODE=\"%s\" 非法（pp/csp）\n",
                             path.c_str(), lineno, val.c_str());
                ++bad;
            }
        }
        else if (key_base == "INC_PER_MM")  { if (need_dbl()) for_axes([&](AxisCfgFile& c){ c.inc_per_mm = d; }); }
        else if (key_base == "AXIS_INDEX")  { if (need_int()) axis[key_axis >= 0 ? key_axis : 0].axis_index = (int)i; }
        else if (key_base == "MV_SPEED")    { if (need_dbl()) for_axes([&](AxisCfgFile& c){ c.def_speed = d; }); }
        else if (key_base == "MV_ACCEL")    { if (need_dbl()) for_axes([&](AxisCfgFile& c){ c.def_accel = d; }); }
        else if (key_base == "MV_SPD_MAX")  { if (need_dbl()) for_axes([&](AxisCfgFile& c){ c.spd_max   = d; }); }
        else if (key_base == "MV_TOL")      { if (need_dbl()) for_axes([&](AxisCfgFile& c){ c.tol_mm    = d; }); }
        else if (key_base == "MV_TIMEOUT_MS" || key_base == "MV_TIMEOUT") {
            if (need_int()) for_axes([&](AxisCfgFile& c){ c.timeout_ms = (uint32_t)(i > 0 ? i : 0); });
        }
        // ---------------- EtherCAT ----------------
        else if (key == "ECAT_MASTER")      { if (need_int()) ecat.master_index = (unsigned)i; }
        else if (key_base == "ECAT_SLAVE_POS") {
            // ECAT_SLAVE_POS（无后缀）= 轴 0；ECAT_SLAVE_POS_<n> = 轴 n
            if (need_int()) ecat.slave_position[key_axis >= 0 ? key_axis : 0] = (unsigned)i;
        }
        else if (key == "ECAT_CYCLE_NS")    { if (need_int()) ecat.cycle_ns = (uint32_t)i; }
        else if (key == "ECAT_DC_ASSIGN")   { if (need_int()) ecat.dc_assign = (uint32_t)i; }
        else if (key == "ECAT_DC_SHIFT_NS") { if (need_int()) ecat.dc_shift_ns = (uint32_t)i; }
        else if (key == "ECAT_VENDOR")      { if (need_int()) ecat.vendor_id = (uint32_t)i; }
        else if (key == "ECAT_PRODUCT")     { if (need_int()) ecat.product_code = (uint32_t)i; }
        else if (key == "ECAT_EXPLICIT_PDO"){ if (need_bool()) ecat.use_explicit_pdo = b; }
        else if (key == "ECAT_EXPLICIT_PROFILE"){ if (need_int()) ecat.explicit_profile = (int)i; }
        else if (key == "ECAT_PROFILE")     { ecat.profile_name = val; }
        else if (key == "ECAT_PROFILE_FORCE"){ if (need_bool()) ecat.profile_force = b; }
        else if (key == "ECAT_AUTO_IDENTITY"){ if (need_bool()) ecat.auto_identity = b; }
        else if (key == "ECAT_WD_DIV")      { if (need_int()) ecat.wd_divider = (uint16_t)i; }
        else if (key == "ECAT_WD_INT")      { if (need_int()) ecat.wd_intervals = (uint16_t)i; }
        else if (key == "ECAT_PARAM_VIA_SDO"){ if (need_bool()) ecat.param_via_sdo = b; }
        // ---------------- 脚本引擎（ZBasic 子集，见 docs/planA/08）----------------
        else if (key == "SCRIPT_ENABLE")    { if (need_bool()) script.enable = b; }
        else if (key == "SCRIPT_FILE")      { script.file = val; }
        else if (key == "SCRIPT_ENGINE")    { script.engine = val; }
        else if (key == "SCRIPT_WAIT_BUS")  { if (need_bool()) script.wait_bus = b; }
        else if (key == "SCRIPT_WAIT_BUS_MS"){ if (need_int()) script.wait_bus_ms = (int)i; }
        else if (key == "SCRIPT_TRACE")     { if (need_bool()) script.trace = b; }
        else if (key == "SCRIPT_MAX_STEPS") { if (need_int()) script.max_steps = i; }
        // ---------------- 调试通道（DebugServer，见 docs/planA/13/16）----------------
        else if (key == "DEBUG_ENABLE")     { if (need_bool()) debug_cfg.enable = b; }
        else if (key == "DEBUG_PORT")       { if (need_int()) debug_cfg.port = (int)i; }
        else if (key == "DEBUG_BIND")       { debug_cfg.bind = val; }
        else if (key == "DEBUG_TOKEN")      { debug_cfg.token = val; }
        else if (key == "DEBUG_MAX_STEPS")  { if (need_int()) debug_cfg.max_steps = i; }
        else if (key == "DEBUG_SCRIPT_DIR") { debug_cfg.script_dir = val; }
        else if (key == "DEBUG_RESTART_CMD") { debug_cfg.restart_cmd = val; }
        // ---------------- 实时 / 日志 ----------------
        else if (key == "RT_CPU")           { if (need_int()) rt.cpu = (int)i; }
        else if (key == "RT_PRIO")          { if (need_int()) rt.prio = (int)i; }
        else if (key == "DBG")              { if (need_bool()) debug = b; }
        else if (key == "LOG_MS")           { if (need_int()) log_ms = (int)i; }
        else {
            std::fprintf(stderr, "[cfg] %s:%d 未知配置项 %s\n", path.c_str(), lineno, key.c_str());
            ++unknown;
        }
    }

    // ---- 规则校验：控制器同一时刻只加载一种脚本语言（见 engine_rule.h）----
    // SCRIPT_ENGINE 只允许 basic | lua（空 = basic）；合法值统一规范化为小写，
    // 非法值明确报错（保留原值，启动时 script_loop 会再次拒绝并跳过脚本线程）。
    {
        ScriptLanguage lang;
        if (script.engine.empty()) {
            script.engine = "basic";
        } else if (parse_script_language(script.engine, &lang)) {
            script.engine = script_language_name(lang);
        } else {
            std::fprintf(stderr, "[cfg] SCRIPT_ENGINE=\"%s\" 非法：只允许 basic | lua\n",
                         script.engine.c_str());
            ++bad;
        }
    }

    // ---- 轴数校验：AXIS_COUNT 必须落在 1..kAxisMax，越界夹紧并告警（绝不静默）----
    if (axis_count < 1 || axis_count > kAxisMax) {
        std::fprintf(stderr, "[cfg] AXIS_COUNT=%d 超出范围 1..%d，已夹紧\n", axis_count, kAxisMax);
        if (axis_count < 1)          axis_count = 1;
        else                         axis_count = kAxisMax;
        ++bad;
    }

    if (bad || unknown) {
        std::fprintf(stderr, "[cfg] %s 载入完成，%d 项格式错误、%d 项未知（已用默认值）\n",
                     path.c_str(), bad, unknown);
    }
    return true;
}

void AppConfig::dump() const {
    std::printf("---- config ----\n");
    std::printf("axes : axis_count=%d (上限 %d)\n", axis_count, kAxisMax);
    for (int a = 0; a < axis_count; ++a) {
        const AxisCfgFile& c = axis[a];
        if (c.inc_per_mm > 0.0) {
            std::printf("axis%d: idx=%d slave_pos=%u inc_per_mm=%.5f def_speed=%.3f def_accel=%.3f ",
                        a, c.axis_index, ecat.slave_position[a], c.inc_per_mm, c.def_speed, c.def_accel);
        } else {
            std::printf("axis%d: idx=%d slave_pos=%u inc_per_mm=unset def_speed=%.3f def_accel=%.3f ",
                        a, c.axis_index, ecat.slave_position[a], c.def_speed, c.def_accel);
        }
        std::printf("spd_max=%.1f tol=%.3f timeout=%ums\n", c.spd_max, c.tol_mm, c.timeout_ms);
    }
    std::printf("ecat : master=%u cycle=%uns dc_assign=0x%04x dc_shift=%uns "
                "vid=0x%08x pid=0x%08x profile=%s(auto_id=%d force=%d) "
                "explicit=%d gear=%d wd=%u/%u sdo_param=%d\n",
                ecat.master_index, ecat.cycle_ns,
                ecat.dc_assign, ecat.dc_shift_ns, ecat.vendor_id, ecat.product_code,
                ecat.profile_name.c_str(), ecat.auto_identity ? 1 : 0,
                ecat.profile_force ? 1 : 0,
                ecat.use_explicit_pdo ? 1 : 0, ecat.explicit_profile,
                ecat.wd_divider, ecat.wd_intervals,
                ecat.param_via_sdo ? 1 : 0);
    std::printf("script: enable=%d engine=%s file=%s wait_bus=%d/%ums trace=%d max_steps=%lld\n",
                script.enable ? 1 : 0, script.engine.c_str(),
                script.file.empty() ? "(none)" : script.file.c_str(),
                script.wait_bus ? 1 : 0, script.wait_bus_ms,
                script.trace ? 1 : 0, script.max_steps);
    std::printf("rt   : cpu=%d prio=%d  dbg=%d log=%ums\n",
                rt.cpu, rt.prio, debug ? 1 : 0, log_ms);
    std::printf("debug: enable=%d port=%d bind=%s token=%s max_steps=%lld\n",
                debug_cfg.enable ? 1 : 0, debug_cfg.port, debug_cfg.bind.c_str(),
                debug_cfg.token.empty() ? "(none)" : "(set)", debug_cfg.max_steps);
    std::printf("debug: restart=%s script_dir=%s\n",
                debug_cfg.restart_cmd.empty() ? "(disabled)" : "(set)",
                debug_cfg.script_dir.c_str());
    std::printf("----------------\n");
}

} // namespace kx
