# Robot 模块接入

## 目标

Robot 保持独立项目和独立业务代码。`dnf-native` 只负责把它作为一个可选 Windows 服务纳入实例生命周期：读取开关、放到服务列表末尾、启动、停止、日志和资源统计。GUI 不复制 Robot 的拍卖、机器人或数据库业务。

源码仓库位于 `D:\cache\dnf-runtime\dnf-robot`，构建脚本为 `python -B tools/build_robot.py`，输出固定在仓库外的 `../x/dnf-native-output/robot/robot.exe`。发布包把它放在 `resources/robot.exe`。

## 实例配置

在实例 `services.ini` 的 `[robot]` 节写入 `enabled=1` 后，重新加载实例即可看到最后一个 `robot` 服务。功能窗口的 Robot 开关与等级、DP2、Frida 设置一起应用；应用时如果状态发生变化，GUI 会先停止现有 Robot，再按新配置启动。

Robot 使用独立目录 `实例目录\robot`，不会把运行状态写入发布目录。首次启动自动创建：

- `robot\conf\config.ini`：默认 `127.0.0.1:3306`、`game / uu5!^%jg`、库 `d_taiwan`；
- `robot\conf\host-profile.json`：记录宿主适配器、服务身份和拍卖维护归属；
- `robot\logs`、`robot\state`、`robot\keys` 等运行目录。

GUI 通过 `--config-dir` 传入实例目录，并创建唯一命名事件 `Local\\dnf-native-robot-stop-*`。Robot 启动后记录 `robot started` 才报告初始化完成；停止通过命名事件并等待进程退出，超时保留进程和日志供定位。

## 宿主适配层

`host-profile.json` 是 Robot 的轻量宿主差异接口。它描述 game、auction、point 的进程身份、路径和能力，Robot 的业务层只读取配置，不依赖 `dnf-native` 的进程名。Linux 独立启动时自动生成 Linux profile；由 Native GUI 管理的 Windows 实例生成 `dnf-native-windows` profile，并把市场维护归属标记为 `controller`。

Windows Robot 当前支持自身配置、数据库、PVF、TCP/Web API、日志和生命周期。依赖 `/proc`、Linux shell、`pidof` 或进程内存写入的市场维护入口会返回明确错误；当 profile 把市场维护交给 controller 时，Robot 自动任务记录归属并退出本轮，不把跳过伪装成成功。Linux 原有市场维护路径保持独立。

## 资源与验证

发布资源清单包含 `robot.exe`，其 SHA-256 写入 `resources/resources.ini`，GUI 在启动时校验。已执行：

- Windows 交叉构建 `GOOS=windows GOARCH=amd64 CGO_ENABLED=0`；
- Robot 全量 `go test ./...`；
- Robot `--help` 启动检查；
- Native x86/x64、GUI、MySQL、登录器和实例打包构建；
- 资源包哈希和默认登录器公钥配对检查。

业务运行仍由目标实例、数据库和客户端组合决定；本模块的生命周期就绪以 Robot 自身启动日志和命名停止协议为准。
