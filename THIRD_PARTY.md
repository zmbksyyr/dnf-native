# 第三方来源

运行时使用 Zydis 4.1.1 解码实际遇到的 syscall/TLS 指令，并编码去除 Linux 段覆盖后的原生指令。manager 的 `.text` 内实际包含 GCC PIC 跳转表，不能用字节子串替换 TLS 指令；使用维护中的指令库避免自行维护完整 x86 解码器。这是指令适配，算术、原子操作和线程代码仍由 CPU 执行。

- [Zydis](https://github.com/zyantific/zydis/tree/a2278f1d254e492f6a6b39f6cb5d1f5d515659dc)，MIT，[许可证全文](LICENSES/Zydis.txt)。
- [Zycore](https://github.com/zyantific/zycore-c/tree/0b2432ced0884fd152b471d97ecf0258ff4d859f)，MIT，[许可证全文](LICENSES/Zycore.txt)。

`tools/bootstrap.py` 从官方仓库获取固定提交，构建不使用参考 DNF 工程。上游源码位于忽略的 `.local/sources/`，以上许可证随含相应代码的交付物保留。

Linux 用户库来自 [CentOS 7.9.2009 Vault](https://vault.centos.org/7.9.2009/os/x86_64/)，开发工具链来自 [LLVM-MinGW 20261104 MSVCRT](https://github.com/mstorsjo/llvm-mingw/releases/tag/20261104)。[用户库来源记录](docs/evidence/p1-user-libraries.json)包含元数据身份和十个 RPM 的 SHA-256；[工具链记录](docs/evidence/toolchain.json)包含下载身份。RPM 脚本未执行。原始许可证、文档和链接元数据保留在本地导入树；最终用户库依赖闭包、对应源代码和随包许可证尚待交付阶段封样，当前没有制作或发布运行包。

数据库开发实验使用 [Oracle 官方 MySQL 5.7.44 Windows x64 ZIP](https://cdn.mysql.com/archives/mysql-5.7/mysql-5.7.44-winx64.zip)，SHA-256 `aed661fe8120254a1dc30f5a4d5de346681922f4847cf025e2d4084eca78e70e`。`tools/import_mysql.py` 校验官方包、六个工具版本和 Oracle Authenticode，并保留官方 LICENSE、README 和 share 文件；mysql_upgrade/mysqlcheck 仅为本阶段恢复开发工具，最终 payload 尚未封样。当前不分发 MySQL；最终发布须保留官方许可材料并完成依赖闭包检查。

NTFS inode 处理依据 Microsoft 的 [MFT_SEGMENT_REFERENCE 格式](https://learn.microsoft.com/en-us/windows/win32/devnotes/mft-segment-reference)和 [GetVolumeInformationByHandleW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getvolumeinformationbyhandlew)：仅在文件系统为 NTFS 时使用低 48 位 MFT 记录号，不包含高 16 位重用序列；超过 32 位的记录号继续保留原版 glibc 的真实溢出行为。实现未使用参考 DNF 工程代码。
