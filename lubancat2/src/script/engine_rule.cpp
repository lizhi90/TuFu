// engine_rule.cpp —— ScriptEngineSlot 的实现（本文件是唯一会 new 出引擎的地方）
//
// 说明：语言解析 / 文件一致性校验都是 head 内联的纯逻辑（供 config_test 等零依赖引用）；
//       仅「按语言造引擎」这一步需要链接两套引擎实现，故单独放在本编译单元。
#include "engine_rule.h"

#include "lua_engine.h"   // kx::LuaEngine
#include "script.h"       // kx::ScriptEngine

namespace kx {

ScriptEngineSlot::~ScriptEngineSlot() { unload(); }

bool ScriptEngineSlot::load(ScriptLanguage lang, ScriptHost* host, std::string* err) {
    if (engine_) {
        if (err) {
            *err = std::string("已装载 ") + script_language_name(lang_) +
                   " 引擎，拒绝再装载 " + script_language_name(lang) +
                   " 引擎：控制器同一时刻只允许一种脚本语言（需先卸载）";
        }
        return false;
    }

    std::unique_ptr<IScriptEngine> eng;
    switch (lang) {
        case ScriptLanguage::BASIC: eng.reset(new ScriptEngine(host)); break;
        case ScriptLanguage::LUA:   eng.reset(new LuaEngine(host));    break;
        default:
            if (err) *err = "未知脚本语言";
            return false;
    }
    if (!eng) {
        if (err) *err = "脚本引擎创建失败";
        return false;
    }

    engine_ = std::move(eng);
    lang_   = lang;
    return true;
}

void ScriptEngineSlot::unload() { engine_.reset(); }

IScriptEngine* ScriptEngineSlot::create_staged(ScriptHost* host, std::string* err) {
    std::unique_ptr<IScriptEngine> eng;
    switch (lang_) {
        case ScriptLanguage::BASIC: eng.reset(new ScriptEngine(host)); break;
        case ScriptLanguage::LUA:   eng.reset(new LuaEngine(host));    break;
        default:
            if (err) *err = "未知脚本语言";
            return nullptr;
    }
    return eng.release();
}

IScriptEngine* ScriptEngineSlot::replace(IScriptEngine* new_eng) {
    IScriptEngine* old = engine_.release();   // 所有权移交调用方
    engine_.reset(new_eng);
    return old;
}

} // namespace kx
