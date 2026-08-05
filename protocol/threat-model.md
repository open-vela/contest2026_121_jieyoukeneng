# VelaGuard 威胁模型

| 威胁 | 责任边界 | 当前控制 | 状态 |
|---|---|---|---|
| 局域网伪造设备事件 | 网关设备接入 | demo token/显式匿名开关；生产 profile 要求设备凭证 | 生产待 TLS/mTLS |
| 重放或乱序事件 | 协议库、网关、outbox | `messageId`、`deviceEpoch`、`deviceSeq`、`eventRevision`、持久化 ACK | 已落地 V1 基础 |
| 浏览器越权解除紧急事件 | 命令服务、设备状态机 | 浏览器只创建命令；紧急 handled/false_alarm 返回待本地确认 | 已落地演示闭环 |
| 命令重放/过期 | 命令服务、设备 | commandId/nonce/过期字段已建模；设备签名验证尚未接入 | 生产待实现 |
| 恶意配置替换 | 配置加载器 | 临时结构、schema/range/cross-field 校验、生产签名非空门 | 签名验证与 active/previous 待实现 |
| 断电导致事实丢失 | 事件日志、outbox | 定长 CRC、提交顺序、快照 previous、启动校验 | 需 Gemini-S1 断电实测 |
| 文件/接口注入音频或对话 | 设备协议、网关、日志 | 字段白名单、递归隐私扫描、事件结构不含音频 | 已落地基础 |
| LLM prompt injection 或越权 | Agent/通知器 | Agent 只生成 advice；状态机不消费 advice；模板兜底 | 已落地边界 |
| 明文传输窃听 | transport | `consoleTls=true` 明确拒绝；demo HTTP 仅限演示 | TLS 适配器待实现 |

## 生产放行条件

生产 profile 必须同时具备 TLS 证书链/主机名校验、独立设备密钥、命令签名验证、
配置签名验证、安全存储或受保护分区，以及 Gemini-S1 上的资源和断电恢复报告。
这些条件未满足时，代码只能运行 demo profile，网络故障不能影响本地告警闭环。
