// debug_server.h —— 调试通道服务端（TCP JSON-Lines，D1~D5）
//
// 规范见 docs/planA/13（调试通道与上位机通讯）、docs/planA/16（报文样例）。
//
// 角色（关键，别搞反）：
//   * **控制器板端是服务端**：随 kine-x.service 开机自启，监听 DEBUG_PORT（默认 5000），
//     等**局域网内 PC** 上的 VSCodium 插件来接；
//   * 插件（VSCodium 扩展）**只运行在 PC 上**，板端不装插件、不跑 VSCodium；
//   * 因此默认绑**业务网**（DEBUG_BIND，默认 0.0.0.0 -> PC 可直连），**不是** 127.0.0.1。
//
// 与本工程其它模块的关系：
//   * 设备命令走 `MotionHost`（脚本层同一套宿主），不另开一条通往后端的路；
//   * 脚本编译/运行走 `ScriptEngineSlot`（同一时刻只加载一种脚本语言，见 engine_rule.h），
//     若自动脚本（SCRIPT_ENABLE=1）已占用，则本服务**明确拒绝** script.*/var.*（不伪装可用）；
//   * 轴/总线/寄存器快照取自 `kx::Shared`（只读，不参与 RT 线程）。
//
// 报文：一行一个 JSON 对象，'\n' 结束（UTF-8）。回包 {id, ok:true, r} / {id, ok:false, err:{code,msg,line}}；
//       事件 {e:"script"|"log"|"axis"|"bus"|"mb", t:...}（无 id）。
//
// 线程模型：本类内部一条线程（accept 循环 + 单客户端会话），会话内用 poll(timeout)
//   交错处理「收请求 / 推事件」；`script.run` 另起 worker 线程执行，保证会话线程不被长脚本卡死。
//   单客户端语义：同一时刻只服务一个连接（与文档一致），断开后允许下一个接入。
//
// 不碰 ecrt / RT 线程，可脱机用回环 socket 单测（tools/debug_server_test.cpp）。
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/config.h"      // kx::DebugCfg
#include "common/state.h"       // kx::Shared
#include "script/engine_rule.h" // kx::ScriptEngineSlot / ScriptLanguage
#include "script/json_lite.h"   // kx::Json
#include "script/motion_host.h" // kx::MotionHost

namespace kx {

class ModbusServer;
class ModbusMaster;

class DebugServer {
public:
    // sh           共享状态（轴/总线/寄存器快照来源）
    // cfg          DEBUG_* 配置
    // allow_script 是否允许本服务**独占**脚本引擎（自动脚本 SCRIPT_ENABLE=1 时为 false）
    // engine_hint  脚本语言名（"basic"/"lua"）：仅在 allow_script=false（自动脚本占用）时
    //              作为**确定语言**；allow_script=true 时语言由 DEBUG_SCRIPT_DIR 内现有
    //              脚本推导（空目录=auto 两种都收，见 scan_lang_bind），hint 仅作兜底初值
    // restart_flag 可选：`sys.restart` 命令执行失败时置位并 SIGTERM，main 据此以失败码
    //              退出，让 systemd `Restart=on-failure` 兜底重启（默认 nullptr = 不用）
    DebugServer(Shared& sh, const DebugCfg& cfg, bool allow_script, const std::string& engine_hint,
                std::atomic<bool>* restart_flag = nullptr);
    ~DebugServer();

    // 固件 Modbus 引擎（D12：mbreg.get/set；planA/20）；未接线时 mbreg.* 明确报不支持
    void set_modbus(ModbusServer* mb, const std::string& cfg_path) {
        mb_ = mb;
        mb_cfg_path_ = cfg_path;
    }
    // 固件 Modbus 主站（D13：mbdev.get/set/status；planA/21）
    void set_master(ModbusMaster* md, const std::string& cfg_path) {
        master_ = md;
        master_cfg_path_ = cfg_path;
    }

    DebugServer(const DebugServer&) = delete;
    DebugServer& operator=(const DebugServer&) = delete;

    // 建监听套接字（不做线程）。失败返回 false 并写 err；port=0 时由内核分配。
    bool listen_now(std::string* err);
    // 起服务线程（需先 listen_now 成功）
    void start();
    // 停止并回收线程
    void stop();

    bool listening() const { return listen_fd_ >= 0; }
    int  port() const { return bound_port_; }   // 实际监听端口（诊断/测试用）

private:
    // 回包错误（对应协议 err 对象）
    struct Err {
        std::string code;                 // BAD_REQUEST / UNKNOWN_METHOD / BAD_PARAM / ...
        std::string msg;
        long long   line = -1;            // >=0 时进回包（行号类错误）
    };

    // 订阅项：on=是否订阅；hz=推送频率；next_ms=下次到期时间
    struct Sub {
        bool      on      = false;
        int       hz      = 20;
        long long next_ms = 0;
    };

    // 运行快照：worker 线程发布，会话线程只读（避免跨线程直接摸引擎内部）
    struct RunSnap {
        std::string        status     = "READY";
        unsigned long long steps      = 0;
        int                error_line = 0;
        std::string        error;
        bool               running    = false;
    };

    // ---- 线程与连接 ----
    void accept_loop();
    void serve(int fd);
    void dispatch(int fd, const std::string& line);
    void push_events(int fd);

    // ---- 发送 ----
    void send_line(int fd, const Json& j);
    void reply_ok(int fd, long long id, Json r);
    void reply_err(int fd, long long id, const Err& e);

    // ---- RPC（返回 false 表示失败，e 已填）----
    bool rpc(const std::string& m, const Json& p, Json* r, Err* e);

    bool  m_sys_info(const Json& p, Json* r, Err* e);
    bool  m_sys_restart(const Json& p, Json* r, Err* e);   // D7：重启控制器（异步执行配置命令）
    bool m_auth(const Json& p, Json* r, Err* e);
    bool m_script_compile(const Json& p, Json* r, Err* e);
    bool m_script_run(const Json& p, Json* r, Err* e);
    bool m_script_stop(const Json& p, Json* r, Err* e);
    bool m_script_status(const Json& p, Json* r, Err* e);
    bool m_script_pause(const Json& p, Json* r, Err* e);   // D5
    bool m_script_resume(const Json& p, Json* r, Err* e);  // D5
    bool m_script_step(const Json& p, Json* r, Err* e);    // D5
    bool m_breakpoint_add(const Json& p, Json* r, Err* e); // D5
    bool m_breakpoint_del(const Json& p, Json* r, Err* e); // D5
    bool m_breakpoint_list(const Json& p, Json* r, Err* e);// D5
    bool m_var_list(const Json& p, Json* r, Err* e);
    bool m_var_get(const Json& p, Json* r, Err* e);
    bool m_var_set(const Json& p, Json* r, Err* e);
    bool  m_file_list(const Json& p, Json* r, Err* e);      // D6 文件管理
    bool  m_file_get(const Json& p, Json* r, Err* e);       // D6 文件管理
    bool  m_file_del(const Json& p, Json* r, Err* e);       // D6 文件管理
    bool  m_boot_get(const Json& p, Json* r, Err* e);       // D8 主文件（开机运行）查询
    bool  m_boot_set(const Json& p, Json* r, Err* e);       // D8 主文件设置
    bool  m_boot_clear(const Json& p, Json* r, Err* e);     // D8 主文件取消
    bool  m_port_max_get(const Json& p, Json* r, Err* e);   // D9 端口数量上限（运行期 + 持久化 .portmax）
    bool  m_port_max_set(const Json& p, Json* r, Err* e);   // D9 端口数量上限设置
    bool  m_mbmap_get(const Json& p, Json* r, Err* e);      // D11 Modbus 寄存器表（脚本目录 .mbmap）
    bool  m_mbmap_set(const Json& p, Json* r, Err* e);      // D11 Modbus 寄存器表写入
    bool  m_mbreg_get(const Json& p, Json* r, Err* e);      // D12 Modbus 组态（产品化；固件引擎）
    bool  m_mbreg_set(const Json& p, Json* r, Err* e);      // D12 Modbus 组态写入 + 热加载
    bool  m_mbdev_get(const Json& p, Json* r, Err* e);      // D13 Modbus 主站组态（planA/21）
    bool  m_mbdev_set(const Json& p, Json* r, Err* e);      // D13 Modbus 主站组态写入 + 热加载
    bool  m_mbdev_status(const Json& p, Json* r, Err* e);   // D13 主站设备在线状态
    bool m_axis_snapshot(const Json& p, Json* r, Err* e);
    bool m_cmd(const Json& p, Json* r, Err* e);
    bool m_subscribe(const Json& p, Json* r, Err* e);
    bool m_unsubscribe(const Json& p, Json* r, Err* e);

    // ---- 语言绑定（用户拍板 2026-09-25：语言由 DEBUG_SCRIPT_DIR 内现有脚本决定）----
    //   AUTO  = 目录为空（无 .bas/.lua）：两种语言都可编译，落盘后即绑定；
    //   BASIC = 只有 .bas；LUA = 只有 .lua；MIXED = 两种都有（拒绝新编译，提示先删除一种）。
    enum class LangBind { AUTO, BASIC, LUA, MIXED };
    LangBind scan_lang_bind() const;        // 调试口语言：脚本占用引擎→运行中语言；否则目录扫描
    LangBind scan_dir_bind() const;         // **只看目录**的绑定（D8 设主文件用，不受引擎占用影响）
    std::string language_display() const;   // sys.info.engine："basic"|"lua"|"auto"|"mixed"
    std::string script_dir_path() const;    // DEBUG_SCRIPT_DIR 规范化路径（D6/D8 共用）

    // ---- 内部工具 ----
    bool  require_script_ready(Err* e) const;              // allow_script_ 判定
    bool  ensure_engine(Err* e);                           // 惰性装载唯一引擎
    bool  engine_running() const;                          // 运行中？（读 RunSnap）
    RunSnap run_snap() const;
    bool  find_var_rhs(const std::string& name, std::string* rhs) const;  // 在 list_vars() 里找 "NAME = v"
    Json  axis_json(int a) const;                          // 单轴快照 -> JSON
    void  emit_script_event_if_changed(int fd);            // 状态跳变时推 script 事件
    void  reset_session();                                 // 新客户端接入时清会话状态
    void  release_retired();                               // worker 结束后释放被换出的旧引擎（D4）
    bool  d5_ready() const { return lang_ == ScriptLanguage::BASIC || lang_ == ScriptLanguage::LUA; }  // D5 两引擎均实装（v0.8.0）
    // D6 文件管理内部工具
    bool  file_path_for(const std::string& name, std::string* full, Err* e) const; // 名字校验 + 拼全路径
    bool  save_script_file(const std::string& name, const std::string& src, std::string* save_err); // 下载成功后落盘

    Shared&      sh_;
    DebugCfg     cfg_;
    bool         allow_script_ = true;
    ScriptLanguage lang_       = ScriptLanguage::BASIC;
    std::atomic<bool>* restart_flag_ = nullptr;   // main 的重启兜底标志（可空）
    ModbusServer* mb_ = nullptr;                  // D12：固件 Modbus 引擎（可空=未接线）
    std::string   mb_cfg_path_;                   // D12：组态文件路径（config/modbus.json）
    ModbusMaster* master_ = nullptr;              // D13：固件 Modbus 主站（可空=未接线）
    std::string   master_cfg_path_;               // D13：组态文件路径（config/modbus_master.json）

    // 监听/连接
    int                listen_fd_ = -1;
    int                bound_port_ = 0;
    std::atomic<bool>  stop_{false};
    std::atomic<int>   client_fd_{-1};
    std::thread        th_;

    // 脚本引擎（惰性装载；仅会话线程操作 load/compile）
    ScriptEngineSlot   slot_;
    std::unique_ptr<MotionHost> host_;
    bool               compiled_   = false;
    bool               compiled_ok_= false;
    std::thread        run_th_;
    mutable std::mutex run_mtx_;
    RunSnap            snap_{};
    // D4 热更新：被换出的旧引擎。worker 线程可能仍在其上跑 run()，
    // 故持有到 run_th_ join 之后再释放（release_retired / 析构）。
    std::unique_ptr<IScriptEngine> retired_;

    // 打印输出缓冲（MotionHost::print -> 队列 -> log 事件）
    mutable std::mutex       log_mtx_;
    std::vector<std::string> logs_;

    // 会话状态（单客户端）
    bool      authed_   = false;
    Sub       sub_axis_, sub_bus_, sub_mb_, sub_conn_;   // sub_conn_：D10 通讯状态（~2Hz）
    bool      sub_log_  = false;
    long long last_script_emit_ms_ = 0;
    std::string last_script_key_;          // status|steps|error_line 组合，用于跳变检测
    int       mb_cursor_ = 0;              // mb 分段推送游标（0..kModbusRegCount）
};

} // namespace kx
