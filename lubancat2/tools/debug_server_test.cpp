// debug_server_test.cpp —— 调试通道服务端（D1~D3）纯逻辑自测
//
// 不接 EtherCAT、不驱动电机：用假 kx::Shared + 回环 socket，起真实 DebugServer，
// 按 docs/planA/16 的报文样例逐条校验：
//   D1: sys.info / var.* / axis.snapshot / cmd
//   D2: script.compile / run / stop / status
//   D3: subscribe / unsubscribe + 事件推送（axis / log）
// 以及错误路径：UNKNOWN_METHOD / NOT_SUPPORTED / BAD_REQUEST / BAD_PARAM / RUNTIME_ERROR / ENGINE_MISMATCH。
//
// 运行：ctest -R debug_server_test  （或直接跑 ./debug_server_test）
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include "common/config.h"
#include "common/state.h"
#include "script/debug_server.h"
#include "script/json_lite.h"
#include "script/port_manager.h"   // D9：占用模拟（进程级占用登记）

using namespace kx;

static int g_fail = 0, g_total = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_total;                                                             \
        if (!(cond)) {                                                         \
            ++g_fail;                                                          \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                      \
    } while (0)

// ---------------------------------------------------------------------------
// 回环 socket 小工具
// ---------------------------------------------------------------------------
static int connect_to(int port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons((unsigned short)port);
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

static bool g_verbose = false;

static bool send_all(int fd, const std::string& s) {
    if (g_verbose) std::printf("C-> %s", s.c_str());
    size_t off = 0;
    while (off < s.size()) {
        const ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

// 读一行 JSON（跳过空行）；超时/断开返回 false
static bool read_line(int fd, std::string& buf, Json* out, int timeout_ms) {
    for (;;) {
        const size_t pos = buf.find('\n');
        if (pos != std::string::npos) {
            std::string one = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (!one.empty() && one.back() == '\r') one.pop_back();
            if (one.empty()) continue;
            if (g_verbose) std::printf("S<- %s\n", one.c_str());
            *out = Json{};          // 复用同一个 Json 时必须先清空：解析是「追加」而非覆盖
            std::string err;
            if (!json_parse(one, out, &err)) return false;
            return true;
        }
        pollfd p{};
        p.fd     = fd;
        p.events = POLLIN;
        const int pr = ::poll(&p, 1, timeout_ms);
        if (pr <= 0) return false;
        char tmp[8192];
        const ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf.append(tmp, (size_t)n);
    }
}

static std::string req(int id, const std::string& m, const std::string& p = "") {
    std::string s = "{\"id\":" + std::to_string(id) + ",\"m\":\"" + m + "\"";
    if (!p.empty()) s += ",\"p\":" + p;
    return s + "}";
}

// 发请求并按 id 收回应答（途中的事件行直接丢弃）
static bool call(int fd, std::string& buf, const std::string& request, int id, Json* resp,
                 int timeout_ms = 3000) {
    if (!send_all(fd, request + "\n")) return false;
    for (int i = 0; i < 500; ++i) {
        Json j;
        if (!read_line(fd, buf, &j, timeout_ms)) return false;
        const Json* pi = j.find("id");
        if (pi && pi->is_num() && pi->as_int(-1) == id) { *resp = j; return true; }
    }
    return false;
}

// id 自增交给本函数统一处理：调用点写成 rpc(fd, buf, &id, "m", "p", &r)，
// 避免 `req(++id, ...), id` 在同一全表达式里既读又改（UB / -Wsequence-point）。
static bool call_next(int fd, std::string& buf, int* id, const std::string& m,
                      const std::string& p, Json* resp, int timeout_ms = 3000) {
    const int nid = ++(*id);
    return call(fd, buf, req(nid, m, p), nid, resp, timeout_ms);
}

static bool ok_of(const Json& resp) {
    const Json* po = resp.find("ok");
    return po && po->as_bool(false);
}
static std::string code_of(const Json& resp) {
    const Json* pe = resp.find("err");
    if (!pe) return "";
    const Json* pc = pe->find("code");
    return pc ? pc->as_str() : "";
}
static const Json* result_of(const Json& resp) { return resp.find("r"); }

// 等到指定主题的事件（可再按 status 过滤），期间丢弃其它行
static bool wait_event(int fd, std::string& buf, const std::string& topic,
                       const std::string& status, Json* ev, int timeout_ms) {
    for (int i = 0; i < 500; ++i) {
        Json j;
        if (!read_line(fd, buf, &j, timeout_ms)) return false;
        const Json* pe = j.find("e");
        if (!pe || pe->as_str() != topic) continue;
        if (!status.empty()) {
            const Json* ps = j.find("status");
            if (!ps || ps->as_str() != status) continue;
        }
        *ev = j;
        return true;
    }
    return false;
}

int main() {
    g_verbose = (std::getenv("KX_DBG_VERBOSE") != nullptr);

    // ---- 假共享状态：单轴，位置 12.5 / 已使能 / 到位 ----
    Shared sh;
    sh.axis_count = 1;
    {
        AxisStatus st;
        st.bus_ok  = 1;
        st.enabled = 1;
        st.idle    = 1;
        st.mpos    = 12.5;
        st.dpos    = 12.5;
        sh.axis[0] = st;
    }

    DebugCfg cfg;
    cfg.enable     = true;
    cfg.port       = 0;             // 内核分配端口
    cfg.bind       = "127.0.0.1";
    cfg.max_steps  = 5000000;
    cfg.script_dir = "/tmp/kx_dbg_test_scripts";   // D6 用例目录（先清空）
    // D7 用例：重启命令换成写标记文件（真实命令会重启/杀掉测试进程）
    cfg.restart_cmd = "touch /tmp/kx_dbg_restart_marker";
    if (::system("rm -rf /tmp/kx_dbg_test_scripts") != 0) { /* 目录不存在也继续 */ }
    if (::system("rm -f /tmp/kx_dbg_restart_marker") != 0) { /* 文件不存在也继续 */ }

    DebugServer ds(sh, cfg, /*allow_script=*/true, "basic");
    std::string lerr;
    if (!ds.listen_now(&lerr)) {
        std::printf("[debug_server_test] 无法监听: %s\n", lerr.c_str());
        return 2;
    }
    ds.start();

    const int fd = connect_to(ds.port());
    if (fd < 0) {
        std::printf("[debug_server_test] 连接失败（port=%d）\n", ds.port());
        ds.stop();
        return 2;
    }
    std::string buf;
    Json r;
    int id = 0;

    // ---- D1: sys.info ----
    if (call_next(fd, buf, &id, "sys.info", "", &r)) {
        CHECK(ok_of(r));
        const Json* rr = result_of(r);
        CHECK(rr && rr->find("engine")->as_str() == "auto");   // 脚本目录为空 → auto（两种语言都收）
        CHECK(rr && rr->find("axis_count")->as_int(-1) == 1);
        const Json* caps = rr ? rr->find("caps") : nullptr;
        bool d1 = false, d2 = false, d3 = false, d4 = false, d5 = false, d6 = false, d7 = false;
        if (caps && caps->is_arr()) {
            for (const Json& c : caps->arr) {
                const std::string s = c.as_str();
                if (s == "d1") d1 = true;
                else if (s == "d2") d2 = true;
                else if (s == "d3") d3 = true;
                else if (s == "d4") d4 = true;
                else if (s == "d5") d5 = true;
                else if (s == "d6") d6 = true;
                else if (s == "d7") d7 = true;
            }
        }
        CHECK(d1 && d2 && d3);
        CHECK(d4 && d5);            // D4/D5 已实装（basic 引擎 + allow_script）
        CHECK(d6);                  // D6 文件管理
        CHECK(d7);                  // D7 重启（测试配置了 restart_cmd）
    } else {
        CHECK(false);
    }

    // ---- D7: sys.restart（测试命令写标记文件，先回应答再异步执行）----
    CHECK(call_next(fd, buf, &id, "sys.restart", "", &r) && ok_of(r));
    CHECK(result_of(r) && result_of(r)->find("restarting") && result_of(r)->find("restarting")->as_bool());
    {
        bool marker = false;
        for (int i = 0; i < 40 && !marker; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::ifstream f("/tmp/kx_dbg_restart_marker");
            marker = f.good();
        }
        CHECK(marker);
    }
    {
        // 空 DEBUG_RESTART_CMD：caps 不报 d7，sys.restart 明确 NOT_SUPPORTED（降级不伪装）
        DebugCfg cfg_no = cfg;
        cfg_no.restart_cmd.clear();
        cfg_no.port = 0;
        cfg_no.script_dir = "/tmp/kx_dbg_test_scripts_no";
        DebugServer ds_no(sh, cfg_no, /*allow_script=*/true, "basic");
        std::string e2;
        if (ds_no.listen_now(&e2)) {
            ds_no.start();
            const int fd2 = connect_to(ds_no.port());
            std::string buf2;
            Json r2;
            int id2 = 0;
            if (fd2 >= 0 && call_next(fd2, buf2, &id2, "sys.info", "", &r2) && ok_of(r2)) {
                const Json* caps2 = result_of(r2) ? result_of(r2)->find("caps") : nullptr;
                bool has7 = false;
                if (caps2 && caps2->is_arr()) {
                    for (const Json& c : caps2->arr) {
                        if (c.as_str() == "d7") has7 = true;
                    }
                }
                CHECK(!has7);
            } else {
                CHECK(false);
            }
            if (fd2 >= 0 && call_next(fd2, buf2, &id2, "sys.restart", "", &r2)) {
                CHECK(code_of(r2) == "NOT_SUPPORTED");
            } else {
                CHECK(false);
            }
            if (fd2 >= 0) ::close(fd2);
            ds_no.stop();
        } else {
            CHECK(false);
        }
    }

    // ---- D1: sys.ping / 未知方法 / D5 状态前置（未运行时 step / 缺参 add）----
    CHECK(call_next(fd, buf, &id, "sys.ping", "", &r) && ok_of(r));
    CHECK(call_next(fd, buf, &id, "bogus.method", "", &r) && code_of(r) == "UNKNOWN_METHOD");
    CHECK(call_next(fd, buf, &id, "script.step", "", &r) && code_of(r) == "BAD_PARAM");
    CHECK(call_next(fd, buf, &id, "breakpoint.add", "", &r) && code_of(r) == "BAD_PARAM");

    // ---- 分帧非法 -> BAD_REQUEST ----
    if (send_all(fd, "{\"not-json}}\n")) {
        CHECK(read_line(fd, buf, &r, 2000) && code_of(r) == "BAD_REQUEST");
    } else {
        CHECK(false);
    }

    // ---- D1: axis.snapshot（合法 / 越界）----
    CHECK(call_next(fd, buf, &id, "axis.snapshot", "{\"axis\":0}", &r) && ok_of(r));
    {
        const Json* rr = result_of(r);
        CHECK(rr && rr->find("mpos")->as_num(-1) == 12.5);
        CHECK(rr && rr->find("enabled")->as_int(-1) == 1);
        CHECK(rr && rr->find("bus_ok")->as_int(-1) == 1);
    }
    CHECK(call_next(fd, buf, &id, "axis.snapshot", "{\"axis\":3}", &r) &&
          code_of(r) == "BAD_PARAM");

    // ---- D6: 文件管理（list 空 → compile 落盘 → list/get → del）----
    {
        // 目录尚不存在：file.list 返回空清单（不算错误）
        CHECK(call_next(fd, buf, &id, "file.list", "", &r) && ok_of(r));
        const Json* rr = result_of(r);
        const Json* files = rr ? rr->find("files") : nullptr;
        CHECK(files && files->is_arr() && files->arr.empty());

        // 非法 name：拒绝（防路径穿越）
        CHECK(call_next(fd, buf, &id, "file.get", "{\"name\":\"../etc/passwd\"}", &r) &&
              code_of(r) == "BAD_PARAM");
        CHECK(call_next(fd, buf, &id, "file.get", "", &r) && code_of(r) == "BAD_PARAM");

        // 下载（编译）带 name → saved=true
        CHECK(call_next(fd, buf, &id, "script.compile",
                        "{\"src\":\"A = 42\\nEND\",\"engine\":\"basic\",\"name\":\"demo.bas\"}",
                        &r) && ok_of(r));
        CHECK(result_of(r) && result_of(r)->find("saved") &&
              result_of(r)->find("saved")->as_bool() == true);

        // file.list 含该文件
        CHECK(call_next(fd, buf, &id, "file.list", "", &r) && ok_of(r));
        {
            const Json* fl = result_of(r)->find("files");
            CHECK(fl && fl->is_arr() && fl->arr.size() == 1);
            if (fl && fl->arr.size() == 1) {
                const Json* pn = fl->arr[0].find("name");
                CHECK(pn && pn->as_str() == "demo.bas");
                // v0.8.4：file.list 带内容哈希（FNV-1a 64）供插件比对本地副本；
                // 已知向量："A = 42\nEND" = 3dcf4319e81d1824（与插件 fnv1a64Hex 同算法）
                const Json* ph = fl->arr[0].find("hash");
                CHECK(ph && ph->is_str() && ph->as_str() == "3dcf4319e81d1824");
            }
        }

        // file.get 内容一致
        CHECK(call_next(fd, buf, &id, "file.get", "{\"name\":\"demo.bas\"}", &r) && ok_of(r));
        {
            const Json* src = result_of(r)->find("src");
            CHECK(src && src->as_str() == "A = 42\nEND");
        }

        // file.get 不存在 → NOT_FOUND
        CHECK(call_next(fd, buf, &id, "file.get", "{\"name\":\"nope.bas\"}", &r) &&
              code_of(r) == "NOT_FOUND");

        // file.del → deleted；再 list 为空
        CHECK(call_next(fd, buf, &id, "file.del", "{\"name\":\"demo.bas\"}", &r) && ok_of(r));
        CHECK(call_next(fd, buf, &id, "file.list", "", &r) && ok_of(r));
        CHECK(result_of(r) && result_of(r)->find("files") &&
              result_of(r)->find("files")->arr.empty());
    }

    // ---- 语言绑定（2026-09-25 拍板：语言由脚本目录内现有脚本决定，不靠配置）----
    {
        // 空目录 → auto
        CHECK(call_next(fd, buf, &id, "sys.info", "", &r) && ok_of(r));
        CHECK(result_of(r) && result_of(r)->find("engine")->as_str() == "auto");

        // 空目录下可编译 lua，带 name 落盘后绑定为 lua
        CHECK(call_next(fd, buf, &id, "script.compile",
                        "{\"src\":\"x = 1\",\"engine\":\"lua\",\"name\":\"demo.lua\"}", &r) && ok_of(r));
        CHECK(call_next(fd, buf, &id, "sys.info", "", &r) && ok_of(r));
        CHECK(result_of(r) && result_of(r)->find("engine")->as_str() == "lua");
        {
            const Json* caps = result_of(r) ? result_of(r)->find("caps") : nullptr;
            bool d5 = false;
            if (caps && caps->is_arr()) {
                for (const Json& c : caps->arr) if (c.as_str() == "d5") d5 = true;
            }
            CHECK(d5);                        // v0.8.0：Lua 也实装 D5（行级 hook），据实声明
        }

        // 绑定 lua 后编译 basic → ENGINE_MISMATCH
        CHECK(call_next(fd, buf, &id, "script.compile",
                        "{\"src\":\"A = 1\\nEND\",\"engine\":\"basic\"}", &r) &&
              code_of(r) == "ENGINE_MISMATCH");

        // 删除唯一脚本 → 回到 auto
        CHECK(call_next(fd, buf, &id, "file.del", "{\"name\":\"demo.lua\"}", &r) && ok_of(r));
        CHECK(call_next(fd, buf, &id, "sys.info", "", &r) && ok_of(r));
        CHECK(result_of(r) && result_of(r)->find("engine")->as_str() == "auto");

        // 混合目录（.bas + .lua）→ mixed，拒绝新编译
        CHECK(call_next(fd, buf, &id, "script.compile",
                        "{\"src\":\"A = 1\\nEND\",\"engine\":\"basic\",\"name\":\"a.bas\"}", &r) && ok_of(r));
        {
            std::ofstream f("/tmp/kx_dbg_test_scripts/b.lua");
            f << "x = 1\n";
        }
        CHECK(call_next(fd, buf, &id, "sys.info", "", &r) && ok_of(r));
        CHECK(result_of(r) && result_of(r)->find("engine")->as_str() == "mixed");
        CHECK(call_next(fd, buf, &id, "script.compile",
                        "{\"src\":\"A = 1\\nEND\",\"engine\":\"basic\"}", &r) &&
              code_of(r) == "ENGINE_MISMATCH");

        // 清理混合状态 → auto
        CHECK(call_next(fd, buf, &id, "file.del", "{\"name\":\"a.bas\"}", &r) && ok_of(r));
        CHECK(call_next(fd, buf, &id, "file.del", "{\"name\":\"b.lua\"}", &r) && ok_of(r));
        CHECK(call_next(fd, buf, &id, "sys.info", "", &r) && ok_of(r));
        CHECK(result_of(r) && result_of(r)->find("engine")->as_str() == "auto");
    }

    // ---- D1: cmd（读值 / 参数不足 / 空命令）----
    if (call_next(fd, buf, &id, "cmd", "{\"line\":\"POS\"}", &r)) {
        CHECK(ok_of(r));
        const Json* rr = result_of(r);
        CHECK(rr && rr->find("ret")->as_int(-1) == 0);
        const Json* out = rr ? rr->find("out") : nullptr;
        CHECK(out && out->is_arr() && out->arr.size() == 1);
        if (out && out->arr.size() == 1) CHECK(out->arr[0].as_str() == "12.5");
    } else {
        CHECK(false);
    }
    CHECK(call_next(fd, buf, &id, "cmd", "{\"line\":\"MOVEABS\"}", &r) &&
          code_of(r) == "RUNTIME_ERROR");
    CHECK(call_next(fd, buf, &id, "cmd", "{\"line\":\"BOGUSCMD\"}", &r) &&
          code_of(r) == "RUNTIME_ERROR");
    CHECK(call_next(fd, buf, &id, "cmd", "{\"line\":\"   \"}", &r) &&
          code_of(r) == "BAD_PARAM");

    // ---- D5 for Lua（v0.8.0：行级 hook + 断点/单步/恢复；BASIC 语句级用例见后文）----
    {
        CHECK(call_next(fd, buf, &id, "script.compile",
                        "{\"src\":\"n = 0\\nfor i = 1, 1000000 do\\nn = n + 1\\nend\\n\","
                        "\"engine\":\"lua\",\"name\":\"dbg.lua\"}",
                        &r) &&
              ok_of(r));
        CHECK(call_next(fd, buf, &id, "sys.info", "", &r) && ok_of(r));
        {
            const Json* rr = result_of(r);
            const Json* caps = rr ? rr->find("caps") : nullptr;
            bool d5 = false;
            if (caps && caps->is_arr()) {
                for (const Json& c : caps->arr) if (c.as_str() == "d5") d5 = true;
            }
            CHECK(d5);                        // Lua 绑定同样声明 d5（行级实装，不伪装）
        }
        CHECK(call_next(fd, buf, &id, "breakpoint.add", "{\"line\":3}", &r) && ok_of(r));
        CHECK(call_next(fd, buf, &id, "script.run", "", &r) && ok_of(r));
        {
            Json ev;
            CHECK(wait_event(fd, buf, "script", "PAUSED", &ev, 3000));
            CHECK(ev.find("line") && ev.find("line")->as_int(-1) == 3);
        }
        if (call_next(fd, buf, &id, "script.status", "", &r)) {
            const Json* rr = result_of(r);
            CHECK(rr && rr->find("status")->as_str() == "PAUSED");
            CHECK(rr && rr->find("line")->as_int(-1) == 3);
        } else {
            CHECK(false);
        }
        // 单步：恢复后在**下个行事件**再停（循环体内 → 行号应 >= 2）
        CHECK(call_next(fd, buf, &id, "script.step", "", &r) && ok_of(r));
        {
            Json ev;
            CHECK(wait_event(fd, buf, "script", "PAUSED", &ev, 3000));
            CHECK(ev.find("line") && ev.find("line")->as_int(-1) >= 2);
        }
        // 先清断点再恢复：循环体会反复命中同一条断点（语义正确；测试需跑到 DONE）
        CHECK(call_next(fd, buf, &id, "breakpoint.del", "", &r) && ok_of(r));
        CHECK(call_next(fd, buf, &id, "script.resume", "", &r) && ok_of(r));
        {
            Json ev;
            CHECK(wait_event(fd, buf, "script", "DONE", &ev, 3000));
        }
        CHECK(call_next(fd, buf, &id, "file.del", "{\"name\":\"dbg.lua\"}", &r) && ok_of(r));
    }

    // ---- D8: 主文件（开机运行）清单 + 多文件 INCLUDE（用户拍板 2026-09-26）----
    {
        // 空目录：boot.get 报「未设置」
        CHECK(call_next(fd, buf, &id, "boot.get", "", &r) && ok_of(r));
        {
            const Json* rr = result_of(r);
            CHECK(rr && rr->find("name") && rr->find("name")->as_str().empty());
            CHECK(rr && rr->find("valid") && rr->find("valid")->as_bool());
        }

        // 子程序文件 + 主文件 INCLUDE 展开：编译、运行、验证子程序确实并入
        // （ZBasic 子程序经全局变量通信；用 A_SUB 证明 sub.bas 的代码真的执行了）
        {
            std::ofstream f("/tmp/kx_dbg_test_scripts/sub.bas");
            f << "SUB bump(x)\n  A_SUB = A_SUB + x\nEND SUB\nA_SUB = 1\n";
        }
        CHECK(call_next(fd, buf, &id, "script.compile",
                        "{\"src\":\"INCLUDE \\\"sub.bas\\\"\\nbump(41)\\nEND\\n\","
                        "\"engine\":\"basic\",\"name\":\"main.bas\"}",
                        &r) &&
              ok_of(r));
        CHECK(call_next(fd, buf, &id, "script.run", "", &r) && ok_of(r));
        {
            Json ev;
            CHECK(wait_event(fd, buf, "script", "DONE", &ev, 3000));
        }
        if (call_next(fd, buf, &id, "var.get", "{\"name\":\"A_SUB\"}", &r)) {
            const Json* rr = result_of(r);
            CHECK(rr && rr->find("value") && rr->find("value")->as_num(-1) == 42);
        } else {
            CHECK(false);
        }

        // 设为主文件 → boot.get / sys.info.boot 同步
        CHECK(call_next(fd, buf, &id, "boot.set", "{\"name\":\"main.bas\"}", &r) && ok_of(r));
        CHECK(call_next(fd, buf, &id, "boot.get", "", &r) && ok_of(r));
        {
            const Json* rr = result_of(r);
            CHECK(rr && rr->find("name") && rr->find("name")->as_str() == "main.bas");
            CHECK(rr && rr->find("valid") && rr->find("valid")->as_bool());
        }
        CHECK(call_next(fd, buf, &id, "sys.info", "", &r) && ok_of(r));
        CHECK(result_of(r) && result_of(r)->find("boot") &&
              result_of(r)->find("boot")->as_str() == "main.bas");

        // 主文件不存在 → NOT_FOUND
        CHECK(call_next(fd, buf, &id, "boot.set", "{\"name\":\"ghost.bas\"}", &r) &&
              code_of(r) == "NOT_FOUND");

        // 目录混合（加 .lua）→ boot.set 拒绝
        {
            std::ofstream f("/tmp/kx_dbg_test_scripts/lib.lua");
            f << "x = 1\n";
        }
        CHECK(call_next(fd, buf, &id, "boot.set", "{\"name\":\"main.bas\"}", &r) &&
              code_of(r) == "ENGINE_MISMATCH");
        CHECK(call_next(fd, buf, &id, "file.del", "{\"name\":\"lib.lua\"}", &r) && ok_of(r));

        // 删除主文件 → boot.get 报 invalid（带明确原因，不静默）
        CHECK(call_next(fd, buf, &id, "file.del", "{\"name\":\"main.bas\"}", &r) && ok_of(r));
        CHECK(call_next(fd, buf, &id, "boot.get", "", &r) && ok_of(r));
        {
            const Json* rr = result_of(r);
            CHECK(rr && rr->find("name") && rr->find("name")->as_str() == "main.bas");
            CHECK(rr && rr->find("valid") && !rr->find("valid")->as_bool());
            CHECK(rr && rr->find("reason"));
        }

        // 清除 → 空
        CHECK(call_next(fd, buf, &id, "boot.clear", "", &r) && ok_of(r));
        CHECK(call_next(fd, buf, &id, "boot.get", "", &r) && ok_of(r));
        CHECK(result_of(r) && result_of(r)->find("name")->as_str().empty());

        // INCLUDE 缺文件 → COMPILE_ERROR（明确报错，不静默跳过）
        CHECK(call_next(fd, buf, &id, "script.compile",
                        "{\"src\":\"INCLUDE \\\"no_such.bas\\\"\\nEND\\n\",\"engine\":\"basic\"}",
                        &r) &&
              code_of(r) == "COMPILE_ERROR");
        CHECK(call_next(fd, buf, &id, "file.del", "{\"name\":\"sub.bas\"}", &r) && ok_of(r));
    }

    // ---- D9: 端口数量上限（运行期 + 持久化 .portmax；用户拍板方案 A）----
    {
        // caps 含 d9
        CHECK(call_next(fd, buf, &id, "sys.info", "", &r) && ok_of(r));
        {
            const Json* caps = result_of(r) ? result_of(r)->find("caps") : nullptr;
            bool has = false;
            if (caps && caps->is_arr())
                for (const Json& c : caps->arr)
                    if (c.as_str() == "d9") has = true;
            CHECK(has);   // sys.info.caps 含 d9
        }

        // get：默认 16 / 静态容量 64 / 无占用
        CHECK(call_next(fd, buf, &id, "port.max.get", "", &r) && ok_of(r));
        {
            const Json* rr = result_of(r);
            CHECK(rr && rr->find("max") && rr->find("max")->as_int(0) == 16);
            CHECK(rr && rr->find("slots") && rr->find("slots")->as_int(0) == 64);
            CHECK(rr && rr->find("default") && rr->find("default")->as_int(0) == 16);
            CHECK(rr && rr->find("used") && rr->find("used")->is_arr() &&
                  rr->find("used")->arr.empty());
        }

        // set 24 → 运行期生效 + 落盘 .portmax=24
        CHECK(call_next(fd, buf, &id, "port.max.set", "{\"max\":24}", &r) && ok_of(r));
        {
            const Json* rr = result_of(r);
            CHECK(rr && rr->find("max") && rr->find("max")->as_int(0) == 24);
            CHECK(rr && rr->find("prev") && rr->find("prev")->as_int(0) == 16);
            CHECK(rr && rr->find("saved") && rr->find("saved")->as_bool());
        }
        CHECK(call_next(fd, buf, &id, "port.max.get", "", &r) && ok_of(r));
        CHECK(result_of(r) && result_of(r)->find("max")->as_int(0) == 24);
        {
            std::ifstream f("/tmp/kx_dbg_test_scripts/.portmax");
            int v = -1;
            f >> v;
            CHECK(v == 24);   // .portmax 落盘 = 24
        }

        // 参数校验：缺参 / 0 / 65 / 非整数
        CHECK(call_next(fd, buf, &id, "port.max.set", "", &r) && code_of(r) == "BAD_PARAM");
        CHECK(call_next(fd, buf, &id, "port.max.set", "{\"max\":0}", &r) && code_of(r) == "BAD_PARAM");
        CHECK(call_next(fd, buf, &id, "port.max.set", "{\"max\":65}", &r) && code_of(r) == "BAD_PARAM");
        CHECK(call_next(fd, buf, &id, "port.max.set", "{\"max\":1.5}", &r) && code_of(r) == "BAD_PARAM");

        // 有端口占用时收缩 → BUSY（用另一个 PortManager 实例模拟脚本侧占用；登记是进程级）
        {
            PortManager holder;
            std::string oerr;
            CHECK(holder.open(20, "TCP_SERVER", 0, "", &oerr));   // 测试实例占用端口 20（回环监听）
            CHECK(call_next(fd, buf, &id, "port.max.set", "{\"max\":16}", &r) &&
                  code_of(r) == "BUSY");
            holder.close_port(20);
        }
        CHECK(call_next(fd, buf, &id, "port.max.set", "{\"max\":16}", &r) && ok_of(r));
        CHECK(call_next(fd, buf, &id, "port.max.get", "", &r) && ok_of(r));
        CHECK(result_of(r) && result_of(r)->find("max")->as_int(0) == 16);
    }

    // ---- D10: 通讯状态（conn 订阅：端口快照 + 标签 + EtherCAT 主/从明细）----
    {
        CHECK(call_next(fd, buf, &id, "sys.info", "", &r) && ok_of(r));
        {
            const Json* caps = result_of(r) ? result_of(r)->find("caps") : nullptr;
            bool has = false;
            if (caps && caps->is_arr())
                for (const Json& c : caps->arr)
                    if (c.as_str() == "d10") has = true;
            CHECK(has);
        }
        // 造一个真实端口（进程级快照；DebugServer 之外的另一实例）+ PORT_INFO 标签
        PortManager peer_pm;
        std::string perr;
        CHECK(peer_pm.open(5, "TCP_SERVER", 0, "", &perr));
        CHECK(PortManager::set_tag(5, "测试用途", "从站", &perr));

        CHECK(call_next(fd, buf, &id, "subscribe", "{\"topics\":[\"conn\"],\"hz\":20}", &r) && ok_of(r));
        {
            const Json* rr  = result_of(r);
            const Json* sub = rr ? rr->find("sub") : nullptr;
            bool has_conn = false;
            if (sub && sub->is_arr())
                for (const Json& t : sub->arr)
                    if (t.as_str() == "conn") has_conn = true;
            CHECK(has_conn);
        }
        Json ev;
        CHECK(wait_event(fd, buf, "conn", "", &ev, 1500));
        const Json* conns = ev.find("conns");
        CHECK(conns && conns->is_arr() && !conns->arr.empty());
        bool saw = false;
        if (conns && conns->is_arr()) {
            for (const Json& c : conns->arr) {
                const Json* pp = c.find("port");
                if (pp && pp->as_int(-1) == 5) {
                    saw = true;
                    CHECK(c.find("kind") && c.find("kind")->as_str() == "TCP_SERVER");
                    CHECK(c.find("conn") && !c.find("conn")->as_bool());
                    CHECK(c.find("tag") && c.find("tag")->as_str() == "测试用途");
                    CHECK(c.find("role") && c.find("role")->as_str() == "从站");
                    CHECK(c.find("listen") && c.find("listen")->as_int(0) > 0);
                }
            }
        }
        CHECK(saw);
        const Json* bus = ev.find("bus");
        CHECK(bus && bus->find("link_up") && bus->find("node_count") && bus->find("slaves") &&
              bus->find("slaves")->is_arr());
        CHECK(call_next(fd, buf, &id, "unsubscribe", "{\"topics\":[\"conn\"]}", &r) && ok_of(r));
        {
            const Json* rr  = result_of(r);
            const Json* sub = rr ? rr->find("sub") : nullptr;
            bool has_conn = false;
            if (sub && sub->is_arr())
                for (const Json& t : sub->arr)
                    if (t.as_str() == "conn") has_conn = true;
            CHECK(!has_conn);
        }
        peer_pm.close_port(5);
    }

    // ---- D8 语言：开机脚本占用引擎时的判定（v0.8.1 修复回归）----
    // 目录里只有 .lua 时：sys.info.engine 必须报真实语言（hint=lua），boot.set .lua 放行；
    // 旧缺陷：scan 取 SCRIPT_ENGINE（basic）导致误报 basic 并拒绝设主文件。
    {
        const std::string dir2 = "/tmp/kx_dbg_test_scripts_boot";
        if (::system(("rm -rf " + dir2).c_str()) != 0) { /* 忽略 */ }
        if (::system(("mkdir -p " + dir2).c_str()) != 0) { /* 忽略 */ }
        {
            std::ofstream f(dir2 + "/main.lua");
            f << "x = 1\n";
        }
        DebugCfg cfg2 = cfg;
        cfg2.port = 0;
        cfg2.script_dir = dir2;
        DebugServer ds2(sh, cfg2, /*allow_script=*/false, "lua");
        std::string e2;
        if (ds2.listen_now(&e2)) {
            ds2.start();
            const int fd2 = connect_to(ds2.port());
            std::string buf2;
            Json r2;
            int id2 = 0;
            if (fd2 >= 0 && call_next(fd2, buf2, &id2, "sys.info", "", &r2) && ok_of(r2)) {
                const Json* eng = result_of(r2) ? result_of(r2)->find("engine") : nullptr;
                CHECK(eng && eng->as_str() == "lua");     // 占用引擎也报真实语言（不再误报 basic）
            } else {
                CHECK(false);
            }
            if (fd2 >= 0 && call_next(fd2, buf2, &id2, "boot.set", "{\"name\":\"main.lua\"}", &r2)) {
                CHECK(ok_of(r2));                          // 目录绑定 lua → 放行
            } else {
                CHECK(false);
            }
            if (fd2 >= 0 && call_next(fd2, buf2, &id2, "boot.get", "", &r2) && ok_of(r2)) {
                CHECK(result_of(r2) && result_of(r2)->find("name")->as_str() == "main.lua");
            } else {
                CHECK(false);
            }
            if (fd2 >= 0) ::close(fd2);
            ds2.stop();
        } else {
            CHECK(false);
        }
    }

    // ---- D2: script.compile（成功 / AUTO 下可切语言 / 未运行时 swap 等价普通编译）----
    CHECK(call_next(fd, buf, &id, "script.compile", "{\"src\":\"A = 1 + 2\\nEND\",\"engine\":\"basic\"}", &r) && ok_of(r));
    // 目录为空（auto）：lua 也可编译并在未运行时切换引擎；随后切回 basic
    CHECK(call_next(fd, buf, &id, "script.compile", "{\"src\":\"x=1\",\"engine\":\"lua\"}", &r) && ok_of(r));
    CHECK(call_next(fd, buf, &id, "script.compile", "{\"src\":\"A = 1\\nEND\",\"engine\":\"basic\",\"swap\":true}", &r) && ok_of(r));
    {
        const Json* rr = result_of(r);
        CHECK(rr && rr->find("swapped") && rr->find("swapped")->as_bool() == false);
    }
    // 编译失败（参数不足）应落 COMPILE_ERROR + line
    {
        Json resp;
        if (call_next(fd, buf, &id, "script.compile",
                      "{\"src\":\"IF 1\\nEND\",\"engine\":\"basic\"}", &resp)) {
            CHECK(code_of(resp) == "COMPILE_ERROR");
            const Json* pe = resp.find("err");
            const Json* pl = pe ? pe->find("line") : nullptr;
            CHECK(pl && pl->as_int(-1) == 1);
        } else {
            CHECK(false);
        }
    }

    // 重新编译一段正常脚本再跑
    CHECK(call_next(fd, buf, &id, "script.compile", "{\"src\":\"A = 1 + 2\\nEND\",\"engine\":\"basic\"}", &r) && ok_of(r));

    // ---- D2: script.run -> 事件 DONE；script.status ----
    if (call_next(fd, buf, &id, "script.run", "", &r)) {
        CHECK(ok_of(r));
        Json ev;
        CHECK(wait_event(fd, buf, "script", "DONE", &ev, 3000));
        CHECK(ev.find("steps") && ev.find("steps")->is_num());
        CHECK(ev.find("error_line") && ev.find("error_line")->as_int(-1) == 0);
    } else {
        CHECK(false);
    }
    if (call_next(fd, buf, &id, "script.status", "", &r)) {
        const Json* rr = result_of(r);
        CHECK(rr && rr->find("status")->as_str() == "DONE");
    } else {
        CHECK(false);
    }

    // ---- D1: var.list / get / set ----
    if (call_next(fd, buf, &id, "var.list", "", &r)) {
        CHECK(ok_of(r));
        const Json* rr = result_of(r);
        bool found = false;
        if (rr && rr->is_arr())
            for (const Json& v : rr->arr)
                if (v.as_str().find("A = 3") != std::string::npos) found = true;
        CHECK(found);
    } else {
        CHECK(false);
    }
    if (call_next(fd, buf, &id, "var.get", "{\"name\":\"A\"}", &r)) {
        const Json* rr = result_of(r);
        CHECK(rr && rr->find("type")->as_str() == "num");
        CHECK(rr && rr->find("value")->as_num(-1) == 3);
    } else {
        CHECK(false);
    }
    CHECK(call_next(fd, buf, &id, "var.set", "{\"name\":\"A\",\"v\":7}", &r) && ok_of(r));
    if (call_next(fd, buf, &id, "var.get", "{\"name\":\"A\"}", &r)) {
        const Json* rr = result_of(r);
        CHECK(rr && rr->find("value")->as_num(-1) == 7);
    } else {
        CHECK(false);
    }
    CHECK(call_next(fd, buf, &id, "var.get", "{\"name\":\"NOPE\"}", &r) &&
          code_of(r) == "BAD_PARAM");

    // ---- D3: subscribe / 事件流 / unsubscribe ----
    if (call_next(fd, buf, &id, "subscribe", "{\"topics\":[\"axis\",\"log\"],\"hz\":50}", &r)) {
        CHECK(ok_of(r));
        const Json* rr = result_of(r);
        const Json* sub = rr ? rr->find("sub") : nullptr;
        bool has_axis = false, has_log = false;
        if (sub && sub->is_arr())
            for (const Json& t : sub->arr) {
                if (t.as_str() == "axis") has_axis = true;
                if (t.as_str() == "log")  has_log = true;
            }
        CHECK(has_axis && has_log);

        Json ev;
        CHECK(wait_event(fd, buf, "axis", "", &ev, 1000));
        CHECK(ev.find("mpos") && ev.find("dpos") && ev.find("axis"));
    } else {
        CHECK(false);
    }
    CHECK(call_next(fd, buf, &id, "subscribe", "{\"topics\":[\"nope\"]}", &r) &&
          code_of(r) == "BAD_PARAM");
    if (call_next(fd, buf, &id, "unsubscribe", "{\"topics\":[\"axis\"]}", &r)) {
        const Json* rr = result_of(r);
        const Json* sub = rr ? rr->find("sub") : nullptr;
        CHECK(sub && sub->is_arr());
        bool has_axis = false;
        if (sub && sub->is_arr())
            for (const Json& t : sub->arr)
                if (t.as_str() == "axis") has_axis = true;
        CHECK(!has_axis);
    } else {
        CHECK(false);
    }
    if (call_next(fd, buf, &id, "unsubscribe", "", &r)) {
        const Json* rr = result_of(r);
        const Json* sub = rr ? rr->find("sub") : nullptr;
        CHECK(sub && sub->is_arr() && sub->arr.empty());
    } else {
        CHECK(false);
    }

    // ---- v0.8.6：mb 事件带「已用寄存器」位图（脚本访问过即标记；插件监视用）----
    {
        // 先经 cmd 访问两个寄存器（16 位 5 与浮点 10/11），再订阅 mb 取事件
        CHECK(call_next(fd, buf, &id, "cmd", "{\"line\":\"MODBUS_REG 5, 7\"}", &r) && ok_of(r));
        CHECK(call_next(fd, buf, &id, "cmd", "{\"line\":\"MODBUS_REG 5\"}", &r) && ok_of(r));  // 读也算
        CHECK(call_next(fd, buf, &id, "cmd", "{\"line\":\"MODBUS_IEEE 10, 1.5\"}", &r) && ok_of(r));
        CHECK(call_next(fd, buf, &id, "subscribe", "{\"topics\":[\"mb\"],\"hz\":50}", &r) && ok_of(r));
        Json ev;
        CHECK(wait_event(fd, buf, "mb", "", &ev, 1500));
        const Json* used = ev.find("used");
        CHECK(used && used->is_arr() && used->arr.size() == 4);
        unsigned long long w0 = 0;
        if (used && used->arr.size() == 4) w0 = std::strtoull(used->arr[0].as_str().c_str(), nullptr, 16);
        CHECK((w0 & (1ull << 5)) != 0);                       // 5 被访问
        CHECK((w0 & (1ull << 10)) != 0 && (w0 & (1ull << 11)) != 0);  // 浮点两字都被标记
        CHECK((w0 & (1ull << 7)) == 0);                       // 未访问的不置位
        CHECK(call_next(fd, buf, &id, "unsubscribe", "{\"topics\":[\"mb\"]}", &r) && ok_of(r));
    }

    // ---- D5: 断点 / 单步 / 恢复（确定性时序：断点停在已知行）----
    CHECK(call_next(fd, buf, &id, "script.compile",
                    "{\"src\":\"A = 0\\nA = A + 1\\nA = A + 1\\nEND\",\"engine\":\"basic\"}", &r) && ok_of(r));
    CHECK(call_next(fd, buf, &id, "breakpoint.add", "{\"line\":3}", &r) && ok_of(r));
    {
        const Json* rr = result_of(r);
        const Json* bps = rr ? rr->find("breakpoints") : nullptr;
        CHECK(bps && bps->is_arr() && bps->arr.size() == 1);
        if (bps && bps->is_arr() && bps->arr.size() == 1) {
            const Json* pl = bps->arr[0].find("line");
            CHECK(pl && pl->as_int(-1) == 3);
        }
    }
    CHECK(call_next(fd, buf, &id, "breakpoint.list", "", &r) && ok_of(r));
    if (call_next(fd, buf, &id, "script.run", "", &r)) {
        CHECK(ok_of(r));
        Json ev;
        CHECK(wait_event(fd, buf, "script", "PAUSED", &ev, 3000));
        CHECK(ev.find("line") && ev.find("line")->as_int(-1) == 3);
    } else {
        CHECK(false);
    }
    // 挂起态：script.status 报 PAUSED + 当前行
    if (call_next(fd, buf, &id, "script.status", "", &r)) {
        const Json* rr = result_of(r);
        CHECK(rr && rr->find("status")->as_str() == "PAUSED");
        CHECK(rr && rr->find("line")->as_int(-1) == 3);
    } else {
        CHECK(false);
    }
    // 单步：走一条语句，停在第 4 行（END）
    CHECK(call_next(fd, buf, &id, "script.step", "", &r) && ok_of(r));
    {
        Json ev;
        CHECK(wait_event(fd, buf, "script", "PAUSED", &ev, 3000));
        CHECK(ev.find("line") && ev.find("line")->as_int(-1) == 4);
    }
    // 恢复：跑完 DONE，A = 2
    CHECK(call_next(fd, buf, &id, "script.resume", "", &r) && ok_of(r));
    {
        Json ev;
        CHECK(wait_event(fd, buf, "script", "DONE", &ev, 3000));
    }
    if (call_next(fd, buf, &id, "var.get", "{\"name\":\"A\"}", &r)) {
        const Json* rr = result_of(r);
        CHECK(rr && rr->find("value")->as_num(-1) == 2);
    } else {
        CHECK(false);
    }
    // 清空断点：无参 = 全清（DAP setBreakpoints 全量替换语义）；再验证删单个
    CHECK(call_next(fd, buf, &id, "breakpoint.del", "", &r) && ok_of(r));
    {
        const Json* rr = result_of(r);
        const Json* bps = rr ? rr->find("breakpoints") : nullptr;
        CHECK(bps && bps->is_arr() && bps->arr.empty());
    }
    CHECK(call_next(fd, buf, &id, "breakpoint.add", "{\"line\":2}", &r) && ok_of(r));
    CHECK(call_next(fd, buf, &id, "breakpoint.del", "{\"line\":2}", &r) && ok_of(r));
    {
        const Json* rr = result_of(r);
        const Json* bps = rr ? rr->find("breakpoints") : nullptr;
        CHECK(bps && bps->is_arr() && bps->arr.empty());
    }

    // ---- D5: script.pause（真实暂停语义：无断点，靠 request_pause 在语句边界停住）----
    // 时序确定性：用**断点**先把脚本钉在运行态（不依赖循环时长），再清掉断点并 pause，
    // 验证「无断点也能被 pause 停住」这条独立路径。
    CHECK(call_next(fd, buf, &id, "script.compile",
                    "{\"src\":\"A = 0\\nWHILE A < 500000\\nA = A + 1\\nWEND\\nEND\",\"engine\":\"basic\"}", &r) && ok_of(r));
    CHECK(call_next(fd, buf, &id, "breakpoint.add", "{\"line\":3}", &r) && ok_of(r));
    if (call_next(fd, buf, &id, "script.run", "", &r)) {
        CHECK(ok_of(r));
        Json ev0;
        CHECK(wait_event(fd, buf, "script", "PAUSED", &ev0, 3000));   // 断点命中，钉住运行态
        CHECK(call_next(fd, buf, &id, "breakpoint.del", "", &r) && ok_of(r));  // 清空断点
        CHECK(call_next(fd, buf, &id, "script.resume", "", &r) && ok_of(r));   // 续跑（此时无断点）
        CHECK(call_next(fd, buf, &id, "script.pause", "", &r) && ok_of(r));    // 纯 pause 停住
        Json ev;
        CHECK(wait_event(fd, buf, "script", "PAUSED", &ev, 3000));
        CHECK(ev.find("line") && ev.find("line")->as_int(-1) >= 1);
        // 运行中 steps 必须取引擎实时值：否则两次 PAUSED 去重键相同会被吞（本用例曾 50% 闪失败）
        CHECK(ev.find("steps") && ev.find("steps")->as_int(-1) > 0);
        if (call_next(fd, buf, &id, "script.status", "", &r)) {
            const Json* rr = result_of(r);
            CHECK(rr && rr->find("status")->as_str() == "PAUSED");
        } else {
            CHECK(false);
        }
        CHECK(call_next(fd, buf, &id, "script.resume", "", &r) && ok_of(r));
        Json ev2;
        // 板上（A55）解释执行 1 亿次循环需数十秒：超时放宽到 60s（用例意图是「最终 DONE」）
        CHECK(wait_event(fd, buf, "script", "DONE", &ev2, 60000));
    } else {
        CHECK(false);
    }

    // ---- D4: 热更新（运行中 swap：编译到新实例并原子替换，旧实例被回收）----
    // 用断点把旧脚本停在已知行，保证 swap 时必然处于运行态（确定性，不靠循环时长）。
    // 循环量级取 500 万：板上（A55）数秒内跑完，PC 上亚秒——够「运行态」语义又不拖测试。
    CHECK(call_next(fd, buf, &id, "script.compile",
                    "{\"src\":\"A = 0\\nWHILE A < 500000\\nA = A + 1\\nWEND\\nEND\",\"engine\":\"basic\"}", &r) && ok_of(r));
    CHECK(call_next(fd, buf, &id, "breakpoint.add", "{\"line\":3}", &r) && ok_of(r));
    CHECK(call_next(fd, buf, &id, "script.run", "", &r) && ok_of(r));
    {
        Json ev;
        CHECK(wait_event(fd, buf, "script", "PAUSED", &ev, 3000));
    }
    // swap：新脚本编译到暂存实例，成功后原子替换；旧实例（挂起中）被 abort
    CHECK(call_next(fd, buf, &id, "script.compile",
                    "{\"src\":\"B = 42\\nEND\",\"engine\":\"basic\",\"swap\":true}", &r) && ok_of(r));
    {
        const Json* rr = result_of(r);
        CHECK(rr && rr->find("swapped") && rr->find("swapped")->as_bool() == true);
    }
    {
        Json ev;
        CHECK(wait_event(fd, buf, "script", "ABORTED", &ev, 3000));   // 旧脚本被回收
    }
    // 新脚本已编译好，可直接运行；断点不迁移（新实例断点集为空）
    CHECK(call_next(fd, buf, &id, "script.run", "", &r) && ok_of(r));
    {
        Json ev;
        CHECK(wait_event(fd, buf, "script", "DONE", &ev, 3000));
    }
    if (call_next(fd, buf, &id, "var.get", "{\"name\":\"B\"}", &r)) {
        const Json* rr = result_of(r);
        CHECK(rr && rr->find("value")->as_num(-1) == 42);
    } else {
        CHECK(false);
    }

    // ---- D4: swap 编译失败不影响运行中脚本 ----
    CHECK(call_next(fd, buf, &id, "script.compile",
                    "{\"src\":\"A = 0\\nWHILE A < 500000\\nA = A + 1\\nWEND\\nEND\",\"engine\":\"basic\"}", &r) && ok_of(r));
    CHECK(call_next(fd, buf, &id, "breakpoint.add", "{\"line\":3}", &r) && ok_of(r));
    CHECK(call_next(fd, buf, &id, "script.run", "", &r) && ok_of(r));
    {
        Json ev;
        CHECK(wait_event(fd, buf, "script", "PAUSED", &ev, 3000));
    }
    CHECK(call_next(fd, buf, &id, "script.compile",
                    "{\"src\":\"IF 1\\nEND\",\"engine\":\"basic\",\"swap\":true}", &r) &&
          code_of(r) == "COMPILE_ERROR");
    // 旧脚本未受影响：清断点后续跑照常完成
    CHECK(call_next(fd, buf, &id, "breakpoint.del", "", &r) && ok_of(r));
    CHECK(call_next(fd, buf, &id, "script.resume", "", &r) && ok_of(r));
    {
        Json ev;
        // 同上：板上 1 亿次循环解释执行需数十秒
        CHECK(wait_event(fd, buf, "script", "DONE", &ev, 60000));
    }

    ::close(fd);
    ds.stop();

    std::printf("[debug_server_test] %d/%d 通过\n", g_total - g_fail, g_total);
    return g_fail == 0 ? 0 : 1;
}
