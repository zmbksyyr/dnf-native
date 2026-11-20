# Native 实验知识与排查入口

整理日期：2026-11-20。本文将仓库外 `x` 中的实验转为可复用结论；按问题查阅，不按调试轮次阅读。
当前实现以源码为准，详细原始过程见 [验证记录](validation.md)，接口说明见 [架构](architecture.md)。
下文证据路径相对于工作区 `x/`；原始日志、数据库、抓包、探针和工具链不纳入仓库。

## 基线与结论的有效范围

| 项目 | 已核对的基线 |
| --- | --- |
| Native 整理前提交 | `7290a72f7efc24b98bfcb76d52659b703ce50a2e` |
| 原 i386 game SHA-256 | `da417c5e65918f655afcddb6edfdaf2fa1e77b2f208e5aa397f021fa23b79979` |
| 固定 ZIP 的 PVF SHA-256 | `c3dde22a8b19e5deff5c498533e0830d13fec5605e81adeda9373841e6e22dec` |
| 历史 jbz69 输入 PVF SHA-256 | `b3272eb24a775fea590eb0bf9d22c4876e2cc44720d505b83227fb01a56156fe` |
| Windows | Windows 10 IoT Enterprise LTSC 2021 x64，build 19044；原 game 使用 x86 宿主 |
| 工具及依赖 | LLVM-MinGW 20261104 MSVCRT、原 glibc 2.17、MySQL 5.7.44 |
| 历史 Linux 对照 | 同机 Ubuntu 22.04.5／WSL2 Linux 5.10.16.3；仅为当时独立比较环境 |

Robot 后续战斗研究使用另一份 PVF 和指定客户端，身份记录在 Robot 的 `docs/combat-research.md`。
文件名相同不代表资源相同。上述环境也不代表其他 Windows、ELF、PVF 或裸机 Linux 的结果。
整理文档没有重新运行这些实验，也不引入 Linux 环境为产品依赖。

## 执行模型与性能判断

同架构普通指令由 CPU 直接执行。主要适配成本集中于 Linux syscall、TLS 指令、信号、
内存权限和 Windows 网络语义，不能将整个 game 工作集或全部运行时间归为兼容层开销。

P52 的同机 PE／ELF 算术循环差异约 0.4%，属于该微基准波动范围；当时 i386 TLS 直达约
6.76 ns，VEH TLS／syscall 边界约 5.3 μs。这定位了优化方向，不能换算成“整服损耗百分比”。
真实负载的地图、摆摊、动作次数不同，也不能把多轮局部收益相加。

定位性能时应在操作发起前采样，尤其是批量退出、重登和收摊。只在 `autoStop` 返回后采样，
曾错过最忙阶段。暂停线程的栈采样用于找热点，性能比较应另用未插桩程序。

证据：`dnf-native-output/P52-native-overhead/`、`P53-performance-robot-research/`；
后续 TLS、信号和文件网关结果位于 `dnf-gm-output/evidence/`。

## 内存：已确认的根因与处理

### 大工作集不等于泄漏或玩家容量过大

bridge 和 channel 原构造函数各建立 1000 个对象，每个对象包含两块 655360 字节队列，
原 `memset` 将大池全部触碰为驻留页。按清零语义处理合格的私有匿名页后，独立协议对照中：

| 服务 | 原工作集 | 优化后工作集 |
| --- | ---: | ---: |
| bridge | 1335.4 MiB | 92.5 MiB |
| channel | 1303.0 MiB | 59.8 MiB |

私有提交量基本不变，协议及池容量保留。不得把工作集下降说成等量虚拟地址或提交量释放。
清零必须保留未对齐边界、共享别名、文件 COW、可执行页和预加载拦截语义。

game 自身有固定玩家池、PVF 数据和原库分配；不能照搬 bridge/channel 的驻留降幅。
P51 的分块哈希实验只证明 game 与一个库的源文件映射合计约 42.82 MiB 的局部驻留收益。
不能据此统一释放 PVF 借出内容、缩小 600 玩家池或更换分配器。

证据：`dnf-native-output/P47-memory-profile/`、`P48-memory-optimize/`、
`P51-game-memory-research/`；回归入口 `tests/test_zero_pages.py`。

### 权限缓存曾是批量退出卡顿的实际热点

两个已修复的问题需分别记住：

1. 大块清零恢复相同私有可写页后，原实现仍清空全部权限缓存。
2. 无关的临时 4 KiB／132 KiB 映射、解除映射，也清空大池的全部权限缓存。

它们都会使后续访问反复进入 `VirtualQuery`。正确做法是保留未变化的缓存，映射、保护、
丢弃和 brk 只使实际重叠范围失效；页权限、所有权和非法访问检查继续有效。
增加缓存容量、跳过权限检查不能代替这项修复。

同一自有 ELF 的 320 MiB 池、3000 次临时分配，三次交错旧宿主为 4.396–4.719 秒，
新宿主为 0.048–0.050 秒。最终另一次真实 600 目标混合负载稳定 120 秒，保持 596–600 在线，
game 约 1399 MiB、Robot 全树约 270 MiB，最大游戏时钟滞后 0.574 秒并正常排空。
此前同类 Windows 退出曾滞后 30.656 秒。此结果不是所有退出场景或长期负载的保证。

证据：`dnf-gm-output/evidence/zero-cache-profile-comparison.json`、
`store-trade/memory-cache-diagnosis.json`、`store-trade/cache-range-final-load-summary.json`；
`dnf-native-output/memory-cache-range-benchmark-20261119/receipt.json`。

### MADV_DONTNEED 必须真的恢复零页语义

旧 `MEM_RESET` 路径可能读回旧值；允许“旧值或零”的测试不足以验证 Linux 语义。
实际修复还暴露 glibc 会丢弃自己的旧栈：若宿主 syscall 处理帧也在该范围，会一同被清掉。

当前主线程 guest 栈独立分配，丢弃和退出清理切回 Windows 栈；按原权限处理支持的匿名页。
PROT_NONE、非法写入仍须产生真实故障，不能用通用重试吞掉错误。文件／共享映射和其他
advice 按实现明确支持范围判断，不能从匿名页测试外推。

证据：`dnf-gm-output/evidence/madvise-regression-summary.json`、`madvise-thread-exit-trace.log`；
回归入口 `tests/test_madvise.py`、`test_vm_concurrent.py`、`test_zero_pages.py`。

## TLS、线程与快速调用的约束

| 现象 | 已证实原因／实现约束 | 复核入口 |
| --- | --- | --- |
| 并发创建线程出现相同 TLS | 空槽检查与写入间未预留；必须原子预留并在失败时释放 | `tests/test_clone_parallel.py`；`dnf-native-output/clone-parallel-final.json` |
| x64 偶发报不支持 TLS | `VirtualQuery` 后地址竞争，失败后按过时范围跨过仍可用地址 | `docs/validation.md` 的 x64 TLS 跳板并发分配；`tls-allocation-before-after.json` |
| TLS 失败诊断反而访问异常 | CRT 日志在切换后的 guest 栈上触及 Windows TEB 栈界限 | 同上；失败分支使用既有直接内核日志 |
| pthread 取消与 errno 高频走 VEH | i386 GS 访问的少量已定位热点；直达仍须取得当前 TLS、保留 LOCK 和原子语义 | `cancel-atomics-tls-before-after.json`、`tests/test_tls.py` |
| 查询和日志写入频繁异常往返 | 固定 loader 的合格入口可直达共用实现；detour、trace、非合格参数须回退 | `sysinfo-final-summary.json`、`file-gateway-final-summary.json` |
| 新快路径看似能跑但寄存器有差异 | 还需核对 GPR、ESP、flags/DF、x87、XMM、MXCSR、信号与 errno | `tests/test_sysinfo.py`、`test_files.py`、`test_process_signal.py` |

上表除注明外的 JSON 位于 `dnf-gm-output/evidence/`。直接路径不是任意 syscall、AVX 状态
或任意 loader 的通用承诺。不能用普通 GS 读通过，替代 LOCK CMPXCHG、动态 TLS 或线程退出验证。

## 网络：已解决的语义和已排除的方案

LuaSQL 的真实驱动使用单次 `MSG_DONTWAIT`。宿主保持 Winsock 非阻塞，再按 guest 调用语义
等待就绪；不能为了普通阻塞读取而破坏共享 socket 的非阻塞行为。
断管的 `ERROR_NO_DATA` 和 `ERROR_BROKEN_PIPE` 均应映射 EPIPE，并向实际写线程投递 SIGPIPE。
错误码正确不等于信号屏蔽、合并、忽略和默认终止语义都正确。

**仍保留的缺口**：对端发送后立即 RST，Windows 可能丢弃尚未交付给本项目的未读数据，
包括 accept 前及完成调度延迟场景。Linux 对照中的数据保留不能由简单 Winsock 映射完全复现。

| 已试方案 | 结果与决定 |
| --- | --- |
| 每 socket 固定 64 KiB 预挂接收／线程池通知 | 小包通过，大包复位只保留容量内数据；CPU、内存、句柄成本增加，未采用 |
| 按 SO_RCVBUF 扩容、共享 IOCP | 部分测试通过；accept 前复位仍失败，延迟处理完成时分段数据仍可能只留首段，未采用 |
| 下一次接收先排队，再 CancelIoEx 当前 WAITALL | 本机后续请求也以 995 取消；等取消完成再提交能读，但仍有所有权空隙 |
| AcceptEx 预先给足缓冲 | 单次 4096 字节可成功；48／128 KiB 分段复位即使容量足够仍只留首段 |
| i386 非阻塞接收快速网关 | 已复用原实现降低局部开销；没有解决 RST 数据所有权问题 |

仅接收网关的 600 连接回显夹具 CPU 中位数约 0.734→0.570 秒；真实 game 八阶段交错
测试的动作强度不同，没有确认整体提速。不得把约 22% 的夹具收益写成整服收益。
重新研究异步接收须先提出能覆盖 accept 队列、连续分段、缓冲满、关闭取消的数据所有权方案，
再测成本；重复调缓冲大小或仅验证单包没有新增价值。

证据：`dnf-gm-output/evidence/receive-integration/decision.json`、
`receive-shared-integration/decision.json`、`tcp-ownership-decision.json`、
`fast-socket-gateway/final-summary.json`；回归 `tests/test_net_buffered.py` 的
`--buffered-reset`、`--probe-limits` 会如实报告当前缺口。

relay 的历史并发故障另涉及描述符分配／关闭／复用，不应把它与 RST 问题混为一因。
先读 [relay 故障记录](validation.md#relay-并发描述符故障定位2026-11-16)，再用
`tests/test_descriptors.py`、`test_select_close.py`、`test_relay.py` 验证对应生命周期。

## PVF、双拍卖与登录问题

| 症状 | 应复用的结论 |
| --- | --- |
| 某 PVF 缺少 `atswordman.lay` 等资源时启动失败 | 先区分客户端视觉模板与服务端必需层号。缺失可选视觉内容按原调用语义清理／返回，必需层号仍拒绝；不按某个文件名放行 |
| PVF 能离线解包，仍在 game 中失败 | 读取器通过不代表装备导入、模板解释、全局构造和完整启动通过；逐级定位首次实际失败 |
| 拍卖已上架但分类搜索为零 | 曾因 `NextToken` 的双字节规则破坏 UTF-8 引号，导致内存分类为 0；不是把客户端分类过滤放宽即可 |
| 高锻造物品不能搜索 | 搜索、普通／ROI 均价容量及数据库键须一致；不能只改一个比较值 |
| 换 PVF 后重建 iteminfo 导致市场起不来 | 订单会在启动回灌；先检查存量物品引用及均价，再备份、同步。无法解释的旧 ID 不自动继承或删除 |
| 金币寄售报等级不足 | 历史 DPX setter 实际写成 90／93／94，和配置不同；核验原指令、参数及写后回读，不以角色页面等级推断门槛 |
| 领取成长武器断线 | 原包读取分支的独立错误，见等级／成长奖励记录；不能统一归为背包损坏 |
| 登录报断开码 36 | 后续 Robot 实验确认可能是封装标记、当前 PVF 不存在的物品或宠物栏；保留原拒绝，修复目标数据前先备份 |

固定 PVF 为 Big5，历史 jbz69 输入为 GBK；编码、等级曲线和物品定义均有差异。
“可索引物品数／全部定义数”不是 PVF 兼容率，未分类或无名称也不等于坏物品。
参考工程仅用于只读理解，再以目标 ELF／实际行为独立复核；不搬运其补丁表或替换运行链。

机制及回归：[PVF 与模板记录](validation.md#pvf-缺失模板按语义兼容2026-11-16)、
[双拍卖维护](architecture.md#双拍卖维护)、`tests/test_pvf_templates.py`、
`test_auction_runtime.py`、`test_auction_guard.py`、`test_game_rules.py`。
上述登录拒绝的后续资料见 Robot `docs/knowledge.md`，其重试修复目前只在实验分支。

## GUI、时间与模块边界

- 标题栏统计包含当前 GUI、服务、MySQL 及其子进程；CPU 按整机逻辑处理器折算，内存为工作集之和。
  共享页可重复计入，与 Robot 单核为 100% 的 CPU 口径不同。两者不能直接比较百分比。
- 进程身份必须使用 PID 加创建时间，并核对父子时间关系。旧 PID 被系统进程复用曾使整组采样失败，
  标题停在 `--`；忽略访问拒绝或只统计 GUI 都不能修复成员识别。
- Windows 后台进程的处理有区别：常驻 mysqld／Robot 子进程可分离；短期 MySQL 客户端全改分离曾
  出现 cp0 警告污染 SQL 输出，应保留隐藏控制台及正确标准句柄。
- 原时间戳仍表示真实时刻；本地显示要统一 guest TZ、MySQL `log_timestamps=SYSTEM` 和 GUI。
  显式 TZ 优先。Windows 当前周期规则不等于跨年份历史时区数据库。
- 进程运行、端口监听、应用初始化、停止中和异常退出分别显示；正常停服期间不能误报红色异常。
  日志无滚动条仍需处理滚轮，更新时保留用户阅读位置。
- 实例目录与服务 IP 分组；运行设置归“功能”；Robot 开关即时保存并禁用重复的拍卖维护入口。
  DPI、最小尺寸、开关文字宽度与同行间距使用实际 Win32 控件检查。
- Native 为 Robot 提供宿主 profile、game 目录配对密钥和生命周期；Robot 管理自身业务文件。
  GM 为独立程序，桥接由 GM 导出和部署，Native 不代装、不为其创建另一套接口。

证据：`dnf-native-output/P45-system-time/`、`P46-total-metrics/`、`P49-metrics-pid-reuse/`、
`probes/`；回归 `test_gui_metrics.py`、`test_gui_metrics_sample.py`、`test_timezone.py`、
`test_gui_layout.py`、`test_gui_logs.py`、`test_gui_robot.py`、`test_gui_client.py`。

## 不应再次从零尝试的路线

| 路线 | 已知边界／重新打开条件 |
| --- | --- |
| 用 Windows CRT 全面替换原 glibc | 少量导入调用实验不覆盖 C++ 异常、对象布局、pthread、TLS 和回调；只有明确局部接口需求才评估 |
| Robot 编成 Linux ELF 后直接托管 | 历史 Go 386 首先卡在 sigaltstack，amd64 涉及内嵌数据与线性反汇编；已选择独立 Windows 平台适配，不为接入 Robot 扩建 Linux 运维环境 |
| 用进程正常退出证明安全链正常 | 原 zergsvr／gunnersvr／secagent 仍涉及缺失配置中心／监控依赖；退出码 0 曾对应初始化失败 |
| 释放所有 PVF 缓存或缩减池容量 | 原对象可能持有内容指针；须有实际生命周期证据，不能为数字好看破坏语义 |
| 用 API 成功或网络存活替代游戏验收 | 原内存、实际通知、存档、重登和图形客户端各有不同证据层级 |

来源：[库替代实验](validation.md#windows-库函数替代实验2026-11-16)、
[安全组件诊断](validation.md#原版安全组件诊断2026-11-16)、
`dnf-native-output/P53-performance-robot-research/robot-linux.md`。

## 构建、历史与发布的复用方法

固定 ZIP 与清单随仓库管理，工具链与所有构建输出在 `../x/`。GitHub 使用 LFS 后，
源码 ZIP 下载中的指针不能当作资源；应取回 LFS 实体再校验。
本机原历史 `7290a72` 与 GitHub LFS 历史 `b917eaa` 不是同一提交 ID，但当时核对的
206 个普通 Git blob／模式一致，两份 ZIP 实体对应 LFS 的哈希与长度；不能仅凭提交 ID 不同重做迁移。

发布从 `python -B tools/build_all.py --output <x 下的新目录>` 开始，Robot 与 GM 各从独立源码构建，
再核对源码收据、资源身份及最终文件。正式目录根层只放 EXE 与 resources。
GM 自检／首次打开会生成 `resources/config/`，它属于验证现场，不能误打入干净发布包。

2026-11-20 的 3.0 重建使用 Robot `2b29662`、GM `4f0a90f`；七项发布检查覆盖 GM 自检、
资源编码、新实例／默认账号／原 game 启停、布局、Robot 登录移动与启停、登录器、客户端同步和打开 GM。
这次没有新增 Linux 或 600 人负载测试。证据：
`dnf-native-output/release-3.0-20261120-restore/{validation-summary,release-receipt}.json`。

## 以后如何使用本文

先匹配症状、源版本和资源哈希，复核对应已保存结果，再运行最小相关回归。
只有目标 ELF／PVF／客户端／系统改变、出现反例，或测试确实未覆盖当前问题时，才重新实验。
新增记录应包含触发条件、根因、最终决定、排除方案、验证层级和证据位置；旧计划被后续实测推翻时明确更正。
GM 内存态操作的业务语义归 GM `docs/knowledge.md`，Robot 调度与战斗归其对应文档，避免三仓重复维护同一结论。
