cmake_minimum_required(VERSION 3.16)
# Lua 引擎为纯 C（vendor Lua 5.4），故同时启用 C 语言（见 docs/planA/09）。
project(kine-x C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -O2 -pthread)

# ---- vendor Lua 5.4（第三方，MIT；见 third_party/lua/LICENSE）----
# 原样 vendor，仅作为静态库链接给 Lua 脚本引擎；不定义 LUA_USE_LINUX，
# 以便不引入 dlopen/readline 依赖（package/require 本就被沙箱禁用）。
file(GLOB LUA_SRC CONFIGURE_DEPENDS third_party/lua/*.c)
add_library(lua STATIC ${LUA_SRC})
target_include_directories(lua PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua)
target_compile_options(lua PRIVATE -w)          # 第三方源码不参与本项目的 -Wall -Wextra
target_link_libraries(lua PUBLIC m)             # Lua 使用 libm（glibc<2.34 需显式链接）

# ---- 依赖：IgH EtherLab (ecrt) ----
# 官方 make install 默认装到 /usr/local；Debian 包则可能在 /usr。
# 注意：未找到 IgH 时**不阻断配置**——纯逻辑目标（cia402_test 等）仍可编译，
#       依赖 ecrt 的目标会自动跳过。上板前必须装好 IgH（见 docs/planA/02）。
find_path(ECRT_INCLUDE_DIR ecrt.h
          PATHS /usr/local/include /usr/include /usr/local/include/ethercat)
find_library(ECRT_LIBRARY ethercat
             PATHS /usr/local/lib /usr/lib /usr/lib/aarch64-linux-gnu)
find_package(PkgConfig QUIET)

set(HAVE_ECRT FALSE)
if(ECRT_INCLUDE_DIR AND ECRT_LIBRARY)
  set(HAVE_ECRT TRUE)
  message(STATUS "ecrt include: ${ECRT_INCLUDE_DIR}")
  message(STATUS "ecrt library: ${ECRT_LIBRARY}")
else()
  message(WARNING
    "未找到 IgH EtherLab (ecrt.h / libethercat)，将跳过 EtherCAT 相关目标。\n"
    "请按 docs/planA/02 编译安装：./configure --prefix=/usr/local && make && sudo make install && sudo ldconfig")
endif()

# ---- 纯逻辑自测（不需要 IgH / 硬件，任何机器都能跑）----
enable_testing()
add_executable(cia402_test tools/cia402_test.cpp src/motion/cia402.cpp)
target_include_directories(cia402_test PRIVATE src)
add_test(NAME cia402_test COMMAND cia402_test)

add_executable(axis_test tools/axis_test.cpp
               src/motion/cia402.cpp src/motion/axis.cpp)
target_include_directories(axis_test PRIVATE src)
add_test(NAME axis_test COMMAND axis_test)

# M3 插补纯逻辑（Trapezoid / LinInterp）
add_executable(interp_test tools/interp_test.cpp)
target_include_directories(interp_test PRIVATE src)
add_test(NAME interp_test COMMAND interp_test)

add_executable(config_test tools/config_test.cpp src/common/config.cpp)
target_include_directories(config_test PRIVATE src)
add_test(NAME config_test COMMAND config_test)

# 驱动器档案：注册表 + 身份匹配（纯逻辑，不需要 IgH / 硬件）
add_executable(drive_profile_test tools/drive_profile_test.cpp
               src/motion/drive_profile.cpp
               src/motion/profiles/sv630.cpp
               src/motion/profiles/generic.cpp)
target_include_directories(drive_profile_test PRIVATE src)
add_test(NAME drive_profile_test COMMAND drive_profile_test)

# ESI/EtherCATInfo 导入解析 + 与档案对比（纯逻辑）
set(KX_PROFILE_SRCS
    src/motion/drive_profile.cpp
    src/motion/profiles/sv630.cpp
    src/motion/profiles/generic.cpp)

add_executable(esi_parser_test tools/esi_parser_test.cpp
               src/motion/esi_parser.cpp ${KX_PROFILE_SRCS})
target_include_directories(esi_parser_test PRIVATE src)
add_test(NAME esi_parser_test COMMAND esi_parser_test)

add_executable(esi_tool tools/esi_tool.cpp
               src/motion/esi_parser.cpp ${KX_PROFILE_SRCS})
target_include_directories(esi_tool PRIVATE src)

# ZBasic 子集脚本引擎（纯逻辑，不需要 IgH / 硬件）
add_executable(script_test tools/script_test.cpp src/script/script.cpp)
target_include_directories(script_test PRIVATE src)
add_test(NAME script_test COMMAND script_test)

# Lua 脚本引擎（vendor Lua 5.4；纯逻辑，不需要 IgH / 硬件，见 docs/planA/09）
add_executable(lua_engine_test tools/lua_engine_test.cpp src/script/lua_engine.cpp)
target_include_directories(lua_engine_test PRIVATE src)
target_link_libraries(lua_engine_test PRIVATE pthread lua)
add_test(NAME lua_engine_test COMMAND lua_engine_test)

# 脚本引擎规则：控制器同一时刻只加载一种脚本语言（纯逻辑，见 src/script/engine_rule.h）
add_executable(engine_rule_test tools/engine_rule_test.cpp
               src/script/engine_rule.cpp src/script/script.cpp src/script/lua_engine.cpp)
target_include_directories(engine_rule_test PRIVATE src)
target_link_libraries(engine_rule_test PRIVATE lua)
add_test(NAME engine_rule_test COMMAND engine_rule_test)

# 主文件（开机运行）清单 D8：目录语言扫描 / .boot 读写（纯文件逻辑）
add_executable(boot_config_test tools/boot_config_test.cpp src/script/boot_config.cpp)
target_include_directories(boot_config_test PRIVATE src)
add_test(NAME boot_config_test COMMAND boot_config_test)
set_tests_properties(boot_config_test PROPERTIES TIMEOUT 30)

# 4x 寄存器持久化（NVSET/NVGET 的 .nvram 读写；纯文件逻辑）
add_executable(nvram_test tools/nvram_test.cpp src/script/nvram_store.cpp)
target_include_directories(nvram_test PRIVATE src)
add_test(NAME nvram_test COMMAND nvram_test)
set_tests_properties(nvram_test PROPERTIES TIMEOUT 30)

# 多文件包含展开（INCLUDE / include）：纯逻辑，不需要硬件
add_executable(source_include_test tools/source_include_test.cpp
               src/script/source_include.cpp src/script/boot_config.cpp)
target_include_directories(source_include_test PRIVATE src)
add_test(NAME source_include_test COMMAND source_include_test)
set_tests_properties(source_include_test PROPERTIES TIMEOUT 30)

# 离线 Lua 运行器 + 端口服务脚本自测（纯逻辑，不需要 IgH / 硬件）
add_executable(lua_runner tools/lua_runner.c)
target_link_libraries(lua_runner PRIVATE lua)
add_test(NAME port_script_selftest
         COMMAND lua_runner ${CMAKE_CURRENT_SOURCE_DIR}/tools/port_script_selftest.lua
                 ${CMAKE_CURRENT_SOURCE_DIR}/../EtherCAT_SocketServer.lua)
set_tests_properties(port_script_selftest PROPERTIES TIMEOUT 30)

# 脚本设备宿主 -> kx::Shared 的绑定（用假 RT 线程，不需要 IgH / 硬件）
add_executable(motion_host_test tools/motion_host_test.cpp
               src/script/motion_host.cpp src/script/port_manager.cpp src/script/script.cpp
               src/script/nvram_store.cpp
               src/script/lua_engine.cpp    src/modbus/modbus_config.cpp
               src/modbus/modbus_server.cpp
               src/modbus/modbus_master_config.cpp
               src/modbus/modbus_master.cpp)
target_include_directories(motion_host_test PRIVATE src)
target_link_libraries(motion_host_test PRIVATE pthread lua)
add_test(NAME motion_host_test COMMAND motion_host_test)
set_tests_properties(motion_host_test PROPERTIES TIMEOUT 60)

# 脚本端口通道 -> 真实 TCP socket（回环 socket 自测，不需要 IgH / 硬件）
add_executable(port_manager_test tools/port_manager_test.cpp src/script/port_manager.cpp
               src/script/port_config.cpp)
target_include_directories(port_manager_test PRIVATE src)
target_link_libraries(port_manager_test PRIVATE pthread)
add_test(NAME port_manager_test COMMAND port_manager_test)
set_tests_properties(port_manager_test PROPERTIES TIMEOUT 60)

# 调试通道服务端（D1~D3：sys.info/var/axis/cmd/script/订阅推送）：
# 回环 socket 自测，不需要 IgH / 硬件（见 docs/planA/13、16）
add_executable(debug_server_test tools/debug_server_test.cpp
               src/script/debug_server.cpp
               src/script/boot_config.cpp
               src/script/port_config.cpp
               src/script/source_include.cpp
               src/script/motion_host.cpp
               src/script/nvram_store.cpp
               src/script/port_manager.cpp
               src/script/script.cpp
               src/script/engine_rule.cpp
               src/script/lua_engine.cpp
               src/modbus/modbus_config.cpp
               src/modbus/modbus_server.cpp
               src/modbus/modbus_master_config.cpp
               src/modbus/modbus_master.cpp)
target_include_directories(debug_server_test PRIVATE src)
target_link_libraries(debug_server_test PRIVATE lua pthread)
add_test(NAME debug_server_test COMMAND debug_server_test)
set_tests_properties(debug_server_test PROPERTIES TIMEOUT 60)

# Modbus 从站引擎（产品化；planA/20）：组态/帧级回环自测，不需要 IgH / 硬件
add_executable(modbus_server_test tools/modbus_server_test.cpp
               src/modbus/modbus_server.cpp
               src/modbus/modbus_config.cpp
               src/script/nvram_store.cpp)
target_include_directories(modbus_server_test PRIVATE src)
target_link_libraries(modbus_server_test PRIVATE pthread)
add_test(NAME modbus_server_test COMMAND modbus_server_test)
set_tests_properties(modbus_server_test PROPERTIES TIMEOUT 60)

# Modbus 主站引擎（产品化 P4；planA/21）：以 ModbusServer 当假从站做环回自测
add_executable(modbus_master_test tools/modbus_master_test.cpp
               src/modbus/modbus_master.cpp
               src/modbus/modbus_master_config.cpp
               src/modbus/modbus_server.cpp
               src/modbus/modbus_config.cpp
               src/script/nvram_store.cpp)
target_include_directories(modbus_master_test PRIVATE src)
target_link_libraries(modbus_master_test PRIVATE pthread)
add_test(NAME modbus_master_test COMMAND modbus_master_test)
set_tests_properties(modbus_master_test PROPERTIES TIMEOUT 60)

# 脚本 CLI：语法检查 + dry 试跑（调试软件的脚本后端雏形）；支持 BASIC 与 Lua 双引擎
add_executable(script_run tools/script_run.cpp
               src/script/script.cpp src/script/lua_engine.cpp src/script/engine_rule.cpp)
target_include_directories(script_run PRIVATE src)
target_link_libraries(script_run PRIVATE lua)

# 网口配置（eth1 设置 IP；eth0 受保护）
add_executable(netcfg_test tools/netcfg_test.cpp src/common/netcfg.cpp)
target_include_directories(netcfg_test PRIVATE src)
add_test(NAME netcfg_test COMMAND netcfg_test)

add_executable(netcfg_tool tools/netcfg_tool.cpp src/common/netcfg.cpp)
target_include_directories(netcfg_tool PRIVATE src)

# ---- 增量2 自检工具 ----
if(HAVE_ECRT)
  add_executable(ecat_probe tools/ecat_probe.cpp
                 src/motion/ethercat_master.cpp
                 src/motion/drive_profile.cpp
                 src/motion/profiles/sv630.cpp
                 src/motion/profiles/generic.cpp)
  target_include_directories(ecat_probe PRIVATE ${ECRT_INCLUDE_DIR} src)
  target_link_libraries(ecat_probe PRIVATE ${ECRT_LIBRARY} pthread rt)
endif()

# ---- M2 运动内核实机验证工具（使能序列 / PP 点位 / 去使能）----
if(HAVE_ECRT)
  add_executable(ecat_axis tools/ecat_axis.cpp
                 src/motion/ethercat_master.cpp
                 src/motion/drive_profile.cpp
                 src/motion/profiles/sv630.cpp
                 src/motion/profiles/generic.cpp
                 src/motion/cia402.cpp
                 src/motion/axis.cpp)
  target_include_directories(ecat_axis PRIVATE ${ECRT_INCLUDE_DIR} src)
  target_link_libraries(ecat_axis PRIVATE ${ECRT_LIBRARY} pthread rt)
endif()

# ---- 增量5/6 主程序 ----
# 控制器本体：EtherCAT 多轴主站 + 运动内核 + 脚本引擎。
# 与机器人/触摸屏/称重等外部设备的具体协议对接改由脚本层实现（见 docs/planA/12）。
if(HAVE_ECRT)
  add_executable(kine-x
      src/main.cpp
      src/common/config.cpp
      src/motion/ethercat_master.cpp
      src/motion/drive_profile.cpp
      src/motion/profiles/sv630.cpp
      src/motion/profiles/generic.cpp
      src/motion/cia402.cpp
      src/motion/axis.cpp
      src/script/script.cpp
      src/script/motion_host.cpp
      src/script/port_manager.cpp
      src/script/engine_rule.cpp
      src/script/lua_engine.cpp
      src/script/debug_server.cpp
      src/script/boot_config.cpp
      src/script/port_config.cpp
      src/script/nvram_store.cpp
      src/script/source_include.cpp
      src/modbus/modbus_config.cpp
      src/modbus/modbus_server.cpp
      src/modbus/modbus_master_config.cpp
      src/modbus/modbus_master.cpp)
  target_include_directories(kine-x PRIVATE ${ECRT_INCLUDE_DIR} src)
  target_link_libraries(kine-x PRIVATE ${ECRT_LIBRARY} pthread rt lua)
endif()
