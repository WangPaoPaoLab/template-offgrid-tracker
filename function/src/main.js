import { Client, ID, Query, TablesDB } from "node-appwrite";

const DATABASE_ID = process.env.OFFGRID_TRACKER_DATABASE_ID || process.env.DATABASE_ID;
const TELEMETRY_TABLE_ID = process.env.OFFGRID_TRACKER_TELEMETRY_TABLE_ID || process.env.TELEMETRY_TABLE_ID;
const OTA_UPDATE_TABLE_ID = process.env.OFFGRID_TRACKER_OTA_UPDATE_TABLE_ID || "ota-updates";
const DOWNLINK_TABLE_ID = process.env.OFFGRID_TRACKER_DOWNLINK_TABLE_ID || "downlinks";
const OTA_REPOSITORY_URL = process.env.OFFGRID_TRACKER_REPOSITORY_URL || process.env.APPWRITE_VCS_REPOSITORY_URL || "";
const GEOFENCE_AREA_TABLE_ID = process.env.OFFGRID_TRACKER_GEOFENCE_AREA_TABLE_ID;
const GEOFENCE_RULE_TABLE_ID = process.env.OFFGRID_TRACKER_GEOFENCE_RULE_TABLE_ID;
const GEOFENCE_ALARM_TABLE_ID = process.env.OFFGRID_TRACKER_GEOFENCE_ALARM_TABLE_ID;
const SENSOR_BATTERY_VOLTAGE = 12;
const SENSOR_MCU_TEMPERATURE = 13;
const SENSOR_LATITUDE = 3;
const SENSOR_LONGITUDE = 4;
const SENSOR_ACCELEROMETER = [6, 7, 8];
const SENSOR_GYROSCOPE = [9, 10, 11];
const SENSOR_TYPE_MAX = 13;
const SENSOR_BLOCK_HEADER_SIZE = 24;
const SENSOR_BLOCK_MAX_SAMPLES = 32;
const PAWR_BULK_HEADER_SIZE = 52;
const PAWR_BULK_VERSION = 4;
const PAWR_BULK_KIND_DATA = 1;
const SENSOR_BLOCK_KINDS = new Map([
  [1, { name: "mcu_temperature", recordSize: 4 }],
  [2, { name: "imu", recordSize: 25 }],
  [3, { name: "battery_voltage", recordSize: 4 }],
]);
const SENSOR_FIELD_NAMES = new Map([
  [1, "temperature"],
  [2, "humidity"],
  [5, "length"],
  [6, "accel_x"],
  [7, "accel_y"],
  [8, "accel_z"],
  [9, "gyro_x"],
  [10, "gyro_y"],
  [11, "gyro_z"],
  [12, "battery_voltage"],
  [13, "mcu_temperature"],
]);
const OTA_STATUSES = new Set(["pending", "succeeded", "failed", "busy"]);
const DOWNLINK_STATUSES = new Set(["cached", "transmitting", "applied", "failed", "expired"]);
const LATEST_RELEASE_CACHE_MS = 5 * 60 * 1000;
let latestReleaseCache = { repository: "", version: "", expiresAt: 0 };

function normalizedFirmwareVersion(value) {
  return typeof value === "string" ? value.trim().replace(/^v/i, "") : "";
}

async function latestReleaseVersion() {
  const repository = OTA_REPOSITORY_URL.replace(/\.git$/, "").replace(/\/$/, "");
  if (!repository) return "";
  const now = Date.now();
  if (latestReleaseCache.repository === repository && latestReleaseCache.expiresAt > now) {
    return latestReleaseCache.version;
  }
  let version = "";
  try {
    const response = await fetch(`${repository}/releases/latest`, {
      method: "HEAD",
      redirect: "follow",
      signal: AbortSignal.timeout(3000),
    });
    if (response.ok && response.url) {
      const match = /\/releases\/tag\/([^/?#]+)/.exec(new URL(response.url).pathname);
      if (match) version = decodeURIComponent(match[1]);
    }
  } catch {
    // OTA ingestion must continue even if the release host is temporarily unavailable.
  }
  latestReleaseCache = { repository, version, expiresAt: now + LATEST_RELEASE_CACHE_MS };
  return version;
}

function sensorValue(payload, type) {
  if (!Array.isArray(payload?.sensors)) return null;
  const sensor = payload.sensors.find((candidate) => candidate?.type === type);
  return typeof sensor?.value === "number" && Number.isFinite(sensor.value) ? sensor.value : null;
}

function measurementForDevice(deviceId) {
  return `device_${deviceId.replace(/[^A-Za-z0-9_-]/g, "_")}`;
}

function escapeMeasurement(value) {
  return value.replace(/\\/g, "\\\\").replace(/([ ,])/g, "\\$1");
}

function escapeTag(value) {
  return String(value).replace(/\\/g, "\\\\").replace(/([ ,=])/g, "\\$1");
}

function stringField(value) {
  return `"${String(value).replace(/\\/g, "\\\\").replace(/"/g, '\\"')}"`;
}

function telemetryLine(target, entry, channel, receivedAt, sequence = 0) {
  const tags = `serial=${escapeTag(target.serial)},channel=${escapeTag(channel)}`;
  const fields = [`payload=${stringField(JSON.stringify(entry))}`];
  for (const sensor of entry.sensors || []) {
    const name = SENSOR_FIELD_NAMES.get(sensor.type);
    if (name) fields.push(`sensor_${name}=${sensor.value}`);
  }
  const latitude = sensorValue(entry, SENSOR_LATITUDE);
  const longitude = sensorValue(entry, SENSOR_LONGITUDE);
  if (latitude !== null && longitude !== null) {
    fields.push(`lat=${latitude}`, `lon=${longitude}`);
  }
  if (Number.isInteger(entry.batteryVoltageMv) && entry.batteryVoltageMv >= 0 && entry.batteryVoltageMv <= 10000 &&
      !entry.sensors?.some((sensor) => sensor.type === SENSOR_BATTERY_VOLTAGE)) {
    fields.push(`sensor_battery_voltage=${entry.batteryVoltageMv / 1000}`);
  }
  const timestamp = BigInt(Date.parse(receivedAt)) * 1000000n + BigInt(sequence);
  return `${escapeMeasurement(measurementForDevice(target.$id))},${tags} ${fields.join(",")} ${timestamp}`;
}

function blockTopicMac(channel) {
  const match = /^blocks\/([0-9a-f]{2}(?::[0-9a-f]{2}){5})$/i.exec(channel);
  return match ? match[1].toUpperCase() : "";
}

function fnv1a32(bytes) {
  let digest = 0x811c9dc5;
  for (const byte of bytes) digest = Math.imul(digest ^ byte, 0x01000193) >>> 0;
  return digest;
}

function unwrapPawrBulk(bytes) {
  if (bytes.length >= 4 && bytes.subarray(0, 4).toString("ascii") === "EZSB") return bytes;
  if (bytes.length < PAWR_BULK_HEADER_SIZE ||
      bytes.subarray(0, 4).toString("ascii") !== "EZBT" ||
      bytes[4] !== PAWR_BULK_VERSION || bytes[7] !== PAWR_BULK_KIND_DATA || bytes[47] !== 0) {
    throw new Error("Sensor block transport header is invalid");
  }

  const totalLength = bytes.readUInt32BE(12);
  const offset = bytes.readUInt32BE(16);
  const expectedDigest = bytes.readUInt32BE(20);
  const chunkIndex = bytes.readUInt16BE(24);
  const chunkCount = bytes.readUInt16BE(26);
  const dataLength = bytes.readUInt16BE(28);
  const windowBaseChunk = bytes.readUInt16BE(44);
  const windowCount = bytes[46];
  const data = bytes.subarray(PAWR_BULK_HEADER_SIZE);
  if (offset !== 0 || chunkIndex !== 0 || chunkCount !== 1 || windowBaseChunk !== 0 || windowCount !== 1 ||
      totalLength !== dataLength || dataLength !== data.length || fnv1a32(data) !== expectedDigest) {
    throw new Error("Sensor block transport shape is invalid");
  }
  return data;
}

function decodeSensorBlock(encoded, encoding) {
  if (encoding !== "base64" || typeof encoded !== "string") {
    throw new Error("Sensor block payload must be base64 encoded");
  }
  const transport = Buffer.from(encoded, "base64");
  const bytes = unwrapPawrBulk(transport);
  if (bytes.length < SENSOR_BLOCK_HEADER_SIZE || bytes.subarray(0, 4).toString("ascii") !== "EZSB" ||
      bytes[4] !== 1 || bytes[7] !== SENSOR_BLOCK_HEADER_SIZE) {
    throw new Error("Sensor block header is invalid");
  }
  const kind = SENSOR_BLOCK_KINDS.get(bytes[5]);
  const samplingIntervalMs = bytes.readUInt32LE(8);
  const configuredSamples = bytes.readUInt16LE(12);
  const sampleCount = bytes.readUInt16LE(14);
  const blockSequence = bytes.readUInt32LE(16);
  const recordSize = bytes.readUInt16LE(20);
  if (!kind || recordSize !== kind.recordSize || configuredSamples > SENSOR_BLOCK_MAX_SAMPLES ||
      sampleCount < 1 || sampleCount > SENSOR_BLOCK_MAX_SAMPLES ||
      samplingIntervalMs < 1 || samplingIntervalMs > 3600000 ||
      bytes.length !== SENSOR_BLOCK_HEADER_SIZE + sampleCount * recordSize) {
    throw new Error("Sensor block shape is invalid");
  }
  if (transport !== bytes && transport[6] !== bytes[5]) {
    throw new Error("Sensor block transport kind does not match its payload");
  }
  const samples = [];
  for (let index = 0, offset = SENSOR_BLOCK_HEADER_SIZE; index < sampleCount; index += 1, offset += recordSize) {
    if (bytes[5] === 2) {
      samples.push({
        accel_x: bytes.readFloatLE(offset), accel_y: bytes.readFloatLE(offset + 4), accel_z: bytes.readFloatLE(offset + 8),
        gyro_x: bytes.readFloatLE(offset + 12), gyro_y: bytes.readFloatLE(offset + 16), gyro_z: bytes.readFloatLE(offset + 20),
        gyro_valid: bytes[offset + 24] === 1,
      });
    } else {
      samples.push({ [kind.name]: bytes.readFloatLE(offset) });
    }
  }
  if (samples.some((sample) => Object.values(sample).some((value) => typeof value === "number" && !Number.isFinite(value)))) {
    throw new Error("Sensor block contains non-finite samples");
  }
  return { kind: kind.name, samplingIntervalMs, configuredSamples, sampleCount, blockSequence, samples };
}

function blockLines(target, block, receivedAt) {
  const receivedNs = BigInt(Date.parse(receivedAt)) * 1000000n;
  return block.samples.map((sample, index) => {
    const elapsedMs = index * block.samplingIntervalMs;
    const ageMs = BigInt((block.sampleCount - 1) * block.samplingIntervalMs - elapsedMs);
    const timestamp = receivedNs - ageMs * 1000000n;
    const tags = [
      `serial=${escapeTag(target.serial)}`,
      "channel=blocks",
      `sample_kind=${escapeTag(block.kind)}`,
      `block_sequence=${block.blockSequence}`,
      `sample_index=${index}`,
    ].join(",");
    const fields = [
      `sampling_interval_ms=${block.samplingIntervalMs}i`,
      `configured_samples=${block.configuredSamples}i`,
      ...Object.entries(sample).map(([name, value]) =>
        typeof value === "boolean" ? `${name}=${value}` : `sensor_${name}=${value}`),
    ];
    return `${escapeMeasurement(measurementForDevice(target.$id))},${tags} ${fields.join(",")} ${timestamp}`;
  });
}

function gatewayCanRelayForTarget(gateway, target) {
  if (target.$id === gateway.$id || target.gatewayDeviceId === gateway.$id) return true;
  const gatewayReads = new Set((gateway.$permissions || []).filter((permission) => permission.startsWith("read(")));
  return (target.$permissions || []).some((permission) => permission.startsWith("read(") && gatewayReads.has(permission));
}

async function writeTimeseries(req, lines) {
  const endpoint = process.env.APPWRITE_FUNCTION_API_ENDPOINT.replace(/\/+$/, "");
  const projectId = process.env.APPWRITE_FUNCTION_PROJECT_ID;
  const response = await fetch(`${endpoint}/timeseries/stores/${encodeURIComponent(projectId)}/points`, {
    method: "POST",
    headers: {
      "content-type": "application/json",
      "x-appwrite-project": projectId,
      "x-appwrite-key": req.headers["x-appwrite-key"],
    },
    body: JSON.stringify({ data: lines.join("\n"), precision: "nanosecond" }),
  });
  if (!response.ok) throw new Error(`Time-series write failed with ${response.status}`);
}

async function upsertLatestTelemetry(tables, target, data, readPermissions) {
  try {
    return await tables.updateRow({
      databaseId: DATABASE_ID, tableId: TELEMETRY_TABLE_ID, rowId: target.$id, data, permissions: readPermissions,
    });
  } catch (caught) {
    const code = caught?.code ?? caught?.status;
    if (code !== 404) throw caught;
  }
  try {
    return await tables.createRow({
      databaseId: DATABASE_ID, tableId: TELEMETRY_TABLE_ID, rowId: target.$id, data, permissions: readPermissions,
    });
  } catch (caught) {
    const code = caught?.code ?? caught?.status;
    if (code !== 409) throw caught;
    return tables.updateRow({
      databaseId: DATABASE_ID, tableId: TELEMETRY_TABLE_ID, rowId: target.$id, data, permissions: readPermissions,
    });
  }
}

function validDownlinkStatus(value) {
  return value && typeof value === "object" && !Array.isArray(value) &&
    typeof value.requestId === "string" && value.requestId.length >= 1 && value.requestId.length <= 64 &&
    DOWNLINK_STATUSES.has(value.status) && Number.isInteger(value.revision) &&
    value.revision >= 1 && value.revision <= 0xffffffff &&
    (value.detail === undefined || (typeof value.detail === "string" && value.detail.length <= 256));
}

async function syncDownlink(tables, gateway, target, value, readPermissions, reportedAt) {
  const existing = await tables.listRows({
    databaseId: DATABASE_ID, tableId: DOWNLINK_TABLE_ID,
    queries: [Query.equal("requestId", value.requestId), Query.limit(1)],
  });
  const data = {
    deviceId: target.$id,
    serial: target.serial,
    gatewayDeviceId: gateway.$id,
    gatewaySerial: gateway.serial,
    requestId: value.requestId,
    revision: value.revision,
    status: value.status,
    detail: value.detail || "",
    reportedAt,
    completedAt: ["applied", "failed", "expired"].includes(value.status) ? reportedAt : null,
  };
  if (existing.rows?.[0]) {
    await tables.updateRow({ databaseId: DATABASE_ID, tableId: DOWNLINK_TABLE_ID,
      rowId: existing.rows[0].$id, data });
  } else {
    await tables.createRow({ databaseId: DATABASE_ID, tableId: DOWNLINK_TABLE_ID,
      rowId: ID.unique(), data, permissions: readPermissions });
  }
}

function otaStatus(entry) {
  const ota = entry?.ota;
  if (!ota || typeof ota !== "object" || Array.isArray(ota)) return null;
  if (typeof ota.requestId !== "string" || !ota.requestId || ota.requestId.length > 64 ||
      typeof ota.status !== "string" || !OTA_STATUSES.has(ota.status) ||
      (ota.detail !== undefined && (typeof ota.detail !== "string" || ota.detail.length > 256))) return undefined;
  return { requestId: ota.requestId, status: ota.status, detail: ota.detail || "" };
}

async function syncOtaUpdate(tables, target, entry, readPermissions, reportedAt) {
  const update = otaStatus(entry);
  if (!update) return;
  const existing = await tables.listRows({
    databaseId: DATABASE_ID,
    tableId: OTA_UPDATE_TABLE_ID,
    queries: [Query.equal("requestId", update.requestId), Query.limit(1)],
  });
  const firmwareVersion = typeof entry.firmwareVersion === "string" && entry.firmwareVersion.length <= 128
    ? entry.firmwareVersion : "";
  const row = existing.rows?.[0];
  const targetFirmwareVersion = row?.targetFirmwareVersion || await latestReleaseVersion();
  const completed = update.status === "succeeded" || update.status === "failed" || update.status === "busy";
  const data = {
    deviceId: target.$id,
    serial: target.serial,
    requestId: update.requestId,
    status: update.status,
    detail: update.detail,
    firmwareVersion,
    targetFirmwareVersion,
    reportedAt,
    completedAt: completed ? reportedAt : null,
  };
  if (row) {
    await tables.updateRow({ databaseId: DATABASE_ID, tableId: OTA_UPDATE_TABLE_ID, rowId: row.$id, data, permissions: readPermissions });
  } else {
    await tables.createRow({ databaseId: DATABASE_ID, tableId: OTA_UPDATE_TABLE_ID, rowId: ID.unique(), data, permissions: readPermissions });
  }
}

async function reconcileOtaUpdate(tables, target, entry, readPermissions, reportedAt) {
  const firmwareVersion = typeof entry.firmwareVersion === "string" && entry.firmwareVersion.length <= 128
    ? entry.firmwareVersion : "";
  if (!firmwareVersion) return;
  const existing = await tables.listRows({
    databaseId: DATABASE_ID,
    tableId: OTA_UPDATE_TABLE_ID,
    queries: [Query.equal("deviceId", target.$id), Query.orderDesc("reportedAt"), Query.limit(25)],
  });
  const pending = (existing.rows || []).filter((row) => row.status === "pending");
  if (!pending.length) return;
  const fallbackTargetVersion = pending.some((row) => !row.targetFirmwareVersion)
    ? await latestReleaseVersion() : "";
  for (const row of pending) {
    const targetVersion = row.targetFirmwareVersion || fallbackTargetVersion;
    if (!targetVersion || normalizedFirmwareVersion(firmwareVersion) !== normalizedFirmwareVersion(targetVersion)) continue;
    await tables.updateRow({
      databaseId: DATABASE_ID,
      tableId: OTA_UPDATE_TABLE_ID,
      rowId: row.$id,
      data: {
        status: "succeeded",
        detail: "running target firmware",
        firmwareVersion,
        targetFirmwareVersion: targetVersion,
        reportedAt,
        completedAt: reportedAt,
      },
      permissions: readPermissions,
    });
  }
}

function locationOf(payload) {
  const latitude = sensorValue(payload, SENSOR_LATITUDE);
  const longitude = sensorValue(payload, SENSOR_LONGITUDE);
  return latitude !== null && longitude !== null && latitude >= -90 && latitude <= 90 && longitude >= -180 && longitude <= 180
    ? { latitude, longitude } : null;
}

function mapAppearanceOf(device) {
  const icon = typeof device.markerType === "string" && device.markerType.length <= 32
    ? device.markerType : null;
  const markerColor = typeof device.markerColor === "string" && device.markerColor.length <= 32
    ? device.markerColor : null;
  return {
    ...(icon ? { icon } : {}),
    ...(markerColor ? { markerColor } : {}),
  };
}

function localMeters(point, center, rotation = 0) {
  const latitude = (point.latitude - center.latitude) * 111320;
  const longitude = (point.longitude - center.longitude) * 111320 * Math.cos(center.latitude * Math.PI / 180);
  const radians = -rotation * Math.PI / 180;
  return { x: longitude * Math.cos(radians) - latitude * Math.sin(radians), y: longitude * Math.sin(radians) + latitude * Math.cos(radians) };
}

function containsArea(area, point) {
  let geometry;
  try { geometry = JSON.parse(area.geometry); } catch { return false; }
  const center = geometry.center;
  if (!center || !Number.isFinite(center.latitude) || !Number.isFinite(center.longitude)) return false;
  if (area.shape === "polygon") {
    const vertices = geometry.vertices;
    if (!Array.isArray(vertices) || vertices.length < 3) return false;
    let inside = false;
    for (let i = 0, j = vertices.length - 1; i < vertices.length; j = i++) {
      const a = vertices[i], b = vertices[j];
      if (((a.latitude > point.latitude) !== (b.latitude > point.latitude)) &&
          point.longitude < (b.longitude - a.longitude) * (point.latitude - a.latitude) /
            (b.latitude - a.latitude) + a.longitude) inside = !inside;
    }
    return inside;
  }
  const { x, y } = localMeters(point, center, geometry.rotationDegrees || 0);
  if (area.shape === "circle") return x * x + y * y <= geometry.radiusMeters ** 2;
  if (area.shape === "oval") return (x / geometry.radiusXMeters) ** 2 + (y / geometry.radiusYMeters) ** 2 <= 1;
  if (area.shape === "rectangle") return Math.abs(x) <= geometry.widthMeters / 2 && Math.abs(y) <= geometry.heightMeters / 2;
  return false;
}

async function evaluateGeofences(tables, target, entry, readPermissions, receivedAt) {
  if (!GEOFENCE_AREA_TABLE_ID || !GEOFENCE_RULE_TABLE_ID || !GEOFENCE_ALARM_TABLE_ID) return;
  const point = locationOf(entry);
  const profileId = target.metadata?.profileId;
  if (!point || !profileId) return;
  const [areasResult, rulesResult, alarmsResult] = await Promise.all([
    tables.listRows({ databaseId: DATABASE_ID, tableId: GEOFENCE_AREA_TABLE_ID, queries: [Query.equal("profileId", profileId), Query.limit(100)] }),
    tables.listRows({ databaseId: DATABASE_ID, tableId: GEOFENCE_RULE_TABLE_ID, queries: [Query.equal("profileId", profileId), Query.limit(100)] }),
    tables.listRows({ databaseId: DATABASE_ID, tableId: GEOFENCE_ALARM_TABLE_ID, queries: [Query.equal("deviceId", target.$id), Query.limit(100)] }),
  ]);
  const areas = new Map((areasResult.rows || []).map((area) => [area.$id, area]));
  const alarms = new Map((alarmsResult.rows || []).map((alarm) => [`${alarm.ruleId}:${alarm.event}`, alarm]));
  for (const rule of rulesResult.rows || []) {
    const area = areas.get(rule.areaId);
    if (!area || (Array.isArray(rule.deviceIds) && rule.deviceIds.length && !rule.deviceIds.includes(target.$id))) continue;
    const isInside = containsArea(area, point);
    for (const [event, matches] of [["enter", Boolean(rule.enterAlert) && isInside], ["exit", Boolean(rule.exitAlert) && !isInside]]) {
      const existing = alarms.get(`${rule.$id}:${event}`);
      const location = `${point.latitude.toFixed(6)},${point.longitude.toFixed(6)}`;
      if (matches && !existing) {
        await tables.createRow({ databaseId: DATABASE_ID, tableId: GEOFENCE_ALARM_TABLE_ID, rowId: ID.unique(), data: {
          profileId, areaId: area.$id, ruleId: rule.$id, deviceId: target.$id, event, active: true, acknowledged: false,
          lastLocation: location, raisedAt: receivedAt,
        }, permissions: readPermissions });
      } else if (existing && existing.active !== matches) {
        await tables.updateRow({ databaseId: DATABASE_ID, tableId: GEOFENCE_ALARM_TABLE_ID, rowId: existing.$id, data: {
          active: matches, lastLocation: location, ...(matches ? { acknowledged: false, raisedAt: receivedAt, clearedAt: "" } : { clearedAt: receivedAt }),
        } });
      }
    }
  }
}

function json(res, payload, status = 200) {
  return res.json(payload, status, { "cache-control": "no-store" });
}

function bodyOf(req) {
  if (req.bodyJson && typeof req.bodyJson === "object") return req.bodyJson;
  if (!req.body) return {};
  try { return JSON.parse(req.body); } catch { return {}; }
}

function eventDeviceId(req) {
  const event = req.headers["x-appwrite-event"] || "";
  return /^devices\.(.+)\.mqtt\.message\.publish$/.exec(event)?.[1] || "";
}

function deletedDeviceId(req) {
  const event = req.headers["x-appwrite-event"] || "";
  return /^devices\.(.+)\.delete$/.exec(event)?.[1] || "";
}

function topicParts(topic) {
  const match = /^projects\/([^/]+)\/devices\/([A-Za-z0-9][A-Za-z0-9._:-]{0,35})\/telemetry\/(.+)$/.exec(topic);
  return match ? { projectId: match[1], serial: match[2], channel: match[3] } : null;
}

async function getDevice(req, deviceId) {
  const endpoint = process.env.APPWRITE_FUNCTION_API_ENDPOINT.replace(/\/+$/, "");
  const response = await fetch(`${endpoint}/devices/${encodeURIComponent(deviceId)}`, {
    headers: {
      "x-appwrite-project": process.env.APPWRITE_FUNCTION_PROJECT_ID,
      "x-appwrite-key": req.headers["x-appwrite-key"],
    },
  });
  if (response.status === 404) return null;
  if (!response.ok) throw new Error(`Device lookup failed with ${response.status}`);
  return response.json();
}

async function getDeviceByHalowMac(req, halowMac) {
  const endpoint = process.env.APPWRITE_FUNCTION_API_ENDPOINT.replace(/\/+$/, "");
  const query = encodeURIComponent(Query.equal("halowMac", [halowMac]));
  const response = await fetch(`${endpoint}/devices?queries[]=${query}`, {
    method: "GET",
    headers: {
      "x-appwrite-project": process.env.APPWRITE_FUNCTION_PROJECT_ID,
      "x-appwrite-key": req.headers["x-appwrite-key"],
    },
  });
  if (!response.ok) throw new Error(`Device lookup by HaLow MAC failed with ${response.status}`);
  const result = await response.json();
  return Array.isArray(result.devices) && result.devices.length === 1 ? result.devices[0] : null;
}

async function updateDeviceLocation(req, deviceId, location) {
  const endpoint = process.env.APPWRITE_FUNCTION_API_ENDPOINT.replace(/\/+$/, "");
  const response = await fetch(`${endpoint}/devices/${encodeURIComponent(deviceId)}`, {
    method: "PATCH",
    headers: {
      "content-type": "application/json",
      "x-appwrite-project": process.env.APPWRITE_FUNCTION_PROJECT_ID,
      "x-appwrite-key": req.headers["x-appwrite-key"],
    },
    body: JSON.stringify({ location: [location.longitude, location.latitude] }),
  });
  if (!response.ok) throw new Error(`Device location update failed with ${response.status}`);
  return response.json();
}

export default async function main({ req, res, error, log = () => {} }) {
  if (!DATABASE_ID || !TELEMETRY_TABLE_ID) {
    return json(res, { error: "Function environment is incomplete" }, 500);
  }

  const body = bodyOf(req);

  const removedDeviceId = deletedDeviceId(req);
  if (removedDeviceId) {
    const client = new Client()
      .setEndpoint(process.env.APPWRITE_FUNCTION_API_ENDPOINT)
      .setProject(process.env.APPWRITE_FUNCTION_PROJECT_ID)
      .setKey(req.headers["x-appwrite-key"]);
    const tables = new TablesDB(client);
    try {
      await tables.deleteRow({
        databaseId: DATABASE_ID, tableId: TELEMETRY_TABLE_ID, rowId: removedDeviceId,
      });
      log(`Deleted latest telemetry for removed device ${removedDeviceId}`);
    } catch (caught) {
      const code = caught?.code ?? caught?.status;
      if (code !== 404) {
        error(caught instanceof Error ? caught.message : String(caught));
        return json(res, { error: "Could not delete latest telemetry" }, 500);
      }
    }
    return json(res, { accepted: true, deletedTelemetryDeviceId: removedDeviceId });
  }

  const deviceId = eventDeviceId(req);
  if (!deviceId || body.event !== "message.publish") {
    return json(res, { accepted: false, reason: "not_mqtt_publish_event" }, 202);
  }

  const topic = typeof body.topic === "string" ? body.topic.trim() : "";
  if (topic.includes("/system/") || topic.endsWith("/telemetry/status")) {
    return json(res, { accepted: false, reason: "system_status_owned_by_devices" }, 202);
  }
  const route = topicParts(topic);
  let loggedPayload = body.payload ?? null;
  if (route?.channel.startsWith("blocks/") && typeof loggedPayload === "string") {
    loggedPayload = {
      encoding: body.payload_encoding ?? null,
      encodedCharacters: loggedPayload.length,
      decodedBytes: body.payload_encoding === "base64" ? Buffer.from(loggedPayload, "base64").length : null,
    };
  } else if (typeof loggedPayload === "string") {
    try { loggedPayload = JSON.parse(loggedPayload); } catch { /* Log the invalid string below. */ }
  }
  log(JSON.stringify({ mqttDeviceId: deviceId, topic, payload: loggedPayload }));
  if (!route || route.projectId !== process.env.APPWRITE_FUNCTION_PROJECT_ID) {
    return json(res, { error: "MQTT topic does not match this Appwrite project" }, 400);
  }

  const sensorBlockMac = blockTopicMac(route.channel);
  if (route.channel.startsWith("blocks/")) {
    if (!sensorBlockMac) return json(res, { error: "Sensor block topic needs a valid HaLow MAC" }, 400);
    try {
      const gateway = await getDevice(req, deviceId);
      if (!gateway || gateway.serial !== route.serial) {
        return json(res, { error: "MQTT topic serial does not match the Appwrite device" }, 403);
      }
      const target = await getDeviceByHalowMac(req, sensorBlockMac);
      if (!target || !gatewayCanRelayForTarget(gateway, target)) {
        return json(res, { error: "Sensor block source does not share this gateway's access scope" }, 403);
      }
      const block = decodeSensorBlock(body.payload, body.payload_encoding);
      const receivedAt = new Date().toISOString();
      await writeTimeseries(req, blockLines(target, block, receivedAt));
      return json(res, {
        accepted: true,
        deviceId: target.$id,
        blockSequence: block.blockSequence,
        sampleCount: block.sampleCount,
        receivedAt,
      }, 201);
    } catch (caught) {
      const message = caught instanceof Error ? caught.message : String(caught);
      if (message.startsWith("Sensor block")) return json(res, { error: message }, 400);
      error(message);
      return json(res, { error: "Could not persist sensor block" }, 500);
    }
  }

  let payload;
  try {
    payload = typeof body.payload === "string" ? JSON.parse(body.payload) : body.payload;
  } catch {
    return json(res, { error: "MQTT payload must be valid JSON" }, 400);
  }
  const entries = Array.isArray(payload) ? payload : [payload];
  if (!entries.length || entries.length > 16 || entries.some((entry) =>
      !entry || typeof entry !== "object" || Array.isArray(entry))) {
    return json(res, { error: "MQTT payload must contain 1 to 16 telemetry objects" }, 400);
  }
  if (JSON.stringify(payload).length > 10000) {
    return json(res, { error: "MQTT payload is too large" }, 413);
  }
  for (const entry of entries) {
    if (Array.isArray(payload) &&
        (typeof entry.clientId !== "string" ||
         !/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i.test(entry.clientId))) {
      return json(res, { error: "Each telemetry entry needs a device clientId" }, 400);
    }
    if (entry.clientId !== undefined && typeof entry.clientId !== "string") {
      return json(res, { error: "Telemetry clientId must be a string" }, 400);
    }
    // Persist application fields only, even if an older sensor publisher includes system fields.
    for (const field of ["status", "halowMac", "topology", "halowRf", "gateway_status", "wifi_enabled"]) delete entry[field];
    if (!entry.ota) delete entry.firmwareVersion;
    if (entry.sensors !== undefined && (!Array.isArray(entry.sensors) || entry.sensors.length > 13 || entry.sensors.some((sensor) =>
        !sensor || typeof sensor !== "object" || !Number.isInteger(sensor.type) || sensor.type < 1 || sensor.type > SENSOR_TYPE_MAX ||
        typeof sensor.value !== "number" || !Number.isFinite(sensor.value)))) {
      return json(res, { error: "Telemetry sensors must contain supported numeric type and value fields" }, 400);
    }
    if (entry.downlink !== undefined && !validDownlinkStatus(entry.downlink)) {
      return json(res, { error: "Downlink status is invalid" }, 400);
    }
    if (route.channel === "ota" && !otaStatus(entry)) {
      return json(res, { error: "OTA telemetry must contain a valid OTA status" }, 400);
    }
    const sensorTypes = new Set((entry.sensors || []).map((sensor) => sensor.type));
    for (const [name, axes] of [["accelerometer", SENSOR_ACCELEROMETER], ["gyroscope", SENSOR_GYROSCOPE]]) {
      if (axes.some((type) => sensorTypes.has(type)) && !axes.every((type) => sensorTypes.has(type))) {
        return json(res, { error: `IMU ${name} data must include X, Y, and Z axes` }, 400);
      }
    }
    const batteryVoltage = sensorValue(entry, SENSOR_BATTERY_VOLTAGE);
    if ((route.channel === "sensors" || route.channel === "battery") && batteryVoltage !== null &&
        (batteryVoltage < 0 || batteryVoltage > 10.0)) {
      return json(res, { error: "Battery sensor voltage must be between 0 and 10.0 volts" }, 400);
    }
    const mcuTemperature = sensorValue(entry, SENSOR_MCU_TEMPERATURE);
    if (mcuTemperature !== null && (mcuTemperature < -40 || mcuTemperature > 125)) {
      return json(res, { error: "MCU temperature must be between -40 and 125 degrees Celsius" }, 400);
    }
    const latitude = sensorValue(entry, SENSOR_LATITUDE);
    const longitude = sensorValue(entry, SENSOR_LONGITUDE);
    if ((latitude === null) !== (longitude === null) ||
        (latitude !== null && (latitude < -90 || latitude > 90 || longitude < -180 || longitude > 180))) {
      return json(res, { error: "Location sensors must include valid latitude and longitude" }, 400);
    }
  }

  const client = new Client()
    .setEndpoint(process.env.APPWRITE_FUNCTION_API_ENDPOINT)
    .setProject(process.env.APPWRITE_FUNCTION_PROJECT_ID)
    .setKey(req.headers["x-appwrite-key"]);
  const tables = new TablesDB(client);

  try {
    const device = await getDevice(req, deviceId);
    if (!device || device.serial !== route.serial) {
      return json(res, { error: "MQTT topic serial does not match the Appwrite device" }, 403);
    }
    const targets = [];
    for (const entry of entries) {
      const targetId = entry.clientId || deviceId;
      const target = targetId === deviceId ? device : await getDevice(req, targetId);
      if (!target || target.$id !== targetId) {
        return json(res, { error: "Telemetry clientId does not match an Appwrite device" }, 403);
      }
      if (!gatewayCanRelayForTarget(device, target)) {
        return json(res, { error: "Remote device does not share this gateway's access scope" }, 403);
      }
      const readPermissions = (target.$permissions || []).filter((permission) => permission.startsWith("read("));
      if (!readPermissions.length) {
        return json(res, { error: "Appwrite device has no owner read permission" }, 409);
      }
      targets.push({ entry, target, readPermissions });
    }

    const receivedAt = new Date().toISOString();
    await writeTimeseries(req, targets.map(({ entry, target }, index) =>
      telemetryLine(target, entry, route.channel, receivedAt, index)));
    const telemetryIds = [];
    for (const { entry, target, readPermissions } of targets) {
      const location = locationOf(entry);
      if (location) await updateDeviceLocation(req, target.$id, location);
      const row = await upsertLatestTelemetry(tables, target, {
        deviceId: target.$id,
        profileId: target.metadata.profileId,
        gatewayDeviceId: target.$id === device.$id ? null : device.$id,
        serial: target.serial,
        channel: route.channel,
        topic,
        payload: JSON.stringify(entry),
        receivedAt,
        ...(location ? { location: [location.longitude, location.latitude] } : {}),
        ...mapAppearanceOf(target),
      }, readPermissions);
      await evaluateGeofences(tables, target, entry, readPermissions, receivedAt);
      await syncOtaUpdate(tables, target, entry, readPermissions, receivedAt);
      await reconcileOtaUpdate(tables, target, { firmwareVersion: target.firmwareVersion }, readPermissions, receivedAt);
      if (entry.downlink) await syncDownlink(tables, device, target, entry.downlink, readPermissions, receivedAt);
      telemetryIds.push(row.$id || target.$id);
    }
    return json(res, { accepted: true, telemetryId: telemetryIds[0], telemetryIds, receivedAt }, 201);
  } catch (caught) {
    error(caught instanceof Error ? caught.message : String(caught));
    return json(res, { error: "Could not persist MQTT telemetry" }, 500);
  }
}
