// lua_engine.cpp —— Lua 脚本引擎实现（vendor Lua 5.4）
//
// 组成：
//   1) 沙箱：只开 base(裁剪)/string/table/math，禁 os/io/debug/package 与 load 族；
//   2) 绑定：命令表批量注册**同名全局函数** -> ScriptHost::call；端口语法糖改显式函数；
//           `ww.call` 通用转调 + `ww.ret` 读最近返回值；
//   3) 预算/中止：lua_sethook(LUA_MASKCOUNT[|LUA_MASKLINE])，超预算或 abort 标志即抛错；
//   4) 值桥接：Value(NIL/NUM/STR) <-> Lua(nil/number/string/boolean)；
//   5) 状态映射：与 IScriptEngine::Status 对齐（见 09 §3.2）。
#include "script/lua_engine.h"

#include "script/command_table.h"   // motion_command_names()（header-only，无重量依赖）
#include "script/script_host.h"     // kx::ScriptHost 完整定义（值桥接用）
#include "script/script_value.h"    // kx::Value 完整定义

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "lua.hpp"                // third_party/lua（extern "C" 包装）

namespace kx {

// ---------------------------------------------------------------------------
// 内部状态
// ---------------------------------------------------------------------------
struct LuaEngine::Impl {
    ScriptHost* host = nullptr;
    lua_State*  L    = nullptr;

    int  chunk_ref = LUA_NOREF;             // 已编译 chunk 的 registry 引用
    IScriptEngine::Status status = IScriptEngine::Status::READY;

    std::string error;
    int         error_line = 0;

    unsigned long long steps     = 0;        // 本次运行的 VM 指令数（hook 近似）
    unsigned long long max_steps = 0;

    std::atomic<bool> abort_req{false};
    bool trace_on   = false;
    bool budget_hit = false;
    bool abort_hit  = false;

    Value last_ret;                          // 最近一次命令返回值（供 ww.ret）
    std::vector<std::string> api_names;      // 已注册 API/命令名（供 labels）
    std::vector<std::string> builtin_names;  // 建环境时的全局名快照（沙箱库+API，clear_vars 保留）

    // ---- D5（v0.8.0）：行级 hook + 断点/暂停/单步；会话线程调、worker 线程消费 ----
    mutable std::mutex      dbg_mtx;
    std::condition_variable dbg_cv;
    std::set<int>           bps;             // 断点行号
    bool  line_hooks = false;                // 是否在 run 时挂 LUA_MASKLINE（调试装载时开）
    bool  pause_req  = false;                // 请求在下个行事件挂起
    bool  step_req   = false;                // 单步：恢复后在下个行事件再挂
    bool  resume_req = false;                // 恢复请求（配合 cv 谓词）
    bool  paused_flag = false;               // 已挂起（供 script.status/PAUSED 事件）
    int   cur_line   = 0;                    // 最近执行到的行（1 基）
};

namespace {

// chunk 名：'=' 前缀告诉 Lua「按字面使用这个名字」，否则会被包成 [string "kx_lua"]
// 而让错误消息里的 "kx_lua:行号:" 前缀失效（parse_line 依赖该前缀取行号）。
const char* const kChunkDisplay = "kx_lua";   // 错误消息中出现的 chunk 名
const char* const kChunkName    = "=kx_lua";  // 传给 luaL_loadbufferx 的 source 名
const int         kHookCount    = 100;        // 每 100 条 VM 指令回调一次（预算精度/开销折中）

// 用不可打印前置字节标记「预算耗尽 / 外部中止」，避免与脚本自发 error() 混淆
const char* const kBudgetMark = "\001zm:budget";
const char* const kAbortMark  = "\001zm:abort";

LuaEngine::Impl* impl_of(lua_State* L) {
    return *static_cast<LuaEngine::Impl**>(lua_getextraspace(L));
}

// ---- 错误消息里的行号：kx_lua:12: ... ----
int parse_line(const std::string& msg) {
    const std::string key = std::string(kChunkDisplay) + ":";
    const size_t pos = msg.find(key);
    if (pos == std::string::npos) return 0;
    size_t i = pos + key.size();
    int line = 0;
    bool any = false;
    while (i < msg.size() && std::isdigit((unsigned char)msg[i])) {
        line = line * 10 + (msg[i] - '0');
        ++i;
        any = true;
    }
    return any ? line : 0;
}

std::string strip_marks(std::string m) {
    for (const char* mark : {kBudgetMark, kAbortMark}) {
        const std::string s(mark);
        size_t p;
        while ((p = m.find(s)) != std::string::npos) m.erase(p, s.size());
    }
    return m;
}

// ---- 值桥接 ----
bool lua_to_value(lua_State* L, int idx, Value* out, std::string* err) {
    switch (lua_type(L, idx)) {
        case LUA_TNIL:     *out = Value();                                  return true;
        case LUA_TNUMBER:  *out = Value::number(lua_tonumber(L, idx));      return true;
        case LUA_TBOOLEAN: *out = Value::number(lua_toboolean(L, idx) ? 1.0 : 0.0); return true;
        case LUA_TSTRING: {
            size_t n = 0;
            const char* s = lua_tolstring(L, idx, &n);
            *out = Value::text(std::string(s ? s : "", n));
            return true;
        }
        default:
            if (err) *err = "参数类型不支持（仅 number/string/boolean/nil；数组请用 PORT_* 函数）";
            return false;
    }
}

void push_value(lua_State* L, const Value& v) {
    switch (v.type) {
        case Value::NUM: lua_pushnumber(L, v.num); break;
        case Value::STR: lua_pushlstring(L, v.str.data(), v.str.size()); break;
        case Value::NIL:
        default:         lua_pushnil(L); break;
    }
}

Value value_of_top(lua_State* L, int idx) {
    Value v;
    if (!lua_to_value(L, idx, &v, nullptr)) return Value();
    return v;
}

// 收集 _G 的全部字符串键（用于快照环境基线）
void collect_global_names(lua_State* L, std::vector<std::string>& out) {
    out.clear();
    lua_pushglobaltable(L);
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            size_t n = 0;
            const char* k = lua_tolstring(L, -2, &n);
            if (k) out.emplace_back(k, n);
        }
        lua_pop(L, 1);                       // 弹 value，保留 key
    }
    lua_pop(L, 1);                           // 弹 _G
}

std::string num_text(double d) {
    char buf[32];
    if (d == (double)(long long)d) std::snprintf(buf, sizeof buf, "%lld", (long long)d);
    else                           std::snprintf(buf, sizeof buf, "%g", d);
    return buf;
}

// ---- 统一命令转发（含返回值/错误/中止语义）----
int do_call(lua_State* L, LuaEngine::Impl* im, const std::string& name, int first_arg) {
    if (!im->host) return luaL_error(L, "脚本引擎未就绪（无设备宿主）");

    const int nargs = lua_gettop(L);
    std::vector<Value> args;
    args.reserve((size_t)std::max(0, nargs - first_arg + 1));
    for (int i = first_arg; i <= nargs; ++i) {
        Value v;
        std::string e;
        if (!lua_to_value(L, i, &v, &e))
            return luaL_error(L, "命令 %s 的第 %d 个参数: %s", name.c_str(), i - first_arg + 1, e.c_str());
        args.push_back(v);
    }

    Value ret;
    std::string err;
    const int rc = im->host->call(name, args, &ret, &err);

    if (rc == 0) {
        im->last_ret = ret;
        push_value(L, ret);
        return 1;
    }
    if (rc == 3) { im->abort_hit = true; return luaL_error(L, "%s", kAbortMark); }
    if (rc == 1) return luaL_error(L, "未绑定/不认识的命令: %s", name.c_str());
    return luaL_error(L, "%s", err.empty() ? (name + " 执行失败").c_str() : err.c_str());
}

// 全局命令函数：upvalue(1) = 命令名
int lua_cmd_dispatch(lua_State* L) {
    LuaEngine::Impl* im = impl_of(L);
    if (!im) return luaL_error(L, "内部错误：引擎状态缺失");
    const char* name = lua_tostring(L, lua_upvalueindex(1));
    return do_call(L, im, name ? name : "", 1);
}

// ww.call(name, ...)：通用转调入口（逃生舱）
int lua_ww_call(lua_State* L) {
    LuaEngine::Impl* im = impl_of(L);
    if (!im) return luaL_error(L, "内部错误：引擎状态缺失");
    if (lua_gettop(L) < 1 || !lua_isstring(L, 1))
        return luaL_error(L, "ww.call(name, ...) 需要命令名字符串");
    const std::string name = lua_tostring(L, 1);
    return do_call(L, im, name, 2);
}

// ww.ret()：最近一次命令返回值（与 BASIC 的 RETURN 系统变量同源同义）
int lua_ww_ret(lua_State* L) {
    LuaEngine::Impl* im = impl_of(L);
    if (!im) return luaL_error(L, "内部错误：引擎状态缺失");
    push_value(L, im->last_ret);
    return 1;
}

// PORT_PRINT(n, s)：整包发送、不附加换行（对应 BASIC `PRINT #n, s`）
int lua_port_print(lua_State* L) {
    LuaEngine::Impl* im = impl_of(L);
    if (!im) return luaL_error(L, "内部错误：引擎状态缺失");
    if (lua_gettop(L) < 2) return luaL_error(L, "PORT_PRINT(n, s) 需要通道与数据");
    // 栈上 [n, s] 与命令 PRINT 的 [通道, 字节串] 同形，直接转发
    return do_call(L, im, "PRINT", 1);
}

// PORT_PUTCHAR(n, tbl|s[, start[, len]])：按原始字节发送（含 0 字节）
int lua_port_putchar(lua_State* L) {
    LuaEngine::Impl* im = impl_of(L);
    if (!im) return luaL_error(L, "内部错误：引擎状态缺失");
    if (lua_gettop(L) < 2) return luaL_error(L, "PORT_PUTCHAR(n, tbl|s[, start[, len]])");

    if (lua_istable(L, 2)) {                  // 表（1 基）-> 字节串
        const lua_Integer start = lua_gettop(L) >= 3 ? (lua_Integer)luaL_checknumber(L, 3) : 1;
        const lua_Integer len   = lua_gettop(L) >= 4 ? (lua_Integer)luaL_checknumber(L, 4)
                                                     : (lua_Integer)lua_rawlen(L, 2);
        std::string bytes;
        bytes.reserve(len > 0 ? (size_t)len : 0);
        for (lua_Integer i = 0; i < len; ++i) {
            if (lua_rawgeti(L, 2, start + i) == LUA_TNUMBER) {   // 越界 -> nil，按 0 字节
                bytes.push_back((char)(unsigned char)lua_tointeger(L, -1));
            } else {
                bytes.push_back('\0');
            }
            lua_pop(L, 1);
        }
        lua_pushlstring(L, bytes.data(), bytes.size());
        lua_replace(L, 2);                    // 用字节串替换表
        lua_settop(L, 2);
    } else if (!lua_isstring(L, 2)) {
        return luaL_error(L, "PORT_PUTCHAR 第 2 参数应为字符串或表");
    }
    return do_call(L, im, "PUTCHAR", 1);
}

// PORT_GET(n, tbl[, max[, idx]])：非阻塞接收 -> 写入表（1 基），返回字节数
//   idx = 客户端下标（多客户端端口，如 502 的触摸屏=0/机器人=1；省略=0）
int lua_port_get(lua_State* L) {
    LuaEngine::Impl* im = impl_of(L);
    if (!im) return luaL_error(L, "内部错误：引擎状态缺失");
    if (lua_gettop(L) < 2 || !lua_istable(L, 2))
        return luaL_error(L, "PORT_GET(n, tbl[, max[, idx]]) 需要表参数");
    const int port = (int)luaL_checknumber(L, 1);
    const double maxb = lua_gettop(L) >= 3 ? luaL_checknumber(L, 3) : 0.0;
    const double idx  = lua_gettop(L) >= 4 ? luaL_checknumber(L, 4) : 0.0;   // 多客户端（2026-09-28）

    std::vector<Value> args = { Value::number(port), Value::text(""),
                                Value::number(0),    Value::number(maxb),
                                Value::number(idx) };
    Value ret;
    std::string err;
    const int rc = im->host ? im->host->call("GET", args, &ret, &err) : -1;
    if (rc == 3) { im->abort_hit = true; return luaL_error(L, "%s", kAbortMark); }
    if (rc == 1) return luaL_error(L, "GET 未绑定");
    if (rc != 0) return luaL_error(L, "%s", err.empty() ? "GET 执行失败" : err.c_str());

    const std::string bytes = ret.to_text();
    for (size_t i = 0; i < bytes.size(); ++i) {
        lua_pushinteger(L, (lua_Integer)(unsigned char)bytes[i]);
        lua_rawseti(L, 2, (lua_Integer)(i + 1));     // 1 基（显式差异，见 09 §10）
    }
    lua_pushinteger(L, (lua_Integer)bytes.size());
    return 1;
}

// ---- 沙箱：只开白名单标准库，禁一切越权入口 ----
void open_sandbox(lua_State* L) {
    luaL_requiref(L, "_G",            luaopen_base,   1); lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME,  luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME,  luaopen_table,  1); lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math,   1); lua_pop(L, 1);

    // 移除加载/反射/系统类入口（能对文件/进程/模块产生副作用的一律不给）
    static const char* const kDrop[] = {
        "dofile", "loadfile", "load", "loadstring", "require",
        "os", "io", "debug", "package",
    };
    for (const char* n : kDrop) { lua_pushnil(L); lua_setglobal(L, n); }
}

// ---- 注册本项目 API（同名命令 + 端口显式函数 + ww 命名空间）----
void register_api(lua_State* L, LuaEngine::Impl* im) {
    // 语法糖类命令：BASIC 里靠 #/数组语法，Lua 侧改 PORT_* 显式函数，故不注册同名全局
    auto skipped = [](const char* n) {
        return std::strcmp(n, "GET") == 0 ||
               std::strcmp(n, "PRINT") == 0 ||
               std::strcmp(n, "PUTCHAR") == 0;
    };

    for (const char* const* p = motion_command_names(); p && *p; ++p) {
        if (skipped(*p)) continue;
        lua_pushstring(L, *p);
        lua_pushcclosure(L, lua_cmd_dispatch, 1);   // upvalue = 命令名
        lua_setglobal(L, *p);
        im->api_names.push_back(*p);
    }

    struct { const char* name; lua_CFunction fn; } const kExtras[] = {
        {"PORT_PRINT",   lua_port_print},
        {"PORT_PUTCHAR", lua_port_putchar},
        {"PORT_GET",     lua_port_get},
    };
    for (const auto& e : kExtras) {
        lua_pushcfunction(L, e.fn);
        lua_setglobal(L, e.name);
        im->api_names.push_back(e.name);
    }

    // ww 命名空间（本项目自有前缀，非 kx/正运动）
    lua_newtable(L);
    lua_pushcfunction(L, lua_ww_call); lua_setfield(L, -2, "call");
    lua_pushcfunction(L, lua_ww_ret);  lua_setfield(L, -2, "ret");
    lua_setglobal(L, "ww");
    im->api_names.push_back("ww");
}

// ---- 预算/中止（COUNT）+ D5 行级门控（LINE）hook ----
void lua_hook(lua_State* L, lua_Debug* ar) {
    LuaEngine::Impl* im = impl_of(L);
    if (!im) return;

    if (ar->event == LUA_HOOKCOUNT) {
        im->steps += (unsigned long long)kHookCount;
        if (im->max_steps && im->steps > im->max_steps) {
            im->budget_hit = true;
            luaL_error(L, "%s", kBudgetMark);
            return;
        }
        if (im->abort_req.load() || (im->host && im->host->aborted())) {
            im->abort_hit = true;
            luaL_error(L, "%s", kAbortMark);
            return;
        }
        return;
    }
    if (ar->event != LUA_HOOKLINE) return;

    // 注意：hook 传入的 lua_Debug 里带内部激活记录指针（i_ci），**不能 memset 清零**；
    // 要补充字段（short_src 等）先按值拷贝再 lua_getinfo。
    const int line = ar->currentline;

    if (im->trace_on && im->host) {
        lua_Debug info = *ar;
        lua_getinfo(L, "Sl", &info);
        char buf[256];
        std::snprintf(buf, sizeof buf, "[lua] %s:%d",
                      info.short_src ? info.short_src : "?", line);
        im->host->print(buf);
    }

    // D5：仅「命中断点 / 请求暂停 / 单步」才挂起（快速路径零阻塞）
    {
        std::lock_guard<std::mutex> lk(im->dbg_mtx);
        if (line > 0) im->cur_line = line;
        const bool hit = im->pause_req || im->step_req || (line > 0 && im->bps.count(line) > 0);
        if (!hit) return;
        im->pause_req   = false;
        im->step_req    = false;
        im->paused_flag = true;
    }
    std::unique_lock<std::mutex> lk(im->dbg_mtx);
    im->dbg_cv.wait(lk, [&] {
        return im->resume_req || im->abort_req.load() || (im->host && im->host->aborted());
    });
    const bool stepping = im->step_req;
    im->step_req   = false;
    im->resume_req = false;
    im->paused_flag = false;
    if (stepping) im->pause_req = true;   // 单步：本行执行完后，下个行事件再停
    const bool abort_now = im->abort_req.load() || (im->host && im->host->aborted());
    lk.unlock();                          // luaL_error 走 longjmp：必须先释放锁（析构不会执行）
    if (abort_now) {
        im->abort_hit = true;
        luaL_error(L, "%s", kAbortMark);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
LuaEngine::LuaEngine(ScriptHost* host) : p_(new Impl()) {
    p_->host = host;
    p_->L = luaL_newstate();
    if (!p_->L) {
        p_->status = Status::RUNTIME_ERROR;
        p_->error  = "luaL_newstate 失败";
        return;
    }
    *static_cast<Impl**>(lua_getextraspace(p_->L)) = p_.get();

    // GC 参数（见 docs/planA/09 §5 / 任务 L-11）：
    //   采用**分代模式**（LUA_GCGEN），minormul/majormul 传 0 → 用 Lua 5.4 内置默认
    //   （minor 20% / major 100%）。分代模式对"大量短命临时值"的脚本吞吐更好、停顿更少。
    // ⚠ 说明：GC 停顿本质不确定，故本引擎 **不承诺硬实时**。脚本运行在独立的**非 RT** 线程，
    //   绝不在 RT 1ms 循环里跑；运动时序由 RT 线程 + 参数信箱保证（见 docs/planA/04）。
    lua_gc(p_->L, LUA_GCGEN, 0, 0);

    open_sandbox(p_->L);
    register_api(p_->L, p_.get());

    // 快照环境基线：基础库 + 已注册 API。clear_vars 只删这份快照之外的键，
    // 从而只清「脚本变量」，不会误删 type/string/table... 及命令函数。
    collect_global_names(p_->L, p_->builtin_names);
}

LuaEngine::~LuaEngine() {
    if (p_ && p_->L) lua_close(p_->L);
}

void LuaEngine::set_host(ScriptHost* host) { p_->host = host; }

// ---------------------------------------------------------------------------
// 编译
// ---------------------------------------------------------------------------
bool LuaEngine::compile(const std::string& src, std::string* err) {
    Impl& im = *p_;
    im.error.clear();
    im.error_line = 0;
    im.steps = 0;
    im.budget_hit = im.abort_hit = false;

    if (!im.L) { im.status = Status::COMPILE_ERROR; im.error = "Lua 状态未初始化"; if (err) *err = im.error; return false; }

    if (im.chunk_ref != LUA_NOREF) {
        luaL_unref(im.L, LUA_REGISTRYINDEX, im.chunk_ref);
        im.chunk_ref = LUA_NOREF;
    }

    const int rc = luaL_loadbufferx(im.L, src.data(), src.size(), kChunkName, "t");
    if (rc != LUA_OK) {
        size_t n = 0;
        const char* msg = lua_tolstring(im.L, -1, &n);
        im.error = msg ? std::string(msg, n) : "编译失败";
        lua_pop(im.L, 1);
        im.error_line = parse_line(im.error);
        im.status = Status::COMPILE_ERROR;
        if (err) *err = im.error;
        return false;
    }
    im.chunk_ref = luaL_ref(im.L, LUA_REGISTRYINDEX);   // 弹出 chunk 并持有
    im.status = Status::READY;
    return true;
}

// ---------------------------------------------------------------------------
// 运行
// ---------------------------------------------------------------------------
IScriptEngine::Status LuaEngine::run(unsigned long long max_steps) {
    Impl& im = *p_;
    if (im.status == Status::COMPILE_ERROR) return im.status;
    if (!im.L || im.chunk_ref == LUA_NOREF) {
        im.status = Status::RUNTIME_ERROR;
        im.error  = "没有可运行的脚本（先 compile）";
        return im.status;
    }

    im.max_steps = max_steps;
    im.steps     = 0;
    im.budget_hit = im.abort_hit = false;
    im.error.clear();
    im.error_line = 0;
    {   // D5：每次运行重置挂起/单步状态（断点集保留，供连续运行）
        std::lock_guard<std::mutex> lk(im.dbg_mtx);
        im.pause_req = im.step_req = im.resume_req = im.paused_flag = false;
        im.cur_line = 0;
    }

    auto finish = [&](Status st) { im.abort_req.store(false); im.status = st; return st; };

    // 运行前已请求中止（或宿主已要求停止）：直接中止，避免短脚本漏检
    if (im.abort_req.load() || (im.host && im.host->aborted())) {
        im.abort_hit = true;
        im.error = "脚本被中止";
        return finish(Status::ABORTED);
    }

    lua_rawgeti(im.L, LUA_REGISTRYINDEX, im.chunk_ref);   // push chunk
    int mask = LUA_MASKCOUNT;
    if (im.trace_on || im.line_hooks) mask |= LUA_MASKLINE;   // 行级 hook 仅调试装载时开（生产零开销）
    lua_sethook(im.L, lua_hook, mask, kHookCount);

    const int rc = lua_pcall(im.L, 0, 0, 0);

    lua_sethook(im.L, nullptr, 0, 0);

    if (rc == LUA_OK) return finish(Status::DONE);

    size_t n = 0;
    const char* msg = lua_tolstring(im.L, -1, &n);
    const std::string m = msg ? std::string(msg, n) : std::string("未知错误");
    lua_pop(im.L, 1);

    if (m.find(kBudgetMark) != std::string::npos) {
        im.error = "超出脚本步数预算（防死循环）";
        return finish(Status::BUDGET_EXCEEDED);
    }
    if (m.find(kAbortMark) != std::string::npos || im.abort_hit) {
        im.error = "脚本被中止";
        return finish(Status::ABORTED);
    }
    im.error      = strip_marks(m);
    im.error_line = parse_line(m);
    return finish(Status::RUNTIME_ERROR);
}

IScriptEngine::Status LuaEngine::status() const { return p_->status; }
const std::string&    LuaEngine::error() const  { return p_->error; }
int                   LuaEngine::error_line() const { return p_->error_line; }
unsigned long long    LuaEngine::steps() const  { return p_->steps; }

void LuaEngine::request_abort() {
    p_->abort_req.store(true);
    // 关键：唤醒可能正阻塞在行级 hook（D5 挂起）里的 worker，否则中止无法生效
    std::lock_guard<std::mutex> lk(p_->dbg_mtx);
    p_->dbg_cv.notify_all();
}

// ---------------------------------------------------------------------------
// D5：断点 / 暂停 / 单步（行级；语义对齐 BASIC 的语句级 dbg_gate）
// ---------------------------------------------------------------------------
void LuaEngine::add_breakpoint(int line) {
    if (line <= 0) return;
    std::lock_guard<std::mutex> lk(p_->dbg_mtx);
    p_->bps.insert(line);
}

void LuaEngine::remove_breakpoint(int line) {
    std::lock_guard<std::mutex> lk(p_->dbg_mtx);
    p_->bps.erase(line);
}

void LuaEngine::clear_breakpoints() {
    std::lock_guard<std::mutex> lk(p_->dbg_mtx);
    p_->bps.clear();
}

std::vector<int> LuaEngine::breakpoints() const {
    std::lock_guard<std::mutex> lk(p_->dbg_mtx);
    return std::vector<int>(p_->bps.begin(), p_->bps.end());
}

void LuaEngine::request_pause() {
    std::lock_guard<std::mutex> lk(p_->dbg_mtx);
    p_->pause_req = true;                 // 未运行时无效（run 开始会清）
}

void LuaEngine::request_step() {
    std::lock_guard<std::mutex> lk(p_->dbg_mtx);
    if (!p_->paused_flag) return;         // 仅暂停中有效
    p_->step_req   = true;
    p_->resume_req = true;
    p_->dbg_cv.notify_all();
}

void LuaEngine::resume() {
    std::lock_guard<std::mutex> lk(p_->dbg_mtx);
    if (!p_->paused_flag) return;
    p_->resume_req = true;
    p_->dbg_cv.notify_all();
}

bool LuaEngine::paused() const {
    std::lock_guard<std::mutex> lk(p_->dbg_mtx);
    return p_->paused_flag;
}

int LuaEngine::current_line() const {
    std::lock_guard<std::mutex> lk(p_->dbg_mtx);
    return p_->cur_line;
}

void LuaEngine::enable_line_hooks(bool on) { p_->line_hooks = on; }

// ---------------------------------------------------------------------------
// 变量内省
// ---------------------------------------------------------------------------
Value LuaEngine::get_var(const std::string& name) const {
    Impl& im = *p_;
    if (!im.L) return Value();
    lua_getglobal(im.L, name.c_str());
    Value v = value_of_top(im.L, -1);
    lua_pop(im.L, 1);
    return v;
}

void LuaEngine::set_var(const std::string& name, const Value& v) {
    Impl& im = *p_;
    if (!im.L) return;
    push_value(im.L, v);
    lua_setglobal(im.L, name.c_str());
}

std::vector<std::string> LuaEngine::list_vars() const {
    Impl& im = *p_;
    std::vector<std::string> out;
    if (!im.L) return out;
    lua_pushglobaltable(im.L);                 // _G
    lua_pushnil(im.L);
    while (lua_next(im.L, -2) != 0) {          // key(-2) value(-1)
        if (lua_type(im.L, -2) == LUA_TSTRING) {
            const char* k = lua_tostring(im.L, -2);
            switch (lua_type(im.L, -1)) {
                case LUA_TNUMBER:  out.push_back(std::string(k) + " = " + num_text(lua_tonumber(im.L, -1))); break;
                case LUA_TSTRING:  out.push_back(std::string(k) + " = \"" + lua_tostring(im.L, -1) + "\""); break;
                case LUA_TBOOLEAN: out.push_back(std::string(k) + " = " + (lua_toboolean(im.L, -1) ? "true" : "false")); break;
                default: break;                // 函数/表/其他类型不作为「变量」列出
            }
        }
        lua_pop(im.L, 1);                      // 弹 value，保留 key
    }
    lua_pop(im.L, 1);                          // 弹 _G
    std::sort(out.begin(), out.end());
    return out;
}

void LuaEngine::clear_vars() {
    Impl& im = *p_;
    if (!im.L) return;
    const std::vector<std::string>& keep = im.builtin_names;   // 环境基线（基础库 + API）保留

    // 先收集待删键，遍历结束后再删（遍历中改表会破坏 lua_next 的遍历保证）
    std::vector<std::string> drop;
    lua_pushglobaltable(im.L);
    lua_pushnil(im.L);
    while (lua_next(im.L, -2) != 0) {
        if (lua_type(im.L, -2) == LUA_TSTRING) {
            const std::string k = lua_tostring(im.L, -2);
            if (std::find(keep.begin(), keep.end(), k) == keep.end()) drop.push_back(k);
        }
        lua_pop(im.L, 1);
    }
    lua_pop(im.L, 1);

    for (const std::string& k : drop) {
        lua_pushnil(im.L);
        lua_setglobal(im.L, k.c_str());
    }
}

void LuaEngine::set_trace(bool on) { p_->trace_on = on; }
bool LuaEngine::trace() const      { return p_->trace_on; }

std::vector<std::string> LuaEngine::labels() const {
    std::vector<std::string> out = p_->api_names;   // Lua 无标签：返回已注册 API 名清单
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace kx
