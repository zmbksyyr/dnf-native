# 实施记录

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
