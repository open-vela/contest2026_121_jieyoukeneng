#!/usr/bin/env node
/**
 * 安聆 VelaGuard - 通知与演示控制台 (PRD-05 / PRD-06)
 *
 * 双重角色：
 *   1. 板前协议联调：校验结构化事件协议、查看状态流转；
 *   2. 家属通知端：SSE 实时推送事件卡片到家属手机浏览器。
 *
 * 设计取舍：只用 Node.js 内置模块（http + node:sqlite），
 * **零 npm 依赖**——演示现场手机开热点即可 `node server.js` 起服务，
 * 不需要联网装包，符合 PRD-06「零公网依赖、零账号审批、零 App 安装」。
 *
 * 接口（与 PRD-05 一致）：
 *   POST   /events        上传事件摘要（按 eventId 幂等 upsert）
 *   GET    /events        查询事件列表
 *   GET    /events/:id    查询单条事件
 *   PATCH  /events/:id    标记已处理 / 误报 / 稍后提醒
 *   GET    /stream        SSE 实时事件流
 *   GET    /health        健康检查
 */

'use strict';

const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const { DatabaseSync } = require('node:sqlite');

const PORT = Number(process.env.VELAGUARD_PORT || 8080);
const HOST = process.env.VELAGUARD_HOST || '0.0.0.0';
const DB_PATH = process.env.VELAGUARD_DB || path.join(__dirname, 'events.db');

const EVENT_TYPES = new Set([
  'alarm_beep', 'water_flow', 'impact', 'distress_voice', 'name_call_help',
]);
const LEVELS = new Set(['notice', 'warning', 'emergency']);
const STATUSES = new Set([
  'pending', 'handled', 'false_alarm', 'snoozed', 'no_response',
]);
const UPLOAD_REASONS = new Set(['manual_test', 'escalated', 'sync']);

/* 明确禁止的字段：原始音频与对话文本绝不允许进入控制台（隐私红线） */
const FORBIDDEN_FIELDS = ['audio', 'pcm', 'wav', 'transcript', 'rawText', 'dialog'];

// ---------------------------------------------------------------------------
// 存储
// ---------------------------------------------------------------------------

const db = new DatabaseSync(DB_PATH);
db.exec(`
  CREATE TABLE IF NOT EXISTS events (
    eventId       TEXT PRIMARY KEY,
    deviceId      TEXT NOT NULL,
    eventType     TEXT NOT NULL,
    level         TEXT NOT NULL,
    confidence    REAL,
    startedAt     TEXT,
    durationSec   INTEGER,
    localStatus   TEXT,
    uploadReason  TEXT,
    timeReliable  INTEGER,
    night         INTEGER,
    matchedPhrase TEXT,
    personLabel   TEXT,
    urgency       REAL,
    repeatCount   INTEGER,
    summary       TEXT,
    advice        TEXT,
    receivedAt    TEXT NOT NULL,
    updatedAt     TEXT NOT NULL
  )
`);

const upsert = db.prepare(`
  INSERT INTO events (eventId, deviceId, eventType, level, confidence,
                      startedAt, durationSec, localStatus, uploadReason,
                      timeReliable, night, matchedPhrase, personLabel,
                      urgency, repeatCount, summary, advice,
                      receivedAt, updatedAt)
  VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
  ON CONFLICT(eventId) DO UPDATE SET
    level        = excluded.level,
    confidence   = excluded.confidence,
    durationSec  = excluded.durationSec,
    localStatus  = excluded.localStatus,
    uploadReason = excluded.uploadReason,
    summary      = excluded.summary,
    advice       = excluded.advice,
    repeatCount  = excluded.repeatCount,
    updatedAt    = excluded.updatedAt
`);

const selectOne = db.prepare('SELECT * FROM events WHERE eventId = ?');
const selectMany = db.prepare(
  'SELECT * FROM events ORDER BY receivedAt DESC, rowid DESC LIMIT ?');
const patchStatus = db.prepare(
  'UPDATE events SET localStatus = ?, updatedAt = ? WHERE eventId = ?');

// ---------------------------------------------------------------------------
// SSE
// ---------------------------------------------------------------------------

const clients = new Set();

function broadcast(type, payload) {
  const chunk = `event: ${type}\ndata: ${JSON.stringify(payload)}\n\n`;
  for (const res of clients) {
    try {
      res.write(chunk);
    } catch {
      clients.delete(res);
    }
  }
}

// ---------------------------------------------------------------------------
// 校验
// ---------------------------------------------------------------------------

function validate(evt) {
  const errors = [];
  if (!evt || typeof evt !== 'object') return ['负载不是 JSON 对象'];

  for (const f of FORBIDDEN_FIELDS) {
    if (f in evt) errors.push(`隐私红线：不允许上传字段 ${f}`);
  }

  if (!evt.eventId) errors.push('缺少 eventId');
  if (!evt.deviceId) errors.push('缺少 deviceId');
  if (!EVENT_TYPES.has(evt.eventType)) {
    errors.push(`eventType 非法: ${evt.eventType}`);
  }
  if (!LEVELS.has(evt.level)) errors.push(`level 非法: ${evt.level}`);
  if (evt.localStatus && !STATUSES.has(evt.localStatus)) {
    errors.push(`localStatus 非法: ${evt.localStatus}`);
  }
  if (evt.uploadReason && !UPLOAD_REASONS.has(evt.uploadReason)) {
    errors.push(`uploadReason 非法: ${evt.uploadReason}`);
  }
  if (evt.confidence != null &&
      (typeof evt.confidence !== 'number' ||
       evt.confidence < 0 || evt.confidence > 1)) {
    errors.push('confidence 必须在 0~1 之间');
  }
  return errors;
}

function row(evt, now, existing) {
  return [
    evt.eventId,
    evt.deviceId,
    evt.eventType,
    evt.level,
    evt.confidence ?? null,
    evt.startedAt ?? now,
    evt.durationSec ?? 0,
    evt.localStatus ?? 'pending',
    evt.uploadReason ?? 'sync',
    evt.timeReliable ? 1 : 0,
    evt.night ? 1 : 0,
    evt.matchedPhrase ?? null,
    evt.personLabel ?? null,
    evt.urgency ?? null,
    evt.repeatCount ?? 0,
    evt.summary ?? '',
    evt.advice ?? '',
    existing ? existing.receivedAt : now,
    now,
  ];
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

function send(res, code, body, headers = {}) {
  const data = typeof body === 'string' ? body : JSON.stringify(body);
  res.writeHead(code, {
    'Content-Type': typeof body === 'string'
      ? 'text/plain; charset=utf-8' : 'application/json; charset=utf-8',
    'Access-Control-Allow-Origin': '*',
    'Access-Control-Allow-Methods': 'GET,POST,PATCH,OPTIONS',
    'Access-Control-Allow-Headers': 'Content-Type',
    ...headers,
  });
  res.end(data);
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    let raw = '';
    req.on('data', (c) => {
      raw += c;
      if (raw.length > 64 * 1024) reject(new Error('负载过大'));
    });
    req.on('end', () => resolve(raw));
    req.on('error', reject);
  });
}

function serveStatic(res, urlPath) {
  const file = urlPath === '/' ? 'index.html' : urlPath.replace(/^\/+/, '');
  const full = path.join(__dirname, 'public', file);
  if (!full.startsWith(path.join(__dirname, 'public'))) {
    return send(res, 403, '禁止访问');
  }
  fs.readFile(full, (err, data) => {
    if (err) return send(res, 404, '未找到');
    const ext = path.extname(full);
    const mime = { '.html': 'text/html', '.js': 'text/javascript',
      '.css': 'text/css', '.json': 'application/json' }[ext] || 'text/plain';
    res.writeHead(200, { 'Content-Type': `${mime}; charset=utf-8` });
    res.end(data);
  });
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, `http://${req.headers.host || 'localhost'}`);
  const { pathname } = url;

  if (req.method === 'OPTIONS') return send(res, 204, '');

  // --- SSE ---------------------------------------------------------------
  if (req.method === 'GET' && pathname === '/stream') {
    res.writeHead(200, {
      'Content-Type': 'text/event-stream; charset=utf-8',
      'Cache-Control': 'no-cache',
      Connection: 'keep-alive',
      'Access-Control-Allow-Origin': '*',
    });
    res.write(`event: hello\ndata: ${JSON.stringify({ ok: true })}\n\n`);
    clients.add(res);
    const ka = setInterval(() => {
      try { res.write(': keepalive\n\n'); } catch { /* ignore */ }
    }, 15000);
    req.on('close', () => { clearInterval(ka); clients.delete(res); });
    return undefined;
  }

  if (req.method === 'GET' && pathname === '/health') {
    return send(res, 200, {
      ok: true,
      clients: clients.size,
      events: db.prepare('SELECT COUNT(*) AS n FROM events').get().n,
    });
  }

  // --- 上传事件 -----------------------------------------------------------
  if (req.method === 'POST' && pathname === '/events') {
    let evt;
    try {
      evt = JSON.parse(await readBody(req));
    } catch (e) {
      return send(res, 400, { ok: false, errors: ['JSON 解析失败'] });
    }

    const errors = validate(evt);
    if (errors.length) {
      console.warn('[console] 拒绝非法事件:', errors.join('; '));
      return send(res, 400, { ok: false, errors });
    }

    const now = new Date().toISOString();
    const existing = selectOne.get(evt.eventId);
    upsert.run(...row(evt, now, existing));
    const saved = selectOne.get(evt.eventId);

    /* 幂等：同一 eventId 重复上传只更新，不重复推送新卡片 */
    broadcast(existing ? 'update' : 'event', saved);
    console.log(`[console] ${existing ? '更新' : '新增'} ${evt.eventId} ` +
                `${evt.eventType}/${evt.level}/${saved.localStatus}`);

    return send(res, existing ? 200 : 201,
                { ok: true, duplicated: Boolean(existing), event: saved });
  }

  // --- 查询列表 -----------------------------------------------------------
  if (req.method === 'GET' && pathname === '/events') {
    const limit = Math.min(Number(url.searchParams.get('limit') || 100), 500);
    return send(res, 200, { ok: true, events: selectMany.all(limit) });
  }

  // --- 单条查询 / 状态回写 -------------------------------------------------
  const m = pathname.match(/^\/events\/([\w.-]+)$/);
  if (m) {
    const id = m[1];
    const found = selectOne.get(id);
    if (!found) return send(res, 404, { ok: false, error: '未找到事件' });

    if (req.method === 'GET') return send(res, 200, { ok: true, event: found });

    if (req.method === 'PATCH') {
      let body;
      try {
        body = JSON.parse(await readBody(req));
      } catch {
        return send(res, 400, { ok: false, error: 'JSON 解析失败' });
      }
      if (!STATUSES.has(body.localStatus)) {
        return send(res, 400,
          { ok: false, error: `localStatus 非法: ${body.localStatus}` });
      }
      patchStatus.run(body.localStatus, new Date().toISOString(), id);
      const updated = selectOne.get(id);
      broadcast('update', updated);
      console.log(`[console] 家属侧标记 ${id} -> ${body.localStatus}`);
      return send(res, 200, { ok: true, event: updated });
    }
  }

  if (req.method === 'GET') return serveStatic(res, pathname);
  return send(res, 405, { ok: false, error: '方法不支持' });
});

server.listen(PORT, HOST, () => {
  console.log('安聆 VelaGuard 通知控制台已启动');
  console.log(`  监听      : http://${HOST}:${PORT}`);
  console.log(`  家属视角  : http://<本机局域网IP>:${PORT}/`);
  console.log(`  数据库    : ${DB_PATH}`);
  console.log('  提示      : 演示时手机与设备连同一热点即可，无需公网');
});
