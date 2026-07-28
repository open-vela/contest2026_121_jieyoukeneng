#!/usr/bin/env node
/**
 * 安聆 VelaGuard - 五类模拟事件注入 (PRD-05 / PRD-08)
 *
 * 用途：
 *   1. 板前协议联调：不依赖硬件即可验证控制台、通知卡片与状态流转；
 *   2. 演示兜底：真实识别不稳定时，用同一份事件协议完成家属侧演示。
 *
 * 用法：
 *   node inject_events.js                     # 按顺序跑完 5 类事件
 *   node inject_events.js --scenario 4        # 只跑第 4 个验收场景
 *   node inject_events.js --list              # 列出全部场景
 *   node inject_events.js --url http://192.168.43.1:8080
 *
 * 注意：本脚本产生的是**结构化摘要**，与端侧上传的字段完全一致，
 * 不含任何音频或对话文本。
 */

'use strict';

const DEFAULT_URL = process.env.VELAGUARD_CONSOLE || 'http://127.0.0.1:8080';
const DEVICE_ID = process.env.VELAGUARD_DEVICE || 'velaguard_demo_001';

const args = process.argv.slice(2);
const argOf = (k, d) => {
  const i = args.indexOf(k);
  return i >= 0 && args[i + 1] ? args[i + 1] : d;
};
const baseUrl = argOf('--url', DEFAULT_URL).replace(/\/+$/, '');

const iso = (offsetSec = 0) => {
  const d = new Date(Date.now() + offsetSec * 1000);
  const p = (n) => String(n).padStart(2, '0');
  const tz = -d.getTimezoneOffset();
  const sign = tz >= 0 ? '+' : '-';
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}` +
    `T${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}` +
    `${sign}${p(Math.floor(Math.abs(tz) / 60))}:${p(Math.abs(tz) % 60)}`;
};

let seq = Date.now() % 100000;
const nextId = () => `sim_${(seq++).toString().padStart(6, '0')}`;

function evt(fields) {
  return {
    eventId: nextId(),
    deviceId: DEVICE_ID,
    confidence: 0.9,
    startedAt: iso(-fields.durationSec || 0),
    durationSec: 5,
    localStatus: 'no_response',
    uploadReason: 'escalated',
    timeReliable: true,
    night: false,
    ...fields,
  };
}

/* 与 PRD-08「最终验收场景」逐条对应 */
const SCENARIOS = [
  {
    id: 1,
    name: '烟雾报警声 → 无人确认 → 家属收到紧急推送',
    steps: [
      evt({
        eventType: 'alarm_beep', level: 'emergency', confidence: 0.95,
        durationSec: 6, localStatus: 'no_response', uploadReason: 'escalated',
        summary: '检测到疑似烟雾或燃气报警蜂鸣，持续 6 秒',
        advice: '[紧急] 疑似烟雾或燃气报警，建议立即电话确认，必要时联系邻居上门查看',
      }),
    ],
  },
  {
    id: 2,
    name: '持续水流 → 提醒 → 警告升级 → 已处理后停止通知',
    steps: [
      evt({
        eventType: 'water_flow', level: 'warning', confidence: 0.82,
        durationSec: 125, localStatus: 'no_response', uploadReason: 'escalated',
        summary: '检测到持续水流声，已持续 125 秒，疑似忘关水龙头或漏水',
        advice: '[警告] 疑似忘关水龙头或漏水，建议电话提醒关闭水阀',
      }),
      (prev) => ({
        ...prev, level: 'warning', localStatus: 'handled',
        uploadReason: 'sync', durationSec: 140,
        summary: '用户已在设备端确认处理，水流事件关闭',
      }),
    ],
  },
  {
    id: 3,
    name: '破碎/撞击 → 瞬态风险事件要求确认',
    steps: [
      evt({
        eventType: 'impact', level: 'warning', confidence: 0.88,
        durationSec: 1, localStatus: 'pending', uploadReason: 'escalated',
        summary: '检测到玻璃破碎或重物撞击声',
        advice: '[警告] 疑似跌倒或物品破碎，建议立即电话确认老人是否受伤',
      }),
    ],
  },
  {
    id: 4,
    name: '呻吟/求救声 → 本地询问无响应 → 远程推送',
    steps: [
      evt({
        eventType: 'distress_voice', level: 'emergency', confidence: 0.91,
        durationSec: 12, repeatCount: 3, urgency: 0.85,
        localStatus: 'no_response', uploadReason: 'escalated',
        summary: '检测到呻吟或痛苦呼救声，重复 3 次，用户未确认',
        advice: '[紧急] 检测到疑似求助呼喊，建议立即电话确认，无人接听请尽快上门',
      }),
    ],
  },
  {
    id: 5,
    name: '个性化录入后急促重复呼喊 → 升级（平静单次不推送）',
    steps: [
      evt({
        eventType: 'name_call_help', level: 'emergency', confidence: 0.86,
        durationSec: 8, repeatCount: 3, urgency: 0.9, night: true,
        matchedPhrase: '快来人', personLabel: '家庭成员 A',
        localStatus: 'no_response', uploadReason: 'escalated',
        summary: '夜间检测到急促呼喊「快来人」，重复 3 次，用户未确认',
        advice: '[紧急] 检测到急促求助呼喊，建议立即电话确认',
      }),
    ],
  },
  {
    id: 6,
    name: '时间未同步场景（timeReliable=false 标记正确透传）',
    steps: [
      evt({
        eventType: 'impact', level: 'warning', confidence: 0.83,
        durationSec: 1, timeReliable: false,
        localStatus: 'no_response', uploadReason: 'escalated',
        summary: '检测到玻璃破碎或重物撞击声',
        advice: '[警告] 疑似跌倒或物品破碎，建议立即电话确认（设备时间未同步，时间仅供参考）',
      }),
    ],
  },
  {
    id: 7,
    name: '断网补发幂等：同一 eventId 重复上传只更新不重复推送',
    steps: [
      evt({
        eventType: 'alarm_beep', level: 'emergency', confidence: 0.93,
        durationSec: 7, localStatus: 'no_response', uploadReason: 'escalated',
        summary: '断网期间产生的报警事件，恢复后补发',
        advice: '[紧急] 疑似烟雾或燃气报警，建议立即电话确认',
      }),
      (prev) => ({ ...prev }),               // 完全相同的重复上传
      (prev) => ({
        ...prev, localStatus: 'handled', uploadReason: 'sync',
        summary: '断网补发事件已被用户确认处理',
      }),
    ],
  },
  {
    id: 8,
    name: '并发：水流 + 呼救分条推送，各自独立流转',
    steps: [
      evt({
        eventType: 'water_flow', level: 'notice', confidence: 0.78,
        durationSec: 35, localStatus: 'pending', uploadReason: 'manual_test',
        summary: '检测到持续水流声，已持续 35 秒',
        advice: '[提醒] 疑似忘关水龙头，暂不需要处理',
      }),
      evt({
        eventType: 'distress_voice', level: 'emergency', confidence: 0.9,
        durationSec: 9, repeatCount: 2, urgency: 0.88,
        localStatus: 'no_response', uploadReason: 'escalated',
        summary: '检测到呻吟或痛苦呼救声，重复 2 次，用户未确认',
        advice: '[紧急] 检测到疑似求助呼喊，建议立即电话确认',
      }),
    ],
  },
];

async function post(payload) {
  const res = await fetch(`${baseUrl}/events`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload),
  });
  const body = await res.json().catch(() => ({}));
  const flag = body.duplicated ? '幂等更新' : '新建';
  if (!res.ok) {
    console.error(`  ✗ ${payload.eventId} 被拒绝: ${(body.errors || []).join('; ')}`);
    return null;
  }
  console.log(`  ✓ ${payload.eventId} ${payload.eventType}/${payload.level}` +
              `/${payload.localStatus} (${flag})`);
  return body.event;
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function run(scenario, delay) {
  console.log(`\n场景 ${scenario.id}: ${scenario.name}`);
  let last = null;
  for (const step of scenario.steps) {
    const payload = typeof step === 'function' ? step(last) : step;
    last = payload;
    await post(payload);
    await sleep(delay);
  }
}

async function main() {
  if (args.includes('--list')) {
    SCENARIOS.forEach((s) => console.log(`${s.id}. ${s.name}`));
    return;
  }

  const only = argOf('--scenario', null);
  const delay = Number(argOf('--delay', 1200));

  console.log(`控制台: ${baseUrl}  设备: ${DEVICE_ID}`);
  try {
    const h = await fetch(`${baseUrl}/health`).then((r) => r.json());
    console.log(`控制台在线，已有 ${h.events} 条事件，${h.clients} 个观看端`);
  } catch {
    console.error(`无法连接控制台 ${baseUrl}，请先运行 prototype/console/server.js`);
    process.exitCode = 1;
    return;
  }

  const list = only
    ? SCENARIOS.filter((s) => String(s.id) === String(only))
    : SCENARIOS;

  if (!list.length) {
    console.error(`没有编号为 ${only} 的场景，用 --list 查看`);
    process.exitCode = 1;
    return;
  }

  for (const s of list) {
    await run(s, delay);
  }

  console.log('\n注入完成。打开控制台页面即可看到家属视角的推送卡片。');
}

main();
