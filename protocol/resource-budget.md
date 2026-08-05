# Gemini-S1 资源与任务预算基线

这是当前主机代码和 demo profile 的预算表，不是生产放行报告。最终值必须用官方
Gemini-S1 构建产物的 map、栈水位、堆峰值和文件系统实测结果替换。

| 责任域 | 当前实现 | 固定上限/恢复动作 |
|---|---|---|
| safety | `vg_event_sm` 状态机；状态机锁内只更新轨道，锁外 dispatch | 普通修订队列 64；升级/关闭优先队列 16；满载计数 `vg_sm_notify_dropped()` |
| audio | 检测器产生结构化观测 | 不传音频到协议；保持现有单窗口预算 `<500ms` |
| storage | 事件日志 + outbox 快照 | 日志 100 条；outbox 64 条、每条 payload 1536 字节；P0 预留 32 条 payload 空间 |
| network | `vg_uploader_tick()` 所属上传任务/演示命令 | 一次最多发送一条；3 秒超时；408/429/5xx/网络失败退避；协议/身份/ACK 错误死信 |
| ui | UI/指示回调在状态机锁外执行 | UI 不拥有安全事实；队列/设备不可用只影响展示，不阻断状态机 |
| command | 控制台持久化命令，设备主动拉取 | 单次拉取最多 32 条；设备离线时保持 `requested`，不展示为已执行 |

## 当前未冻结的生产测量

- TLS/mTLS 握手的 RAM、Flash、任务栈和 CPU；
- `/data` 文件系统 `fsync`、`rename` 和目录同步的断电语义；
- outbox 最大 P0 事件率、最长离线时长和闪存写放大；
- 安全存储/证书轮换、Secure Boot 与 A/B 或等价回滚能力。

在这些测量完成前，不得把 demo 的 64 条 outbox 容量、明文 HTTP 或 `demo.none` 命令
签名当作量产参数。
