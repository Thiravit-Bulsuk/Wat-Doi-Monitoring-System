import express from 'express';
import mqtt from 'mqtt';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const app = express();
const port = Number(process.env.PORT || 4173);
const mqttUrl = process.env.MQTT_URL || 'mqtts://aa9e03df167b40b086069a385503cf2f.s1.eu.hivemq.cloud:8883';
const mqttUsername = process.env.MQTT_USERNAME;
const mqttPassword = process.env.MQTT_PASSWORD;

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
  mqttClient.subscribe([
    ...nodeDefinitions.map((node) => node.topic),
    ...nodeDefinitions.filter((node) => node.battery).map((node) => `${node.topic}/battery`),
    ...nodeDefinitions.map((node) => `link_quality/${node.topic}`),
    'master_status',
    'master_last_update',
    'notification_status',
    'notification_control',
    'message'
  ]);
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

app.use(express.json());
app.use(express.static(__dirname));

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

app.listen(port, () => console.log(`Dashboard gateway listening on http://localhost:${port}`));
