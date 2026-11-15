# 验证、已知问题与证据

本文区分本次整理的新检查和原项目历史实测。验证环境、源码/程序哈希及实际运行范围以对应 JSON 收据为准；映射、进程存活、端口监听或测试数量均不能单独作为业务验收。

## 本次目录与构建整理

本次把源码按 runtime/controller/database 功能归入 `src/`，测试与探针统一到 `tests/`，说明文档合并为四份。原独立许可证目录已撤除，必要声明保存在 [third-party.md](third-party.md)。

`resources/native.zip`、`mysql.zip` 和 `baseline.ini` 与 [p20 交付记录](evidence/p20-instance-delivery.json)中的对应哈希一致。两个 ZIP 作为固定基底随仓库保存；当前宿主、维护工具和 GUI 从源码重新构建。正常打包的资源输入来自仓库基底，自研程序来自本轮外部构建输出，不再读取历史资源目录、原数据库或旧 CAB。

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

所有运行数据写入仓库外的 `../x/dnf-native-output/`，不在仓库内创建输出目录。GUI 测试需要可交互桌面，固定游戏端口应空闲；测试仅操作自己创建或明确传入的私有实例。部分早期 `probe_*.py`、原用户库/原库导入实验及旧布局 GUI 专项仍要求独立 `../x/dnf-native-output/originals/` 和开发缓存，它们保留作诊断，不能混作新 ZIP 包的默认验收入口。

## 历史实测范围

以下均是此前在 Windows 10 x64 10.0.19044 上完成的记录，并非本次重新执行：

| 阶段 | 已记录结果 | 证据 |
| --- | --- | --- |
| 原版 ELF 与用户库 | 双架构固定地址加载、动态加载器进入、部分启动 syscall | [p1 映射](evidence/p1-map-checks.json)、[用户库](evidence/p1-user-libraries.json) |
| core 协议 | channel 拆/连包、relay 双向转发、真实 monitor 拒绝未登录账号、STUN UDP 往返 | [p3 core](evidence/p3-core-protocols.json)、[p20](evidence/p20-instance-delivery.json) |
| MySQL | 官方程序身份、安全初始化、原种子往返、应用库联动、维护备份/还原及恢复 | [p4 集成](evidence/p4-mysql-core-integration.json)、[p5 维护](evidence/p5-mysql-native-maintenance.json)、[p20](evidence/p20-instance-delivery.json) |
| GUI 与插件 | 初始化、服务控制、原生日志、DP2/Lua/Frida 开关与 hook/双向调用 | [p6 插件](evidence/p6-gui-plugin-switches.json)、[p8 日志](evidence/p8-gui-original-logs.json)、[p11 GUI](evidence/p11-gui-reference.json) |
| 游戏业务 | 原服务 PVF 下认证、建角、洛兰多房间战斗、Boss、结算回城；冷重启存档读回 | [p12 游戏](evidence/p12-gameplay-and-cache.json) |
| 并发与性能 | 映射槽竞态复现与锁修复、TLS 快路径、只读 ELF 视图及缓存 reset 局部实验 | [p7 性能](evidence/p7-runtime-performance.json)、[p12](evidence/p12-gameplay-and-cache.json)、[p13 并发](evidence/p13-runtime-concurrency.json) |
| ZIP 实例交付 | 中文/空格路径双实例创建选择、数据保留、损坏拒绝、占用互斥、完整删除、编码和自然启停；桌面备份还原读回 | [p20](evidence/p20-instance-delivery.json) |

历史 p20 game 自然启动约 30.167 秒，正常停服到达 Frida dispose 和 DP2 unload；这只是该版本、该配置的一次观察。历史游戏验证使用带客户端功能的 `full` 模式，不能计为纯净评分、倍率和全部客户端验收。之前曾发生后续客户端图形初始化失败，因此没有声称所有修复后的真实玩家退出都重新验证。

## 未完成事项

- **relay 偶发异常**：历史 `0x0805b91a` / `TManager<RelayService>::getManager()` 访问异常，在 ZIP 实例回归中也出现过。后续两轮完整通过及并发修复后 30 次未复现均不能证明根治。
- **STUN TLS**：曾有历史 guest TLS 站点失败现场，不能由普通 UDP 回归扩展为所有 x64 ELF 支持。
- **jbz69 定制 PVF**：原服务 PVF 为 48984，客户端定制 PVF 为 66282；未找到匹配的原版 Linux ELF。缺失模板、BIG5/UTF-8 编码及资源字段差异仍阻塞 Loading Items。模板别名、临时 iconv 映射和 `.str` 重编码实验均未形成可交付兼容结果；认证公钥加载成功不能解释成定制 PVF 可用。
- **安全与负载**：安全三件套/TSS 完整拓扑、真实大连接数、生产负载、所有插件组合和完整客户端矩阵没有验收。256 描述符及 64 线程槽等限制见[实现说明](architecture.md)。
- **操作系统**：干净 Windows 7/11、跨机器迁移及所有离线依赖组合未完成矩阵验证。

## 历史路径与来源对应

原阶段 Markdown 已合并；原内容可从 Git 整理前提交 `f7d57f5` 复核，例如 `git show f7d57f5:docs/20-instance-delivery.md`。`docs/evidence/` 的历史 JSON 保持原样，其中旧路径、哈希、命令和本机 probe 位置都是当时的身份记录，不自动替换为本轮路径或结果。

| 历史路径 | 当前位置 |
| --- | --- |
| `runtime/` | `src/runtime/` |
| `delivery/controller_gui.cpp` 及 GUI 头文件 | `src/controller/` |
| `delivery/mysql_control.cpp`、`mysql_operations.h` | `src/database/` |
| `delivery/win32_files.h` | `src/win32_files.h` |
| `third_party/miniz/` | `src/third_party/miniz/` |
| `tools/test.py`、`tools/test_*.py`、`tools/probe_*.py` | `tests/test_elf.py`、`tests/test_*.py`、`tests/probe_*.py` |
| 四个 CAB/旧交付重建脚本 | `tools/legacy/` |
| 根 README、THIRD_PARTY、LICENSES、阶段 Markdown | 本目录四份文档；必要原始声明见 `third-party.md` |

资源来源和参考边界仍以原始输入身份、ELF 观察和独立实验为准。参考工程未接入本次构建和验收链，本次整理不改变历史失败或降低验收标准。
