# DNF Native：使用与构建

本项目用独立实现的 Windows x86/x64 宿主执行原版 DNF 台服 Linux ELF，提供原生 Win32 控制台。产品运行不依赖 Python、VM、WSL、Docker、驱动或系统安装的 MySQL，不安装 Windows 服务。目标系统为 Windows 7/10/11 x64；已有运行证据限 Windows 10 x64 10.0.19044，不能据此宣布其他系统通过。

当前采用 **GUI + ZIP 资源 + 独立实例目录**，包体积没有上限。旧单文件 MySQL、CAB 和 210 MB 限制属于历史交付形态。

## 仓库目录

| 路径 | 内容 |
| --- | --- |
| `src/runtime/` | ELF 加载、指令适配和 Linux ABI 到 Windows 的兼容实现 |
| `src/controller/` | Win32 GUI、实例生命周期、服务控制、日志、IP 和插件设置 |
| `src/database/` | 官方 MySQL 工具调用、初始化、备份、影子还原与恢复 |
| `src/win32_files.h` | 控制台和维护程序共用的 Win32 文件接口 |
| `src/third_party/miniz/` | 内置 ZIP 解压所需的上游代码 |
| `resources/` | 随仓库管理的固定 ZIP 基底及校验清单 |
| `tools/` | 当前开发准备、资源/ELF 核验、编译与打包脚本 |
| `tests/` | 当前回归、C/汇编/Lua/JS 夹具，以及共用的窗口、协议和资源辅助函数 |
| `docs/` | 本文、[实现说明](architecture.md)、[验证记录](validation.md)、[第三方声明](third-party.md)与历史 JSON 证据 |

根目录只保留 `AGENTS.md` 这一份项目文档，另有必要的 Git 配置。开发缓存、构建输出、运行测试实例、日志和收据统一存放于仓库外的 `../x/dnf-native-output/`，仓库内禁止创建 `build/`；自定义构建输出也必须在 `../x/` 下。

## 资源基底

资源取自此前验证的 `DNF-Native-1.0/resources`，原件位于用户指定的 `D:\cache\dnf-runtime\x`；正常构建已经解除对此本机路径的依赖。

| 文件 | 内容 |
| --- | --- |
| `resources/native.zip` | 657 个原生服务、Linux 用户库、PVF、DP2/Lua/Frida、配置模板及应用建库种子 |
| `resources/mysql.zip` | 114 个官方 MySQL 5.7.44 程序、本地依赖及上游许可文件 |
| `resources/baseline.ini` | 统一目录的 v4 清单，含原生逐文件哈希和种子哈希 |
| `resources/baseline.json` | 上述三个文件的大小、SHA-256、来源和验证边界 |

两个 ZIP 纳入仓库；native.zip 已按完整 Linux 目录树规整，不纳入自研宿主或 GUI 的历史 EXE。打包时校验基底身份、原生全部成员与 ELF 直接依赖，并用本轮构建的程序重新生成产品 `resources.ini`。本次目录迁移保留所有 ELF、库、资产和种子原字节，仅修改服务模板中的根路径；原 ZIP 和清单可从父提交恢复。基底内嵌的两份自研插件辅助脚本还会与当前源码核对，避免源码变化后静默发布旧实现。

## 开发构建

需要 Windows x64、Python 3.11 或更新版本、Git，以及固定版本 LLVM-MinGW 20261104 MSVCRT 和 Zydis 4.1.1 / Zycore。Python 只供开发与测试。

首次准备开发依赖：

```powershell
python -B tools/bootstrap.py
```

该命令获取已固定身份的开发工具和解码库，默认保存到仓库外的 `../x/dnf-native-output/.local/`。已有开发缓存时，可在当前进程指定缓存根目录：

```powershell
$env:DNF_NATIVE_LOCAL = 'D:\cache\dnf-runtime\x\your-development-cache'
```

其内部应有 `toolchains/llvm-mingw-20261104-msvcrt-x86_64/bin/` 和 `sources/zydis-4.1.1/`。构建只读取这些依赖；不要对只读历史缓存运行导入或 bootstrap 命令。

从仓库资源完整构建：

```powershell
python -B tools/build_all.py
```

分步入口：

```powershell
python -B tools/build.py
python -B tools/build_mysql.py
python -B tools/build_gui.py
python -B tools/build_instance_package.py --output ../x/dnf-native-output/package
```

默认中间目录分别为 `../x/dnf-native-output/i686`、`../x/dnf-native-output/x86_64`、`../x/dnf-native-output/mysql-tools` 和 `../x/dnf-native-output/gui`。输出目录必须尚不存在；再次打包使用新的 `--output`。构建收据记录源码、编译器命令和产物哈希，打包校验 GUI、维护程序及宿主与其收据一致。

正常构建和基底回归不需要原始安装包、原数据库、旧 CAB、历史宿主或参考工程。旧导入、CAB 重建和阶段诊断脚本已撤除，原内容可从清理前提交 `fbb11f3` 追溯。需要数据库的专项测试使用明确指定的私有实例。

`tools/` 的职责如下，全部属于当前构建链：

| 文件 | 用途 |
| --- | --- |
| `bootstrap.py` | 固定工具链与 Zydis 开发依赖、共享路径与哈希函数 |
| `elf_manifest.py` | 离线读取全部 ELF 的段、TLS、依赖、重定位类型与未定义动态符号 |
| `resource_baseline.py` | 基底文件身份核验及 ELF 直接依赖读取 |
| `build.py` | 双架构 ELF 宿主 |
| `build_mysql.py` | 官方 MySQL 文件准备与维护程序 |
| `build_gui.py` | Win32 GUI 与 ZIP 解压组件 |
| `build_instance_package.py` | 核对资源、源码和构建收据，生成当前产品包 |
| `build_all.py` | 按顺序调用上述构建步骤 |

## 产品使用

```text
../x/dnf-native-output/package/
  DNF-Native.exe
  resources/
    native.zip
    mysql.zip
    dnf-host-x86.exe
    dnf-host-x64.exe
    mysql-control.exe
    resources.ini
    README.md
    third-party.md
```

将整个目录复制到目标机器后打开 `DNF-Native.exe`。点击“创建实例”，选择新建或空目录；GUI 校验资源、解压程序、安全初始化数据库、导入并逐字节核对应用种子，完成后自动选中该实例。损坏资源和非空目标目录会拒绝；失败现场带 `instance.pending` 保留供诊断。

每个实例有自己的 `mysql/bin/`、`data/`、`home/neople/`、`dp2/`、`lib/`、`lib64/`、`services.ini`、`server.cnf`、`client.private.cnf`、`service-logs/` 和 `.mysql/`。游戏实例当前要求数据库端口 3306，原版 bridge 使用该固定端口。切换既有实例保留配置和存档，同一实例由 `.instance.lock` 防止多个窗口同时持有。

游戏 ELF、密钥与 PVF 位于完整的 `home/neople/game/` 路径，不再被分散到架构目录。宿主 EXE 仍区分 x86/x64；32 位与 64 位 Linux 库分别放在 `lib/` 与 `lib64/`。旧实例继续按自己的 `services.ini` 读取，本次不会自动移动或覆盖既有实例。更换 ELF 时仍须同步核对清单哈希。

“整体启动”启动数据库和默认 16 个服务；三个安全组件保留独立入口，其完整业务尚未验收。单个服务的“进程运行中（业务未验证）”只表示宿主已安装停服通道；应用就绪和业务通过需要原生日志及协议/客户端验证。

修改服务 IP 要在全部停止后应用；修改 DP2/Lua 和 Frida 开关要先停止 game。日志查看直接读取 stdout、stderr、服务应用日志和 MySQL 日志，可选自动、UTF-8、936、950、949 编码，并支持刷新和跟随滚动。

维护连接配置保存在实例的 `client.private.cnf`。手动修改数据库密码后需同步更新该文件；公开记录不保存真实凭据。官方 `mysqld/mysql/mysqladmin/mysqldump` 都可在实例的 `mysql/bin/` 直接使用。

“备份”及“还原”会先逆序停止本窗口持有的服务，再核对数据库身份并正常停止。操作结束保持停止。备份目录应位于实例目录外。还原包含预备份、影子副本导入、重启与导出一致性核对、冷目录切换和失败恢复，详见[实现说明](architecture.md#数据库维护)。

“删除实例”经确认永久删除整个实例目录，包括自定义内容；需要的数据先备份。取消操作保留原目录，内部目录链接不递归删除外部目标。关闭窗口会正常停止本窗口持有的服务与数据库。

## 验证入口

```powershell
python -B tests/test_elf.py
python -B tests/test_entry.py
python -B tests/test_vm.py
python -B tests/test_vm_concurrent.py
python -B tests/test_tls.py
python -B tests/test_futex.py
python -B tests/test_futex.py --robust
python -B tests/test_ld.py
python -B tests/test_luv.py
python -B tests/test_frida.py --item-reset
python -B tests/test_resource_encodings.py --package ../x/dnf-native-output/package
python -B tests/test_instance_workflow.py --gui ../x/dnf-native-output/package/DNF-Native.exe
```

后两项分别验证实际字符编码转换，以及真实 Win32 双实例生命周期、辅助服务协议和 game/DP2/Frida 启停。Lua/Frida 专项以自写夹具的退出码 42 验证接口，不计为游戏自然启动或业务通过。GUI 测试需要可交互 Windows 桌面和空闲的服务端口，会在 `../x/dnf-native-output/probes/` 创建独立实例。其他测试所需输入及本轮实际执行结果见[验证记录](validation.md)。
