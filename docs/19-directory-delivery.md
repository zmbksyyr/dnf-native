# 目录式审阅包

> 历史阶段记录。本轮交付结构已由[实例式交付](20-instance-delivery.md)替代；以下路径、CAB 和命令不适用于新版 GUI。

当前交付包位于 `build/release-package/`，根目录只有四个产品目录：

```text
gui/
  DNF-Native.exe
mysql/
  bin/mysqld.exe
  bin/mysql.exe
  bin/mysqladmin.exe
  bin/mysqldump.exe
compatibility/
  mysql-control.exe
  hosts/i686/dnf-host.exe
  hosts/x86_64/dnf-host.exe
native/
  resources.ini
  resources.cab
```

`mysql/bin` 中的四个 EXE 来自锁定的官方下载包，`compatibility/mysql-control.exe` 只管理实例并直接调用这些官方程序。兼容宿主与维护工具不再藏进资源 CAB。`native/resources.cab` 包含原生文件、用户态依赖和实例种子；创建实例时由 GUI 解包到实例目录并校验清单。

GUI 从自身所在的 `gui/` 目录定位上一级产品根目录，因此可以直接从压缩包启动：

```powershell
build/release-package/gui/DNF-Native.exe --instance PRIVATE_INSTANCE
```

构建和验证命令：

```powershell
python tools/build_mysql.py
python tools/build_gui.py --build-dir build/gui-compact-v3
python tools/build_review_package.py --gui-dir build/gui-compact-v3 --resources build/review/resources --output build/release-package
python tools/test_gui_setup.py --gui build/release-package/gui/DNF-Native.exe
```

在 Windows 10 x64 10.0.19044 上，目录包首次创建验证通过：资源校验、18 个应用库、固定 root 默认密码、三份服务账号、15 个原版 peer、channel/relay/STUN、Frida 数据库接口、服务 IP 修改和两份实例正常关闭均通过。验证收据位于 `build/probes/20261115T041952451590Z/gui-setup/receipt.json`；目录包原生日志验证位于 `build/probes/20261115T031408253573Z/gui-logs/receipt.json`，通过 UTF-8/GBK、追加、轮转、尾部限制、滚动保持和原生日志显示。数据库维护控件的独立回归位于 `build/probes/20261115T031324902680Z/gui/receipt.json`。

压缩包为 `build/release-package.zip`，生成记录为 `build/release-package.receipt.json`。该阶段仍不宣称干净 Windows 7/10/11 矩阵、定制 jbz69 PVF 业务通过或完整客户端登录验收。
