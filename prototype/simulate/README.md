# prototype/simulate —— 五类模拟事件注入

板前协议联调工具 + 演示兜底方案（PRD-05 / PRD-08）。

## 用法

```bash
# 先起控制台
cd ../console && node server.js

# 再注入
cd ../simulate
node inject_events.js --list          # 列出全部场景
node inject_events.js                 # 按顺序跑完 8 个场景（约 15 秒）
node inject_events.js --scenario 4    # 只跑「呻吟/求救 → 远程推送」
node inject_events.js --delay 300     # 加快节奏
node inject_events.js --url http://192.168.43.1:8080
```

环境变量：`VELAGUARD_CONSOLE`（控制台地址）、`VELAGUARD_DEVICE`（设备 ID）。

## 场景与 PRD-08 的对应

| # | 场景 | 对应验收项 |
|---|---|---|
| 1 | 烟雾报警声 → 无人确认 → 家属紧急推送 | 场景 1 |
| 2 | 持续水流 → 警告升级 → 已处理后停止 | 场景 2 |
| 3 | 破碎/撞击 → 要求确认 | 场景 3 |
| 4 | 呻吟/求救 → 本地询问无响应 → 远程推送 | 场景 4 |
| 5 | 急促重复呼喊「快来人」→ 夜间紧急升级 | 场景 5 |
| 6 | 时间未同步（`timeReliable:false` 正确透传） | 时钟场景 |
| 7 | 断网补发幂等：同一 eventId 重复上传只更新 | 场景 7 |
| 8 | 并发：水流 + 呼救分条推送，各自独立流转 | 场景 9 |

## 与端侧的关系

本脚本产生的是**结构化摘要**，字段与端侧上传完全一致，不含任何音频或对话文本。
它走的是同一份事件协议、同一套控制台卡片渲染与状态流转逻辑——
所以真实识别不稳定时，用它做演示不会"演的是另一套东西"。

如果要连端侧一起演，用设备命令更贴近真实链路：

```sh
velaguard sim alarm_beep --conf 0.95 --times 6 --interval 1000
velaguard sim name_call_help --phrase 救命 --urgency 0.9 --times 3
velaguard ack handled
```

区别是：`velaguard sim` 注入的是**观测**（走完整状态机再决定要不要通知），
`inject_events.js` 注入的是**已成型的事件**（直接进控制台）。
前者验证端侧决策，后者验证协议与家属侧展示。
