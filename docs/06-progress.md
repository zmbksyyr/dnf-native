# 实施记录

## 2026-11-13 · P1 原址加载基础

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
