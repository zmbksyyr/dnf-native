# 实施记录

## 2026-11-14 · 客户端认证与数据库兼容

原包 PVF 对照关闭客户端简体转换后，频道文字恢复可读。新实例加入 `system_monitoring_ip = 0.0.0.0`，修复原版接受线程把同机客户端当作监控探针直接断开的配置问题。实际客户端通过原版 RSA 解密和账号登录校验；仅修正私有测试身份的 UID 映射、登录记录和无效资料密文后，角色列表不再断线，能够打开建角界面。

建角尝试继续定位到数据库迁移遗漏：恢复原包 SQL 模式，提供原过程需要的锁定定义者，并建立实际缺失的当前月响应日志表。最终单文件回归、GUI 两实例首次创建、原种子逐字节往返、原存储过程、15-peer 协议与 Frida 数据库接口通过，无强制清理。数据库修正后的客户端复测在登录前出现“图表系统建立失败”，最后退出 `0xc0000005`；建角、进图、结算和存档仍未通过。

审阅程序和 ZIP 已更新，ZIP 为 190330912 字节。临时客户端网络诊断 DLL 已移除，原始输入及原版 ELF 未变。详见[本轮说明](11-client-auth-and-database.md)和[证据](evidence/p10-client-auth-and-database.json)。用户另提供 GUI 参考图，下一阶段按该图调整本项目原生界面，保持真实服务名称与原始日志。

## 2026-11-13 · P1 原址加载基础

本节记录提交 `31793ea` 的历史范围；后续结果见下一节。

用户已授权开始实施，继续采用独立宿主方案和简洁实现纪律。当前进入 P1，G1–G6 均未全部通过。

已完成：

- 从官方 LLVM-MinGW 20261104 MSVCRT 发布包独立获取工具链，校验 GitHub 发布的 SHA-256；没有使用历史工程工具链或参考编译产物。
- 校验原始归档后准备 599 个服务/插件/资源文件和 715 个物理数据库文件的独立副本，未执行安装器或数据库。
- 从零编写 x86/x64 Windows ELF 宿主，使用 BCrypt 校验 SHA-256，通过程序头校验输入边界、架构、入口及加载段。
- 用 Windows 基础内存 API 为 `ET_EXEC` 保留原地址，加载文件内容、零初始化并按页设置权限；重叠页采用段权限并集。
- 在本机 Windows 10 x64（10.0.19044）上，19 个原版 ELF 均完成原址映射，入口与封样清单一致；错误哈希、错误架构、截断输入和越界程序头四项拒绝检查通过。

证据：[工具链来源](evidence/toolchain.json)、[构建命令与源码身份](evidence/p1-build.json)、[映射与拒绝检查](evidence/p1-map-checks.json)。生成物位于忽略的 `.local/`、`originals/` 和 `build/`。

这些结果仅证明私有初始 `PT_LOAD` 映射和输入拒绝，**没有执行任何原版 guest 入口**。尚未实现解释器启动、Linux syscall/TLS、普通文件 `mmap`/COW/跨进程共享、插件、服务业务、数据库恢复及 GUI；Win7/Win11 尚未验证。不能据此宣布 G1 或 19 服务运行通过。

复现（开发机 Python 3.11+）：

```powershell
python tools/bootstrap.py
python tools/import_inputs.py
python tools/test.py
```

当前宿主仅提供 `--map ELF SHA256`，不会调用 ELF 入口。下一步接入受控执行入口与 syscall 边界，并独立获取 Linux 用户库以验证真实解释器启动；x64 syscall 与 TLS 适配需要经过可靠解码，不进行字节串替换。

## 2026-11-13 · 原版入口、初始 TLS 与 STUN 协议

本节对应提交 `cf08d22`，后续修复见下一节。

本阶段先解决真实启动路径的具体缺口，再集中检查和记录。实际开发环境仍是 Windows 10 x64 10.0.19044；P1 六关均未全部通过。

当前可复现结果：

- **原版 x64 STUN 接口原语通过**：原始 `df_stun_r`（SHA-256 `63d94ee108d3f0b2f0f6aa433a2b97c96438da66447117b66d106750efb97528`）经原版 `ld.so`、glibc 和 pthread，在原地址直接执行。使用原程序已有的 `test` 参数，无需改写 daemon 分支。三个实际 guest 线程通过真实 UDP 完成 50 次往返：2311/2312 类型 `01` 返回客户端 IPv4 和端口，类型 `05` 从 2313 回包，2313 返回请求长度。逐包核对内容和源端口，日志没有未知 syscall。测试结束强制回收，尚不证明正常停服、外网 NAT 或三系统兼容。
- **原版 i386 manager 初始化原语通过**：原始 `df_manager_r`（SHA-256 `74b3adba3c2d376547ee65c53940129bd20af9e48926be3f4facc7d729dd65e3`）使用原版已有的 `manager nofork` 参数，完成 `Application Init`、应用配置和 server table 加载，进入真实 UDP/客户端初始化。配置、表、pid/log 目录均在独立测试根目录。尚未验证管理协议或完整服务就绪。
- 双架构静态样件检查初始栈、UTF-8 argv、BSS、write/writev 错误、brk 跨 4 KiB/64 KiB 增减及清零、Linux TLS 读写与原子操作、信号屏蔽/递送/返回；x64 另核对 syscall 后 RCX/R11、SSE 和 128 字节 red zone。7 个执行用例通过，未初始化 TLS 明确失败的检查保留。
- 原版 `ld.so` 的用法和缺失程序路径各两架构通过，缺失文件实际返回 ENOENT；19 个原版 ELF 原址映射和四项拒绝检查继续通过。

实现范围保持直接：一个进程执行一个服务；C 文件按启动、文件、VM、网络、线程、TLS 和信号职责分开。通过 `--root ROOT INTERPRETER_SHA256 --cwd GUEST_DIR --run ELF SHA256 ARGV0 ...` 进入真实解释器，保留原程序地址与原 Linux 用户库的符号解析/重定位。六个 syscall 参数按各自架构捕获。

指令适配使用固定提交的 Zydis/Zycore。引入的实际原因是 x64 syscall 需要指令边界，TLS 需要保留操作本身的寄存器、标志、原子性；自写完整 x86 解码/编码成本更高。原版 manager 的代码段中实际存在 GCC PIC 跳转表，线性解码会误读数据；当前只识别带 GOT 相对索引、SUB/JMP 和连续合法目标的具体表结构，遇到其他解码失败仍停止。TLS 仅重编码段内存寻址，指令仍由 CPU 执行，未实现指令明确失败；Windows TEB 和真实段寄存器保持给宿主使用。线程通过 Windows TLS 保存各自 guest TLS/适配上下文，未使用 TEB 私有槽。

Linux 库从官方 CentOS 7.9 元数据导入 glibc、libgcc、libstdc++、zlib 和 STUN 实际缺少的 nss-softokn-freebl，共十个双架构 RPM。包 SHA-256 经锁定元数据校验，未执行 RPM 脚本。别名暂以开发副本呈现，完整符号链接语义未实现。[来源与许可证说明](../THIRD_PARTY.md)。

明确限制：

- VM 目前提供初始 ELF 私有映射、brk、匿名页，以及**不可变 ELF 依赖的私有快照**。它不是通用文件 mmap：COW、共享文件、跨进程双别名及部分释放后的地址再使用仍未解决。Win10 上的 Native section API 实测也拒绝未按 64 KiB 对齐的映射，不能把这次服务启动进展计作 G1 通过。
- 指令适配仍依赖可用节表及已识别的静态布局，未证明所有控制流、保护展开、动态代码或 hook 共存。多模块动态 TLS、重入、Win7 调度压力和 Frida 均未通过 G2/G3。
- clone 只实现实际观察到的 pthread 线程标志组合；文件和 VM 槽位当前各有固定上限。通用进程 fork/exec/wait、robust owner death、跨进程 futex 等仍未实现。manager 的标准 `start` 路径会真实遇到 fork 缺口，不能冒充成功；已有 `nofork`/`test` 前台模式的使用不改变这个事实。
- 信号当前只覆盖本进程、syscall 边界的普通 handler、mask 与返回；SA_SIGINFO、嵌套和完整异步中断仍明确不支持。不能据此宣称完整 Linux 信号契约。
- 文件、网络和系统信息仍是当前实际触发子集。manager 本次暴露 stat 旧布局、llseek、madvise、fcntl64、epoll_create/ctl 等缺口；下一步沿这些实际路径补齐。数据库恢复、DP2/Lua/Frida、GUI、离线尺寸和 Win7/Win11 均未验收。

本阶段证据：[构建与 PE 导入](evidence/p1-execution-build.json)、[本次映射回归](evidence/p1-execution-map-checks.json)、[执行原语](evidence/p1-entry-checks.json)、[加载器路径](evidence/p1-ld-checks.json)、[STUN 逐包检查](evidence/p1-stun-checks.json)、[manager 初始化与缺口](evidence/p1-manager-probe.json)。旧 `p1-build.json` 和 `p1-map-checks.json` 仍对应前一映射阶段，不能与当前构建哈希混用。

复现按 README 中的开发检查顺序执行。原输入不变，运行产物和完整日志位于忽略的 `build/`；入库记录只保留命令、哈希、实际路径摘要和必要检查结果。

## 2026-11-13 · manager 前台运行与原版停服

原版 manager 已完成 `manager nofork → manager stop`：第二个原版进程读取 PID 文件、发送 SIGTERM 并删除 PID 文件；目标进程执行原版线程及应用资源清理，自行退出，无强制回收或未知 syscall。原程序启动和 stop 分支均返回 1，记录保留实际退出码，不将其改写为 0。此结果覆盖初始化和停服，管理协议仍未验证。

沿实际失败路径补齐 i386 stat/fstat 旧布局、llseek、fcntl64、匿名可写页 MADV_DONTNEED，以及 socket 的基本 LT epoll。Linux 空集合 select 本可等待，Winsock 却立即返回 WSAEINVAL，曾导致 manager 忙循环；现按真实超时等待。PID 文件的读写共享模式已修正；初始 guest TID 与 PID 保持相等，避免 glibc 缓存 Windows 线程 ID 作为进程 ID；i386 的旧 sigreturn 路径也恢复保存的 handler 上下文。

跨进程 SIGTERM 仅面向本 runtime 的实际进程，经命名 Windows 事件排队，在 guest syscall 边界递送。它不提供任意进程信号、阻塞 I/O 中断、完整 Linux signal frame 或进程派生；epoll 暂只实现 LT socket 子集，ET/ONESHOT 明确返回未支持。通用 mmap/COW/共享内存、插件、数据库及三系统等原有缺口仍保留，P1 尚未完成。

证据：[构建身份](evidence/p1-manager-build.json)、[启停检查](evidence/p1-manager-stop.json)、[执行原语回归](evidence/p1-manager-entry-checks.json)、[STUN 回归](evidence/p1-manager-stun-checks.json)。复现命令为 `python tools/build.py`、`python tools/test_entry.py`、`python tools/test_stun.py` 和 `python tools/probe_manager.py`。开发环境仍为 Windows 10 x64 10.0.19044。

## 2026-11-13 · game 数据库路径与运行时实际缺口

本阶段沿原版 game 的真实失败路径补齐最小语义，并在本机 Windows 10 x64 10.0.19044 重新构建双架构宿主。运行时现在支持 1024 个匿名 VM 区间、初始环境变量、双架构 `clock_gettime`、NTFS 文件记录号形式的 guest inode、`/dev/urandom` 的 BCrypt 随机读取、文件锁、已触发的终端 ioctl 错误、socket `read/write`、poll、TCP_NODELAY、keepalive、IP_TOS、收发超时和超时 `EAGAIN` 映射。匿名 `PROT_NONE` 保留页和部分 `munmap` 洞用页位图区分，重复保护与洞访问按实际状态返回。

独立双架构样件检查通过：匿名 VM 17 页跨 64 KiB、文件身份/硬链接/截断扩展/随机设备、TCP 二进制往返、poll、socket 选项与 100 ms 接收超时；入口、加载器、19 个原版 ELF 原址映射和拒绝检查继续通过。`python tools/test_stun.py` 完成 50 个真实 UDP 回包；`python tools/probe_manager.py` 完成原版 manager 初始化和 `stop` 正常退出。

官方 MySQL 5.7.44 ZIP 已由 `tools/import_mysql.py` 固定 SHA-256、六个工具版本和 Oracle Authenticode 校验；`tools/probe_mysql.py` 在独立副本中复制 714 个物理文件，升级、重启、导出 18 个应用库，导入全新实例后重导逐字节一致。种子和重导均为 977716 字节，SHA-256 为 `d4a3b2021194448730575f50595d7597e38f9b6ff8aaeab080208ef8e84f5198`。三次 mysqld 均正常退出，无强制回收；升级过程保留一条已知 nonfatal `mysql.proc` 错误，后续检查、导出和导入均成功。证据位于忽略的 `build/mysql-probe.json`，脚本不记录凭据。

原版 game（哈希 `da417c5e65918f655afcddb6edfdaf2fa1e77b2f208e5aa397f021fa23b79979`）使用私有数据库副本已完成全部 13 类业务库连接、查询表初始化、`GlobalData`、`HistoryLog`、脚本字符串管理器和 DataManager 资源加载入口，实际进入 Money Limit、地图、怪物、技能和物品加载。当前长 probe 在本机 900 秒观察窗口内仍未完成 game 服务初始化；这项结果是启动进展，不是业务就绪。已知剩余实际缺口包括 `setrlimit(RLIMIT_CORE)`、部分 socket ioctl/选项、普通文件 `mmap(MAP_SHARED)`、原版 DP2 保护库动态展开，以及未验证的 game 后续资源、服务互通和正常停服。

为降低 DataManager 阶段的宿主开销，trap 记录已按地址排序并二分查找，异常处理直接复用命中的记录；未改变原指令地址、补丁字节或 TLS 生成语义。独立 GS 样件已否决“设置一次 GS 后依靠自然 fault 适配动态 TLS”的方案：本机 WOW64 线程切换会改变隐藏段状态，异常返回可能静默读取错误段。DP2 仍保持明确未通过，未跳过保护节检查或伪造加载成功。

当前仍只验证 Windows 10 x64 开发机；Win7/Win11、Frida 双向 hook、完整 Lua/DP2、19 服务互通、GUI、CRT 依赖闭包、离线包体积和最终业务链均未验收。生成物、原始资产、数据库和日志继续留在忽略目录。

本阶段摘要证据见 [runtime、game 与 MySQL](evidence/p1-runtime-game-mysql.json)。其中 900 秒 game 观察使用的宿主哈希与最终回归构建哈希分别记录：最后补充了实际普通文件 TCGETS 的 ENOTTY 返回语义，未将后续构建身份冒充前次长时间观察。完整可复现证据仍由上述工具生成到 `build/`。

## 2026-11-13 · 文件共享映射、TSS 共享内存与目录读取

本阶段提交补齐了 game 实际触发的最小文件路径：i386 `statfs/fstatfs` 的布局和 `/dev/shm` 的 tmpfs 类型、`O_NOFOLLOW` 打开标志、真实 `MAP_SHARED` 文件视图、`getdents64` 目录读取以及 `setrlimit` 查询。共享映射使用 Windows file mapping section，解除映射时释放 view 和 section；普通私有文件映射仍保持 ELF 依赖快照的限定。

独立双架构文件样件通过：映射文件后写入内容，关闭 fd、解除映射并重新打开仍读到相同内容，文件长度为 4096。`python tools/test.py`、`test_vm.py`、`test_files.py`、`test_net.py`、`test_entry.py`、`test_ld.py`、`test_stun.py` 和 `probe_manager.py` 均通过。证据见 [文件、共享映射与 TSS](evidence/p1-shared-files-and-shm.json)。

原版 i386 game 在 Windows 10 x64 10.0.19044 上观察 180 秒，无 syscall gap；较短复现中 TSS 日志出现 `DPBillAdapter reload successful` 和 `In AntibotInterface::Initialize, rc=0`，私有 root 生成 `dev/shm/sec/tss_sdk_bus_1` 与 `dev/shm/TSS_43_1.STAT`。此前唯一目录缺口 `getdents64 nr=220` 已补齐。长观察仍保留 AF_UNIX、部分 socket option 和超长 socket timeout 的诊断；这些路径当前未宣称完整支持。

本阶段仍不等于服务就绪：game 观察由超时清理，业务初始化只记录到本次 probe 实际出现的标记；DP2/Lua/Frida、完整服务互通、Win7/Win11 和最终交付包继续等待后续阶段。
补充复测：修正目录游标后，`20261113T113748756047Z`的 180 秒观察同时记录了 TSS `rc=0`、`Init GlobalData`、`Init HistoryLog`、`InitRDARScriptStringManager`，并进入 `Loading Monsters`；syscall gap 仍为空。长观察仍保留上段列出的网络诊断。

## 2026-11-13 · 超长 socket 超时

本阶段沿 game trace 中实际出现的 `SO_RCVTIMEO/SO_SNDTIMEO=31536000` 秒缺口补齐最小语义。Windows Winsock 的 `DWORD` 毫秒参数无法表示一年，runtime 现在保存超出该范围的 Linux timeval，并在阻塞读写前用 Winsock `select` 等待；可直接表示的短超时仍使用原有 `setsockopt` 路径，非阻塞 socket 不额外等待。关闭 socket 时同步清除记录。

独立双架构网络样件先设置 31536000 秒接收超时并检查返回成功，再恢复 100 ms 验证真实 `EAGAIN` 等待和 TCP 往返；两架构均通过。45 秒的原版 game + DP2 preload 复测中，原有超长 timeout 诊断消失，数据库初始化进入 `All DB Open Success` 和 `Init GlobalData Success`；AF_UNIX、`SIOCGIFHWADDR` 以及 DP2 打包 ELF 节适配仍明确保留为缺口。

证据见 [超长 socket 超时](evidence/p1-long-socket-timeout.json)。本阶段未将一年等待实际阻塞测试，样件在 setter 检查后恢复 100 ms，game 观察也由超时清理，因此不构成服务就绪或最终验收结论。

## 2026-11-13 · SIOCGIFHWADDR 实际网卡查询

本阶段补齐 trace 中确认的 i386 `ioctl(0x8927)`，即 Linux `SIOCGIFHWADDR`。runtime 通过 Windows `GetAdaptersInfo` 取得真实六字节适配器地址，按 Linux `ifreq` 布局写入 `ARPHRD_ETHER` 和 MAC；没有可用硬件地址时返回 `ENODEV`，不伪造成功。构建显式加入系统自带 `iphlpapi`，没有引入第三方依赖。

双架构网络样件均验证 `eth0` 请求返回真实 MAC、地址族为 1，同时继续通过 TCP、poll、短超时和超长超时 setter 检查。带私有数据库的 45 秒 game 复测未产生新的 `SIOCGIFHWADDR` 缺口；该次在更早的 NSS AF_UNIX 路径结束，故 ioctl 样件是本阶段的直接覆盖，不能据此宣称 game 服务就绪。AF_UNIX 和 DP2 打包 ELF 适配仍保留为下一阶段缺口。

证据见 [SIOCGIFHWADDR](evidence/p1-sio-ifhwaddr.json)。

## 2026-11-13 ? `/proc/self/exe` ? packed ELF ????

???? DP2 ??????????? `readlink` ???`/proc/self/exe` ?? guest `run->argv[0]`???????????????? NUL??????? guest ????? Linux ?????i686 ? x86_64 ????????

DP2 ??? `libdp2.so` ?? RWE PT_LOAD ? sectionless packed ELF???????????????? executable section ??????????????? packed ?????? skip???????????????? syscall/TLS ????????????????? DP2 ?????

? DP2 preload ? 10 ???????snapshot failure ? syscall gap ????DP2 ????????? `/proc/self/maps`?????????? maps ??????????????????

??? [readlink ? packed ELF](evidence/p1-readlink-packed.json)?

## 2026-11-13 · `/proc/self/maps` 运行时映射快照

本阶段沿 DP2 preload 的实际失败路径补齐 `/proc/self/maps`。打开该路径时，runtime 根据已加载 ELF 的 `PT_LOAD`、私有 ELF 依赖快照、匿名 VM 区域、heap 和 stack 生成一次只读快照；映射按 guest 地址排序，权限和文件偏移来自 ELF，依赖快照保留 guest 路径。合成文件支持 `read`、`lseek`、`fstat`、`fcntl` 标志查询和 `close`。

双架构文件样件通过，`/proc/self/maps` 能打开、读取到 `/proc-test` 路径并关闭；此前 DP2 的 `cannot open maps` 已消失。完整回归 `test.py`、`test_vm.py`、`test_entry.py`、`test_ld.py`、`test_net.py`、`test_stun.py` 及文件检查均通过。

最新 `python tools/probe_game.py --preload --timeout 3` 继续越过 maps 阶段，随后暴露 `nr=266`、`nr=270`、`nr=331`、`nr=42` 四个 syscall gap，并以 `0xc0000096` 结束；packed DP2 ELF 的适配仍是部分适配。因此本阶段只证明 maps 缺口已解决，不代表 DP2、Lua、Frida、game 服务或最终验收通过。探测未强制清理。

证据见 [proc maps](evidence/p1-proc-maps.json)。开发环境为 Windows 10 x64 10.0.19044；构建、原始 ELF 哈希、命令、强制清理状态和后续 syscall 缺口均记录在证据文件中。

## 2026-11-13 · i386 启动 syscall 路径

本阶段沿 DP2 preload 的最新 trace 补齐 i386 `clock_getres`、`pipe2`、`pipe` 和当前线程 `tgkill`。匿名管道使用 Windows pipe，支持 close-on-exec、nonblock、`FIOCLEX`、`FIONBIO` 和空管道读取的 `EAGAIN`；`clock_getres` 返回 Linux 对应时钟分辨率；`tgkill` 只覆盖当前 runtime 进程和当前 guest 线程，其他目标明确返回错误。`eventfd/eventfd2` 保持明确 `ENOSYS`，让 glibc 使用已观察到的 pipe fallback。

双架构文件、入口、syscall、VM、ELF 映射、加载器、网络和 STUN 回归均通过。最新 `python tools/probe_game.py --preload --timeout 3` 中此前的 4 个 syscall gap 已全部消失，也没有新的 gap 或 unhandled exception；进程运行到 3 秒观察窗口结束后被清理，尚未出现业务初始化标记，因此不代表 DP2、Lua、Frida、game 服务或最终验收通过。

证据见 [i386 启动 syscall](evidence/p1-i386-startup-syscalls.json)。开发环境为 Windows 10 x64 10.0.19044；强制清理、原始输入哈希、构建哈希和未支持的 eventfd 限制均已记录。

## 2026-11-13 · i386 `mremap` 与动态代码页权限

沿带私有 MySQL 的原版 game + DP2 preload 复测，实际缺口是 i386 `mremap`（nr=163），而不是此前观察到的 `/proc/self/fd/8` 或 `/etc/localtime` 打开失败。runtime 为匿名映射补齐同进程可移动扩展、缩小和内容复制，并允许已映射匿名页按真实 `mprotect` 请求切换为可执行权限；只接受本次 trace 出现的 `MREMAP_MAYMOVE` 组合。独立 i386 VM 样件现在同时验证扩展后内容保留和匿名页执行，x86_64 行为保持原路径。

清理后的双架构构建通过 `test.py`、`test_vm.py`、`test_files.py`、`test_net.py`、`test_entry.py`、`test_ld.py`、`test_stun.py` 和 `probe_manager.py`。同一台 Windows 10 x64 10.0.19044 上，game 复测运行约 63 秒、无 syscall gap，继续进入 Lua/Frida 加载；当前下一停止点是 Frida ELF（SHA-256 `8ca458e422dead8081c6e25831050dcbb006794c811bc2fe471f0cb59fdcc239`）section 12 的 Zydis 解码失败，guest 以 2 退出。没有跳过该检查或伪造插件加载成功。

证据见 [i386 mremap 与动态代码](evidence/p1-i386-mremap-dynamic-code.json)。本阶段仍不代表 DP2、Frida、game 服务就绪或最终验收通过；Win7/Win11、完整 Lua/Frida 双向调用、服务互通、GUI 和交付包仍未验收。

## 2026-11-13 · DP2 preload 启动路径与 Frida 可执行页

本阶段沿上一阶段的 i386 game + 原版 DP2 preload 真实启动路径补齐最小缺口。Frida 通过启动栈中的真实 auxv 快照定位 libc；runtime 增加 `/proc/self/auxv` 只读快照、i386 UID/GID 查询、`sched_yield`、普通文件和管道 `poll`、`fcntl64(F_GETFL/F_SETFL)`，并让匿名 `mmap(PROT_EXEC/PROT_RWX)` 保留其真实可执行权限。AF_UNIX socket 在当前 Windows socket 子集之外，明确返回 `EAFNOSUPPORT`。

对当前 Frida ELF（SHA-256 `8ca458e422dead8081c6e25831050dcbb006794c811bc2fe471f0cb59fdcc239`），适配器只按已核对的 section、文件偏移和指令字节跳过 AES 内嵌数据及 PadLock XSHA256 指令；没有放宽一般 ELF 解码失败的处理。

双架构构建及 `test.py`、`test_vm.py`、`test_files.py`、`test_net.py`、`test_entry.py`、`test_ld.py`、`test_stun.py`、`probe_manager.py` 均通过。当前宿主执行 `python tools/probe_game.py --preload --timeout 120`，运行 120.121 秒后按探测器清理，退出码为 1；syscall gap 和 runtime gap 均为空，未出现业务初始化标记。该结果说明启动路径已越过本阶段观测到的缺口并进入 DP2/Frida 路径，不代表服务就绪、数据库验收、插件完整行为或最终交付验收。

证据见 [DP2 preload 探测](evidence/p2-dp2-preload.json)。开发环境仍为 Windows 10 x64 10.0.19044；Win7/Win11、完整 Lua/Frida 双向调用、19 服务互通、GUI、正常停服和最终交付包继续等待后续阶段。

## 2026-11-14 · DP2 后段 syscall 边界复核

沿 `Server key Check OK` 后的原版 DP2 启动路径，以私有 MySQL 执行 `python build/run_game_db.py --preload --trace --timeout 60`。本次诊断延后 30 秒启用 trace，避免启动期日志扰动；诊断改动随后移除。

观察窗口内没有 `syscall gap`、`runtime gap` 或未处理异常。尾段反复读取约 22.5 MB 的 `dp2/lua/df/game/symbols/records.lua`，并调用已实现的 `brk`、`mmap2`、`mprotect`。这些证据只将边界缩小到 DP2/Lua 内部执行，不能证明缺少某个 syscall，因此没有添加猜测性 fallback。

证据见 [DP2 后段 syscall 复核](evidence/p2-dp2-late-trace.json)。该次在 Windows 10 x64 10.0.19044 上观察 60 秒后清理，未达到业务就绪。

## 2026-11-14 · TLS 原生返回与辅助页开销

本阶段确认并减少了 TLS 转接中的重复开销：普通 TLS 指令原先通过第二次非法指令异常返回，现在由原生跳转返回；各 guest 线程使用独占的 4 KiB 可读写执行辅助页，省去每条 TLS 指令的两次页面权限切换。保留有效地址检查、指令缓存刷新和 CPU 原生执行，删除不再需要的 TLS 返回状态，没有新增依赖或缓存层。临时 runtime 采样代码已移除。

新增双架构 TLS 样件检查读写、动态寻址、LOCK 运算及标志、间接调用和跳转的返回语义。三组交替计时中，i386 耗时中位数由 0.358 秒降至 0.194 秒，x64 由 0.172 秒降至 0.112 秒；该结果只代表本机专用样件。完整映射、入口、信号、VM、文件、网络、加载器回归通过，原版 STUN 的 50 次 UDP 响应及 manager 初始化和 SIGTERM 停止也通过。实现提交为 `6622034`。

带私有 MySQL 的原版 game + 原版 DP2 preload 复测运行 240.080 秒，实际映射了 Frida ELF，未产生 syscall gap、runtime gap 或未处理异常，但业务初始化标记仍为空，最终按观察超时清理；MySQL 正常停库。Frida 映射后的外部线程采样仍显示两个活动 CPU 线程，主要位于内存查询和异常处理路径，因此剩余瓶颈尚未解决，不能宣称插件完整可用或服务就绪。下一定位点是 Frida 映射后的具体活动执行链。

证据见 [TLS 原生返回与实际启动复测](evidence/p2-tls-native-return.json)。原始输入哈希已复核，私有数据库及凭据未提交；Win7/Win11、完整 Lua/Frida 双向调用、19 服务互通及交付验收仍未完成。

## 2026-11-14 · 管道 poll 与 Frida 初始化等待

本阶段解决了两个有实测依据的问题。首先，`poll` 把空匿名管道当作普通文件报告可读，导致 GLib/Frida 不断 poll、读取 `EAGAIN` 并重试。runtime 现在使用系统 `NtQueryInformationFile` 查询管道数据、写入额度及关闭状态；管道和 socket 混合等待时每次最多等待 10 ms，并保留原始超时截止。非阻塞管道在 writer 关闭且数据耗尽后返回 EOF。没有新增依赖或通用事件框架，实现提交为 `a132710`。

旧宿主在双架构新样件的第 33 行均复现空管道错误就绪；修复后双架构管道超时、延迟写入唤醒、写满背压、HUP/ERR 和混合 TCP 等待全部通过。现有映射、TLS、入口、VM、文件、网络和加载器回归通过，原版 STUN 的 50 次协议响应及 manager 初始化、SIGTERM 停止也通过。180 秒原版 game + DP2 复测中，映射 Frida 后的 100 次线程采样均未增加 CPU 时间，原先的空转消失，但业务初始化仍未完成。

继续追踪确认：Frida 工作线程在缺少 `HOME` 时通过 `getpwuid_r` 查询用户，NSS 尝试 `dlopen` 并等待主线程持有的加载器锁；主线程同时等待 Frida 初始化。补齐真实 guest 用户的 `HOME=/home/neople` 即解除等待，不需要修改 futex 或绕过加载器锁。game 探测器已默认提供该环境，也允许通过 `--env` 传入 guest 环境，实现提交为 `a0e5e8a`。

同一正式宿主的 120 秒复测实际出现 `Frida Init Stage:early`、`setup success`、`set function success` 和 `frida main, args =`，并进入全部业务库连接成功、查询表、`GlobalData` 和 `HistoryLog` 初始化。后续暴露 i386 `lstat64`、`inotify_init` 和 `inotify_init1` 缺口；该次仍由超时清理，MySQL 正常停库，尚不构成完整 Frida 双向调用、game 就绪或最终验收。

证据见 [管道等待、加载器锁与 HOME 复测](evidence/p2-pipe-poll-readiness.json)。临时诊断均从忽略目录中的源码副本构建，正式 runtime 无诊断改动，宿主已恢复；验证环境仍仅为 Windows 10 x64 10.0.19044。

## 2026-11-14 · lstat 与原版 Frida 脚本重载

本阶段补齐原版 Frida 实际触发的 i386 `lstat64`，以及同一元数据路径的 x86_64 `lstat`。普通文件和目录通过不跟随重解析点的 Windows handle 获取真实身份、大小和时间；Windows junction 等重解析点的 POSIX 元数据暂不支持，明确失败。双架构样件验证普通文件、目录、缺失路径、无效输出地址，以及 `stat` 跟随 junction、`lstat` 明确拒绝的区别。实现提交为 `ae6c8e4`，全部既有回归再次通过。

使用默认 guest `HOME` 的 180 秒原版 game + DP2 复测中，`lstat64` 缺口消失，出现 Frida early setup、函数设置成功及 `frida main`，game 继续到 `InitRDARScriptStringManager Success`。在观察到原版 main 标记后，仅向私有副本的原版 JS 追加一条 `console.log`，1.253 秒内出现该标记及 `Frida Init Stage:late`，证明原版脚本的监控与重新执行路径实际工作。原始 JS、ELF、PVF、字体及其他输入哈希均已复核；没有修改原始资产。

`inotify_init`/`inotify_init1` 仍明确返回 `ENOSYS` 并记录 gap，当前 GLib 的文件轮询路径已满足本次脚本重载验证，因此未增加 inotify 机制。该次无其他 syscall gap、runtime gap 或未处理异常；DataManager 初始化仍未完成，进程在观察超时后清理，MySQL 正常退出。此结果不等于完整 JS/Lua 双向调用、全部 hook、game 就绪或最终验收。

证据见 [lstat 与原版脚本重载](evidence/p2-lstat-frida-reload.json)。本阶段仅验证 Windows 10 x64 10.0.19044；19 服务互通、完整插件语义、Win7/Win11、GUI 和最终交付仍未完成。

## 2026-11-14 · 原版 Lua/JS 双向调用与真实 game hook

实现提交 `3041574` 增加本项目自编 Lua/JS 样件，在原版 game、DP2、Lua、Frida 中执行，两次独立进程均通过。Lua→JS→Lua 的 32 轮嵌套调用严格检查整数、浮点数、字符串、Lua state 指针及返回值；同时检查 `frida.main` 参数和返回值 1401。

样件从真实 `CEnvironment` 实例调用原地址 `0x08106ce0` 的 `get_server_group`，验证 hook 的 onEnter/onLeave、修改返回值、detach、原返回值及原指令字节恢复。实测 `original=0 hooked=64 restored=0`；脚本执行时配置尚未加载，因此基线是实际读取的 0，不能用后续配置值 3 代替。两次均由 guest `os.exit(42)` 正常结束，宿主返回 42，约 27 秒，无强制回收。

证据见 [原版桥接与 game hook](evidence/p2-original-frida-bridge.json)。这验证了 G3 的上述接口子集，不代表完整原版脚本、并发回调、补丁冲突、game 业务或整个 P1/P2 退出通过。默认原版脚本的重载仍由上一阶段证据单独覆盖。

## 2026-11-14 · monitor UDP 停服顺序

原版 monitor 使用私有 loopback 配置补齐自身 `server_str.xml` 后，可完成 Application Init/Load；异常发生在停服阶段。诊断显示主线程调用 UDP `stop` 后立即析构对象，没有调用 `join`，而 UDP worker 仍进入 `RecvFromClient`，在 `0x08050422` 解引用已清空的 handler 指针。单次 SIGTERM 由主线程接收的现场同样复现，因此没有修改 runtime 的线程、futex 或信号分发来掩盖应用顺序问题。

仅加入 join 会等待阻塞的 UDP 接收；worker 还会在启动延时后重新置位运行标志，覆盖提前停止。实现提交 `6263d98` 增加 `tools/probe_monitor.py --fix-udp-stop`：核对原版 SHA-256 和三处旧字节，仅生成私有 ELF 副本，在 pthread_create 前设置运行标志，移除 UDP 延时后的覆盖；停服时清除标志、关闭并置无效接收 fd、join，最后析构。文件长度和原虚拟地址保持不变，省略选项即可运行未修正原件；原始资产始终只读。

诊断现场实际记录到 worker exit/clear_tid、主线程 futex 返回、随后析构。正式宿主四次复测覆盖启动 5 秒和 12 秒后停止，均完整出现停服标记并移除 PID 文件，无异常和强制清理。原版目标在 Application Stop 后返回 1；stop 辅助进程成功发出 SIGTERM 后仍主动抛出 `CDNFException`，返回 134，两个值均如实保留。启动 2 秒后停止仍有超时，未计通过。缺少的 DBMW/manager peer 连接明确被拒绝；未验证注册、心跳或服务就绪。

证据见 [monitor UDP 停服](evidence/p2-monitor-udp-stop.json)。此补丁只覆盖当前哈希原版 monitor 在本机 Windows 10 x64 的已测停服路径，尚未并入产品启动器；其他服务和系统不能外推。

## 2026-11-14 · game 物品加载进度与 20 分钟复测

原版 game + 原版 DP2/Lua/Frida/脚本在私有 MySQL 副本上复测 1200.146 秒，实际越过 `Loading Items Success`，继续完成 Avatar Roulette、RandomOption、Quests、Characters、Disjoints 和 Advance Altar 加载。没有新增 runtime gap 或未处理异常；仍仅记录已知的 inotify 两项缺口。`Init DataManager Success` 尚未出现，游戏按观察时限清理，MySQL 正常退出，不能宣称 game 就绪。

物品阶段由外部只读 Win32 采样定位，实际 guest 调用链包含 `CItemList::load_item_files`、`ImportEquipmentScript`、装备动画处理及字符串分配/析构。再按原版反汇编读取加载函数的迭代器，并独立枚举不可变的装备索引树，核对该表共 72,779 项；采样得到 56 个不同物品 ID，位次从 56,126 推进到 56,461。这证明该窗口仍在推进物品解析，不是此前怀疑的锁等待，因此没有添加猜测性的 futex fallback 或 TLS 缓存。

原版日志先输出 Loading Items 的时间前缀，再执行工作并追加 Success，故该行的 `01:08:42` 不能作为完成时间；后续 `01:25:59` 的新加载行才给出物品完成的上界。证据见 [game 物品加载](evidence/p2-game-item-loading.json)。当前主要未完成项仍为 DataManager 后续初始化、完整业务链及服务互通。

## 2026-11-14 · channel 真实 TCP 握手与接收栈

本阶段沿原版 channel 的真实收包路径解决两个具体问题。实现提交 `386885e` 先补齐 `SO_LINGER`：Linux 的两个 32 位整数转换为 Winsock 的两个 16 位字段，双架构均核对禁用时正常 EOF、启用零秒时 TCP reset。继续发送真实报文后，trace 确认 channel 接受连接时查询 `SO_RCVBUF`；实现提交 `c1d46c8` 补齐 i386 socketcall 和 x64 `getsockopt` 的发送、接收缓冲区大小查询，支持输出长度截断及实际错误返回。容量来自 Winsock，未仿制 Linux 内核缓冲区记账和倍增规则。

查询补齐后仍出现 TLS 间接调用失败。独立诊断构建确认 TLS 基址有效，失败的是栈地址：runtime 报告 512 KiB 的 `RLIMIT_STACK`，glibc 据此给四个 pthread 分配 `0x81000` 字节，而原版 `TCPUser::onRead_` 单个栈帧就需要 `0xa0064` 字节。当前将初始 guest 栈及报告的默认限制调整为 8 MiB，实际 pthread 栈随 glibc 分配；没有删除内存范围检查、修改 TLS 行为或改写原版 ELF。诊断代码仅存在于忽略目录，最终构建使用正式源码。

本项目自编 `tools/test_channel.py` 从原版 ELF 的协议注册、报头、连接回调和 AES 初始化反汇编独立确定报文，使用系统 BCrypt 生成 AES-128 ECB 测试请求，没有使用参考工程源码、加密表或编译产物。原版 channel 在私有 loopback 配置上先返回错误握手结果，再对 32 次正确请求逐字节返回成功，覆盖分开发送一个报头和一次发送两个报文。测试不保存临时协商密钥。结束时通过 runtime 事件请求 SIGTERM，原版 handler 自行返回 0，无强制回收或 runtime gap；`run` 模式不写 PID 文件，此处没有计作原版 PID stop 辅助命令或所有 worker 清理已验证。

最终独立构建通过双架构网络、管道 poll、入口/信号、TLS、加载器回归，原版 STUN 的 50 次 UDP 响应和 manager 初始化、原版 stop 正常退出继续通过。构建和回归先使用 `build/staged/`，避免替换旧 game 长观察持有的原宿主，证据分别记录哈希；旧 game 退出后，正式 `tools/build.py`、`test_channel.py`、`test_net.py` 也已通过。正常复现使用 README 中的检查命令。

证据见 [channel TCP 协议与回归](evidence/p2-channel-protocol.json)。仅验证 Windows 10 x64 10.0.19044；频道信息、bridge 数据库与互通、客户端业务、Win7/Win11 和整个阶段退出仍未通过。relay 和 bridge 已观察到原版网络线程启动，但 bridge 仍含待配置的数据库占位值，这些观察不计协议或业务通过。

## 2026-11-14 · game 频道脚本失败的实际终点

此前后台长观察在 1506.919 秒自行结束，没有超时清理。原版 game、DP2、Lua、Frida 和脚本实际越过物品、等级经验、PVP 等加载，进入频道脚本获取；`01:51:00` 开始后，`01:56:02` 输出 `Init Get Channel Data(channel_info.etc) Fail`。随后日志虽出现 `Init DataManager Success`、`Init GameManager Success` 和 `Init Arad GameScriptManager Success`，原版最终明确报告 `Init ChannelType Fail`，Frida dispose 后以访问异常退出，宿主返回 125。**DataManager 的 Success 文本不能覆盖此前失败，也不能作为完整资源初始化通过。** 私有 MySQL 日志记录正常 Shutdown complete，本次没有独立保存其退出码。

原版反汇编确认 `CDataManager::init` 在 `0x0835d2c3` 调用 `CmmChannelProxy::SendCheckChannelScript`；返回失败会直接跳过频道脚本导入。`GameWorld::_GetChannelType` 在自动类型下按 server group/channel number 查询脚本，找不到记录返回 19，`InitChannelType` 据此返回 false。本次独立 game 配置指向 loopback TCP 7000，运行期间没有 bridge peer，故下一步应补齐真实 bridge/频道脚本链路并复测，不强制频道类型或跳过原版检查。dispose 访问异常仍须单独定位，不能只用缺失 peer 解释它。

证据见 [game 频道脚本失败](evidence/p2-game-channel-type-failure.json)。该次使用旧宿主，不混入本阶段 channel 修复构建的测试结果。

## 2026-11-14 · bridge 数据库与原版 game 频道脚本接口

本阶段先补齐实际缺失的 peer 与配置，没有修改 runtime 或跳过 game 检查。原版 bridge 的 `DBMgr::Mysql_logon` 反汇编确认 MySQL 端口固定为 3306；在该端口空闲时，复制已正常关闭的私有恢复库，逐文件核对副本身份，再配置独立测试账号和 loopback 地址。原版 bridge 实际读取 ch_script_version、ch_gc_info、ch_dungeon_data、ch_dungeon_list、ch_server_data，并返回版本检查和 13,306 字节频道脚本。初次观察的 bridge、MySQL 均正常返回 0，无强制回收；输入私有恢复库的前后身份一致。

自编 `tools/test_bridge.py` 根据原版 game 与 bridge 的报文构造函数、版本回调及脚本回调独立确定请求。它通过官方 MySQL 客户端独立读取期望版本，核对版本不同返回 0、相同返回 1，以及分开发送报头、一次发送两个不同请求；四次脚本响应均与原始 channel_info.etc 逐字节一致。原版响应构造函数未初始化报头后五字节，故仅按其实际设置的 category、ID、size 核对，不把这些未定义字节当作 ACK 或协议内容。

`--game-proxy` 再通过原版 DP2/Frida 调用原版 game 的 CmmChannelProxy 构造、Init、SendCheckChannelScript，实际执行原版连接、版本检查、下载和文件写入。测试临时设置自己的 CEnvironment 端口并恢复，不改原版指令或函数结果；落盘的频道脚本和版本分别与原始文件、数据库值一致，测试 game 自行返回 42，无强制回收或新增 runtime gap，仍有已知 inotify 两项缺口。该接口测试在资源初始化之前主动退出，不能替代自然启动路径或完整 DataManager 验收。完整 game + bridge 联合长观察另行运行，结果尚未写入本节。

接口检查使用另一个私有服务端口 7002，避开长观察使用的 7000；数据库仍为原版固定的 3306。bridge 接收 runtime SIGTERM 事件后自行返回 0，未强制结束。仅实测 Windows 10 x64 10.0.19044；频道脚本导入后的完整 game 初始化、服务业务、Win7/Win11 仍未通过。复现条件及命令见 README，证据见 [bridge 与 game 频道代理](evidence/p2-bridge-channel-script.json)。

## 2026-11-14 · relay 实际转发与 S2S 定向断开

`tools/test_relay.py` 从原版 TCPHandlerRelay::dispatch、relayToTCP、UDPHandlerS2S::dispatch 独立确定协议：TCP 报头为两个 16 位值和两个 32 位账号字段，登记 ID 0、转发 ID 1，原版将转发 ID 改为 3 并保留后续字节。自编的两个测试连接先通过各自的回送确认登记，再逐字节核对 32 次双向二进制转发，以及拆包和连包；随后向 UDP 8200 发送原版 ID 2500、结果 0 的 S2S 报文，指定连接确实关闭，另一个连接仍能回送。

本次使用原版 PVP 模式，TCP 7200 和 S2S UDP 8200；没有把未暴露的 UDP 数据转发路径算作通过。独立副本仅将 max_client 改为 16，原版 ELF 和 runtime 均未修改。结束时请求 runtime SIGTERM，原版返回 0，无强制回收或 gap。尚未验证原版 monitor 对接、PVP 客户端业务、满额负载、全部 worker 清理或 Win7/Win11。证据见 [relay 转发协议](evidence/p2-relay-protocol.json)。

## 2026-11-14 · futex 比较迁移与条件原子唤醒

Frida 失败退出路径的诊断实际发现 futex 操作 132/133，即 PRIVATE 的 CMP_REQUEUE/WAKE_OP 未实现。runtime 现在在原有等待队列锁内比较并迁移等待者，返回实际唤醒与迁移数量；WAKE_OP 对第二个字执行 CAS 原子更新，并根据更新前的有符号值决定第二队列唤醒。实现依据 Linux man-pages 的操作编码、返回值和同步语义独立编写，没有增加依赖。

自编双架构 ELF 使用真实 clone 线程，核对比较失败不改变队列、先唤醒再迁移、条件成立/不成立、五种原子运算、移位、有符号参数及 clear_tid 退出。i386、x64 均正常返回 42；同一正式构建的 TLS、入口/信号、原版 channel 握手和 relay 转发回归通过。仅覆盖本进程地址队列；跨进程 futex、PI 和 robust owner-death 等不在本次实现范围。

这两项补齐没有解决 Frida 退出异常。独立诊断构建显示主线程先 munmap Frida 的 0x17ca570 字节映射，另一个线程随后在模块偏移 0x89e3c 发生执行访问异常；代码页已解除提交，栈仍已提交且可读。补齐 futex 后相同短失败路径仍返回 125。因此只将新操作计作原语通过，模块卸载和线程生命周期仍须定位，没有禁止 munmap 或虚假保留映射。证据见 [futex 队列与退出边界](evidence/p2-futex-queues.json)。

## 2026-11-14 · game 自然导入频道脚本与原版 JS 数据库

补齐 bridge 后，第一轮原版 game + 原版 DP2/Lua/Frida/脚本在私有 MySQL 上运行 1304.942 秒，自然取得频道数据、导入频道脚本，继续完成 ChannelType、GameWorld、Login State 初始化，并输出频道启动标记。随后原版 JS 的独立数据库账号认证失败 1045，在 init_db 的 api_MySQL_exec 调用中报 expected a pointer，game 自行返回 134。该次没有未处理访问异常或强制回收；认证错误出现在退出之前，不足以单独证明返回 134 的全部根因。bridge、MySQL 均正常返回 0，输入私有数据库身份不变。

原版 JS 的数据库账号独立于 game master_db 和 d_taiwan.db_connect。只在私有测试库按原始脚本设置该账号，并授予 d_taiwan.*、frida.* 权限后，通过附加测试 RPC 直接调用原版 init_db，SELECT 1 成功，独立 MySQL 查询确认 frida 中实际创建 data、dgnfinishtime、init_sp_tp 三张表；测试 game 自行返回 42，耗时 26.493 秒。该接口测试提前退出，不能代替原始脚本自然运行。配置该账号的完整复测运行 1293.749 秒，再次自然导入频道脚本并推进到 GameWorld、Login State 和频道启动标记；私有 SQL 日志实际记录 JS 连接及建库、三张表创建语句，1045 和 expected a pointer 均未再出现，但进程仍自行返回 134，终止原因尚未定位。bridge、MySQL 均正常返回 0，输入数据库身份不变。

新增 tools/probe_game_chain.py 将数据库逐文件校验复制、私有账号设置、bridge 实际脚本响应、game 启动观察和正常停库串成可复现命令。它要求已有停止的私有恢复测试库及 game-db.json，不改变输入原件；--script-db 明确选择上述接口测试。probe_game.py 另外保存每实例 receipt，避免后续探测覆盖唯一结果，并允许选择诊断宿主和私有 Gadget teardown 配置。正式整理后的 --script-db 命令独立复测通过，game 返回 42、耗时 26.583 秒，三张表查询通过，bridge/MySQL 均返回 0，源库身份不变。

证据见 [game 频道导入与脚本数据库](evidence/p2-game-channel-init-script-db.json)。原版日志的 Success 或启动标记仍不计客户端或完整业务通过，仅实测 Windows 10 x64 10.0.19044。

## 2026-11-14 · Frida 卸载配置的诊断对照

独立诊断捕获原版 libuv 的 uv_dlclose（0xcd30）调用 dlclose，返回点为模块偏移 0xcd64。关闭 Frida 的实际调用来自该封装；不能把 libuv 的模块身份误记为 libdp2game。原版 frida.config 没有 teardown 字段，而 [Frida 官方 Gadget 文档](https://frida.re/docs/gadget/)明确说明默认 minimal 不会停止内部线程，需要卸载时应使用 full，这与此前解除映射后仍有线程执行的现场一致。

只在私有配置设置 teardown=full 后，短失败路径仍返回 125，但异常变为主线程执行地址 0；诊断栈的返回地址落在原版 libdp2game 的偏移 0x345516。替换成仅含空 init/dispose 的自编 JS 同样复现，因此新异常不依赖原版业务脚本。未解决后续空调用，full 仅作为探测器的显式诊断选项，不修改默认输入配置或宣称停服修复。证据见 [Frida teardown 对照](evidence/p2-frida-teardown-diagnostic.json)。

## 2026-11-14 · libuv 定时器 abort 与 epoll 管道

继续定位返回 134 时发现可在资源加载前独立复现的问题：自编 Lua 在原版 DP2/luv 中创建真实一次性定时器，回调触发并关闭定时器后，uv.run 登记内部管道时调用 i386 epoll_ctl（nr=255），runtime 原先仅接受 socket，将该有效管道返回 EBADF，原版 libuv 随即调用 abort。旧正式宿主重复返回 134；同一双架构自编 ELF 在登记第一个管道时均停在第 48 行。

最小修复将现有 poll 的管道和 socket 等待逻辑提取为同一个内部函数，epoll 复用这套实际就绪查询和截止时间处理，不增加依赖。epoll_ctl 接受有效管道；普通文件等不可监视对象返回 EPERM。没有伪造登记或忽略 libuv 的失败。独立构建的原版 DP2/luv 定时器现已触发、关闭并正常返回，测试 guest 自行退出 42，无强制回收或 gap；此样件不加载 Frida，也不进入完整 game 资源初始化。

新增双架构 ELF 核对空管道超时、64 位事件数据、MOD/DEL、UDP socket 与管道混合等待、HUP、关闭 fd 后注销，以及外部 Windows 管道延迟写入唤醒。双架构全部通过，同一独立构建的 poll、TCP、入口/信号、加载器及原版 channel、relay 协议回归也通过。正式宿主仍由此前完整诊断运行使用，未覆盖正在运行的映像；完整 game 的 abort 调用栈及新构建的自然启动结果另行记录，不能以短样件通过宣称整体问题已解决。

后续完整诊断运行在 1286.188 秒返回 134，实际 abort 返回地址同样是原版 `libuv.so+0x19661`。原版反汇编确认该地址紧随 `uv__io_poll` 的 epoll_ctl 非 EEXIST 错误分支，和短样件一致。修复构建中附加 Frida 的短定时器也自行返回 42，耗时 29.392 秒；正式宿主重建后的无 Frida 定时器检查返回 42，耗时 25.372 秒，无强制回收或 gap。完整修复构建已启动新的 game、bridge、私有 MySQL 联合观察，其自然启动结果仍待本轮结束后记录。

证据见 [epoll 管道与原版 luv 定时器](evidence/p2-epoll-pipe-luv.json)。仅实测 Windows 10 x64 10.0.19044，EPOLLET/ONESHOT 等仍未支持。

## 2026-11-14 · select 微秒进位与 bridge 空转

完整 game 观察期间，外部只读采样发现原版 bridge 主循环持续占用 CPU，活动栈落在 `App::run`、`LinuxSystem::sleep` 和 select。原版 `0x080508cc` 将十秒等待写成 `{tv_sec=0, tv_usec=10000000}`；runtime 原先拒绝大于等于一百万的微秒值，立即返回 EINVAL，原调用方忽略错误后反复重试。核对 Linux v2.6.32、v3.10 的 select 超时转换后，仅修改现有转换：微秒按一百万向秒进位。没有给应用加固定延时、修改原版 ELF 或新增依赖。

双架构样件在旧宿主均复现错误；修复后确实等待一秒并返回 0，同时核对剩余 timeval 清零和负微秒值拒绝。复测还发现 GetTickCount64 的粒度会让已超时请求偶尔留下一个微秒，因此在实际返回超时时直接把剩余值清零。原有管道 poll、背压、HUP 和外部写入唤醒继续通过；同一阶段的双架构 TCP、epoll 检查也通过。

原版 bridge 采用独立 loopback 端口 7002、相同私有 MySQL 和 max_client=16 副本作对照。三秒无客户端窗口中，旧宿主消耗 3.078125 秒进程 CPU，最终修复宿主消耗 0.078125 秒；版本相同/不同、拆包/连包、四次完整频道脚本响应都通过，两者均响应 SIGTERM 后自行返回 0。此数据只证明本次空转消除，不代表长期负载或完整业务通过。正在运行的 game 联合观察保留原宿主，因此这组修复先在独立 `build/staged/` 构建验证。

证据见 [select 超时与 bridge 空转](evidence/p2-select-timeout-bridge-idle.json)。正式构建后可用 `python tools/test_poll.py` 复现；已有正式宿主运行时可传 `--hosts build/staged`。仅验证 Windows 10 x64 10.0.19044。

## 2026-11-14 · epoll 修复后的 game 自然启动与停服边界

完整 game、原版 DP2/Lua/Frida/脚本、原版 bridge 及私有 MySQL 联合运行 1785.295 秒。未修改启动脚本的自然路径完成频道脚本下载/导入、DataManager、ChannelType、GameWorld、Login State，并在 03:48:30 UTC 输出启动标记及原版 Lua 文件加载、执行成功日志。随后继续观察 120.037 秒，未再触发此前 `libuv.so+0x19661` 的 abort。只在启动后的私有 Work_Reload.lua 追加自编日志标记，原版自动重载机制实际执行成功。该结果验证本次启动阻塞解除，不代表客户端业务或完整服务就绪。

首次发送 runtime 的 SIGTERM 后，原版注销频道但继续运行。核对原版 `CShutdowManager::SendShutdownSignal` 和 signal_handler 的跳表确认：game 正常关服使用 SIGUSR2（12），SIGTERM（15）只执行 rdar_terminate 和包日志清理。当前跨进程接口仅支持 15；为检查本次已启动实例，在私有副本通过自编 Lua C 模块调用已有的 `kill(self, 12)`，原版实际输出 `RECV SIGUSR2`、`ALL USER LOGOUT COMPLETE`、收包/交易线程结束及 `GAME SERVER SHUTDOWN COMPLETE`。自编模块最初因默认共享库入口为零被当前 loader 拒绝，显式设置可执行入口后才成功加载；这些测试改动均发生在自然启动和观察之后，没有修改输入原件。

shutdown complete 后仍出现访问异常，地址对应原版 Frida 偏移 `0x89e3c`，game 返回 125，无强制回收。该偏移与先前 minimal 卸载诊断一致，但本次没有重新采集页状态，故不计正常停止。另一组隔离的 full 卸载诊断已确认：DP2 保护代码通过 ret 读取栈中的零地址而异常，并非已经证实的间接空函数调用；零值来源尚未解决，也没有 guest SIGSEGV handler 可以转发。未为此跳过检查、保留已卸载映射或修改默认 Frida 配置。

bridge 和 MySQL 均自行返回 0，源私有数据库逐文件身份不变。长观察退出后，正式宿主已按当前源码重新构建，双架构 select/poll、TCP、epoll 及原版 Lua/luv 定时器检查通过。自然启动用的是包含 epoll 修复的原宿主；select 修复另有独立构建和正式构建证据，不混用两者结论。

证据见 [自然启动与停服](evidence/p2-game-epoll-natural-startup.json)、[Frida 卸载诊断](evidence/p2-frida-teardown-diagnostic.json)。下一实际缺口为 game 的跨进程 SIGUSR2、Frida 正常卸载及后续服务互通；共享库零入口是已观察到的 loader 限制。Win7/Win11、客户端业务和最终交付均未验收。

## 2026-11-14 · 跨进程 SIGUSR2

原版 game 的正常关服使用信号 12，而 runtime 的跨进程 kill 原先只支持 0/15。沿现有命名事件增加 SIGUSR2 通知，接收后仍使用原有 pending、屏蔽、handler 与恢复路径；没有新增通用 IPC 框架。

自编独立 ELF 进程分别发送 kill(pid, 0)、12 和 15。接收方先屏蔽 12，核对 15 已处理而 12 未处理，再解除屏蔽确认 12 到达。修复前四种发送/接收架构组合全部失败；修复后 32→32、64→32、32→64、64→64 的双方均自行返回 42，无强制回收。同一正式构建的双架构入口/信号检查通过。

证据见 [跨进程信号](evidence/p2-process-signals.json)，复现命令为 `python tools/build.py`、`python tools/test_process_signal.py` 和 `python tools/test_entry.py`。本次通过范围是信号原语；原版 game 的完整正常关服仍受 Frida/DP2 卸载异常阻塞。实测系统为 Windows 10 x64 10.0.19044。

## 2026-11-14 · DP2 析构期间的 Lua 定时器回调

单步追踪将零返回地址定位到原版 DP2 的 CPluginBase 虚函数表：保护代码读取对象的 vptr，再取偏移 0x18 的表项；该地址为 libdp2game.so+0xc76e0，值为零，随后经栈上的 ret 进入地址零。私有内存快照中的 RTTI 确认类型为 CPluginBase。原版解包后的析构函数在 0x311bf 改写为基类 vptr，之后在 0x3121a 调用 uv_run(loop, UV_RUN_NOWAIT)，再执行关闭 luv 句柄的 Lua 代码。因此到期的脚本仍可能在 game 派生对象析构后执行。

对照没有把故障归给 Frida 本身：加载普通 libm、单独注册 Lua handler、只创建空回调定时器、只 require df.game 均返回原版数据库配置失败码 1；保留原始 Lua 内容、仅注释 frida.load 后仍复现返回 125、PC=0。对该私有脚本追加已有 add_unload_handler 回调，提前关闭并处理 luv 句柄后返回 1。原版 Lua/JS/Frida 全部加载、私有配置 teardown=full 并应用同样清理时也返回 1，记录实际 Frida init、dispose 和 DP2 unload，无访问异常或强制回收。

新增独立编写的 runtime/dp2_cleanup.lua，通过既有停服回调提前关闭 luv 句柄并处理 close 回调；probe_game.py 的显式 --dp2-cleanup 仅在私有副本追加该脚本，并将 Gadget teardown 设为 full。正式文件复测返回 1，耗时 48.102 秒。没有跳过原版数据库错误、修改原版机器码、禁止 munmap 或强制成功退出。仍存在已知的 inotify 探测 fallback（i386 291/332），本次未将其扩成新的实现。

证据见 [DP2 清理对照](evidence/p2-dp2-cleanup.json)。这解除的是短失败退出路径，尚不等于完整服务正常停服。联合探测器现可在原版启动标记后观察指定秒数，再发送跨进程 SIGUSR2，并要求原版关服标记和退出码 0。完整 game、bridge、私有 MySQL 的 --dp2-cleanup --stop-after-started 120 复测已启动，结果另行记录。

## 2026-11-14 · 标准共享库的零入口

此前自编 Lua C 模块因 ELF e_entry=0 被拒绝，只能临时指定导出函数为入口。根因是 nr_elf_open 对进程、解释器和共享库统一要求可执行入口。现由三个调用点明确是否要执行入口：进程和 PT_INTERP 要求非零、位于可执行 PT_LOAD 内的入口；仅映射的 ET_DYN 可省略入口，非零错误入口继续拒绝。沿原有验证路径增加一个参数，没有改用其他 loader 或放宽映射边界。

自编 i386/x64 共享库均由标准链接器生成 e_entry=0，通过上游 glibc 的 dlopen 实际执行构造函数、调用导出并运行析构，双方返回 42；修复前均在 dlopen 失败。错误非零 DSO 入口、将零入口库当程序运行、零入口 PT_INTERP 均拒绝。负例同时发现旧宿主可能将解释器 ELF 头当入口执行，该行为现已在映射前拒绝。独立构建上的双架构入口/信号、上游 ld.so，以及原版 19 ELF 原址映射和原有四项拒绝检查通过。

证据见 [共享库零入口](evidence/p2-shared-entry.json)，复现命令为 `python tools/build.py`、`python tools/test_shared_entry.py`。本轮为避免覆盖联合复测正在使用的正式宿主，使用 build/staged 独立构建并传 --hosts build/staged。联合 game 自然启动/关服观察仍使用包含 SIGUSR2 修复的正式宿主，不将两组构建混用为同一验收结果。

## 2026-11-14 · 原版 game 自然启动与完整正常关服

完整联合复测已通过：原版 game 加载原版 DP2、Lua/JS、Frida 和 PVF，连接私有 MySQL 与原版 bridge；1275.298 秒出现原版启动成功标记，继续观察 120.083 秒后通过跨进程事件发送 SIGUSR2。原版依次输出 RECV SIGUSR2、ALL USER LOGOUT COMPLETE 和 GAME SERVER SHUTDOWN COMPLETE，随后 Frida dispose、DP2 unload，game 自行返回 0，总计 1455.341 秒，无访问异常或强制回收。私有 Lua 仅追加已提交的提前关闭 luv 句柄回调，Gadget 使用 full teardown；没有修改原版 ELF/DSO 指令或省略原始应用脚本。

bridge 完整频道脚本响应匹配原文件，并自行返回 0。MySQL 日志记录 InnoDB 正常关闭和 Shutdown complete，服务器返回 0，无强制回收，源私有库逐文件身份不变。停库辅助命令 mysqladmin 返回 1，旧探测器未保留其 stderr，故不把辅助命令计为成功；该问题不影响已经取得的 game 正常关服证据。仍记录 msync（144）、inotify（291/332）探测 fallback，未声明这些调用已经实现。

证据见 [game 完整关服](evidence/p2-game-clean-shutdown.json)。复现命令为 `python tools/probe_game_chain.py --database-source <停止的私有恢复库> --dp2-cleanup --stop-after-started 120 --timeout 2100`。本次使用含 SIGUSR2 修复的原宿主，后续零入口与 select 修复不混入本轮构建结论。通过范围是 game、bridge、私有数据库的自然启动及正常关服，实测系统仍为 Windows 10 x64 10.0.19044；16 服务就绪、客户端业务、GUI、最终包和另外两个系统均未验收。

## 2026-11-14 · DBMW 并发关闭 socket

三个原版 DBMW 的停服崩溃根因已确认：一个线程关闭监听 socket 时，Winsock 唤醒另一个线程的 select；runtime 原先返回了 stale ready count，而 guest 按已被原版改写为 -1 的 fd 重建集合，在 `TCPSocket::pollReadEvent()+0xf2` 访问越界。修复在关闭前发布 INVALID_SOCKET，select 返回后检查监视槽位是否改变并返回 `-EBADF`；结果集合按快照重建，避免空集合配正数。

双架构自编 ELF 样件五次并发关闭均以 42 退出，验证 fd 集合、读写计数、关闭策略和 EBADF。修复后的三个原版 DBMW 均完成 11 个数据库连接、网络线程、加载和 SIGTERM 清理标记后自行返回 1；反汇编确认原版 main 在正常 Clear 后固定返回 1。stderr 无 runtime gap、无强制回收，私有 MySQL 返回 0，源库身份不变。

证据见 [DBMW 并发关闭](evidence/p2-select-close.json)，复现 `python tools/test_select_close.py` 与 `python tools/probe_dbmw.py --database-source <停止的私有恢复库>`。本轮修复已由正式双架构构建完成；只验证 DBMW 初始化与停服，不宣称 DBMW 业务协议、16 服务或客户端验收。

## 2026-11-14 · 六个辅助服务与 DBMW 联合初始化

在同一私有 MySQL 副本上实际运行原版 auction、point、guild、statics、coserver、community 和三个 DBMW。guild 连上原版 DBMW，并在停止时输出 Guild And Guild Member DB Save Success；statics/coserver 完成原版加载和 SIGTERM 清理；community 使用原版 SIGUSR2 停止，自行返回 0。auction/point 采用原版支持的 run 前台动作和短服务名，避免误用 daemon 参数或把服务名当绝对路径。

auction 首次进入实际数据库初始化后返回 9。MySQL 语句历史给出确切原因：原版查询 auction_history_202611，旧种子只有无年月后缀的模板表，错误为 1146。在私有副本内用 CREATE TABLE ... LIKE 原模板创建两个拍卖库当月的 history 和 history_buyer 表后，该错误消失；保留全部原版 ELF 指令、原始数据及真实数据库调用。没有改系统时间、跳过查询或伪造查询结果。

早先五秒观察后停服不能确认拍卖服务已完成初始化；最终探测对 auction/point 各观察 20 秒，并以原版初始化及 Server Terminated 标记核对结果，不把等待时间或端口计为就绪。九服务联合正式复测全部通过：auction/point/community 返回 0，其余原版框架正常返回 1；全部无强制回收、无 runtime gap，MySQL 返回 0，源库身份不变。停止按依赖倒序执行，guild 先于 DBMW。community 前台模式未创建 daemon PID 文件，因此停止时有缺少 PID 文件的删除提示，不隐藏此差异。

证据见 [辅助服务联合初始化](evidence/p3-auxiliary-service-init.json)，复现 `python tools/probe_dbmw.py --database-source <停止的私有恢复库> --auxiliary`。本轮没有增加 runtime 实现；九服务结果只证明已观察的初始化与停止路径。默认 16 个 core 服务现均已有各自的部分实际运行证据，但完整 16 服务联合就绪、请求/数据核对、客户端业务和最终交付仍未验收。

## 2026-11-14 · secagent 实际暴露的 rename 与 msync

原版安全三件套的私有副本首次运行时，secagent 实际触发 i386 rename（38）和 msync（144）缺口。沿现有路径解析增加 MoveFileEx 替换；共享文件映射保留独立句柄，MS_SYNC 使用 FlushViewOfFile 和 FlushFileBuffers，因此 guest 关闭原 fd 后仍能同步。没有增加依赖或改写原版 ELF。MS_ASYNC/零 flags 使用系统已有脏页异步写回；MS_INVALIDATE 明确返回 ENOSYS，范围目前须落在单个已跟踪 NrVm 映射内。

Windows 兼容接口不能替换仍打开的目标文件，独立 Windows 对照实际返回错误 5。本次对此返回 EACCES 并保留两个文件，没有先删目标再改名来冒充 Linux 原子替换。双架构 ELF 核对已关闭目标的替换、源句柄仍打开、同路径和错误输入，以及打开目标时明确失败；msync 核对关闭原 fd 后同步、部分范围、异步、匿名映射和错误范围，进程结束后逐字节比较磁盘内容。相同最终夹具在旧宿主均失败，新构建均返回 42；文件 ABI、共享映射别名和匿名 VM 回归通过。不能将该范围写成完整 Linux rename/msync 或断电耐久性验收。

正式化的 tools/probe_security.py 复测原版 ELF 和原版配置，secagent 建立 347045468 字节用户共享文件，完成 gamesvr channel、user info manager、dpsdk 配置初始化，SIGTERM 后返回 0，无上述缺口、访问异常或强制回收。原版缺 cfg/restrict.txt，日志明确关闭该限制功能，保留此差异。gunnersvr 虽返回 0，但实际因连接 127.0.0.1:9000 监控服务失败而初始化失败；zergsvr 配置使用空 cfgsvr 地址，返回 183 并记录 init_instance ret=290010039，均不计通过。

证据见 [安全组件文件调用](evidence/p3-security-file-sync.json)。本轮仅验证 Windows 10 x64 10.0.19044；修复在 build/staged 独立构建执行，正在进行的 16 服务联合观察保留旧正式构建，不混用构建结论。安全组件互通、必要性与客户端业务仍待验证。

## 2026-11-14 · zerg 实际连接的 getpeername

核对原版 Zerg_Server_Config::init 后确认其支持 cfgfile；私有本地路由探测实际触发 socketcall 7（getpeername）缺口。runtime 沿既有 getsockname/accept 路径调用真实 Winsock getpeername，增加 x64 syscall 52 映射；地址只写调用方容量，保留完整地址长度，拒绝负长度和无效输出。

双架构真实 TCP 样件核对连接前 ENOTCONN、连接后对端 loopback IP/端口、截断地址不越界、零容量及错误指针。旧宿主在连接前检查失败，新构建的普通/abortive 关闭四种组合均返回 42；同构建 epoll 管道/socket 和并发 close/select 回归通过。原版 zerg 不再记录 getpeername 缺口，但未修补的停服路径仍在 PC=0 返回 125，因此没有把此原语修复写成服务正常关服。

证据见 [getpeername](evidence/p3-getpeername.json)。工具 test_net.py 现可用 --hosts 指定独立构建。仅实测 Windows 10 x64 10.0.19044，本节与仍使用旧构建的 core 联合观察分开计证。

## 2026-11-14 · 安全组件本地路由与 zerg 清理顺序

原版 Zerg_Server_Config::init 在 0x0807f700 比较 cfgsvr，在 0x0807f77e 比较 cfgfile；后者进入 ServicesConfig::LoadSvrConfig。私有副本选择 cfgfile 后，原版继续明确拒绝 svcid.xml 中重复的 type/id。输入文件的六项实际为三条路由各重复两次；仅合并完全相同项并更新数量，命令行使用原配置服务编号 -t30 -i570011。zerg 因而进入运行循环，原版 gunner 实际记录 connect monitor succ，不再因连接失败提前返回。默认 probe 仍保留原配置，修改由 --local-routes 显式启用。

随后正常 SIGTERM 暴露独立停服异常。诊断宿主捕获 PC=0，栈顶返回 0x080ad575，对应 ZEN_Server_Status::increase_by_statid 在 0x080ad572 通过统计锁虚表偏移 8 调用；上一层为 TCP_Svc_Handler::handle_close。现场锁对象的虚表地址落在已被改写的堆数据，表项为零。核对原版 exit_instance：Comm_Stat_Monitor::clean_instance 位于 reactor/timer 的虚函数清理之前，后者仍触发连接关闭回调。没有把该现场误报成 getpeername 修复失败或任意忽略异常。

仅对私有 zerg ELF 的 0x080775d5 起 49 字节调整顺序，将原本的统计清理调用移到 reactor/timer 清理之后；修正移动指令的相对 call，保留全部清理、函数大小及其余字节。补丁先核对原版 SHA-256 和旧字节，记录新字节与执行哈希；原件不变，省略 --fix-cleanup-order 即恢复未修补对照。反汇编核对新的统计清理位于 0x08077601。正式探测复测三个组件均 SIGTERM 自行返回 0，无强制回收、无 runtime gap，原始 zerg 哈希复核不变。

该结果仅证明已观察的初始化/退出和 gunner 的真实 TCP 连接。原路由仍引用缺少的 9001 端点，type 31 与本地 zerg 共用 9000，且 watchdog 缺 /proc/<pid>/stat、restrict.txt 缺失等提示仍保留；没有宣称完整 TSS 拓扑、反外挂业务或可选性已验收。证据见 [安全组件清理顺序](evidence/p3-security-cleanup-order.json)，复现 `python tools/probe_security.py --host build/staged/i686/dnf-host.exe --local-routes --fix-cleanup-order --observe-seconds 10`。仅实测 Windows 10 x64 10.0.19044。

## 2026-11-14 · 首次 16 服务联合启动与停止观察

同一私有 MySQL 上同时运行 core 的 16 个原版服务，monitor 使用已记录的私有 UDP 停服补丁。game 加载原版 DP2/Lua/JS/Frida/PVF，1280.542 秒出现自然启动标记和原版 Lua 加载/执行成功；再观察 120.054 秒，SIGUSR2 后三项关服标记齐全，Frida dispose、DP2 unload，game 自行返回 0，总计 1460.043 秒。其余 14 个 peer 在各自停止请求前均仍存活，全部自行退出，无强制回收、无 peer runtime gap。guild 实际记录 Accept GameServer 并在停止时完成数据库保存标记；这些局部事件不能替代完整服务互通。

bridge 脚本响应逐字节匹配原件，返回 0。MySQL 服务器和 mysqladmin 本次均返回 0，停库输出为空，源私有数据库逐文件身份未变。本轮 channel/relay 容量为 16，所有私有服务地址为 loopback，当月拍卖表在副本按原模板创建。STUN 使用原版 test 模式，SIGTERM 加一字节 UDP 唤醒后本次实际返回 0，没有额外应用清理标记；不沿用此前预计的 143，也不把它写成完整清理验收。

game 使用上一正式构建，仍记录 msync（144）和 inotify（291/332）fallback；另反复出现 network unsupported nr=102 op=15，具体 socket option 未被旧日志记录。因此本轮只计联合自然启动和停止观察，完整互通仍未通过。后续静态定位发现 game 的 getsockopt 调用除频道代理的 SO_RCVBUF 外，其余位于非阻塞连接/MonitorTcpThread，使用 SOL_SOCKET/SO_ERROR；实际动态调用细节和修复另行验证，不能将推断记为已修复。

证据见 [16 服务联合观察](evidence/p3-core-co-run.json)。tools/probe_game_chain.py 新增 --core 复用本轮准备和停止步骤，并记录 peer 退出、配置和 monitor 补丁；核心标记仍须结合应用日志审阅。复现 `python tools/probe_game_chain.py --database-source <停止的私有恢复库> --core --dp2-cleanup --stop-after-started 120 --timeout 2100`。仅验证 Windows 10 x64 10.0.19044，客户端、GUI、MySQL 单文件和交付包仍未验收。

## 2026-11-14 · 非阻塞连接与 SO_ERROR

首次 core 观察的 game 实际记录 7664 次 socketcall 15 不支持。独立扫描并逐处反汇编 game 对 getsockopt PLT 的六个直接调用：频道代理使用已支持的 SO_RCVBUF，其余现金连接及四处 MonitorTcpThread 使用 SOL_SOCKET/SO_ERROR。runtime 原先仅支持缓冲区查询，因此存在明确的连接错误查询缺口。旧日志没有 level/option，未将所有 7664 次调用的具体归属写成已动态确认；新日志会记录不支持的 level/option。

实现调用真实 Winsock SO_ERROR，并将其中的 WSA 错误码转成正的 Linux errno。实际失败样件同时确认非阻塞 connect 的 WSAEWOULDBLOCK 应返回 EINPROGRESS，不能沿用普通读写的 EAGAIN。独立 Windows 对照证明拒绝连接后的 SO_ERROR 连续两次均为 10061，因此仅对 pending connect 记录消费状态，首次查询返回 ECONNREFUSED（111），再次返回 0，新连接尝试和 fd 关闭/重用时重置。没有对实际失败连接返回虚假成功。

双架构真实 TCP 样件使用另一个已绑定但未监听的端口制造拒绝连接，旧构建在 SO_ERROR 查询失败；修复后连接成功、EINPROGRESS、poll 的失败事件、111、读取后清除，以及原有传输、getpeername、超时和普通/abortive 关闭全部通过。最终正式双架构构建的 epoll、并发 close/select 和 rename/msync 回归通过，构建身份见 [SO_ERROR 证据](evidence/p3-so-error.json)。该清除处理限定于非阻塞连接错误，不宣称完整 Linux socket error queue。

已用包含上述修复的正式宿主启动新的 16 服务联合复测，继续要求原版自然启动、120 秒观察和正常关服。该轮尚未结束，不能把原语通过替代 game 网络缺口消失或完整互通结论。仅实测 Windows 10 x64 10.0.19044。

## 2026-11-14 · 联跑实例协议响应与 channel 配置修正

直接连接正在联跑的原版实例，核对 channel 随机挑战、成功握手、拆包和连包；relay 两个自编账号登记、八次双向逐字节转发、UDP 定向断开及另一连接保留；STUN 五类地址/长度响应与响应源端口，全部通过。新增 tools/probe_core_protocols.py 可复现这些请求；它连接现有私有实例，不负责启动或停止服务。

实际 channel 监听 5555，且日志记录 bridge ipport=0。原因是联跑脚本只替换数字 IP，遗漏了原配置的 this_ip = Public IP。原版 Script::get_key_val 只接受一个节名或两个键值 token；该占位行无法形成有效 this_ip，get_server_section 提前返回，后续端口和 bridge 字段没有载入。这不是 bridge 下发端口。现已在私有配置准备步骤中明确设置 this_ip = 127.0.0.1。

对联跑 channel 的另一个私有副本仅修改这一行，保留 max_client=16：实际监听 7001，建立到现有原版 bridge:7000 的连接，拆包/连包握手通过，SIGTERM 后自行返回 0，无 runtime gap 或强制回收。独立 channel 原有 32 次成功响应回归也通过。证据见 [联跑协议与配置](evidence/p3-core-protocols.json)。正在运行的整轮仍保留旧私有配置，修正后的单服务对照不能计作整轮修正通过；完整互通、客户端及最终交付仍未验收。

## 2026-11-14 · SO_ERROR 修复后的完整 core 联合复测结束

本轮原版 game 自然启动耗时 1294.301 秒，启动后观察 120.937 秒，随后 SIGUSR2 正常停服，三个关服标记、Frida dispose、DP2 unload 齐全；总计 1475.737 秒，返回 0，无强制回收。此前的 7664 次 socketcall 15 不支持记录降为零，msync 缺口也未再出现。game 仅保留 inotify 332/291 的 ENOSYS fallback，无其他 runtime gap。其余 14 个 peer 均未提前退出，正常完成停止，无 runtime gap 或强制回收；bridge 响应匹配并返回 0，MySQL 及 mysqladmin 均返回 0，源私有库身份不变。

该轮使用 15fee3b 的联跑脚本和包含 SO_ERROR 修复的正式宿主；执行期间后续编辑的脚本不计入本轮。channel 仍是已发现的占位配置，monitor 首次统计写库出现 MySQL 1264；因此本轮仍不计完整互通或业务验收。两项问题分别修正和验证，不能把本轮启动/停服通过写成所有配置已正确。证据见 [SO_ERROR 后的 core 联跑](evidence/p3-core-so-error-co-run.json)。

## 2026-11-14 · monitor 首次统计溢出的根因与修复

完整联跑的 DBMW 首次写 log_num_occupations 时出现 MySQL 1264：登录/退出计数分别为 110056896 和 574452856，后续周期恢复为零。原版 monitor 的 CLoginLogoutStatistics 构造函数只初始化对象偏移 0xac/0xb0，遗漏 0xb4/0xb8；ProcessByMinute 读取遗漏的字段上报，然后才清零，因而首次传出残留内存。问题位于原版对象初始化，不是数据库列宽或 SQL mode。

在原版哈希和旧字节校验后，仅对私有 monitor 副本替换 0x080a635a 起的 26 字节初始化块，显式清零四个计数，保持 ELF 长度、全部函数地址和其他指令布局。指令对照在预填 0xa5 的对象上执行：原块留下两个 a5a5a5a5，补丁清零四项，邻接字段及返回指针保持正确。该对照只验证初始化指令块，不冒充完整 ELF 业务测试。

新增 --peers-only-seconds 复用现有准备/停服步骤，实际运行 bridge 与另外 14 个原版 core 服务，省去 game 自然加载以定位 peer 问题。本轮观察 75 秒，原版 monitor 经真实 DBMW 写入两条新统计，四项计数均为零，未出现原来的 1264。修正后的 channel 实际监听 TCP/UDP 7001，握手、relay 转发与定向断开、STUN 响应全部通过。14 个 peer 均自行退出，无提前退出、runtime gap 或强制回收；bridge 返回 0，源私有库不变。

本次 mysqladmin 返回 1 并报告 shutdown 时连接丢失；实际 MySQL 服务器返回 0，日志完整记录 Normal shutdown 和 Shutdown complete，未强制终止。保留这两项不同结果，不将客户端错误改写为成功。证据见 [monitor 首次统计修复](evidence/p3-monitor-statistics.json)。本轮未运行 game，不计修正后的完整 16 服务或客户端验收；GUI、MySQL 单文件、离线包和 Windows 7/11 仍未完成。

## 2026-11-14 · 官方 MySQL 的本地运行库与单文件首版

先从四个官方 PE 的实际导入确认 VC++/UCRT 依赖，再从微软固定版本分发包导入三个 VC++ 2019 DLL 与 SDK x64 UCRT 的 42 个 DLL，全部 Microsoft 签名通过。未复制 System32 文件，未执行 VC/SDK 安装器。7-Zip 只作为开发解包工具使用。来源和逐文件身份见 [运行库导入](evidence/p4-mysql-runtime-import.json)。

独立实现 delivery/mysql_standalone.cpp，以系统 FDI 解压内嵌 CAB，CNG 校验 payload，Win32 原生接口启动官方四工具。首版 EXE 为 15,882,240 字节，包含 114 个文件；不嵌入数据库、参考产物或 7-Zip。首次展开的目录分隔符问题已按实测修正；已有缓存校验及并发文件锁均通过实际测试。

只复制单个 EXE，在 PATH 限于 Windows System32 的中文/空格/括号目录里测试：四个官方工具均为 5.7.44；四进程并发准备归于同一有效 payload；新私有库初始化成功；TLS 加密连接读写、两行二进制内容逻辑备份/还原逐字节匹配；正常停服 mysqld 和 mysqladmin 均返回 0，无强制回收。直接检查真实 mysqld 模块，三个 VC++ DLL 均来自实例缓存；已有数据拒绝重新初始化，修改缓存明确拒绝运行。

官方客户端不能读取本机中文绝对 defaults-file 路径，而相对 ASCII 文件名可用。控制器沿用已固定的实例工作目录，以相对配置名传递凭据，未依赖仍含中文的 8.3 别名；中文绝对备份文件路径在实际 dump/restore 中通过。证据见 [单文件实测](evidence/p4-mysql-standalone.json)。

该首版保留全部官方 share，尚未声称最终最小闭包。Windows 10 实际使用系统 UCRT，Win7/11 与干净离线机未测；产品首次安全凭据配置、GUI 备份/还原/回滚、DNF 与 SFX 集成及 ≤210 MB 完整 ZIP 仍未完成。测试 --initialize-insecure 限于新建的 loopback 私有实例，不是产品默认策略。

## 2026-11-14 · 原生 MySQL 安全首次初始化

单文件引导新增 initialize INSTANCE [PORT]：官方 --initialize 生成临时密码，通过禁止 TCP 的本机命名管道设置 CNG 随机密码，使用新凭据执行真实查询后正常停库。生成 server.cnf 和 client.private.cnf，后者强制 TLS；data、配置和私有日志的 DACL 限于当前 Windows 用户。密码不进入命令行和普通输出。失败保留数据与私有诊断，已有实例明确拒绝重新初始化。

新版本为 15,906,304 字节。只复制 EXE，在新的中文目录里、PATH 限 System32 实测：独占预定 TCP 端口时初始化成功；敏感目录和四个文件均只有当前用户访问规则；新凭据重启查询成功，空密码 root 返回认证错误 1045；TLS、二进制两行备份/还原、缓存校验、VC DLL 本地加载和正常停服全部通过。安全初始化重试与官方重新初始化均被拒绝，全部 data 文件内容哈希保持相同。证据见 [安全初始化](evidence/p4-mysql-secure-initialize.json)。

本轮只实测开发机 Windows 10。GUI 密码管理、跨用户迁移、预快照/失败回滚、最终完整离线包与 Win7/11 尚未完成。MySQL 单文件接入修正配置后的完整 core 联跑正在进行，尚不计通过。

## 2026-11-14 · 完整原始数据库往返与初始化停服修正

将安全单文件测试从两行样本扩展到原始 18 个应用库、601 张表，首次 result-file 备份还原后少了 1056 个 CR。单加 --binary-mode 仍失败。读取官方 5.7.44 client/readline.cc 确认 Windows 分支始终删除 LF 前的 CR；改为捕获 mysqldump 文本标准输出，保留官方工具配套的换行处理，没有改动官方 PE，也没有对结果做归一化。

最终在安全初始化的新私有实例中，以 TLS 导入原始种子，完成全表锁备份、删库、还原和再导出；原始种子、备份、再导出均为 977716 字节，SHA-256 完全相同。各库表数一致，原始种子未变；另外准备独立诊断账号及连接表供原版服务使用，未导入源系统账号。该账号为隔离测试配置，不是产品权限策略。

复测还触发官方 mysqladmin 命名管道停服连接丢失。初始化原实现通过短路条件跳过了服务器等待，随后提前结束尚在关闭的自有子进程；现改用 SQL SHUTDOWN，并独立取得客户端、服务器退出码。修正版单文件再次通过中文路径、安全权限、空密码拒绝、TLS、备份还原、全部 data 文件保留和正常停服。失败记录与修正后的证据见 [原始应用数据库](evidence/p4-mysql-original-seed.json)。GUI、失败回滚和三系统验收仍未完成。

## 2026-11-14 · MySQL 单文件接入原版服务链

联合探测的所有数据库工具调用均改经本项目单文件引导，数据库进程 PATH 限于 System32。修正 channel 配置和 monitor 计数后的完整 16 服务联跑中，game 自然启动 1298.894 秒，观察 120.039 秒，总计 1480.445 秒；SIGUSR2 后三个关服标记、Frida dispose 与 DP2 unload 齐全，返回 0，无强制回收。14 个其他 peer 和 bridge 均自行停止；MySQL/mysqladmin 均返回 0，源私有库未变。仅保留已知的 inotify 291/332 fallback。该轮末次 relay 自编检查失败，原始 receipt 保留错误，不能改写为整轮互通通过。

安全新库接入的首次失败来自配置口令格式：原版 CAppConfig::DecryptValue 要求 48 个十六进制字符，先前诊断 fixture 误填了随机明文。根据原版 DBMW 的哈希、构造函数和 XTEA 分组指令独立实现编码，固定材料只读原版 ELF。重建私有 fixture 后，三路 DBMW、auction/point 和其他 peer 正常连接；monitor 真实写入两条新统计，四项计数均为零。

进一步记录真实 UDP 报文，并用 Windows UDP 端口所有者核对发送进程：原版 monitor 分别对自编账号 701/702 发出结果为 0 的拒绝响应。先前 relay 检查要求另一未登录账号保持连接，不符合存在 monitor 时的实际认证行为。联跑检查现通过无操作类型 1 唤醒原版认证队列，再要求两个未登录账号被真实 monitor 拒绝；没有由测试端发送认证结果。75 秒观察前后，channel、双向 relay 转发、monitor 拒绝和 STUN 全部通过。独立 relay 原有 32 次转发、拆包/连包、定向断开及另一连接保留仍通过，另有五轮连续重连成功。这些结果不证明真实游戏登录认证或可玩业务通过。

最终取消诊断转发器，使用默认端口直接复测安全数据库与 15 个 peer，协议检查均通过、全部服务自行停止、源库不变。另用原版 Frida 脚本的 init_db 接口实际创建并读取 data/dgnfinishtime/init_sp_tp 三张表，game 按此专项测试约定返回 42，无强制回收；它不属于另一轮完整 game 自然启动验收。

SQL SHUTDOWN 也复现客户端 ERROR 2013、服务器 0 且完整关闭的情况。此前以客户端必须返回 0 判整项失败过严；现在分别记录两者，仅对该特定确认丢失且自有服务器明确正常退出的结果作已关闭处理，其他错误仍失败。该阶段单文件为 15,908,352 字节，再次完成安全初始化、权限、TLS、原始 18 库/601 表逐字节往返及正常停服。证据见 [单文件与服务链](evidence/p4-mysql-core-integration.json)。该阶段结束时 GUI、预快照/失败回滚、客户端业务、最小离线包和干净 Win7/10/11 验收仍未完成。

## 2026-11-14 · 原生离线备份、还原与回滚

把开发期已验证的数据库流程接入同一个 `mysql-standalone.exe`，新增 `backup/restore/recover`，没有引入框架、第三方解析库或额外执行器。新产物为 15,958,016 字节，SHA-256 `6bde49e586430520e0c75e81965f08a098b9de10b17393f5e297e9edfc1bbc13`，位于忽略的 `build/mysql-maintenance/`。原有默认构建目录中的旧产物不会被这次隔离构建自动替换。

当前维护只接受已停止、同 UUID 且连接配置未变化的实例。完整逻辑备份包含应用库、账号与存储对象；还原前保存逻辑预备份，在逐文件核验过的冷副本中导入、重启、重新导出并要求字节哈希完全一致。只有验证通过才切换；原数据保留在操作目录内的 `previous-data/`。文件锁协调本项目的启动/维护，真实 SQL 与自有进程确定维护结果，不接管其他数据库。

本阶段定位并解决两项实际失败：官方命名管道通过 16384/65536 字节 SQL，却对 262144/811848 字节 SQL 返回 ERROR 2006，当时服务器 max_allowed_packet 为 4194304。改用官方本机共享内存协议后相同大请求通过，维护服务器仍以 `--skip-networking` 关闭 TCP，未修改官方 PE。另一个失败来自完整导出中的 `mysql.innodb_*_stats` 和 `mysql.proc` 内部时间戳：表重建会再生统计，过程定义重建会更新时间。新备份格式只省去可再生优化器统计，以及已经由 `--routines/--events` 导出定义的重复系统行；应用数据、账号、过程/事件/触发器定义继续参与逐字节比较，没有归一化结果或忽略比较差异。

最终在原始 18 库/601 表的独立安全副本上增加 InnoDB/MyISAM 二进制样本、CRLF 存储过程、事件和触发器，验证正常还原、删除备份后新增库/账号、哈希损坏提前拒绝、真实无效 SQL 在副本内失败且活动库不变。利用 Windows 目录共享锁让原库已移入冷快照后激活失败，实际触发自动回滚，回滚后的完整逻辑备份仍与原备份一致。三个改名窗口的磁盘状态分别通过 `recover` 恢复；恢复入口和未完成标记检查不依赖 payload 展开。包装器在执行子进程前设置 Windows Job，实际强制结束包装器后，其官方 MySQL 子进程也退出。

最终维护专项与原有安全初始化/TLS/权限/四工具检查均返回 0，源数据不变，无兜底强制清理。刻意终止包装器属于单独的故障试验，不算正常停服。证据见 [原生数据库维护](evidence/p5-mysql-native-maintenance.json)。三个切换窗口使用构造的真实冷数据目录状态，尚不是断电实测；GUI、DNF 停服/恢复协调、跨实例/密码迁移、空间预估、磁盘写满、断电和干净 Win7/10/11 验收仍待完成。下一阶段接入自研 Win32 GUI。

## 2026-11-14 · 自研 Win32 数据库 GUI

新增本项目独立编写的 Win32/C++ 控制台，已接入真实 MySQL 单文件操作：实例目录选择、端口、安全初始化、SQL 身份/TLS 核对、启停、备份、还原、未完成还原恢复和日志目录入口。耗时任务在工作线程，完成结果经窗口消息更新；关闭运行中的控制台会正常停库，繁忙时关闭不会中断备份。新 GUI 为 854528 字节，构建只依赖系统 DLL；源代码和 Win32 manifest 均在本仓库，无参考 GUI 或 .NET。

`tools/test_gui.py` 通过实际窗口及标准控件完成 15 项检查：空目录输入可重新选择、中文实例安全初始化、SQL 版本/UUID/本次进程标记/TLS 核对、同端口另一 UUID 启动失败且原库继续可用、正常停库、备份及还原期间消息循环响应、忙时关闭不取消备份、损坏备份报错且数据不写入、二进制行恢复、备份后新增库清除、关闭窗口正常停库，以及未完成还原状态拒绝启动并通过恢复按钮恢复原冷数据。最终 GUI 专项返回 0，无强制清理；截图已本地检查，位于忽略的 probe 目录。原生完整维护专项在共用 Win32 文件代码提取后再次通过。

证据见 [Win32 数据库界面](evidence/p6-win32-gui.json)。复现：`python tools/build_mysql_standalone.py`、`python tools/build_gui.py`、`python tools/test_gui.py`。本阶段隔离产物位于 `build/gui/`，相邻的 MySQL 来自 `build/mysql-gui/`。界面目前仅管理数据库；19 个服务管理、插件/Frida 开关、密码编辑、等级选择、DNF 停服协调、日志筛选导出、干净 Win7/10/11 与完整离线 ZIP 仍未完成，真实客户端业务也尚未验收。下一阶段接入原版服务实例编排。

## 2026-11-14 · 原版服务接入自研 GUI

GUI 新增 19 项服务清单、PID/原始退出码、单项与整体启停、所选服务日志。原生 C++ 直接读取实例 `services.ini`，使用已有双架构宿主和停止事件；未引入框架、JSON 运行库或额外执行器。默认启动 16 个 core，三个安全组件保留可选，运行状态明确标注业务未验证。整体启动先核对自有数据库 SQL 身份，整体停止逆序等待服务退出后才停库；服务存活时禁用数据库停止、维护和实例切换。新 GUI 为 886784 字节，隔离产物位于 `build/gui-services/`。

重复启动暴露原版 CApplication 在前台正常退出后遗留 PID 文件的问题：进程已经退出，但下一次初始化被旧文件拒绝。现仅在自有进程句柄确认退出、且文件内容匹配该 PID 时清理，不根据文件中的 PID 去结束其他进程。manager、DBMW、monitor 等八个服务完成原版清理后返回 1，与以前联合实测一致；保留原始返回码及清理标记，不将其改写为 0。测试也修正了错误的全零返回码假设，并为异常记录加入类型，避免空 AssertionError 文本被误判为没有错误；先前那份命令返回 0 但包含空 error 的 receipt 明确不计通过。

完整 core 通过实际 GUI 整体启动，原版 game 自然启动耗时 1301.047 秒，启动后观察 60 秒，再完成 channel 拆包/连包握手、relay 双向转发及真实 monitor 拒绝、STUN 检查。关闭窗口经原版 SIGUSR2 关服，三个 game 关服标记、Frida dispose、DP2 unload 齐全；16 个宿主均自行结束，MySQL 正常关闭，GUI 返回 0，无强制清理，总计 1445.694 秒。只出现已知的 inotify 291/332 fallback，无新增 runtime 缺口。

随后在中文、空格和括号目录中的新私有实例完成 15 个 peer 的两次整体启停：两轮协议检查均通过；单独停止/重启 channel 通过，真实所选日志可见，私有 ELF 被改动时哈希校验明确失败且其余服务保留。停服后自有 PID 文件清空，备份按钮才可用，关闭窗口正常停止全部自有宿主和数据库，无强制清理或 runtime gap。新版 GUI 的全部数据库控件专项也通过，包含初始化、TLS/实例身份、备份、还原、恢复和另一实例占用相同端口时保留原库。

开发期新增只复制原版输入与固定用户库的实例准备工具，拒绝覆盖已有目录，并核对冷数据库复制一致、源库不变。运行准备完成的实例不依赖 Python；产品首次资源导入仍未实现。复现与布局见 [数据库和 GUI 方案](03-database-and-delivery.md)，构建、各次失败、修正及最终证据见 [GUI 服务编排](evidence/p6-gui-services.json)。本阶段仅开发机 Windows 10 实测，不计客户端可玩业务、安全三件套完整拓扑、自动停服维护与重启、插件/Frida 开关、密码编辑、等级切换、最小离线 ZIP 或干净 Win7/10/11 验收。GUI 强制终止后的数据库重新接管仍待完成。

## 2026-11-14 · GUI 从本地资源创建游戏实例

本阶段解决实际首启缺口：此前 GUI 初始化只得到空数据库，服务目录和应用数据仍需开发脚本事先准备。现由开发构建工具生成相邻本地资源，GUI 原生 C++ 直接复制资源、调用已有单文件安全初始化、导入原始应用种子、生成账号与服务配置。未新增运行框架、解释器或第三方依赖；新 GUI 为 940544 字节，位于 `build/gui-setup/`，正常导入依赖仅增加系统 bcrypt。资源模板沿用本项目已验证的原版输入与用户库复制逻辑，保留原版 channel/relay 容量；没有接入参考产物。

通过真实窗口控件，在中文/空格/括号目录中只复制两个 EXE 和本地资源，子进程 PATH 限 System32。创建的新实例先完整导入 18 库/601 表，再导出 977716 字节并与原始种子哈希一致，随后设置应用连接表及随机口令，按当月从原表建立四张拍卖历史表。root、game、bridge、Frida 四个账号在两个新实例中的认证值均不同；game 没有全局权限。口令编码从哈希锁定的原版 DBMW ELF 取材料，沿用此前依据原版指令独立实现并验证的格式；原始种子未导入源系统账号。

新建实例的 15 个原版 peer 实际运行，channel 拆包/连包握手、relay 双向转发与真实 monitor 拒绝未登录账号、STUN 响应通过。原版 Frida 的 init_db 使用新账号建立并查询三张表，专项 fixture 按约定返回 42；随后还原其脚本，全部自有 peer 和数据库正常停止，PID 文件清空，无强制回收。这次没有重复完整 game 自然加载；上一阶段完整 core 联跑保留原有范围，不能混写为新实例客户端业务通过。

本地资源字节被修改时，在写入实例前拒绝创建；已有非空目录保持逐文件相同。创建期间窗口响应为 0.132 毫秒，成功后出现 19 项清单，数据库已停止，未完成标记移除，实例不重复复制 seed.sql。源资源和复制后的资源目录均在测试前后保持相同。相同 GUI 二进制另在无资源目录下通过原有数据库界面专项，覆盖 TLS/身份、端口冲突、正常停库、备份还原、损坏备份拒绝、关闭窗口与未完成还原恢复，返回 0，无强制清理。

复现与布局见 [数据库和 GUI 方案](03-database-and-delivery.md)，构建、资源清单身份与两个专项 receipt 见 [GUI 首次创建](evidence/p6-gui-instance-setup.json)。当前资源未经最终裁剪，1308 个文件共 667534605 字节，包含可选安全文件；不能据此宣布 ≤210 MB ZIP 或 core/full 分包完成。仅开发机 Windows 10 10.0.19044 实测，干净普通用户断网验收、Win7/11、客户端可玩业务、安全三件套拓扑、插件开关、密码编辑与等级切换仍未完成。本阶段提交后等待用户审阅。

## 2026-11-14 · 部分解除映射后的记录回收

关闭 DP2 的 game 初始化曾在约 1710 秒后报告 `mmap guest region table exhausted`。已修复匿名区分次 `munmap` 后所有页面均已解除、但 Windows 预留区和映射记录仍未释放的问题。双架构夹具连续创建、分次释放 1500 个区域，通过真实读写检查；原始 ELF 未变。

这项检查覆盖映射回收原语，不代表 game 自然启动通过。此前的失败现场、修复及检查见[映射回收证据](evidence/p6-partial-unmap-release.json)。后续完整复测另见下方记录。

## 2026-11-14 · 兼容核查与运行开销

只读对照用户指定的 rednf 逆向源码，建立带文件哈希和行号的候选调用索引，核对本项目 runtime 的实际实现边界。未支持的后台派生、SysV IPC、线程复用和取消、完整信号、文件监控与大连接数等见[兼容清单](08-compatibility-and-performance.md)；引用代码和参考构建产物未进入交付链。

game 初始化的主要 CPU 热点是常见 TLS 读取反复触发异常。独立实现 x86 绝对 GS 读取的直接执行路径，保留寄存器、标志和每线程 TLS 语义；game 选择已在固定 CentOS 7.9 glibc 原包中的 nosegneg 变体。相同 TLS 夹具三次对照的中位数约快 15.7 倍，不据此声称整服 CPU 降低相同比例。修复部分 unmap 后，另一次完整 game 仍用尽 1024 个有效映射槽，已将容量提高至 4096，并验证双架构 1500 个同时存活映射的读写与释放。

ELF 原始字节改用只读文件视图，省去每进程的私有堆复制。相同原版 game 在独立冷实例中对照，关闭 DP2 时闲置私有提交从 1410.56 MiB 降为 1341.15 MiB，工作集约 729 MiB；带 DP2/Frida 的完整自然启动约 41.133 秒，闲置 10 秒消耗 0.0625 CPU 秒，并经原版应用完整清理、退出 0。各组 channel/relay/真实 monitor/STUN 对照通过，源私有数据库未变；观察限本机 Windows 10，没有玩家负载。

19 个原始 ELF 的固定地址映射与输入拒绝、双架构 TLS/线程/VM、文件同步和动态加载检查通过。构建、独立对照及边界见[运行证据](evidence/p7-runtime-performance.json)。jbz69 客户端与配套 PVF 已获用户授权，登录、进图和存档仍待实际业务验证；本轮不把闲置数据写成客户端性能结论。

## 2026-11-14 · 原生 GUI 插件设置

同一窗口已接入 DP2/Lua、Frida 开关，配置持久化至现有服务清单；game 运行时禁止编辑，停服后恢复。关闭 DP2 移除其预加载；仅关闭 Frida 则在实例自己的模块入口使用显式环境开关，保留 Lua 与原模块备份，不修改输入资源或用户入口脚本。

最终 GUI 在新建的中文路径实例中通过保存/重开、重复保存、Lua-only、Frida 真正映射与原址 hook/双向 RPC，以及不停止其他 peer 的连续切换。DP2 关闭的 game 无插件映射，自然启动 36.067 秒，随后完整停服、退出 0；关闭窗口正常停止其他 peer 和数据库，无强制清理，源数据库未变。见[插件设置证据](evidence/p6-gui-plugin-switches.json)。

## 2026-11-14 · 本地压缩资源与审阅样件

完整候选资源直接 ZIP 曾超过体积门禁。现按文件内容哈希去重，使用 Windows LZX CAB；原生 GUI 经系统 Cabinet FDI 解压并逐文件核对原清单，不依赖运行时解压工具或 Python，不删减原文件内容。1308 个文件对应 963 个唯一对象，包含优化宿主的 CAB 为 175429927 字节；四个运行文件的普通外层 ZIP 为 190277074 字节，SHA-256 `90911e9ff6db63610acef5b70b3d718c03797216006b24c6de41833e46114468`。

最终 GUI 与 ZIP 成员字节一致。首次创建专项在中文/空格/括号路径中通过损坏资源拒绝、非空目录保留、原始种子逐字节往返、每实例独立账号、15 个 peer 协议及原版 Frida 数据库接口；成功后清除暂存种子和模板，所有进程正常停止，无强制清理。此后从该实例复制并通过完整插件专项。ZIP CRC 通过；尚未执行独立干净系统上的解压验收。

审阅程序位于 `build/review/DNF-Native.exe`，样件位于 `build/review/native-local-resources.zip`。按用户要求清除已完成实验的可重建大副本，先归档并校验诊断，再保留小型 receipt、六个最新完整验证目录及当前产物，build 约 8.51 GiB；输入原件、工具链和源码未动。详情见[压缩资源方案](07-local-package.md)及[来源、检查和清理证据](evidence/p7-local-package.json)。当前停在审阅点，不将样件视为已完成客户端业务、core/full 分包或三系统正式发布验收。

## 2026-11-14 · 客户端 PVF 启动现场与后续工作

继续推进用户授权的 jbz69 客户端与配套 PVF，复现原版 game 在物品动画模板初始化期间的崩溃。现场确认查找 `equipment/character/atswordman.lay` 返回 end 后被原版代码继续解引用；直接原因及证据见[兼容边界](08-compatibility-and-performance.md)和[脱敏记录](evidence/p8-client-pvf-startup.json)。尚未完成客户端登录、进图或存档，模板缺失的来源仍待核对。诊断只作用于私有宿主，输入原件与原版 ELF 未修改，进程正常收回。

独立客户端约 13.2 GiB，后续验证复用这一份。再次归档旧实验日志并校验 ZIP，清除已完成的四组旧 probe、重复冷实例和三份重复 MySQL 构建，释放约 5.3 GiB，build 约 17.4 GiB。清单保存在 `build/cleanup-client-stage-receipt.json`；当前故障现场、一个冷实例、最终插件实例和审阅包保留。

用户追加 GUI 布局改善与原版日志查看要求。后续在本项目 Win32 GUI 内完成，保持原生实现与逐阶段提交；仍需独立验证原始 stdout/stderr、应用自身日志与数据库日志的查看及刷新。

## 2026-11-14 · GUI 布局与原始日志

原生 GUI 改为左侧服务列表、右侧日志、底部数据库操作，统一间距、字体和状态呈现。日志区域直接读取应用 `log/` 文件、stdout、stderr 和 MySQL 日志，支持来源切换、编码选择、自动刷新、跟随滚动与日志目录；控制台操作记录单独显示。只读取原文件末尾至多 256 KiB，原文件不改写，不向应用日志附加 GUI 时间戳。

最终 GUI 的真实专项在已有私有实例通过：夹具验证 UTF-8/GBK、追加、截短、同路径轮转、较大文件截取及滚动保留；随后原版 game 自然启动约 36.798 秒，`.init`、stdout、stderr、MySQL 原生日志可见。channel 握手、relay 双向转发、真实 monitor 拒绝未登录账号与 STUN 检查通过；正常停服后 game 退出 0，日志仍可读，窗口关闭返回 0，无强制回收。已查看常规和最小窗口截图；限开发机 Windows 10。

审阅程序为 `build/review/DNF-Native.exe`，1025536 字节，SHA-256 `7fa963b4a12e1cb3d2b0d62d165e2ecbec7ebb97b35a5b5b74bcf176b62ef967`。同路径 ZIP 已更新为 190328945 字节，SHA-256 `b798482033b6010d9a610f991b7ae208320caf9d8dc2cb4d8790772748a65ef3`；CRC 与全部成员字节哈希通过。资源 CAB、清单和原版 ELF 未变；MySQL 单文件从相同源码及相同官方 CAB 重新构建，并使用本轮实际测试的对应二进制。

详见 [GUI 与原版日志](09-gui-and-original-logs.md)和[本轮证据](evidence/p8-gui-original-logs.json)。本轮没有重跑首次资源创建和数据库备份还原专项，保留先前结果的版本边界。jbz69 当前 PVF 的模板缺失仍未解决，不计登录、进图或存档通过。完成本轮后等待用户审阅。

## 2026-11-14 · 客户端对照与实例地址修复

进一步只读核对当前 jbz69 PVF，确认 54 个 `.equ` 文件引用不存在的 `equipment/character/atswordman.lay`；四份备份仅核对索引/字符串表，未逐份验证引用或启动。为隔离资源问题，在私有客户端和服务端同时使用原包 PVF 作独立对照，原版 game 自然启动 37.092 秒，客户端实际进入 `ch.11` 频道界面。这不是当前定制 PVF 或登录业务通过。

实际频道日志暴露配置生成遗漏：游戏把 `Public IP` 文字当作对外地址。原生新建实例与开发准备脚本现将原包 `Public IP` / `UDP IP` 占位符替换为单机场景的 `127.0.0.1`，兼容现有 CAB，输入原件不变。已有私有实例正常停服后备份并修正，整组正常重启后，channel 实际发布 `127.0.0.1:10011`。单独重启遇到的原版 monitor 重复频道登记拒绝单独记录，没有掩盖为成功。

重新构建后的 GUI 首次创建专项通过：两份实例、18 个应用库与当前月表、原始种子逐字节往返、每实例独立账号、三份配置无占位符、15 个 peer 协议及 Frida 数据库接口。GUI 退出 0，资源未改写，无强制回收。客户端和原对照栈也正常退出 0。后续桌面 BitBlt 错误 6、前台窗口为空，输入检查失败时未发送点击；登录、角色、进图和存档仍未验证。

审阅程序同路径更新为 1027072 字节，SHA-256 `28fc397cf5ba6b1522bc07b123734755b2bbd760a515d34cc67fa718d80ba124`；ZIP 为 190329738 字节，SHA-256 `75ae8cf0a6e7c0a64c7855f6564b88ce64beff939eceac3b4689eaaf75309e01`。ZIP CRC 和成员哈希通过，CAB、清单及 MySQL 与前一版一致。原生日志专项沿用前一提交的独立结果，本轮重跑了首次创建专项；没有重跑备份还原或多系统验收。

归档并逐成员校验本轮可重建目录的诊断后，移除旧冷实例、新测试实例 1 及重复交付资源副本，释放约 2.20 GiB；保留最新修正后的冷实例 2、客户端故障/对照现场、唯一客户端副本与审阅包。清理收据为 `build/cleanup-address-stage-receipt.json`。详见[客户端对照与地址修复](10-client-control-and-local-addresses.md)及[本轮证据](evidence/p9-local-addresses.json)。
