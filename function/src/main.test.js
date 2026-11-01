import assert from "node:assert/strict";
import { after, beforeEach, test } from "node:test";
import { TablesDB } from "node-appwrite";

process.env.APPWRITE_FUNCTION_API_ENDPOINT = "https://appwrite.example/v1";
process.env.APPWRITE_FUNCTION_PROJECT_ID = "project-a";
process.env.OFFGRID_TRACKER_DATABASE_ID = "database-a";
process.env.OFFGRID_TRACKER_TELEMETRY_TABLE_ID = "telemetry-a";
process.env.OFFGRID_TRACKER_OTA_UPDATE_TABLE_ID = "ota-a";
process.env.OFFGRID_TRACKER_DOWNLINK_TABLE_ID = "downlink-a";
process.env.OFFGRID_TRACKER_REPOSITORY_URL = "https://github.example/acme/offgrid-tracker";
const { default: main } = await import("./main.js");

const gatewayId = "11111111-1111-4111-8111-111111111111";
const remoteId = "22222222-2222-4222-8222-222222222222";
const devices = new Map([
  [gatewayId, { $id: gatewayId, serial: "AABBCCDDEEFF", markerType: "gateway", markerColor: "blue", metadata: { profileId: "profile-a" },
    $permissions: ['read("team:profile-a")'] }],
  [remoteId, { $id: remoteId, serial: "112233445566", halowMac: "02:11:22:33:44:55", markerType: "tracker", markerColor: "orange", gatewayDeviceId: gatewayId, metadata: { profileId: "profile-a" },
    $permissions: ['read("team:profile-a")'] }],
]);
const originalFetch = globalThis.fetch;
const originalCreateRow = TablesDB.prototype.createRow;
const originalDeleteRow = TablesDB.prototype.deleteRow;
const originalListRows = TablesDB.prototype.listRows;
const originalUpdateRow = TablesDB.prototype.updateRow;
const rows = [];
const otaRows = [];
const downlinkRows = [];
const timeseriesWrites = [];
const deviceLocationUpdates = [];

beforeEach(() => {
  delete devices.get(gatewayId).firmwareVersion;
  delete devices.get(gatewayId).location;
  delete devices.get(remoteId).location;
  devices.get(remoteId).gatewayDeviceId = gatewayId;
  devices.get(gatewayId).$permissions = ['read("team:profile-a")'];
  devices.get(remoteId).$permissions = ['read("team:profile-a")'];
  rows.length = 0;
  otaRows.length = 0;
  downlinkRows.length = 0;
  timeseriesWrites.length = 0;
  deviceLocationUpdates.length = 0;
  for (const deviceId of [...devices.keys()]) {
    if (deviceId !== gatewayId && deviceId !== remoteId) devices.delete(deviceId);
  }
  process.env.APPWRITE_FUNCTION_API_ENDPOINT = "https://appwrite.example/v1";
  process.env.APPWRITE_FUNCTION_PROJECT_ID = "project-a";
  process.env.OFFGRID_TRACKER_DATABASE_ID = "database-a";
  process.env.OFFGRID_TRACKER_TELEMETRY_TABLE_ID = "telemetry-a";
  process.env.OFFGRID_TRACKER_OTA_UPDATE_TABLE_ID = "ota-a";
  devices.get(gatewayId).metadata = { profileId: "profile-a", icon: "gateway", markerColor: "blue" };
  devices.get(remoteId).metadata = { profileId: "profile-a", icon: "tracker", markerColor: "orange" };
  globalThis.fetch = async (url, options = {}) => {
    const href = String(url);
    if (href === "https://github.example/acme/offgrid-tracker/releases/latest") {
      return { ok: true, status: 200, url: "https://github.example/acme/offgrid-tracker/releases/tag/v0.0.4" };
    }
    if (href === "https://appwrite.example/v1/timeseries/stores/project-a/points") {
      timeseriesWrites.push(JSON.parse(options.body));
      return { ok: true, status: 201 };
    }
    const parsed = new URL(href);
    if (parsed.pathname === "/v1/devices" && options.method === "GET") {
      const query = parsed.searchParams.get("queries[]");
      const filter = query ? JSON.parse(query) : null;
      return { ok: true, status: 200, json: async () => ({
        devices: [...devices.values()].filter((device) => !filter || device[filter.attribute] === filter.values[0]),
      }) };
    }
    if (parsed.pathname === "/v1/devices" && options.method === "POST") {
      const data = JSON.parse(options.body);
      const device = { $id: `cloud-${devices.size + 1}`, serial: data.serial, ...data };
      devices.set(device.$id, device);
      return { ok: true, status: 201, json: async () => device };
    }
    const device = devices.get(decodeURIComponent(href.split("/").pop()));
    if (options.method === "PATCH" && device) {
      const update = JSON.parse(options.body);
      Object.assign(device, update);
      deviceLocationUpdates.push({ deviceId: device.$id, ...update });
    }
    return { ok: Boolean(device), status: device ? 200 : 404, json: async () => device };
  };
  TablesDB.prototype.createRow = async (args) => {
    if (args.tableId === "ota-a") {
      const row = { $id: `ota-${otaRows.length + 1}`, ...args.data };
      otaRows.push(row);
      return row;
    }
    if (args.tableId === "downlink-a") {
      const row = { $id: `downlink-${downlinkRows.length + 1}`, ...args.data };
      downlinkRows.push(row);
      return row;
    }
    const row = { ...args, $id: args.rowId };
    rows.push(row);
    return row;
  };
  TablesDB.prototype.listRows = async (args) => {
    if (args.tableId === "telemetry-a") {
      const equal = (args.queries || []).map((query) => JSON.parse(query))
        .find((query) => query.method === "equal" && query.attribute === "halowMac");
      return { rows: equal ? rows.filter((row) => row.data.halowMac === equal.values[0]).map((row) => ({ $id: row.$id, ...row.data })) : [] };
    }
    return { rows: args.tableId === "ota-a"
      ? otaRows
      : args.tableId === "downlink-a" ? downlinkRows : [] };
  };
  TablesDB.prototype.deleteRow = async (args) => {
    const index = rows.findIndex((candidate) => candidate.$id === args.rowId);
    if (index < 0) throw Object.assign(new Error("not found"), { code: 404 });
    rows.splice(index, 1);
    return {};
  };
  TablesDB.prototype.updateRow = async (args) => {
    if (args.tableId === "telemetry-a") {
      const telemetry = rows.find((candidate) => candidate.$id === args.rowId);
      if (!telemetry) throw Object.assign(new Error("not found"), { code: 404 });
      Object.assign(telemetry.data, args.data);
      telemetry.permissions = args.permissions;
      return telemetry;
    }
    const ota = otaRows.find((candidate) => candidate.$id === args.rowId);
    if (ota) Object.assign(ota, args.data);
    const downlink = downlinkRows.find((candidate) => candidate.$id === args.rowId);
    if (downlink) Object.assign(downlink, args.data);
    return ota || downlink;
  };
});

after(() => {
  globalThis.fetch = originalFetch;
  TablesDB.prototype.createRow = originalCreateRow;
  TablesDB.prototype.deleteRow = originalDeleteRow;
  TablesDB.prototype.listRows = originalListRows;
  TablesDB.prototype.updateRow = originalUpdateRow;
});

async function publish(payload) {
  const withHalowMac = (entry) => ({
    halowMac: entry.clientId === remoteId ? "02:11:22:33:44:55" : "0C:BF:74:1A:AE:36",
    ...entry,
  });
  const report = Array.isArray(payload) ? payload.map(withHalowMac) : withHalowMac(payload);
  const req = {
    headers: {
      "x-appwrite-event": `devices.${gatewayId}.mqtt.message.publish`,
      "x-appwrite-key": "test-key",
    },
    bodyJson: {
      event: "message.publish",
      topic: "projects/project-a/devices/AABBCCDDEEFF/telemetry/sensors",
      payload: JSON.stringify(report),
    },
  };
  return main({ req, res: { json: (body, status) => ({ body, status }) }, error: (message) => {
    throw new Error(message);
  } });
}

async function deleteDevice(deviceId) {
  return main({
    req: { headers: {
      "x-appwrite-event": `devices.${deviceId}.delete`,
      "x-appwrite-key": "test-key",
    } },
    res: { json: (body, status = 200) => ({ body, status }) },
    error: (message) => { throw new Error(message); },
  });
}

async function publishImuBlock(samples, { mac = "02:11:22:33:44:55", sequence = 7, intervalMs = 15000 } = {}) {
  const bytes = Buffer.alloc(24 + samples.length * 25);
  bytes.write("EZSB", 0, "ascii");
  bytes[4] = 1;
  bytes[5] = 2;
  bytes[6] = 1;
  bytes[7] = 24;
  bytes.writeUInt32LE(intervalMs, 8);
  bytes.writeUInt16LE(samples.length, 12);
  bytes.writeUInt16LE(samples.length, 14);
  bytes.writeUInt32LE(sequence, 16);
  bytes.writeUInt16LE(25, 20);
  samples.forEach((sample, index) => {
    const offset = 24 + index * 25;
    [...sample.accel, ...sample.gyro].forEach((value, field) => bytes.writeFloatLE(value, offset + field * 4));
    bytes[offset + 24] = sample.gyroValid ? 1 : 0;
  });
  return main({
    req: {
      headers: {
        "x-appwrite-event": `devices.${gatewayId}.mqtt.message.publish`,
        "x-appwrite-key": "test-key",
      },
      bodyJson: {
        event: "message.publish",
        topic: `projects/project-a/devices/AABBCCDDEEFF/telemetry/blocks/${mac}`,
        payload: bytes.toString("base64"),
        payload_encoding: "base64",
      },
    },
    res: { json: (body, status) => ({ body, status }) },
    error: (message) => { throw new Error(message); },
  });
}

test("one MQTT batch saves each clientId under its own device and permissions", async () => {
  const result = await publish([
    { clientId: gatewayId, sensors: [{ type: 12, value: 3.9 }] },
    { clientId: remoteId, sensors: [{ type: 12, value: 3.7 }, { type: 3, value: 59.3 }, { type: 4, value: 18.0 }] },
  ]);
  assert.equal(result.status, 201);
  assert.equal(rows.length, 2);
  assert.deepEqual(rows.map(({ data }) => [data.deviceId, data.serial]),
    [[gatewayId, "AABBCCDDEEFF"], [remoteId, "112233445566"]]);
  assert.equal(rows[0].data.gatewayDeviceId, null);
  assert.equal(rows[1].data.gatewayDeviceId, gatewayId);
  assert.equal(JSON.parse(rows[1].data.payload).sensors[1].value, 59.3);
  assert.deepEqual(rows[1].data.location, [18, 59.3]);
  assert.deepEqual(deviceLocationUpdates, [{ deviceId: remoteId, location: [18, 59.3] }]);
  assert.deepEqual({ icon: rows[1].data.icon, markerColor: rows[1].data.markerColor },
    { icon: "tracker", markerColor: "orange" });
  assert.deepEqual(rows[1].permissions, devices.get(remoteId).$permissions);
  assert.equal(timeseriesWrites.length, 1);
  assert.match(timeseriesWrites[0].data, new RegExp(`device_${remoteId}.*sensor_battery_voltage=3\\.7`));
  assert.match(timeseriesWrites[0].data, /lat=59\.3,lon=18/);
  assert.doesNotMatch(timeseriesWrites[0].data, /sensor_(3|4)=/);
  assert.doesNotMatch(timeseriesWrites[0].data, /sensor_gps=/);
});

test("a mesh gateway can relay a remote device in the same access scope", async () => {
  devices.get(remoteId).gatewayDeviceId = "33333333-3333-4333-8333-333333333333";
  const result = await publish([{ clientId: gatewayId }, { clientId: remoteId }]);
  assert.equal(result.status, 201);
  assert.equal(rows.length, 2);
});

test("a gateway cannot relay a remote device outside its access scope", async () => {
  devices.get(remoteId).gatewayDeviceId = "33333333-3333-4333-8333-333333333333";
  devices.get(remoteId).$permissions = ['read("team:another-profile")'];
  const result = await publish([{ clientId: gatewayId }, { clientId: remoteId }]);
  assert.equal(result.status, 403);
  assert.equal(rows.length, 0);
});

test("multiple queued readings remain in time series while TablesDB keeps one latest row per device", async () => {
  const result = await publish([
    { clientId: gatewayId, status: "online" },
    { clientId: remoteId, sensors: [{ type: 12, value: 3.7 }] },
    { clientId: remoteId, sensors: [{ type: 12, value: 3.65 }] },
  ]);
  assert.equal(result.status, 201);
  assert.equal(rows.length, 2);
  assert.deepEqual(rows.map(({ data }) => data.deviceId), [gatewayId, remoteId]);
  assert.equal(JSON.parse(rows[1].data.payload).sensors[0].value, 3.65);
  assert.match(timeseriesWrites[0].data, /sensor_battery_voltage=3\.7/);
  assert.match(timeseriesWrites[0].data, /sensor_battery_voltage=3\.65/);
});

test("single-device telemetry remains accepted", async () => {
  const result = await publish({ status: "online", sensors: [{ type: 12, value: 3.8 }] });
  assert.equal(result.status, 201);
  assert.equal(rows.length, 1);
  assert.equal(rows[0].data.deviceId, gatewayId);
  assert.equal(rows[0].data.gatewayDeviceId, null);
});

test("battery voltage accepts the inclusive 0 to 10 volt range", async () => {
  let result = await publish({ status: "online", sensors: [{ type: 12, value: 0 }] });
  assert.equal(result.status, 201);
  result = await publish({ status: "online", sensors: [{ type: 12, value: 10 }] });
  assert.equal(result.status, 201);
});

test("battery voltage rejects values outside 0 to 10 volts", async () => {
  let result = await publish({ status: "online", sensors: [{ type: 12, value: -0.01 }] });
  assert.equal(result.status, 400);
  result = await publish({ status: "online", sensors: [{ type: 12, value: 10.01 }] });
  assert.equal(result.status, 400);
});

test("sensor indexes become named fields and GPS remains numeric", async () => {
  const result = await publish({ sensors: [
    { type: 1, value: 21.5 }, { type: 2, value: 61 },
    { type: 3, value: 59.3293 }, { type: 4, value: 18.0686 },
    { type: 5, value: 2.4 },
    { type: 6, value: 0.1 }, { type: 7, value: 0.2 }, { type: 8, value: 9.8 },
    { type: 9, value: 0.01 }, { type: 10, value: 0.02 }, { type: 11, value: 0.03 },
    { type: 12, value: 3.8 },
    { type: 13, value: 48.25 },
  ] });
  assert.equal(result.status, 201);
  const line = timeseriesWrites[0].data;
  for (const field of ["temperature", "humidity", "length", "accel_x", "accel_y", "accel_z",
    "gyro_x", "gyro_y", "gyro_z", "battery_voltage", "mcu_temperature"]) {
    assert.match(line, new RegExp(`sensor_${field}=`));
  }
  assert.match(line, /lat=59\.3293,lon=18\.0686/);
  assert.doesNotMatch(line, /sensor_\d+=|sensor_gps=/);
});

test("MCU temperature accepts the supported ESP32-S3 range", async () => {
  let result = await publish({ sensors: [{ type: 13, value: -40 }] });
  assert.equal(result.status, 201);
  result = await publish({ sensors: [{ type: 13, value: 125 }] });
  assert.equal(result.status, 201);
  assert.match(timeseriesWrites.at(-1).data, /sensor_mcu_temperature=125/);
});

test("MCU temperature rejects values outside the supported range", async () => {
  let result = await publish({ sensors: [{ type: 13, value: -40.01 }] });
  assert.equal(result.status, 400);
  result = await publish({ sensors: [{ type: 13, value: 125.01 }] });
  assert.equal(result.status, 400);
});


test("complete IMU readings are stored with unified telemetry", async () => {
  const result = await publish({ status: "online", sensors: [
    { type: 6, value: 0.12 }, { type: 7, value: -0.34 }, { type: 8, value: 9.81 },
    { type: 9, value: 0.01 }, { type: 10, value: 0.02 }, { type: 11, value: -0.03 },
  ] });
  assert.equal(result.status, 201);
  assert.deepEqual(JSON.parse(rows[0].data.payload).sensors.map((sensor) => sensor.type), [6, 7, 8, 9, 10, 11]);
});

test("nRF54L15 binary IMU blocks are expanded into time-series points", async () => {
  const result = await publishImuBlock([
    { accel: [0.1, -0.2, 9.8], gyro: [0.01, 0.02, -0.03], gyroValid: true },
    { accel: [0.4, -0.5, 9.7], gyro: [0.04, 0.05, -0.06], gyroValid: false },
  ]);
  assert.equal(result.status, 201);
  assert.equal(result.body.deviceId, remoteId);
  assert.equal(result.body.blockSequence, 7);
  assert.equal(result.body.sampleCount, 2);
  assert.equal(timeseriesWrites.length, 1);
  const lines = timeseriesWrites[0].data.split("\n");
  assert.equal(lines.length, 2);
  assert.match(lines[0], new RegExp(`device_${remoteId},serial=112233445566,channel=blocks,sample_kind=imu,block_sequence=7,sample_index=0`));
  assert.match(lines[0], /sensor_accel_x=0\.10000000149011612/);
  assert.match(lines[0], /gyro_valid=true/);
  assert.match(lines[1], /sample_index=1 .*gyro_valid=false/);
  const firstTimestamp = BigInt(lines[0].split(" ").at(-1));
  const secondTimestamp = BigInt(lines[1].split(" ").at(-1));
  assert.equal(secondTimestamp - firstTimestamp, 15000000000n);
  assert.equal(rows.length, 0, "blocks belong in history and must not replace latest beacon telemetry");
});

test("sensor blocks are rejected when the source is not assigned to the publishing gateway", async () => {
  devices.get(remoteId).gatewayDeviceId = "33333333-3333-4333-8333-333333333333";
  devices.get(remoteId).$permissions = ['read("team:another-profile")'];
  const result = await publishImuBlock([
    { accel: [0, 0, 9.8], gyro: [0, 0, 0], gyroValid: true },
  ]);
  assert.equal(result.status, 403);
  assert.equal(timeseriesWrites.length, 0);
});

test("partial IMU readings are rejected", async () => {
  const result = await publish({ status: "online", sensors: [{ type: 6, value: 0.12 }] });
  assert.equal(result.status, 400);
  assert.match(result.body.error, /IMU accelerometer/);
  assert.equal(rows.length, 0);
});

test("OTA telemetry creates and completes one status row", async () => {
  const requestId = "33333333-3333-4333-8333-333333333333";
  const accepted = await publish({ clientId: gatewayId, firmwareVersion: "v0.0.3", ota: { requestId, status: "pending", detail: "accepted" } });
  assert.equal(accepted.status, 201);
  assert.equal(otaRows.length, 1);
  assert.deepEqual(otaRows[0].status, "pending");
  assert.equal(otaRows[0].targetFirmwareVersion, "v0.0.4");

  const completed = await publish({ clientId: gatewayId, firmwareVersion: "v0.0.4", ota: { requestId, status: "succeeded", detail: "installed" } });
  assert.equal(completed.status, 201);
  assert.equal(otaRows.length, 1);
  assert.equal(otaRows[0].status, "succeeded");
  assert.ok(otaRows[0].completedAt);
});

test("ordinary telemetry completes a pending OTA when the target version is running", async () => {
  const requestId = "44444444-4444-4444-8444-444444444444";
  await publish({ clientId: gatewayId, firmwareVersion: "v0.0.3", ota: { requestId, status: "pending", detail: "downloading" } });
  assert.equal(otaRows[0].status, "pending");

  devices.get(gatewayId).firmwareVersion = "0.0.4";
  const reconciled = await publish({ clientId: gatewayId, firmwareVersion: "0.0.4", status: "online" });
  assert.equal(reconciled.status, 201);
  assert.equal(otaRows[0].status, "succeeded");
  assert.equal(otaRows[0].detail, "running target firmware");
  assert.equal(otaRows[0].firmwareVersion, "0.0.4");
  assert.ok(otaRows[0].completedAt);
});

test("sensor ingestion does not persist system state or write topology", async () => {
  const result = await publish({ status: "online", firmwareVersion: "v1", halowRf: { available: true },
    topology: { links: [] }, gateway_status: "online", wifi_enabled: true,
    sensors: [{ type: 1, value: 21 }] });
  assert.equal(result.status, 201);
  assert.equal(rows[0].data.halowMac, undefined);
  assert.equal(rows[0].data.status, undefined);
  assert.deepEqual(JSON.parse(rows[0].data.payload), { sensors: [{ type: 1, value: 21 }] });
  assert.doesNotMatch(timeseriesWrites[0].data, /halow|firmwareVersion|topology|status/);
});

test("system and retired status topics never enter application ingestion", async () => {
  for (const suffix of ["system/status", "telemetry/status"]) {
    const result = await main({ req: {
      headers: { "x-appwrite-event": `devices.${gatewayId}.mqtt.message.publish`, "x-appwrite-key": "test-key" },
      bodyJson: { event: "message.publish", topic: `projects/project-a/devices/AABBCCDDEEFF/${suffix}`, payload: "{}" },
    }, res: { json: (body, status) => ({ body, status }) }, error: assert.fail });
    assert.equal(result.status, 202);
    assert.equal(result.body.reason, "system_status_owned_by_devices");
  }
  assert.equal(rows.length, 0);
  assert.equal(timeseriesWrites.length, 0);
});

test("device deletion removes its latest telemetry row", async () => {
  rows.push({ $id: gatewayId, data: {
    deviceId: gatewayId, profileId: "profile-a", serial: "AABBCCDDEEFF", halowMac: "0C:BF:74:1A:AE:36",
  } });
  const result = await deleteDevice(gatewayId);
  assert.equal(result.status, 200);
  assert.equal(result.body.deletedTelemetryDeviceId, gatewayId);
  assert.equal(rows.length, 0);
});

test("device deletion is idempotent when latest telemetry is absent", async () => {
  const result = await deleteDevice(gatewayId);
  assert.equal(result.status, 200);
  assert.equal(result.body.accepted, true);
});

test("gateway downlink status is stored against the remote nRF device", async () => {
  let result = await publish({ clientId: remoteId, downlink: {
    requestId: "6ab551810011772b0ac9", revision: 7, status: "cached",
  } });
  assert.equal(result.status, 201);
  assert.equal(downlinkRows.length, 1);
  assert.equal(downlinkRows[0].deviceId, remoteId);
  assert.equal(downlinkRows[0].gatewayDeviceId, gatewayId);

  result = await publish({ clientId: remoteId, downlink: {
    requestId: "6ab551810011772b0ac9", revision: 7, status: "applied",
  } });
  assert.equal(result.status, 201);
  assert.equal(downlinkRows.length, 1);
  assert.equal(downlinkRows[0].status, "applied");
  assert.ok(downlinkRows[0].completedAt);
});
