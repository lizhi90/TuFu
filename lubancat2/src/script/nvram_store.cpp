// nvram_store.cpp —— 4x 寄存器持久化存储实现（见 nvram_store.h）
#include "script/nvram_store.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace kx {
namespace {

constexpr const char* kTmpSuffix = ".tmp";

// 逐级创建目录（与 port_config.cpp / debug_server.cpp 同做法）
bool mkdir_p(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!cur.empty() && cur != "/" &&
                ::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
                return false;
            }
        }
        if (i < path.size()) cur.push_back(path[i]);
    }
    return true;
}

std::string parent_dir(const std::string& path) {
    const size_t p = path.find_last_of('/');
    return p == std::string::npos ? std::string(".") : path.substr(0, p);
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

} // namespace

bool NvramStore::load(std::string* err) {
    std::lock_guard<std::mutex> lk(m_);
    regs_.fill(0);
    has_.fill(0);
    int bad = 0;

    std::ifstream f(file_, std::ios::binary);
    if (!f) return true;                               // 缺失/不可读 → 空表（首次运行）

    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) { ++bad; continue; }
        const std::string ks = trim(line.substr(0, eq));
        const std::string vs = trim(line.substr(eq + 1));
        char* end = nullptr;
        const long reg = std::strtol(ks.c_str(), &end, 10);
        if (end == ks.c_str() || *end != '\0' || reg < 0 || reg >= kRegs) { ++bad; continue; }
        end = nullptr;
        const long val = std::strtol(vs.c_str(), &end, 10);
        if (end == vs.c_str() || *end != '\0' || val < 0 || val > 65535) { ++bad; continue; }
        regs_[(int)reg] = (uint16_t)val;
        has_[(int)reg]  = 1;
    }
    if (bad > 0 && err) {
        *err = "NVRAM " + file_ + " 有 " + std::to_string(bad) + " 行非法（已忽略）";
    }
    return true;
}

bool NvramStore::has(int reg) const {
    std::lock_guard<std::mutex> lk(m_);
    if (reg < 0 || reg >= kRegs) return false;
    return has_[reg] != 0;
}

uint16_t NvramStore::get(int reg) const {
    std::lock_guard<std::mutex> lk(m_);
    if (reg < 0 || reg >= kRegs || !has_[reg]) return 0;
    return regs_[reg];
}

bool NvramStore::set(int reg, uint16_t v, std::string* err) {
    std::lock_guard<std::mutex> lk(m_);
    if (reg < 0 || reg >= kRegs) {
        if (err) *err = "NVRAM 寄存器号越界";
        return false;
    }
    if (has_[reg] && regs_[reg] == v) return true;      // 值未变，跳过写盘
    regs_[reg] = v;
    has_[reg]  = 1;
    return flush_locked(err);
}

bool NvramStore::flush(std::string* err) {
    std::lock_guard<std::mutex> lk(m_);
    return flush_locked(err);
}

bool NvramStore::flush_locked(std::string* err) {
    const std::string dir = parent_dir(file_);
    if (!mkdir_p(dir)) {
        if (err) *err = "NVRAM 无法创建目录: " + dir;
        return false;
    }
    const std::string tmp = file_ + kTmpSuffix;
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            if (err) *err = "NVRAM 无法写临时文件: " + tmp;
            return false;
        }
        f << "# kine-x nvram —— 4x 寄存器持久化（NVSET/NVGET 维护；手工编辑后需重启）\n";
        for (int i = 0; i < kRegs; ++i) {
            if (has_[i]) f << i << "=" << regs_[i] << "\n";
        }
        f.flush();
        if (!f) {
            if (err) *err = "NVRAM 写入失败: " + tmp;
            return false;
        }
    }
    if (std::rename(tmp.c_str(), file_.c_str()) != 0) {
        if (err) *err = std::string("NVRAM rename 失败: ") + std::strerror(errno);
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

int NvramStore::count() const {
    std::lock_guard<std::mutex> lk(m_);
    int n = 0;
    for (int i = 0; i < kRegs; ++i) if (has_[i]) ++n;
    return n;
}

} // namespace kx
