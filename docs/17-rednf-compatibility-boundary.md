# 参考源码与当前兼容边界

本记录只把 `dnf-winport/rednf` 作为只读行为线索，并与本项目 runtime 的实际代码、日志和原版 ELF 观察对照。参考源码的调用次数不能证明原版 ELF 会走到该路径，也不能作为交付通过证据。

## 资源结论

工作区没有与用户提供的 jbz69 客户端 PVF 匹配的原版 ELF 服务端：

| 资源 | 版本/哈希 |
|---|---|
| 原版服务 `Script.pvf` | PVF 48984，SHA-256 `c3dde22a8b19e5deff5c498533e0830d13fec5605e81adeda9373841e6e22dec` |
| jbz69 客户端 `Script.pvf` | PVF 66282，SHA-256 `dc444c3429a44c19419dba83c8b1c30e4263371afa528ba90430fda2f598e1e2` |
| jbz69 目录 `game/df_game_r.exe` | x64 PE/Rust，SHA-256 `d38cbdf67939041ac0e1d43d15dbe8006384adb27ec42c5c99ae990c59d91ea6` |

jbz69 目录中的 PE 服务不能替代需求要求的原版 Linux ELF。其余 PVF 备份也均为版本 66282，未发现 48984 配套版本。当前定制 PVF 的阻塞边界见[模板别名验证](16-jbz69-template-alias-probe.md)。

## 已覆盖的实际路径

- 原版 i386/x86_64 ELF 原址加载、原版 `ld.so`、初始栈、已触发的 VM、文件、TCP/UDP、poll/LT epoll、线程/TLS、futex 子集和 runtime 进程间停服路径。
- 原版 game 的 DP2/Lua/Frida 初始化、真实 hook/双向调用、数据库连接、原服务 PVF 下的客户端认证和一轮建角、进图、结算、存档冷读。
- 原版资源中的 `/dev/shm` 文件映射与 TSS 共享文件路径；这不等于完整 System V IPC 支持。

这些结果限 Windows 10 x64 开发机，并且不覆盖所有服务、所有系统版本或任意未知等级文件。

## 尚未提供或尚未证明的兼容支持

| 范围 | 参考源码线索 | 当前状态 |
|---|---|---|
| 进程 | `fork` 24 处、`setsid` 24 处 | GUI 使用已验证的前台/`nofork` 配置；通用 `fork/exec/wait`、daemon 父子状态和任意多线程 fork 未通过 |
| 文件监听 | i386 日志出现 `syscall gap nr=291` (`inotify_init`) 与 `nr=332` (`inotify_init1`) | 未实现 inotify fd/watch/read 语义；当前原版路径退回 fallback，服务仍可启动，但不能宣称文件热加载兼容 |
| System V IPC | `shmget/shmat/shmctl/shm_unlink`、`semget/semop/semctl` | runtime 没有对应通用实现；已验证的是 `/dev/shm` 文件 section 的实际 TSS 路径 |
| POSIX 消息 | `sendmsg/recvmsg` | 未形成原版 ELF 的业务通过证据，未实现为“成功桩” |
| 内存同步 | `msync` | 普通刷新子集存在；`MS_INVALIDATE` 明确返回未支持，完整文件映射/COW/共享别名仍有限定 |
| 网络 | 参考代码大量 `select`/`epoll`，并使用 socket 选项 | TCP/UDP、select/poll 和 LT socket epoll 已覆盖；ET/ONESHOT、AF_UNIX、部分 ioctl/option 仍未完整支持 |
| 线程取消 | `pthread_cancel` | 未验证异步取消、取消点、析构和 TLS 交互 |
| 限制/信号 | `getrlimit/setrlimit`、信号处理 | 实际 game 所需的 core/stack 查询和受限设置已覆盖；完整 `SA_SIGINFO`、异步嵌套、robust owner death 未通过 |
| x64 STUN | 参考服务的线程和网络路径 | 原版 STUN 已完成原协议 UDP 往返；历史未实现 TLS 站点仍需作为窄范围失败记录，不能扩展到任意 x64 ELF |

## 决策

下一步只沿原版 ELF 的新失败证据补齐具体语义。不会为参考源码中尚未证明可达的 API 预建抽象，也不会用 jbz69 的 PE 服务、PVF 别名、编码强制映射或空实现来制造兼容通过。获得匹配版本的 ELF/PVF/数据库后，再单独建立该等级组合的最小验证记录。
