// nvram_test.cpp —— 4x 寄存器持久化（.nvram 读写）纯逻辑自测
// 用例：缺失文件 → 空表 / set-get 往返 / 落盘与重载 / 坏行忽略 / 越界拒绝 / 值未变不写盘
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "script/nvram_store.h"

using namespace kx;

static int g_fail = 0;
static int g_total = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_total;                                                         \
        if (!(cond)) {                                                     \
            ++g_fail;                                                      \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                  \
    } while (0)

static const std::string kDir  = "/tmp/kx_nvram_test";
static const std::string kFile = kDir + "/.nvram";

static void write_raw(const std::string& content) {
    std::ofstream f(kFile, std::ios::binary | std::ios::trunc);
    f << content;
}

int main() {
    if (::system(("rm -rf " + kDir).c_str()) != 0) { /* 不存在也继续 */ }
    if (::system(("mkdir -p " + kDir).c_str()) != 0) {
        std::printf("[nvram_test] 无法创建测试目录\n");
        return 2;
    }

    std::printf("== 1) 缺失文件 → 空表 ==\n");
    {
        NvramStore nv(kFile);
        CHECK(nv.load(nullptr));
        CHECK(nv.count() == 0);
        CHECK(nv.get(204) == 0);
    }

    std::printf("== 2) set/get 往返 + 立即落盘 ==\n");
    {
        NvramStore nv(kFile);
        CHECK(nv.load(nullptr));
        CHECK(nv.set(204, 1500, nullptr));
        CHECK(nv.set(211, 0x0000, nullptr));           // 浮点低字（1.5f 低字=0）
        CHECK(nv.set(212, 0x3FC0, nullptr));           // 浮点高字
        CHECK(nv.set(221, 0x8F5C, nullptr));
        CHECK(nv.count() == 4);
        CHECK(nv.get(204) == 1500 && nv.get(212) == 0x3FC0);
        // 值未变 → 跳过（返回成功）
        CHECK(nv.set(204, 1500, nullptr));
    }

    std::printf("== 3) 重载 → 值保持（模拟重启） ==\n");
    {
        NvramStore nv(kFile);
        CHECK(nv.load(nullptr));
        CHECK(nv.count() == 4);
        CHECK(nv.get(204) == 1500);
        CHECK(nv.get(211) == 0x0000 && nv.get(212) == 0x3FC0);
        CHECK(nv.get(221) == 0x8F5C);
        CHECK(nv.get(100) == 0);                       // 未保存 → 0
    }

    std::printf("== 4) 坏行忽略 ==\n");
    write_raw("# comment\n204=77\nbadline\n1200=1\n212=99999\n211=abc\n\n# x\n221 = 300 \n");
    {
        NvramStore nv(kFile);
        std::string err;
        CHECK(nv.load(&err));                          // 坏行不影响成功
        CHECK(nv.get(204) == 77);
        CHECK(nv.get(221) == 300);                     // 容忍空格
        CHECK(nv.count() == 2);
        CHECK(!err.empty());                           // 有摘要
    }

    std::printf("== 5) 越界与非法值拒绝 ==\n");
    {
        NvramStore nv(kFile);
        CHECK(nv.load(nullptr));
        std::string err;
        CHECK(!nv.set(-1, 1, &err) && !err.empty());
        CHECK(!nv.set(1200, 1, &err));
        // 用户寄存器区（4x300~999）：已纳入持久化范围（v0.9.0 扩到 1024）
        CHECK(nv.set(512, 1234, nullptr) && nv.get(512) == 1234);
        // 65535 合法（uint16 上限）
        CHECK(nv.set(0, 65535, nullptr));
        CHECK(nv.get(0) == 65535);
    }

    std::printf("[nvram_test] %d/%d 通过\n", g_total - g_fail, g_total);
    return g_fail == 0 ? 0 : 1;
}
