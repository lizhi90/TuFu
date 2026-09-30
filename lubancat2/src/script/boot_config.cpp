#include "script/boot_config.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace kx {

namespace {

constexpr const char* kBootFile = ".boot";
constexpr const char* kBootTmp  = ".boot.tmp";

std::string join(const std::string& dir, const std::string& name) {
    if (dir.empty() || dir == ".") return name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

} // namespace

bool valid_script_file_name(const std::string& n) {
    if (n.empty() || n.size() > 128) return false;
    if (n[0] == '.') return false;
    for (char c : n) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

std::string script_lang_of_name(const std::string& name) {
    if (name.size() > 4) {
        const std::string ext = name.substr(name.size() - 4);
        if (ext == ".lua") return "lua";
        if (ext == ".bas") return "basic";
    }
    return "";
}

ScriptDirLang scan_script_dir_lang(const std::string& dir) {
    bool bas = false, lua = false;
    DIR* d = ::opendir(dir.c_str());
    if (d) {
        struct dirent* ent = nullptr;
        while ((ent = ::readdir(d)) != nullptr) {
            const std::string nm = ent->d_name;
            if (!valid_script_file_name(nm)) continue;   // 跳过 . / .. / 隐藏（含 .boot）
            struct stat st {};
            if (::stat(join(dir, nm).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
            if (script_lang_of_name(nm) == "lua") lua = true;
            else if (script_lang_of_name(nm) == "basic") bas = true;
        }
        ::closedir(d);
    }
    if (bas && lua) return ScriptDirLang::MIXED;
    if (bas) return ScriptDirLang::BASIC;
    if (lua) return ScriptDirLang::LUA;
    return ScriptDirLang::NONE;
}

std::string read_boot_name(const std::string& dir) {
    std::ifstream f(join(dir, kBootFile));
    if (!f) return "";
    std::string line;
    std::getline(f, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();   // Windows 编辑兜底
    return trim(line);
}

BootInfo read_boot_info(const std::string& dir) {
    BootInfo bi;
    bi.name = read_boot_name(dir);
    if (bi.name.empty()) {
        bi.name.clear();
        bi.valid = true;                       // 未设置 = 合法（走 SCRIPT_FILE 回退）
        return bi;
    }
    if (!valid_script_file_name(bi.name)) {
        bi.valid  = false;
        bi.reason = "清单记录的文件名不合法：" + bi.name;
        return bi;
    }
    if (script_lang_of_name(bi.name).empty()) {
        bi.valid  = false;
        bi.reason = "主文件必须是 .bas 或 .lua：" + bi.name;
        return bi;
    }
    struct stat st {};
    if (::stat(join(dir, bi.name).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        bi.valid  = false;
        bi.reason = "主文件在控制器上不存在：" + bi.name + "（可能在文件列表里被删除，请重新指定）";
        return bi;
    }
    if (scan_script_dir_lang(dir) == ScriptDirLang::MIXED) {
        bi.valid  = false;
        bi.reason = "脚本目录同时存在 .bas 与 .lua，语言不唯一：请先删除其中一种";
        return bi;
    }
    return bi;
}

bool write_boot_name(const std::string& dir, const std::string& name, std::string* err) {
    const std::string tmp = join(dir, kBootTmp);
    const std::string dst = join(dir, kBootFile);
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            if (err) *err = "无法写入 " + tmp;
            return false;
        }
        f << name << "\n";
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

bool clear_boot_name(const std::string& dir, std::string* err) {
    const std::string dst = join(dir, kBootFile);
    if (::unlink(dst.c_str()) != 0 && errno != ENOENT) {
        if (err) *err = std::string("删除 ") + dst + " 失败";
        return false;
    }
    return true;
}

} // namespace kx
