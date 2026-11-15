# dnf-native

使用本项目的 Windows 原生宿主运行原版 DNF 台服 Linux ELF，提供独立实现的 Win32 控制台。运行包不依赖 Python、VM、WSL、Docker 或系统安装的 MySQL。

当前交付采用 **根目录 GUI + resources 资源目录 + 用户自行选择的实例目录**。MySQL 程序和原生服务端分别为普通 ZIP；创建实例时释放到实例目录，数据库程序、配置和数据均由该实例独立持有。GUI 提供资源检查、新建/选择/删除实例、单项/整体启停、服务 IP 修改、DP2/Frida 开关、原生日志，以及停服备份/还原。

```text
DNF-Native.exe
resources/
  mysql.zip
  native.zip
  dnf-host-x86.exe
  dnf-host-x64.exe
  mysql-control.exe
  resources.ini
  README.txt
  licenses/
```

打开 GUI 后点击“创建实例”，选择新建或空文件夹；完成后自动切换到新实例。已有实例通过“选择…”切换，保留其配置与存档。同一时间启动一个实例。实例停止后，“删除实例”经确认会永久移除**整个实例文件夹及全部内容**。

MySQL 为 Oracle 官方 5.7.44 的四个原样工具及必要依赖，释放后位于 `INSTANCE/mysql/bin/`，可以直接调用。默认数据库 root 密码遵循用户设置；控制台维护连接保存在各实例的 `client.private.cnf`。手动改密后同步更新该文件即可。

原生资源已移除无用编码模块、调试辅助、系统部署文件、插件示例和旧 Linux 启动脚本；保留实际运行的 ELF、依赖库、PVF、配置及 DP2/Lua/Frida 资源。ZIP 使用真实文件名，可以直接浏览。**没有包体积上限**，不以减少文件数为由删掉必需依赖。

本轮验证环境为 Windows 10 x64 10.0.19044。两个实例的创建、自动切换、数据保留、资源缺失/损坏拒绝、完整删除，以及 15 个辅助服务协议、game/DP2/Frida 自然启动和正常停止已通过实测。具体收据、最终文件身份及局限见[实例式交付](docs/20-instance-delivery.md)。历史 relay 启动访问异常曾再次出现，复测通过不能视为该问题已经修复；安全三件套完整业务、客户端完整验收及干净 Win7/Win11 矩阵不在本轮完成范围内。

当前构建与回归：

```powershell
python -B tools/build_mysql.py --build-dir build/mysql-build
python -B tools/build_gui.py --mysql-dir build/mysql-build --build-dir build/gui-build
python -B tools/build_instance_package.py --source VERIFIED_V2_NATIVE_DIRECTORY --gui-dir build/gui-build --hosts VERIFIED_COMPATIBILITY_DIRECTORY --output build/package-final
python -B tools/test_resource_encodings.py --package build/package-final
python -B tools/test_instance_workflow.py --gui build/package-final/DNF-Native.exe
```

包构建器只读校验此前已验证的 CAB 快照，转换为新的可浏览 ZIP；产品运行链不再使用 CAB。输入导入和历史实验命令保留在各阶段文档，不能与当前交付结构混用。开发工具链来源见[第三方来源](THIRD_PARTY.md)，首次配置从 `tools/bootstrap.py` 开始。原版输入、数据库、用户库、工具链和构建产物均不提交 Git；本机生成数据通过忽略目录存放在工作区外的 `x` 中。

- [工作纪律](AGENTS.md)：目标优先、复杂度相称，原版输入及参考工程使用边界。
- [初始需求](docs/requirements.md)：原文归档；本轮用户确认的实例式交付与取消体积限制优先于旧形态。
- [当前实例式交付](docs/20-instance-delivery.md)：八项需求、目录、验证、发布记录。
- [实施记录](docs/06-progress.md)：历史阶段及证据入口。
- [原生运行方案](docs/02-technical-plan.md)：ELF、syscall、TLS 与服务管理。
- [数据库方案](docs/03-database-and-delivery.md)：备份、还原及失败回滚。
- [兼容与性能](docs/08-compatibility-and-performance.md)：已确认缺口及优化证据。
- [客户端与数据库](docs/11-client-auth-and-database.md)：密钥、编码、原数据库行为。
- [GUI 与原生日志](docs/12-gui-reference.md)：界面、日志和历史专项。
- [游戏与缓存](docs/13-gameplay-and-cache-performance.md)、[运行时并发](docs/14-runtime-concurrency.md)：已有实测与未解决现场。
- [维护停服协调](docs/18-gui-maintenance-coordination.md)：备份/还原前逆序停服。
- [旧目录式交付](docs/19-directory-delivery.md)：已被本轮实例式结构替代，保留为历史记录。
