# dnf-native

DNF 台服原版 Linux ELF 在 Windows 7/10/11 x64 上原生运行的独立项目。

**当前状态：P1 实施中。** 自研双架构宿主已完成 19 个原版 ELF 的原址映射及四项输入拒绝检查；尚未执行原版服务。实际范围与证据见[实施记录](docs/06-progress.md)。

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
python tools/test.py
```
