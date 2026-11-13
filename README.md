# dnf-native

DNF 台服原版 Linux ELF 在 Windows 7/10/11 x64 上原生运行的独立项目。

**当前状态：P1 实施中。** 原版 x64 STUN 已在本机完成 50 次 UDP 协议往返；原版 i386 manager 已通过原有 `nofork` 路径完成配置初始化。19 个 ELF 原址映射和双架构执行原语检查通过，完整服务业务及 Win7/Win11 尚未验证。实际范围与缺口见[实施记录](docs/06-progress.md)。

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
```

协议检查使用 STUN 原有 `test` 前台模式，绑定原 UDP 端口 2311–2313，结束后强制回收本次启动的测试进程；manager 使用原有 `nofork` 模式。测试实例写在 `build/probes/` 的独立目录。它们证明的范围不包含后台派生、正常停服或最终验收。[第三方来源](THIRD_PARTY.md)记录实际引入的指令库和用户库。
