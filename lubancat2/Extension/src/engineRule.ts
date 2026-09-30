// engineRule.ts —— 「控制器同一时刻只加载一种脚本语言」在插件侧的镜像实现
//
// 单一事实来源：控制器 src/script/engine_rule.h
//   * script_file_is_lua/.bas  → 本文件 isLuaFile/isBasicFile（大小写敏感，与 C++ 一致）
//   * script_file_matches_language() → 本文件 fileMatchesEngine()（语义逐条对齐）
//
// 纪律（docs/planA/15 §6.4、FR-2.6、T-11）：不一致必须**明确拦截并报错**，绝不静默回退；
// 扩展名非 .lua/.bas 视为「无法判断」，放行（避免误伤历史配置）。

import type { ScriptEngine } from './config';
import { S } from './strings';

export function isLuaFile(file: string): boolean {
    return file.endsWith('.lua');
}

export function isBasicFile(file: string): boolean {
    return file.endsWith('.bas');
}

/** 由扩展名推断引擎：.lua → lua，其余按 basic（与 engine_rule.h 一致）。
 *  ⚠ 仅用于“确定是脚本文件”的场合；对 .md/.txt 等非脚本文件会**误判 basic**——
 *  裁决请用 `decideEngine` / `inferEngineName`。 */
export function engineFromFile(file: string): ScriptEngine {
    return isLuaFile(file) ? 'lua' : 'basic';
}

/** 由扩展名推断引擎：.lua→lua / .bas→basic / 其它→null（**无法判断**，如 .md/.txt/无扩展名） */
export function inferEngineName(file: string): ScriptEngine | null {
    if (isLuaFile(file)) return 'lua';
    if (isBasicFile(file)) return 'basic';
    return null;
}

export interface EngineDecision {
    /** 可用引擎（`conflict` 为真时无效） */
    engine?: ScriptEngine;
    /** true = 显式设置与**已知**扩展名冲突（调用方须明确拦截，绝不静默回退） */
    conflict?: boolean;
    /** 扩展名推断结果（null = 无法判断） */
    inferred: ScriptEngine | null;
}

/**
 * D-03 纯裁决（供 `engineResolve.ts` 与 Node 单测）：
 *   * 扩展名为 `.lua`/`.bas` 且与**显式设置**冲突 → `conflict: true`（调用方报错拦截）；
 *   * 扩展名无法判断（非 .lua/.bas，如 `.md`/`.txt`/无扩展名） → **不拦截**：
 *     显式设置优先，未显式设置则按 basic（历史默认）——避免对非脚本文件误报“引擎冲突”。
 */
export function decideEngine(engineSetting: ScriptEngine, explicit: boolean, file: string): EngineDecision {
    const inferred = inferEngineName(file);
    if (inferred !== null && explicit && engineSetting !== inferred) {
        return { conflict: true, inferred };
    }
    return { engine: explicit ? engineSetting : (inferred ?? 'basic'), inferred };
}

export interface EngineMatch {
    ok: boolean;
    reason?: string;
}

/**
 * 文件扩展名与所选引擎是否一致。
 * 仅当扩展名是已知的 `.lua`/`.bas` 且与引擎冲突时才判不通过。
 */
export function fileMatchesEngine(engine: ScriptEngine, file: string): EngineMatch {
    const lua = isLuaFile(file);
    const bas = isBasicFile(file);
    if (!lua && !bas) {
        return { ok: true };
    }
    if (lua === (engine === 'lua')) {
        return { ok: true };
    }
    return {
        ok: false,
        reason: S.engine.mismatch(engine, file),
    };
}

/** 引擎显示名（用于 UI 文案与日志） */
export function engineLabel(engine: ScriptEngine): string {
    return engine === 'lua' ? 'Lua' : 'BASIC';
}

/**
 * `sys.info.engine` 原值 → 显示名（v0.4.8）。
 *
 * 控制器语言由脚本目录推导（见 `debug_server.cpp` `scan_lang_bind`）：
 *   * `basic` / `lua`：已绑定；
 *   * `auto`：目录为空，两种语言都可编译（首次带 name 落盘后绑定）；
 *   * `mixed`：目录同时存在 .bas 与 .lua（新编译被拒，需先删除一种）；
 *   * 其他：未知（不猜）。
 */
export function engineDisplay(raw: string): string {
    switch (raw.trim().toLowerCase()) {
        case 'lua':
            return 'Lua';
        case 'basic':
            return 'BASIC';
        case 'auto':
            return S.engine.autoLabel;
        case 'mixed':
            return S.engine.mixedLabel;
        default:
            return S.common.unknown;
    }
}

/**
 * 控制器当前引擎与本文件的引擎是否冲突（v0.4.7）。
 *
 * 机制澄清：控制器的脚本语言是**控制器配置**（`SCRIPT_ENGINE`，默认 `basic`），
 * 由 `sys.info.engine` 上报；它与「控制器目录里有没有脚本文件」**无关**——
 * 空目录不等于语言未定义（见 docs/planA/12/13 与 engine_rule.h）。
 *
 * 连接后 controllerEngine 已知时先在本地拦截，避免发一条必然被
 * `ENGINE_MISMATCH` 拒绝的 `script.compile`，并直接给出改法（降级不伪装）。
 *
 * 返回 null = 不冲突，或控制器引擎未知（空/异常值，交服务端裁决）。
 */
export function controllerEngineConflict(
    controllerEngine: string,
    fileEngine: ScriptEngine,
): string | null {
    const c = controllerEngine.trim().toLowerCase();
    if (c !== 'basic' && c !== 'lua') {
        return null;
    }
    if (c === fileEngine) {
        return null;
    }
    return S.engine.controllerConflict(c, fileEngine);
}
