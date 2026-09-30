// axis_limits.h —— 控制器轴数上限（多轴系统的**唯一真源**）
//
// 本控制器为**多轴系统**（当前项目通过 AXIS_COUNT=1 跑单轴）。
// 轴数组长度、每轴 EtherCAT 从站数上限均取此值；轴 i 默认对应从站 i
// （见 config.h 的 EcatCfg::slave[]）。
//
// 说明：
//   * 这是**编译期上限**，不是运行时实际轴数；实际轴数由配置 AXIS_COUNT 决定
//     （AppConfig::axis_count，合法范围 1..kAxisMax）。
//   * config.h / state.h 都依赖本常量；为避免二者互相包含，单独放这个中性头文件。
#pragma once

namespace kx {

constexpr int kAxisMax = 8;

} // namespace kx
