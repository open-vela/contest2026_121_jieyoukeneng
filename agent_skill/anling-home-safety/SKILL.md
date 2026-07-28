# anling-home-safety

安聆 VelaGuard 家庭安全事件通知 Skill。把设备端产生的**结构化安全事件摘要**
转成一句家属能立刻据以行动的通知文案。

## When to use

当 VelaGuard 设备上报一条 `SafetyEvent`（本地告警超时无人确认而升级），
或用户问「家里刚才发生了什么」「设备为什么报警」时使用。

PRIORITY: 紧急级事件立即执行，不要反问确认。

## 输入约定（隐私红线）

本 Skill **只接收结构化事件摘要**，字段固定如下；
**绝不接收原始音频、音频特征或家庭对话文本**，也不要向用户索取这些内容。

| 字段 | 含义 |
|---|---|
| `eventType` | `alarm_beep` / `water_flow` / `impact` / `distress_voice` / `name_call_help` |
| `level` | `notice` / `warning` / `emergency` |
| `confidence` | 综合置信度 0~1 |
| `startedAt` / `durationSec` | 发生时间与持续时长 |
| `localStatus` | `pending` / `handled` / `false_alarm` / `snoozed` / `no_response` |
| `timeReliable` | 设备时间是否已 NTP 同步 |
| `night` | 是否判定为夜间 |
| `matchedPhrase` / `personLabel` / `urgency` / `repeatCount` | 人声事件附加信息 |
| `summary` | 设备端生成的中文事件摘要 |

## How to use

1. 读取事件 JSON，按 `level` 决定语气强度：
   - `emergency` → 直接给出「立刻做什么」，不要铺垫；
   - `warning` → 说明风险 + 建议先电话确认；
   - `notice` → 只做知会，明确说明「暂不需要处理」。
2. 按 `eventType` 选择建议动作（见 `advice.json`，与端侧兜底文案一致）。
3. 若 `timeReliable` 为 false，在文案末尾附「设备时间未同步，时间仅供参考」。
4. 若 `night` 为 true 且 `level` 为 `emergency`，提示「夜间无人应答风险更高」。
5. 若 `localStatus` 为 `handled` / `false_alarm`，说明事件已由用户在设备端确认，
   **改为状态同步语气，不要再催促**。
6. 输出**一段不超过 60 字的中文文案**，不要 Markdown、不要列表、不要 emoji 堆砌。
7. 调用设备侧回灌命令，把文案交回 notifier 发送（见下）：
   `run_shell` → `velaguard notify --json '<原始事件 JSON，附加 advice 字段>'`

## 设备侧调用约定

- 端侧 notifier 的文案来源有三级回退，保证「不因 Agent 失败丢通知」：
  1. ai_agent（本 Skill，LLM 生成）
  2. 本目录的 `advice.json` 规则表（端侧直接读取，无需 LLM）
  3. 端侧内置模板（`vg_notifier_template()`）
- 因此端侧**不会阻塞等待 LLM**：本地告警、LCD 提醒、事件日志与控制台推送
  在 Agent 不可用时照常工作，Skill 文案是增强项而非依赖项。

## Example

输入事件：

```json
{
  "eventId": "evt_001",
  "deviceId": "velaguard_demo_001",
  "eventType": "name_call_help",
  "level": "emergency",
  "confidence": 0.86,
  "startedAt": "2026-07-05T22:40:00+08:00",
  "durationSec": 8,
  "localStatus": "no_response",
  "uploadReason": "escalated",
  "matchedPhrase": "快来人",
  "personLabel": "家庭成员 A",
  "repeatCount": 3,
  "night": true,
  "timeReliable": true,
  "summary": "夜间检测到急促呼喊「快来人」，重复 3 次，用户未确认"
}
```

输出文案：

> 22:40 家中检测到急促呼喊「快来人」，重复 3 次，设备提醒后无人确认。夜间无人应答风险更高，建议立即电话联系，无人接听请尽快上门查看。

再执行：

```
run_shell velaguard notify --json '{...上述事件, "advice": "22:40 家中检测到..."}'
```

## 反例（不要这样做）

- ❌ 输出「我需要听一下录音才能判断」——本 Skill 永远拿不到音频，也不应索取。
- ❌ 对 `notice` 级事件说「请立刻赶回家」——等级与语气必须匹配。
- ❌ 对 `localStatus=handled` 的事件继续催促——那是状态同步，不是新告警。
- ❌ 输出超过 60 字或带大段分析——家属在手机通知卡片上只看得到一两行。
