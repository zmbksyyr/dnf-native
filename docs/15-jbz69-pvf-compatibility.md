# jbz69 PVF 兼容性复核

本阶段只复核用户授权的 jbz69 客户端及其 PVF，原始客户端、PVF、原版 ELF 和原数据库保持只读。目标是区分密钥故障与 PVF 编码/版本故障；不把临时转换文件放入交付资源。

## 结果

原版 game 使用原服务包 PVF 时，数据库初始化和完整启动通过，日志包含 `Init Taiwan Garena Auth Key ... publickey.pem Success`，随后可正常完成客户端认证。原服务 PVF 的 SHA-256 为 `c3dde22a8b19e5deff5c498533e0830d13fec5605e81adeda9373841e6e22dec`，版本为 `48984`。

用户授权的 jbz69 PVF 的 SHA-256 为 `dc444c3429a44c19419dba83c8b1c30e4263371afa528ba90430fda2f598e1e2`，版本为 `66282`，包含 439165 个条目和 94583 个装备脚本。原版 game 能读到该 PVF 并完成数据库连接、RSA 公钥加载，但在 `Loading Items` 阶段反复记录 `iconv error : Invalid or incomplete multibyte or wide character` 及 `Error!!! BIG5->UTF-8`，没有到达 channel ready 或客户端认证。因此本次失败的直接根因是 PVF 文本编码/资源格式不兼容，当前证据不支持“密钥不匹配”结论。

## 转换实验

将 jbz69 PVF 中 26 个 `.str` 文件从 CP936 转为 BIG5/CP950，并修复 PVF 树尾对齐和按补齐载荷计算 CRC 后，私有副本可通过树校验、条目校验和完整读取。启动时仍未完成 `Loading Items`，说明仅重编码 `.str` 不能兼容这个版本的全部资源。

另在一次只读隔离实验中，将 `iconv_open("BIG5")` 临时映射到 `GBK`，原始 jbz69 PVF 的早期 BIG5 错误消失，但长时间仍停在 `Loading Items`，没有形成可交付的兼容结果。这表明资源版本/字段语义差异仍在，继续扩大运行时拦截会增加复杂度且缺少已证实收益。

## 当前支持边界

交付路径继续使用原服务包 PVF，并要求客户端关闭 `simplified_pvf`；该组合已通过建角、进图、结算、回城、冷重启存档读回和认证。jbz69 自定义 PVF 暂不作为支持输入，也不把临时 PVF 转换器、密钥或客户端资产提交 Git。若后续必须支持该 PVF，应先取得与其版本/字段定义匹配的服务端资源或协议说明，再做最小的版本适配。

本阶段验证仅限本机 Windows 10 x64、原版 i686 game 和现有 native host；临时探针、转换副本和运行日志均保留在忽略目录中。
