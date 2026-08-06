#!/usr/bin/env node
/**
 * 安聆 VelaGuard - 通知与演示控制台 (PRD-05 / PRD-06)
 *
 * 双重角色：
 *   1. 板前协议联调：校验结构化事件协议、查看状态流转；
 *   2. 家属通知端：SSE 实时推送事件卡片到家属手机浏览器。
 *
 * 设计取舍：只用 Node.js 内置模块（http/https + node:sqlite），
 * **零 npm 依赖**——演示现场手机开热点即可 `node server.js` 起服务，
 * 不需要联网装包，符合 PRD-06「零公网依赖、零账号审批、零 App 安装」。
 *
 * 接口（与 PRD-05 一致）：
 *   POST   /events        上传事件摘要（按 eventId 幂等 upsert）
 *   GET    /events        查询事件列表
 *   GET    /events/:id    查询单条事件
 *   POST   /events/:id/commands  创建家属侧命令
 *   PATCH  /events/:id    兼容入口：只创建命令，不直接改事件
 *   GET    /stream        SSE 实时事件流
 *   GET    /health        健康检查
 *   GET    /pairing       局域网配对状态
 *   POST   /pairing       使用配对密钥建立家属会话
 *   DELETE /pairing       注销当前手机会话
 */

'use strict';

const http = require('node:http');
const https = require('node:https');
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const { DatabaseSync } = require('node:sqlite');

const PORT = Number(process.env.VELAGUARD_PORT || 8080);
const DB_PATH = process.env.VELAGUARD_DB || path.join(__dirname, 'events.db');
const DEMO_PROFILE = (process.env.VELAGUARD_PROFILE || 'demo') !== 'production';
const PRODUCTION_TLS_TERMINATED =
  process.env.VELAGUARD_TLS_TERMINATED === 'true';
const TLS_CERT_FILE = process.env.VELAGUARD_TLS_CERT_FILE || '';
const TLS_KEY_FILE = process.env.VELAGUARD_TLS_KEY_FILE || '';
const NATIVE_TLS = Boolean(TLS_CERT_FILE || TLS_KEY_FILE);
const TRUSTED_PROXY_ADDRESSES = new Set(
  (process.env.VELAGUARD_TRUSTED_PROXY || '')
    .split(',').map((value) => value.trim()).filter(Boolean));
const HOST = process.env.VELAGUARD_HOST ||
  (DEMO_PROFILE ? '0.0.0.0' : '127.0.0.1');
const SESSION_TOKEN = process.env.VELAGUARD_SESSION_TOKEN || '';
const EXPECTED_DEVICE_ID = process.env.VELAGUARD_DEVICE_ID || '';
const PAIRING_DEVICE_ID = EXPECTED_DEVICE_ID ||
  (DEMO_PROFILE ? 'velaguard_demo_001' : '');
const PAIRING_DEVICE_HINT = PAIRING_DEVICE_ID;
const COMMAND_ALG = DEMO_PROFILE ? 'demo.none' : 'hmac-sha256';
const COMMAND_KEY_ID = process.env.VELAGUARD_COMMAND_KEY_ID ||
  (DEMO_PROFILE ? 'demo' : '');
const COMMAND_SIGNING_KEY = process.env.VELAGUARD_COMMAND_SIGNING_KEY ||
  (DEMO_PROFILE ? 'demo-command-key' : '');
const configuredPairingKey = process.env.VELAGUARD_PAIRING_KEY || '';
const PAIRING_KEY = configuredPairingKey ||
  (DEMO_PROFILE ? crypto.randomBytes(6).toString('hex') : '');
const PAIRING_SESSION_TTL_MS = 24 * 60 * 60 * 1000;
const FAMILY_COOKIE = DEMO_PROFILE ? 'vg_session' : '__Host-vg_session';
const MAX_PAIRING_SESSIONS = 64;
const PAIRING_FAILURE_WINDOW_MS = 5 * 60 * 1000;
const MAX_PAIRING_FAILURES = 5;
const MAX_PAIRING_DEVICE_ID_LENGTH = 32;
const MAX_PAIRING_FAILURE_BUCKETS = 512;
const MAX_SSE_CLIENTS = 64;
const MAX_SSE_CLIENTS_PER_TOKEN = 4;
const MAX_SSE_CLIENTS_PER_ADDRESS = 8;
const MAX_PRIVACY_DEPTH = 32;
const MAX_PRIVACY_NODES = 2048;
const MAX_COMMAND_TTL_MS = 5 * 60 * 1000;
const MAX_COMMAND_RESULT_REVISION = 1000000;
const pairingSessions = new Map();
const pairingAddressFailures = new Map();
const pairingDeviceFailures = new Map();
let staticSessionRevoked = false;

if (NATIVE_TLS && (!TLS_CERT_FILE || !TLS_KEY_FILE)) {
  console.error('原生 HTTPS 必须同时设置 VELAGUARD_TLS_CERT_FILE 和 VELAGUARD_TLS_KEY_FILE');
  process.exit(2);
}

if (!DEMO_PROFILE && !NATIVE_TLS && !PRODUCTION_TLS_TERMINATED) {
  console.error('生产 profile 必须配置原生 HTTPS 证书，或设置 VELAGUARD_TLS_TERMINATED=true 运行在可信 HTTPS 终止层之后');
  process.exit(2);
}

if (!DEMO_PROFILE && !NATIVE_TLS && TRUSTED_PROXY_ADDRESSES.size === 0) {
  console.error('生产反向代理模式必须设置 VELAGUARD_TRUSTED_PROXY（逗号分隔的代理 IP）');
  process.exit(2);
}

if (!DEMO_PROFILE && !NATIVE_TLS && !['127.0.0.1', '::1'].includes(HOST)) {
  console.error('生产反向代理后端必须绑定本机回环地址');
  process.exit(2);
}

let TLS_OPTIONS = null;
if (NATIVE_TLS) {
  try {
    TLS_OPTIONS = {
      key: fs.readFileSync(TLS_KEY_FILE),
      cert: fs.readFileSync(TLS_CERT_FILE),
    };
  } catch (error) {
    console.error(`读取 HTTPS 证书失败: ${error.message}`);
    process.exit(2);
  }
}

const EVENT_TYPES = new Set([
  'alarm_beep', 'water_flow', 'impact', 'distress_voice', 'name_call_help',
]);
const LEVELS = new Set(['notice', 'warning', 'emergency']);
const STATUSES = new Set([
  'pending', 'handled', 'false_alarm', 'snoozed', 'no_response',
]);
const UPLOAD_REASONS = new Set(['manual_test', 'escalated', 'sync']);
const ACTION_TYPES = new Set(['handled', 'false_alarm', 'snooze']);
const ENVELOPE_SCHEMA = 'event.v1';
const ACK_SCHEMA = 'ack.v1';

/* 明确禁止的字段：原始音频与对话文本绝不允许进入控制台（隐私红线） */
const FORBIDDEN_FIELDS = ['audio', 'pcm', 'wav', 'transcript', 'rawText', 'dialog'];

function isObject(v) {
  return v != null && typeof v === 'object' && !Array.isArray(v);
}

function isFiniteNumber(v, min, max) {
  return typeof v === 'number' && Number.isFinite(v) && v >= min && v <= max;
}

function idOk(v) {
  return typeof v === 'string' && /^[A-Za-z0-9_.-]+$/.test(v) && v.length > 0;
}

function textOk(v, maxLen = 512) {
  return typeof v === 'string' && v.length <= maxLen && !/[\u0000-\u001f]/.test(v);
}

function nowIso() {
  return new Date().toISOString();
}

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
    messageId     TEXT,
    deviceEpoch   TEXT,
    deviceSeq     INTEGER NOT NULL DEFAULT 0,
    eventRevision INTEGER NOT NULL DEFAULT 1,
    traceId       TEXT,
    source        TEXT NOT NULL DEFAULT 'device',
    adviceRevision INTEGER NOT NULL DEFAULT 0,
    receivedAt    TEXT NOT NULL,
    updatedAt     TEXT NOT NULL
  )
`);

function ensureColumn(table, column, definition) {
  const columns = db.prepare('PRAGMA table_info(' + table + ')').all();
  if (!columns.some((item) => item.name === column)) {
    db.exec('ALTER TABLE ' + table + ' ADD COLUMN ' + column + ' ' + definition);
  }
}

ensureColumn('events', 'messageId', 'TEXT');
ensureColumn('events', 'deviceEpoch', 'TEXT');
ensureColumn('events', 'deviceSeq', 'INTEGER NOT NULL DEFAULT 0');
ensureColumn('events', 'eventRevision', 'INTEGER NOT NULL DEFAULT 1');
ensureColumn('events', 'traceId', 'TEXT');
ensureColumn('events', 'source', "TEXT NOT NULL DEFAULT 'device'");
ensureColumn('events', 'adviceRevision', 'INTEGER NOT NULL DEFAULT 0');

db.exec(
  'CREATE TABLE IF NOT EXISTS ingress_messages (' +
  'messageId TEXT PRIMARY KEY, deviceId TEXT NOT NULL, eventId TEXT NOT NULL, ' +
  'eventRevision INTEGER NOT NULL, receivedAt TEXT NOT NULL);' +
  'CREATE TABLE IF NOT EXISTS commands (' +
  'commandId TEXT PRIMARY KEY, deviceId TEXT NOT NULL, eventId TEXT, ' +
  'commandType TEXT NOT NULL, action TEXT, desiredRevision INTEGER, ' +
  'nonce TEXT NOT NULL, issuedAt TEXT NOT NULL, expiresAt TEXT NOT NULL, ' +
  'status TEXT NOT NULL, resultRevision INTEGER, resultMessageId TEXT, ' +
  'errorCode TEXT, ' +
  'createdAt TEXT NOT NULL, updatedAt TEXT NOT NULL);' +
  'CREATE TABLE IF NOT EXISTS command_results (' +
  'deviceId TEXT NOT NULL, commandId TEXT NOT NULL, messageId TEXT NOT NULL, ' +
  'resultRevision INTEGER NOT NULL, status TEXT NOT NULL, errorCode TEXT, ' +
  'receivedAt TEXT NOT NULL, PRIMARY KEY (deviceId, commandId, messageId));' +
  'CREATE TABLE IF NOT EXISTS audit_log (' +
  'auditId INTEGER PRIMARY KEY AUTOINCREMENT, actor TEXT NOT NULL, ' +
  'deviceId TEXT, commandId TEXT, action TEXT NOT NULL, result TEXT NOT NULL, ' +
  'createdAt TEXT NOT NULL)'
);

ensureColumn('commands', 'alg', "TEXT NOT NULL DEFAULT 'demo.none'");
ensureColumn('commands', 'keyId', "TEXT NOT NULL DEFAULT 'demo'");
ensureColumn('commands', 'signature', "TEXT NOT NULL DEFAULT 'demo-unsigned'");
ensureColumn('commands', 'resultMessageId', 'TEXT');

const upsertEvent = db.prepare(
  'INSERT INTO events (eventId, deviceId, eventType, level, confidence, ' +
  'startedAt, durationSec, localStatus, uploadReason, timeReliable, night, ' +
  'matchedPhrase, personLabel, urgency, repeatCount, summary, advice, ' +
  'messageId, deviceEpoch, deviceSeq, eventRevision, traceId, source, ' +
  'adviceRevision, receivedAt, updatedAt) ' +
  'VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) ' +
  'ON CONFLICT(eventId) DO UPDATE SET ' +
  'level = excluded.level, confidence = excluded.confidence, ' +
  'durationSec = excluded.durationSec, localStatus = excluded.localStatus, ' +
  'uploadReason = excluded.uploadReason, summary = excluded.summary, ' +
  'advice = excluded.advice, repeatCount = excluded.repeatCount, ' +
  'messageId = excluded.messageId, deviceEpoch = excluded.deviceEpoch, ' +
  'deviceSeq = excluded.deviceSeq, eventRevision = excluded.eventRevision, ' +
  'traceId = excluded.traceId, source = excluded.source, ' +
  'adviceRevision = excluded.adviceRevision, updatedAt = excluded.updatedAt ' +
  'WHERE excluded.eventRevision > events.eventRevision OR ' +
  '(excluded.eventRevision = events.eventRevision AND ' +
  'excluded.adviceRevision >= events.adviceRevision)'
);

const selectOne = db.prepare('SELECT * FROM events WHERE eventId = ?');
const selectMany = db.prepare(
  'SELECT * FROM events ORDER BY receivedAt DESC, rowid DESC LIMIT ?');
const selectManyForDevice = db.prepare(
  'SELECT * FROM events WHERE deviceId = ? ' +
  'ORDER BY receivedAt DESC, rowid DESC LIMIT ?');
const selectIngress = db.prepare(
  'SELECT * FROM ingress_messages WHERE messageId = ?');
const insertIngress = db.prepare(
  'INSERT INTO ingress_messages ' +
  '(messageId, deviceId, eventId, eventRevision, receivedAt) VALUES (?, ?, ?, ?, ?)');
const selectCommand = db.prepare(
  'SELECT * FROM commands WHERE commandId = ?');
const selectCommandsForEvent = db.prepare(
  'SELECT * FROM commands WHERE eventId = ? ORDER BY updatedAt DESC LIMIT 8');
const selectCommandResult = db.prepare(
  'SELECT * FROM command_results WHERE deviceId = ? AND commandId = ? AND messageId = ?');
const insertCommand = db.prepare(
  'INSERT INTO commands ' +
  '(commandId, deviceId, eventId, commandType, action, desiredRevision, ' +
  'nonce, issuedAt, expiresAt, status, alg, keyId, signature, ' +
  'createdAt, updatedAt) ' +
  'VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)');
const updateCommandReceipt = db.prepare(
  "UPDATE commands SET status = CASE WHEN status = 'requested' " +
  "THEN 'received' ELSE status END, updatedAt = ? " +
  "WHERE commandId = ? AND deviceId = ?");
const updateCommandResult = db.prepare(
  'UPDATE commands SET status = ?, resultRevision = ?, resultMessageId = ?, ' +
  'errorCode = ?, updatedAt = ? WHERE commandId = ? AND deviceId = ?');
const insertCommandResult = db.prepare(
  'INSERT INTO command_results ' +
  '(deviceId, commandId, messageId, resultRevision, status, errorCode, receivedAt) ' +
  'VALUES (?, ?, ?, ?, ?, ?, ?)');
const insertAudit = db.prepare(
  'INSERT INTO audit_log ' +
  '(actor, deviceId, commandId, action, result, createdAt) ' +
  'VALUES (?, ?, ?, ?, ?, ?)');

// ---------------------------------------------------------------------------
// SSE
// ---------------------------------------------------------------------------

const clients = new Set();

function closeSseClient(client) {
  clients.delete(client);
  if (client.keepalive) clearInterval(client.keepalive);
  try { client.res.end(); } catch { /* ignore */ }
}

function closeClientsForToken(token) {
  for (const client of clients) {
    if (tokenMatches(client.token, token)) closeSseClient(client);
  }
}

function clientSessionValid(client) {
  if (SESSION_TOKEN && tokenMatches(client.token, SESSION_TOKEN)) {
    return !staticSessionRevoked;
  }
  return pairingSessionValid(client.token);
}

function broadcast(type, payload) {
  const eventId = payload?.eventId || payload?.commandId || '';
  const idLine = eventId ? `id: ${eventId}\n` : '';
  const chunk = `${idLine}event: ${type}\ndata: ${JSON.stringify(payload)}\n\n`;
  for (const client of clients) {
    if (!clientSessionValid(client)) {
      closeSseClient(client);
      continue;
    }
    if (client.deviceId && client.deviceId !== payload.deviceId) continue;
    try {
      if (!client.res.write(chunk)) closeSseClient(client);
    } catch {
      closeSseClient(client);
    }
  }
}

// ---------------------------------------------------------------------------
// 校验
// ---------------------------------------------------------------------------

const ENVELOPE_FIELDS = new Set([
  'protocol', 'schema', 'messageType', 'messageId', 'deviceId',
  'deviceEpoch', 'deviceSeq', 'eventId', 'eventRevision', 'sentAt',
  'monotonicMs', 'traceId', 'payload',
]);
const PAYLOAD_FIELDS = new Set([
  'eventId', 'deviceId', 'eventType', 'level', 'confidence', 'startedAt',
  'durationSec', 'localStatus', 'uploadReason', 'timeReliable', 'night',
  'matchedPhrase', 'personLabel', 'urgency', 'repeatCount', 'summary',
  'advice',
]);

function scanPrivacy(value, path, errors, depth = 0, state = { nodes: 0 }) {
  if (!isObject(value) && !Array.isArray(value)) return;
  if (depth > MAX_PRIVACY_DEPTH) {
    errors.push('负载嵌套层级超过限制');
    return;
  }
  state.nodes++;
  if (state.nodes > MAX_PRIVACY_NODES) {
    errors.push('负载字段数量超过限制');
    return;
  }
  for (const [key, child] of Object.entries(value)) {
    const lower = key.toLowerCase();
    if (FORBIDDEN_FIELDS.some((name) => lower === name.toLowerCase()) ||
        /audio|pcm|wav|transcript|rawtext|dialog|recording|speech|voice/.test(lower) ||
        /password|passwd|token|secret|privatekey|credential|certificate/.test(lower)) {
      errors.push('隐私红线：不允许字段 ' + path + key);
    }
    scanPrivacy(child, path + key + '.', errors, depth + 1, state);
  }
}

function validateEventPayload(payload, strict = true) {
  const errors = [];
  if (!isObject(payload)) return ['payload 不是 JSON 对象'];
  scanPrivacy(payload, '', errors);
  if (strict) {
    for (const key of Object.keys(payload)) {
      if (!PAYLOAD_FIELDS.has(key)) errors.push('payload 未知字段: ' + key);
    }
  }

  if (!idOk(payload.eventId) || payload.eventId.length > 40) {
    errors.push('eventId 非法');
  }
  if (!idOk(payload.deviceId) || payload.deviceId.length > 32) {
    errors.push('deviceId 非法');
  }
  if (!EVENT_TYPES.has(payload.eventType)) {
    errors.push('eventType 非法: ' + payload.eventType);
  }
  if (!LEVELS.has(payload.level)) errors.push('level 非法: ' + payload.level);
  if (!STATUSES.has(payload.localStatus)) {
    errors.push('localStatus 非法: ' + payload.localStatus);
  }
  if (!UPLOAD_REASONS.has(payload.uploadReason)) {
    errors.push('uploadReason 非法: ' + payload.uploadReason);
  }
  if (!isFiniteNumber(payload.confidence, 0, 1)) {
    errors.push('confidence 必须是有限的 0~1 数值');
  }
  if (!textOk(payload.startedAt, 64) || Number.isNaN(Date.parse(payload.startedAt))) {
    errors.push('startedAt 非法');
  }
  if (!Number.isInteger(payload.durationSec) ||
      payload.durationSec < 0 || payload.durationSec > 604800) {
    errors.push('durationSec 越界');
  }
  if (typeof payload.timeReliable !== 'boolean' ||
      typeof payload.night !== 'boolean') {
    errors.push('timeReliable/night 必须是布尔值');
  }
  if (!textOk(payload.summary, 128)) errors.push('summary 非法或过长');
  for (const key of ['matchedPhrase', 'personLabel']) {
    if (payload[key] != null && !textOk(payload[key], 64)) {
      errors.push(key + ' 非法或过长');
    }
  }
  if (payload.urgency != null && !isFiniteNumber(payload.urgency, 0, 1)) {
    errors.push('urgency 必须在 0~1');
  }
  if (payload.repeatCount != null &&
      (!Number.isInteger(payload.repeatCount) ||
       payload.repeatCount < 0 || payload.repeatCount > 10000)) {
    errors.push('repeatCount 越界');
  }
  if (payload.advice != null && !textOk(payload.advice, 256)) {
    errors.push('advice 非法或过长');
  }
  return errors;
}

function normalizeIngress(body, targetDeviceId = null) {
  const errors = [];
  if (!isObject(body)) return { errors: ['负载不是 JSON 对象'] };
  scanPrivacy(body, '', errors);

  if (body.schema === ENVELOPE_SCHEMA) {
    for (const key of Object.keys(body)) {
      if (!ENVELOPE_FIELDS.has(key)) errors.push('envelope 未知字段: ' + key);
    }
    if (body.protocol !== 'velaguard' || body.messageType !== 'event.upsert') {
      errors.push('protocol/messageType 不支持');
    }
    if (!idOk(body.messageId) || body.messageId.length > 48) {
      errors.push('messageId 非法');
    }
    if (!idOk(body.deviceEpoch) || body.deviceEpoch.length > 40) {
      errors.push('deviceEpoch 非法');
    }
    if (!Number.isSafeInteger(body.deviceSeq) || body.deviceSeq <= 0) {
      errors.push('deviceSeq 非法');
    }
    if (!Number.isInteger(body.eventRevision) || body.eventRevision <= 0) {
      errors.push('eventRevision 非法');
    }
    if (!textOk(body.sentAt, 64) || Number.isNaN(Date.parse(body.sentAt))) {
      errors.push('sentAt 非法');
    }
    if (!Number.isSafeInteger(body.monotonicMs) || body.monotonicMs < 0) {
      errors.push('monotonicMs 非法');
    }
    if (!idOk(body.traceId) || body.traceId.length > 48) {
      errors.push('traceId 非法');
    }
    const payloadErrors = validateEventPayload(body.payload, true);
    errors.push(...payloadErrors);
    if (body.deviceId !== body.payload?.deviceId ||
        body.eventId !== body.payload?.eventId ||
        body.deviceId !== targetDeviceId && targetDeviceId != null) {
      errors.push('envelope 与 payload/URL 身份不一致');
    }
    if (body.payload?.eventRevision != null &&
        body.payload.eventRevision !== body.eventRevision) {
      errors.push('eventRevision 不一致');
    }
    return {
      errors,
      envelope: body,
      payload: body.payload,
      metadata: {
        messageId: body.messageId,
        deviceEpoch: body.deviceEpoch,
        deviceSeq: body.deviceSeq,
        eventRevision: body.eventRevision,
        traceId: body.traceId,
        source: 'device',
      },
    };
  }

  if (!DEMO_PROFILE) {
    errors.push('生产 profile 拒绝未版本化事件');
    return { errors };
  }

  const legacyErrors = validateEventPayload(body, false);
  errors.push(...legacyErrors);
  if (errors.length) return { errors };
  const revision = Number.isInteger(body.eventRevision) &&
                   body.eventRevision > 0 ? body.eventRevision : 1;
  const legacyId = 'legacy_' + body.deviceId + '_' + body.eventId + '_' + revision;
  return {
    errors,
    envelope: null,
    payload: body,
    metadata: {
      messageId: legacyId.slice(0, 48),
      deviceEpoch: 'legacy',
      deviceSeq: 0,
      eventRevision: revision,
      traceId: legacyId.slice(0, 48),
      source: 'legacy-demo',
    },
  };
}

function rowV1(payload, metadata, now, existing) {
  return [
    payload.eventId,
    payload.deviceId,
    payload.eventType,
    payload.level,
    payload.confidence,
    payload.startedAt,
    payload.durationSec,
    payload.localStatus,
    payload.uploadReason,
    payload.timeReliable ? 1 : 0,
    payload.night ? 1 : 0,
    payload.matchedPhrase ?? null,
    payload.personLabel ?? null,
    payload.urgency ?? null,
    payload.repeatCount ?? 0,
    payload.summary,
    payload.advice ?? '',
    metadata.messageId,
    metadata.deviceEpoch,
    metadata.deviceSeq,
    metadata.eventRevision,
    metadata.traceId,
    metadata.source,
    0,
    existing ? existing.receivedAt : now,
    now,
  ];
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

const DEVICE_TOKEN = process.env.VELAGUARD_DEVICE_TOKEN ||
  (DEMO_PROFILE ? 'demo-token' : '');
const ALLOWED_ORIGIN = process.env.VELAGUARD_CORS_ORIGIN || '';
const DEMO_ALLOW_ANONYMOUS_DEVICE = DEMO_PROFILE &&
  process.env.VELAGUARD_ALLOW_ANONYMOUS_DEVICE === 'true';

function cookieValue(req, name) {
  const header = String(req.headers.cookie || '');
  for (const part of header.split(';')) {
    const separator = part.indexOf('=');
    if (separator < 0 || part.slice(0, separator).trim() !== name) continue;
    const value = part.slice(separator + 1).trim();
    try {
      return decodeURIComponent(value);
    } catch {
      return '';
    }
  }
  return '';
}

function isTrustedProxy(req) {
  const remoteAddress = String(req.socket?.remoteAddress || '');
  if (TRUSTED_PROXY_ADDRESSES.has(remoteAddress)) return true;
  return remoteAddress.startsWith('::ffff:') &&
    TRUSTED_PROXY_ADDRESSES.has(remoteAddress.slice('::ffff:'.length));
}

function requestIsSecure(req) {
  if (req.socket?.encrypted) return true;
  if (!PRODUCTION_TLS_TERMINATED || !isTrustedProxy(req)) return false;
  return String(req.headers['x-forwarded-proto'] || '').trim().toLowerCase() ===
    'https';
}

function transportHeaders(req) {
  return !DEMO_PROFILE && requestIsSecure(req)
    ? { 'Strict-Transport-Security': 'max-age=31536000' }
    : {};
}

function sessionCookie(req, token, maxAge) {
  return `${FAMILY_COOKIE}=${token ? encodeURIComponent(token) : ''}; ` +
    `HttpOnly; SameSite=Strict; Path=/; Max-Age=${maxAge}` +
    (requestIsSecure(req) ? '; Secure' : '');
}

function tokenFromRequest(req) {
  const auth = req.headers.authorization || '';
  if (auth.startsWith('Bearer ')) return auth.slice(7);
  const deviceToken = String(req.headers['x-velaguard-device-token'] || '');
  if (deviceToken) return deviceToken;
  return cookieValue(req, FAMILY_COOKIE);
}

function tokenMatches(actual, expected) {
  if (!actual || !expected || actual.length !== expected.length) return false;
  let diff = 0;
  for (let i = 0; i < expected.length; i++) diff |= actual.charCodeAt(i) ^ expected.charCodeAt(i);
  return diff === 0;
}

function issuePairingSession(deviceId) {
  const now = Date.now();
  for (const [token, session] of pairingSessions) {
    if (session.expiresAt <= now) revokePairingSession(token);
  }
  while (pairingSessions.size >= MAX_PAIRING_SESSIONS) {
    const oldest = pairingSessions.keys().next().value;
    if (oldest == null) break;
    revokePairingSession(oldest);
  }

  const token = crypto.randomBytes(24).toString('base64url');
  pairingSessions.set(token, {
    deviceId,
    expiresAt: now + PAIRING_SESSION_TTL_MS,
  });
  return token;
}

function pairingSessionValid(token) {
  const session = pairingSessions.get(token);
  if (!session || session.expiresAt <= Date.now()) {
    if (session) revokePairingSession(token);
    return false;
  }
  return true;
}

function revokePairingSession(token) {
  const revoked = pairingSessions.delete(token);
  if (revoked) closeClientsForToken(token);
  return revoked;
}

function requestClientAddress(req) {
  if (PRODUCTION_TLS_TERMINATED && isTrustedProxy(req)) {
    const forwarded = String(req.headers['x-forwarded-for'] || '')
      .split(',').map((value) => value.trim()).filter(Boolean);
    if (forwarded.length) return forwarded[0];
  }
  return req.socket.remoteAddress || 'unknown';
}

function pairingFailureKeys(req, deviceId = '') {
  const address = requestClientAddress(req);
  return {
    address,
    device: deviceId ? `${address}|${deviceId}` : '',
  };
}

function pairingFailureEntries(req, deviceId) {
  const keys = pairingFailureKeys(req, deviceId);
  const entries = [{ map: pairingAddressFailures, key: keys.address }];
  if (keys.device) {
    entries.push({ map: pairingDeviceFailures, key: keys.device });
  }
  return entries;
}

function pairingAttemptAllowed(req, deviceId) {
  const now = Date.now();
  return pairingFailureEntries(req, deviceId).every(({ map, key }) => {
    const entry = map.get(key);
    if (!entry || entry.resetAt <= now) {
      if (entry) map.delete(key);
      return true;
    }
    return entry.count < MAX_PAIRING_FAILURES;
  });
}

function recordPairingFailure(req, deviceId) {
  const now = Date.now();
  for (const { map, key } of pairingFailureEntries(req, deviceId)) {
    const entry = map.get(key);
    if (!entry || entry.resetAt <= now) {
      while (map.size >= MAX_PAIRING_FAILURE_BUCKETS) {
        const oldest = map.keys().next().value;
        if (oldest == null) break;
        map.delete(oldest);
      }
      map.set(key, {
        count: 1,
        resetAt: now + PAIRING_FAILURE_WINDOW_MS,
      });
    } else {
      entry.count++;
    }
  }
}

function clearPairingFailures(req, deviceId) {
  for (const { map, key } of pairingFailureEntries(req, deviceId)) {
    map.delete(key);
  }
}

function sseAdmissionAllowed(req, token) {
  if (clients.size >= MAX_SSE_CLIENTS) return false;

  const address = requestClientAddress(req);
  let tokenCount = 0;
  let addressCount = 0;
  for (const client of clients) {
    if (tokenMatches(client.token, token)) tokenCount++;
    if (client.address === address) addressCount++;
  }
  return tokenCount < MAX_SSE_CLIENTS_PER_TOKEN &&
    addressCount < MAX_SSE_CLIENTS_PER_ADDRESS;
}

function authorizedDevice(req, deviceId) {
  if (!idOk(deviceId)) return false;
  if (PAIRING_DEVICE_ID && deviceId !== PAIRING_DEVICE_ID) {
    return false;
  }
  const token = tokenFromRequest(req);
  if (DEVICE_TOKEN && tokenMatches(token, DEVICE_TOKEN)) return true;
  if (DEMO_ALLOW_ANONYMOUS_DEVICE && !token) {
    console.warn('[console] 演示模式允许匿名设备接入，仅限局域网演示');
    return true;
  }
  return false;
}

function authorizedSession(req) {
  const token = tokenFromRequest(req);
  if (SESSION_TOKEN && tokenMatches(token, SESSION_TOKEN) &&
      !staticSessionRevoked) {
    return DEMO_PROFILE || Boolean(EXPECTED_DEVICE_ID);
  }
  if (pairingSessionValid(token)) return true;
  return false;
}

function sessionDeviceId(req) {
  const token = tokenFromRequest(req);
  const session = pairingSessions.get(token);
  if (session && session.expiresAt > Date.now()) return session.deviceId;
  if (SESSION_TOKEN && tokenMatches(token, SESSION_TOKEN) &&
      !staticSessionRevoked) {
    return EXPECTED_DEVICE_ID || null;
  }
  return null;
}

function authorizedSessionForDevice(req, deviceId) {
  if (!authorizedSession(req)) return false;
  const token = tokenFromRequest(req);
  const pairedDeviceId = sessionDeviceId(req);
  if (pairedDeviceId) return pairedDeviceId === deviceId;
  if (SESSION_TOKEN && tokenMatches(token, SESSION_TOKEN)) {
    return Boolean(EXPECTED_DEVICE_ID) && EXPECTED_DEVICE_ID === deviceId;
  }
  return true;
}

function corsHeaders(req) {
  const origin = req.headers.origin;
  if (origin && ALLOWED_ORIGIN && origin === ALLOWED_ORIGIN) {
    return {
      'Access-Control-Allow-Origin': origin,
      'Access-Control-Allow-Credentials': 'true',
      Vary: 'Origin',
    };
  }
  return { Vary: 'Origin' };
}

function csrfAllowed(req) {
  if (!['POST', 'PATCH', 'DELETE'].includes(req.method)) return true;
  const hasCookie = Boolean(cookieValue(req, FAMILY_COOKIE));
  const hasExplicitToken = Boolean(req.headers.authorization) ||
    Boolean(req.headers['x-velaguard-device-token']);
  if (!hasCookie || hasExplicitToken) return true;

  const origin = String(req.headers.origin || '');
  if (!origin || origin === 'null') return false;
  const expected = `${requestIsSecure(req) ? 'https' : 'http'}://` +
    `${req.headers.host || ''}`;
  return origin === expected || (ALLOWED_ORIGIN && origin === ALLOWED_ORIGIN);
}

function send(res, code, body, headers = {}) {
  const data = typeof body === 'string' ? body : JSON.stringify(body);
  res.writeHead(code, {
    'Content-Type': typeof body === 'string'
      ? 'text/plain; charset=utf-8' : 'application/json; charset=utf-8',
    'Access-Control-Allow-Methods': 'DELETE,GET,POST,PATCH,OPTIONS',
    'Access-Control-Allow-Headers': 'Content-Type, Authorization, X-VelaGuard-Device-Token, Idempotency-Key',
    ...corsHeaders(res.req || { headers: {} }),
    ...transportHeaders(res.req || { headers: {}, socket: {} }),
    ...headers,
  });
  res.end(data);
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    let raw = '';
    let failed = false;
    req.on('data', (c) => {
      if (failed) return;
      raw += c;
      if (raw.length > 64 * 1024) {
        failed = true;
        reject(new Error('负载过大'));
      }
    });
    req.on('end', () => { if (!failed) resolve(raw); });
    req.on('error', (error) => { if (!failed) reject(error); });
  });
}

function serveStatic(res, urlPath) {
  const file = urlPath === '/' ? 'index.html' : urlPath.replace(/^\/+/, '');
  const publicRoot = path.resolve(__dirname, 'public');
  const full = path.resolve(publicRoot, file);
  let realRoot;
  let realFull;
  try {
    realRoot = fs.realpathSync(publicRoot);
    realFull = fs.realpathSync(full);
  } catch {
    return send(res, 404, '未找到');
  }
  const relative = path.relative(realRoot, realFull);
  if (relative.startsWith('..' + path.sep) || path.isAbsolute(relative)) {
    return send(res, 403, '禁止访问');
  }
  fs.readFile(realFull, (err, data) => {
    if (err) return send(res, 404, '未找到');
    const ext = path.extname(realFull);
    const mime = { '.html': 'text/html', '.js': 'text/javascript',
      '.css': 'text/css', '.json': 'application/json' }[ext] || 'text/plain';
    res.writeHead(200, {
      'Content-Type': `${mime}; charset=utf-8`,
      ...transportHeaders(res.req || { headers: {}, socket: {} }),
    });
    res.end(data);
  });
}

function eventIngressAck(metadata, accepted, duplicate, errorCode = null) {
  return {
    schema: ACK_SCHEMA,
    messageType: 'eventIngressAck',
    messageId: metadata.messageId,
    deviceId: metadata.deviceId,
    eventId: metadata.eventId,
    eventRevision: metadata.eventRevision,
    accepted,
    duplicate,
    serverTime: nowIso(),
    errorCode,
    traceId: metadata.traceId || metadata.messageId,
  };
}

function commandResultAck(deviceId, commandId, messageId, resultRevision,
                          accepted, duplicate, errorCode = null) {
  return {
    schema: ACK_SCHEMA,
    messageType: 'commandResultIngressAck',
    messageId,
    deviceId,
    commandId,
    resultRevision,
    accepted,
    duplicate,
    serverTime: nowIso(),
    errorCode,
  };
}

function commandReceiptAck(deviceId, commandId, messageId, duplicate,
                           errorCode = null) {
  return {
    schema: ACK_SCHEMA,
    messageType: 'commandReceiptAck',
    messageId,
    deviceId,
    commandId,
    accepted: true,
    duplicate,
    serverTime: nowIso(),
    errorCode,
  };
}

function commandCanonical(command) {
  const args = {};
  if (command.eventId) args.eventId = command.eventId;
  if (command.action) args.action = command.action;
  return JSON.stringify({
    schema: 'command.v1',
    commandId: command.commandId,
    targetDeviceId: command.deviceId,
    commandType: command.commandType,
    issuedAt: command.issuedAt,
    expiresAt: command.expiresAt,
    desiredRevision: command.desiredRevision,
    nonce: command.nonce,
    args,
    alg: command.alg,
    keyId: command.keyId,
  });
}

function commandSignature(command) {
  if (command.alg === 'demo.none') return 'demo-unsigned';
  if (!COMMAND_SIGNING_KEY || !COMMAND_KEY_ID) return null;
  return crypto.createHmac('sha256', COMMAND_SIGNING_KEY)
    .update(commandCanonical(command))
    .digest('base64url');
}

function commandView(command) {
  if (!command) return null;
  return {
    commandId: command.commandId,
    schema: 'command.v1',
    deviceId: command.deviceId,
    targetDeviceId: command.deviceId,
    eventId: command.eventId,
    commandType: command.commandType,
    action: command.action,
    desiredRevision: command.desiredRevision,
    issuedAt: command.issuedAt,
    expiresAt: command.expiresAt,
    status: command.status,
    nonce: command.nonce,
    args: { eventId: command.eventId, action: command.action },
    alg: command.alg || (DEMO_PROFILE ? 'demo.none' : 'hmac-sha256'),
    keyId: command.keyId || (DEMO_PROFILE ? 'demo' : ''),
    signature: command.signature || (DEMO_PROFILE ? 'demo-unsigned' : ''),
    resultRevision: command.resultRevision,
    resultMessageId: command.resultMessageId,
    errorCode: command.errorCode,
    createdAt: command.createdAt,
    updatedAt: command.updatedAt,
  };
}

function createCommand(event, body, actor) {
  if (!isObject(body)) {
    return { error: '命令负载必须是 JSON 对象', code: 400 };
  }

  const now = nowIso();
  const commandId = body.commandId ||
    String(body.idempotencyKey || '') ||
    'cmd_' + Date.now().toString(36) + '_' +
    Math.random().toString(36).slice(2, 10);
  const nonce = body.nonce || 'nonce_' + Date.now().toString(36);
  const desiredRevision = body.desiredRevision == null
    ? event.eventRevision : body.desiredRevision;
  const expiresAtRaw = body.expiresAt ||
    new Date(Date.now() + 120000).toISOString();
  const expiresAtMs = typeof expiresAtRaw === 'string'
    ? Date.parse(expiresAtRaw) : NaN;
  const expiresAt = Number.isNaN(expiresAtMs)
    ? expiresAtRaw : new Date(expiresAtMs).toISOString();
  const action = body.action;

  if (!idOk(commandId) || commandId.length > 64 ||
      !idOk(nonce) || nonce.length > 96 ||
      !ACTION_TYPES.has(action) ||
      !Number.isInteger(desiredRevision) || desiredRevision <= 0 ||
      desiredRevision !== event.eventRevision ||
      Number.isNaN(expiresAtMs) || expiresAtMs <= Date.now() ||
      expiresAtMs > Date.now() + MAX_COMMAND_TTL_MS) {
    return { error: '命令字段、修订号或有效期非法', code: 400 };
  }
  if (action === 'snooze' && event.level === 'emergency') {
    return { error: '紧急事件禁止远程 snooze', code: 409 };
  }

  const existing = selectCommand.get(commandId);
  if (existing) return { command: existing, duplicate: true };

  const command = {
    commandId,
    deviceId: event.deviceId,
    eventId: event.eventId,
    commandType: 'event.action',
    action,
    desiredRevision,
    nonce,
    issuedAt: now,
    expiresAt,
    status: 'requested',
    resultRevision: null,
    resultMessageId: null,
    errorCode: null,
    createdAt: now,
    updatedAt: now,
    alg: COMMAND_ALG,
    keyId: COMMAND_KEY_ID,
    signature: null,
  };

  command.signature = commandSignature(command);
  if (!command.signature) {
    return { error: '生产命令签名器未配置', code: 503 };
  }

  db.exec('BEGIN');
  try {
    insertCommand.run(
      command.commandId, command.deviceId, command.eventId,
      command.commandType, command.action, command.desiredRevision,
      command.nonce, command.issuedAt, command.expiresAt, command.status,
      command.alg, command.keyId, command.signature,
      command.createdAt, command.updatedAt);
    insertAudit.run(actor, command.deviceId, command.commandId,
                    'event.action.' + action, 'requested', now);
    db.exec('COMMIT');
  } catch (error) {
    db.exec('ROLLBACK');
    return { error: '命令持久化失败: ' + error.message, code: 503 };
  }
  return { command, duplicate: false };
}

async function handleRequest(req, res) {
  const secure = requestIsSecure(req);
  const scheme = secure ? 'https' : 'http';
  const url = new URL(req.url, `${scheme}://${req.headers.host || 'localhost'}`);
  const { pathname } = url;

  if (!DEMO_PROFILE && !secure) {
    return send(res, 421, {
      ok: false,
      errorCode: 'https_required',
      error: '生产控制台只接受 HTTPS 请求',
    });
  }

  if (!csrfAllowed(req)) {
    return send(res, 403, {
      ok: false,
      errorCode: 'csrf_origin_required',
      error: 'Cookie 会话的写操作必须来自受信 Origin',
    });
  }

  if (req.method === 'OPTIONS') return send(res, 204, '');

  // --- SSE ---------------------------------------------------------------
  if (req.method === 'GET' && pathname === '/stream') {
    if (!authorizedSession(req)) {
      return send(res, 401, { ok: false, error: '需要家属会话授权' });
    }
    const token = tokenFromRequest(req);
    if (!sseAdmissionAllowed(req, token)) {
      return send(res, 429, {
        ok: false,
        errorCode: 'sse_connection_limit',
        error: '实时连接数已达到上限，请关闭重复页面后重试',
      }, {
        'Cache-Control': 'no-store',
        'Retry-After': '60',
      });
    }
    res.writeHead(200, {
      'Content-Type': 'text/event-stream; charset=utf-8',
      'Cache-Control': 'no-cache',
      'Referrer-Policy': 'no-referrer',
      Connection: 'keep-alive',
      ...corsHeaders(req),
      ...transportHeaders(req),
    });
    if (!res.write(`event: hello\ndata: ${JSON.stringify({ ok: true })}\n\n`)) {
      res.end();
      return undefined;
    }
    const client = {
      res,
      deviceId: sessionDeviceId(req),
      token,
      address: requestClientAddress(req),
    };
    clients.add(client);
    client.keepalive = setInterval(() => {
      if (!clientSessionValid(client)) {
        closeSseClient(client);
        return;
      }
      try {
        if (!res.write(': keepalive\n\n')) closeSseClient(client);
      } catch {
        closeSseClient(client);
      }
    }, 15000);
    req.on('close', () => { closeSseClient(client); });
    return undefined;
  }

  if (req.method === 'GET' && pathname === '/health') {
    const health = {
      ok: true,
      profile: DEMO_PROFILE ? 'demo' : 'production',
      warning: DEMO_PROFILE ? '演示 profile：请勿暴露到公网' : undefined,
    };
    if (DEMO_PROFILE) {
      health.clients = clients.size;
      health.events = db.prepare('SELECT COUNT(*) AS n FROM events').get().n;
    }
    return send(res, 200, health);
  }

  if (pathname === '/pairing' && req.method === 'GET') {
    const token = tokenFromRequest(req);
    const paired = pairingSessionValid(token);
    return send(res, 200, {
      ok: true,
      profile: DEMO_PROFILE ? 'demo' : 'production',
      deviceId: PAIRING_DEVICE_HINT || null,
      keyRequired: !DEMO_PROFILE || Boolean(PAIRING_KEY),
      paired,
      pairedDeviceId: paired ? sessionDeviceId(req) : null,
      expiresInSec: PAIRING_SESSION_TTL_MS / 1000,
    }, {
      'Cache-Control': 'no-store',
    });
  }

  if (pathname === '/pairing' && req.method === 'DELETE') {
    const token = tokenFromRequest(req);
    let revoked = revokePairingSession(token);
    if (SESSION_TOKEN && tokenMatches(token, SESSION_TOKEN)) {
      staticSessionRevoked = true;
      closeClientsForToken(token);
      revoked = true;
    }
    return send(res, 200, {
      ok: true,
      revoked,
    }, {
      'Cache-Control': 'no-store',
      'Set-Cookie': sessionCookie(req, '', 0),
    });
  }

  if (pathname === '/pairing' && req.method === 'POST') {
    let body;
    try {
      body = JSON.parse(await readBody(req));
    } catch {
      return send(res, 400, { ok: false, errorCode: 'invalid_json' });
    }

    const deviceId = isObject(body) && idOk(body.deviceId) &&
      body.deviceId.length <= MAX_PAIRING_DEVICE_ID_LENGTH ? body.deviceId : '';
    const key = isObject(body) && typeof body.key === 'string' ? body.key : '';
    if (!pairingAttemptAllowed(req, deviceId)) {
      return send(res, 429, {
        ok: false,
        errorCode: 'pairing_rate_limited',
      }, {
        'Cache-Control': 'no-store',
        'Retry-After': String(PAIRING_FAILURE_WINDOW_MS / 1000),
      });
    }
    if (!deviceId || !PAIRING_KEY || !tokenMatches(key, PAIRING_KEY) ||
        (PAIRING_DEVICE_ID && deviceId !== PAIRING_DEVICE_ID)) {
      recordPairingFailure(req, deviceId);
      return send(res, 401, {
        ok: false,
        errorCode: 'pairing_rejected',
      }, { 'Cache-Control': 'no-store' });
    }

    clearPairingFailures(req, deviceId);
    const sessionToken = issuePairingSession(deviceId);
    return send(res, 200, {
      ok: true,
      paired: true,
      deviceId,
      expiresInSec: PAIRING_SESSION_TTL_MS / 1000,
    }, {
      'Cache-Control': 'no-store',
      'Set-Cookie': sessionCookie(req, sessionToken,
                                  PAIRING_SESSION_TTL_MS / 1000),
    });
  }

  if (req.method === 'POST' &&
      (pathname === '/events' ||
       /^\/v1\/devices\/[\w.-]+\/events$/.test(pathname))) {
    const targetMatch = pathname.match(/^\/v1\/devices\/([\w.-]+)\/events$/);
    let body;
    try {
      body = JSON.parse(await readBody(req));
    } catch {
      return send(res, 400, { ok: false, errorCode: 'invalid_json',
        errors: ['JSON 解析失败'] });
    }

    const normalized = normalizeIngress(body, targetMatch?.[1] || null);
    if (normalized.errors.length) {
      console.warn('[console] 拒绝非法事件:', normalized.errors.join('; '));
      return send(res, 400, { ok: false, errorCode: 'invalid_event',
        errors: normalized.errors });
    }

    const payload = normalized.payload;
    const metadata = {
      ...normalized.metadata,
      deviceId: payload.deviceId,
      eventId: payload.eventId,
    };
    if (!authorizedDevice(req, payload.deviceId)) {
      return send(res, 401, { ok: false, errorCode: 'device_unauthorized' });
    }

    const previousMessage = selectIngress.get(metadata.messageId);
    if (previousMessage) {
      if (previousMessage.deviceId !== metadata.deviceId ||
          previousMessage.eventId !== metadata.eventId ||
          previousMessage.eventRevision !== metadata.eventRevision) {
        return send(res, 409, { ok: false, errorCode: 'message_identity_conflict' });
      }
      const saved = selectOne.get(metadata.eventId);
      return send(res, 200, {
        ok: true,
        duplicated: true,
        ack: eventIngressAck(metadata, true, true),
        event: saved,
      });
    }

    const now = nowIso();
    const existing = selectOne.get(payload.eventId);
    if (existing && existing.deviceId !== payload.deviceId) {
      return send(res, 409, { ok: false, errorCode: 'event_device_conflict' });
    }
    const duplicate = Boolean(existing &&
      existing.eventRevision >= metadata.eventRevision);

    db.exec('BEGIN');
    try {
      upsertEvent.run(...rowV1(payload, metadata, now, existing));
      insertIngress.run(metadata.messageId, metadata.deviceId,
                        metadata.eventId, metadata.eventRevision, now);
      db.exec('COMMIT');
    } catch (error) {
      db.exec('ROLLBACK');
      console.error('[console] 事件持久化失败:', error.message);
      return send(res, 503, { ok: false, errorCode: 'store_unavailable' });
    }

    const saved = selectOne.get(payload.eventId);
    if (!duplicate) {
      broadcast(existing ? 'update' : 'event', saved);
    }
    console.log('[console] ' + (duplicate ? '重复/旧修订' :
      (existing ? '更新' : '新增')) + ' ' + payload.eventId + ' ' +
      payload.eventType + '/' + payload.level + '/' + saved.localStatus);

    return send(res, 200, {
      ok: true,
      duplicated: duplicate,
      ack: eventIngressAck(metadata, true, duplicate),
      event: saved,
    });
  }

  // --- 查询列表 -----------------------------------------------------------
  if (req.method === 'GET' && pathname === '/events') {
    if (!authorizedSession(req)) {
      return send(res, 401, { ok: false, error: '需要家属会话授权' });
    }
    const rawLimit = url.searchParams.get('limit');
    const limit = rawLimit == null ? 100 : Number(rawLimit);
    if (!Number.isInteger(limit) || limit < 1 || limit > 500) {
      return send(res, 400, {
        ok: false,
        errorCode: 'invalid_limit',
        error: 'limit 必须是 1 到 500 的整数',
      }, { 'Cache-Control': 'no-store' });
    }
    const deviceId = sessionDeviceId(req);
    return send(res, 200, {
      ok: true,
      events: deviceId ? selectManyForDevice.all(deviceId, limit) : selectMany.all(limit),
    }, { 'Cache-Control': 'no-store' });
  }

  const commandPath = pathname.match(/^\/events\/([\w.-]+)\/commands$/);
  if ((req.method === 'GET' || req.method === 'POST') && commandPath) {
    if (!authorizedSession(req)) {
      return send(res, 401, { ok: false, error: '需要家属会话授权' });
    }
    const event = selectOne.get(commandPath[1]);
    if (!event) return send(res, 404, { ok: false, error: '未找到事件' });
    if (!authorizedSessionForDevice(req, event.deviceId)) {
      return send(res, 403, { ok: false, error: '会话未授权此设备' });
    }
    if (req.method === 'GET') {
      return send(res, 200, {
        ok: true,
        commands: selectCommandsForEvent.all(event.eventId).map(commandView),
      }, { 'Cache-Control': 'no-store' });
    }
    let body;
    try {
      body = JSON.parse(await readBody(req));
    } catch {
      return send(res, 400, { ok: false, error: 'JSON 解析失败' });
    }
    const idempotencyKey = req.headers['idempotency-key'];
    if (idempotencyKey && isObject(body) && body.idempotencyKey == null) {
      body = { ...body, idempotencyKey: String(idempotencyKey) };
    }
    const result = createCommand(event, body, 'family-session');
    if (result.error) {
      return send(res, result.code, { ok: false, error: result.error });
    }
    const command = result.command;
    if (!result.duplicate) {
      broadcast('command', commandView(command));
    }
    return send(res, result.duplicate ? 200 : 202, {
      ok: true,
      duplicated: result.duplicate,
      status: command.status,
      requiresLocalConfirmation:
        event.level === 'emergency' && body.action !== 'snooze',
      command: commandView(command),
      event,
    });
  }

  const deviceCommandPath =
    pathname.match(/^\/v1\/devices\/([\w.-]+)\/commands$/);
  if (req.method === 'GET' && deviceCommandPath) {
    const deviceId = deviceCommandPath[1];
    if (!authorizedDevice(req, deviceId)) {
      return send(res, 401, { ok: false, errorCode: 'device_unauthorized' });
    }
    const commands = db.prepare(
      'SELECT * FROM commands WHERE deviceId = ? AND status = ? ' +
      'AND julianday(expiresAt) > julianday(?) ' +
      'ORDER BY createdAt ASC LIMIT 32')
      .all(deviceId, 'requested', nowIso());
    return send(res, 200, { ok: true, commands: commands.map(commandView) });
  }

  const commandReceiptPath =
    pathname.match(/^\/v1\/devices\/([\w.-]+)\/command-receipts$/);
  if (req.method === 'POST' && commandReceiptPath) {
    const deviceId = commandReceiptPath[1];
    if (!authorizedDevice(req, deviceId)) {
      return send(res, 401, { ok: false, errorCode: 'device_unauthorized' });
    }
    let body;
    try {
      body = JSON.parse(await readBody(req));
    } catch {
      return send(res, 400, { ok: false, errorCode: 'invalid_json' });
    }
    if (!isObject(body) || !idOk(body.commandId)) {
      return send(res, 400, { ok: false, errorCode: 'invalid_command_receipt' });
    }
    const command = selectCommand.get(body.commandId);
    if (!command || command.deviceId !== deviceId) {
      return send(res, 404, { ok: false, errorCode: 'command_not_found' });
    }
    const duplicate = command.status !== 'requested';
    if (!duplicate) {
      const now = nowIso();
      db.exec('BEGIN');
      try {
        updateCommandReceipt.run(now, body.commandId, deviceId);
        insertAudit.run('device', deviceId, body.commandId,
                        'command.receipt', 'received', now);
        db.exec('COMMIT');
      } catch (error) {
        db.exec('ROLLBACK');
        return send(res, 503, { ok: false, errorCode: 'store_unavailable' });
      }
    }
    const messageId = idOk(body.messageId) ? body.messageId : body.commandId;
    return send(res, 200, {
      ok: true,
      ack: commandReceiptAck(deviceId, body.commandId, messageId, duplicate),
      command: commandView(selectCommand.get(body.commandId)),
    });
  }

  const commandResultPath =
    pathname.match(/^\/v1\/devices\/([\w.-]+)\/command-results$/);
  if (req.method === 'POST' && commandResultPath) {
    const deviceId = commandResultPath[1];
    if (!authorizedDevice(req, deviceId)) {
      return send(res, 401, { ok: false, errorCode: 'device_unauthorized' });
    }
    let body;
    try {
      body = JSON.parse(await readBody(req));
    } catch {
      return send(res, 400, { ok: false, errorCode: 'invalid_json' });
    }
    if (!isObject(body)) {
      return send(res, 400, { ok: false, errorCode: 'invalid_command_result' });
    }
    const allowedResults = new Set([
      'received', 'applied', 'rejected', 'requires_local_confirmation',
      'expired', 'stale', 'duplicate', 'failed',
    ]);
    if (!idOk(body.commandId) || !allowedResults.has(body.status) ||
        !idOk(body.messageId) || body.messageId.length > 64 ||
        !Number.isSafeInteger(body.resultRevision) ||
        body.resultRevision <= 0 ||
        body.resultRevision > MAX_COMMAND_RESULT_REVISION ||
        (body.errorCode != null && !textOk(body.errorCode, 64))) {
      return send(res, 400, { ok: false, errorCode: 'invalid_command_result' });
    }
    const command = selectCommand.get(body.commandId);
    if (!command || command.deviceId !== deviceId) {
      return send(res, 404, { ok: false, errorCode: 'command_not_found' });
    }
    const messageId = body.messageId;
    const resultRevision = body.resultRevision;
    const priorResult = selectCommandResult.get(deviceId, body.commandId,
                                                messageId);
    if (priorResult) {
      if (priorResult.resultRevision !== resultRevision ||
          priorResult.status !== body.status ||
          priorResult.errorCode !== (body.errorCode || null)) {
        return send(res, 409, {
          ok: false,
          errorCode: 'command_result_message_conflict',
          command: commandView(command),
        });
      }
      return send(res, 200, {
        ok: true,
        ack: commandResultAck(deviceId, body.commandId, messageId,
                              resultRevision, true, true),
        command: commandView(command),
      });
    }
    if (command.resultRevision != null &&
        resultRevision < command.resultRevision) {
      return send(res, 409, {
        ok: false,
        errorCode: 'stale_command_result',
        command: commandView(command),
      });
    }
    const duplicate = command.resultRevision === resultRevision &&
      command.resultMessageId === messageId &&
      command.status === body.status &&
      command.errorCode === (body.errorCode || null);
    if (command.resultRevision === resultRevision && !duplicate) {
      return send(res, 409, {
        ok: false,
        errorCode: 'command_result_conflict',
        command: commandView(command),
      });
    }
    if (!duplicate) {
      const now = nowIso();
      db.exec('BEGIN');
      try {
        insertCommandResult.run(deviceId, body.commandId, messageId,
                                resultRevision, body.status,
                                body.errorCode || null, now);
        updateCommandResult.run(body.status, resultRevision, messageId,
                                body.errorCode || null, now,
                                body.commandId, deviceId);
        insertAudit.run('device', deviceId, body.commandId,
                        'command.result', body.status, now);
        db.exec('COMMIT');
      } catch (error) {
        db.exec('ROLLBACK');
        return send(res, 503, { ok: false, errorCode: 'store_unavailable' });
      }
    }
    const savedCommand = selectCommand.get(body.commandId);
    broadcast('command-result', commandView(savedCommand));
    return send(res, 200, {
      ok: true,
      ack: commandResultAck(deviceId, body.commandId, messageId,
                            resultRevision, true, duplicate),
      command: commandView(savedCommand),
    });
  }

  const legacyPatch = pathname.match(/^\/events\/([\w.-]+)$/);
  if (req.method === 'PATCH' && legacyPatch) {
    if (!authorizedSession(req)) {
      return send(res, 401, { ok: false, error: '需要家属会话授权' });
    }
    const event = selectOne.get(legacyPatch[1]);
    if (!event) return send(res, 404, { ok: false, error: '未找到事件' });
    if (!authorizedSessionForDevice(req, event.deviceId)) {
      return send(res, 403, { ok: false, error: '会话未授权此设备' });
    }
    let body;
    try {
      body = JSON.parse(await readBody(req));
    } catch {
      return send(res, 400, { ok: false, error: 'JSON 解析失败' });
    }
    const commandBody = isObject(body) ? body : {};
    const result = createCommand(event, {
      ...commandBody,
      action: commandBody.action || commandBody.localStatus,
    }, 'family-session');
    if (result.error) return send(res, result.code, { ok: false, error: result.error });
    return send(res, result.duplicate ? 200 : 202, {
      ok: true,
      duplicated: result.duplicate,
      requested: true,
      status: result.command.status,
      command: commandView(result.command),
      event,
    });
  }

  // --- 单条查询 ------------------------------------------------------------
  const m = pathname.match(/^\/events\/([\w.-]+)$/);
  if (m) {
    if (!authorizedSession(req)) {
      return send(res, 401, { ok: false, error: '需要家属会话授权' });
    }
    const id = m[1];
    const found = selectOne.get(id);
    if (!found) return send(res, 404, { ok: false, error: '未找到事件' });
    if (!authorizedSessionForDevice(req, found.deviceId)) {
      return send(res, 403, { ok: false, error: '会话未授权此设备' });
    }

    if (req.method === 'GET') {
      return send(res, 200, { ok: true, event: found },
                  { 'Cache-Control': 'no-store' });
    }

    if (req.method === 'PATCH') {
      return send(res, 409, {
        ok: false,
        error: '状态不能由浏览器直接写入，请使用事件命令接口',
      });
    }
  }

  if (req.method === 'GET') return serveStatic(res, pathname);
  return send(res, 405, { ok: false, error: '方法不支持' });
}

const server = NATIVE_TLS
  ? https.createServer({
      ...TLS_OPTIONS,
      minVersion: 'TLSv1.2',
    }, handleRequest)
  : http.createServer(handleRequest);

server.listen(PORT, HOST, () => {
  const scheme = NATIVE_TLS ? 'https' : 'http';
  const familyScheme = NATIVE_TLS || (!DEMO_PROFILE && PRODUCTION_TLS_TERMINATED)
    ? 'https' : 'http';
  console.log('安聆 VelaGuard 通知控制台已启动');
  console.log(`  传输      : ${NATIVE_TLS ? '原生 HTTPS' : 'HTTP（仅演示或可信 TLS 终止层内）'}`);
  console.log(`  监听      : ${scheme}://${HOST}:${PORT}`);
  console.log(`  家属视角  : ${familyScheme}://<本机局域网IP>:${PORT}/`);
  console.log(`  数据库    : ${DB_PATH}`);
  console.log('  提示      : 演示时手机与设备连同一热点即可，无需公网');
  if (DEMO_PROFILE) {
    console.log(`  配对设备编号: ${PAIRING_DEVICE_HINT}`);
  }
  if (DEMO_PROFILE && !configuredPairingKey) {
    console.log(`  本次演示配对密钥: ${PAIRING_KEY}`);
  } else if (DEMO_PROFILE) {
    console.log('  演示配对密钥: 已由 VELAGUARD_PAIRING_KEY 环境变量提供');
  }
});
