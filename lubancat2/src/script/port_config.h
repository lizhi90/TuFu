// port_config.h —— 端口数量上限的持久化（D9）
//
// 上限值存放于脚本目录内的隐藏文件 `DEBUG_SCRIPT_DIR/.portmax`（单行整数）：
//   * 隐藏文件被 valid_script_file_name 排除 → 不进 file.list、不影响语言绑定（与 `.boot` 同类）；
//   * 由调试通道 `port.max.get/set`（D9）读写；main.cpp 启动时加载（缺失/非法 → 默认 16）；
//   * 运行期上限本体在 `PortManager`（进程级，见 port_manager.h），本模块只管读写文件。
//
// 本文件不依赖 vscode / ecrt，可被纯逻辑自测直接覆盖。
#pragma once

#include <string>

namespace kx {

// 读 `.portmax`：文件缺失/为空/非法/超出 1..PortManager::kMaxSlots → 返回 fallback（不报错）
int read_port_max(const std::string& dir, int fallback);

// 写 `.portmax`（先写 `.portmax.tmp` 再 rename，必要时创建目录）；value 由调用方先做业务校验
bool write_port_max(const std::string& dir, int value, std::string* err);

} // namespace kx
