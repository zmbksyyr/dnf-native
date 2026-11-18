# Robot 模块

Robot 是独立的可选 Windows 模块。Native GUI 只负责开关、实例生命周期、密钥准备、服务列表、日志和资源统计，不复制 Robot 的拍卖、机器人或数据库业务。

源码位于 `D:\cache\dnf-runtime\dnf-robot`，Native 构建使用 `python -B tools/build_robot.py`，输出在仓库外 `../x/dnf-native-output/robot/robot.exe`；正式包放在 `resources/robot.exe`。

## GUI 接口

在“功能”窗口启用 Robot 后，开关立即写入当前实例 `services.ini`，Robot 出现在服务列表末尾；整体启动会按正常服务顺序启动它，停止、日志和资源统计沿用同一套服务控制。启用期间 Native 的拍卖维护按钮全部置灰，拍卖索引、数据库和内存维护由 Robot 负责。关闭 Robot 后 Native 维护恢复。

Native 只在 game 目录补齐缺失的默认 `privatekey.pem`，并调用 Robot 的密钥检查入口；不会写入 Robot 自己的 `robot/keys`。Robot 启动后按自己的规则同步和校验公钥，已有自定义密钥不被覆盖。Robot 的配置、日志和状态均位于实例 `robot/` 目录。

## 宿主配置契约

Native 每次启动 Robot 前生成 `robot/conf/host-profile.json`。它只描述宿主事实：game、auction、point、monitor、relay 的 Windows host 可执行文件、原始 ELF 路径和 SHA-256、PID 标记、工作目录、完整启动参数和命名停止事件，以及 `market_owner=robot`、`memory_model=refine-banks-31`。Robot 的业务代码不依赖 Native 的进程名或 GUI 控件。

Robot Windows 平台层通过 PID 标记、host 路径、原始 ELF 映射头和 profile 哈希核对身份；停止使用命名事件并等待原始服务退出，启动使用 profile 的完整参数。Robot 重启市场服务后，Native 根据 PID 标记和 Job Object 重新接回句柄，所以 GUI 的单服务控制和总数统计仍然有效。

Windows 市场兼容使用原始拍卖 ELF 的已核对地址和旧字节：平均价格 refine 保留原算法上限 7，搜索 refine 为 31，等级为 127；Linux 继续使用原有 127 补丁表。索引同步会把 PVF 导出的 UTF-8 名称转换为原版可接受的基础 Big5，无法表示的字符安全回退为带物品 ID 的 ASCII 名称，已有服务索引原字节保持不变。

组队和邮箱补丁由 Robot Windows 平台层暂停已核对 game 的线程、核验旧字节、写入并回读，再恢复线程；端口 PID 必须与 profile 身份相符。Windows Web 的 Robot 重启在同一 PID 内重建运行资源，保留 GUI 的进程、停止事件和 Job 句柄。`/root/run`、`/root/stop` 按钮在 Windows 置灰，直接调用同样明确拒绝；game 上限从实际 profile 配置读取，修改后需重启 game。

Windows 资源统计读取实际进程 CPU 时间及工作集，CPU 按单核百分比报告；Linux 保留原有统计实现。连接读缓冲复用，断开时清理会话库存、摊位、组队和城镇缓存，保留重连所需令牌与密钥。手动批量上线沿用现有登录速率，批量登出最多 16 个 actor 并行；退出时取消未完成的上线确认，取消项明确报告失败。分解摊先等待完整登录，再单独计时机器确认窗口。

manager 关闭会中断同 UID 重登、离线清理、选角存档和分解摊确认等待。离线清理以取消错误终止，禁止后续数据库准备或恢复写入；正常运行的存档安全边界不因自动行为停止而跳过。相关批量和取消语义是公共业务修复，Linux 也会使用，不能表述为 Linux 业务完全没有变化。

## 独立运行与 Linux 边界

Robot 仍可在 Linux 独立运行，原有 `/proc`、shell、进程和内存路径保留；宿主 profile 只在实例首次初始化时生成。Windows 版本禁止把 Linux `/proc` 假装成可用能力，缺少完整 profile 或目标身份不匹配时明确失败并记录日志。Web/API、数据库、PVF、密钥和机器人业务均由 Robot 自己处理，Native 不增加业务代理层。

## 构建和验证

Windows 构建固定为 `GOOS=windows GOARCH=amd64 CGO_ENABLED=0`；Linux 交叉编译使用 `GOOS=linux GOARCH=amd64 CGO_ENABLED=0`。完整 Go 单测、Windows 宿主身份和内存原语测试、PVF/索引编码测试均在仓库外收据记录。Native 真实 GUI 回归使用 Windows 10 IoT Enterprise LTSC x64 10.0.19044、Go 1.26.4 和独立数据库副本；只有实际完成的服务初始化、角色上线、移动和停服结果才列为通过。
