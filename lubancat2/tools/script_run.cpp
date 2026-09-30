// script_run.cpp —— 脚本命令行工具（调试软件的脚本后端雏形），支持 BASIC 与 Lua 双引擎
//
// 用法:
//   script_run <file.bas>            语法检查 + 逻辑试跑（dry host：命令打印出来，不驱动硬件）
//   script_run <file.lua>            同上，按扩展名自动选 Lua 引擎
//   script_run <file> --engine lua   显式指定引擎（basic | lua）
//   script_run <file> --check        只做语法检查
//   script_run <file> --trace        单步跟踪（打印每条语句/行）
//   script_run <file> --steps N      步数预算（BASIC=语句级 / Lua=VM 指令级，默认 5000000）
//   script_run <file> --sim-bus      模拟"总线已连上"（SLOT_SCAN/SLOT_START 成功、
//                                    NODE_COUNT/NODE_AXIS_COUNT>0），以便离线走通
//                                    遗留脚本 扫描→映射轴→开总线→主循环 的整条逻辑
//
// dry host 的约定：任何命令都"成功"，查询类返回 0，动作类只打印不走设备。
// 因此本工具用于**脚本逻辑自测**；真正驱动设备请在控制器进程内运行（见 motion_host.h）。
#include "script/engine_rule.h"
#include "script/lua_engine.h"
#include "script/script.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace kx;

namespace {

// 逻辑命令集合：dry 模式下这些返回 0，其余按"未知"处理以便暴露拼写错误
bool is_known_cmd(const std::string& n) {
    static const char* kKnown[] = {
        "EN", "ENABLE", "DIS", "DISABLE", "STOP", "MOVE", "MOVR", "MOVEABS", "MOVE_ABS",
        "MOVEREL", "JOG", "VJOG", "HOME", "DELAY", "SLEEP", "WAIT", "SCAN", "BUSSTOP",
        // B 层：端口 / 任务 / 总线
        "OPEN", "CLOSE", "BASE", "PRINT", "PUTCHAR", "RUNTASK", "STOPTASK",
        "SLOT_START", "SLOT_STOP", "SDO_WRITE", "VMOVE", "CANCEL", "RAPIDSTOP",
        "SLOT_SCAN", "DISABLE_GROUP", "WAITIDLE", "DATUM"};
    for (const char* k : kKnown) if (n == k) return true;
    return false;
}

// 查询类命令：dry 模式返回 0
bool is_query(const std::string& n) {
    static const char* kQ[] = {
        "POS", "MPOS", "DPOS", "BUS", "BUSOK", "BUSY", "IDLE", "ISIDLE", "ENABLED",
        "ALARM",
        // B 层：端口 / Modbus / 任务 / 总线 / 轴参数
        "PORT_STATUS", "PORT_TARGET", "PORT_MAX", "PROC_STATUS",
        "MODBUS_REG", "MODBUS_IEEE", "MODBUSM_DES2",
        "SCAN_EVENT", "NODE_COUNT", "NODE_AXIS_COUNT", "NODE_STATUS", "NODE_IO",
        "NODE_AIO", "NODE_INFO", "ECUSTOM", "ETHERCAT", "ETH_MODE",
        "ATYPE", "UNITS", "SPEED", "ACCEL", "DECEL", "AXIS_ADDRESS", "DRIVE_PROFILE",
        "DRIVE_CONTROLWORD", "AXISSTATUS", "AXIS",
        "IP", "IPSTR", "TCP", "TABLE", "TX", "IN", "OUT", "AD"};
    for (const char* k : kQ) if (n == k) return true;
    return false;
}

struct DryHost : ScriptHost {
    bool show_cmd = true;
    bool sim_bus = false;                        // --sim-bus：模拟总线已连接

    int call(const std::string& name, const std::vector<Value>& args,
             Value* ret, std::string* err) override {
        std::string argstr;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) argstr += ", ";
            argstr += args[i].to_text();
        }
        if (name == "GET") {                     // 端口接收：dry 模式恒回传 0 字节
            if (ret) *ret = Value::text("");
            if (show_cmd) std::printf("[dry] GET(%s) -> 0 字节\n", argstr.c_str());
            return 0;
        }
        if (sim_bus) {                           // 模拟"扫到 1 个带驱动器的从站"，让脚本越过总线初始化
            long v = 0; bool hit = true;
            if (name == "SLOT_SCAN" || name == "SCAN") v = 1;            // 扫到 1 个从站
            else if (name == "SLOT_START") v = 1;                        // 总线开启成功
            else if (name == "NODE_COUNT") v = 1;
            else if (name == "NODE_AXIS_COUNT") v = 1;
            else if (name == "NODE_STATUS") v = 8;                       // AL_STATE OP
            else hit = false;
            if (hit) {
                if (ret) *ret = Value::number(v);
                if (show_cmd) std::printf("[dry] %s(%s) -> %ld (sim-bus)\n", name.c_str(), argstr.c_str(), v);
                return 0;
            }
        }
        if (is_query(name)) {
            if (ret) *ret = Value::number(0);
            if (show_cmd) std::printf("[dry] %s(%s) -> 0\n", name.c_str(), argstr.c_str());
            return 0;
        }
        if (is_known_cmd(name)) {
            if (show_cmd) std::printf("[dry] %s %s\n", name.c_str(), argstr.c_str());
            return 0;
        }
        if (err) *err = "dry host 不认识该命令（检查拼写，或改用真正的设备宿主）";
        return 2;
    }
    void print(const std::string& line) override { std::printf("%s\n", line.c_str()); }
};

std::string read_file(const std::string& path, bool* ok) {
    std::ifstream f(path);
    if (!f) { *ok = false; return {}; }
    std::ostringstream ss;
    ss << f.rdbuf();
    *ok = true;
    return ss.str();
}

void usage(const char* a0) {
    std::printf(
        "用法: %s <file> [--engine basic|lua] [--check] [--trace] [--steps N] [--quiet] [--sim-bus]\n"
        "  --engine S 指定引擎：basic(默认，.bas) | lua(.lua 自动识别)\n"
        "  --check   只做语法检查\n"
        "  --trace   单步跟踪\n"
        "  --steps N 步数预算（BASIC=语句级 / Lua=VM 指令级，默认 5000000）\n"
        "  --quiet   不打印 dry 命令回显\n"
        "  --sim-bus 模拟总线已连接，走通整条总线初始化逻辑\n",
        a0);
}

} // namespace

int main(int argc, char** argv) {
    std::string path;
    std::string engine_name;                 // 空 = 按扩展名自动判断
    bool check_only = false, trace = false, quiet = false, sim_bus = false;
    unsigned long long steps = 5000000ULL;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else if (a == "--check") { check_only = true; }
        else if (a == "--trace") { trace = true; }
        else if (a == "--quiet") { quiet = true; }
        else if (a == "--sim-bus") { sim_bus = true; }
        else if (a == "--engine" && i + 1 < argc) { engine_name = argv[++i]; }
        else if (a == "--steps" && i + 1 < argc) { steps = std::strtoull(argv[++i], nullptr, 0); }
        else if (!a.empty() && a[0] != '-') { path = a; }
        else { std::fprintf(stderr, "未知参数: %s\n", a.c_str()); usage(argv[0]); return 2; }
    }
    if (path.empty()) { usage(argv[0]); return 2; }

    // 引擎选择：显式 --engine 优先；否则 .lua 扩展名走 Lua，其余按 BASIC
    if (engine_name.empty()) {
        const bool lua_ext = path.size() > 4 && path.compare(path.size() - 4, 4, ".lua") == 0;
        engine_name = lua_ext ? "lua" : "basic";
    }

    bool ok = false;
    const std::string src = read_file(path, &ok);
    if (!ok) { std::fprintf(stderr, "无法读取脚本: %s\n", path.c_str()); return 1; }

    DryHost host;
    host.show_cmd = !quiet;
    host.sim_bus = sim_bus;

    // 规则：同一时刻只加载一种脚本语言（见 engine_rule.h）——
    // 解析引擎名，并校验其与脚本文件扩展名一致后，经单引擎槽装载。
    ScriptLanguage lang;
    if (!parse_script_language(engine_name, &lang)) {
        std::fprintf(stderr, "未知脚本引擎: %s（可选 basic | lua）\n", engine_name.c_str());
        return 2;
    }
    std::string lang_err;
    if (!script_file_matches_language(lang, path, &lang_err)) {
        std::fprintf(stderr, "%s\n", lang_err.c_str());
        return 2;
    }

    ScriptEngineSlot slot;
    std::string slot_err;
    if (!slot.load(lang, &host, &slot_err)) {
        std::fprintf(stderr, "%s\n", slot_err.c_str());
        return 2;
    }
    IScriptEngine* e = slot.engine();

    std::string err;
    if (!e->compile(src, &err)) {
        std::fprintf(stderr, "[编译失败/%s] %s\n", e->name(), err.c_str());
        return 1;
    }
    std::printf("[编译通过/%s] %s（%zu 个标签）\n", e->name(), path.c_str(), e->labels().size());
    if (check_only) return 0;

    e->set_host(&host);
    e->set_trace(trace);
    const IScriptEngine::Status st = e->run(steps);

    std::printf("[运行结果] %s（%llu 步）\n", IScriptEngine::status_name(st), e->steps());
    if (st != IScriptEngine::Status::DONE) {
        std::fprintf(stderr, "[错误] 第 %d 行: %s\n", e->error_line(), e->error().c_str());
        return 1;
    }
    return 0;
}
