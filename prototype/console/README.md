# prototype/console —— 通知与演示控制台

安聆 VelaGuard 的家属侧通知端 + 板前协议联调工具（PRD-05 / PRD-06）。

## 零依赖启动

```bash
cd prototype/console
VELAGUARD_ALLOW_ANONYMOUS_DEVICE=true node server.js
```

**不需要 `npm install`**：只用 Node 内置模块（`http/https` + `node:sqlite`）。
演示现场一部手机开热点、笔记本 `node server.js` 即可，零公网依赖、
零账号审批、零 App 安装。

环境变量：

| 变量 | 默认 | 说明 |
|---|---|---|
| `VELAGUARD_PORT` | `8080` | 监听端口 |
| `VELAGUARD_HOST` | demo 为 `0.0.0.0`，production 为 `127.0.0.1` | 监听地址 |
| `VELAGUARD_DB` | `./events.db` | SQLite 文件（含事件记录，已在 .gitignore 中排除） |
| `VELAGUARD_PROFILE` | `demo` | `demo` 仅在显式开关下允许匿名设备接入；`production` 默认拒绝匿名和未绑定设备 |
| `VELAGUARD_DEVICE_TOKEN` | 空 | 设备 Bearer 令牌；生产必须由环境注入，不能写入仓库 |
| `VELAGUARD_DEVICE_ID` | 空 | 生产设备绑定 ID，和设备令牌一起使用 |
| `VELAGUARD_SESSION_TOKEN` | 空 | 家属查询/命令会话令牌；生产必须配置 |
| `VELAGUARD_PAIRING_KEY` | demo 随机生成 | 家属手机首次配对密钥；随机密钥会打印在控制台启动终端 |
| `VELAGUARD_ALLOW_ANONYMOUS_DEVICE` | `false` | 仅临时局域网演示可显式设为 `true`，生产禁止 |
| `VELAGUARD_TLS_CERT_FILE` / `VELAGUARD_TLS_KEY_FILE` | 空 | 同时设置时启用 Node 原生 HTTPS，最低 TLS 版本为 1.2 |
| `VELAGUARD_TLS_TERMINATED` | `false` | 没有原生证书时，生产可设为 `true` 表示前面有 HTTPS 终止代理 |
| `VELAGUARD_TRUSTED_PROXY` | 空 | 生产代理模式必填，填写代理实际来源 IP，多个地址用逗号分隔 |
| `VELAGUARD_CORS_ORIGIN` | 空 | 只允许一个精确 Origin，不支持 `*` |
| `VELAGUARD_COMMAND_KEY_ID` | demo | 生产 `command.v1` 的 HMAC keyId |
| `VELAGUARD_COMMAND_SIGNING_KEY` | demo 内置值 | 生产命令签名密钥，只从受保护环境注入 |

## 家属视角页面

小米手机与开发板、运行控制台的电脑接入同一个 Wi-Fi 或手机热点。启动后，控制台
终端会打印手机访问地址、默认设备编号和本次配对密钥；小米浏览器打开
`http://<笔记本局域网IP>:8080/`，输入密钥即可完成演示绑定：

1. 手机、开发板和电脑连同一个 Wi-Fi；电脑执行下面的演示启动命令。
2. 在开发板串口执行 `velaguard console set <电脑局域网IP> 8080`，保存设备上行地址。
3. 小米浏览器打开终端打印的家属地址；设备编号会自动填入 `velaguard_demo_001`。
4. 输入终端最后打印的配对密钥，点击「配对」；配对后再点「启用提示音」。

```bash
cd prototype/console
VELAGUARD_ALLOW_ANONYMOUS_DEVICE=true node server.js
```

这是局域网演示开关，只允许演示设备匿名上传，不能用于生产网络。配对密钥查看控制台启动日志最后一行：

- SSE 实时推送，新事件 3 秒内弹出卡片；
- 卡片按等级配色（提醒绿 / 警告黄 / 紧急红），紧急且未处理时呼吸高亮；
- 紧急/警告事件用 Web Audio 合成提示音 + 震动（无需额外资源文件）；
- 卡片动作先创建设备命令，显示「已请求」或「待本地确认」，不会直接改事件状态；
- 断线自动重连，重连后自动补齐历史事件。
- 页面首次打开输入设备编号和配对密钥，成功后会写入 HttpOnly Cookie；需要换手机时点「解除本机绑定」。
- 设备命令的最终结果会通过 SSE 回推手机，并用 `resultRevision/messageId` 防止旧结果覆盖新结果。

生产环境必须通过 HTTPS 访问。可以直接启用原生 HTTPS：

```bash
VELAGUARD_PROFILE=production \
VELAGUARD_TLS_CERT_FILE=/path/server.crt \
VELAGUARD_TLS_KEY_FILE=/path/server.key \
VELAGUARD_SESSION_TOKEN='<家属会话令牌>' \
VELAGUARD_DEVICE_ID='<设备编号>' \
VELAGUARD_PAIRING_KEY='<配对密钥>' \
node server.js
```

也可以放在可信 HTTPS 反向代理后面。此时服务只信任指定代理地址发送的单一
`X-Forwarded-Proto: https`，直接访问后端明文端口会返回 `https_required`：

```bash
VELAGUARD_PROFILE=production \
VELAGUARD_TLS_TERMINATED=true \
VELAGUARD_TRUSTED_PROXY=127.0.0.1 \
VELAGUARD_SESSION_TOKEN='<家属会话令牌>' \
VELAGUARD_DEVICE_ID='<设备编号>' \
VELAGUARD_PAIRING_KEY='<配对密钥>' \
node server.js
```

反向代理必须先删除客户端传入的 `X-Forwarded-Proto` 和 `X-Forwarded-For`，再由代理
自己覆盖写入；后端代理模式只绑定回环地址，避免手机绕过 HTTPS 终止层直接访问。

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
| `GET` | `/pairing` | 查询当前手机是否已绑定 |
| `POST` | `/pairing` | 用设备编号和配对密钥建立手机会话 |
| `DELETE` | `/pairing` | 注销当前手机会话并关闭对应 SSE |
| `GET` | `/health` | 健康检查（demo 返回事件数，production 只返回最小状态） |

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
演示 transport 使用明文 HTTP；设备配置 `consoleTls=true` 时，Gemini-S1
固件改走板端 libcurl + mbedTLS，并校验 CA、服务端名称和 mTLS 客户端证书。
生产仍需完成设备密钥安全存储、配置签名和实板证书轮换验收，任何 TLS 失败
都只影响同步，不会回退到明文或阻断本地告警。
