// debug_server.cpp —— 调试通道服务端实现（TCP JSON-Lines，D1~D5）
// 规范与角色说明见 debug_server.h 顶部注释；报文样例见 docs/planA/16。
#include "script/debug_server.h"

#include <dirent.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>

#include "script/boot_config.h"    // D8：主文件清单（.boot）读写
#include "script/port_config.h"    // D9：端口数量上限（.portmax）读写
#include "script/port_manager.h"   // D9 运行期端口上限；PortManager::is_reserved_port
#include "script/source_include.h" // 编译前 INCLUDE 展开（多文件）

namespace kx {
namespace {

constexpr int    kBacklog      = 4;
constexpr int    kPollMs       = 20;        // 会话轮询间隔：收请求与推事件的交错粒度
constexpr size_t kMaxLineBytes = 4u << 20;  // 单行上限 4MB（防爆；源码一般 < 100KB）
// 会话空闲超时（V-C-01 拍板）：60s 内未收到任何客户端数据 → 断开会话。
// 背景：DebugServer 单客户端；客户端异常终止留下的**半开连接**会永久占住 accept 槽位，
// 后续所有连接「连上但无响应」。插件 v0.3.6+ 每 10s 发 sys.ping 保活，正常使用不会触发。
constexpr long long kIdleTimeoutMs = 60 * 1000;
constexpr size_t kMaxLogQueue  = 1000;      // 未取走的打印行上限
constexpr int    kMbSegment    = 64;        // mb 事件每包寄存器数（256 个分 4 包轮推）

long long epoch_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

Json jstr(const std::string& s) { return Json::make_str(s); }
Json jnum(double v)             { return Json::make_num(v); }
Json jint(long long v)          { return Json::make_int(v); }

// D6 文件管理：文件名校验（纯文件名，防路径穿越；只允许字母/数字/._-，不以 . 开头）
// 文件名校验唯一实现在 boot_config.h（D6 与主文件清单共用），此处仅保留调用别名语义注释：
// valid_script_file_name = 字母/数字/._-，不以 . 开头，长度 1..128（防路径穿越）。

// 逐级创建目录（POSIX mkdir 无 -p 语义；实机曾因 /userdata/kine-x 不存在导致落盘失败）
bool mkdir_p(const std::string& path) {
    if (path.empty()) return false;
    size_t i = (path[0] == '/') ? 1 : 0;
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        const std::string cur = path.substr(0, j);
        if (!cur.empty() && cur != "/" &&
            ::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
            return false;
        }
        if (j == path.size()) break;
        i = j + 1;
    }
    return true;
}

// 已订阅主题清单（顺序固定，便于日志/用例比对）
Json sub_list_json(bool axis, bool bus, bool mb, bool log, bool conn) {
    Json a = Json::make_arr();
    if (axis) a.push(jstr("axis"));
    if (bus)  a.push(jstr("bus"));
    if (mb)   a.push(jstr("mb"));
    if (log)  a.push(jstr("log"));
    if (conn) a.push(jstr("conn"));
    return a;
}

// subscribe / unsubscribe 的回包体：{"sub":[...]}（见 docs/planA/16 §4.5）
Json sub_result_json(bool axis, bool bus, bool mb, bool log, bool conn) {
    Json o = Json::make_obj();
    o.set("sub", sub_list_json(axis, bus, mb, log, conn));
    return o;
}

// kx::Value -> JSON（NIL 表示为 null；供 var.get）
Json jval(const Value& v) {
    if (v.type == Value::NUM) return jnum(v.num);
    if (v.type == Value::STR) return jstr(v.str);
    return Json{};
}

std::string trim_lower(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    std::string k = s.substr(b, e - b);
    for (char& c : k) c = (char)std::tolower((unsigned char)c);
    return k;
}

std::string trim_upper(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    std::string k = s.substr(b, e - b);
    for (char& c : k) c = (char)std::toupper((unsigned char)c);
    return k;
}

long long period_ms(int hz) {
    if (hz < 1) hz = 1;
    if (hz > 200) hz = 200;
    return 1000 / hz;
}

// 解析 cmd 的 line："POS 10 20" / "EN"（逗号/分号/空白都当分隔符）
bool parse_cmd_line(const std::string& line, std::string* name, std::vector<double>* args) {
    std::string s = line;
    for (char& c : s) {
        if (c == ',' || c == ';' || c == '\t' || c == '\r') c = ' ';
    }
    std::vector<std::string> toks;
    std::string cur;
    for (char c : s) {
        if (c == ' ') {
            if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) toks.push_back(cur);
    if (toks.empty()) return false;

    args->clear();
    *name = trim_upper(toks[0]);
    for (size_t i = 1; i < toks.size(); ++i)
        args->push_back(std::strtod(toks[i].c_str(), nullptr));
    return true;
}

// 尚未实装的方法名：命中即明确回 NOT_SUPPORTED（不伪装可用）。
// D4（script.compile 带 swap）与 D5（script.pause/resume/step、breakpoint.*）已实装，不再列此。
const std::set<std::string>& not_supported_methods() {
    static const std::set<std::string> s = {
        // D5 余项：变量监视 / 标签断点 / 换引擎（本期不做）
        "breakpoint.remove", "breakpoint.set", "breakpoint.clear",
        "watch.add", "watch.remove", "var.watch", "engine.swap",
        // D4 余项：更细的运行时快照
        "state.dump", "machine.snapshot", "trace.set", "trace.get",
        "task.list", "mb.write", "io.read", "io.write",
        "scope.start", "scope.stop", "axis.monitor"
    };
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
DebugServer::DebugServer(Shared& sh, const DebugCfg& cfg, bool allow_script,
                         const std::string& engine_hint, std::atomic<bool>* restart_flag)
    : sh_(sh), cfg_(cfg), allow_script_(allow_script), restart_flag_(restart_flag) {
    ScriptLanguage lang = ScriptLanguage::BASIC;
    if (parse_script_language(engine_hint, &lang)) lang_ = lang;

    // 语言绑定（用户拍板 2026-09-25）：调试口独占引擎时，语言以 DEBUG_SCRIPT_DIR 内
    // 现有脚本为准（空目录 = AUTO：两种都可编译，首次带 name 落盘后绑定）；
    // engine_hint 仅在目录为空/混合时作兜底初值。
    if (allow_script_) {
        const LangBind bind = scan_lang_bind();
        if (bind == LangBind::BASIC) lang_ = ScriptLanguage::BASIC;
        else if (bind == LangBind::LUA) lang_ = ScriptLanguage::LUA;
    }

    // 设备宿主：与自动脚本线程**同一套**（只经 kx::Shared 下发，不碰 ecrt / RT）。
    //   * 中止标志挂 stop_：服务停止时在跑的脚本会被及时叫停；
    //   * 打印落点进队列（logs_），由会话线程按订阅推成 log 事件。
    host_ = std::make_unique<MotionHost>(sh_);
    host_->set_abort_flag(&stop_);
    host_->set_output([this](const std::string& line) {
        std::lock_guard<std::mutex> lk(log_mtx_);
        if (logs_.size() >= kMaxLogQueue) logs_.erase(logs_.begin());   // 环形丢弃最旧
        logs_.push_back(line);
    });
}

DebugServer::~DebugServer() {
    stop();
    if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
}

bool DebugServer::listen_now(std::string* err) {
    const auto fail = [&](const std::string& m) { if (err) *err = m; return false; };

    if (!cfg_.enable) return fail("DEBUG_ENABLE=0，调试通道未启用");
    if (cfg_.port < 0 || cfg_.port > 65535) return fail("DEBUG_PORT 非法: " + std::to_string(cfg_.port));
    if (PortManager::is_reserved_port(cfg_.port))
        return fail("DEBUG_PORT 不能是系统保留端口（22=SSH / 80=HTTP）");

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return fail(std::string("socket(): ") + std::strerror(errno));

    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons((unsigned short)cfg_.port);

    const std::string bind = cfg_.bind.empty() ? std::string("0.0.0.0") : cfg_.bind;
    if (bind == "0.0.0.0" || bind == "*") {
        a.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, bind.c_str(), &a.sin_addr) != 1) {
        ::close(fd);
        return fail("DEBUG_BIND 不是合法 IPv4 地址: " + bind);
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        const std::string m = "bind(" + bind + ":" + std::to_string(cfg_.port) + ") 失败: " +
                              std::strerror(errno) +
                              "（端口被占用？见 ss -tlnp）";
        ::close(fd);
        return fail(m);
    }
    if (::listen(fd, kBacklog) != 0) {
        const std::string m = std::string("listen() 失败: ") + std::strerror(errno);
        ::close(fd);
        return fail(m);
    }

    sockaddr_in got{};
    socklen_t   gl = sizeof(got);
    bound_port_ = (::getsockname(fd, reinterpret_cast<sockaddr*>(&got), &gl) == 0)
                      ? (int)ntohs(got.sin_port)
                      : cfg_.port;
    listen_fd_ = fd;
    return true;
}

void DebugServer::start() {
    if (listen_fd_ < 0 || th_.joinable()) return;
    th_ = std::thread([this] { accept_loop(); });
}

void DebugServer::stop() {
    stop_.store(true);
    // 运行中的脚本：request_abort() 置位即返回（可能在别的线程跑），
    // 且 MotionHost 的 aborted() 也挂在同一个 stop_ 上，长等待会被及时叫停。
    if (slot_.loaded()) slot_.engine()->request_abort();
    if (retired_) retired_->request_abort();   // D4：worker 可能仍在被换出的旧实例上

    const int c = client_fd_.load();
    if (c >= 0) ::shutdown(c, SHUT_RDWR);
    if (listen_fd_ >= 0) ::shutdown(listen_fd_, SHUT_RDWR);

    if (th_.joinable()) th_.join();
    if (run_th_.joinable()) run_th_.join();
}

// ---------------------------------------------------------------------------
// 线程与连接
// ---------------------------------------------------------------------------
void DebugServer::accept_loop() {
    while (!stop_.load()) {
        pollfd pfd{};
        pfd.fd     = listen_fd_;
        pfd.events = POLLIN;
        const int pr = ::poll(&pfd, 1, 200);
        if (pr <= 0) continue;
        if (!(pfd.revents & POLLIN)) continue;

        sockaddr_in cli{};
        socklen_t   cl = sizeof(cli);
        const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&cli), &cl);
        if (fd < 0) continue;

        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        client_fd_.store(fd);
        serve(fd);
        client_fd_.store(-1);
        ::close(fd);
    }
}

void DebugServer::serve(int fd) {
    reset_session();

    std::string buf;
    long long last_rx_ms = epoch_ms();   // 最近一次收到客户端数据的时间（空闲超时判据）
    while (!stop_.load()) {
        pollfd pfd{};
        pfd.fd     = fd;
        pfd.events = POLLIN;
        const int pr = ::poll(&pfd, 1, kPollMs);

        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr > 0 && (pfd.revents & POLLIN)) {
            char tmp[8192];
            const ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
            if (n <= 0) break;                       // 对端关闭 / 出错
            last_rx_ms = epoch_ms();                 // 收到数据 → 刷新活跃时间
            buf.append(tmp, (size_t)n);

            size_t pos = 0;
            while ((pos = buf.find('\n')) != std::string::npos) {
                std::string one = buf.substr(0, pos);
                buf.erase(0, pos + 1);
                if (!one.empty() && one.back() == '\r') one.pop_back();
                if (!one.empty()) dispatch(fd, one);
                if (stop_.load()) break;
            }
            if (buf.size() > kMaxLineBytes) break;   // 单行过大：断开（避免内存被拖爆）
        } else if (pr > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            break;
        }

        // 空闲超时：半开连接（客户端异常终止未挥手）会永久占住单客户端槽位
        if (epoch_ms() - last_rx_ms > kIdleTimeoutMs) {
            break;
        }

        push_events(fd);
    }

    // 收尾：客户端断开后脚本不该继续跑（它由这个会话上传），先请求中止再回收 worker，
    //       避免下一个会话与其并发碰同一引擎/宿主。
    if (run_th_.joinable()) {
        if (engine_running() && slot_.loaded()) slot_.engine()->request_abort();
        if (retired_) retired_->request_abort();
        run_th_.join();
    }
    release_retired();   // D4：worker 已结束，被换出的旧引擎此时才可释放
}

void DebugServer::reset_session() {
    authed_              = cfg_.token.empty();
    sub_axis_            = Sub{};
    sub_bus_             = Sub{};
    sub_mb_              = Sub{};
    sub_log_             = false;
    // 会话基线：清掉上一会话的运行快照，并把跳变基线设为「空闲」，避免新连接一上来
    // 就收到一条无意义的 script READY 事件。
    {
        std::lock_guard<std::mutex> lk(run_mtx_);
        snap_ = RunSnap{};
    }
    last_script_key_ = "STOP:READY|0|0";
    mb_cursor_       = 0;
    {
        std::lock_guard<std::mutex> lk(log_mtx_);
        logs_.clear();
    }
}

// ---------------------------------------------------------------------------
// 收发
// ---------------------------------------------------------------------------
void DebugServer::send_line(int fd, const Json& j) {
    std::string s = json_dump(j);
    s += '\n';
    size_t off = 0;
    while (off < s.size()) {
        const ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return;
        }
        off += (size_t)n;
    }
}

void DebugServer::reply_ok(int fd, long long id, Json r) {
    Json o = Json::make_obj();
    o.set("id", jint(id));
    o.set("ok", Json::make_bool(true));
    o.set("r",  std::move(r));
    send_line(fd, o);
}

void DebugServer::reply_err(int fd, long long id, const Err& e) {
    Json err = Json::make_obj();
    err.set("code", jstr(e.code));
    err.set("msg",  jstr(e.msg));
    if (e.line >= 0) err.set("line", jint(e.line));

    Json o = Json::make_obj();
    o.set("id",  jint(id));
    o.set("ok",  Json::make_bool(false));
    o.set("err", std::move(err));
    send_line(fd, o);
}

void DebugServer::dispatch(int fd, const std::string& line) {
    Json req;
    std::string jerr;
    Err e;
    if (!json_parse(line, &req, &jerr) || !req.is_obj()) {
        e.code = "BAD_REQUEST";
        e.msg  = jerr.empty() ? "报文不是 JSON 对象" : jerr;
        reply_err(fd, -1, e);
        return;
    }

    long long id = -1;
    if (const Json* pi = req.find("id"); pi && pi->is_num()) id = pi->as_int(-1);

    const Json* pm = req.find("m");
    if (!pm || !pm->is_str() || pm->as_str().empty()) {
        e.code = "BAD_REQUEST";
        e.msg  = "缺少 m 字段（方法名）";
        reply_err(fd, id, e);
        return;
    }

    // 参数在 p 里；缺失/非对象时传空对象，由各方法自行报 BAD_PARAM
    Json p;
    if (const Json* pp = req.find("p"); pp && pp->is_obj()) p = *pp;

    Json r;
    if (rpc(pm->as_str(), p, &r, &e)) reply_ok(fd, id, std::move(r));
    else                              reply_err(fd, id, e);
}

// ---------------------------------------------------------------------------
// RPC 分发
// ---------------------------------------------------------------------------
bool DebugServer::rpc(const std::string& m, const Json& p, Json* r, Err* e) {
    // 可选加固：配了 DEBUG_TOKEN 时，除 auth / sys.ping 外必须先认证
    if (!authed_ && m != "auth" && m != "sys.ping") {
        e->code = "BAD_REQUEST";
        e->msg  = "需要先认证：先发 {\"m\":\"auth\",\"token\":\"...\"}（DEBUG_TOKEN 已启用）";
        return false;
    }

    if (m == "sys.ping")     { *r = Json::make_obj(); return true; }
    if (m == "sys.restart")  return m_sys_restart(p, r, e);   // D7 重启控制器
    if (m == "auth")         return m_auth(p, r, e);
    if (m == "sys.info")     return m_sys_info(p, r, e);
    if (m == "script.compile") return m_script_compile(p, r, e);
    if (m == "script.run")     return m_script_run(p, r, e);
    if (m == "script.stop")    return m_script_stop(p, r, e);
    if (m == "script.status")  return m_script_status(p, r, e);
    if (m == "script.pause")   return m_script_pause(p, r, e);
    if (m == "script.resume")  return m_script_resume(p, r, e);
    if (m == "script.step")    return m_script_step(p, r, e);
    if (m == "breakpoint.add")  return m_breakpoint_add(p, r, e);
    if (m == "breakpoint.del")  return m_breakpoint_del(p, r, e);
    if (m == "breakpoint.list") return m_breakpoint_list(p, r, e);
    if (m == "var.list")       return m_var_list(p, r, e);
    if (m == "var.get")        return m_var_get(p, r, e);
    if (m == "var.set")        return m_var_set(p, r, e);
    if (m == "file.list")      return m_file_list(p, r, e);     // D6 文件管理
    if (m == "file.get")       return m_file_get(p, r, e);      // D6 文件管理
    if (m == "file.del")       return m_file_del(p, r, e);      // D6 文件管理
    if (m == "boot.get")       return m_boot_get(p, r, e);      // D8 主文件（开机运行）
    if (m == "boot.set")       return m_boot_set(p, r, e);
    if (m == "boot.clear")     return m_boot_clear(p, r, e);
    if (m == "port.max.get")   return m_port_max_get(p, r, e);  // D9 端口数量上限
    if (m == "port.max.set")   return m_port_max_set(p, r, e);  // D9 端口数量上限
    if (m == "axis.snapshot")  return m_axis_snapshot(p, r, e);
    if (m == "cmd")            return m_cmd(p, r, e);
    if (m == "subscribe")      return m_subscribe(p, r, e);
    if (m == "unsubscribe")    return m_unsubscribe(p, r, e);

    if (not_supported_methods().count(m)) {
        e->code = "NOT_SUPPORTED";
        e->msg  = "方法 " + m + " 本固件未实装（能力位见 sys.info 的 caps，见 docs/planA/13 §4.4）";
        return false;
    }

    e->code = "UNKNOWN_METHOD";
    e->msg  = "未知方法: " + m;
    return false;
}

// ---------------------------------------------------------------------------
// D1：系统 / 认证
// ---------------------------------------------------------------------------
bool DebugServer::m_sys_info(const Json&, Json* r, Err*) {
    Json info = Json::make_obj();
    info.set("ver",        jstr("0.1.0"));
    // engine：脚本目录推导（"basic"/"lua"/"auto"/"mixed"）；自动脚本占用时为 SCRIPT_ENGINE
    info.set("engine",     jstr(language_display()));
    // boot：主文件（开机运行）清单记录（空 = 未设置）；有效性用 boot.get
    info.set("boot",       jstr(read_boot_name(script_dir_path())));
    info.set("axis_count", jint(sh_.axis_count));
    info.set("debug_port", jint(bound_port_ > 0 ? bound_port_ : cfg_.port));

    Json caps = Json::make_arr();
    caps.push(jstr("d1"));
    if (allow_script_) {
        caps.push(jstr("d2"));
        caps.push(jstr("d4"));                       // D4 热更新（script.compile 带 swap）
        if (d5_ready()) caps.push(jstr("d5"));       // D5 断点/暂停/单步（BASIC 语句级 / Lua 行级）
    }
    caps.push(jstr("d3"));
    caps.push(jstr("d6"));                           // D6 文件管理（file.list/get/del + 下载落盘）
    if (!cfg_.restart_cmd.empty()) {
        caps.push(jstr("d7"));                       // D7 重启控制器（sys.restart；命令可配置）
    }
    caps.push(jstr("d8"));                           // D8 主文件（开机运行）：boot.get/set/clear
    caps.push(jstr("d9"));                           // D9 端口数量上限：port.max.get/set（持久化 .portmax）
    caps.push(jstr("d10"));                          // D10 通讯状态：conn 订阅（端口快照 + EtherCAT 主/从明细）
    info.set("caps", std::move(caps));

    *r = std::move(info);
    return true;
}

// D7：重启控制器（用户拍板 2026-09-25，插件菜单「控制器 → 重启控制器」）。
// 先回应答，再异步执行配置的重启命令（默认 `sudo -n systemctl restart kine-x.service`）；
// 命令失败（如无 systemd / 无免密 sudo）则置 restart_flag 并 SIGTERM，走 main 的优雅收尾，
// 以失败码退出交 `Restart=on-failure` 兜底。两条路都先经 main 的去使能收尾，不硬杀。
bool DebugServer::m_sys_restart(const Json&, Json* r, Err* e) {
    if (cfg_.restart_cmd.empty()) {
        e->code = "NOT_SUPPORTED";
        e->msg  = "控制器未启用重启（DEBUG_RESTART_CMD 为空）";
        return false;
    }

    Json out = Json::make_obj();
    out.set("restarting", Json::make_bool(true));
    *r = std::move(out);

    const std::string cmd = cfg_.restart_cmd;
    std::thread([this, cmd] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));   // 先让应答刷出
        const int rc = std::system(cmd.c_str());
        if (rc != 0) {                                  // 兜底：优雅退出 + 失败码 → systemd 重启
            if (restart_flag_) restart_flag_->store(true);
            std::raise(SIGTERM);
        }
    }).detach();
    return true;
}

bool DebugServer::m_auth(const Json& p, Json* r, Err* e) {
    *r = Json::make_obj();
    if (cfg_.token.empty()) { authed_ = true; return true; }   // 未启用 token：直接放行

    const Json* pt = p.find("token");
    if (!pt || !pt->is_str() || pt->as_str() != cfg_.token) {
        e->code = "BAD_PARAM";
        e->msg  = "token 不正确（见控制器 config/app.conf 的 DEBUG_TOKEN）";
        return false;
    }
    authed_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// D2：脚本编译 / 运行 / 停止 / 状态
// ---------------------------------------------------------------------------
bool DebugServer::m_script_compile(const Json& p, Json* r, Err* e) {
    const Json* psrc = p.find("src");
    if (!psrc || !psrc->is_str()) {
        e->code = "BAD_PARAM";
        e->msg  = "缺少 src（源码字符串）";
        return false;
    }

    // D4：swap 必须先于 run_th_.join() 判定——worker 可能正卡在断点上，
    // join() 会一直等到旧脚本跑完，热更新就永远做不了（见下方 swap 分支）。
    bool want_swap = false;
    if (const Json* psw = p.find("swap"); psw && psw->is_bool()) want_swap = psw->as_bool(false);

    // D6：可选 name——编译成功后把源码落盘到 DEBUG_SCRIPT_DIR，供插件「控制器文件 / 同步」拉取。
    // 不带 name 时行为与旧版一致（只编译不落盘）；落盘失败不阻断编译结果（回包带 saved=false）。
    std::string name;
    if (const Json* pnm = p.find("name"); pnm && pnm->is_str() && !pnm->as_str().empty()) {
        if (!valid_script_file_name(pnm->as_str())) {
            e->code = "BAD_PARAM";
            e->msg  = "name 只能是纯文件名（字母/数字/._-，不以 . 开头，不含路径分隔符）";
            return false;
        }
        name = pnm->as_str();
    }

    if (!(want_swap && run_th_.joinable())) {       // 非热更新：先等上一轮 worker 收尾
        if (run_th_.joinable()) run_th_.join();
        release_retired();
    }

    // ---- 语言绑定（用户拍板 2026-09-25：语言由 DEBUG_SCRIPT_DIR 内现有脚本决定）----
    //   * 目录为空 → AUTO：basic / lua 都可编译，带 name 落盘成功后即绑定；
    //   * 只有 .bas 或只有 .lua → 绑定该语言，另一种明确 ENGINE_MISMATCH（不静默转换）；
    //   * 两种都有 → MIXED：拒绝新编译，提示先删除一种（file.del / 插件文件列表）。
    // 自动脚本占用引擎（allow_script_=false）时语言仍是 SCRIPT_ENGINE，不受目录影响。
    // v0.8.0：语言裁决完整化——除「请求 vs 目录绑定」外，还要保证**已装载引擎**与目标语言
    // 一致（目录内容可能在会话期间被改过：例如先编 lua、删掉后目录只剩 .bas，此时必须
    // 卸载 lua 重装 basic，否则会把 BASIC 源码交给 Lua 引擎编译）。
    ScriptLanguage want = lang_;
    bool engine_explicit = false;
    if (const Json* pe = p.find("engine"); pe && pe->is_str()) {
        if (!parse_script_language(pe->as_str(), &want)) {
            e->code = "BAD_PARAM";
            e->msg  = "engine 只能是 basic | lua（收到: " + pe->as_str() + "）";
            return false;
        }
        engine_explicit = true;
    }
    const LangBind bind = scan_lang_bind();
    if (bind == LangBind::MIXED) {
        e->code = "ENGINE_MISMATCH";
        e->msg  = "控制器脚本目录同时存在 .bas 与 .lua 脚本，无法确定语言："
                  "请先删除其中一种（file.del / 插件「控制器文件」列表），再编译";
        return false;
    }
    ScriptLanguage target = want;
    if (bind == LangBind::BASIC)      target = ScriptLanguage::BASIC;
    else if (bind == LangBind::LUA)   target = ScriptLanguage::LUA;
    else if (!engine_explicit)        target = lang_;      // AUTO 且未指定：沿用当前语言
    if (!engine_explicit) want = target;                   // 未指定：按目录绑定（不误报不匹配）

    if (bind == LangBind::BASIC || bind == LangBind::LUA) {
        if (want != target) {
            e->code = "ENGINE_MISMATCH";
            if (allow_script_) {
                e->msg = std::string("控制器脚本目录中为 ") + script_language_name(target) +
                         " 脚本，不能编译 " + script_language_name(want) +
                         "（语言由控制器现有脚本决定：目录为空时两种都可编译，见 docs/planA/13）";
            } else {
                e->msg = std::string("控制器当前脚本语言为 ") + script_language_name(target) +
                         "（SCRIPT_ENGINE / 自动脚本），不能编译 " + script_language_name(want) +
                         "（同一时刻只加载一种脚本语言，见 engine_rule.h）";
            }
            return false;
        }
    }
    if (target != lang_) {                            // 引擎与目标语言不一致：未运行才可换
        if (engine_running()) {
            e->code = "BUSY";
            e->msg  = "脚本正在运行，先 script.stop 再切换脚本语言";
            return false;
        }
        slot_.unload();
        lang_        = target;
        compiled_    = false;
        compiled_ok_ = false;
    }
    want = target;

    // ---- 多文件包含（用户拍板 2026-09-26）----
    // 编译前展开 INCLUDE "sub.bas"（BASIC）/ include("sub.lua")（Lua）：其它脚本文件
    // 作为子程序并入主文件。失败按 COMPILE_ERROR 明确报错（缺文件/跨语言/循环/超深）。
    std::string src_expanded;
    {
        const std::string from = name.empty() ? std::string("<compile>") : name;
        std::string ierr;
        if (!expand_source_includes(psrc->as_str(), script_dir_path(),
                                    script_language_name(want), from, &src_expanded, &ierr)) {
            e->code = "COMPILE_ERROR";
            e->msg  = ierr;
            return false;
        }
    }
    const std::string& src_final = src_expanded;

    // D4 热更新（13 §5.1）：swap=true → 编译到**新实例**，校验通过后原子替换；
    // 运行中脚本在编译期间不受影响，替换后旧实例被 abort 回收。
    if (!require_script_ready(e)) return false;

    const bool running = engine_running();
    if (want_swap && running && slot_.loaded()) {
        IScriptEngine* staged = slot_.create_staged(host_.get());
        if (!staged) {
            e->code = "RUNTIME_ERROR";
            e->msg  = "暂存引擎创建失败（D4 热更新）";
            return false;
        }
        staged->enable_line_hooks(true);         // D5：换入的暂存引擎同样支持断点/单步
        std::string cerr;
        const bool ok = staged->compile(src_final, &cerr);
        if (!ok) {
            const int err_line = staged->error_line();
            delete staged;                       // 编译失败：运行中脚本不受任何影响
            e->code = "COMPILE_ERROR";
            e->msg  = cerr.empty() ? "编译失败" : cerr;
            e->line = err_line;
            return false;
        }
        slot_.engine()->request_abort();         // 旧实例叫停（worker 随后退出）
        retired_.reset(slot_.replace(staged));   // 原子替换；旧实例持有到 worker 结束
        compiled_    = true;
        compiled_ok_ = true;
        {   // 快照重置为新实例的干净状态（READY/0）：steps 语义 = 每实例独立从 0 起步
            std::lock_guard<std::mutex> lk(run_mtx_);
            snap_ = RunSnap{};
        }

        Json labels = Json::make_arr();
        for (const auto& l : slot_.engine()->labels()) labels.push(jstr(l));
        Json out = Json::make_obj();
        out.set("labels",  std::move(labels));
        out.set("swapped", Json::make_bool(true));
        if (!name.empty()) {
            std::string serr;
            out.set("saved", Json::make_bool(save_script_file(name, psrc->as_str(), &serr)));
            if (!serr.empty()) out.set("save_error", jstr(serr));
        }
        *r = std::move(out);
        return true;
    }

    if (!run_th_.joinable()) release_retired();   // 非热更新路径：worker 已 join，可释放旧实例
    if (!ensure_engine(e)) return false;
    if (engine_running()) {
        e->code = "BUSY";
        e->msg  = "脚本正在运行，先 script.stop 再重新编译";
        return false;
    }

    IScriptEngine* eng = slot_.engine();
    std::string cerr;
    if (!eng->compile(src_final, &cerr)) {
        e->code = "COMPILE_ERROR";
        e->msg  = cerr.empty() ? "编译失败" : cerr;
        e->line = eng->error_line();
        compiled_    = false;
        compiled_ok_ = false;
        return false;
    }
    compiled_    = true;
    compiled_ok_ = true;

    Json labels = Json::make_arr();
    for (const auto& l : eng->labels()) labels.push(jstr(l));

    Json out = Json::make_obj();
    out.set("labels",  std::move(labels));
    out.set("swapped", Json::make_bool(false));   // 未运行时 swap 等价普通编译
    if (!name.empty()) {
        std::string serr;
        out.set("saved", Json::make_bool(save_script_file(name, psrc->as_str(), &serr)));
        if (!serr.empty()) out.set("save_error", jstr(serr));
    }
    *r = std::move(out);
    return true;
}

bool DebugServer::m_script_run(const Json&, Json* r, Err* e) {
    if (run_th_.joinable()) run_th_.join();
    release_retired();
    if (!require_script_ready(e)) return false;
    if (!compiled_ok_) {
        e->code = "RUNTIME_ERROR";
        e->msg  = "尚未编译脚本（先 script.compile）";
        return false;
    }
    if (engine_running()) {
        e->code = "BUSY";
        e->msg  = "脚本已在运行";
        return false;
    }
    if (!ensure_engine(e)) return false;

    IScriptEngine* eng = slot_.engine();
    {
        std::lock_guard<std::mutex> lk(run_mtx_);
        snap_.status     = "READY";
        snap_.steps      = 0;
        snap_.error_line = 0;
        snap_.error.clear();
        snap_.running    = true;
    }

    const unsigned long long budget =
        cfg_.max_steps > 0 ? (unsigned long long)cfg_.max_steps : 5000000ULL;

    // worker 线程跑脚本：会话线程继续 poll，既能推事件也能及时响应 script.stop
    run_th_ = std::thread([this, eng, budget] {
        const IScriptEngine::Status st = eng->run(budget);
        RunSnap s;
        s.status     = IScriptEngine::status_name(st);
        s.steps      = eng->steps();
        s.error_line = eng->error_line();
        s.error      = eng->error();
        s.running    = false;
        std::lock_guard<std::mutex> lk(run_mtx_);
        snap_ = s;
    });

    Json out = Json::make_obj();
    out.set("status", jstr("READY"));
    *r = std::move(out);
    return true;
}

bool DebugServer::m_script_stop(const Json&, Json* r, Err* e) {
    if (!require_script_ready(e)) return false;

    const RunSnap s = run_snap();
    if (s.running && slot_.loaded()) slot_.engine()->request_abort();
    if (retired_) retired_->request_abort();   // D4：worker 可能仍在被换出的旧实例上

    Json out = Json::make_obj();
    out.set("status", jstr(s.running ? "ABORTED" : s.status));
    *r = std::move(out);
    return true;
}

bool DebugServer::m_script_status(const Json&, Json* r, Err*) {
    RunSnap s = run_snap();
    bool paused_now = false;
    int  pause_line = 0;
    if (s.running && slot_.loaded()) {           // D5：挂起态从引擎直读（worker 阻塞在 run() 里）
        paused_now = slot_.engine()->paused();
        pause_line = slot_.engine()->current_line();
        s.steps    = slot_.engine()->steps();    // 运行中取引擎实时步数（RunSnap 只在结束时发布）
    }
    Json out = Json::make_obj();
    out.set("status",     jstr(paused_now ? "PAUSED" : (s.running ? "READY" : s.status)));
    out.set("steps",      jint((long long)s.steps));
    out.set("error_line", jint(s.error_line));
    out.set("line",       jint(paused_now ? pause_line : s.error_line));
    if (!s.error.empty()) out.set("error", jstr(s.error));
    *r = std::move(out);
    return true;
}

// ---------------------------------------------------------------------------
// D5：暂停 / 恢复 / 单步 / 断点（BASIC 语句级 / Lua 行级；见 docs/planA/13 §5.2）
// 状态前置不满足时回 BAD_PARAM（协议错误码表未设状态类码，见 16 §9 登记）。
// ---------------------------------------------------------------------------
bool DebugServer::m_script_pause(const Json&, Json* r, Err* e) {
    if (!require_script_ready(e)) return false;
    if (!d5_ready()) {
        e->code = "NOT_SUPPORTED";
        e->msg  = std::string("D5 暂停/断点需要引擎的调试钩子（当前引擎: ") +
                  script_language_name(lang_) + "）";
        return false;
    }
    if (!ensure_engine(e)) return false;
    if (!engine_running()) {
        e->code = "BAD_PARAM";
        e->msg  = "脚本未在运行，无法暂停";
        return false;
    }
    slot_.engine()->request_pause();             // 下一条语句边界生效；PAUSED 事件随后推出
    Json out = Json::make_obj();
    out.set("status", jstr("PAUSED"));
    *r = std::move(out);
    return true;
}

bool DebugServer::m_script_resume(const Json&, Json* r, Err* e) {
    if (!require_script_ready(e)) return false;
    if (!d5_ready()) {
        e->code = "NOT_SUPPORTED";
        e->msg  = std::string("D5 暂停/断点需要引擎的调试钩子（当前引擎: ") +
                  script_language_name(lang_) + "）";
        return false;
    }
    if (!ensure_engine(e)) return false;
    if (!engine_running()) {
        e->code = "BAD_PARAM";
        e->msg  = "脚本未在运行，无法恢复";
        return false;
    }
    slot_.engine()->resume();
    Json out = Json::make_obj();
    out.set("status", jstr("READY"));
    *r = std::move(out);
    return true;
}

bool DebugServer::m_script_step(const Json&, Json* r, Err* e) {
    if (!require_script_ready(e)) return false;
    if (!d5_ready()) {
        e->code = "NOT_SUPPORTED";
        e->msg  = std::string("D5 暂停/断点需要引擎的调试钩子（当前引擎: ") +
                  script_language_name(lang_) + "）";
        return false;
    }
    if (!ensure_engine(e)) return false;
    if (!engine_running()) {
        e->code = "BAD_PARAM";
        e->msg  = "脚本未在运行，无法单步";
        return false;
    }
    if (!slot_.engine()->paused()) {
        e->code = "BAD_PARAM";
        e->msg  = "脚本未处于暂停态，无法单步（先 script.pause 或命中断点）";
        return false;
    }
    slot_.engine()->request_step();              // 走一条语句后在下一行边界再停
    Json out = Json::make_obj();
    out.set("status", jstr("PAUSED"));
    *r = std::move(out);
    return true;
}

// 断点列表 -> JSON（[{line:N},...]，对齐 Mock 与 16 §2.4）
static Json breakpoints_json(const std::vector<int>& lines) {
    Json arr = Json::make_arr();
    for (int ln : lines) {
        Json o = Json::make_obj();
        o.set("line", jint(ln));
        arr.push(std::move(o));
    }
    return arr;
}

bool DebugServer::m_breakpoint_add(const Json& p, Json* r, Err* e) {
    if (!require_script_ready(e)) return false;
    if (!d5_ready()) {
        e->code = "NOT_SUPPORTED";
        e->msg  = std::string("D5 暂停/断点需要引擎的调试钩子（当前引擎: ") +
                  script_language_name(lang_) + "）";
        return false;
    }
    if (!ensure_engine(e)) return false;

    const Json* pl = p.find("line");
    if (!pl || !pl->is_num()) {
        e->code = "BAD_PARAM";
        e->msg  = "缺少 line（断点行号；标签断点本固件不支持）";
        return false;
    }
    const int line = (int)pl->as_int(0);
    if (line <= 0) {
        e->code = "BAD_PARAM";
        e->msg  = "line 必须是正整数";
        return false;
    }
    slot_.engine()->add_breakpoint(line);

    Json out = Json::make_obj();
    out.set("breakpoints", breakpoints_json(slot_.engine()->breakpoints()));
    *r = std::move(out);
    return true;
}

bool DebugServer::m_breakpoint_del(const Json& p, Json* r, Err* e) {
    if (!require_script_ready(e)) return false;
    if (!d5_ready()) {
        e->code = "NOT_SUPPORTED";
        e->msg  = std::string("D5 暂停/断点需要引擎的调试钩子（当前引擎: ") +
                  script_language_name(lang_) + "）";
        return false;
    }
    if (!ensure_engine(e)) return false;

    const Json* pl = p.find("line");
    if (pl && pl->is_num()) {
        const int line = (int)pl->as_int(0);
        if (line <= 0) {
            e->code = "BAD_PARAM";
            e->msg  = "line 必须是正整数";
            return false;
        }
        slot_.engine()->remove_breakpoint(line);
    } else {
        slot_.engine()->clear_breakpoints();     // 无参 = 清空全部（DAP setBreakpoints 全量替换）
    }

    Json out = Json::make_obj();
    out.set("breakpoints", breakpoints_json(slot_.engine()->breakpoints()));
    *r = std::move(out);
    return true;
}

bool DebugServer::m_breakpoint_list(const Json&, Json* r, Err* e) {
    if (!require_script_ready(e)) return false;
    if (!d5_ready()) {
        e->code = "NOT_SUPPORTED";
        e->msg  = std::string("D5 暂停/断点需要引擎的调试钩子（当前引擎: ") +
                  script_language_name(lang_) + "）";
        return false;
    }
    if (!ensure_engine(e)) return false;

    Json out = Json::make_obj();
    out.set("breakpoints", breakpoints_json(slot_.engine()->breakpoints()));
    *r = std::move(out);
    return true;
}

// ---------------------------------------------------------------------------
// D6：文件管理（file.list / file.get / file.del；目录 DEBUG_SCRIPT_DIR，不递归）
// 脚本源码只经 script.compile（带 name）落盘；本组 RPC 让插件能列出/拉回/删除。
// ---------------------------------------------------------------------------
bool DebugServer::file_path_for(const std::string& name, std::string* full, Err* e) const {
    if (!valid_script_file_name(name)) {
        if (e) {
            e->code = "BAD_PARAM";
            e->msg  = "name 只能是纯文件名（字母/数字/._-，不以 . 开头，不含路径分隔符）";
        }
        return false;
    }
    const std::string dir = script_dir_path();
    *full = dir + "/" + name;
    return true;
}

bool DebugServer::save_script_file(const std::string& name, const std::string& src,
                                   std::string* save_err) {
    std::string full;
    if (!file_path_for(name, &full, nullptr)) {
        if (save_err) *save_err = "文件名非法";
        return false;
    }
    const std::string dir = script_dir_path();
    if (!mkdir_p(dir)) {
        if (save_err) *save_err = std::string("mkdir: ") + std::strerror(errno);
        return false;
    }
    std::ofstream f(full, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) {
        if (save_err) *save_err = std::string("open: ") + std::strerror(errno);
        return false;
    }
    f.write(src.data(), static_cast<std::streamsize>(src.size()));
    if (!f.good()) {
        if (save_err) *save_err = "write 失败";
        return false;
    }
    return true;
}

// 文件内容哈希：FNV-1a 64 位，十六进制 16 字符（「控制器文件 ↔ 本地副本」一致性比对用）。
// 插件侧 filesPanelPure.fnv1a64Hex 必须保持同一算法/初值；测试用 '' 与 'a' 的已知向量锁定
// （'' = cbf29ce484222325，'a' = af63dc4c8601ec8c）。
static std::string fnv1a64_hex(const std::string& data) {
    uint64_t h = 14695981039346656037ULL;    // FNV offset basis (64-bit)
    for (unsigned char c : data) {
        h ^= c;
        h *= 1099511628211ULL;               // FNV prime (64-bit)
    }
    char buf[17] = {0};
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf);
}

bool DebugServer::m_file_list(const Json&, Json* r, Err*) {
    const std::string dir = script_dir_path();

    Json files = Json::make_arr();
    DIR* d = ::opendir(dir.c_str());
    if (d) {
        std::vector<std::string> names;
        struct dirent* ent = nullptr;
        while ((ent = ::readdir(d)) != nullptr) {
            const std::string nm = ent->d_name;
            if (!valid_script_file_name(nm)) continue;      // 滤掉 . .. 与隐藏/异常文件
            names.push_back(nm);
        }
        ::closedir(d);
        std::sort(names.begin(), names.end());
        for (const auto& nm : names) {
            struct stat st {};
            const std::string full = dir + "/" + nm;
            if (::stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
            std::string content;                      // 读取内容用于哈希（脚本文件很小；失败则跳过）
            {
                std::ifstream in(full, std::ios::binary);
                if (!in.is_open()) continue;
                content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            if (content.size() != static_cast<size_t>(st.st_size)) continue;   // stat/读竞争：跳过异常条目
            Json o = Json::make_obj();
            o.set("name", jstr(nm));
            o.set("size", jint(static_cast<long long>(st.st_size)));
            o.set("hash", jstr(fnv1a64_hex(content))); // v0.8.4：插件据此比对本地副本
            files.push(std::move(o));
        }
    }
    // 目录不存在 = 尚无落盘脚本：返回空清单（不算错误，不伪装）

    Json out = Json::make_obj();
    out.set("dir",   jstr(dir));
    out.set("files", std::move(files));
    *r = std::move(out);
    return true;
}

bool DebugServer::m_file_get(const Json& p, Json* r, Err* e) {
    const Json* pn = p.find("name");
    if (!pn || !pn->is_str() || pn->as_str().empty()) {
        e->code = "BAD_PARAM";
        e->msg  = "缺少 name（文件名）";
        return false;
    }
    std::string full;
    if (!file_path_for(pn->as_str(), &full, e)) return false;

    std::ifstream f(full, std::ios::binary);
    if (!f.is_open()) {
        e->code = "NOT_FOUND";
        e->msg  = "文件不存在: " + pn->as_str();
        return false;
    }
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (src.size() > kMaxLineBytes) {
        e->code = "BAD_PARAM";
        e->msg  = "文件过大（>4MB，超出单行报文上限）";
        return false;
    }

    Json out = Json::make_obj();
    out.set("name", jstr(pn->as_str()));
    out.set("src",  jstr(src));
    *r = std::move(out);
    return true;
}

bool DebugServer::m_file_del(const Json& p, Json* r, Err* e) {
    const Json* pn = p.find("name");
    if (!pn || !pn->is_str() || pn->as_str().empty()) {
        e->code = "BAD_PARAM";
        e->msg  = "缺少 name（文件名）";
        return false;
    }
    std::string full;
    if (!file_path_for(pn->as_str(), &full, e)) return false;
    if (::unlink(full.c_str()) != 0) {
        e->code = "NOT_FOUND";
        e->msg  = std::string("删除失败: ") + std::strerror(errno);
        return false;
    }
    Json out = Json::make_obj();
    out.set("deleted", Json::make_bool(true));
    *r = std::move(out);
    return true;
}

// ---------------------------------------------------------------------------
// D8：主文件（开机运行）—— 清单 `.boot`（用户拍板 2026-09-26，方案 A）
//   目录可存多个脚本，开机只跑清单指定的一个；其它文件由主文件 INCLUDE 调用。
//   修改下次启动生效（插件可配合 D7 sys.restart 立即应用）。
// ---------------------------------------------------------------------------
bool DebugServer::m_boot_get(const Json&, Json* r, Err*) {
    const std::string dir = script_dir_path();
    const BootInfo bi = read_boot_info(dir);
    Json out = Json::make_obj();
    out.set("dir",   jstr(dir));
    out.set("name",  jstr(bi.name));
    out.set("valid", Json::make_bool(bi.valid));
    if (!bi.valid && !bi.reason.empty()) out.set("reason", jstr(bi.reason));
    *r = std::move(out);
    return true;
}

bool DebugServer::m_boot_set(const Json& p, Json* r, Err* e) {
    const Json* pn = p.find("name");
    if (!pn || !pn->is_str() || pn->as_str().empty()) {
        e->code = "BAD_PARAM";
        e->msg  = "缺少 name（要设为主文件的脚本文件名）";
        return false;
    }
    const std::string name = pn->as_str();
    const std::string dir  = script_dir_path();
    if (!valid_script_file_name(name)) {
        e->code = "BAD_PARAM";
        e->msg  = "name 只能是纯文件名（字母/数字/._-，不以 . 开头，不含路径分隔符）";
        return false;
    }
    const std::string lang = script_lang_of_name(name);
    if (lang.empty()) {
        e->code = "BAD_PARAM";
        e->msg  = "主文件必须是 .bas 或 .lua：" + name;
        return false;
    }
    struct stat st {};
    if (::stat((dir + "/" + name).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        e->code = "NOT_FOUND";
        e->msg  = "控制器上没有该文件: " + name + "（先「下载」保存，再设为主文件）";
        return false;
    }
    // 语言一致性：以**目录绑定**为准（主文件下次启动生效；不看当前调试口引擎状态）
    const LangBind bind = scan_dir_bind();
    if (bind == LangBind::MIXED) {
        e->code = "ENGINE_MISMATCH";
        e->msg  = "脚本目录同时存在 .bas 与 .lua，无法确定语言：请先删除其中一种，再设主文件";
        return false;
    }
    if ((bind == LangBind::BASIC || bind == LangBind::LUA) &&
        lang != script_language_name(bind == LangBind::LUA ? ScriptLanguage::LUA
                                                           : ScriptLanguage::BASIC)) {
        e->code = "ENGINE_MISMATCH";
        e->msg  = std::string("控制器脚本目录中为 ") +
                  script_language_name(bind == LangBind::LUA ? ScriptLanguage::LUA
                                                             : ScriptLanguage::BASIC) +
                  " 脚本，不能把 " + lang + " 文件设为主文件（同一时刻只加载一种语言）";
        return false;
    }
    std::string werr;
    if (!write_boot_name(dir, name, &werr)) {
        e->code = "RUNTIME_ERROR";
        e->msg  = "写主文件清单失败：" + werr;
        return false;
    }
    Json out = Json::make_obj();
    out.set("name",  jstr(name));
    out.set("valid", Json::make_bool(true));
    *r = std::move(out);
    return true;
}

bool DebugServer::m_boot_clear(const Json&, Json* r, Err* e) {
    std::string cerr;
    if (!clear_boot_name(script_dir_path(), &cerr)) {
        e->code = "RUNTIME_ERROR";
        e->msg  = "清除主文件清单失败：" + cerr;
        return false;
    }
    Json out = Json::make_obj();
    out.set("name", jstr(""));
    *r = std::move(out);
    return true;
}

// ---------------------------------------------------------------------------
// D9：端口数量上限（运行期 + 持久化 `.portmax`）
// ---------------------------------------------------------------------------
bool DebugServer::m_port_max_get(const Json&, Json* r, Err*) {
    Json out = Json::make_obj();
    out.set("max",     jint(PortManager::global_max_ports()));
    out.set("slots",   jint(PortManager::kMaxSlots));
    out.set("default", jint(PortManager::kDefaultMaxPorts));
    out.set("dir",     jstr(script_dir_path()));
    Json used = Json::make_arr();
    for (int p : PortManager::open_ports()) used.push(jint(p));
    out.set("used", std::move(used));
    *r = std::move(out);
    return true;
}

bool DebugServer::m_port_max_set(const Json& p, Json* r, Err* e) {
    const Json* pm = p.find("max");
    if (!pm || !pm->is_num()) {
        e->code = "BAD_PARAM";
        e->msg  = "缺少 max（端口数量上限，1.." + std::to_string(PortManager::kMaxSlots) + " 的整数）";
        return false;
    }
    const double dv = pm->as_num();
    const int    n  = (int)dv;
    if ((double)n != dv) {
        e->code = "BAD_PARAM";
        e->msg  = "max 必须是整数";
        return false;
    }
    const int prev = PortManager::global_max_ports();
    std::string perr;
    if (!PortManager::set_global_max_ports(n, &perr)) {
        // 范围错误 → BAD_PARAM；端口占用导致不能收缩 → BUSY（插件只按 code 分支）
        e->code = (n < 1 || n > PortManager::kMaxSlots) ? "BAD_PARAM" : "BUSY";
        e->msg  = perr;
        return false;
    }
    std::string werr;
    if (!write_port_max(script_dir_path(), n, &werr)) {
        PortManager::set_global_max_ports(prev, nullptr);   // 持久化失败 → 回滚运行期值
        e->code = "RUNTIME_ERROR";
        e->msg  = "端口上限未生效（写入 .portmax 失败，已回滚）：" + werr;
        return false;
    }
    Json out = Json::make_obj();
    out.set("max",   jint(n));
    out.set("prev",  jint(prev));
    out.set("saved", Json::make_bool(true));
    out.set("file",  jstr(script_dir_path() + "/.portmax"));
    *r = std::move(out);
    return true;
}

// ---------------------------------------------------------------------------
// D1：变量 / 轴快照 / 终端命令
// ---------------------------------------------------------------------------
bool DebugServer::m_var_list(const Json&, Json* r, Err* e) {
    if (!require_script_ready(e)) return false;
    if (!ensure_engine(e))        return false;

    Json arr = Json::make_arr();
    for (const auto& s : slot_.engine()->list_vars()) arr.push(jstr(s));
    *r = std::move(arr);
    return true;
}

bool DebugServer::m_var_get(const Json& p, Json* r, Err* e) {
    if (!require_script_ready(e)) return false;
    if (!ensure_engine(e))        return false;

    const Json* pn = p.find("name");
    if (!pn || !pn->is_str() || pn->as_str().empty()) {
        e->code = "BAD_PARAM";
        e->msg  = "缺少 name（变量名）";
        return false;
    }
    const std::string name = pn->as_str();

    std::string rhs;
    if (!find_var_rhs(name, &rhs)) {                 // 以 list_vars() 为准判断存在性
        e->code = "BAD_PARAM";
        e->msg  = "变量不存在: " + name;
        return false;
    }

    const Value v = slot_.engine()->get_var(name);
    std::string type = "nil";
    if (v.type == Value::NUM)      type = "num";
    else if (v.type == Value::STR) type = "str";

    Json out = Json::make_obj();
    out.set("name",  jstr(name));
    out.set("type",  jstr(type));
    out.set("value", jval(v));
    *r = std::move(out);
    return true;
}

bool DebugServer::m_var_set(const Json& p, Json* r, Err* e) {
    if (!require_script_ready(e)) return false;
    if (!ensure_engine(e))        return false;

    const Json* pn = p.find("name");
    if (!pn || !pn->is_str() || pn->as_str().empty()) {
        e->code = "BAD_PARAM";
        e->msg  = "缺少 name（变量名）";
        return false;
    }
    const std::string name = pn->as_str();

    std::string rhs;
    if (!find_var_rhs(name, &rhs)) {                 // 只允许写已存在的变量（防拼错静默新建）
        e->code = "BAD_PARAM";
        e->msg  = "变量不存在: " + name;
        return false;
    }

    const Json* pv = p.find("v");
    if (!pv || pv->is_nil()) {
        e->code = "BAD_PARAM";
        e->msg  = "缺少 v（变量值）";
        return false;
    }
    if (pv->is_num())      slot_.engine()->set_var(name, Value::number(pv->as_num()));
    else if (pv->is_str()) slot_.engine()->set_var(name, Value::text(pv->as_str()));
    else {
        e->code = "BAD_PARAM";
        e->msg  = "v 只支持数字或字符串";
        return false;
    }

    *r = Json::make_obj();
    return true;
}

bool DebugServer::m_axis_snapshot(const Json& p, Json* r, Err* e) {
    int a = 0;
    if (const Json* pa = p.find("axis"); pa && pa->is_num()) a = (int)pa->as_int(0);
    if (!sh_.axis_ok(a)) {
        e->code = "BAD_PARAM";
        e->msg  = "axis out of range: " + std::to_string(a) +
                  "（本机轴数 " + std::to_string(sh_.axis_count) + "）";
        return false;
    }
    *r = axis_json(a);
    return true;
}

bool DebugServer::m_cmd(const Json& p, Json* r, Err* e) {
    const Json* pl = p.find("line");
    if (!pl || !pl->is_str() ||
        pl->as_str().find_first_not_of(" \t\r") == std::string::npos) {
        e->code = "BAD_PARAM";
        e->msg  = "命令为空";
        return false;
    }

    std::string         name;
    std::vector<double> nargs;
    if (!parse_cmd_line(pl->as_str(), &name, &nargs)) {
        e->code = "BAD_PARAM";
        e->msg  = "命令为空";
        return false;
    }

    std::vector<Value> args;
    args.reserve(nargs.size());
    for (double d : nargs) args.push_back(Value::number(d));

    // 与脚本同一套宿主（MotionHost）：命令名/错误语义完全一致，不另开一条通往后端的路。
    Value       ret;
    std::string err;
    const int rc = host_->call(name, args, &ret, &err);
    if (rc != 0) {
        e->code = "RUNTIME_ERROR";
        e->msg  = (rc == 1) ? ("未知命令: " + name)
                            : (err.empty() ? "命令执行失败" : err);
        return false;
    }

    Json out = Json::make_arr();
    if (ret.type != Value::NIL) out.push(jstr(ret.to_text()));   // 查询类命令（POS/MPOS...）的读值

    Json o = Json::make_obj();
    o.set("ret", jint(0));                        // 0 = 成功（失败走 err，见 16 §7.1）
    o.set("out", std::move(out));
    *r = std::move(o);
    return true;
}

// ---------------------------------------------------------------------------
// D3：订阅 / 退订
// ---------------------------------------------------------------------------
namespace {

// 解析 topics（数组或单字符串），统一小写
std::vector<std::string> parse_topics(const Json& p) {
    std::vector<std::string> out;
    const Json* pt = p.find("topics");
    if (!pt) return out;
    if (pt->is_arr()) {
        for (const Json& t : pt->arr)
            if (t.is_str()) out.push_back(trim_lower(t.as_str()));
    } else if (pt->is_str()) {
        out.push_back(trim_lower(pt->as_str()));
    }
    return out;
}

} // namespace

bool DebugServer::m_subscribe(const Json& p, Json* r, Err* e) {
    const std::vector<std::string> topics = parse_topics(p);
    if (topics.empty()) {
        e->code = "BAD_PARAM";
        e->msg  = "topics 为空（可选 axis / bus / mb / log / conn）";
        return false;
    }

    int hz = 20;
    if (const Json* ph = p.find("hz"); ph && ph->is_num()) hz = (int)ph->as_num();

    // 先整体校验再落地——避免「部分订阅已生效却回了错误」的半吊状态
    for (const auto& t : topics) {
        if (t != "axis" && t != "bus" && t != "mb" && t != "log" && t != "conn") {
            e->code = "BAD_PARAM";
            e->msg  = "未知订阅主题: " + t + "（可选 axis / bus / mb / log / conn）";
            return false;
        }
    }
    for (const auto& t : topics) {
        if (t == "axis")     { sub_axis_.on = true; sub_axis_.hz = hz; sub_axis_.next_ms = 0; }
        else if (t == "bus") { sub_bus_.on  = true; sub_bus_.hz  = hz; sub_bus_.next_ms  = 0; }
        else if (t == "mb")  { sub_mb_.on   = true; sub_mb_.hz   = hz; sub_mb_.next_ms   = 0; }
        else if (t == "conn") { sub_conn_.on = true; sub_conn_.hz = hz; sub_conn_.next_ms = 0; }
        else if (t == "log") { sub_log_ = true; }
    }

    *r = sub_result_json(sub_axis_.on, sub_bus_.on, sub_mb_.on, sub_log_, sub_conn_.on);
    return true;
}

bool DebugServer::m_unsubscribe(const Json& p, Json* r, Err* e) {
    const std::vector<std::string> topics = parse_topics(p);

    for (const auto& t : topics) {
        if (t != "axis" && t != "bus" && t != "mb" && t != "log" && t != "conn") {
            e->code = "BAD_PARAM";
            e->msg  = "未知订阅主题: " + t + "（可选 axis / bus / mb / log / conn）";
            return false;
        }
    }
    if (topics.empty()) {                     // 无参 = 全部退订
        sub_axis_ = Sub{};
        sub_bus_  = Sub{};
        sub_mb_   = Sub{};
        sub_conn_ = Sub{};
        sub_log_  = false;
    } else {
        for (const auto& t : topics) {
            if (t == "axis")      sub_axis_ = Sub{};
            else if (t == "bus")  sub_bus_  = Sub{};
            else if (t == "mb")   sub_mb_   = Sub{};
            else if (t == "conn") sub_conn_ = Sub{};
            else if (t == "log")  sub_log_  = false;
        }
    }

    *r = sub_result_json(sub_axis_.on, sub_bus_.on, sub_mb_.on, sub_log_, sub_conn_.on);
    return true;
}

// 每轮 poll 调用一次：脚本状态跳变 + PRINT 输出 + 订阅主题按 hz 推送
void DebugServer::push_events(int fd) {
    emit_script_event_if_changed(fd);

    // 打印输出（PRINT / 命令回显）：随有随推，未订阅也推（脚本调试必需，见 16 §4.2）
    {
        std::vector<std::string> batch;
        {
            std::lock_guard<std::mutex> lk(log_mtx_);
            batch.swap(logs_);
        }
        for (const auto& line : batch) {
            Json e = Json::make_obj();
            e.set("e",   jstr("log"));
            e.set("t",   jint(epoch_ms()));
            e.set("s",   jstr(line));
            e.set("lvl", jstr("info"));
            send_line(fd, e);
        }
    }

    // 到期判定：到点后把下次到期时间推进一个周期
    const auto due = [](Sub& s) {
        if (!s.on) return false;
        const long long now = epoch_ms();
        if (now < s.next_ms) return false;
        s.next_ms = now + period_ms(s.hz);
        return true;
    };

    if (due(sub_axis_)) {
        const int n = sh_.axis_count < kAxisMax ? sh_.axis_count : kAxisMax;
        for (int a = 0; a < n; ++a) {
            Json ev = Json::make_obj();
            ev.set("e",    jstr("axis"));
            ev.set("t",    jint(epoch_ms()));
            ev.set("axis", jint(a));                 // 多轴：topic 只有 'axis'，轴号在事件里
            for (const auto& kv : axis_json(a).obj) ev.set(kv.first, kv.second);
            send_line(fd, ev);
        }
    }

    if (due(sub_bus_)) {
        BusInfo bi;
        {
            std::lock_guard<std::mutex> lk(sh_.mtx);
            bi = sh_.bus;
        }
        Json nodes = Json::make_arr();
        const int n = bi.node_count < kBusNodeMax ? bi.node_count : kBusNodeMax;
        for (int i = 0; i < n; ++i) {
            const BusNodeInfo& nd = bi.node[i];
            Json o = Json::make_obj();
            o.set("index",      jint(i));
            o.set("axis_count", jint(nd.axis_count));
            o.set("status",     jint(nd.status));
            o.set("io",         jint(nd.io_base));
            o.set("aio",        jint(nd.aio_base));
            o.set("in_count",   jint(nd.in_count));
            o.set("out_count",  jint(nd.out_count));
            nodes.push(std::move(o));
        }
        Json ev = Json::make_obj();
        ev.set("e",          jstr("bus"));
        ev.set("t",          jint(epoch_ms()));
        ev.set("node_count", jint(bi.node_count));
        ev.set("nodes",      std::move(nodes));
        send_line(fd, ev);
    }

    if (due(sub_mb_)) {
        const int start = mb_cursor_ % kModbusRegCount;
        const int n     = std::min(kMbSegment, kModbusRegCount - start);
        Json regs = Json::make_arr();
        {
            std::lock_guard<std::recursive_mutex> lk(sh_.mb_mtx);
            for (int i = 0; i < n; ++i) regs.push(jint(sh_.mb_regs[start + i]));
        }
        mb_cursor_ = (start + n) % kModbusRegCount;

        Json ev = Json::make_obj();
        ev.set("e",     jstr("mb"));
        ev.set("t",     jint(epoch_ms()));
        ev.set("start", jint(start));
        ev.set("regs",  std::move(regs));
        {   // v0.8.6：脚本访问过的寄存器位图（4×16 位十六进制；插件「已用寄存器」监视用）
            Json used = Json::make_arr();
            char hexbuf[17];
            for (int w = 0; w < 4; ++w) {
                std::snprintf(hexbuf, sizeof(hexbuf), "%016llx",
                              (unsigned long long)sh_.mb_used[w].load(std::memory_order_relaxed));
                used.push(jstr(hexbuf));
            }
            ev.set("used", std::move(used));
        }
        send_line(fd, ev);
    }

    // D10「通讯状态」：端口连接快照（进程级，任何实例）+ EtherCAT 主站/从站明细
    if (due(sub_conn_)) {
        Json conns = Json::make_arr();
        for (const auto& s : PortManager::snapshot()) {
            Json o = Json::make_obj();
            o.set("port", jint(s.port));
            o.set("kind", jstr(s.kind == PortManager::TCP_SERVER ? "TCP_SERVER" : "TCP_CLIENT"));
            o.set("conn", Json::make_bool(s.conn));
            o.set("listen",   jint(s.lport));      // server：监听端口
            o.set("peer_port", jint(s.rport));     // server：客户端端口；client：远端端口
            o.set("target", jstr(s.target));
            o.set("peer",   jstr(s.peer));
            if (!s.tag.empty())  o.set("tag",  jstr(s.tag));    // PORT_INFO 用途
            if (!s.role.empty()) o.set("role", jstr(s.role));   // PORT_INFO 主/从
            conns.push(std::move(o));
        }
        BusInfo bi;
        {
            std::lock_guard<std::mutex> lk(sh_.mtx);
            bi = sh_.bus;
        }
        Json bus = Json::make_obj();
        bus.set("link_up",           jint(bi.link_up));
        bus.set("slaves_responding", jint(bi.slaves_responding));
        bus.set("master_al",         jint(bi.master_al));
        bus.set("slave_al",          jint(bi.slave_al));
        bus.set("slave_online",      jint(bi.slave_online));
        bus.set("slave_op",          jint(bi.slave_op));
        bus.set("node_count",        jint(bi.node_count));
        Json slaves = Json::make_arr();
        for (int i = 0; i < bi.node_count && i < kBusNodeMax; ++i) {
            const BusNodeInfo& n = bi.node[i];
            Json o = Json::make_obj();
            o.set("index",   jint(i));
            o.set("axis",    jint(n.axis));
            o.set("online",  jint(n.online));
            o.set("al_state", jint(n.al_state));
            o.set("vid",     jint((long long)n.vendor_id));
            o.set("pid",     jint((long long)n.product_code));
            o.set("rev",     jint((long long)n.revision));
            if (n.name[0] != '\0') {
                o.set("name", jstr(std::string(n.name, ::strnlen(n.name, sizeof(n.name)))));
            }
            slaves.push(std::move(o));
        }
        bus.set("slaves", std::move(slaves));
        Json ev = Json::make_obj();
        ev.set("e",   jstr("conn"));
        ev.set("t",   jint(epoch_ms()));
        ev.set("conns", std::move(conns));
        ev.set("bus",   std::move(bus));
        send_line(fd, ev);
    }
}

// 脚本状态跳变（含 steps/error_line 变化）时推 script 事件；无跳变则不推。
// 事件同时带 error_line / line，免客户端二次查询（见 16 §7.1）。
// D5：运行中且引擎挂起 → status=PAUSED、line=当前执行行（16 §4.2 / Mock 行为对齐）。
void DebugServer::emit_script_event_if_changed(int fd) {
    RunSnap s = run_snap();
    bool paused_now = false;
    int  pause_line = 0;
    if (s.running && slot_.loaded()) {
        paused_now = slot_.engine()->paused();
        pause_line = slot_.engine()->current_line();
        // 运行中取引擎**实时步数**：否则 steps 恒 0，会让「暂停→恢复→再暂停」两次 PAUSED
        // 去重键完全相同而被吞事件（DAP 收不到第二次 stopped）。
        s.steps    = slot_.engine()->steps();
    }
    const std::string status = paused_now ? "PAUSED" : (s.running ? "READY" : s.status);
    const int         line   = paused_now ? pause_line : s.error_line;
    const std::string key = std::string(s.running ? "RUN:" : "STOP:") + status + "|" +
                            std::to_string(s.steps) + "|" + std::to_string(line);
    if (key == last_script_key_) return;
    last_script_key_ = key;

    Json e = Json::make_obj();
    e.set("e",          jstr("script"));
    e.set("t",          jint(epoch_ms()));
    e.set("status",     jstr(status));
    e.set("steps",      jint((long long)s.steps));
    e.set("error_line", jint(s.error_line));
    e.set("line",       jint(line));
    if (!s.error.empty()) e.set("error", jstr(s.error));
    send_line(fd, e);
}

// ---------------------------------------------------------------------------
// 内部工具
// ---------------------------------------------------------------------------
// 语言绑定扫描（用户拍板 2026-09-25）：只认 DEBUG_SCRIPT_DIR 下的普通文件，
// 文件名需过 valid_file_name（滤 . .. 与隐藏文件）；扩展名判定与 engine_rule.h 一致（大小写敏感）。
DebugServer::LangBind DebugServer::scan_lang_bind() const {
    if (!allow_script_) {                          // 开机脚本占用引擎：语言即其实际语言（main 传入）
        return lang_ == ScriptLanguage::LUA ? LangBind::LUA : LangBind::BASIC;
    }
    return scan_dir_bind();
}

// 只看 DEBUG_SCRIPT_DIR 的绑定（D8「设主文件」用）：
// 主文件是**下次启动**才生效的，其语言必须按目录内容判定，不能沿用调试口当前引擎状态
// （v0.8.1 修复：开机脚本占用引擎时曾误用 SCRIPT_ENGINE=basic，导致目录里只有 .lua 也报 basic）。
DebugServer::LangBind DebugServer::scan_dir_bind() const {
    switch (scan_script_dir_lang(script_dir_path())) {
        case ScriptDirLang::BASIC: return LangBind::BASIC;
        case ScriptDirLang::LUA:   return LangBind::LUA;
        case ScriptDirLang::MIXED: return LangBind::MIXED;
        default:                   return LangBind::AUTO;   // 目录不存在/为空/无脚本文件
    }
}

// DEBUG_SCRIPT_DIR 规范化（去尾部 '/'；空 → 默认目录）。D6 与主文件清单共用。
std::string DebugServer::script_dir_path() const {
    std::string dir = cfg_.script_dir.empty() ? "/userdata/kine-x/scripts" : cfg_.script_dir;
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    return dir;
}

std::string DebugServer::language_display() const {
    if (!allow_script_) return script_language_name(lang_);
    switch (scan_lang_bind()) {
        case LangBind::BASIC: return "basic";
        case LangBind::LUA:   return "lua";
        case LangBind::MIXED: return "mixed";
        default:              return "auto";
    }
}

bool DebugServer::require_script_ready(Err* e) const {
    if (allow_script_) return true;
    e->code = "BUSY";
    e->msg  = "自动脚本已占用脚本引擎（SCRIPT_ENABLE=1），调试口不能编译/运行脚本或读写变量";
    return false;
}

bool DebugServer::ensure_engine(Err* e) {
    if (slot_.loaded()) return true;
    std::string err;
    if (!slot_.load(lang_, host_.get(), &err)) {     // 槽已占用 / 未知语言
        e->code = "BUSY";
        e->msg  = err.empty() ? "脚本引擎装载失败" : err;
        return false;
    }
    // D5（v0.8.0）：调试通道装载的引擎打开行级 hook，支持断点/暂停/单步；
    // 开机生产脚本走 script_loop 的独立引擎，不做这一步（零额外开销）。
    slot_.engine()->enable_line_hooks(true);
    return true;
}

bool DebugServer::engine_running() const {
    std::lock_guard<std::mutex> lk(run_mtx_);
    return snap_.running;
}

DebugServer::RunSnap DebugServer::run_snap() const {
    std::lock_guard<std::mutex> lk(run_mtx_);
    return snap_;
}

// D4：被换出的旧引擎，待其 worker 线程结束后释放（compile/run 开头已 join）。
void DebugServer::release_retired() {
    if (run_th_.joinable()) return;   // worker 还在跑（可能就在旧实例上）：不能释放
    retired_.reset();
}

bool DebugServer::find_var_rhs(const std::string& name, std::string* rhs) const {
    if (!slot_.loaded()) return false;
    const std::string want = trim_lower(name);
    for (const auto& line : slot_.engine()->list_vars()) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (trim_lower(line.substr(0, eq)) != want) continue;
        if (rhs) {
            size_t b = eq + 1, en = line.size();
            while (b < en && std::isspace((unsigned char)line[b])) ++b;
            while (en > b && std::isspace((unsigned char)line[en - 1])) --en;
            *rhs = line.substr(b, en - b);
        }
        return true;
    }
    return false;
}

Json DebugServer::axis_json(int a) const {
    const AxisStatus st = sh_.snapshot(a);
    Json o = Json::make_obj();
    o.set("bus_ok",      jint(st.bus_ok));
    o.set("mpos",        jnum(st.mpos));
    o.set("dpos",        jnum(st.dpos));
    o.set("inc_per_mm",  jnum(st.inc_per_mm));
    o.set("idle",        jint(st.idle));
    o.set("enabled",     jint(st.enabled));
    o.set("alarm",       jint(st.alarm));
    o.set("axis_status", jint((long long)st.axis_status));
    o.set("err_code",    jint(st.err_code));
    return o;
}

} // namespace kx
