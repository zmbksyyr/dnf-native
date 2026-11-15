# 验证、已知问题与证据

本文区分清理阶段、上轮目录整理和原项目历史实测。验证环境、源码/程序哈希及实际运行范围以对应 JSON 收据为准；映射、进程存活、端口监听或测试数量均不能单独作为业务验收。

## ELF 诊断实现进度（2026-11-15）

当前双架构已通过真实 guest 访问异常、符号/寄存器/指令/栈帧诊断、重复 ENOSYS 计数夹具；19 个原版服务映射与六种错误拒绝、入口、TLS、八线程 VM 和 futex 回归通过。固定资源全部 121 个 ELF 的离线 manifest 已生成。线程退出使用单次内核日志写入，避免 CRT 日志与 exit_group 并发导致的退出故障。

带诊断宿主的 GUI 回归 `20261115T152941288208Z` 完成两轮 channel/relay/STUN 协议、逆序停止、协调备份和正常关闭；整轮最终因新暴露的 ENOSYS 记录而失败，不能写成零缺口通过。记录包括 glibc robust list 与 signal flags 等回退路径；这些以前存在但没有统一日志。旧 relay 崩溃本轮未复现，尚未证明修复。清单与收据见外部 `P3-20261115/`。任意缺失导入的寄存器现场、无帧指针的完整栈展开及 proc maps 完整页级生命周期仍未完成。

## 当前 GUI 工作流阶段（2026-11-15）

P2 实现四组折叠、状态/功能筛选、选择保留、服务元数据及分层就绪状态；维护摘要显示数据库身份、停服计划、目标和结果。Windows 10 x64 10.0.19044 上当前 GUI 编译通过，100/125/150% 模拟 DPI 的矩形检查和截图已核对；独立维护回归验证了 15 项服务按清单逆序停止、数据库正常停库后备份、结果与目标显示，没有强制清理。

较早的 P2 服务完整回归 `20261115T151140773105Z` 通过。最终 GUI 的两次协议复测分别复现原 relay `0x0805b91a` 访问异常，以及 channel TCP bind 失败；前者 GUI 正确显示退出，后者进程仍存活但接口不可用。这些失败说明进程状态不能代替协议验证，仍保留为 P3/P4 的实际问题。当前不声称整轮协议回归通过或业务就绪；本阶段没有修改运行时。原始失败、维护和布局收据及二进制哈希见外部 `P2-20261115/p2-receipt.json`。

## P1 GUI 布局阶段

本阶段把 GUI 改为顶部实例上下文、左侧数据库/服务分组、右侧服务详情与日志的两栏布局。服务列表改为单列并显示四类分组标记，去除 `DrawFocusRect()` XOR 焦点框，加入 DPI 变更后的重新布局。源码构建和收据位于仓库外的 `../x/dnf-native-output/P1-20261115/`。

在 Windows 10 x64 10.0.19044、LLVM-MinGW 20261104 MSVCRT 上，P1 完成以下检查：

- GUI 使用 `-Wall -Wextra -Werror` 编译通过，构建收据记录程序和源文件哈希。
- 独立布局探针在 100%、125%、150% 模拟 DPI 和连续窗口尺寸下检查所有可见子控件矩形，`gui-layout-receipt.json` 中 3 组结果均无越界或非正尺寸，并保留截图。
- `test_gui_services.py` 使用新的 GUI 包完成 19 项服务加载、15 个原版服务协议、单服务停止/重启、真实日志显示、ELF 哈希拒绝、整体逆序停服、数据库协调备份和正常关闭；`runtime_gaps` 为空，未发生强制清理。

该阶段只证明 GUI 布局和既有服务控制回归；服务业务就绪、安全组件完整链路、客户端业务和 P2 折叠/筛选工作流仍未验收。

## 仓库清理

清理以 `fbb11f3` 为基线，撤除 23 个旧导入、CAB 重建、阶段诊断及已替代的测试程序，`tools/` 从 18 个脚本收敛为当前构建所需的 7 个。ELF 依赖读取并入基底核验；窗口、协议、目录身份和私有资源夹具从旧程序中提取。现有源码与两个资源 ZIP 未修改。

保留的加载器、channel、relay、bridge、Lua/Frida 专项直接使用固定 ZIP 中的资源；GUI 服务专项从停库实例建立副本，并按当前实现检查运行时可用的两项协调维护操作。旧 GUI/CAB 初始化程序不再充当当前交付验收。Python 子进程显式使用 `-B`，原有缓存已移到仓库外。

81 份历史证据合并为 `history.json`，逐份与清理前提交的 JSON 内容和原始 Git 文件哈希核对。当前文档链接及资源来源指针已同步更新，历史内容可按原文件名定位。

本轮在 Windows 10 x64 10.0.19044、Python 3.14、LLVM-MinGW 20261104 MSVCRT 上实际完成以下检查，脱敏摘要及源码/程序哈希见 [cleanup.json](evidence/cleanup.json)，私有原始日志与实例保存在仓库外：

- 完整构建与 `../x/dnf-native-output/package-cleanup/` 打包；39 个 Python 模块语法与导入检查、文档链接、7 个入口及公共夹具的仓库内输出拒绝；仓库没有 `build/`、缓存或其他生成输出。
- 精简后的依赖读取与旧实现逐一比对基底内 121 个 ELF；81 份历史证据的内容和 Git 原字节哈希一致。
- 15 项映射、入口、内存、线程、文件、网络、信号和真实动态加载器脚本回归；Lua timer、Frida hook、双向 RPC、缓存 reset 边界与恢复夹具，以及实际资源编码往返。
- 独立 channel 拆/连包与 relay 转发/断开及 3 轮重连；bridge 版本/脚本协议和真实 game channel proxy。
- 新包双实例创建、资源故障拒绝、数据保留、窗口占用、取消与完整删除；15 个辅助服务协议及 game/DP2/Frida 自然启停。本次 game 启动约 30.060 秒，正常停服，没有强制回收。
- GUI 单服正常停止/重启、真实日志、损坏 ELF 拒绝、两次整体启停与协调备份；官方 MySQL 初始化、TLS、影子还原、失败保留、冷快照、回滚、中断恢复，以及 GUI 从运行态停服再备份。

完整 GUI 插件模式与日志查看专项保留并通过导入检查，本轮未重新跑完这两组专项，也未重新执行客户端完整业务、负载或 Win7/Win11 矩阵。下述历史及已知问题的边界继续适用。

## 上轮目录与构建整理

本次把源码按 runtime/controller/database 功能归入 `src/`，测试与探针统一到 `tests/`，说明文档合并为四份。原独立许可证目录已撤除，必要声明保存在 [third-party.md](third-party.md)。

`resources/native.zip`、`mysql.zip` 和 `baseline.ini` 与 [p20 交付记录](evidence/history.json#/records/p20-instance-delivery.json)中的对应哈希一致。两个 ZIP 作为固定基底随仓库保存；当前宿主、维护工具和 GUI 从源码重新构建。正常打包的资源输入来自仓库基底，自研程序来自本轮外部构建输出，不再读取历史资源目录、原数据库或旧 CAB。

本轮在 Windows 10 x64 10.0.19044、Python 3.14 和固定 LLVM-MinGW 20261104 MSVCRT 上完成以下新检查，完整原始收据保存在仓库外的 `../x/dnf-native-output/`，脱敏摘要见[整理阶段证据](evidence/reorganization.json)：

- 外部输出路径的双架构宿主、数据库维护程序、GUI 完整构建与 ZIP 包生成；四个当前构建入口实际拒绝仓库内输出，仓库没有 `build/`。
- 基底全部 19 个服务的固定地址映射、四类错误输入拒绝，以及入口、VM、并发、TLS、futex、文件、同步、网络和信号等 13 项脚本回归。
- 资源内简繁中文、韩文与 UTF-8 的实际 12 项字节往返转换。
- 新包真实双实例生命周期、资源缺失/损坏拒绝、数据保留、窗口占用、取消与完整删除、15 个辅助服务协议；game/DP2/Frida 自然启动约 29.652 秒，正常停服，无强制回收。
- 实例内官方 MySQL 初始化、TLS 读写、备份还原；独立私有副本的影子还原、损坏拒绝、失败保留、冷快照、自动回滚和中断恢复；GUI 从运行状态逆序停服后备份，均正常完成。
- 迁移脚本的语法/导入检查、文档链接、13 份必要原始声明内容核对，以及最终包与实际测试包的程序、ZIP 和运行清单字节一致性核对。

最终审阅构建为 `../x/dnf-native-output/package-review/`。它与外部实际运行的 `package-clean/` 使用相同 GUI、双架构宿主、维护程序、两个 ZIP 和 `resources.ini`，仅附带说明文档更新。历史 `p20` 是迁移前版本；本轮没有重跑客户端完整业务、负载或 Win7/Win11 矩阵，也没有把通过复测写成 relay 偶发故障已修复。

## 当前回归入口

| 范围 | 命令 | 能证明的边界 |
| --- | --- | --- |
| 完整构建 | `python -B tools/build_all.py` | 当前双架构宿主、GUI/维护程序可编译和打包，基底/依赖身份核对 |
| ELF 映射 | `python -B tests/test_elf.py` | 固定基底中的服务映射和错误输入拒绝，不执行服务业务 |
| 入口与基础 syscall | `python -B tests/test_entry.py` | 双架构独立夹具 |
| 内存与并发 | `python -B tests/test_vm.py`、`tests/test_vm_concurrent.py` | 双架构映射语义及八线程并发路径 |
| TLS/线程 | `python -B tests/test_tls.py`、`tests/test_futex.py` | 已实现 TLS 和 futex 路径 |
| 文件、同步、网络原语 | `tests/test_files.py`、`tests/test_file_sync.py`、`tests/test_net.py`、`tests/test_poll.py`、`tests/test_epoll.py`、`tests/test_select_close.py`、`tests/test_process_signal.py` | 对应夹具，均以 `python -B` 执行 |
| 编码 | `python -B tests/test_resource_encodings.py --package ../x/dnf-native-output/package` | ZIP 内实际 GBK、GB18030、BIG5、BIG5-HKSCS、EUC-KR、UHC 与 UTF-8 字节往返 |
| 真实实例与 core | `python -B tests/test_instance_workflow.py --gui ../x/dnf-native-output/package/DNF-Native.exe` | 真实资源故障拒绝、双实例、占用、删除、15 个辅助服务协议及 game/DP2/Frida 启停 |
| 数据库维护 | `python -B tests/test_mysql_maintenance.py --exe ../x/dnf-native-output/package/resources/mysql-control.exe --database-source PRIVATE_STOPPED_INSTANCE` | 私有停库副本的备份、影子还原、失败保留与中断恢复 |
| GUI 停服维护 | `python -B tests/test_gui_maintenance.py --instance PRIVATE_INSTANCE --gui ../x/dnf-native-output/package/DNF-Native.exe` | GUI 从运行态逆序停服再备份 |

所有运行数据写入仓库外的 `../x/dnf-native-output/`，不在仓库内创建输出目录。GUI 测试需要可交互桌面，固定游戏端口应空闲；测试仅操作自己创建或明确传入的私有实例。现存测试已解除对原始安装包、原数据库、历史 sysroot 和旧布局交付包的依赖；动态用户库、协议和插件夹具从仓库 ZIP 解出到独立副本。

`gui_support.py` 集中窗口、列表和截图函数；`runtime_support.py` 集中基底解包、目录身份与停服事件；`probe_core_protocols.py` 集中 channel/relay/STUN 协议及独立探测入口；`game_fixture.py` 仅运行显式 Lua/Frida 夹具。这些辅助文件不分别代表业务验收。

保留的专项入口还包括 `test_channel.py`、`test_relay.py --reconnect-rounds N`、`test_bridge.py --db-config PRIVATE_JSON [--game-proxy]`、`test_gui_services.py --instance PRIVATE_STOPPED_INSTANCE --gui PACKAGE_EXE`、`test_gui_logs.py` 和 `test_gui_plugins.py`。bridge 专项要求指定实例的数据库已在 3306 运行，使用明文 bridge 格式的私有连接文件及当前官方客户端；GUI 服务与插件专项从传入的停库实例创建私有副本。详细参数见各入口的 `--help`。

## 历史实测范围

以下均是此前在 Windows 10 x64 10.0.19044 上完成的记录，并非本次重新执行：

| 阶段 | 已记录结果 | 证据 |
| --- | --- | --- |
| 原版 ELF 与用户库 | 双架构固定地址加载、动态加载器进入、部分启动 syscall | [p1 映射](evidence/history.json#/records/p1-map-checks.json)、[用户库](evidence/history.json#/records/p1-user-libraries.json) |
| core 协议 | channel 拆/连包、relay 双向转发、真实 monitor 拒绝未登录账号、STUN UDP 往返 | [p3 core](evidence/history.json#/records/p3-core-protocols.json)、[p20](evidence/history.json#/records/p20-instance-delivery.json) |
| MySQL | 官方程序身份、安全初始化、原种子往返、应用库联动、维护备份/还原及恢复 | [p4 集成](evidence/history.json#/records/p4-mysql-core-integration.json)、[p5 维护](evidence/history.json#/records/p5-mysql-native-maintenance.json)、[p20](evidence/history.json#/records/p20-instance-delivery.json) |
| GUI 与插件 | 初始化、服务控制、原生日志、DP2/Lua/Frida 开关与 hook/双向调用 | [p6 插件](evidence/history.json#/records/p6-gui-plugin-switches.json)、[p8 日志](evidence/history.json#/records/p8-gui-original-logs.json)、[p11 GUI](evidence/history.json#/records/p11-gui-reference.json) |
| 游戏业务 | 原服务 PVF 下认证、建角、洛兰多房间战斗、Boss、结算回城；冷重启存档读回 | [p12 游戏](evidence/history.json#/records/p12-gameplay-and-cache.json) |
| 并发与性能 | 映射槽竞态复现与锁修复、TLS 快路径、只读 ELF 视图及缓存 reset 局部实验 | [p7 性能](evidence/history.json#/records/p7-runtime-performance.json)、[p12](evidence/history.json#/records/p12-gameplay-and-cache.json)、[p13 并发](evidence/history.json#/records/p13-runtime-concurrency.json) |
| ZIP 实例交付 | 中文/空格路径双实例创建选择、数据保留、损坏拒绝、占用互斥、完整删除、编码和自然启停；桌面备份还原读回 | [p20](evidence/history.json#/records/p20-instance-delivery.json) |

历史 p20 game 自然启动约 30.167 秒，正常停服到达 Frida dispose 和 DP2 unload；这只是该版本、该配置的一次观察。历史游戏验证使用带客户端功能的 `full` 模式，不能计为纯净评分、倍率和全部客户端验收。之前曾发生后续客户端图形初始化失败，因此没有声称所有修复后的真实玩家退出都重新验证。

## 未完成事项

- **relay 偶发异常**：历史 `0x0805b91a` / `TManager<RelayService>::getManager()` 访问异常，在 ZIP 实例回归中也出现过。后续两轮完整通过及并发修复后 30 次未复现均不能证明根治。
- **STUN TLS**：曾有历史 guest TLS 站点失败现场，不能由普通 UDP 回归扩展为所有 x64 ELF 支持。
- **Frida 文件监视接口**：本轮插件夹具仍记录 i386 `inotify_init` / `inotify_init1`（syscall 291/332）未实现。hook、双向 RPC、reset 边界和恢复专项通过不代表这些接口已支持，诊断保留在外部收据中。
- **jbz69 定制 PVF**：原服务 PVF 为 48984，客户端定制 PVF 为 66282；未找到匹配的原版 Linux ELF。缺失模板、BIG5/UTF-8 编码及资源字段差异仍阻塞 Loading Items。模板别名、临时 iconv 映射和 `.str` 重编码实验均未形成可交付兼容结果；认证公钥加载成功不能解释成定制 PVF 可用。
- **安全与负载**：安全三件套/TSS 完整拓扑、真实大连接数、生产负载、所有插件组合和完整客户端矩阵没有验收。256 描述符及 64 线程槽等限制见[实现说明](architecture.md)。
- **操作系统**：干净 Windows 7/11、跨机器迁移及所有离线依赖组合未完成矩阵验证。

## 历史路径与来源对应

原阶段 Markdown 已合并；原内容可从 Git 整理前提交 `f7d57f5` 复核，例如 `git show f7d57f5:docs/20-instance-delivery.md`。81 份历史 JSON 合并到 [history.json](evidence/history.json)，通过 `records` 下的原文件名定位，数据内容保持不变。`original_sha256` 记录合并前每份原文件的哈希，精确原字节可从其 `source_commit`（`fbb11f3`）恢复。原有路径、哈希、命令和本机 probe 位置都是当时的身份记录，不自动替换为新路径或实测结论。上轮整理收据仍单独保留为 `reorganization.json`。

| 历史路径 | 当前位置 |
| --- | --- |
| `runtime/` | `src/runtime/` |
| `delivery/controller_gui.cpp` 及 GUI 头文件 | `src/controller/` |
| `delivery/mysql_control.cpp`、`mysql_operations.h` | `src/database/` |
| `delivery/win32_files.h` | `src/win32_files.h` |
| `third_party/miniz/` | `src/third_party/miniz/` |
| 仍适用的测试、协议和插件夹具 | `tests/`；公共函数从历史阶段程序中提取 |
| 原始导入、CAB 重建及已替代的阶段测试 | 已撤除，原件见清理前提交 `fbb11f3` |
| 根 README、THIRD_PARTY、LICENSES、阶段 Markdown | 本目录四份文档；必要原始声明见 `third-party.md` |

资源来源和参考边界仍以原始输入身份、ELF 观察和独立实验为准。参考工程未接入本次构建和验收链，本次整理不改变历史失败或降低验收标准。
