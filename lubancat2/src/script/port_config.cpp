// port_config.cpp —— 端口数量上限的持久化实现（`.portmax`，见 port_config.h）
#include "script/port_config.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "script/port_manager.h"   // kDefaultMaxPorts / kMaxSlots

namespace kx {
namespace {

constexpr const char* kPortFile = ".portmax";
constexpr const char* kPortTmp  = ".portmax.tmp";

std::string join(const std::string& dir, const char* name) {
    std::string d = dir;
    while (d.size() > 1 && d.back() == '/') d.pop_back();
    return d + "/" + name;
}

// 逐级创建目录（POSIX mkdir 无 -p 语义；与 debug_server.cpp 的 mkdir_p 同做法）
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

} // namespace

int read_port_max(const std::string& dir, int fallback) {
    std::ifstream f(join(dir, kPortFile), std::ios::binary);
    if (!f) return fallback;
    std::string line;
    std::getline(f, line);
    // 去空白
    const size_t b = line.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return fallback;
    const size_t e = line.find_last_not_of(" \t\r\n");
    line = line.substr(b, e - b + 1);
    if (line.empty() || line.size() > 3) return fallback;
    for (char c : line)
        if (c < '0' || c > '9') return fallback;      // 只接受纯十进制（拒绝 "-1"/"1e2" 之类）
    const int v = std::atoi(line.c_str());
    if (v < 1 || v > PortManager::kMaxSlots) return fallback;
    return v;
}

bool write_port_max(const std::string& dir, int value, std::string* err) {
    if (value < 1 || value > PortManager::kMaxSlots) {
        if (err) *err = "端口数量越界（1.." + std::to_string(PortManager::kMaxSlots) + "）";
        return false;
    }
    if (!mkdir_p(dir)) {
        if (err) *err = "无法创建目录 " + dir + ": " + std::strerror(errno);
        return false;
    }
    const std::string tmp = join(dir, kPortTmp);
    const std::string dst = join(dir, kPortFile);
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            if (err) *err = "无法写入 " + tmp;
            return false;
        }
        f << value << "\n";
        if (!f.good()) {
            if (err) *err = "写入 " + tmp + " 失败";
            return false;
        }
    }
    if (::rename(tmp.c_str(), dst.c_str()) != 0) {
        if (err) *err = "重命名 " + tmp + " -> " + dst + " 失败";
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace kx
