# 运行时并发与首次创建回归

本阶段修复了一个已复现的地址空间记录竞态。验证环境为 Windows 10 x64 10.0.19044，输入 ELF、PVF、数据库和客户端保持只读。

`NrRun.vm` 是所有 guest 线程共享的映射表。原实现在线程并发执行 `mmap`、`mprotect`、部分 `munmap` 时无锁地查找和占用空槽；同一时刻两个线程可能观察到同一个空槽，或一个线程在另一个线程读取记录时清空它。这个问题会让地址空间元数据与实际 Windows 映射不同，属于真实的运行时竞态。

现为共享映射表增加一个进程内 `CRITICAL_SECTION`。`mmap`、`mremap`、`mprotect`、`munmap`、`madvise`、`msync` 的记录操作在同一锁内完成；内存有效性检查和异常处理读取映射记录时使用同一锁。`nr_vm` 内部调用的内存检查允许同一线程递归进入 Windows 临界区。变更只保护共享状态，没有改变 ELF 地址、系统调用号或原版程序。

## 证据

新增的双架构并发映射夹具让 8 个 guest 线程同时执行 200 轮匿名映射、读写、保护变更和释放。当前宿主两架构均输出 `native-concurrent-mappings-ok` 并以 42 退出。使用相同夹具和无锁诊断宿主时，i686 返回 50，x86_64 返回 `0x80000001`；现场保留在启动诊断目录中。该对照确认锁改变了可复现的并发失败行为，而非只增加静态代码覆盖。

可复现命令为 `python tools/test_vm_concurrent.py`，夹具源码在 `tests/concurrent_mappings.c`，使用项目已有的双架构 guest thread 启动代码。

当前双架构原语回归通过：VM、TLS、futex/requeue/wake-op、跨进程信号；Frida 物品清零、relay TCP/UDP、STUN UDP 和 channel TCP 也通过。relay 旧的 `getManager()` 访问异常在新宿主 30 次独立启动中未复现；用新宿主从最终 CAB 重跑首次创建，两份实例的种子往返、18 个应用库、独立账号、15 个 peer 协议、Frida 数据库接口和正常关闭全部通过。

随后把当前双架构宿主写回私有 core 实例，包含 game 的完整 GUI core 自然启动为 34.94 秒；就绪后通过 GUI 正常停止，返回 0 且无强制回收。该轮只验证服务栈启动/停服，没有把客户端登录状态写成业务通过。

首次创建新资源的专项 receipt 为 `build/probes/20261115T001102730133Z/gui-setup/receipt.json`；并发夹具和 30 次 relay/STUN 诊断的脱敏摘要见 [阶段证据](evidence/p13-runtime-concurrency.json)。

审阅包已重新生成，GUI 字节保持不变。最新 ZIP 为 190382482 字节，SHA-256 `4decacc69356383bfb7778ee0d22d567a37ff84cbdd58e43fe87eec3d0aab118`，CAB 为 175510859 字节；CRC、成员哈希和 ≤210 MB 检查通过。资源内包含当前双架构宿主及上一阶段的原生物品 reset。

验证结束后归档并移除了三组可重建的完整 core/首次创建副本，归档 CRC 和每个成员 SHA-256 均通过；保留首次创建 receipt、截图和阶段摘要。释放约 2.035 GiB，build 现约 17.365 GiB；归档和收据为 `build/cleanup-concurrency-stage-diagnostics.zip` 与 `build/cleanup-concurrency-stage-receipt.json`。

客户端登录前的图表系统初始化错误、定制 jbz69 PVF 缺失模板、一次历史 STUN guest TLS 现场仍保留，尚未声称修复。安全三件套、负载测试、Win7/Win11 和客户端业务的完整矩阵仍未完成。本阶段提交后等待审阅。
