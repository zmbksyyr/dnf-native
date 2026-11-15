# jbz69 模板别名隔离验证

本阶段只对用户授权的 jbz69 PVF 做私有副本实验。原始客户端、原始 PVF、原版 ELF 和交付资源均未修改。

## 实验变更

jbz69 PVF 原本缺少以下三个条目：

```text
equipment/character/atswordman.lay
equipment/character/dswordman.lay
equipment/character/atpriest.lay
```

隔离副本分别复制现有的 `swordman.lay`、`dsswordman.lay`、`priest.lay` 作为同名条目。副本通过完整 PVF 树校验和条目 CRC 校验，版本仍为 `66282`，条目数从 `439165` 增至 `439168`。该副本 SHA-256 为 `e22d7a1dc6d12c8292f14caaf2741e0abadd5d7d08864ccfefd11cca34e7472d`。

## 结果

在同一份私有 GUI、数据库和原版 game 流程下，对原始 jbz69 PVF 与模板别名副本各运行一次：

- 两次都成功完成数据库连接和 `Init Taiwan Garena Auth Key ... publickey.pem Success`。
- 两次都在 `Loading Items` 阶段出现 `iconv error : Invalid or incomplete multibyte or wide character`。
- 模板别名副本没有进入 channel ready，也没有形成客户端登录通过证据。
- 别名只绕过了已知的条目缺失点，未消除编码错误或后续资源版本/字段语义差异。

原始 PVF 的对照 SHA-256 为 `dc444c3429a44c19419dba83c8b1c30e4263371afa528ba90430fda2f598e1e2`。对照日志和别名日志分别保存在：

```text
build/probes/20261115T012921209474Z/custom-pvf-original/
build/probes/20261115T012818724383Z/custom-pvf-alias/
```

## 决策

不把模板别名、临时 `iconv` 映射或 PVF 重编码器纳入交付。当前最小正确结论是：密钥链路已经通过，jbz69 PVF 的阻塞点属于编码及资源版本兼容边界；在没有匹配服务端资源定义前，继续添加别名会增加复杂度而不能提供支持承诺。
