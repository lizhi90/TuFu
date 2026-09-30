// engine_rule.h —— 「控制器同一时刻只加载一种脚本语言」这条规则的唯一定义与落地点
//
// 规则（本控制器约束）：
//   BASIC 引擎（script.h）与 Lua 引擎（lua_engine.h）**互斥，二选一**：
//   任一时刻控制器**只能加载/运行一种脚本语言的程序**，不允许同时加载两套引擎，
//   也不允许「用 A 引擎去跑 B 语言的脚本」（例如 SCRIPT_ENGINE=basic 却给 .lua 文件）。
//
// 为什么要有这条规则：
//   * 两引擎共用同一份设备宿主 `ScriptHost` 与 `kx::Shared`（见 09 §3.1/§5），
//     同时运行会让单槽命令相互覆盖、位置/状态读回错乱；
//   * 「用户可选」= 配置二选一，**不是同时运行**。
//
// 落地点（谁依赖本文件）：
//   * 配置层 `config.cpp`：校验并规范化 SCRIPT_ENGINE（非法值明确报错，绝不静默）；
//   * 主程序 `main.cpp` 的 `script_loop`：经 `ScriptEngineSlot` 装载唯一引擎；
//   * 调试工具 `tools/script_run.cpp`：同一套解析与装载逻辑（命令行只跑一个引擎）；
//   * 纯逻辑自测 `tools/engine_rule_test.cpp`：不需要 IgH / 硬件即可验证本规则。
#pragma once

#include "engine.h"   // kx::IScriptEngine（语言无关接口）+ 前置声明 ScriptHost

#include <cctype>
#include <memory>
#include <string>

namespace kx {

// ---------------------------------------------------------------------------
// 允许的脚本语言（同一时刻只能选其一）
// ---------------------------------------------------------------------------
enum class ScriptLanguage { BASIC = 0, LUA };

// 解析 SCRIPT_ENGINE 字符串：大小写不敏感、忽略首尾空白。
//   "" / "basic"（含 "BASIC"/" Basic "）-> BASIC；"lua"/"LUA" -> LUA；
//   其它值 -> 返回 false（调用方**必须明确报错**，不得静默回退）。
inline bool parse_script_language(const std::string& name, ScriptLanguage* out) {
    size_t b = 0, e = name.size();
    while (b < e && std::isspace((unsigned char)name[b])) ++b;
    while (e > b && std::isspace((unsigned char)name[e - 1])) --e;
    std::string k = name.substr(b, e - b);
    for (char& c : k) c = (char)std::tolower((unsigned char)c);

    if (k.empty() || k == "basic") { if (out) *out = ScriptLanguage::BASIC; return true; }
    if (k == "lua")                { if (out) *out = ScriptLanguage::LUA;   return true; }
    return false;
}

// 规范化名称（"basic" / "lua"），用于日志 / 写回配置
inline const char* script_language_name(ScriptLanguage lang) {
    return lang == ScriptLanguage::LUA ? "lua" : "basic";
}

// 脚本文件是否以 .lua / .bas 结尾（与 tools/script_run.cpp 既有约定一致，大小写敏感）
inline bool script_file_is_lua(const std::string& file) {
    return file.size() > 4 && file.compare(file.size() - 4, 4, ".lua") == 0;
}
inline bool script_file_is_basic(const std::string& file) {
    return file.size() > 4 && file.compare(file.size() - 4, 4, ".bas") == 0;
}

// 由脚本文件扩展名推断语言：.lua -> LUA，其余按 BASIC（兼容无扩展名/历史 .bas 路径）
inline ScriptLanguage script_language_from_file(const std::string& file) {
    return script_file_is_lua(file) ? ScriptLanguage::LUA : ScriptLanguage::BASIC;
}

// 文件扩展名与所选引擎是否一致（规则的直接校验）：
//   * 仅当扩展名是**已知**的 .lua/.bas 且与引擎冲突时才报错；
//   * 无扩展名或其它扩展名视为「无法判断」，放行（避免误伤历史配置）。
inline bool script_file_matches_language(ScriptLanguage lang, const std::string& file,
                                         std::string* err) {
    const bool is_lua = script_file_is_lua(file);
    const bool is_bas = script_file_is_basic(file);
    if (!is_lua && !is_bas) return true;                 // 无法判断 -> 放行
    if (is_lua == (lang == ScriptLanguage::LUA)) return true;
    if (err) {
        *err = std::string("脚本文件扩展名(.lua/.bas)与脚本引擎(") +
               script_language_name(lang) + ")不一致：" + file;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 单一脚本引擎槽：全进程**只允许**装一个引擎
//   * 这是「控制器同一时刻只加载一种脚本语言」在代码里的强制点：
//     已装载时再 load() 一律失败（含同语言重复装载），必须先 unload()；
//   * 语言随槽保存，engine()/language() 供上层查询；
//   * 槽析构自动卸载，引擎生命周期随槽结束。
// ---------------------------------------------------------------------------
class ScriptEngineSlot {
public:
    ScriptEngineSlot() = default;
    ~ScriptEngineSlot();
    ScriptEngineSlot(const ScriptEngineSlot&) = delete;
    ScriptEngineSlot& operator=(const ScriptEngineSlot&) = delete;

    // 装载唯一引擎。host 可为空（调试/语法检查场景）。
    // 失败（槽已占用 / 未知语言）返回 false 并写 err。
    bool load(ScriptLanguage lang, ScriptHost* host, std::string* err = nullptr);
    void unload();

    bool           loaded() const   { return engine_ != nullptr; }
    ScriptLanguage language() const { return lang_; }
    IScriptEngine* engine() const   { return engine_.get(); }

    // ---- D4 热更新（docs/planA/13 §5.1）----
    // create_staged：创建一个**同语言**的新引擎实例（不碰槽内实例），供「旁路编译」：
    //   运行中脚本不受影响，新源码先在暂存实例上编译校验。失败返回 nullptr 并写 err。
    //   返回指针由调用方接管（用 replace 换入，或 delete 丢弃）。
    IScriptEngine* create_staged(ScriptHost* host, std::string* err = nullptr);

    // replace：用 new_eng（必须由本槽 create_staged 创建、同语言）原子替换槽内实例，
    // 返回被换出的旧实例（调用方持有，待其运行线程结束后自行 delete）。
    IScriptEngine* replace(IScriptEngine* new_eng);

private:
    std::unique_ptr<IScriptEngine> engine_;
    ScriptLanguage lang_ = ScriptLanguage::BASIC;
};

} // namespace kx
