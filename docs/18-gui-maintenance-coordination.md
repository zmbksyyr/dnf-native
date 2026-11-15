# GUI 维护停服协调

本阶段补齐 GUI 备份和还原的实际停服路径。此前按钮只在服务和数据库都停止时可用，用户必须先执行整体停止；这与“默认停服逻辑备份”的要求不符。

现在备份或还原按钮在实例已初始化且没有未完成操作时可用。若当前窗口持有运行中的数据库，控制器先按服务清单逆序请求原版服务正常退出，等待服务退出后再通过本次会话的身份和 TLS 核对正常停止数据库，最后调用已有的原生离线备份或还原操作。维护操作完成后服务和数据库保持停止，便于检查备份结果和还原后的冷快照；服务不是本窗口拥有的数据库时，操作会明确拒绝，不会停掉外部实例。

## 验证

使用审阅入口 `build/review/DNF-Native.exe`（SHA-256 `7fb3ea9973261df15954e7c79090032e7303410c752f03e9f034b711da573f00`）和独立私有实例执行：

```powershell
python tools/test_gui_maintenance.py --instance PRIVATE_INSTANCE --gui build/gui-maintenance/DNF-Native.exe
```

实测启动 16 个自动服务后，备份按钮保持可用、数据库停止按钮保持禁用；点击备份后所有自动服务退出，数据库正常关闭，备份目录生成 `manifest.txt`，控制台原生日志含“维护前已按逆序停止服务和数据库”，GUI 及其子进程以 0 退出，无强制清理。专项 receipt 为 `build/probes/20261115T020258055548Z/gui-maintenance/receipt.json`，窗口截图为同目录的 `after-coordinated-backup.png`。当前审阅 ZIP 为 190420043 字节，SHA-256 `0f8b93e736cb714e5777c270478f505867b6a39dbe0ab5c553e1370d84badedb`，CRC 和成员哈希均通过。验证环境为 Windows 10 x64 10.0.19044，实例和数据库均为私有副本。

实现仍复用既有 `stop_service`、`stop_database` 和 `mysql-standalone.exe` 维护命令，没有引入新的服务编排框架或外部依赖。失败时保留已停止的服务、数据库和私有操作日志，交由现有控制台记录继续诊断。
