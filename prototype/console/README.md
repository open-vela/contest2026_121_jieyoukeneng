# prototype/console —— 通知与演示控制台

安聆 VelaGuard 的家属侧通知端 + 板前协议联调工具（PRD-05 / PRD-06）。

## 零依赖启动

```bash
cd prototype/console
node server.js            # 需要 Node.js >= 22.5（用到内置 node:sqlite）
```

**不需要 `npm install`**：只用 Node 内置模块（`http` + `node:sqlite`）。
演示现场一部手机开热点、笔记本 `node server.js` 即可，零公网依赖、
零账号审批、零 App 安装。

环境变量：

| 变量 | 默认 | 说明 |
|---|---|---|
| `VELAGUARD_PORT` | `8080` | 监听端口 |
| `VELAGUARD_HOST` | `0.0.0.0` | 监听地址 |
| `VELAGUARD_DB` | `./events.db` | SQLite 文件（含事件记录，已在 .gitignore 中排除） |
| `VELAGUARD_PROFILE` | `demo` | `demo` 允许显式演示匿名接入；`production` 默认拒绝匿名和未绑定设备 |
| `VELAGUARD_DEVICE_TOKEN` | demo 内置值 | 设备 Bearer 令牌；生产必须由环境注入，不能写入仓库 |
| `VELAGUARD_DEVICE_ID` | 空 | 生产设备绑定 ID，和设备令牌一起使用 |
| `VELAGUARD_SESSION_TOKEN` | 空 | 家属查询/命令会话令牌；生产必须配置 |
| `VELAGUARD_CORS_ORIGIN` | 空 | 只允许一个精确 Origin，不支持 `*` |
| `VELAGUARD_COMMAND_KEY_ID` | demo | 生产 `command.v1` 的 HMAC keyId |
| `VELAGUARD_COMMAND_SIGNING_KEY` | demo 内置值 | 生产命令签名密钥，只从受保护环境注入 |

## 家属视角页面

浏览器打开 `http://<笔记本局域网IP>:8080/`：

- SSE 实时推送，新事件 3 秒内弹出卡片；
- 卡片按等级配色（提醒绿 / 警告黄 / 紧急红），紧急且未处理时呼吸高亮；
- 紧急/警告事件用 Web Audio 合成提示音 + 震动（无需额外资源文件）；
- 卡片动作先创建设备命令，显示「已请求」或「待本地确认」，不会直接改事件状态；
- 断线自动重连，重连后自动补齐历史事件。

## 接口（与 PRD-05 一致）

| 方法 | 路径 | 说明 |
|---|---|---|
| `POST` | `/events` 或 `/v1/devices/:id/events` | 上传 `event.v1`，持久化后返回 `eventIngressAck` |
| `GET` | `/events?limit=n` | 查询事件列表 |
| `GET` | `/events/:id` | 查询单条事件 |
| `POST` | `/events/:id/commands` | 家属动作进入命令表，不直接改变事件 |
| `GET` | `/v1/devices/:id/commands` | 设备主动拉取待执行命令 |
| `POST` | `/v1/devices/:id/command-receipts` | 设备持久化接收回执 |
| `POST` | `/v1/devices/:id/command-results` | 设备提交结果，持久化后返回 `commandResultIngressAck` |
| `PATCH` | `/events/:id` | 旧客户端兼容入口，仅创建命令，不直接写状态 |
| `GET` | `/stream` | SSE 实时事件流 |
| `GET` | `/health` | 健康检查（事件数、在线观看端数） |

### 协议与隐私校验

上传负载会被严格校验，不合规直接 400 并记录日志：

- `eventType` / `level` / `localStatus` / `uploadReason` 必须是枚举内取值；
- `confidence` 必须在 0~1；
- 版本化 envelope 必须匹配 `deviceId`、`eventId`、`eventRevision` 和 `messageId`；旧修订不会覆盖新修订；
- ACK 只在 SQLite 事务提交后生成，设备只据此回收对应 outbox 记录；
- **隐私红线**：负载中出现 `audio` / `pcm` / `wav` / `transcript` / `rawText` /
  `dialog` 任一字段直接拒绝——控制台永远不接收原始音频与对话文本。

## 板前联调 / 演示兜底

```bash
# 另开一个终端，按 PRD-08 验收场景顺序注入五类事件
cd prototype/simulate
node inject_events.js --list          # 列出全部场景
node inject_events.js                 # 全部跑一遍
node inject_events.js --scenario 4    # 只跑「呻吟/求救 → 远程推送」
node inject_events.js --url http://192.168.43.1:8080
```

真实识别不稳定时，用同一份事件协议完成家属侧演示，走的是完全相同的
卡片渲染与状态流转路径。

## 与端侧对接

设备侧把 `consoleHost` / `consolePort` 写进 `/data/velaguard/config.json`
（模板见仓库根目录 `config.example.json`）。主机侧联调可用环境变量：

```bash
export VELAGUARD_CONSOLE_HOST=127.0.0.1
export VELAGUARD_CONSOLE_PORT=8080
./tests/build/velaguard notify test
```

控制台**不承担端侧安全判断**：它只消费结构化摘要，不能反向接管设备的
告警决策。控制台、Agent、LLM 全部不可用时，端侧本地识别、提醒、确认与
事件日志仍然完整工作（`tests/run_tests.sh` 第 5 节覆盖了这条）。当前 demo
transport 只有明文 HTTP；设备配置 `consoleTls=true` 会被端侧传输安全门拒绝，
不会静默回退到明文。生产 TLS/mTLS、设备密钥和命令验证需要通过协议目录中的能力门。
