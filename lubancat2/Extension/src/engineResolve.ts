// engineResolve.ts —— 引擎裁决（开工决议表 D-03）
//
// 与 `engineRule.ts` 的分工：
//   * `engineRule.ts`：`engine_rule.h` 的**纯镜像**（无 vscode 依赖，可在 Node 下单测）；
//   * 本文件：把镜像 + 用户设置合起来做**裁决**，因此需要读配置（依赖 vscode）。
//
// 纪律（docs/planA/15 §6.4 / FR-2.6）：扩展名推断为主，显式设置冲突时**明确拦截**，绝不静默回退。

import { readConfig, ScriptEngine } from './config';
import { decideEngine } from './engineRule';
import { S } from './strings';

export interface EngineResolution {
    engine?: ScriptEngine;
    reason?: string;
}

/** 返回 `engine` 表示可用；返回 `reason` 表示须拦截并向用户报错 */
export function resolveEngine(fsPath: string): EngineResolution {
    const cfg = readConfig();
    const d = decideEngine(cfg.engine, cfg.engineExplicit, fsPath);
    if (d.conflict && d.inferred !== null) {
        return {
            reason: S.engineResolve.settingConflict(cfg.engine, d.inferred, fsPath),
        };
    }
    return { engine: d.engine };
}
