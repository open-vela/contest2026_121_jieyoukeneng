# 演示脚本（≤ 5 分钟，主线 3 分钟）

> 目标：让评委在 3 分钟内看懂「端侧优先 + 主动处置 + 隐私边界」这三件事。
> 真实识别不稳定时，用按键/串口/脚本触发**完全相同的状态机**，演示不中断。

## 演示前准备（5 分钟）

```bash
# 1) 笔记本起控制台（零 npm 依赖）
cd prototype/console && node server.js
#    记下笔记本的局域网 IP，例如 192.168.43.100

# 2) 手机开热点，笔记本 + 设备连同一热点；手机浏览器打开
#    http://192.168.43.100:8080/   ← 这就是"家属视角"

# 3) 设备侧写配置（串口/nsh）
#    cp config.example.json → /data/velaguard/config.json
#    把 consoleHost 改成笔记本 IP

# 4) 部署 Skill（可选，不部署也能演示，通知会用内置模板文案）
#    把 agent_skill/anling-home-safety/ 拷到 /data/agent/skills/
```

模拟器路线（无开发板时）完全等价，只是把「设备」换成 `./emulator.sh`。

## 主线（3 分钟）

| 时间 | 画面 | 操作 | 讲什么 |
|---|---|---|---|
| 0:00–0:20 | 问题引入 | — | 独居长辈家里出事，往往没人知道；装摄像头老人不愿意 |
| 0:20–0:35 | 设备亮相，LCD 监测首页 | `velaguard status` | 守护中、时间已同步、上传在线、模型 2.6KB、单窗口 306ms |
| 0:35–1:00 | **烟雾报警** | `velaguard sim alarm_beep --conf 0.95 --times 6 --interval 1000` | 连续 5 秒高置信度 → **直接紧急级**；本地先响，无人确认 → 家属手机弹卡片 |
| 1:00–1:25 | **持续水流** | `velaguard sim water_flow --conf 0.85 --times 30 --interval 1000` | 30 秒只提醒（不打扰）→ 超阈值升警告 → `velaguard ack handled` 后立刻停止升级 |
| 1:25–1:45 | **破碎撞击** | `velaguard sim impact --conf 0.9` | 瞬态事件要求确认；演示 `velaguard ack snooze` 稍后提醒 |
| 1:45–2:15 | **呻吟/求救** | `velaguard sim distress_voice --conf 0.95 --urgency 0.9 --times 3 --interval 900` | 先本地询问"是否需要帮助"，重复出现 → 升级 → 家属推送 |
| 2:15–2:45 | **个性化录入与急促呼喊** | 先 `velaguard sim name_call_help --phrase 老伴 --urgency 0.2`（只到提醒级、**不推送**）<br>再 `velaguard sim name_call_help --phrase 救命 --urgency 0.9 --times 3` | 这是本作品最关键的一条：**平静喊名字不打扰家属，急促重复求救才升级** |
| 2:45–3:00 | 家属手机 + 历史记录 | `velaguard ui history` | 家属侧卡片含类型/时间/持续/置信度/建议动作；本地 100 条环形日志可追溯 |

## 收尾（30 秒，隐私边界）

一句话说清三层硬约束：

1. 事件结构体里**根本没有**音频或文本字段，音频只在内存环形缓冲里流过；
2. Skill 的输入只有结构化字段，`SKILL.md` 明确禁止索取音频/对话；
3. 控制台有字段黑名单，带 `transcript` 的负载直接 400 拒绝。

现场可当场演示第 3 条：

```bash
curl -X POST -H 'Content-Type: application/json' \
  -d '{"eventId":"x","deviceId":"d","eventType":"impact","level":"warning","transcript":"家里的对话"}' \
  http://127.0.0.1:8080/events
# → 400 {"ok":false,"errors":["隐私红线：不允许上传字段 transcript"]}
```

## 可选加演（有时间才做）

| 内容 | 命令 |
|---|---|
| 断网补发 | 关掉控制台 → `velaguard sim ...` → `velaguard status` 看待发送 → 重开控制台 → `velaguard flush`，卡片补上且不重复 |
| Agent/LLM 全挂 | 删掉 Skill 目录 → 事件照样推送，文案自动回落到端侧模板 |
| 时钟不可信 | `velaguard time unsync` → `velaguard sim impact` → 夜间加严规则不生效，卡片标注「设备时间未同步」 |
| 真实识别链路 | `velaguard feed water_flow.wav`（wav 需先推到设备 `/data`） |
| 一键跑完全部验收 | `velaguard selftest`（25 项断言，约 20 秒） |

## 纯控制台演示（设备完全不可用时的兜底）

```bash
cd prototype/simulate
node inject_events.js --list       # 8 个场景与 PRD-08 一一对应
node inject_events.js              # 全部跑一遍，约 15 秒
node inject_events.js --scenario 5 # 只演"急促呼喊升级"
```

走的是同一份事件协议与同一套卡片渲染，家属视角画面完全一致。

## 录制注意事项

- 官方限 5 分钟：主线按 3 分钟设计，留 1.5 分钟讲复现步骤与 AI Coding 亮点。
- 手机画面建议用投屏或分屏录制，让"设备响 → 手机弹卡片"的因果关系一眼可见。
- 演示前跑一次 `cd tests && make test`，确保环境干净（21 项全绿再开录）。
