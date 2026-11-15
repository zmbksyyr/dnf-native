# 验证、已知问题与证据

本文区分清理阶段、上轮目录整理和原项目历史实测。验证环境、源码/程序哈希及实际运行范围以对应 JSON 收据为准；映射、进程存活、端口监听或测试数量均不能单独作为业务验收。

## 原版 relay 配置容量（2026-11-16）

P16 在固定原版 `relay_200.cfg` 的 `max_client=1000` 下实际复现：旧宿主完成 249 个连接注册，第 250 个被重置；已有连接仍可转发，释放后可恢复。共享描述符表只有 256 项，少于应用声明的容量。现将表扩为 `NR_FD_SLOTS=1024`，统一文件、socket、eventfd、文件映射、select/poll/epoll 和 `RLIMIT_NOFILE` 的边界；不修改原版 ELF 或配置。

双架构夹具在 fd 514 以上完成 8 线程、200 轮混合分配和事件隔离，覆盖高编号 socket/mixed select、1024 条 poll、耗尽后的 `EMFILE` 与 pipe 预留回滚。文件夹具预占 512 个描述符后，实际文件、procfs、目录、私有/共享映射及跨进程读写均通过。既有网络、超时/linger、select 并发关闭、poll、epoll、eventfd 和入口夹具也通过。

原版 relay 的并发协议验证分两组记录：持续组在 1000 个连接下转发 60.155 秒、核验 84,000 条消息；替换 100 个连接后，再完成 1000 条消息的完整转发环。该轮随后未等待原连接关闭确认就立即执行新连接恢复，因此整轮收据仍为失败。最终修正探针的批量心跳及关闭确认后，完整流程通过：1000 连接、1.450 秒负载和替换后的 3000 条消息，100 个连接的 EOF/替换，1000 个连接全部 EOF，再完成正常新连接、双向转发、拆/连包、定向断开与三轮重连。两组均无运行时异常或缺口，进程正常退出 0、无强制清理；不将短负载的最终通过写成重跑了 60 秒。

新包完整实例回归通过资源故障拒绝、中文/空格双实例、数据库读回、占用与删除保护、原版 channel/relay-monitor/STUN 协议及 game/DP2/Frida 自然启停，game 初始化 30.737 秒。GUI 退出 0、无运行时异常或强制清理，仅有已知 inotify 291/332 缺口，资源前后哈希不变。原始收据为 `probes/20261115T224727607898Z/instance-workflow/receipt.json`，当前交付为 `P16-20261116-relay-capacity/package/`；GUI/数据库工具沿用 P14，宿主为本阶段构建。

原始失败、各轮探针及最终收据保存在 `P16-20261116-relay-capacity/`，最终并发收据为 `concurrent-final.json`、持续组为 `concurrent-third.json`；私有实例路径由其中 `command` 标识。环境为 Windows 10 x64 10.0.19044、Python 3.14.6、LLVM-MinGW 20261104 MSVCRT。本阶段是固定原版 relay 的协议容量验证，未包含原版 monitor 认证、游戏玩家负载或长期运行验收。

## STUN 历史 TLS 站点与 ELF 清单（2026-11-16）

P15 将历史 `unimplemented guest TLS site pc=0x1a92c627dfb` 定位到固定 `libpthread.so.0`（SHA-256 `b3bf84a9c150a558b0fe4512ed20e18a346b6485ee0c5e770e55811678505abb`）的 `start_thread+0x1b`，ELF 地址 `0x7dfb`，原指令为 `movq %rdx,%fs:0x620`。这里 TLS 指线程局部存储。原版 `do_clone` 传入 `start_thread`，其入口在调用用户 worker 前必经该写入；STUN 的三个 worker 均由原版 `pthread_create` 创建。

使用 P14 交付的 x64 宿主和固定 ZIP 独立副本，原版 STUN 的 2311/2312 地址回应、跨端口回应及 2313 长度回应共 5 项全部通过。日志确认加载相同 pthread、进入三个 guest 线程，并经停止事件及 UDP 唤醒正常退出 0，保留原版 `STUN PROCESS IS TERMINATED`；无运行时缺口或强制清理，ELF、loader、pthread 和配置前后哈希不变。此结果结合原版控制流关闭该历史站点待办，没有增加补丁或运行时代码；未进行逐指令跟踪，不扩展为通用 x64 ELF、互联网 NAT 或客户端业务通过。

同时修复 `tools/elf_manifest.py`：重复的 `DT_NEEDED` 字符串必须按当前条目的偏移解析，此前错误地按标签寻找第一项。全部 121 个 ELF（95 个 32 位、26 个 64 位）的 424 条动态字符串与 LLVM 23.1.5 `llvm-readelf --dynamic` 独立逐项一致；80 个文件的展示得到纠正，原有 `dt_needed` 依赖数组及其他清单字段不变。该工具仅生成离线清单，交付程序与资源无需重建。

环境为 Windows 10 x64 10.0.19044、Python 3.14.6；宿主沿用 LLVM-MinGW 20261104 MSVCRT 构建。原始收据、历史日志身份、原版反汇编及独立清单核对保存在 `P15-20261116-stun/`，本次 STUN 收据为 `verification/receipt.json`，清单核对为 `manifest-verification.json`。P15 沿用 `P14-20261116-keyboard/delivery/`。

## GUI 键盘操作修复（2026-11-16）

P14 在当前 P13 交付程序上验证焦点后投递键盘消息，实际复现回车不能打开“选择实例”；自绘按钮只返回 `DLGC_BUTTON`，没有按钮激活语义。进一步回归复现 Tab 停在只读多行日志框；原生删除/还原“是/否”框也无法用 Esc 取消。修复只涉及 GUI：补充自绘按钮键盘语义，保留其绘制类型，释放只读多行控件对 Tab 的捕获，并将两种维护确认框改为默认“取消”的确定/取消框。

`tests/test_gui_keyboard.py` 使用停库实例的独立副本，核对真实焦点后把按键投递到 GUI 消息队列，经产品消息循环、`IsDialogMessage` 和原生控件执行，没有直接伪造按钮通知。最终收据 `probes/20261115T220518540267Z/gui-keyboard/receipt.json` 通过以下路径：

- Tab 正向遍历全部 28 个当时可见且可用的 Tab 控件，包含只读日志及详情；方向键更新服务选择和详情、筛选结果；回车折叠和展开服务组。
- 回车打开实例选择器，Esc 取消且保留原路径；原生筛选下拉框的方向键/F4/Esc、开关的空格操作通过。
- 删除和还原确认框默认取消，分别以回车和 Esc 取消后，实例完整保留，冷数据身份不变，没有 `restore.pending`。
- 回车执行数据库启动，原 SQL 身份/TLS检查通过，再以回车正常停库；GUI 退出 0、无强制清理，复制来源的数据身份不变。
- 遍历和操作后全部按钮的自绘/开关类型保持不变，截图保留当前配色。中间实现曾被 Windows 默认按钮切换改回灰色普通按钮，已通过处理 `BM_SETSTYLE` 修复，未列为交付。

环境仍为 Windows 10 x64 10.0.19044、LLVM-MinGW 20261104 MSVCRT；这是队列键盘消息的真实 GUI 验证，未实测物理键盘、Shift+Tab 或其他操作系统。Esc 取消当前原生弹窗，不为控制台新增直接退出路径。诊断探针、失败收据和构建保留于外部 `P14-20261116-keyboard/`。

最终 GUI 的长路径完整回归也通过新建、同实例锁、channel/relay/monitor/STUN 协议、直接长名 CLI SQL、game 自然初始化（30.938 秒）、协调备份、确认后影子还原、UUID/TLS 和正常关闭；原始收据为 `probes/20261115T220539967264Z/gui-long-path/receipt.json`。无运行时异常或强制清理，只有已知 inotify 291/332 缺口。

普通中文/空格路径的完整实例回归通过资源故障拒绝、双实例、数据库读回、删除取消与实际删除、辅助协议及 game/DP2/Frida 自然启停（30.382 秒），资源前后哈希一致，GUI 退出 0、无异常或强制清理，仅有已知 inotify；收据为 `probes/20261115T220759965547Z/instance-workflow/receipt.json`。最终交付位于 `P14-20261116-keyboard/delivery/`，程序、资源和清单与实际回归的 `package5/` 字节一致，仅更新随包文档；宿主、数据库工具和固定 ZIP 均沿用 P13。

## 长路径实例修复（2026-11-16）

P13 复现普通 271 字符根目录无法加载服务清单；加扩展前缀后虽能加载 19 项，子进程仍因长工作目录报 Win32 267。官方 MySQL 5.7.44 使用显式应用路径、短工作目录及绝对配置后，仍拒绝本次长 `datadir`。同一目录已有的 NTFS 短名可正常完成数据库 SQL、身份和 TLS 检查，因此修复集中在共享路径入口与文件夹选择器，没有更换数据库或扩展运行时。

- `tests/test_gui_long_path.py` 在当前 NTFS 卷创建 274 字符目录，从固定 ZIP 新建完整实例；显示保留长名，长短名文件身份一致，两个窗口持有同一实例锁，19 项清单正常加载。
- 原版 game 自然初始化 31.148 秒，channel、relay/monitor、STUN 协议通过；直接向 `mysql-control` 传入原长名的 SQL 查询也通过。
- GUI 协调备份完成自然 game 退出、Frida dispose 与 DP2 unload；经实际确认框执行影子还原，提交后重新启动数据库核对 UUID/TLS，再正常停止。GUI 退出 0，无运行时异常或强制清理，仅有已知 inotify 291/332 缺口。最终脚本原始收据为 `probes/20261115T214400854554Z/gui-long-path/receipt.json`。
- 当前维护程序的 `test_mysql_maintenance.py` 通过全库逻辑备份、影子还原、损坏备份拒绝、导入失败保留、三种目录切换窗口恢复、账号/存储程序/事件/触发器往返及所属子进程退出；源数据身份不变。收据为 `probes/20261115T213925033826Z/mysql-maintenance/receipt.json`。其中中断恢复是构造的磁盘状态，所属进程实验是刻意终止 wrapper；不计作掉电实验或自然停服。
- 普通中文/空格路径的 `test_instance_workflow.py` 重新通过资源缺失/损坏拒绝、新建双实例、数据库保存读回、实例占用、删除取消与完整删除、辅助协议及 game/DP2/Frida 自然启停（30.332 秒）；GUI 退出 0，无运行时异常或强制清理，只有已知 inotify 缺口。收据为 `probes/20261115T214618299902Z/instance-workflow/receipt.json`，资源前后哈希一致。
- 对 345 字符、不存在且无法取得短名的目标，CLI 在写入前退出 1 并给出明确路径错误，没有创建目标。未测试禁用短名的卷、任意子目录深度或全部 UNC 场景，不修改系统短名设置。

原始失败与中间夹具错误继续保留于 `P13-20261116-long-path/`：首版文件夹选择器显示短名，已修复后重新跑完整流程；另有漏点还原确认框、快速探针等待错误状态文本的问题，均不作为产品通过依据。最终程序未修改原版 ELF、数据库程序和固定 ZIP；宿主沿用已验证的 P9/P11 程序。

最终交付为 `P13-20261116-long-path/delivery/`，GUI、维护程序、宿主、ZIP 与 `resources.ini` 均与上述实际回归使用的 `package2/` 字节一致，仅更新随包文档。阶段收据和源码/程序/验证记录哈希保存在该外部阶段目录。

## 当前客户端实际启动（2026-11-16）

P17 使用 P16 当前产品包与独立冷实例副本，在同一 Windows 10 x64 10.0.19044 上重试；32 位客户端可执行文件及原版 PVF 的哈希与 P12 副本相同。此时自写 D3D9 探针已能枚举一个适配器并创建窗口化设备，全屏设备仍返回 `0x8876086c`；这只说明探针结果变化。客户端副本保持 `mode=fixonly`、`simplified_pvf=false`，并将 `skip_script_hash_report` 设为 `false` 以保留校验。重新生成测试 RSA 配对，只在服务副本使用测试公钥及合成账号，没有修改原客户端、固定资源或认证逻辑。

本轮实际结果如下，均不计为客户端业务通过：

- 首次普通启动创建了主窗口，后续无响应；正常关闭请求无效，确认副本进程路径后强制清理，保留的真实句柄记录退出 1。
- RDP 会话变为 Active 后再次普通启动，出现“公告”错误框，截图未呈现错误正文。该进程自行以 `0xC0000005` 退出；Windows Application Error 1000 的进程 ID 与副本路径一致，记录故障模块 `ntdll.dll` 版本 `10.0.19041.1288`、偏移 `0x5f583`。该模块位置不能独立证明堆、显卡、插件或运行时的具体根因。
- 另一次调试启动没有进入主窗口，超时后强制清理；没有得到可用于确定根因的崩溃现场，不将其作为普通启动结果。未观察到客户端认证完成，未验收建角、城镇、副本或冷重启存档。

服务自然初始化 31.151 秒；客户端尝试结束后经 GUI 正常整体停服并退出 0，无服务端运行时异常或强制清理，仅有已知 inotify 291/332 缺口。测试公钥已恢复，冷副本来源的数据身份不变。当前不增加 ELF 补丁，不重复已通过的 GUI/容量检查；下一步需要先解决客户端普通启动的实际故障，再继续业务验收。

P17 证据位于 `P17-20261116-client-retry/`，普通启动异常为 `client-exit-5144.json`、`client-crash-5144.json`；服务原始收据为 `probes/20261115T225749761511Z/client-business/server-receipt.json`。该阶段 `delivery/` 只更新随包使用说明，程序、资源及 `resources.ini` 与实际使用的 P16 包字节一致。原客户端及 P12 来源副本的四个关键文件哈希均未改变；客户端、账号、密钥、截图和私有日志只保留在仓库外。以下 P12 记录保留为先前环境的历史事实。

P12 使用 P11 最终产品包、独立服务实例和用户客户端的完整副本。环境为 Windows 10 x64 10.0.19044；32 位 `DNF.exe` SHA-256 为 `246b838163fa7c71aa0445fa4aafde04e7ac72aa72855a4b2a7eaeed5eeae264`。客户端副本使用与固定服务匹配的原版 PVF，配置为 `mode=fixonly`、`simplified_pvf=false`，未将 jbz69 定制 PVF 的兼容计入本轮。

- 服务副本自然初始化 31.034 秒。测试只在副本库创建合成账号；客户端原有 gate 私钥与固定服务公钥不匹配，另用 Windows CAPI 生成新的测试用 RSA 配对，仅替换服务副本公钥并正常重启 game。token 采用原版字段布局和 RSA PKCS#1 type 1 运算，没有跳过认证，也没有使用历史私钥。
- 实际启动客户端后，在认证前弹出“图表系统建立失败”。同一会话的自写 32 位 D3D9 探针显示 `Direct3DCreate9` 成功、适配器列表为空，进程位于已断开的 RDP session 2。系统显卡型号存在不能代替当前进程可枚举适配器。恢复有可用显卡的桌面后应先复查探针，再继续客户端业务；当前未到认证，不能判断 token 已通过或服务端业务已通过。
- 通过客户端对话框实际按钮关闭，最终进程已不存在，未强制结束；未保留最终退出码，不记录为退出 0。服务正常逆序停止、GUI 退出 0，无运行时异常或强制清理，仅有已知 inotify 291/332 缺口。服务副本测试公钥在停服后恢复。
- 原客户端 `DNF.exe`、`ijl15.dll`、`Script.pvf` 与 `GameSettings.toml` 前后哈希一致，未运行或修改原目录。合成账号、测试私钥、客户端副本和私有日志全部保留于仓库外，未进入 Git。

原始服务收据：`probes/20261115T210911437839Z/client-business/server-receipt.json`；图形失败、独立 D3D9 实验、公钥恢复及原客户端身份记录位于 `P12-20261116-client-business/`。这是一次被图形环境阻塞的客户端尝试，完整认证、建角、战斗、退出与冷重启存档仍未在当前版本验收。

## Frida 热加载与旧实例升级（2026-11-16）

P11 使用 P9 最终交付的 GUI 与双架构宿主，未改运行时代码或固定资源。环境仍为 Windows 10 x64 10.0.19044、LLVM-MinGW 20261104 MSVCRT；不能外推到其他操作系统。

- 新增 `test_frida_reload.py`：从仓库 ZIP 释放独立副本，以自写 Lua/JS 夹具验证原版 Gadget `on_change=reload`。覆盖写入后 5.368 秒、临时文件原子替换后 5.014 秒，Lua 均收到新版 JS 的实际 RPC 返回值；正常按夹具预期退出 42，无异常或强制清理。只允许现存 inotify 291/332 缺口，其他运行时缺口使测试失败。收据：`probes/20261115T204835098486Z/frida-reload/receipt.json`。这是 game 自然启动前的接口验证；证明当前 Gadget 的轮询回退可用于这两种保存方式，没有实现通用 inotify。
- 旧 `i686/`、`x86_64/` 布局实例的独立副本完成 19 项列表、两轮 15-peer 协议、单服启停、哈希拒绝、逆序停服、协调备份及正常关闭；`runtime_faults/runtime_gaps` 均为空，无强制清理。收据：`probes/20261115T204801243691Z/gui-services/receipt.json`。该脚本未运行 game 或安全拓扑。
- 另一个旧布局副本完成 game 原生初始化（30.932 秒）、整体正常停服和插件释放，日志包含 `main() exited OK`、Frida dispose、DP2 unload；所有服务无运行时异常或强制清理，只剩 game 的 inotify 291/332 缺口。原生日志在本次运行前移开，避免以旧启动标记误判。收据：`probes/20261115T205212232098Z/legacy-startup/receipt.json`。
- 测试前后原实例的数据库及频道配置目录身份相同，原清单哈希与两轮副本收据一致；仅运行独立副本。身份记录、验证副本和源码哈希位于 `P11-20261116-reload-upgrade/`，升级与冷副本回退步骤见使用文档。未实测其他自定义数据库版本，也未将副本验证计作客户端业务通过。

## 原版安全组件诊断（2026-11-16）

P10 用当前 x86 宿主和固定原版 zergsvr、gunnersvr、secagent 实际运行。三个组件均通过原版 loader、用户库及共享 mmap pipe 初始化，未出现 syscall 缺口、访问异常或强制清理；完整安全链仍失败，具体阻塞如下：

| 原始配置下的组件 | 实际结果 |
| --- | --- |
| zergsvr | 向配置服务取信息失败，`init_instance fail. ret=290010039`，退出 183 |
| gunnersvr | 连接 `127.0.0.1:9000` 的安全监控失败，`init_app fail. ret=5092`；尽管退出码为 0，初始化仍失败 |
| secagent | 原生日志记录游戏通道、游戏数据及 DPSDK 配置加载成功；缺少 `cfg/restrict.txt`，限制功能关闭，不能据此认定安全业务通过 |

原始配置收据：`probes/20261115T203912478274Z/security/receipt.json`。只在新诊断副本将 zerg 模式由 `cfgsvr` 改成原版支持的 `cfgfile` 后，原程序拒绝 `svcid.xml` 重复条目（`ret=5016`，退出 152）；收据：`probes/20261115T204117221564Z/security/receipt.json`。再仅删除三条字节相同的重复条目后，三个进程进入循环并正常停止，但 `[31|570001]` 安全监控与 `[30|570011]` zerg 均指向 9000，形成自连；`[2|1]` 配置端点 9001 无实际对端，日志仍有未认证 peer 超时及通道发送失败。该轮 `connect monitor succ` 和进程退出 0 均不能作为安全协议通过，收据：`probes/20261115T204227444589Z/security/receipt.json`。

固定 ZIP 不含这套安全组件对应的独立配置中心/监控可执行资源；现有 DNF monitor 不能冒充其角色。没有修改仓库 ZIP、产品默认配置或引入参考工程组件。诊断脚本、配置差异、原版反汇编及收据汇总位于 `P10-20261116-security/`。下一步需要匹配的原版对端和拓扑，不能用扩大 syscall 实现、自连或默认成功替代。

## game/Frida 信号与文件映射（2026-11-16）

P9 修复来自实际原版路径的三个缺口：Frida 注册 `0x44000004`（SIGINFO/RESTORER/NODEFER）被拒；验证码字体 `/home/neople/game/captcha_font/SeoulHangangL.ttf` 的 9093120 字节只读私有映射被送进 ELF 加载器；连续内存的 munmap 横跨多条匿名记录被拒。触发收据为 `probes/20261115T202233188700Z/startup-compat/receipt.json`，保留原始诊断。运行环境为 Windows 10 x64 10.0.19044，工具链为 LLVM-MinGW 20261104 MSVCRT。

- 双架构信号夹具通过普通自动屏蔽、NODEFER 同信号嵌套、NODEFER 加显式 action mask 的延后投递和逐层掩码恢复；原版 glibc sigaction/raise/restorer 与四种跨进程架构组合继续通过。
- 双架构 VM 夹具通过跨两条真实相邻 Windows allocation 的部分/完整解除、两端保留页、重复空洞、非对齐及溢出拒绝；八线程 VM 并发继续通过。
- 双架构文件/maps 夹具通过只读 fd 的私有 COW、视图间写隔离、不回写文件、不移动 fd、4 KiB 偏移、EOF 页尾零填充、部分解除和关闭 fd 后的身份；普通文件执行映射、EOF 外整页及坏 fd 均明确拒绝。原有共享视图和跨进程可见性继续通过。
- 原版 game/DP2/Frida hook 与双向 Lua/JS RPC 正常退出 42，无强制清理；收据为 `probes/20261115T202814737760Z/game-fixture/receipt.json`。只剩 inotify 291/332 的 ENOSYS，signal 和 munmap 缺口消失。
- 独立副本的 GUI 整体启动、game 原生日志初始化和逆序自然停服通过，game 初始化 30.983 秒，保留 `main() exited OK`、Frida dispose、DP2 unload；所有服务无运行时异常或强制清理。收据为 `probes/20261115T202902393057Z/startup-compat/receipt.json`，仅剩 inotify 两项缺口，字体 mmap ENOSYS 消失。
- 最终宿主的 GUI 完整回归通过两轮 channel/relay/monitor/STUN 协议、单服启停、哈希拒绝、19 项分组/筛选、逆序停服及协调备份，正常关闭且无运行时异常/缺口或强制清理；收据为 `probes/20261115T203202599925Z/gui-services/receipt.json`。Frida item reset 与既有共享 msync 另行通过；这轮 GUI 专项没有运行 game 或完整安全拓扑。

阶段程序、源码哈希及验证副本位于仓库外 `P9-20261116-game-compat/`。开发中曾将数据文件的 EOF 长度检查误用于含 BSS 的 ELF 预留，造成 libstdc++ 加载失败；该中间失败保留在 `probes/20261115T202745817856Z/game-fixture/receipt.json`，最终实现将两条路径分开。没有修改原版 ELF 或资源 ZIP；同步异常信号转交、备用信号栈、通用 SIGBUS、inotify、完整客户端及跨系统矩阵仍未完成。

## DBMW/manager 停服修复（2026-11-16）

在 Windows 10 x64 10.0.19044、LLVM-MinGW 20261104 MSVCRT 上，已从固定原版 ELF 确认 P7 停服访问异常的原因：原版 Free/析构没有 join 网络工作线程，TCP 的实际退出标志又与基础 stop 写入的字段不同。`service.c` 仅在两个已核验哈希的执行副本上补充停止、取消 DBMW 阻塞接收、等待自然退出和原版 guest join，再继续全部原有清理。身份、旧字节和恢复方法见[实现说明](architecture.md)，只读参考与独立实现边界见[来源记录](third-party.md)。

- 双架构宿主构建和固定资源打包通过。原始指令/虚表、构建收据及本轮结果保存在外部 `P8-20261116-service-stop/`。
- `test_service_shutdown.py` 的六轮通过：manager 和三个 DBMW 在三类网络线程刚创建时立即停止，另两轮等待稳定运行后停止；每轮均完成 accept/TCP/UDP 三个真实 join，保留原版 `Application Stop!` 和退出码 1。无运行时异常/缺口、无强制清理，磁盘 ELF 不变。收据：`probes/20261115T200717827812Z/service-shutdown/receipt.json`。
- `test_gui_services.py` 完整通过 19 项分组/筛选、两轮 channel/relay/monitor/STUN 协议、单服停止/重启、日志与 ELF 哈希拒绝、逆序整体停服、数据库协调备份和正常关闭；26 项检查完成，`runtime_faults/runtime_gaps` 为空，无强制清理。收据：`probes/20261115T200853543048Z/gui-services/receipt.json`。
- `test_instance_workflow.py` 完成新建双实例、数据库保存/恢复、资源拒绝和删除保护、15 个辅助服务协议，以及 game/DP2/Frida 自然启停；game 启动 30.264 秒，所有服务无访问异常和强制清理。game/Frida 仍有 `rt_sigaction`、文件 mmap、部分 munmap 与 inotify 的 ENOSYS，不能称为零兼容缺口。收据：`probes/20261115T201137905036Z/instance-workflow/receipt.json`。
- 回归现在精确读取退出码，避免将 126 误匹配为 1；停服适配明确失败也计入集成异常。首轮 UDP 关闭入口错误及随后诊断行交错造成的专项失败均保留原始现场，未写成通过。

这些结果证明上述原版服务的网络线程退出与对应 GUI 路径；完整客户端、安全链、生产负载和 Win7/11 仍不在本轮验收范围。

## robust mutex 退出恢复（2026-11-16）

双架构独立夹具通过 owner death 原子更新、真实等待者唤醒、负 futex_offset、链内/链外/已解锁 pending 节点，以及错误长度、坏指针、环形链表和 PI 拒绝诊断。进一步使用固定资源内原版 glibc 2.17/libpthread，真实 pthread 持有 robust mutex 后退出，另一线程得到 EOWNERDEAD，修复后可重新持锁；放弃修复后得到 ENOTRECOVERABLE。双架构 futex/TLS/VM 并发和诊断回归通过，原版 game/Frida hook 与双向 RPC 正常退出，无强制清理，robust list ENOSYS 消失。收据在外部 `P4-20261116/robust-receipt.json`。未将结果扩展到 PI 或跨进程 futex。

## proc maps 生命周期（2026-11-16）

双架构文件夹具验证了 PROT_NONE、逐页 r/w/x 变化、部分 munmap 的空洞、全部释放后消失，以及共享文件的偏移、中文路径和关闭描述符后的 inode。八个 guest 线程进行映射/权限/释放操作时，主线程反复读取 maps 快照；双架构 VM 与并发检查通过。原版 game/DP2/Frida hook 和双向 RPC 正常退出 42，无强制清理。环境仍为 Windows 10 x64 10.0.19044；收据及哈希保存于外部 `P4-20261116/maps-receipt.json`。这些结果不代表任意 ELF 或客户端业务通过。

## P5/P6 交付链历史脚本结果（2026-11-16）

最终源码宿主、GUI、数据库维护程序和统一资源包已在外部 `P6-20261116-madvise3/package/` 生成。新实例回归通过中文/空格路径双实例、资源缺失和损坏拒绝、存档保持、实例锁、取消删除与 reparse point 保护，15 个辅助服务的 channel/relay/STUN 协议，以及 game/DP2/Frida 自然启动和正常关闭。资源编码往返、19 项固定映射、eventfd、文件/maps、双架构 `SA_SIGINFO`/`rt_sigreturn`、TLS、VM 并发、匿名 `MADV_DONTNEED` 保留映射、诊断、robust mutex、Frida item reset 和 Lua/luv timer 均通过；未发生强制清理。

固定停库副本的 MySQL 维护回归通过完整逻辑备份、损坏备份拒绝、失败导入现场保留、影子还原字节核对、账号/例程/事件/触发器、冷快照和 rename 窗口恢复；源数据未改变。GUI 服务专项完成 19 项分组/筛选、15 个 peer 协议、单服停止/重启、真实日志、ELF 哈希拒绝、逆序停服、数据库身份和备份流程，全部正常关闭；最新收据的 `runtime_gaps` 为空，且未发生强制清理。最新专项收据为外部 `probes/20261115T165539418613Z/gui-services/receipt.json`；较早的 `probes/20261115T164114515975Z/gui-services/receipt.json` 保留了 `rt_sigaction` 缺口，不能继续代表当前源码。实例和数据库收据仍分别为外部 `probes/20261115T161134156596Z/instance-workflow/receipt.json` 和 `probes/20261115T161409615934Z/mysql-maintenance/receipt.json`。

## eventfd 与插件路径（2026-11-15）

固定 game/Frida 实际调用的 eventfd/eventfd2 已独立实现。双架构计数读写、溢出拒绝、信号量、阻塞跨线程通知及 poll/epoll/select 回归通过；原文件、网络及并发关闭回归保持通过。最终宿主运行原版 game、DP2 和 Frida hook/双向 RPC 夹具正常退出 42，无强制清理，未再出现 eventfd ENOSYS。

GUI 插件专项 `20261115T155020067625Z` 使用前一版 eventfd 宿主完成 Lua-only/Frida 开关、真实模块观测、双向 RPC、关闭插件后 game 自然启动（30.890 秒）、原版辅助协议、正常关闭及源数据库不变；最终 select/中断细节由原语与最终 game 夹具验证。12 次 relay 独立启动重连未复现旧崩溃，不能声明根治。inotify 和部分 signal/VM 语义仍有明确缺口；非 PI robust list 已由后续专项补齐。完整收据见外部 `P4-20261115/eventfd-receipt.json`。

## 完整资源目录（2026-11-15）

新资源基底 v4 清单与新建实例统一为 Linux 根目录树，取消顶层 i686/x86_64；657 个资源成员无路径冲突，全部 ELF、库、密钥、PVF、插件和种子原字节保持不变，仅服务模板 root 改为点号。父提交与外部 resource-layout-20261115/original 保存恢复来源；不移动已有实例。

当前新包在 Windows 10 x64 10.0.19044 上通过资源缺失/损坏拒绝、中文空格路径双实例创建、存档保持、锁与删除保护，15 个原版辅助服务的 channel/relay/STUN 协议，以及 game/DP2/Frida 自然启动（30.348 秒）和正常停服，无强制清理。实例事后核对确认 19 项共用根路径、游戏 ELF/PVF 和两种加载器完整。19 个映射与六种输入拒绝、12 种编码字节往返及 121 ELF manifest 生成通过。原始收据在外部 `resource-layout-20261115/` 与 `probes/20261115T153606931567Z/`；不扩展为客户端或完整安全链通过。

## ELF 诊断实现进度（2026-11-15）

当前双架构已通过真实 guest 访问异常、符号/寄存器/指令/栈帧诊断、重复 ENOSYS 计数夹具；19 个原版服务映射与六种错误拒绝、入口、TLS、八线程 VM 和 futex 回归通过。固定资源全部 121 个 ELF 的离线 manifest 已生成。线程退出使用单次内核日志写入，避免 CRT 日志与 exit_group 并发导致的退出故障。

带诊断宿主的 GUI 回归 `20261115T152941288208Z` 完成两轮 channel/relay/STUN 协议、逆序停止、协调备份和正常关闭；整轮最终因新暴露的 ENOSYS 记录而失败，不能写成零缺口通过。记录包括 glibc robust list 与 signal flags 等回退路径；这些以前存在但没有统一日志。旧 relay 崩溃本轮未复现，尚未证明修复。清单与收据见外部 `P3-20261115/`。任意缺失导入的寄存器现场及无帧指针的完整栈展开仍未完成；proc maps 页级生命周期已由后续专项补齐。

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
| relay 并发容量 | `python -B tests/test_relay.py --host HOST_X86_EXE --concurrent-clients 1000 --load-seconds 60 --reconnect-rounds 3` | 固定 1000-client 配置的注册、转发环、10% 连接替换、EOF 确认和恢复；持续时间可指定，不含 monitor/game/client 验收 |
| Frida 热加载 | `python -B tests/test_frida_reload.py [--host HOST_X86_EXE]` | 固定原版 Gadget 的覆盖写入/原子替换、实际新版 RPC 结果；不含 game 自然启动或客户端业务 |
| 编码 | `python -B tests/test_resource_encodings.py --package ../x/dnf-native-output/package` | ZIP 内实际 GBK、GB18030、BIG5、BIG5-HKSCS、EUC-KR、UHC 与 UTF-8 字节往返 |
| 真实实例与 core | `python -B tests/test_instance_workflow.py --gui ../x/dnf-native-output/package/DNF-Native.exe` | 真实资源故障拒绝、双实例、占用、删除、15 个辅助服务协议及 game/DP2/Frida 启停 |
| 长路径实例 | `python -B tests/test_gui_long_path.py --gui ../x/dnf-native-output/package/DNF-Native.exe` | 274 字符根目录的新建、同目录锁、原版启动/协议、CLI 与 GUI 维护；要求文件系统已有可用短名 |
| GUI 键盘 | `python -B tests/test_gui_keyboard.py --gui PACKAGE_EXE --instance PRIVATE_STOPPED_INSTANCE` | 独立副本的焦点/Tab、方向键/回车/空格、弹窗取消和数据库启停；投递队列键盘消息，未实测物理键盘 |
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

- **当前客户端业务**：P12 客户端在认证前失败，同会话 D3D9 适配器为零；P16 末复查已恢复一个适配器且窗口化设备创建成功，将继续独立副本业务尝试。图形探针通过及历史业务结果均不能代替当前客户端验收。
- **长期运行与容量**：P7 已修复 relay 描述符重复分配；P16 已补齐固定配置的 1000 连接容量并验证持续转发、替换和恢复。生产玩家负载和长期运行仍未验收。
- **Frida 文件监视接口**：i386 `inotify_init` / `inotify_init1`（syscall 291/332）仍未实现；P11 已实测当前 Gadget 轮询回退可完成覆盖写入及原子替换后的热加载与新版 RPC。该需求已有可复现路径，通用 inotify 及所有脚本/编辑器组合仍未支持。
- **jbz69 定制 PVF**：原服务 PVF 为 48984，客户端定制 PVF 为 66282；未找到匹配的原版 Linux ELF。缺失模板、BIG5/UTF-8 编码及资源字段差异仍阻塞 Loading Items。模板别名、临时 iconv 映射和 `.str` 重编码实验均未形成可交付兼容结果；认证公钥加载成功不能解释成定制 PVF 可用。
- **安全与负载**：P10 已验证原版安全组件加载，完整拓扑被缺失的配置中心/安全监控对端及现有配置冲突阻塞；生产负载、所有插件组合和完整客户端矩阵没有验收。1024 描述符及 64 线程槽等限制见[实现说明](architecture.md)。
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


## relay 并发描述符故障定位（2026-11-16）

针对固定原版 `df_relay_r` 的偶发访问冲突，增加了默认关闭的 `DNF_NATIVE_EPOLL_TRACE=1` 定向日志，记录 epoll 创建、注册/修改/删除、等待结果和返回事件的 fd/data 关联。

目标环境为 Windows 10 x64 10.0.19044，guest 为 i686 relay ELF；SHA-256 为 `39c87b68408a82f840808c09c520b911c01176349af9eaa3bf071cce2295c3eb`。

- `python -B tests/test_relay.py --reconnect-rounds 3` 通过：TCP 双向转发、分片/合并请求、UDP 断开、无关连接保留和三轮重连均通过。
- 初期实例工作流脚本退出 0；收据为 `../x/dnf-native-output/probes/20261115T191758019436Z/instance-workflow/receipt.json`。当时脚本尚未汇总所有服务的未处理异常，不能据此证明全部服务正常退出。
- GUI 服务回归中的 channel、relay/monitor、STUN、ELF 哈希拒绝和 relay 重启通过；`dbmw_guild` 缺少 `Application Stop!`，其 stderr 实际保留了 `CTcpHandler::GetEventPtr()` 访问异常，因此该次完整 GUI 回归失败。收据为 `../x/dnf-native-output/probes/20261115T192119233108Z/gui-services/receipt.json`。
- 上述早期轮次未复现崩溃，不能单独证明故障修复。

随后使用诊断包 `P7-20261116-epoll-trace-final/package` 的实例回归再次失败，收据为 `probes/20261115T192913835376Z/instance-workflow/receipt.json`。失败发生在辅助协议和 game 启动前，GUI 正常退出、无强制清理。relay 原始诊断显示两个线程的 epoll 创建都返回 `epfd=3`，随后同一 epoll 内同时出现 listener data `0x4` 和 `0x5`；TCP 线程在 `TManager<RelayService>::getManager()+0x6`（PC `0x805b91a`）把 `0x5` 当作对象指针读取而崩溃。共享描述符分配未同步，使独立 reactor 的事件集混合，已具备具体根因证据。

修复统一了 epoll、socket、文件、管道和 procfs 的描述符预留；eventfd 保持在同一临界区分配。并发夹具还复现了 epoll 销毁与其他 fd 关闭时遍历监听表的竞争，已同步这两项操作。未修改原版 ELF、manager 逻辑或增加空指针兜底。

`test_descriptors.py` 使用双架构自建 ELF、8 个 guest 线程和每线程 200 轮混合分配，检查所有存活 fd 唯一、每组 epoll 只返回自己的 eventfd/data、关闭复用，以及文件打开失败和管道分配不足后的预留释放。修复前失败收据为 `probes/20261115T193434126279Z/descriptors/receipt.json`；修复后双架构通过收据为 `probes/20261115T193708425319Z/descriptors/receipt.json`。既有双架构 files、net、ipc、epoll、poll、select/concurrent-close 回归及原版 relay 三轮重连均通过。新包位于 `P7-20261116-descriptor-fix/package`。

新包的实例收据 `probes/20261115T193853281896Z/instance-workflow/receipt.json` 记录了实例/数据库生命周期、channel/relay/STUN 协议和 game/DP2/Frida 自然启停通过（game 启动约 30.525 秒），脚本退出 0、无强制清理。但事后读取全部 stderr，发现 dbmw_guild、dbmw_mnt 和 manager 在停服时仍有未处理访问异常；同时 game/Frida 路径留有明确 ENOSYS。该轮只能认定上述局部路径通过，不能认定完整实例工作流通过或零兼容缺口。原始收据不改写，补充证据见 `P7-20261116-descriptor-fix/integration-postcheck.json`。

GUI 专项收据 `probes/20261115T194156105324Z/gui-services/receipt.json` 再次记录 `dbmw_guild` 停止标记缺失，全部 stderr 同样记录上述三个服务的停服异常。DBMW 固定 ELF 的 SHA-256 为 `0979ef164615e5e3eae006fde74ca2a39a920367dae6a143847dbc0a8d49a967`，PC `0x8057daf` 位于 `CTcpHandler::GetEventPtr()+0x9`；manager 的 PC 为 `0x805494f`。固定 ELF 的 dispatch 指令读取到空 handler；P7 提交时尚未定位销毁与线程退出顺序，后续结论和修复见本文顶部 P8 记录。

两项集成脚本在最终清理后汇总所有服务的异常与缺口：实例脚本遇未处理异常必须失败，GUI 脚本提前失败时也保留 `runtime_faults/runtime_gaps`。P7 以保留现场复验汇总逻辑，两轮均检出三个服务异常，当时没有将现场复验写成整轮通过。P8 已修正停服并重新完成完整 GUI 流程；完整业务和目标系统验收继续独立记录。
