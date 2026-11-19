# 实现与兼容边界

本文描述当前源码行为；具体已验证组合和历史故障见[验证记录](validation.md)。服务执行文件来自固定资源基底，项目自研部分是 Windows 宿主、控制台和数据库维护工具。

## 运行关系

```mermaid
flowchart TD
  GUI[Win32 控制台] --> DB[实例内官方 MySQL]
  GUI --> H32[多个独立 x86 宿主进程]
  GUI --> H64[x64 STUN 宿主]
  GUI --> MT[原生数据库维护工具]
  H32 --> E32[原版 i386 ELF、ld.so、glibc、DP2/Lua/Frida]
  H64 --> E64[原版 x64 ELF 与 Linux 用户库]
  E32 --> ABI[Linux ABI 子集到 Win32/Winsock]
  E64 --> ABI
```

普通指令由同架构 CPU 执行，宿主对 syscall 和 Linux TLS 边界进行适配。每个服务是独立 Windows 进程；DP2/Lua/Frida 和 game 保持同进程指针及 ABI。没有引入 CPU 模拟器、通用全程序翻译或内核驱动。

## GM 入口与可选客户端同步

主界面通过 `CreateProcessW` 打开随包独立 GM，不传实例或连接参数，也不纳入服务 Job。
`game_keys.h` 复用本项目登录器的 CryptoAPI RSA 导出实现，在新实例初始化时准备配对密钥；
Robot 启用、启动复用同一检查，已有公私钥不匹配时失败并保留文件。

`client_sync.h` 读取客户端 `Script.pvf` 和私钥，公钥由实际私钥派生。
同步只在服务全部停止后执行，先备份旧文件、暂存新文件，随后提交 PVF、双方缺失的密钥及
`services.ini` 的 `[client] directory`。失败按相反顺序恢复；备份留在实例 `.mysql/client-sync-*`。
指示灯在后台核对 PVF 的 SHA-256 和公私钥配对，定时发现文件大小或修改时间改变后重新核对，
不在界面线程散列大文件，不自动启动服务、改等级或清理拍卖订单。

## GM 在线命令

桥接已归入独立 [GM 项目](https://github.com/zmbksyyr/dnf-dm-c/blob/main/docs/bridge.md)，
由其 `src/gm_bridge.lua` 自带并导出；Native 仅随包分发 GM，不安装或管理桥接。
下面记录与原 ELF 的接口语义及历史联调行为。模块使用固定原 game 的 DP2 符号接口，不更改 ELF 或新增网络监听。
所有 SQL 在原 luv worker 中执行；完成回调在游戏线程重新查找账号、检查状态与当前角色，
才调用 `CInventory::SetCoin/GetCoin`、`CUser::SendCoin/SaveMoneyCoin`；
金币调用 `CInventory::get_money/set_money` 和 `CUser::SendUpdateItemList(0,0)`。
增加金币前用原 `CInventory::getValidMoney` 检查当前等级／PVF 上限，数值限定在有符号 32 位范围。
`SaveMoneyCoin` 仅表示保存已排队，须另外查到 `inventory.coin` 或 `inventory.money` 等于回执值才确认完成。
补满疲劳读取原 `CUserCharacInfo` 的基础和额外上限，分别设置剩余疲劳、清除 used 计数，
读回后调用原 `CUser::SendFatigue/_SaveCharacData`；基础疲劳须为 1..32767，额外疲劳须不超过 32767，
与原数据库有符号 SMALLINT 范围一致。确认 `charac_stat.fatigue`、`used_fatigue` 和 `premium_fatigue`
全部等于目标后才完成。离线 GM 单条多表 UPDATE 同时核对归属、未删除、离线和有效上限，
补满基础疲劳并保留额外疲劳，不推测离线角色当前的额外疲劳资格。
SP/TP 四组计数限定 0..65535。通过原 `SkillSlot::get_remain_sp/set_remain_sp` 读写，
可写技能访问器标记脏数据；读回一致后调用 `CUser::send_skill_info/UpdateData`，
再确认 `skill` 表四组字段全部等于目标。`_SaveCharacData` 不负责技能存档，不能用于此操作。
沿用原技能序列化，保留已学技能与技能指令；离线同时检查归属、未删除、离线及技能记录存在。
QP 调用原 `CUser::getQuestPoint/setQuestPoint`。设置接口标记扩展数据为已修改并发送 32 位通知，
`UpdateData` 沿用原 `CQuestShop::_saveData` 保存属性、重置计数和任务碎片，确认 SQL 的 QP 后才完成。
按原调度器检查扩展模块 11，避免缺模块时 getter 返回零被误认成成功。
QP 限定为原 getter 的非负有符号 32 位范围；离线允许懒建 QP 行，实际写入同时检查角色归属、未删除与离线，
仅更新 QP，不重置已有属性、重置计数或任务碎片。
`inspect` 命令只读当前角色，供 GM 冻结仅在线泡点收件名单；没有把登录表账号在线标记外推为所有角色在线。

装备栏与装备等级共用 v7 队列。前者调用原 `SetCurExpEquipSlotStat(3)` 开启两个扩展槽，
显式标记角色统计为待保存，再发送原基本信息通知；后者调用原 `SetCurCharacMaxEquipLevel`，
由原 setter 标记管理数据修改，再发送原装备等级通知。原 `_SaveCharacData` 排队存档，
分别确认 `charac_stat.add_slot_flag` 或 `charac_manage_info.max_equip_level` 才完成。
不改原指令或原内存布局；等级限定 1..255，离线允许懒建管理行，只更新目标字段，
保留支援兵角色及技能选择。实际离线 SQL 同时检查归属、未删除、离线，装备栏要求统计行存在。

负重通过 v8 队列设置为原按钮既有目标 99999999。原 `getCurCharacAddInfoW` 标记角色属性待保存，
仅写入属性块内一个有符号 32 位值，再调用原基本信息通知和 `_SaveCharacData`，
确认 `charac_info.inven_weight` 等于目标才完成。字段布局从固定原 ELF 的数据库装载、
角色设置与访问器独立核对：角色内偏移 `0xc2` 减访问器基址 `0x88`，即 packed 属性块 `0x3a`。
没有调整整个角色属性、修改原指令或重建内存布局；其他属性字节保持原值。
原界面此前要求离线，本次增加在线能力；离线 SQL 同时核对归属、未删除与离线，只更新该字段。

负重和装备栏的完整基本信息使用原 `eSendTarget=1`，仅发送给本人。
原目标 0 会将可见角色的报文交给世界广播，其他城镇的在线角色也会收到，不能用于私有属性。
等级操作的公开基本信息仍走原广播路径；其完整属性、等级经验和技能通知只发送给本人。

角色等级通过 v9 队列传递目标等级与累计经验。仅处理游戏状态 3 的当前角色，
范围为等级 1..127、经验 0..2147483647，须与原 `CDataManager::get_level_exp` 当前曲线一致。
按原 `GetSpAtLevelUp` 和等级函数的 TP 规则预演各步，任一技能点超出 0..65535 即在修改前拒绝。
随后逐级调用原 `CUser::SetCharacLevel`，实际等级改变才调用 `_onLevelUp`，再设置累计经验、
刷新账号最高等级，向本人发送原等级经验通知 37、完整属性通知 2 和技能通知 19，
按原城镇路径广播公开基本信息通知 2，由 `UpdateData` 排队保存。
确认 `charac_info.lev`、`charac_stat.exp`、四组技能点与角色金币均匹配才完成。
原在线保存不保证同步 `charac_info.exp`；重新登录从统计行加载当前经验，不强制改写另一份经验。
原保存失败会清除部分脏标记；同等级重试通过原等级 setter 和技能可写访问器重新标记，
经验 setter 重新标记统计行，不再次加点或触发升级事件。离线等级写入在同一事务核对
归属、未删除、离线及统计行存在，更新两张经验表、读回确认，再清除原处罚记录；失败回滚。

复活币／inspect 沿用 v1 队列，金币使用 v2，补满疲劳使用 v3，SP/TP 使用 v4，QP 使用 v5，在线账单刷新使用 v6，装备设置使用 v7，负重使用 v8，角色等级使用 v9；各自独立队列和心跳，防止旧 helper 误处理新操作。
同账号任一队列存在 claimed／applied 时，其他队列不领取，避免原游戏保存时互相覆盖。
MySQL 队列表使用唯一 token，状态为 pending→claimed→applied→done，
或 rejected/unknown。每个队列每 500ms 至多处理 32 条，同账号只领取最早一条，
保存确认前不执行下一条，连续增量由实际游戏内存计算。pending 20 秒过期；
已领取命令、已修改内存但保存未确认的命令均不重放。旧宿主或丢失 worker 的命令
明确终结为 unknown，避免永久阻塞该账号；这不表示原操作一定没发生。
数据库奖励和在线队列在同一事务提交；内存操作在提交后进行，不能声称跨游戏内存与 SQL 整批原子回滚。

点券／赠送点券的权威数据是 billing 表，处理方式与角色内存数值不同。泡点在同一 SQL 事务中增加两类余额、
提交在线账号的 v6 刷新请求，离线账号由下次登录读取。领取请求时读取最新已提交余额，避免并发奖励仍要求
刷新到较早的一笔余额；游戏线程调用原 `CIPGHelper::IPGQuery`，随后在现有计时器内核对两个缓存值，
匹配才调用原 `SendCashData(false)` 并记录 `billing_refreshed`。不修改原版指令，不写入缓存余额或重放增量。
刷新期间保留账号号和截止时间，每轮重新查找当前玩家，不跨异步过程保留玩家指针。
玩家退出、余额持续变化或 15 秒内无法核对时返回未确认；账单已提交的事实与刷新失败分开报告。
v6 直接从 claimed 到 done／unknown，不使用角色字段的 applied→存档确认路径。
GM 显示实际部分结果并停止泡点，避免定时重复发放。登录或切换角色竞态仍由游戏线程复查，拒绝错误目标。

GM 桥接由独立 GM 项目随程序导出脚本、配置和简短说明，Native 不安装或升级该模块。
Linux 与 Windows 宿主使用同一份 GM 模块；`controller/game_compat.h` 只处理原有拍卖兼容。
原 luv 对较大 work function 的序列化曾在两端原游戏中崩溃，因此 worker 仅传短入口，
业务代码在 worker 内 loadfile，不把数据库操作移回游戏主线程。
停用桥接时先停 game，再按 GM 的说明移除入口中的标记段及对应 helper/配置；保留队列与备份供核对。
不要将 unknown 改回 pending 或自动重发；旧备份若包含后续用户改动，不能盲目覆盖整个 DP2 入口。

## ELF 宿主

`src/runtime/main.c` 解析 `--map` 和 `--run`，以及 `--root ROOT INTERPRETER_SHA256`、`--cwd`、`--env`。程序 ELF 和动态加载器均按传入 SHA-256 校验，位数不匹配、错误文件边界或缺失入口会明确拒绝。

`elf.c` 以只读文件视图保留输入，再为执行副本映射 PT_LOAD、零填充 BSS 并设置页权限。ET_EXEC 按原地址映射；ET_DYN 分配独立区域。原磁盘 ELF 不因加载和适配被改写。`run.c` 构造 argc/argv、环境、auxv 和初始栈，再经 `entry.S` 进入原动态加载器或程序入口。

主线程的 guest 栈为独立 8 MiB 私有分配，与 clone 的 guest 栈一样和原 Windows 栈
分离。`madvise` 在 VEH 返回后转到该线程原 Windows 栈执行，再通过既有现场恢复
路径返回 guest，避免 glibc 丢弃自己旧栈页时覆盖 Windows 的 syscall 处理帧。
线程退出也先切回原 Windows 栈，再处理 robust list、clear_tid 与唤醒，最后执行
Windows 线程清理；唤醒 join 后不再使用 guest 栈或其 TLS。

匿名页丢弃保持原页权限，短暂 decommit 期间的并发合法访问可在映射锁后重试。
每个线程对同一重置代次最多重试一次，并复核页仍映射、已提交且允许该访问；
真实保护页、空洞、非法写入及未解决的错误继续交给 guest 信号或故障诊断。
该行为依据 [Linux madvise(2)](https://man7.org/linux/man-pages/man2/madvise.2.html)
和 Windows 页提交接口独立实现，并用相同 ELF 夹具对照原 Linux 行为；没有引入参考兼容层。

`zero.c` 从主 ELF 的动态符号、JUMP_SLOT 重定位及标准 PLT 推导 `memset` 跳转位置，核对原文件与执行副本的完整跳转指令，再仅在执行内存接入；日志记录 ELF 哈希、地址、原字节与恢复来源，不修改原 ELF。首调用及非优化路径执行原 GOT 跳转，保留惰性符号解析；仅在目标已解析到 libc、填充值低字节为零、长度至少 64 KiB 且整个目标属于可写私有匿名映射时，对内部完整页使用 `MEM_DECOMMIT` 后 `MEM_COMMIT`，由 Windows 保证后续读取得到零，边界不足一页的部分实际清零。映射锁覆盖检查与重置，任何失败明确终止并记录；不使用无法保证内容清零的 `MEM_RESET`，不清理工作集来制造短暂下降。共享页、文件 COW、可执行页及其他填充值仍由原 libc 处理；有 `LD_PRELOAD` 的程序完全保留预加载路径，避免绕过插件对 libc 的插桩。没有标准 PLT 的 ELF 正常沿用原路径。该优化降低常驻物理内存，不缩小连接池、虚拟地址范围或提交量。页语义依据 [VirtualFree / MEM_DECOMMIT](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualfree) 和 [VirtualAlloc / MEM_COMMIT](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc) 独立实现。

`time`、`gettimeofday` 和 `CLOCK_REALTIME` 继续从 Windows 实时时钟取得 Unix UTC 时间戳；单调时钟继续使用性能计数器，不将本地时区偏移加到时间戳。`run.c` 启动 ELF 时，若没有显式 `TZ`，通过 `GetTimeZoneInformation` 读取本机规则，并由 `timezone.h` 转为 POSIX `TZ` 交给原版 glibc；固定偏移、分数小时、南北半球周期夏令时均保留。该默认值不占用原有 32 项显式环境槽。显式 `--env TZ=...` 优先；按绝对年份指定的转换规则明确报错，当前实现不生成历史时区数据库。时区规则在服务启动时读取，更改 Windows 时区后需重新拉起服务。MySQL 包装器的所有 mysqld 启动路径统一追加 `--default-time-zone=SYSTEM --log-timestamps=SYSTEM`，新实例配置也写入这两项；既有实例无需改配置。GUI 日志原本已使用 `GetLocalTime`。旧日志与已存数据不回写。规则依据 [Windows TIME_ZONE_INFORMATION](https://learn.microsoft.com/en-us/windows/win32/api/timezoneapi/ns-timezoneapi-time_zone_information) 和 [POSIX TZ 环境变量](https://pubs.opengroup.org/onlinepubs/9799919799/basedefs/V1_chap08.html) 独立实现。

`nr_memory` 的缓存只记录有效范围和页权限，不缓存内存内容。大块清零在同一映射锁内
将已核对的私有可写页还原为相同的已提交、可读写状态，因此保留权限缓存；失败会
明确终止。改变映射、保护或 brk 的路径只使重叠范围的缓存失效，防止沿用已撤销权限。
非固定地址 mmap 新建分配、msync 不撤销既有权限，不清空其他区域缓存。
检查和修改仍使用同一映射锁，失败的部分修改也清除涉及范围。

`adapt.c` 使用固定 Zydis/Zycore 解码，记录原 syscall/TLS 站点并适配加载副本。x64 syscall 和常规 TLS 陷入经 Windows VEH 分发；x86 `int 0x80` 在异常边界处理。常见 x86 GS 绝对读取、MOV 写入、ADD 读取、32 位 CMPXCHG 及间接调用可经每线程 Windows TLS 槽直接执行，每次重新读取当前 TLS 基址与调用目标。MOV 写入和 ADD 覆盖 glibc 线程清理链、errno 查询；CMPXCHG 覆盖 glibc/pthread 取消状态的高频原子更新，保留原 LOCK 前缀和 CPU 比较交换语义，不在宿主中模拟读改写。跳板保留真实 CPU 标志及无关寄存器；间接调用保留原寄存器、标志、返回地址和一个返回字的栈布局，避免重复异常解码与刷新指令缓存。其他已支持的 TLS 由 `tls.c` 处理，未实现站点记录地址并失败退出。

`clone` 通过原子操作预留独立线程槽，再设置子线程现场及创建 Windows 线程；创建失败释放预留。不能用尚未填入的线程句柄判断槽是否可分配，否则同时调用的父线程会覆盖同一份 TLS 和寄存器现场。现有 64 槽及退出后不复用的限制保持明确。

x64 TLS 跳板须分配在目标地址的 RIP 相对范围内。VirtualQuery 后若地址被其他线程抢占，
仅前进一个 Windows 分配粒度后重新查询，不能按过时的空闲区长度跳过其余可用地址。
TLS 失败记录使用直接 WriteFile 的既有日志路径，避免 MSVCRT 写流时按原 Windows 栈界限
探测已切换的 guest 栈而掩盖真正原因。未初始化或不支持的 TLS 仍明确失败退出。

固定 x64 STUN 的历史失败站点已定位为原版 glibc 2.17 `start_thread+0x1b` 的 FS 写入。P15 以当前宿主完成三个原版 pthread worker 的协议与自然停服，结合原版控制流确认该站点路径已覆盖，沿用现有 TLS 实现；通用 ELF 的其他 FS/GS 指令仍以实际支持和验证范围为准，详见[验证记录](validation.md)。

原始 manager 的 GCC PIC 跳转表、特定 Frida 数据/指令和 packed image 处理都有局部识别条件；不能把这些有限适配外推到任意 ELF。所有分发路径的 ENOSYS 记录服务、位数、线程、PC、参数、模块哈希和首次/重复计数；按 1/2/4/8 次采样，返回值保持 ENOSYS。`diagnostics.c` 在 guest 异常边界记录寄存器、原始指令字节、可读局部栈及最多 16 层 frame-pointer 帧；没有帧指针或符号时明确保留未解析地址，不声称完整展开。`tools/elf_manifest.py` 可离线生成资源 ELF 的段、TLS、依赖、重定位类型和未定义动态符号清单，输出必须在 x 下。

| 模块 | 职责 |
| --- | --- |
| `diagnostics.c` | ELF 模块身份、符号定位、重复 ENOSYS 和异常现场；以加载器/VM 原表为准 |
| `run.c` | Linux 初始上下文、异常分发、基础 syscall、内存有效性检查 |
| `vm.c` | mmap/mprotect/munmap/madvise/msync 和有限 x86 mremap；匿名区支持 DONTNEED 的保留映射语义 |
| `files.c` | 实例根目录映射、文件/目录/锁、部分 procfs 和共享文件 |
| `ipc.c` | eventfd 64 位计数、阻塞/非阻塞、信号量及 poll/epoll/select 就绪联动 |
| `net.c` | TCP/UDP、select/poll、LT epoll、已支持的 socket 选项 |
| `thread.c` | 已观察 pthread clone、TLS 接入、futex/wake/requeue 子集及非 PI robust owner death |
| `signal.c` | 当前服务所需处理器、掩码、SA_SIGINFO/SA_RESTORER/SA_NODEFER、嵌套帧及上下文恢复 |
| `tls.c` | Linux FS/GS TLS 指令适配和返回路径 |
| `service.c` | 固定 DBMW/manager ELF 的网络线程停服顺序修正，独立于 syscall 兼容语义 |
| `game.c` | 固定 game 的等级规则及成长奖励包读取修正；进入原程序前校验并修改执行内存 |
| `pvf.c` | 固定 game ELF 的可选视觉模板、宠物列表编号及自动脚本编码适配，独立于 syscall 兼容语义 |

映射表最多 4096 项，记录已使用前缀并复用空槽。VM 修改、内存检查和异常读取使用同一进程内临界区，防止 guest 线程争用映射槽。文件、socket 和 eventfd 共用 `NR_FD_SLOTS=1024` 项描述符，`RLIMIT_NOFILE` 与分配、文件映射、select/poll/epoll 使用同一上限；Winsock 集合容量在编译时核对。该容量满足固定 relay 配置的 1000 个客户端及其内部描述符，实际验证见[容量记录](validation.md)。线程表仍为 64 项，描述符耗尽继续返回 `EMFILE`，不支持任意扩容。

所有描述符创建路径在同一临界区内选择并预留空槽，再初始化资源并发布实际类型；创建失败释放预留槽，关闭时先清理元数据再释放槽。eventfd 的原有创建锁与此锁一致，防止 epoll、socket、文件、管道和 procfs 并发分配同一个 fd。epoll 销毁与关闭 fd 时移除监听的遍历也使用该锁，避免遍历已释放的监听表；阻塞 I/O 不持有该锁。

`/proc/self/maps` 在同一内存锁内从现有 ELF/VM 表生成快照，结合 Windows 实际页权限和部分解除映射的位图，保留 PROT_NONE，排除空洞及已释放区域。共享文件显示共享标志、UTF-8 路径、文件偏移和与 stat 一致的 inode；关闭描述符后仍保留映射身份。它描述本宿主管理的 guest 区域，不枚举 Windows 自身模块。

普通非执行文件的 `MAP_PRIVATE` 使用 Windows `PAGE_WRITECOPY` / `FILE_MAP_COPY` 视图；只读 fd 也可获得私有可写页，写入不会回写文件或其他私有视图，也不移动原 fd 的文件位置。4 KiB 文件偏移通过对齐 Windows 64 KiB 视图并保存实际 view 起点实现；最后一个文件页的 EOF 后部分为零。ELF 依赖仍使用原有 eager snapshot 和指令适配路径，包含 BSS 的预留长度不受普通数据文件的 EOF 限制。普通文件的执行映射和完整超出 EOF、需要 SIGBUS 的页明确返回 ENOSYS。

`munmap` 遍历所有相交 VM 记录，允许跨相邻映射及已解除的空洞。局部释放保留页位图；匿名页 decommit，文件视图页设置为不可访问，直到该记录全部解除后释放实际 Windows view。guest 内存检查和 maps 均排除这些页。程序、解释器、初始栈和 brk 堆的解除仍明确拒绝；跨记录 MAP_FIXED、通用地址复用不由这项修正提供。

`SA_NODEFER` 允许同一未屏蔽信号在 handler 内再次投递，但 action 中的显式 mask 仍然生效。每层 handler 保存独立中断上下文、掩码及 guest 帧，`sigreturn` 逐层恢复。guest 模块执行中的 Windows 访问异常同步交给该线程已注册且未屏蔽的 Linux SIGSEGV handler，填写 `si_addr`、MAPERR/ACCERR、页错误编号和 ucontext；handler 可修复内存或修改 PC 后恢复。未注册或屏蔽时保留原崩溃诊断与退出 125；备用信号栈、浮点扩展状态、其他异常类型与纯计算中的异步信号仍未补齐。

控制台及跨进程事件传入的 SIGUSR2/SIGTERM 优先由未屏蔽该信号的 guest 初始线程处理；该线程屏蔽时，其他未屏蔽线程仍可处理。这样进程停服不再由执行 syscall 最密集的工作线程抢先处理，避免原游戏日志函数被同线程停服 handler 重入。投递仍发生在现有 syscall/异常返回边界，不提供阻塞 Windows 调用或纯计算过程中的任意即时中断；线程自发的 `tgkill` 和同步故障不由此改投初始线程。

已有的宿主主线程在等待 guest 线程期间，用 `WaitForMultipleObjects` 同时接收这两个外部事件，原子发布待决标记；syscall 返回只读该标记，有信号时再原子取走并进入原有投递逻辑。没有可投递信号时直接返回，不再扫描全部 64 位。初始 guest 线程退出后，等待其他 guest 线程期间仍接收事件，无需增加后台线程。等待失败记录真实错误并终止本次运行。普通文件 `write/writev` 保留描述符、访问权限和实际 `WriteFile` 错误检查，去掉写入前重复的 `GetFileType` 查询。

i386 的既有绝对 `GS:16` CALL 跳板可直接执行 `time(NULL)`、`getpid`、`gettid` 查询、普通磁盘文件 `write`，以及非阻塞 socket `read` 和 `socketcall` 的 recv/recvfrom。仅当实际调用目标为固定原 glibc 2.17 loader（SHA-256 `12d9ecb8cd7ddc7491b2726d0bc5a2a47f4364646e49da115b7c7b4548535dfd`）的 `0x1130` 网关，且该处仍为 `cd 80 c3` 时启用；不改 loader 指令或 TLS 数据。跳板保存全部 GPR、ESP、标志、x87/XMM/MXCSR，清除 DF 后调用原有处理函数，再完整恢复。

网络直接调用仍共用 `nr_net` 的参数、指针、长度、收发与错误处理；在同一份参数快照上判断资格。要求描述符为非阻塞或调用带 `MSG_DONTWAIT`，发送在跳板内直接保留原入口，避免每次广播额外保存浮点状态和尝试不符合条件的快速调用，SIGPIPE 仍沿用完整上下文投递。进入后保留已观察到的非阻塞语义，不因其他线程更改描述符模式而等待。磁盘类型在打开/接管句柄时缓存，关闭和复用时清除，写入检查仍共用原路径。管道、socket 的普通 `write`、eventfd、合成描述符和可能阻塞的网络调用保留原分发。存在待决信号、开启 trace、非空时间指针、其他 syscall、改变回调或原入口被插桩时仍进入原网关。普通 `int 80`、x64 和其他 loader 不因此变成直接调用；TCP 复位接收缺口未由此解决，扩展浮点状态仍受本项目既有支持范围限制。

普通句柄的 `write/writev` 将 Windows `ERROR_BROKEN_PIPE` 和 `ERROR_NO_DATA` 映射为 Linux EPIPE，并向当前写入线程排入 SIGPIPE。信号仍按原有屏蔽、合并、忽略、handler 或默认终止规则处理，不能只返回错误码而遗漏信号。

设置 `DNF_NATIVE_EPOLL_TRACE=1` 后，运行时会把 epoll 创建、`EPOLL_CTL_ADD/MOD/DEL`、等待结果以及实际返回事件的 guest fd/data 写入标准错误。该开关默认关闭，用于把事件中的 fd、revents 和用户数据关联起来；不改变事件处理语义。

`service.c` 对固定原版 DBMW 和 manager 的 `CApplication::Free()` 增加局部停服适配。原版 `stop()` 仅清除对象 `+8`，TCP dispatch 实际循环标志在 `+0x2c`；原版清理还在工作线程退出前删除 handler，且没有调用已有的 `pthread_join`。这属于应用生命周期缺陷，不能通过改变通用线程或网络 syscall 语义修复。

适配先核验完整 ELF SHA-256、原地址、完整待改写 MOV 的旧字节及 join 函数入口，再仅在执行内存中将 MOV 的前两个字节改为 UD2。异常分发等待已创建线程完成初始标志设置，清除 accept/TCP/UDP 的实际循环标志；DBMW 通过正常关闭其 UDP 描述符取消阻塞接收，manager 使用其原有一秒 select 返回路径。保留所有对象至工作线程自然退出，随后逐一调用原版 guest join，最后执行被替代 MOV 的原始效果并继续全部原版 Free/析构。超时、布局不符或 join 失败均记录 `service shutdown failed` 并以 126 退出，不继续释放仍在使用的对象。

| ELF | SHA-256 | 适配 PC / 原始 MOV |
| --- | --- | --- |
| DBMW（guild/mnt/stat 同一 ELF） | `0979ef164615e5e3eae006fde74ca2a39a920367dae6a143847dbc0a8d49a967` | `0x806c0c2` / `c70424cdaa1a08` |
| manager | `74b3adba3c2d376547ee65c53940129bd20af9e48926be3f4facc7d729dd65e3` | `0x805e3b4` / `c704241d6e1508` |

适配记录写明哈希、PC、旧字节、替换字节及 `scope=execution-memory`，停服记录各 worker 的 stop/join 结果。磁盘 ELF 和 ZIP 基底保持原字节；回退宿主到此修正之前的构建即可恢复原版执行行为。具体范围不推广到其他服务或任意版本 ELF。只读参考源码提供对象关系线索，地址、布局与指令均重新从上述原版 ELF 核对；未使用参考实现、补丁或生成器。

`pvf.c` 仅针对 game SHA-256 `da417c5e65918f655afcddb6edfdaf2fa1e77b2f208e5aa397f021fa23b79979`，核验原文件和执行副本的地址、完整旧指令及返回位置后，在执行内存适配原有三处：`0x089c1d31` 的模板结果保存、`0x089c1e80` 的动作路径展开入口、`0x0833ccdf` 的宠物解析成功分支。前两处保留原版旧层提交、clear、显式层号和变体名赋值；当视觉模板不存在、装备明确给出正层号、职业为原版 0–11 且模式为 0–2 时，不展开动作路径。这项规则不检查 PVF 身份或模板名称；有效模板继续原版处理，缺失模板需要补充层号或上下文未支持时明确失败，退出 126。没有注册伪模板、借用其他职业模板、删除装备或默认返回成功。

原版 `clear()` 漏清动作字符串 `+0x5c`。缺失模板分支在原元数据赋值后，通过 guest 调用原 `std::string::operator=(char const*)`（`0x08708720`）将该字段清空，再返回原 epilogue（`0x089c2826`）；其余动作字段和向量已由原 clear 清理。调用沿用 guest 参数区、原空串与原 COW 字符串所有权，入口指令及空串也先核验。这样交替加载有效和缺失模板不会残留上一层动作路径，层号与 `+0x60` 变体标识保持不变。

宠物列表允许多个编号共用脚本路径。原导入函数按文件名反查编号，只能得到第一项；适配在原导入成功后恢复当前列表项的真实编号，再执行原版属性转换、装备绑定、数据库同步及查重注册。P23 独立验证了指定 PVF 的 243 个编号和 243 个不同对象。编码默认在 `loadStringTable` 的 `0x088bb551` 读取原版 PackSet 已解密的字符串表，核对计数和偏移后，仅对字符串跨度抽样，按 Windows 严格 GBK／BIG5 解码、常用汉字和私用区字符评分，记录分数与结果。纯 ASCII 保留 BIG5；差异不足或表结构损坏明确失败。`DNF_NATIVE_SCRIPT_ENCODING=GBK`／`BIG5` 继续作为显式覆盖。该位置完整原指令 `8b45f0`、原编码常量和原字符串赋值入口均先核验；通过 guest CALL 调用原 `std::string::operator=(char const*)` 更新脚本编码对象，再执行原 MOV。继续由原 Linux iconv 转换，不改写输入 PVF、数据库编码或客户端编码，也不使用 PVF 哈希白名单。所有修改记录哈希、原字节、替换字节及执行内存范围；结束进程或回退宿主即可恢复，PVF、磁盘 ELF 和固定资源基底不变。

## 等级规则与成长奖励

固定原版 `CMailBox` 构造函数 `0x085579b2` 只构造容器，没有初始化加载标记、邮件/信件起始编号及计数。堆内存复用时，这些值会进入原版 SQL 的 `postal_id > ...`、`letter_id > ...` 条件，使真实附件被过滤。`runtime/game.c` 在核对上述固定 game 哈希及完整原序言后，以 `UD2` 接入现有异常入口，只在构造时将对象前 16 字节和 `0x7c..0x83` 的标量字段归零，重放原 `push ebp; mov ebp,esp` 后继续原构造函数。链表、映射、20 格附件池、数据库查询和领取路径仍由原版执行；没有针对 PVF 或物品编号分支。补丁记录原/新字节，退出或回退宿主即恢复。

`src/game_levels.h` 定义 13 个等级选项；GUI 将 `[gameplay] max_level` 传为 game 的 `DNF_NATIVE_MAX_LEVEL`，缺省 70。`runtime/game.c` 仅接受固定 i386 game SHA-256 `da417c5e65918f655afcddb6edfdaf2fa1e77b2f208e5aa397f021fa23b79979`。进入程序前同时核对磁盘与执行内存中的完整原指令，再修改等级立即数；未知版本、旧字节不同、重复或无效配置明确拒绝。日志保存身份、地址及旧/新值；进程退出释放映射即恢复，不生成替换 ELF。

以下是依据基底原指令、函数用途独立确定的 15 处操作数，L 为所选上限；十六进制 46/45 分别为原 70/69。SP 初始化/读取、升级事件、状态经验边界、升级道具及角色上限同时调整。

| 操作数字节地址 | 原值 → 新值 | 原函数 |
| --- | --- | --- |
| `0x08360c3b`、`0x08360c79` | `46` → L | `CDataManager::set_reward_sp` |
| `0x08360cc4` | `46` → L | `CDataManager::GetSpAtLevelUp` |
| `0x08662f55`、`0x086630f3` | `46` → L | `CUser::_check_level_up` |
| `0x086638f6` | `46` → L | `CUser::_onLevelUp` |
| `0x08665d28`、`0x08666e9c` | `45` → L−1 | `CUser::increase_status` |
| `0x0866a4a8` | `45` → L−1 | `CUser::gain_exp_sp` |
| `0x0866a659`、`0x0866a929`、`0x0866a941` | `46` → L | `CUser::gain_exp_sp` |
| `0x08689d4b` | `45` → L−1 | `CUser::CalLevelUpItemCheck` |
| `0x0868fece`、`0x0868feda` | `46` → L | `CUser::SetUserMaxLevel` |

基底 `Dispatcher_Select_Item_Grwoth_Power::read` 在 `0x081db4ea` 的 `74 00` 使成功读取也落入错误块。原正常继续地址为 `0x081db512`，按相对分支公式计算偏移为 26，执行内存修正为 `74 26`。合法选择继续解析；空包和截断选择仍返回原 `0x4a55`、`0x4a5a`。这属于固定原程序的包读取错误，与具体 PVF 无关，不跳过读取或错误检查。

用户提供的 13 份等级 ELF 只作为只读差异证据：70 级版已有 76 字节、35 组与等级无关的改动，其他版本再增加 15 个等级字节。项目不接入这些替换 ELF、不重放共同改动；来源差异见[第三方记录](third-party.md#用户提供的等级补丁)。本功能不重建经验/SP 表或客户端内容，也不回写既有角色等级、经验。

## 控制台与实例

game 的应用初始化状态取自当前宿主本次创建的 stdout，逐段读取原版 `server has been started successfully.` 完成标记，并保留跨段匹配所需的尾部；不会读取旧 `.init` 或上次启动的 stdout 来更新状态。每次启动清零读取位置和初始化标记，退出后显示宿主已退出。此层级确认原程序报告初始化完成，接口、协议和业务验证仍独立；其他服务没有经确认的原版完成标记，继续显示运行时通道层级。

`src/controller/controller_gui.cpp` 使用原生 Win32 控件和一个后台操作线程，操作进行时限制冲突按钮。共享文件接口在 `src/win32_files.h`，支持 Unicode 路径和私有文件权限；ZIP 通过 miniz 和 Win32 HANDLE 解压。

实例路径由共享 `instance_directory()` 规范化，GUI 与 `mysql-control` 使用相同入口。原版 MySQL 5.7.44 无法使用本次实测的扩展长工作目录或长 `datadir`；根路径达到 `MAX_PATH` 时查询已有的文件系统短名，只有结果短于该限制才用于运行。长短名仍指向同一物理目录，实例锁、UUID、数据和冷副本均保留原位；没有创建目录链接、调整系统设置或更换数据库。文件夹选择器通过 `SHGetNameFromIDList` 取得路径，并恢复完整长名供显示。根路径未达限制时仍按普通绝对路径处理；任意子路径长度、无短名卷和 UNC 支持不在该修复的保证范围内。

当前 GUI 按用户指定的 `Desktop/win32/preview.png` 视觉规格独立重排为四块：顶部实例控制、7 列服务卡片、独立 MySQL 横条和日志工作区。19 项服务保持 `services.ini` 顺序与身份，不引入参考演示模型。原生 LISTBOX 通过 `LBS_OWNERDRAWFIXED` 持有字符串与选择；原生逐项绘制通知由网格统一接管，避免选中时短暂露出系统蓝色条。自绘、鼠标命中、`LB_GETITEMRECT` 和方向键共享同一行优先网格；19 个服务各有按卡片矩形注册的原生 tooltip，文本由 UI 字符串快照中的服务名映射到简要用途，仅显示功能说明，缩放或实例切换时更新命中区域，空白区域不注册提示；操作期间绘制使用 UI 线程的字符串快照，不读取工作线程正在修改的服务对象。移除旧分组、折叠和功能筛选。服务区仅保留“功能”按钮，等级和插件控件实际创建于该窗口；右键菜单仅保留“启动所选”“停止所选”“检查资源”三个四字动作，选择卡片直接查看日志。菜单与命令处理共享当前选择、运行状态及数据库依赖判定。顶部“检查资源”同时将所选服务的元数据、根目录、CWD、ELF 哈希、架构、停服信号及就绪层级输出到主日志，原服务详情按钮和信息弹窗已移除。目录与 IP 输入框分组留有独立间距，IP 标签始终显示。MySQL“详情”显示数据库身份、逆序停服计划及维护结果；实例初始化与删除收至顶部菜单。日志保留原生文件选择状态，通过标签和文件菜单操作，新增只作用于显示的关键字过滤。

窗口与服务网格使用 GDI 双缓冲，主窗口使用 `WS_EX_COMPOSITED` 合成子控件；调整大小时用 `BeginDeferWindowPos`／`EndDeferWindowPos` 批量更新控件位置，禁止中途重绘和旧像素拷贝，再同步刷新。重复点击已选模块不重新触发选中通知或日志扫描。按钮按鼠标、按下、禁用和键盘焦点绘制，保留原生回车及 Tab 行为。尺寸按客户区及 DPI 重算，清单声明每显示器 DPI 感知，DPI 变化同时重建字体；先对逻辑行坐标缩放，避免 125% 时逐行舍入累积越界。当前最小窗口为 1180×760 DIP；模拟 DPI 检查不能代替实际多显示器或其他系统验证。参考工程保持只读，未复制、编译或接入其源码、模拟业务及构建产物。P19 的原始构建、截图和验证收据位于仓库外 `../x/dnf-native-output/P19-20261116-reference-gui/`。

`instance_setup.h` 从 v4 清单校验两个 ZIP、两个宿主、数据库维护工具和独立 Robot 模块的身份。新建仅接受空目录，解压后逐文件核对；官方 MySQL 安全初始化后导入 18 个应用库，重新导出与种子 SHA-256 比对。随后生成每实例应用账号、更新数据库连接、准备当前月表、写入服务配置，正常停库后提交实例。失败保留 pending 标记和私有诊断。

新资源包以一个 Linux 目录树释放到实例根目录，服务清单 `root=.`；`home/neople/` 汇集所有服务，`dp2/` 保存插件，`lib/` 与 `lib64/` 保留 Linux 原有库位数区分。已有拆分目录实例继续由原清单解析，不自动迁移用户数据。

`services.ini` 保存 19 个服务的宿主位数、根目录、原 ELF 与 loader 哈希、参数、环境、停服信号和自动启动标志。顺序是 bridge、manager、三个 DBMW、monitor、guild、statics、coserver、community、auction、point、channel、relay、stun、game；zergsvr/secagent/gunnersvr 默认不自动启动。

安全组件已用固定原版 ELF 实测加载，但资源中的拓扑不完整：zerg 默认向配置服务取信息；改用原版支持的 `cfgfile` 后，`svcid.xml` 的完全重复条目会被拒绝。仅在诊断副本去重后，安全监控 `[31|570001]` 与 zerg `[30|570011]` 仍共用 `127.0.0.1:9000`，形成自连，配置端点 `[2|1]` 的 9001 没有实际对端。DNF monitor 与这个安全监控角色不同；产品保留原配置及三个独立入口，没有发布不完整的诊断拓扑。

`service_processes.h` 先以挂起状态创建宿主、加入本窗口的 Job，再恢复执行并等待停服通道。该通道接入后显示“进程运行中”；game 再依据本次 stdout 更新为“应用已初始化”。正常停止使用本项目命名事件向原程序交付信号；STUN 另发 UDP 数据唤醒阻塞接收。超时保留进程和依赖供诊断。正常停服后仅清理由本窗口已退出宿主拥有的 PID 标记。在线计数每 500ms 查询本窗口服务 Job 的实际进程，再以父 PID 筛选直接启动的服务宿主，排除控制台辅助进程；UI 绘制仅使用 PID 与告警快照，避免与后台关闭进程句柄竞争。请求停服后的退出码保留原值：固定 CApplication 模块返回 1 且本轮日志包含 `Application Stop!` 时按正常停止显示，其他非零退出和超时继续标红。

实例选择持有独占 `.instance.lock`。成功打开有效实例后，立即以 UTF-16 配置原子保存列表及最后选择到当前用户 `%LOCALAPPDATA%\DNF-Native\instances.ini`，不要求启动数据库或服务；启动时恢复，`--instance` 显式参数优先。列表去重并保留最多 64 个路径；删除实例同步移除记录，启动时跳过已不存在的实例。配置保存错误写入主日志，运行包与实例配置均不承载这份界面设置。删除仅允许已识别、已停止的真实实例目录，需要确认；递归处理普通目录，遇 reparse point 只移除链接自身。资源 ZIP 和外部原件不作为运行实例写入。

键盘导航沿用 `IsDialogMessageW` 和原生控件。自绘按钮在 `WM_GETDLGCODE` 中声明当前焦点按钮的回车语义，处理 `BM_SETSTYLE` 时保留 `BS_OWNERDRAW`，避免 Windows 切换默认按钮时恢复普通灰色样式。只读多行日志/详情框对 Tab 释放 `DLGC_WANTALLKEYS/WANTTAB`，保留其他文本选择和滚动行为。删除和还原使用默认“取消”的确定/取消框；回车默认取消，Esc 由原生模态框取消，不为主窗口增加直接退出快捷键。

IP 更新集中于 `service_ip.h`，修改前备份原配置；DP2/Lua/Frida 环境由服务清单和插件模式生成。`log_view.h` 与 GUI 仅读取原日志末尾至多 256 KiB，支持编码、轮转、截短、跟随及显示过滤，不改写原始日志。日志控件不设置可见滚动条，显式处理 `WM_MOUSEWHEEL`，累积不足一格的滚轮增量，遵循系统行数／整页设置，并通过 `EM_LINESCROLL`／`EM_SCROLL` 滚动。消息循环将日志上方的滚轮转交到日志控件，键盘焦点无需离开其他控件；向上滚动关闭跟随，后续刷新保留阅读位置。控制台操作记录单独显示。

## 数据库维护

`src/database/mysql_control.cpp` 调用实例自己的官方 MySQL 5.7.44 工具。产品不安装服务，也不扫描和复用本机其他 MySQL。GUI 运行时通过 SQL 核对版本、server UUID、本次 PID 文件名及 TLS，确认属于本窗口的数据库后才发正常停库命令。

`mysql_operations.h` 实现：

1. **备份**：确认数据目录已停止，以禁 TCP 的独立共享内存端点启动维护数据库，逻辑导出全部库、账号、存储程序、事件和触发器，正常停库后写入带哈希的清单。
2. **还原验证**：验证备份、连接配置及实例 UUID，先保存当前逻辑预备份，再逐文件复制并核对冷数据到影子目录；只在影子副本中移除旧应用库并导入。
3. **提交**：重启影子库、重新导出并逐字节核对逻辑备份，写 `restore.pending` 后保留旧冷目录、切入新目录，启动核对身份并正常停止，再删除 pending 标记。
4. **恢复**：失败或中断时通过 pending 日志恢复原冷目录，保留被拒绝的数据和诊断；解析无效的日志会明确失败，不猜测恢复。

MySQL 的 NTFS 配置使用 `lower_case_table_names=1`。历史原库导入和二进制/存储程序往返另有独立证据，不能据一次简单 SQL 查询推断所有数据库语义。中文 defaults 路径使用实例工作目录内的相对配置名。

`controller/monthly_tables.h` 根据原服务实际列清单维护 `taiwan_cain_auction_gold`、`taiwan_cain_auction_cera` 的 `auction_history_YYYYMM`（26 列）、`auction_history_buyer_YYYYMM`（8 列），以及 `taiwan_cain_log.log_response_time_YYYYMM`（6 列）。月份由 MySQL `NOW()` 计算，当前月与下月均准备，不再从不完整种子表 `LIKE` 复制。响应日志使用 `channel_no,occ_time,packet_id,packet_count,total_response_time,avg_response_time`，旧种子同名表实际是其他统计结构。

新实例导入后、数据库启动后及 game/auction/point 启动前运行月表维护；持有数据库时每 15 分钟由单一后台操作线程复查。运行中允许建立缺失月表；修改既有结构要求这三个写入服务均停止。拍卖表仅补缺列；不识别的关键字段结构明确拒绝。响应表通过原表改名保存、正确新表原子换名处理。每次修改既有表前由官方 `mysqldump` 导出并记录 SHA-256，预先写回滚 SQL；迁移后核对字段，核对失败时执行回滚，失败现场与恢复 SQL 保留。拍卖回滚先改名保留迁移后表，再导回原结构和数据；响应回滚交换原表名称。月表维护不删除历史月份；主动重置市场数据库是下面独立的红字操作。

## 双拍卖维护

`controller/auction_window.h` 提供维护窗口，复用主窗口按钮、开关和单一后台线程；`auction_maintenance.h` 管理检查、备份、迁移与恢复；`auction_pvf.h` 独立读取 PVF；`runtime/auction.c` 适配固定原版 i386 auction／point。功能与作用域见[使用说明](README.md)。配置保存到已有 `services.ini [auction]`，保留原服务入口等字段；`extend_levels/extend_refine/dp2_guard/auto_check` 是选项，`level_limit/refine_limit` 是应用后的数值。宿主环境由控制台生成。

维护窗口摘要由两条单行 STATIC 组成：PVF 路径采用路径省略并提供完整路径提示，第二行显示 auction／point 状态。下方为运行设置、兼容设置及一个合并维护面板；运行设置按等级下拉框、DP2、Frida 从左到右横排一行，应用并重启按钮靠右对齐，不再创建独立插件保存按钮及说明文字。一次工作线程快照同时保存等级与插件模式；game 运行时只停、启一次，其他服务不重启；停服时保存供下次启动使用。无变化时禁用应用按钮。写入前核对固定 ELF 哈希与 Lua 入口，先在实例 `.mysql/runtime-settings-*` 保留配置和需要修改的模块；写配置失败恢复原文，错误写入主日志。八个维护动作按两行四列布置，第一行索引、第二行红字数据操作。UI 不再创建市场范围控件，所有维护动作固定以范围 2 同时处理两个拍卖库，仍沿用备份及失败回滚。没有底部关闭按钮，标题栏与 Esc 使用同一正常关闭路径。执行结果与详情统一写入主窗口日志，不再单设报告区；摘要与所在横条使用同一底色。所有动作使用同一行高和列边界；主窗口与维护窗口的开关由自绘控件处理悬停、禁用、焦点和开启状态，轨道及圆形滑块先按 4 倍分辨率绘制，再以 HALFTONE 缩小，文字保持原生 DPI；鼠标点击隐藏整圈焦点框，键盘 Tab 导航时按 Win32 UI 状态显示焦点提示。

主窗口标题栏每 500ms 通过 Toolhelp 进程快照，从控制台 PID 递归查找全部子进程，包含控制台、ELF 服务、数据库包装器、mysqld 和临时数据库工具。逐进程只读打开句柄，汇总 `GetProcessTimes` 的内核与用户 CPU 时间差和 `GetProcessMemoryInfo` 的当前工作集。CPU 按 `GetTickCount64` 采样间隔及全部逻辑处理器数量归一化，内存为工作集之和（共享驻留页可能重复计入）。PID 加创建时间识别进程，首次采样建立基线，退出后移除；读取失败显示 `--`，不沿用旧值。独立采样句柄避免读取后台线程关闭或替换的服务句柄；不统计其他控制台实例或无关进程，不作为服务就绪或业务成功信号。

控制台创建服务宿主与数据库包装器、包装器创建 mysqld 时使用 `DETACHED_PROCESS` 并继续显式传递日志与输入句柄，省去各常驻进程的隐藏 conhost。短期 MySQL 命令行客户端保留 `CREATE_NO_WINDOW`：其字符集 `auto` 依赖 `GetConsoleCP()`，直接分离会返回 cp0 并把警告混入 SQL 输出。该保留不会产生随服务器长期运行的控制台辅助进程。

Toolhelp 快照的父 PID 可能属于已经退出的旧进程，因此不能仅按编号递归归属。采样先以 `PROCESS_QUERY_LIMITED_INFORMATION` 查询候选创建时间，仅纳入创建时间不早于本次父进程的候选，再请求工作集读取权限；旧系统进程因此在访问内存前被排除。采样时再次核对创建时间，跳过查询期间已经复用的 PID。真实成员读取失败仍显示 `--`，不把旧系统进程的访问拒绝当成本项目采样失败。

固定 ELF 身份分别为 auction `b31eafd5068b450929133365f071fc2d0dedc7a8a5b80f9c81da44ce4008290c`、point `0339aa6cc51f32c4b50fe9e55f3fd8b00d4b495fcad46871c59a5eda345c9256`。每个站点先核对文件与执行映射的完整旧指令，再写执行内存，并记录旧新字节；不修改磁盘 ELF，退出后恢复。具体站点与预期字节集中在 `auction.c`，拒绝其他 ELF 身份。

搜索原本用动态 map 保存锻造维度，但请求校验只接受 0～7；`0x0808281c` 只调整该上限，等级在 `0x080811d5/0x0808292b/0x08083471/0x0808347c` 调整。范围取当前 PVF；等级受原指令表示限制到 127，锻造受 `UpgradeSeparateInfo::GetUpgradeSeparate` 的五位物品协议限制到 31，超限明确拒绝。不会把 PVF 压回 70／7，也不把请求改成任意 127。

原均价容器只构造每强化 8 档 map，直接放宽校验会使锻造 8 与下一强化档锻造 0 共用地址。现在每 8 个锻造值使用一个完整原版 `AveragePriceDictionary`：额外组独立构造、保留原算法、对象池、ROI 和约束，make／Add／Get 按组转发，Update／析构覆盖各组。外层 Auction 仍序列化真实锻造值。数据库普通均价主键及 ROI 唯一键加入 `seperate_upgrade`；人工均价补该列，原人工更新 SQL 的 join 也匹配锻造值。只有 schema 标志存在时启用 SQL 适配，不猜测迁移未知唯一键。

PVF 解析读取目录、字符串表、equipment／stackable 主列表、实际脚本及 PVF 内 `etc/iteminfo.dat`。支持 GBK／Big5 自动判断、9／10 间接名称引用以及自带 PVF 表尾的 `[explain]` 说明，合并当前真实名称、稀有度、等级和职业。分类依据当前 PVF 自带条目与类型／子类型／目录的无歧义证据；没有依据、无名、原版缓冲无法表示的条目列为待处理，不用编号假名、不凭空填分类。金币条目只有主列表及脚本实际存在才保留。UTF-8 的 17 字段 dat 同步到两市场；原 CodePage 初始化改为 UTF-8，原 `script2Database` 同编码分支不填目标缓冲，因此执行内存回调补上真实复制，防止中文名变空。

两个原市场的 `NextToken`（`0x0807c9a4`）把每个负 char 与后一个字节一起复制，属于双字节编码规则；UTF-8 名称含奇数个高位字节时会吞掉结束引号，把英文名和分类读错。仅在 `DNF_NATIVE_AUCTION_ENCODING=UTF8` 模式下，以独立的逐字节拆分函数替换其入口，保留 ASCII 引号、分隔符、空名称和原静态缓冲调用约定；不修改搜索谓词。入口完整旧指令 `55 89 e5 83 ec 38` 与固定 ELF 哈希同时验证，变更只在执行内存中，重启原服务重新从现有订单建立索引。

`controller/game_compat.h` 在启用 DP2 的固定 game 启动前安装有标记的 Lua 兼容段，首次备份入口，后续只替换自身旧段，保留用户内容。实际 DPX 的 `set_auction_min_level(number)` 在多次完整启动中把随栈位置变化的值写入门槛；调用 `_DP.mem.hotfix` 传入单字节字符串可正确写入。兼容段独立重建同名 Lua 接口，接受整数 0～127，经 `string.char` 写入并回读；没有声称已完整反解 DPX 内部错误。固定 game 身份为 `da417c5e65918f655afcddb6edfdaf2fa1e77b2f208e5aa397f021fa23b79979`，写入前核对 `0x08213f27` 的 `cmp eax,imm8` 和随后 `setle/test` 指令。只更换比较操作数，保留严格大于门槛的规则与原错误路径；每次记录原值、新值和地址，磁盘 ELF 不变，原 ZIP 不变。

检查覆盖两个市场的 `auction_main` 和普通／ROI／人工均价，核对实际 ID、可索引性、强化／锻造／等级、旧名称／分类／稀有度，以及上次同步记录的类型、路径、大小和目录 CRC。订单与均价冲突分别列出；自动同步不删除其中任何数据。`auction_system_main` 在本基底只含排程字段，没有物品 ID，清空在售时显式清除。已有索引及定义记录用于检测 ID 改用途；首次迁移没有旧定义身份时只能比较现有索引字段，不能证明脚本全部语义相同。

索引清单 `.mysql/market-index.ini` 绑定 PVF、两份 dat 哈希和 UTF-8 编码，定义记录另存 `market-definitions.txt`。相关启动前及 15 分钟复查发现哈希变化后检查候选；关闭自动检查时由维护窗口手动应用。候选在内存完整生成后才进入写入。写操作限定当前控制台 SQL UUID／PID／TLS 身份；停止本窗口持有的三个写入服务后再次检查，官方 SQL 备份覆盖选中市场全部表。索引操作额外备份 dat、配置、manifest、定义与 DP2 脚本，以临时文件和原子替换提交。失败导回 SQL 及原文件，保持相关服务停止；成功按原运行集合重新拉起。上一版 dat 绑定其旧 manifest 的 PVF，未知来源记录为空，恢复时拒绝不同或未知 PVF。

DP2 搜索保护仅替换明确识别的搜索 hook 段，保留周围凭据和用户代码；未知自定义 hook 明确拒绝启用，关闭只还原本项目已安装的段。v7 根据搜索包中的物品和实例编号查询已有孔记录，要求 SQL 成功、有且仅一条、HEX 为恰好 30 字节并带非空孔记录才覆盖；取消原有 `CDataManager/CItem` 调用，避免 PVF 缺失物品引发空对象解引用。无记录及失败保持原包字节，加入重入保护与异常日志。Windows 初始化会安装已识别的新版本并保存原段；Linux 保留原有显式安装入口。此实现未改变上架、购买、定价和机器人逻辑。

## 已确认的兼容限制

- 当前采用原服务的前台 `run/nofork/test` 入口；通用 fork/exec/wait、setsid 和后台父子语义未提供完整支持。
- `/dev/shm` 的真实共享文件映射已有路径；System V shm/sem 不等于文件映射，尚无通用实现。
- 网络以现有 TCP/UDP、select/poll、LT epoll 路径为主；AF_UNIX、完整 sendmsg/recvmsg 辅助数据、ET/ONESHOT 等未完成。
- 非 PI robust list 注册及正常线程退出时的 owner death、pending 节点与进程内唤醒已有实现；PI 锁、跨进程 futex、强制终止后的恢复、线程退出槽复用、pthread cancel 和通用线程定向信号未完整覆盖。
- SA_SIGINFO/SA_NODEFER 已支持固定 i386/x86_64 `siginfo_t`/`ucontext` 帧、嵌套投递、GPR/PC/SP/flags/掩码修改及 `rt_sigreturn`；同步访问异常支持范围见上文。浮点扩展状态、备用信号栈、其他同步异常、纯计算中的异步抢占和完整 syscall 中断/重启语义仍有限。
- 匿名私有映射的 `madvise(MADV_DONTNEED)` 对已提交页 decommit/recommit 为原权限的零页，保留地址及 guest 映射；从未提交的 PROT_NONE 页继续保留。长度按页向上取整；范围内有解除映射的洞时返回 ENOMEM，并丢弃仍映射的部分，与 Linux 对照一致。当前范围须处于同一匿名记录，文件、共享映射及其他 advice 仍明确拒绝，不扩展为通用页面回收。
- 打开目标上的 POSIX rename、符号链接、普通文件执行映射、EOF 外整页的 SIGBUS、跨多个预留区固定映射及部分 msync 行为存在边界。
- inotify_init/init1 仍返回 ENOSYS；固定原版 Frida Gadget 的 `GPollFileMonitor` 轮询回退已通过覆盖写入、原子替换和新版 RPC 结果验证，延迟约 5 秒。无需为此路径增加文件监视兼容层；通用 inotify、全部编辑器行为和业务运行中的任意脚本热加载不在已验证范围。
- 保留原地址不等于任意等级 ELF/PVF 都兼容；每个新组合都需要匹配资源和独立业务验证。

## 基底与源码的关系

`native.zip` 继承已验证阶段的局部 monitor 适配、插件脚本和原包 PVF；这些不是本次整理新加入的补丁。来源、旧字节与恢复边界保留在 `docs/evidence/history.json` 的历史记录及清理前提交 `fbb11f3` 的重建工具中。新修改原版指令仍须遵守 `AGENTS.md` 的身份、地址、旧字节和恢复记录要求。

普通打包不重放旧 CAB 转换或修改基底。需要更新资源时，应先生成独立候选、核对来源差异、验证实际路径，再更新 ZIP、逐文件清单与基底哈希。历史实验不因目录迁移成为当前支持承诺。

## 独立登录器

`src/login/` 由用户明确授权提供的 C 工程导入，来源哈希见 `evidence/login-source.json`。它是独立客户端程序，手动连接 MySQL、注册四表并签发原令牌，不读取或依赖服务实例。改动包括当前密钥对导出、无效外部密钥拒绝、去除硬编码私有默认连接、Unicode 客户端路径和子进程游戏地址传递、注册附表失败报错。公钥通过 Windows CryptoAPI 从当前私钥实际 n/e 导出；默认私钥仅以 `resources/login/private_key.pem` 维护，构建在外部生成内嵌头。固定服务资源仅更新默认公钥。正式运行目录仅附登录器 EXE，源码保留在仓库，不包含用户客户端或数据库。使用与限制见 `login.md`。

TCP 的 FIN／RST 在 Windows WSAPoll 中可能仅返回 HUP／ERR。Linux 的读就绪必须让调用方读出 EOF 或待决错误，因此兼容层在 socket HUP 时保留 HUP，并补上调用方请求的 POLLIN／POLLRDNORM；epoll 同步得到 EPOLLIN。原 monitor 通过可读事件读取断开并清除频道，同一 game 正常停止后可重新注册，不能通过跳过重复频道检查掩盖。

TCP 终止错误按描述符消费一次；之后读出 EOF、写入返回 EPIPE，并向当前写入线程投递 SIGPIPE。`SO_ERROR` 同样消费终止错误；本地关闭写端不影响读端。发送支持 `MSG_NOSIGNAL`，阻塞的普通信号合并，不能被其他线程接收；自发 `tgkill` 也使用线程待决队列。UDP、异步连接错误与 TCP 终止状态分开处理。仍存在 Winsock 底层边界：对端发送数据后立即 RST，Windows 可能丢弃未读取数据，而 Linux 仍能读出；当前同步收包实现未解决该差异，专项夹具会明确失败，不据常见断线检查通过宣称完整 TCP 等价。

预挂接收与 64 KiB socket 自有缓冲的候选没有纳入当前实现：它改善小包复位读取，
但缓冲满后仍丢数据，600 连接的独立夹具还显示 CPU、内存和句柄开销增加。
取舍与逐项证据见 `validation.md` 的“TCP 预挂接收候选的取舍”。

收发支持单次调用的 `MSG_DONTWAIT`。Winsock socket 始终保持非阻塞，guest 的
`O_NONBLOCK`、FIONBIO 和单次 flags 决定当前调用是否等待；阻塞 read/write、
send/recv、connect 和 accept 通过 select 等待真实就绪，重试保留同一超时截止点。
不会为了一个调用临时切换共享 socket 模式，也不通过取消异步发送来模拟 EAGAIN：
后者可能已经发送数据却返回取消，造成应用重发和协议字节错位。发送返回真实接受的
字节数，等待就绪后仍重新尝试 I/O，以处理其他线程先读走数据的情况。

新实例的公开演示登录使用用户指定的 game 数据库凭据和 test 游戏账号，四张登录表在初始化时建立；内部 dnf_game、维护 root、bridge 和 frida 仍按原职责使用实例凭据。登录器不读取服务端配置，默认值明文显示；控制台数据库详情显示当前实例实际连接参数，不记录到控制台日志。

## Robot 与性能阶段实现

`src/runtime/elf.c` 的全文件哈希仍保持 SHA-256 身份校验，但读取改为 64 KiB `ReadFile` 分块，减少大 ELF 启动时对只读映射工作集的影响。`NrRun` 在启动时缓存 `DNF_NATIVE_TRACE`、`DNF_NATIVE_EPOLL_TRACE` 和 QPC 频率，syscall、socket、epoll 与文件热路径不再重复查询环境变量或频率。

已提交私有内存的 `VirtualQuery` 结果使用 32 项缓存，代码读取和数据访问均保留真实页权限检查；guest 所有权仍逐次核对。Windows 查询从当前页开始返回区间，因此缓存把同一分配、相同权限且终点相同的重复尾段合并，避免角色对象交错访问时重复淘汰和扫描大区页表。VM 和 brk 在同一映射锁内只清除发生变化的重叠范围；成功恢复原权限的零页重置保留缓存。文件视图、未提交页和 guard 保留直接查询。该规则依据 [VirtualQuery](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualquery) 返回区间语义独立实现。运行 ELF 时申请 1ms Windows 定时器分辨率，正常返回时配对释放。epoll 从上次返回位置继续扫描就绪项，避免小 `maxevents` 长期饿死后续描述符。`pread64` 用独立重开句柄和 OVERLAPPED 偏移读取，保留原文件位置。

Robot 通过独立 `robot.exe` 进程接入服务列表末尾。Native 只创建实例目录配置、命名停止事件和 Job Object 归属；Robot 通过 `--config-dir`、`--stop-event` 及 `host-profile.json` 选择宿主。Windows 进程、内存补丁和市场维护由 Robot 自己的平台层处理，Linux 原有路径保留；依赖 `/root/run`、`/root/stop` 的入口在 Windows 明确不支持并置灰。详见 [Robot 接入](robot.md)。
