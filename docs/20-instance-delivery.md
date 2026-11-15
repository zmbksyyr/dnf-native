# 实例式交付

本轮按用户确认的八项需求改造发布结构；不再限制包体积。此前的 `gui/mysql/compatibility/native` 目录包和 CAB 形态为历史记录。

## 发布与实例

```text
DNF-Native-1.0/
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

用户选择的实例目录/
  mysql/bin/             官方 MySQL 工具及本地依赖
  data/                  该实例的数据库数据
  i686/                  32 位原版服务、库、PVF 与插件
  x86_64/                原版 STUN 与 64 位用户库
  services.ini
  server.cnf
  client.private.cnf
  service-logs/
  .mysql/                该实例的维护记录与冷快照
```

`mysql.zip` 是包含四个官方 EXE、Microsoft 本地依赖和许可材料的单个资源文件。创建后四个 EXE 都位于实例自己的 `mysql/bin/`，字节与官方导入清单相同；可以直接使用 mysql/mysqladmin/mysqldump。维护辅助程序只负责实例操作，不合并或改写官方 EXE。

`native.zip` 使用普通 DEFLATE ZIP、真实相对路径和文件名，打开即可浏览。GUI 内置 miniz 读取和解压；无 CAB、外部解压器或额外运行环境参与产品创建路径。`resources.ini` 为版本 3，包含包级与原生文件级 SHA-256。

## 八项需求与当前证据

| 需求 | 实现与验证 |
| --- | --- |
| 1. 根目录 GUI、统一资源目录 | 最终根目录只有 `DNF-Native.exe` 和 `resources/`；全树与 ZIP 成员哈希一致。 |
| 2. MySQL 单文件，释放到实例 | 资源为 `mysql.zip`；实测实例中的四个官方工具均返回 5.7.44，字节与官方清单一致。 |
| 3. GUI 资源检查 | 启动和“检查资源”显示缺失的具体文件；逐一移开 ZIP、两个 host、辅助程序和清单，均禁止新建。 |
| 4. 创建实例并自动切换 | 真实文件夹选择器创建两个中文/空格路径的实例；解压、导入、自动选中、19 个服务入口通过。 |
| 5. 选择已有实例保留配置和数据 | 从第二个实例切回第一个，SQL 读取先前保存的行 73；没有再次解压或初始化。 |
| 6. 删除整个实例文件夹 | 运行时禁用；取消保留；确认后整个目录、深层自定义文件和只读文件均移除，GUI 清空选择。目录链接指向的外部文件保持原样。 |
| 7. 原生服务端普通 ZIP | CRC、真实路径、PVF 和插件成员验证通过；产品资源中没有 CAB。 |
| 8. 裁剪无用文件、保留必要依赖 | 原生文件 1306 → 657，移除 649 项；121 个 ELF 的直接依赖无缺失，简繁中文/韩文转换、辅助服务协议与 game/DP2/Frida 启停通过。 |

新建时先校验包身份，再写入目标目录。损坏 ZIP 和非空目录实际拒绝且目标内容不变。打开已有实例时持有 `.instance.lock`，防止另一个 GUI 启动或删除同一目录；专项实测通过。关闭窗口会正常停止本窗口持有的服务和数据库。

## 裁剪依据

删除无用 gconv 编码模块、Python GDB 脚本、dracut 部署文件、审计/调试库、未引用用户库、插件示例和三份由 GUI 接管的旧 shell 启动脚本。保留原服务 ELF、配置、PVF、安全组件资源、依赖的 `.so`、Linux 动态加载器、NSS/DNS 库、所需编码、DP2/Lua/Frida 模块和 TCC 的实际运行编译输入。

`.so` 是原 ELF 的真实动态依赖，不能只保留十几个服务主程序。文件名别名和原始实体文件名也可能由加载器分别查找，因此保留需要的原路径。每次构建扫描保留 ELF 的 `DT_NEEDED`，缺失即失败；NSS/DNS 和编码的动态加载需求另行保留并实际验证。

本轮发现 EUC-KR/UHC 依赖 `libKSC.so`。专门编写实际 iconv 字节往返夹具：旧裁剪包在加载编码时返回失败，补回依赖后 GBK、GB18030、BIG5、BIG5-HKSCS、EUC-KR、UHC 与 UTF-8 的 12 项往返转换通过。该修正由已复现失败支持。

## 构建与验证

```powershell
python -B tools/build_mysql.py --build-dir build/mysql-build
python -B tools/build_gui.py --mysql-dir build/mysql-build --build-dir build/gui-build
python -B tools/build_instance_package.py --source VERIFIED_V2_NATIVE_DIRECTORY --gui-dir build/gui-build --hosts VERIFIED_COMPATIBILITY_DIRECTORY --output build/package-final
python -B tools/test_resource_encodings.py --package build/package-final
python -B tools/test_instance_workflow.py --gui build/package-final/DNF-Native.exe
```

`--source` 指此前验证过的 v2 资源快照目录，内含 `resources.cab/resources.ini`，只读使用；`--hosts` 指本项目此前独立构建、验证的兼容宿主目录，内含 `hosts/i686/dnf-host.exe` 和 `hosts/x86_64/dnf-host.exe`。转换中会校验原 CAB 及全部对象哈希，不使用参考工程构建产物。

开发机构建使用固定 LLVM-MinGW，GUI 和维护工具启用 `-Wall -Wextra -Werror`。最终流程收据为 `build/probes/20261115T053725867838Z/instance-workflow/receipt.json`，编码收据为 `build/probes/20261115T053707233017Z/resource-encodings/receipt.json`。最终 game 自然启动约 30.167 秒，原版正常停服、Frida dispose、DP2 unload 均到达，无强制回收。常规和 1120×820 最小窗口已实际查看。

在最终桌面位置另行通过 GUI 备份、将测试行 73 改为 91、还原并重新启动，SQL 读回恢复为 73；数据库和窗口正常关闭。这验证了实例内 MySQL 路径下的现有备份/还原链，收据为同一回归目录的 `desktop-maintenance.json`。

完整原生日志、SQL 及账号材料留在本地忽略目录。可提交的脱敏摘要见 [阶段证据](evidence/p20-instance-delivery.json)。本轮只验证 Windows 10 x64 10.0.19044，不外推 Win7/Win11、完整安全业务或客户端验收。

一次早期 659 文件包回归中，relay 于历史已有的 `0x0805b91a`（`TManager<RelayService>::getManager()`）访问异常，现场保留在 `build/probes/20261115T052344718204Z/instance-workflow/`。其后两轮完整流程通过；这不能证明该间歇问题已修复。原完整 CAB 阶段已出现相同异常，详见[历史记录](13-gameplay-and-cache-performance.md)；当前没有将它宣称为裁剪修复或新增业务通过。

## 桌面发布

发布目录为 `C:\Users\Administrator\Desktop\DNF-Native-1.0`，压缩包为同级 `DNF-Native-1.0.zip`，校验记录为 `DNF-Native-1.0.sha256.json`。ZIP 为 280314547 字节，SHA-256 为 `453f6a18a48611f9a6a7da9d9bbb22c2954a0a3c244b681172c249f460266834`；CRC、逐成员 SHA-256 和官方 MySQL 工具身份检查通过。

用户此前整理过的旧目录、旧 ZIP 及校验记录已整体迁移至 `D:\cache\dnf-runtime\x\desktop-before-instance-20261115-1345` 保留。当前发布包包含代码对应的最终资源，未夹带开发实例、数据库、日志或工具链。
