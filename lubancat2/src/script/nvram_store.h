// nvram_store.h —— 4x 寄存器持久化存储（NVSET/NVGET 设备命令，2026-09-28）
//
// 用途：把脚本/HMI 需要掉电保存的少量 4x 寄存器值存到脚本目录内的隐藏文件
//       `.nvram`（文本「寄存器=值」逐行；与 `.portmax`/`.boot` 同类，隐藏文件不进
//       file.list、不影响语言绑定）。
//   * `NVSET(reg, value)`：更新并**立即原子落盘**（tmp+rename）——写入频率低
//     （操作员改参数），无需节流；掉电最多丢“正在写”的那一次。
//   * `NVGET(reg)`：读回（无记录返回 0）。
//   * 恢复由脚本在 init 阶段逐项 reg_set（见 EtherCAT_SocketServer.lua）。
//
// 本文件不依赖 vscode / ecrt，可被纯逻辑自测直接覆盖（tools/nvram_test.cpp）。
#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <string>

namespace kx {

class NvramStore {
public:
    static constexpr int kRegs = 256;                  // 4x 寄存器个数

    explicit NvramStore(std::string file) : file_(std::move(file)) {}

    // 读文件：缺失 → 空表（成功）；坏行忽略并计数（err 非空时给出摘要）。
    bool load(std::string* err = nullptr);

    // 越界/未存 → 0。
    uint16_t get(int reg) const;

    // 更新 + 立即落盘；值未变化时跳过写盘。失败返回 false 并给出原因。
    bool set(int reg, uint16_t v, std::string* err = nullptr);

    // 原子写（先写 <file>.tmp 再 rename；必要时建目录）。
    bool flush(std::string* err = nullptr);

    int count() const;                                 // 已存条数（诊断/日志）
    const std::string& file() const { return file_; }

private:
    bool flush_locked(std::string* err);

    std::string file_;
    std::array<uint16_t, kRegs> regs_{};
    std::array<uint8_t,  kRegs> has_{};                // 0/1：该寄存器是否有持久化记录
    mutable std::mutex m_;
};

} // namespace kx
