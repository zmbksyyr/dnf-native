# dnf-native

DNF 台服原版 Linux ELF 在 Windows 7/10/11 x64 上原生运行的独立项目。

**当前状态：P1 实施中。** 原版 x64 STUN 已在本机完成 50 次 UDP 协议往返；原版 i386 manager 已通过原有 `nofork` 路径完成配置初始化和正常停服；原版 channel 已通过 32 次正确 TCP 握手及拆包、连包检查；原版 bridge 已连接私有 MySQL，版本检查、完整频道脚本响应及原版 game 代理下载接口通过。原版 game 已打开 13 类业务库，自然导入频道脚本并推进到 GameWorld 和登录状态初始化，随后仍返回 134，正在定位终止原因。19 个 ELF 原址映射、双架构 VM/文件/网络/执行原语和官方 MySQL 5.7.44 数据往返检查通过，完整服务业务及 Win7/Win11 尚未验证。实际范围与缺口见[实施记录](docs/06-progress.md)。

设计与初始审阅事项见[审阅记录](docs/05-review.md)。用户已授权开始实施，持续按阶段提交可复核结果。

- [工作纪律](AGENTS.md)：仅约束工作行为。
- [用户需求原文](docs/requirements.md)：从工作区需求文件逐字节归档，作为需求基线。
- [资源调研](docs/01-resource-research.md)：本地原包、公开规范、依赖与包体风险。
- [静态证据](docs/evidence/resource-inventory.json)与[网络来源](docs/evidence/web-resources.json)：本轮重新生成的记录。
- [原生运行方案](docs/02-technical-plan.md)：双架构、ELF、syscall/TLS、插件和服务管理。
- [数据库、GUI 与交付](docs/03-database-and-delivery.md)：MySQL 单文件、预快照/回滚及离线包。
- [实施阶段与验收](docs/04-milestones-and-acceptance.md)：P1 否决关卡、19 项服务矩阵、三系统矩阵及需求追踪。

仓库与上级目录中的输入包、`dnf-winport/`、`opendnf-win-jbz69/`、`x/` 独立。原始资产和构建产物不进入 Git。

静态审计可在开发机使用 Python 3.11+ 标准库复核，不执行或落盘解包输入代码：

```powershell
python tools/audit_resources.py --workspace .. --output .local/resource-inventory.json
```

其中 `--workspace` 是包含原始输入包的目录。运行包将不依赖 Python。证据包含生成时间，重复审计应比较输入/工具哈希与结构化结果，不要求时间字段相同。

开发与当前检查：

```powershell
python tools/bootstrap.py
python tools/import_inputs.py
python tools/import_linux.py
python tools/test.py
python tools/test_entry.py
python tools/test_ld.py
python tools/test_stun.py
python tools/probe_manager.py
python tools/test_vm.py
python tools/test_files.py
python tools/test_poll.py
python tools/test_net.py
python tools/test_channel.py
python tools/test_relay.py
python tools/test_futex.py
python tools/test_epoll.py
python tools/test_luv.py
python tools/import_mysql.py
python tools/probe_mysql.py
```

协议检查使用 STUN 原有 `test` 前台模式，绑定原 UDP 端口 2311–2313，结束后强制回收本次启动的测试进程；manager 使用原有 `nofork` 模式，再通过第二个原版 manager 的 `stop` 命令完成 SIGTERM 和应用资源清理。测试实例写在 `build/probes/` 的独立目录。后台派生、manager 管理协议和最终验收仍未通过。[第三方来源](THIRD_PARTY.md)记录实际引入的指令库和用户库。

channel 检查使用原版 `channel run`，需要 TCP/UDP 7001 空闲，配置仅修改独立副本中的 loopback 地址；完成握手后经 runtime 事件发送 SIGTERM，核对目标自行返回 0。该结果不包含 bridge、频道信息或游戏客户端业务。开发时已有旧宿主运行，可用 `python tools/test_channel.py --host build/staged/i686/dnf-host.exe` 明确选择独立构建。

bridge 检查需要已恢复原始 d_channel 数据的**私有 MySQL 副本**。原版 ELF 固定连接数据库 TCP 3306；服务端口可通过 `--port` 指定，默认 TCP/UDP 7000。将私有连接信息写到忽略目录的 `build/bridge-db.private.json`，字段为 `host`、`port`、`user`、`password`、`database`；数据库名为 `d_channel`，密码为 bridge 使用的明文配置，不是 game 配置中的编码值。测试只读取数据库，在独立副本配置 loopback 服务地址；原版 bridge 会将连接信息输出到私有日志，勿提交这些日志。

```powershell
python tools/test_bridge.py --db-config build/bridge-db.private.json --game-proxy
```

该检查核对版本相同/不同、拆包/连包和逐字节频道脚本响应。`--game-proxy` 通过原版 DP2/Frida 调用原版 game 的频道代理，并核对实际落盘的脚本和版本；测试在资源初始化之前主动退出，不计完整 game 启动或业务验收。bridge 通过 runtime SIGTERM 事件自行退出。

relay 检查需要 TCP 7200、UDP 8200 空闲，使用原版 PVP 模式和独立的 16 连接配置。两个自编测试连接登记后，核对双向二进制转发、拆包/连包，再通过 UDP S2S 控制报文定向断开一个连接，并验证另一个连接仍可收发。测试不包含原版 monitor 对接或客户端 PVP 业务；结束时通过 runtime SIGTERM 事件请求退出。

futex 检查使用自编双架构 ELF 的实际线程，验证比较失败、队列迁移、条件唤醒及原子字运算；不代表完整 Linux futex 或插件退出通过。原版 Frida 失败退出时仍存在模块解除映射后线程继续执行的异常，详见[实施记录](docs/06-progress.md)。

game 联合观察需要一个已停止的私有恢复测试库，目录包含 `data/` 和 `game-db.json`，且已配置 game 的测试账号及 `d_taiwan.db_connect`。JSON 使用 `probe_game.py --db-config` 的五个连接字段，其中密码为原版 game 使用的编码值。该夹具应基于 `probe_mysql.py` 的恢复副本准备，不使用原始数据库；其本地 named pipe 的 root 管理连接应可用。

```powershell
python tools/probe_game_chain.py --database-source PRIVATE_TEST_INSTANCE --timeout 2100
python tools/probe_game_chain.py --database-source PRIVATE_TEST_INSTANCE --script-db --timeout 60
```

联合观察在新的数据库副本中设置 bridge 和原版 JS 各自的账号，实际核对 bridge 脚本响应，再运行原版 game；要求 TCP 3306、7000 空闲。`--script-db` 只检查原版 JS 建库建表接口，随后主动退出 42。所有日志和每实例 receipt 保存在忽略的 `build/probes/`，不得将含连接信息的原始日志提交 Git。两种观察均不计完整业务验收。

`test_epoll.py` 用双架构自编 ELF 核对管道和 UDP socket 的实际混合等待；`test_luv.py` 在原版 game/DP2/Lua/luv 中触发并关闭真实定时器，循环返回后主动退出 42。它们覆盖已复现的 libuv 内部管道登记失败问题，不替代完整 game 或停服验收。已有进程占用正式宿主时，可分别用 `--hosts build/staged`、`--host build/staged/i686/dnf-host.exe` 选择独立构建。

`test_poll.py` 同时验证原版 bridge 所依赖的 select 微秒进位及超时后的剩余时间清零，保留管道实际等待检查；可用 `--hosts build/staged` 选择独立宿主。
