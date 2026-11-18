# DNF Native：使用与构建

本项目用独立实现的 Windows x86/x64 宿主执行原版 DNF 台服 Linux ELF，提供原生 Win32 控制台。产品运行不依赖 Python、VM、WSL、Docker、驱动或系统安装的 MySQL，不安装 Windows 服务。目标系统为 Windows 7/10/11 x64；已有运行证据限 Windows 10 x64 10.0.19044，不能据此宣布其他系统通过。

当前采用 **GUI + ZIP 资源 + 独立实例目录**，包体积没有上限。旧单文件 MySQL、CAB 和 210 MB 限制属于历史交付形态。

当前发布为 **3.0**：自动识别 PVF 的 GBK／BIG5 脚本编码，新实例提供公开默认 `game / uu5!^%jg` 数据库连接与 `test / 123456` 游戏账号；附带独立简单登录器及当前密钥对导出按钮，源码随仓库管理，服务端默认公钥与其配对；沿用缺失视觉模板和共享宠物编号修复。正式发布目录为 `../DNF-Native-3.0/`，根层仅主程序与 `resources/`，登录器位于 `resources/DNFLogin.exe`。此前 ZIP 与校验文件是旧发布快照，按最新规则默认不再生成压缩包。运行 `DNF-Native.exe`；使用与升级步骤见下文。两份 PVF 的原版初始化、正常停服和新实例工作流已通过；用户已确认现有业务验证通过。自动回归范围和历史排查过程见[验证记录](validation.md)。

## 仓库目录

| 路径 | 内容 |
| --- | --- |
| `src/runtime/` | ELF 加载、指令适配和 Linux ABI 到 Windows 的兼容实现 |
| `src/controller/` | Win32 GUI、实例生命周期、服务控制、日志、IP 和插件设置 |
| `src/login/` | 用户提供的独立 C 登录器、签名和 MySQL 原生协议，含密钥对导出 |
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
| `resources/login/` | 用户授权公开的默认登录器密钥对 |
| `resources/baseline.json` | 上述三个文件的大小、SHA-256、来源和验证边界 |

两个 ZIP 纳入仓库；native.zip 已按完整 Linux 目录树规整，不纳入自研宿主或 GUI 的历史 EXE。打包时校验基底身份、原生全部成员与 ELF 直接依赖，并用本轮构建的程序重新生成产品 `resources.ini`。目录迁移保留所有 ELF、库、资产和种子原字节，仅修改服务模板中的根路径；3.0 按用户授权仅将默认公钥改为与独立登录器配对，657 个成员中其余 656 个内容不变，原 ZIP 和清单可从父提交恢复。基底内嵌的两份自研插件辅助脚本还会与当前源码核对，避免源码变化后静默发布旧实现。

GitHub 仓库使用 Git LFS 管理这两份 ZIP。克隆前准备 Git LFS 并执行 `git lfs install`；
已有克隆执行 `git lfs pull` 取回资源。GitHub 的源码下载压缩包不作为完整构建输入，
应使用已取回 LFS 实体文件的 Git 克隆；构建会核对资源哈希，LFS 指针文本不能替代 ZIP。

## GM 在线金币、复活币与疲劳

[独立 GM 工具](https://github.com/zmbksyyr/dnf-dm-c) 可连接 Native 或原版 Linux 服务端。
在线金币、复活币、补满疲劳和 SP/TP 通过游戏线程修改，再确认原游戏存档，避免退出时被内存旧值覆盖。
快捷增加按实际余额累加；金币使用原游戏的等级／PVF 金币上限，超限拒绝，不截断发放。
疲劳按原游戏当前基础与额外上限补满，并清除已使用计数；离线按角色上限补满基础疲劳，保留额外疲劳。
本项目固定原 ELF 的 `charac_stat.fatigue` 表示剩余疲劳，清零会耗尽疲劳。
“满 SP/TP”统一设置四组计数为 65535；原协议每组只有 16 位，旧值 90000 会在报文中截成 24464。
其他缓存角色数值仍需退出游戏后修改。此接口不依赖 Robot，也不新增端口。

Native 在固定 game 启用 DP2 时自动安装桥接，从实例 `login.private.cnf` 读取连接，
无需用户再填一套配置。没有该文件则跳过；DP2 未启用或桥接不可用时，GM 明确拒绝在线操作。
Linux 可使用同一份 [gm_online.lua](../src/runtime/gm_online.lua)：将其复制为
`/dp2/dnf-gm-online.lua`，创建 `/dp2/gm-online.private.json`，再于 DP2 入口加载 helper。
配置为 `{"host":"127.0.0.1","port":3306,"user":"game","password":"uu5!^%jg","channel":3011}`；
`channel` 取实际 `server_group*1000+channel_no`，密码为数据库明文连接密码，不能使用游戏配置中的加密值。
连接账号须能创建和读写 `d_taiwan.dnf_gm_hosts_v1/v2/v3/v4`、`dnf_gm_online_v1/v2/v3/v4`。
金币使用独立 v2 队列，疲劳使用 v3，SP/TP 使用 v4；旧 helper 不能处理新增命令，升级时同时更新 GM 和 helper。

“仅在线”泡点只奖励游戏线程确认的当前角色；指定账号或全服复活币若包含在线账号的多个角色，
会在提交奖励前拒绝，需改用仅在线范围或先退出该账号。不确定或部分完成时 GM 会提示并停用泡点，
应核对记录后再操作，不能直接重发整批。详见[实现和恢复语义](architecture.md#gm-在线命令)。

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

完整产品包还需要同级 `dnf-robot`（`feature/windows-host-adapter` 分支）及
`dnf-dm-c` 源码。Robot 使用 Go，GM 沿用自身的 Zig 构建入口；这些都是开发依赖，
运行发布包无需安装它们。

```powershell
python -B tools/build_all.py
```

分步入口：

```powershell
python -B tools/build.py
python -B tools/build_mysql.py
python -B tools/build_gui.py
python -B tools/build_login.py
python -B tools/build_robot.py  # 从同级 dnf-robot 分支构建可选 Windows Robot
python -B tools/build_gm.py     # 从同级 dnf-dm-c 构建独立 GM
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
| `build_login.py` | 独立登录器与默认密钥配对核验 |
| `build_robot.py` | 从同级 dnf-robot 分支构建可选 Windows Robot |
| `build_gm.py` | 从同级 dnf-dm-c 构建独立 GM，记录源码和程序哈希 |
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
    DNFLogin.exe
    robot.exe
    DNF_GM.exe
```

将整个目录复制到目标机器后打开 `DNF-Native.exe`。点击顶部“+”，选择新建或空目录；GUI 校验资源、解压程序、安全初始化数据库、导入并逐字节核对应用种子，完成后自动选中该实例。损坏资源和非空目标目录会拒绝；失败现场带 `instance.pending` 保留供诊断。

GM 随正式包放在 `resources/DNF_GM.exe`，独立打开使用，不作为服务启动或停止。
默认连接仍为 `127.0.0.1:3306`、`game / uu5!^%jg`，程序保留自己的配置；
不会读取或覆盖控制台实例的连接配置。打包要求同级 `dnf-dm-c` 源码及其现有 Zig
构建依赖，核对源码和构建收据后只交付 EXE，不附带开发机的测试配置或数据库。

界面按用户指定的 Win32 参考图独立实现：顶部为实例选择、目录、服务 IP 与整体启停；中部按 7 列平铺全部 19 项真实服务，随后是独立 MySQL 横条；底部为日志。点击左上实例名展开已记住的实例列表，或选择其他本地目录，“+”创建实例，“⋯”提供初始化、删除及维护日志入口。卡片单击选择、双击看日志，方向键按行列移动；悬停各卡片显示服务用途，例如“游戏主服务”“拍卖行”“金币寄售”，不重复服务名称或添加括号，空白处不显示提示。服务区只保留“功能”按钮；右键菜单仅保留“启动所选”“停止所选”“检查资源”三个四字动作，顶部“检查资源”将资源检查、所选服务元数据和就绪状态统一写入主日志。目录地址与服务 IP 分组显示。参考图中的演示服务名称和模拟运行状态不进入产品。成功选择有效实例后即保存到当前用户 `%LOCALAPPDATA%\DNF-Native\instances.ini`，无需启动服务；关闭重开恢复最后选择，并保留其他实例。显式 `--instance` 参数优先，不会自动启动数据库或服务。

控制台、新启动的原版服务及 MySQL 统一使用 Windows 当前时间与本机时区，无需手动补 8 小时。旧实例由新版启动时同样生效；更新后重开控制台并正常重启服务。历史日志、已有数据库记录保留原值。

标题栏每 500ms 汇总当前控制台及其全部子进程的 CPU 和内存，包括 GUI、所有服务、MySQL 和数据库工具，不计入其他控制台实例或无关程序。父子归属同时核对进程创建时间，避免 Windows 复用进程编号后误计旧系统进程。CPU 按整机全部逻辑处理器算力折算百分比，内存为各进程当前工作集之和；新进程从第二次采样开始计算 CPU，退出的进程随下次刷新移出。读取失败显示 `--`。

大块匿名缓冲区清零保留全部连接容量，使用 Windows 按需零页减少未使用缓冲区的物理驻留；地址范围与提交量仍保留。共享或文件映射、非零填充和插件预加载路径继续使用原实现。服务宿主、数据库包装器及 MySQL 服务端不再创建常驻隐藏控制台，日志仍经原有文件句柄输出。更新后正常停服、重开控制台并启动服务生效。

日志标签切换控制台、当前服务、宿主诊断与 MySQL；“日志文件”可选择该服务发现的具体应用文件、stdout、stderr，或打开当前日志目录。关键字过滤不区分英文大小写，清空后恢复原内容，编码及跟随设置继续作用于实际日志。数据库“详情”显示 UUID、逆序停服预览与维护结果；修改服务 IP 需先停止全部服务和数据库。


每个实例有自己的 `mysql/bin/`、`data/`、`home/neople/`、`dp2/`、`lib/`、`lib64/`、`services.ini`、`server.cnf`、`client.private.cnf`、`service-logs/` 和 `.mysql/`。游戏实例当前要求数据库端口 3306，原版 bridge 使用该固定端口。切换既有实例保留配置和存档，同一实例由 `.instance.lock` 防止多个窗口同时持有。

实例目录建议使用较短路径。根路径达到 260 字符时，控制台与维护工具会使用文件系统已有的短名访问同一目录，界面保留完整长名；没有可用短名会明确拒绝，并提示选择较短目录。已验证本机 NTFS 上 274 字符目录的新建、协议、备份和还原；不保证关闭短名的卷、任意深度子目录或网络路径可用，也不会修改系统短名设置。

游戏 ELF、密钥与 PVF 位于完整的 `home/neople/game/` 路径，不再被分散到架构目录。宿主 EXE 仍区分 x86/x64；32 位与 64 位 Linux 库分别放在 `lib/` 与 `lib64/`。旧实例继续按自己的 `services.ini` 读取，本次不会自动移动或覆盖既有实例。更换 ELF 时仍须同步核对清单哈希。

更换 PVF 前正常停服并保存完整冷副本。缺失装备视觉模板按数据语义兼容，不绑定 PVF 哈希或具体模板名称：装备明确提供有效层号、职业与模式在原版支持范围内时，保留层号、变体与原版提交过程，仅省去缺失模板的动作路径展开；仍需从模板取得层号时明确失败。共享宠物脚本保留各列表项的独立编号。适配仍限定固定 game ELF，不能推广到任意服务端版本。jbz69 的指定 GBK PVF 已通过服务初始化及自然停服，具体输入身份与证据见[验证记录](validation.md)。

脚本编码默认自动识别：原版加载器解密 PVF 字符串表后，宿主按实际文本区分 GBK／BIG5，再调用原版字符串赋值与 iconv 转换；自带 PVF 和指定 jbz69 PVF 均无需填写环境项。纯 ASCII 沿用 BIG5，两种编码结果相同；内容不足以可靠判断或表结构损坏时明确失败并记录原因。特殊 PVF 仍可在 `[game]` 的下一个连续 `envN` 中设置 `DNF_NATIVE_SCRIPT_ENCODING=GBK` 或 `BIG5`，显式值优先；不要覆盖其他环境项。适配限定固定 game ELF。

TCP 对端断开后，兼容层按 Linux 语义同时报告读就绪与挂断，原 monitor／guild 可读取 EOF 并清除旧频道，game 单独正常停止后可以重新注册。此前只报告挂断会留下旧频道，第二次 game 在初始化后收到重复注册拒绝并自行停服。

“整体启动”启动数据库和默认 16 个服务；三个安全组件保留独立入口。单个服务显示当前启动、运行、停止或退出状态。game 起初显示“等待应用初始化”，读取本次启动的原版完成日志后显示“应用已初始化”；重启会重新等待，不使用旧日志判断。

修改服务 IP 要在全部停止后应用；修改 DP2/Lua 和 Frida 开关要先停止 game。日志查看直接读取 stdout、stderr、服务应用日志和 MySQL 日志，可选自动、UTF-8、936、950、949 编码，并支持刷新和跟随滚动。

“功能”窗口的“等级上限”提供 50、55、60、65、70、75、80、85、86、90、95、99、100 级，默认 70。选择后点击“应用并重启”：正在运行时正常停止并重新拉起 game；停服时保存配置，下次启动生效。等级保存在实例 `services.ini` 的 `[gameplay] max_level`，宿主只修改原 ELF 的执行内存，磁盘文件、角色等级和经验不改写。PVF 的经验/SP 表、任务、物品和客户端仍须配套；仅提高上限不会生成这些内容。具体字节及兼容范围见[实现说明](architecture.md#等级规则与成长奖励)。

控制台负责金币、点券拍卖库的历史及竞价历史月表，并准备响应时间日志表。创建实例、启动数据库及拍卖相关服务前会按数据库日期准备当前月和下月，运行中每 15 分钟核对一次。缺失表自动建立；旧拍卖表补齐原服务所需字段，误用其他统计结构的响应日志表另建正确表，保留旧表。修正前保存官方 SQL 备份、SHA-256 和回滚 SQL 到实例 `.mysql/monthly-*.private.sql*`。已有结构需要修正时须停止 game、auction、point，再从控制台重启数据库；运行中只创建缺失表。月表维护不删除历史月份，物品索引另由“功能”窗口中的拍卖维护处理。

主窗口标题栏持续显示本控制器进程的 CPU 和工作集内存；数值仅描述控制台自身，不代表原版服务或数据库的资源占用。服务数量每 500 ms 读取控制器持有的实际宿主进程，不包含 Windows 控制台辅助进程。停服保留真实退出码；固定原版模块返回 1 且记录正常停止标记时不误报红色，异常退出仍标红。主日志无滚动条，支持鼠标悬停滚轮及高精度滚轮；向上翻阅自动暂停跟随，新日志追加时保留阅读位置，可重新开启“跟随滚动”回到底部。过程状态与维护结果统一写入“控制台操作”。

“功能”窗口集中运行设置、拍卖兼容设置和维护操作。运行设置从左到右一行显示等级上限、DP2、Frida，最右侧是统一的“应用并重启”按钮，无额外说明文字。八个拍卖维护按钮按两行四列排列：第一行为索引维护，第二行为红字数据维护；数据动作固定同时处理金币拍卖与点券拍卖（金币寄售），无需选择市场。顶部摘要固定两行，长 PVF 路径省略显示、悬停可查看完整路径。兼容设置位于上方；弹窗不另设日志框和底部关闭按钮，使用标题栏 × 或 Esc 关闭。

服务区的“功能”按钮打开当前实例的设置窗口；先启动该控制台持有的数据库，拍卖检查和维护按钮才可用。等级与插件设置可同时修改，Frida 依赖 DP2；统一点击“应用并重启”保存，运行中的 game 正常停止并拉起一次，其他服务保持运行；停服时保存供下次启动使用。未改动设置时按钮禁用。应用前核对固定 ELF 和所需 Lua 入口，原配置及实际修改的 Lua 模块备份在实例 `.mysql/runtime-settings-*`。

| 区域 | 功能 |
| --- | --- |
| 运行设置 | 等级上限 → DP2 → Frida → 应用并重启；一次保存并应用三项设置 |
| 兼容设置 | 扩展等级检索、锻造兼容（搜索与均价）、DP2 搜索保护、PVF 变化自动检查；点击“应用并重启相关服务”保存生效，重建同步也保存当前开关 |
| 索引维护 | 检查兼容性、重建并同步两个 iteminfo、恢复上一版索引；显示 PVF 哈希、编码、有效范围、待处理定义、存量订单及均价冲突 |
| 数据维护 | 同时处理金币拍卖和点券拍卖，红字按钮清理不兼容订单、清空在售订单、清空均价数据、重置拍卖库 |

等级和锻造范围从当前 PVF 读取，关闭扩展分别回到原规则 70／7；当前指定 jbz69 PVF 是装备等级 85、锻造 10。锻造扩展同时隔离均价档位，普通强化和真实锻造值保持原值。DP2 保护只在 Frida 模式及已识别搜索脚本下允许启用；它只用有效孔／宝石记录覆盖搜索结果，普通装备没有记录时保留原始数据。

同步的 UTF-8 索引由宿主按 UTF-8 拆分字段，避免中文名称的字节长度使分类错位；已有订单保留，更新宿主后正常重启拍卖服务即可重新建立内存索引。启用 DP2 的固定原版 game 在启动时自动安装金币寄售等级接口修复，沿用实例脚本配置的门槛；原 Lua 入口备份为 `dp2/df_game_r.lua.before-native-auction-compat`。原比较规则为“角色等级严格大于配置值”，例如配置 10 允许 11 级以上；该修复不清库，也不取消等级检查。

自动检查默认开启：相关服务启动前，以及持有数据库时每 15 分钟检查 PVF 和两份索引哈希。兼容时自动备份并同步；发现缺失物品、定义改变、超出已选范围的订单或不兼容均价时阻止更新，保留订单和索引，不自动清库。报告保存到实例 `.mysql/market-check-*.private.txt`。缺少可靠分类、名称或超出原版格式的定义会单独列出；当前生成器不是任意 PVF 的完整物品分类器，不应忽略报告中的待处理项。

写入维护先正常停止 game、auction、point 中原来运行的服务，导出两个拍卖库全部表并记录 SHA-256，再修改；索引操作同时备份两份 dat、配置、定义记录和相关 DP2 脚本。成功后重新拉起原运行服务，业务状态仍须查看实际日志。失败自动用备份回滚，相关服务保持停止，现场保留在 `.mysql/market-*/`。恢复上一版索引要求其记录的 PVF 与当前文件一致；没有来源记录的旧索引不能冒认为当前 PVF，跨 PVF 回退应使用完整冷副本。

红字按钮直接执行，没有额外确认框；删除订单不会返还物品或货币。“清空在售订单”同时移除两个市场的系统排程；“清空均价数据”包含普通、ROI 和人工均价；“重置拍卖库”重建两个市场的全部表（含历史），然后补月表和锻造均价键。以上动作均先备份，删除范围限定两个拍卖库，角色物品库保持原值；两个市场的日常月表补齐仍自动进行。

键盘可用 Tab 切换控件，方向键选择服务或筛选项，回车执行当前焦点按钮，空格切换开关；只读日志和详情框也可用 Tab 离开。Esc 取消当前选择器、下拉框或维护确认框。删除和还原默认选中“取消”；关闭控制台仍通过窗口关闭入口，并沿用正常停服流程。

固定基底中的 Frida 配置为 `on_change=reload`。已验证覆盖写入和临时文件原子替换两种 JS 保存方式，原版 Gadget 均通过轮询回退在约 5 秒后加载新版，并实际返回新版 RPC 结果；这不是即时通知。该专项在 game 自然启动前的 DP2/Lua 夹具中完成，验证上述两种保存方式及其 RPC 结果。

新实例保留可登录的数据库账号 `game`，与原版存储过程的 DEFINER 身份兼容，权限限定游戏应用库。登录器连接信息保存在实例 `login.private.cnf`，数据库“详情”提示该路径；维护连接仍使用 `client.private.cnf`。按用户要求，新实例默认 `game / uu5!^%jg`，默认游戏账号 `test / 123456`，登录器直接显示这些公开默认值；维护连接密码仍按实例生成。手动修改密码后同步更新对应私有文件。旧实例不会因打开新版而擅自重置账号密码。官方 `mysqld/mysql/mysqladmin/mysqldump` 都可在实例的 `mysql/bin/` 直接使用。

“备份”及“还原”会先逆序停止本窗口持有的服务，再核对数据库身份并正常停止。操作结束保持停止。备份目录应位于实例目录外。还原包含预备份、影子副本导入、重启与导出一致性核对、冷目录切换和失败恢复，详见[实现说明](architecture.md#数据库维护)。

顶部“⋯”菜单中的“删除当前实例”经确认永久删除整个实例目录，包括自定义内容；需要的数据先备份。取消操作保留原目录，内部目录链接不递归删除外部目标。关闭窗口会正常停止本窗口持有的服务与数据库。

升级既有实例时，采用独立产品包和实例副本：

1. 用旧版正常停止全部服务和数据库，关闭窗口；保留旧产品包，对停止后的整个实例目录做完整副本。数据库逻辑备份可以另存，但不能代替配置、插件、PVF 等实例文件的备份。
2. 将新版产品包放入新目录，使用“选择实例”打开上述实例副本。保留原 `services.ini`、数据库和配置；不要在旧实例上使用“创建实例”，也不要将新 ZIP 直接覆盖到旧实例。
3. 分别检查数据库、所需服务的原生日志与协议，再验证自己的客户端和插件。旧 `i686/`、`x86_64/` 布局继续按清单解析，无需搬移目录。
4. 需要回退时，先正常停止并关闭新版，再用保留的旧产品包打开升级前的完整冷副本。升级后产生的新数据不包含在旧副本中，不能靠覆盖运行中的 `data/` 合并。

当前升级验证覆盖一个固定旧布局副本的辅助协议、停服维护及 game 自然初始化、插件释放；其他自定义资源和数据库版本仍须按实际组合验证。三个安全组件的现有配置依赖未随包提供的配置服务/安全监控端点；不能仅改成本地配置并自连端口就当作完整安全模式，详见[验证记录](validation.md)。

P25 已用指定客户端及登录器的独立副本实际完成注册、频道认证、角色创建、角色读取和进入教程场景。该客户端的 `ijl15.dll` 需要有效游戏地址：测试通过环境变量 `GAME_SERVER_IP=127.0.0.1` 提供；原输入登录器的数据库地址不会自动设置游戏地址；3.0 附带的独立登录器在没有显式 `GAME_SERVER_IP` 时，以填写的数据库地址传递给子进程。服务器公钥必须与登录器签名密钥配对。历史客户端重启曾出现“图表系统建立失败”及访问异常；当前 RDP 下窗口化 D3D9 设备创建通过、全屏设备失败，不能把客户端显示问题当作 PVF 或认证失败。详细现场见[验证记录](validation.md)。

独立登录器的放置、密钥导出及构建见 [登录器说明](login.md)。它保留手动连接信息，不读取服务实例；源码和构建入口保留在项目仓库。新实例默认公钥已经匹配，已有实例不会自动替换公钥。

## 验证入口

```powershell
python -B tests/test_login.py
python -B tests/test_elf.py
python -B tests/test_entry.py
python -B tests/test_vm.py
python -B tests/test_vm_concurrent.py
python -B tests/test_tls.py
python -B tests/test_futex.py
python -B tests/test_futex.py --robust
python -B tests/test_descriptors.py
python -B tests/test_ld.py
python -B tests/test_luv.py
python -B tests/test_frida.py --item-reset
python -B tests/test_frida_reload.py
python -B tests/test_pvf_templates.py
python -B tests/test_game_rules.py
python -B tests/test_gui_gameplay.py --gui ../x/dnf-native-output/package/DNF-Native.exe --instance <已停止的测试实例>
python -B tests/test_auction_runtime.py
python -B tests/test_auction_guard.py
python -B tests/test_gui_auction.py --gui ../x/dnf-native-output/package/DNF-Native.exe --instance <已停止的测试实例>
python -B tests/test_resource_encodings.py --package ../x/dnf-native-output/package
python -B tests/test_instance_workflow.py --gui ../x/dnf-native-output/package/DNF-Native.exe
```

PVF 模板专项通过原 loader、全局构造和装备函数，检查有效/缺失模板交替时的层号、变体、视觉清理及必需层号拒绝；这是函数语义验证，不能代替完整启动。后两项分别验证实际字符编码转换，以及真实 Win32 双实例生命周期、辅助服务协议和 game/DP2/Frida 启停。Lua/Frida 专项以自写夹具的退出码 42 验证接口；自然启动由实例工作流及就绪专项覆盖。GUI 测试需要可交互 Windows 桌面和空闲的服务端口，会在 `../x/dnf-native-output/probes/` 创建独立实例。其他测试所需输入及本轮实际执行结果见[验证记录](validation.md)。

已有已停止的私有实例可作为 GUI 与原版停服专项的复制来源；以下命令使用外部私有实例；布局专项仅读取指定停库夹具的元数据和日志，其他专项建立独立副本：

```powershell
python -B tests/test_service_shutdown.py --instance <私有实例目录> --gui ../x/dnf-native-output/package/DNF-Native.exe
python -B tests/test_gui_services.py --instance <私有实例目录> --gui ../x/dnf-native-output/package/DNF-Native.exe
python -B tests/test_gui_readiness.py --instance <曾完成 game 初始化的停服实例目录> --gui ../x/dnf-native-output/package/DNF-Native.exe
```

停服专项覆盖 manager、三个 DBMW 在网络线程刚创建时停止，以及 manager/DBMW 稳定运行后的停止，核对真实线程 join、原版退出日志、退出码及磁盘 ELF 不变。`service shutdown failed` 与未处理异常一样会使集成验收失败。

就绪状态专项保留来源实例的旧初始化日志，验证两次真实 game 启动均先等待本次初始化，再更新 GUI；重启、切换服务和正常停服后状态正确，详情显示当前启动状态。

当前已补齐真实 game/Frida 触发的 `SA_NODEFER`、跨映射 `munmap` 和验证码字体的普通私有文件映射。双架构原语、原版 Frida hook/RPC、两种保存方式的轮询热加载与 game 自然启停均通过；inotify 本身仍未实现。安全组件加载已通过；配置依赖及验证系统见验证记录。原始收据见[验证记录](validation.md)。

## 当前性能与 Robot 接入阶段

本阶段的性能改动集中在运行时热路径：启动时缓存 trace 开关和 QPC 频率，ELF 身份校验改为 64 KiB 文件块读取，避免为计算完整 SHA-256 触碰整份只读映射。ELF 映射、重定位和 syscall 语义保持不变；x86 与 x64 宿主均已重新构建。

Robot 是独立 Windows 模块，通过 `resources/robot.exe` 随发布包提供。控制台只管理生命周期和观测，实例内的 `robot` 目录保存配置和状态；详细协议与 Windows 支持边界见 [Robot 接入](robot.md)。
