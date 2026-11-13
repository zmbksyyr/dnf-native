# 资源调研与可行性判断

调研日期：2026-11-13。当前证据为原包静态分析、压缩测量、本地 PE 导入检查和公开文档核对；没有执行服务、插件、历史宿主或数据库。运行可行性仍需后续实验验证。

## 1. 资源与来源边界

工作区根目录为 `D:\cache\dnf-runtime`，新项目为其下的 `dnf-native`。

| 资源 | 本轮用途 | 后续处理 |
|---|---|---|
| `DNF原生运行_需求整理.md` | 唯一需求基线 | [原文归档](requirements.md)；原始字节 SHA-256 为 `8d9ed003bbafbb7332969aa464024eebbd0fc511ebb424bd79333ecfafdbfbbd` |
| `DP无脑一键端v1.2.3(密码：DP无脑一键端)/DP无脑一键端(密码：DP无脑一键端)/` | 原始服务、插件、PVF、物理数据库候选来源 | 原包只读；封样后使用独立副本 |
| `dnf-winport/src_dnf-winport_20261111/dnf-winport/` | 只读参考 README、许可证及目录结构 | 不导入代码、配置、GUI、生成器和编译产物 |
| `opendnf-win-jbz69/mysql/mysql-5.7.44-winx64/` | 四个 MySQL PE 的尺寸、哈希和普通导入 DLL 检查 | 本地文件不自动视为官方可信 payload；实施时与独立获取的官方包逐文件核对 |
| `x/dnf-native-runtime/`、`x/dnf-win-runtime/` | 阅读历史 README，了解失败风险 | 历史结论仅作参考；不接入其代码、工具链、宿主或自解压产物 |

原始 `DOF.tar.gz` 为 236,578,598 字节，SHA-256 为 `c85260526a8ce7862a254e2fab20043631c773db8d2ade7e11d95756a9f9c0b5`。参考源码 ZIP 的 SHA-256 为 `58539cf82f4dc0357400e6201bb6986ccd4099871f1c24e5647ed9132e1f47e3`。来源文件哈希与参考文档哈希见[来源记录](evidence/local-sources.json)。哈希证明输入身份和后续未改动，不能单独证明其由原厂发布或从未被第三方修改。

本轮没有阅读或移植参考 loader/compat 的实现代码。后续以公开 ABI 和独立实验形成行为规格；需要参考实现时，先记录观察到的行为及来源，再独立编写代码并记录差异。

## 2. 从原包重新得到的事实

审计程序由本项目独立编写，使用 Python 标准库读取 tar 成员，没有将成员解压到磁盘或执行输入代码。完整结果见[资源清单](evidence/resource-inventory.json)，包括每个 ELF 的 SHA-256、程序头、入口、动态依赖、版本需求、重定位类型和 TLS 模板。

| 检查项 | 本轮结果 | 可以得出的结论 |
|---|---|---|
| `DOF.tar.gz` | 726 个成员，全部为普通文件或目录；51 个 ELF：45 个 i386、6 个 x86_64 | 资源可静态清点；不是执行覆盖率 |
| 默认 `root/run` | 19 次服务启动，game 预加载 `/dp2/libdp2pre.so` | 默认原包包含安全三件套；启动顺序不是已经证明的依赖图 |
| 16 个 `df_*` 实例 | 15 个 i386，STUN 为 x86_64；均为 `ET_EXEC` | 需要两个执行宿主；不能用 x86 重编 STUN 替代 |
| 3 个 DBMW | 路径不同，文件哈希相同 | 仍是三个独立实例、配置与生命周期，不能合并成一个服务 |
| 3 个 secsvr | 均为 i386 `ET_EXEC` | 完整验收仍须覆盖其原始 ELF |
| game | 入口 `0x0807e910`，加载范围 `[0x08048000, 0x095045e0)` | 主程序固定地址是设计前提，不能通过重定位 game 解决宿主冲突 |
| STUN | 入口 `0x00401440`，加载范围 `[0x00400000, 0x00603fb8)` | x64 宿主必须避开该范围 |
| `dp2/frida/frida.so` | ELF SONAME 为 `libfrida-gadget.so`，内嵌独立版本字符串 `16.1.8`，依赖 `GLIBC_PRIVATE` | 以该哈希及 16.1.8 候选版本封样；glibc 兼容性不能只看最高公开版本号 |
| `libfd.so`、`libfd_monitor.so` | 也依赖 `GLIBC_PRIVATE`；出现 `GLIBC_2.17` / `GLIBCXX_3.4.18` 需求 | 两库是否为当前默认链必需仍待验证；不能仅凭文件名删除 |
| TSS / antisvr | 两个 31,904,293 字节文件哈希完全相同 | 后续有内容去重空间；运行时文件名和加载语义仍需保持 |
| `Script.pvf` | 257,227,917 字节 | 单一共享资源；不能靠只比较未压缩尺寸判断 ZIP 超限 |

19 项路径、参数和预加载信息已从原始 `root/run` 解析，见清单的 `startup_from_original_run`。具体实例为：stun、monitor、manager、relay、bridge、channel、dbmw_guild、dbmw_mnt、dbmw_stat、auction、point、guild、statics、coserver、community、gunnersvr、secagent、zergsvr、game。路径中三份程序均叫 `df_dbmw_r`，实例名来自各自目录。

静态 `DT_NEEDED` 不覆盖 `dlopen`、配置触发的加载、自修改代码或插件动态调用。本轮未进行完整反汇编和 syscall 路径覆盖；不沿用历史工程中的 syscall 数量、TLS 改写数量或“已跑通”表述。

## 3. 数据库资源核实

`MySQL.tar.gz` 为 1,233,216 字节，SHA-256 为 `1e6144797f3c566a97d6448d49a886b1847c83ab3ded431b2ccc8493d27f5d9d`。**它是物理数据库包**，包含 715 个普通文件、42,314,572 字节：642 个 `.frm`、24 个 `.MYD`、24 个 `.MYI`、19 个 `.opt`，以及 `ibdata1` 等文件。清单见资源证据的 `database_archive`。

包中 `ibdata1` 为 35,651,584 字节；未找到 `.sql`、`.ibd` 或 `ib_logfile*`。这表明必须先验证物理恢复、存储引擎和完整性，不能把它当成可直接导入的 SQL，也不能据缺少 redo 文件宣称可恢复。按完整相对路径 casefold 未发现重名冲突，但 SQL 标识符和配置的大小写仍须检查。

实施方案应先在独立工作副本中，用同版本 Windows MySQL 5.7.44 尝试受控恢复；能完整读出后导出逻辑种子，再导入新建实例并核对。若失败，记录具体恢复缺口，要求补充可靠逻辑种子或完整物理备份；不得覆盖原件、拼接其他版本现成库或改用 MySQL 8。此处是后续数据可行性关卡，本轮未尝试恢复。

## 4. 外部资料及其设计影响

已获取资料的 URL、访问日期、响应哈希和关键摘录见[网络资料记录](evidence/web-resources.json)。编号用于定位本轮实际阅读的资料。

| 编号 | 资料 | 对方案的影响 |
|---|---|---|
| S01 | [System V gABI：程序头](https://gabi.xinuos.com/elf/07-pheader.html) | 依据 `PT_LOAD`、`PT_INTERP`、`PT_DYNAMIC`、`PT_TLS` 加载，不依赖节表存在 |
| S04 | [Linux syscall ABI](https://man7.org/linux/man-pages/man2/syscall.2.html) | 分开处理 i386 `int 0x80` 与 x86_64 `syscall` 的寄存器、返回值和错误约定 |
| S05 | [ELF TLS 设计](https://www.akkadia.org/drepper/tls.pdf) | TLS 包含 TCB/DTV、动态模块与线程生命周期，不是替换一个寄存器就完成 |
| S06、S07 | [VirtualAlloc](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc)、[MapViewOfFile](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile) | 区分页大小和分配粒度；Linux 文件映射不能直接等同于 Windows 视图 |
| S08、S09 | [VirtualAlloc2](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc2)、[WaitOnAddress](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitonaddress) | 分别要求 Win10、Win8；Win7 路径不能硬链接这些 API |
| S10 | [Windows VEH](https://learn.microsoft.com/en-us/windows/win32/debug/vectored-exception-handling) | 只能证明异常处理机制存在，不能证明所有 Linux syscall/TLS 场景都能拦截；还要验证与 hook 的处理顺序 |
| S11、S12 | [Frida Gadget](https://frida.re/docs/gadget/)、[JavaScript API](https://frida.re/docs/javascript-api/) | Gadget 有构造时序；ELF 模块查询、ABI、回调和 `Process.platform` 都影响兼容性 |
| S19 | [Frida 17 变更](https://frida.re/news/2025/05/17/frida-17-0-0-released/) | 移除了 `Module.getExportByName` 等旧静态接口，不能把最新版本当成默认替换件 |
| S13、S14 | [MySQL 5.7 Windows 安装](https://docs.oracle.com/cd/E17952_01/mysql-5.7-en/windows-installation.html)、[数据目录初始化](https://docs.oracle.com/cd/E17952_01/mysql-5.7-en/data-directory-initialization.html) | 5.7.40 起要求 VC++ 2019；新实例初始化必须与物理恢复分开 |
| S15、S16 | [mysqldump](https://docs.oracle.com/cd/E17952_01/mysql-5.7-en/mysqldump.html)、[标识符大小写](https://docs.oracle.com/cd/E17952_01/mysql-5.7-en/identifier-case-sensitivity.html) | 混合存储引擎不能只靠 `--single-transaction`；记录 `lower_case_table_names=1` 偏差 |
| S17、S18 | [UCRT 本地部署](https://learn.microsoft.com/en-us/cpp/windows/universal-crt-deployment?view=msvc-170)、[VC 运行库分发](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files?view=msvc-170) | 评估本地部署完整依赖，固定支持目标系统且可分发的版本；不要求用户另装运行库 |

MySQL 原始 `dev.mysql.com/doc/refman/5.7/` 链接实测跳转到 9.7，故使用 Oracle 的 5.7 归档。S02、S03 的 ABI PDF 请求虽返回 HTTP 200，但未通过 PDF 内容核验，不作为已验证资料引用。

官方 Windows 包 HEAD 请求返回 200：`https://cdn.mysql.com/archives/mysql-5.7/mysql-5.7.44-winx64.zip`，Content-Length 为 352,891,656。HEAD 不证明包内容；本轮未下载完整官方包。后续必须锁定下载文件 SHA-256、版本、签名及裁剪后逐文件哈希。

## 5. MySQL 免安装与 Win7 的实际约束

本地四个 MySQL 程序均为 x64 PE，总计 52,310,312 字节。其普通导入包含 `MSVCP140.dll`、`VCRUNTIME140.dll`、`VCRUNTIME140_1.dll` 及一组 UCRT APISet DLL，印证了归档文档的 VC++ 2019 要求。当前检查仅覆盖普通导入 DLL 名，尚未审计导入函数、延迟导入、动态加载、字符集、插件和 SSL 等完整闭包。

因此单文件自解压程序必须携带经验证的本地依赖闭包；不能复用开发机已装运行库的成功结果。Win7 的 SP/补丁、CPU 指令支持、VC/UCRT 与 Frida 版本要一同封样。文档提及 SP1，但需求没有限定 SP，不能将“Win7”静默改成“已安装所有更新的 Win7”；最低基线作为审阅事项保留。

## 6. 包体测量及预算风险

以下是 zlib 级别 9、逐文件 raw DEFLATE 的实际测量，单位为十进制字节。**不包含 ZIP 头、目录、宿主、系统库闭包等，也不是已生成的最终 ZIP。**

| 候选内容 | 原始字节 | raw DEFLATE 字节 |
|---|---:|---:|
| 16 个服务 ELF | 89,828,707 | 29,245,657 |
| 整个原包 `dp2/`，120 个文件 | 96,545,626 | 32,344,093 |
| 单份 `Script.pvf` | 257,227,917 | 116,991,774 |
| 上述三项小计 | 443,602,250 | 178,581,524 |
| 本地 MySQL 四个程序 | 52,310,312 | 17,553,481 |
| `libfd.so` + `libfd_monitor.so`（是否必需待证） | 37,107,240 | 11,448,912 |
| 安全三个 ELF | 49,365,109 | 16,255,806 |
| TSS / antisvr 两份同内容文件 | 63,808,586 | 21,121,070 |

若前六项都保留，压缩小计为 **207,583,917 字节**，相对 210,000,000 字节只剩 **2,416,083 字节**，尚未加 MySQL DLL/数据种子、双架构 Linux 用户库、自研宿主、GUI 和配置。210 MB 目前是待验证的硬目标，不能承诺已满足。

实施前应先建立完整依赖闭包和代表性 ZIP。优先核实默认链是否需要两份 `libfd*`、哪些 Lua 扩展属于当前配置、同哈希文件能否只存一份并在解包时派生。不得 strip 原始 ELF、修改 PVF、删必需库、移除 DP2/Frida 或把默认必需项改称 addon 以挤出预算。安全三件套加去重 TSS 约 26.82 MB raw DEFLATE，但 70S2 来源与尺寸仍未封样，“addon 约 40 MB”仍是预算目标。

## 7. 可行性结论

建议进入“原版用户库 + 自研 Linux 内核 ABI 兼容层”的可行性封样；不批准跳过双架构 syscall/TLS、原地址保留、Win7 内存映射、DP2/Frida、数据恢复和包体六类关卡直接开发全套服务或 GUI。参考源码重建、自带参考 loader 与参考 GUI 均不构成候选交付路线。

本轮完成的是资源身份和风险定位；原版运行、插件初始化、客户端链路及三系统兼容性全部尚未验证。
