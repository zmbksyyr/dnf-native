# 本地压缩资源样件

## 实际问题与处理

首次创建所用的未裁剪资源目录共 1308 个文件、667534605 字节。两个原生程序与该目录直接制成普通 DEFLATE ZIP 后为 **274212081 字节**，超过 210000000 字节门禁；其中逐字节相同的重复文件约占 19938832 个压缩字节。

本阶段保留全部文件内容与实例路径，按 SHA-256 将相同对象在 CAB 中保存一次，使用 Windows LZX 压缩。GUI 通过系统 Cabinet FDI 解压，每个对象先写入其一个目标路径，再复制到其他同内容路径；所有结果仍逐文件计算原清单哈希。不改变 ELF 指令、PVF、插件或原始输入，不以删除组件换取体积达标。

包含本轮优化宿主与 nosegneg 配置的新样件仍为 1308 个文件、963 个唯一对象，CAB 为 **175429927 字节**。包含以下四个文件的外层普通 DEFLATE ZIP 为 **190277074 字节**，SHA-256 为 `90911e9ff6db63610acef5b70b3d718c03797216006b24c6de41833e46114468`：

```text
DNF-Native.exe
mysql-standalone.exe
resources/resources.ini
resources/resources.cab
```

外层 ZIP 无需特殊压缩方法；运行时不调用 makecab、expand、7-Zip、Python 或下载工具。开发构建才使用系统 makecab。版本 2 清单保留原逐文件路径与哈希，增加整体 CAB 哈希；旧版本 1 的目录资源仍由原路径处理。

## 创建行为

创建前核对 CAB 哈希，损坏时不写入目标实例。目标仍要求为空，失败保留 `instance.pending` 和私有诊断。解压文件沿用实例私有权限，未列入清单的 CAB 对象明确失败，不允许多卷或不完整 CAB。按内容去重只影响包内表示，三个 DBMW 和其他同内容文件仍还原到原程序期望的各自路径。

逻辑种子与服务清单在创建中暂存于实例 `.mysql/`。应用库导入后重新导出并逐字节核对，账号与配置生成完成、数据库正常关闭后才提交正式 `services.ini`；成功时删除临时种子和模板。原始版本 1 资源与压缩包都保持只读。

## 开发复现

先按数据库方案准备并验证未压缩资源，再生成新的压缩目录与 GUI：

```powershell
python tools/build_service_cabinet.py --resources build/review-resources/resources --output build/review/resources
python tools/build_gui.py --mysql-exe build/mysql-gui/mysql-standalone.exe --build-dir build/review
python tools/test_gui_setup.py --gui build/review/DNF-Native.exe
```

开发 makecab 输入和工作目录目前要求 ASCII 路径；原生运行路径的中文、空格和括号由真实 GUI 检查覆盖。本地 ZIP 样件位于忽略目录 `build/review/native-local-resources.zip`，原生构建位于 `build/review/`。目录已存在时需使用新的输出路径。最终结果与 receipt 见[本阶段证据](evidence/p7-local-package.json)。

同一组 ZIP 成员字节的原生 GUI 首次创建已通过：损坏资源在写入目标前拒绝、已有非空目录不变、18 个原始应用库导入再导出一致、每实例四个账号分别生成、15 个 peer 协议和原版 Frida 数据库接口通过，正常停服；成功后没有遗留临时种子或模板。随后从该新建实例再次验证插件设置和关闭 DP2 的 game 自然启动、停服。ZIP 已检查 CRC；没有据此宣称在干净操作系统上独立验收通过。

## 验证边界

该样件保留了当前所有候选资源，包括可选安全文件，尚未完成 core/full addon 的分包、必要文件白名单与第三方随包材料封样。体积实测证明当前完整候选资源能够在门禁内表示；它不证明默认包可玩、19 服务完整拓扑、干净普通用户断网运行或 Win7/10/11 全部通过，也不是正式发布包。
