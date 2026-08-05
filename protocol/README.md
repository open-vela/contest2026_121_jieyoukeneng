# VelaGuard 协议契约

`event.v1`、`command.v1` 和 `ack.v1` 是设备、边缘网关和控制台之间的版本边界。
事件只允许结构化摘要，禁止原始音频、PCM、WAV、转写、完整对话和凭证字段。

当前实现范围：

- 主机端 C 协议库可以生成并严格回读 `event.v1`，上传器只因匹配的
  `messageId + deviceId + eventId + eventRevision` 接入 ACK 回收 outbox。
- 演示控制台在事务提交后返回 `eventIngressAck`，同消息重放返回成功的
  `duplicate: true`，旧修订不会覆盖新修订。
- 浏览器动作进入命令表，设备通过 `/v1/devices/:id/commands` 主动拉取，结果经
  `/v1/devices/:id/command-results` 持久化并返回 `commandResultIngressAck`。
- `command.v1` 的生产签名算法冻结为 `hmac-sha256`、base64url 编码；`demo.none`
  只允许显式 demo profile。当前固件尚未接入密钥存储和签名验证，因此生产命令通道
  仍是能力门，不能宣称已具备生产安全性。
- 当前 `vg_transport` 已隔离明文 HTTP 演示适配器；`consoleTls=true` 会明确拒绝发送，
  不会静默回退到明文。接入官方 TLS/mTLS 适配器并完成 Gemini-S1 资源与证书测试后，
  才能开放生产 profile。

`test-vectors.json` 是跨语言回归的最小输入集合。修改字段、枚举、错误码或签名规范时，
必须同步更新 schema、设备测试、控制台测试和向量，并提升主版本或写明兼容策略。
