# anling-home-safety Agent Skill

安聆 VelaGuard 的自定义 ai_agent Skill（PRD-06）。职责是把设备端产生的
**结构化安全事件摘要**转成一句家属能立刻据以行动的通知文案。

## 文件

| 文件 | 作用 |
|---|---|
| `SKILL.md` | Skill 指令本体，供 ai_agent 加载 |
| `advice.json` | 分事件类型 / 分等级的建议动作规则表，端侧直接读取 |

## 部署

```bash
# 板端（PRD-06 指定路径）
mkdir -p /data/agent/skills/anling-home-safety
# 通过串口 / adb / TF 卡把 SKILL.md 与 advice.json 拷贝过去
```

部署成功后 `velaguard status` 的「通知文案」一行会显示来源为 `Skill 规则表`；
未部署时显示 `内置模板`，通知功能不受影响。

主机侧联调可用环境变量指定目录：

```bash
export VELAGUARD_SKILL_DIR=$PWD/agent_skill/anling-home-safety
./tests/build/velaguard notify test
```

## 文案三级回退（PRD-06 可靠性要求）

```
1. ai_agent + LLM   —— 按 SKILL.md 指令生成个性化文案
        ↓ 不可用 / 超时 / LLM 失败
2. Skill 规则表     —— 端侧直接读 advice.json，无需网络与 LLM
        ↓ 未部署 Skill
3. 端侧内置模板     —— vg_notifier_template()，永远可用
```

端侧**不会阻塞等待 LLM**：本地告警、LCD 提醒、事件日志、控制台推送在
Agent 完全不可用时照常工作。这条设计对应 PRD-06「LLM 请求失败时通知仍送达」
与 PRD-08 验收场景 8。

## LLM 生成文案如何回到通知链路

ai_agent 执行 Skill 得到文案后，调用设备命令回灌：

```bash
velaguard notify --json '{"eventId":"evt_001","deviceId":"velaguard_demo_001",
  "eventType":"name_call_help","level":"emergency","confidence":0.86,
  "startedAt":"2026-07-05T22:40:00+08:00","durationSec":8,
  "localStatus":"no_response","uploadReason":"escalated",
  "matchedPhrase":"快来人","personLabel":"家庭成员 A","repeatCount":3,
  "summary":"夜间检测到急促呼喊「快来人」，重复 3 次，用户未确认",
  "advice":"22:40 家中检测到急促呼喊…"}'
```

命令会校验字段合法性后入队上传，断网时按 `eventId` 幂等补发。

## 隐私边界

Skill 的输入**只有**结构化事件字段，不包含原始音频、音频特征或家庭对话文本；
`SKILL.md` 中明确要求模型不得索取这些内容。控制台侧还有一层字段黑名单校验
（`prototype/console/server.js` 的 `FORBIDDEN_FIELDS`），带 `audio` / `transcript`
等字段的负载会被直接拒绝并记录日志。
