import express from 'express';
import { FieldValue, Firestore, Timestamp } from '@google-cloud/firestore';
import mqtt from 'mqtt';
import crypto from 'node:crypto';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const app = express();
const port = Number(process.env.PORT || 4173);
const mqttUrl = process.env.MQTT_URL || 'mqtts://a7821161.ala.us-east-1.emqxsl.com:8883';
const mqttUsername = process.env.MQTT_USERNAME;
const mqttPassword = process.env.MQTT_PASSWORD;
const historyToken = process.env.HISTORY_TOKEN || '';
const controlPassword = process.env.CONTROL_PASSWORD || '';

const nodeDefinitions = [
  { topic: 'JD', name: 'JD', id: 1, kind: 'water', threshold: 50 },
  { topic: 'PhraNon', name: 'PhraNon', id: 2, kind: 'water', threshold: 50 },
  { topic: 'Plastic', name: 'Plastic', id: 3, kind: 'water', threshold: 10 },
  { topic: 'ThamYai', name: 'ThamYai', id: 4, kind: 'water', threshold: 50, battery: true },
  { topic: 'RongPui', name: 'RongPui', id: 5, kind: 'water', threshold: 50, battery: true },
  { topic: 'TaiFa', name: 'TaiFa', id: 6, kind: 'water', threshold: 10 },
  { topic: 'KhonPD', name: 'KhonPD', id: 7, kind: 'water', threshold: 10 },
  { topic: 'SuanMM', name: 'SuanMM', id: 8, kind: 'water', threshold: 10 },
  { topic: 'NongPog', name: 'NongPog', id: 20, kind: 'spd' }
];

const state = {
  masterOnline: false,
  lastUpdate: null,
  notificationEnabled: null,
  mqttConnected: false,
  nodes: new Map(nodeDefinitions.map((node) => [node.topic, { ...node, value: null, batteryValue: null, linkQuality: null, updatedAt: null, error: false }])),
  events: []
};

function recordEvent(title, detail) {
  state.events.unshift({ time: new Date().toISOString(), title, detail });
  state.events = state.events.slice(0, 20);
}

function parseValue(payload) {
  const text = payload.toString().trim();
  const value = Number(text);
  return Number.isFinite(value) ? value : text;
}

function isEnabledPayload(value) {
  return String(value).trim() === '1';
}

function parseLinkQuality(payload) {
  try {
    const data = JSON.parse(payload.toString());
    return Number.isFinite(Number(data.rssi)) && Number.isFinite(Number(data.snr))
      ? { rssi: Number(data.rssi), snr: Number(data.snr), updatedAt: new Date().toISOString() }
      : null;
  } catch {
    return null;
  }
}

function getAlerts() {
  const alerts = [];
  for (const node of state.nodes.values()) {
    if (node.error) alerts.push({ node: node.name, type: 'communication', message: 'Communication error' });
    else if (node.kind === 'spd' && node.value === 1) alerts.push({ node: node.name, type: 'spd', message: 'SPD alert' });
    else if (node.kind === 'water' && typeof node.value === 'number' && node.value >= 0 && node.value < node.threshold) {
      alerts.push({ node: node.name, type: 'water', message: `Water level below ${node.threshold}%` });
    }
  }
  if (!state.masterOnline) alerts.push({ node: 'Master', type: 'system', message: 'Master offline' });
  return alerts;
}

function mqttOptions() {
  const options = { reconnectPeriod: 5000, clean: true };
  if (mqttUsername && mqttPassword) {
    options.username = mqttUsername;
    options.password = mqttPassword;
  }
  return options;
}

const mqttClient = mqtt.connect(mqttUrl, mqttOptions());
mqttClient.on('connect', () => {
  state.mqttConnected = true;
  // EMQX Serverless allows only 10 subscriptions per client, so use wildcards; unknown topics are ignored below.
  mqttClient.subscribe(['+', '+/battery', 'link_quality/+'], (error) => {
    if (error) recordEvent('MQTT subscribe error', error.message);
  });
  recordEvent('MQTT gateway connected', 'Subscribed to monitoring topics');
});
mqttClient.on('reconnect', () => { state.mqttConnected = false; });
mqttClient.on('offline', () => { state.mqttConnected = false; state.masterOnline = false; });
mqttClient.on('error', (error) => { recordEvent('MQTT gateway error', error.message); });
mqttClient.on('message', (topic, payload) => {
  const value = parseValue(payload);
  const now = new Date().toISOString();
  if (topic === 'master_status') state.masterOnline = value === 'online';
  else if (topic === 'master_last_update') state.lastUpdate = String(value);
  else if (topic === 'notification_status') state.notificationEnabled = isEnabledPayload(value);
  else if (topic === 'notification_control') state.notificationEnabled = isEnabledPayload(value);
  else if (topic === 'message') recordEvent('Master message', String(value));
  else if (topic.startsWith('link_quality/')) {
    const node = state.nodes.get(topic.slice('link_quality/'.length));
    const linkQuality = parseLinkQuality(payload);
    if (node && linkQuality) node.linkQuality = linkQuality;
  }
  else if (topic.endsWith('/battery')) {
    const node = state.nodes.get(topic.slice(0, -8));
    if (node) node.batteryValue = value;
  } else {
    const node = state.nodes.get(topic);
    if (node) {
      node.value = value;
      node.error = value === -1;
      node.updatedAt = now;
    }
  }
});

app.set('trust proxy', 1);
app.use(express.json({ limit: '32kb' }));
app.use(express.static(__dirname));

// ---------- History (Firestore, kept 3 days) ----------
// The master uploads its logs and each poll cycle's levels; one document per Bangkok day per
// collection, deleted by a TTL policy on expireAt. Reads are cached per document so page views
// cannot exhaust the Firestore free quota.
const HISTORY_DAYS = 3;
const DAY_MS = 24 * 60 * 60 * 1000;
const BANGKOK_OFFSET_MS = 7 * 60 * 60 * 1000;
const TODAY_CACHE_MS = 60 * 1000;
const PAST_CACHE_MS = 10 * 60 * 1000;
const UPLOAD_MAX_ITEMS = 50;
const LOG_TEXT_MAX = 200;
const LEVELS = 'waterLevels';
const LOGS = 'systemLogs';
const waterNodes = nodeDefinitions.filter((node) => node.kind === 'water');

let firestore;
const db = () => (firestore ??= new Firestore());
const dayKey = (ms) => new Date(ms + BANGKOK_OFFSET_MS).toISOString().slice(0, 10);
const dayStartMs = (key) => Date.parse(`${key}T00:00:00+07:00`);
const historyDays = () => Array.from({ length: HISTORY_DAYS }, (_, i) => dayKey(Date.now() - i * DAY_MS));

function secretMatches(given, expected) {
  const a = crypto.createHash('sha256').update(String(given || '')).digest();
  const b = crypto.createHash('sha256').update(expected).digest();
  return crypto.timingSafeEqual(a, b);
}

const docCache = new Map();

async function readDay(collection, key) {
  const id = `${collection}/${key}`;
  const cached = docCache.get(id);
  const maxAge = key === dayKey(Date.now()) ? TODAY_CACHE_MS : PAST_CACHE_MS;
  if (cached && Date.now() - cached.at < maxAge) return cached.data;
  const snap = await db().collection(collection).doc(key).get();
  const data = snap.exists ? snap.data() : {};
  docCache.set(id, { at: Date.now(), data });
  for (const [cacheId, entry] of docCache) {
    if (Date.now() - entry.at > PAST_CACHE_MS) docCache.delete(cacheId);
  }
  return data;
}

// Device time 0 or far off means its clock was not synced yet.
function entryTime(seconds, now) {
  const ms = Number.isInteger(seconds) ? seconds * 1000 : 0;
  return ms > 0 && Math.abs(ms - now) < DAY_MS ? ms : now;
}

function addToDay(byDay, t, entry) {
  const key = dayKey(t);
  if (!byDay.has(key)) byDay.set(key, []);
  byDay.get(key).push(entry);
}

async function saveHistory(logs, samples) {
  const now = Date.now();
  const logDays = new Map();
  for (const item of logs) {
    const text = typeof item?.m === 'string' ? item.m.slice(0, LOG_TEXT_MAX) : '';
    if (!text) continue;
    const t = entryTime(item.t, now);
    addToDay(logDays, t, { t, s: Number.isInteger(item.s) ? item.s : 0, m: text });
  }
  const levelDays = new Map();
  for (const item of samples) {
    const v = {};
    for (const node of waterNodes) {
      const value = item?.v?.[node.topic];
      // -1 is the master's communication-error marker.
      v[node.topic] = Number.isFinite(value) && value >= 0 && value <= 100 ? value : null;
    }
    const t = entryTime(item?.t, now);
    addToDay(levelDays, t, { t, v });
  }

  const batch = db().batch();
  for (const [collection, field, byDay] of [[LOGS, 'logs', logDays], [LEVELS, 'samples', levelDays]]) {
    for (const [key, entries] of byDay) {
      batch.set(db().collection(collection).doc(key), {
        expireAt: Timestamp.fromMillis(dayStartMs(key) + (HISTORY_DAYS + 1) * DAY_MS),
        // arrayUnion skips entries already stored, so a retried upload is not duplicated.
        [field]: FieldValue.arrayUnion(...entries)
      }, { merge: true });
      docCache.delete(`${collection}/${key}`);
    }
  }
  if (logDays.size || levelDays.size) await batch.commit();
}

// Called by the master about once a minute when it has new logs or level samples.
app.post('/api/history', async (request, response) => {
  if (!historyToken) return response.status(503).json({ error: 'HISTORY_TOKEN not set' });
  if (!secretMatches(request.get('x-log-token'), historyToken)) return response.status(401).json({ error: 'unauthorized' });
  const logs = request.body?.logs ?? [];
  const samples = request.body?.samples ?? [];
  if (!Array.isArray(logs) || !Array.isArray(samples) || logs.length > UPLOAD_MAX_ITEMS || samples.length > UPLOAD_MAX_ITEMS) {
    return response.status(400).json({ error: 'invalid body' });
  }
  try {
    await saveHistory(logs, samples);
    response.json({ ok: true });
  } catch (error) {
    console.error('Save history failed:', error.message);
    response.status(502).json({ error: 'save failed' });
  }
});

app.get('/api/logs', async (_request, response) => {
  try {
    const since = Date.now() - HISTORY_DAYS * DAY_MS;
    const docs = await Promise.all(historyDays().map((key) => readDay(LOGS, key)));
    const logs = docs.flatMap((doc) => doc.logs || [])
      .filter((log) => log.t >= since)
      .sort((a, b) => b.t - a.t || b.s - a.s)
      .map((log) => ({ time: new Date(log.t).toISOString(), text: log.m }));
    response.set('Cache-Control', 'no-cache');
    response.json({ logs });
  } catch (error) {
    console.error('Read logs failed:', error.message);
    response.status(502).json({ error: 'read failed' });
  }
});

app.get('/api/levels', async (request, response) => {
  const node = waterNodes.find((n) => n.topic === request.query.node);
  const day = String(request.query.day || '');
  if (!node || !historyDays().includes(day)) return response.status(400).json({ error: 'invalid node or day' });
  try {
    const doc = await readDay(LEVELS, day);
    const points = (doc.samples || [])
      .map((sample) => ({ t: sample.t, v: sample.v?.[node.topic] ?? null }))
      .sort((a, b) => a.t - b.t);
    response.set('Cache-Control', day === dayKey(Date.now()) ? 'no-cache' : 'private, max-age=600');
    response.json({ node: node.topic, threshold: node.threshold, day, dayStart: dayStartMs(day), points });
  } catch (error) {
    console.error('Read levels failed:', error.message);
    response.status(502).json({ error: 'read failed' });
  }
});

app.get('/api/dashboard', (_request, response) => {
  const nodes = [...state.nodes.values()];
  const responding = nodes.filter((node) => node.value !== null && !node.error).length;
  const alerts = getAlerts();
  const linkQualityCount = nodes.filter((node) => node.linkQuality).length;
  response.json({
    system: { masterOnline: state.masterOnline, mqttConnected: state.mqttConnected, lastUpdate: state.lastUpdate, notificationEnabled: state.notificationEnabled, repeaterLink: linkQualityCount ? 'live' : 'unknown', linkQualityCount, responding, total: nodes.length, alertCount: alerts.length },
    nodes,
    alerts,
    events: state.events
  });
});

app.post('/api/notification', (request, response) => {
  if (!state.mqttConnected) {
    response.status(503).json({ ok: false, error: 'MQTT gateway is offline' });
    return;
  }

  const enabled = request.body?.enabled === true;
  const payload = enabled ? '1' : '0';
  mqttClient.publish('notification_control', payload, { qos: 1, retain: true }, (error) => {
    if (error) {
      response.status(500).json({ ok: false, error: error.message });
      return;
    }
    state.notificationEnabled = enabled;
    recordEvent('Notification command', enabled ? 'Set notification service to ENABLED' : 'Set notification service to DISABLED');
    response.json({ ok: true, enabled });
  });
});

// ---------- Master commands (password protected) ----------
// Sent non-retained: a retained "1" on reset_system would reboot the master on every reconnect.
const MASTER_COMMANDS = {
  request_data: { topic: 'request_data', label: 'Request data' },
  reset_notification: { topic: 'reset_notification', label: 'Reset notification' },
  reset_system: { topic: 'reset_system', label: 'Reset system' }
};
const MAX_FAILED_LOGINS = 10;
const FAILED_LOGIN_WINDOW_MS = 10 * 60 * 1000;
const failedLogins = new Map();

function isLockedOut(ip) {
  const entry = failedLogins.get(ip);
  if (!entry) return false;
  if (Date.now() - entry.first > FAILED_LOGIN_WINDOW_MS) {
    failedLogins.delete(ip);
    return false;
  }
  return entry.count >= MAX_FAILED_LOGINS;
}

function recordFailedLogin(ip) {
  const entry = failedLogins.get(ip) || { count: 0, first: Date.now() };
  entry.count += 1;
  failedLogins.set(ip, entry);
}

app.post('/api/command', (request, response) => {
  if (!controlPassword) return response.status(503).json({ error: 'ยังไม่ได้ตั้งรหัสควบคุม (CONTROL_PASSWORD)' });
  if (isLockedOut(request.ip)) return response.status(429).json({ error: 'ใส่รหัสผิดหลายครั้ง กรุณารอ 10 นาที' });
  if (!secretMatches(request.get('x-control-password'), controlPassword)) {
    recordFailedLogin(request.ip);
    return response.status(401).json({ error: 'รหัสผ่านไม่ถูกต้อง' });
  }
  failedLogins.delete(request.ip);

  const command = MASTER_COMMANDS[request.body?.action];
  if (!command) return response.status(400).json({ error: 'คำสั่งไม่ถูกต้อง' });
  if (!state.mqttConnected) return response.status(503).json({ error: 'MQTT gateway is offline' });
  mqttClient.publish(command.topic, '1', { qos: 1, retain: false }, (error) => {
    if (error) return response.status(502).json({ error: 'ส่งคำสั่งไม่สำเร็จ' });
    recordEvent('Master command', command.label);
    response.json({ ok: true });
  });
});

app.listen(port, () => console.log(`Dashboard gateway listening on http://localhost:${port}`));
