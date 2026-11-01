import AsyncStorage from "@react-native-async-storage/async-storage";
import Constants from "expo-constants";
import { Account, Client, ID, Models, Permission, Query, Role, TablesDB, Teams } from "react-native-appwrite";
import type { EdgezSensorSample } from "@edgez/react-native-sdk";
import type { StoredProfile } from "./profile-store";
import type { ProvisioningRequestDetails, StoredDevice, StoredProvisioningRequest } from "./profile-store";
import { provisioningRequestFromPacket } from "./halow-provisioning";
import { nodeNumFromSerial } from "./mesh-utils";
import { fromByteArray, toByteArray } from "base64-js";

export type CloudConnectionState = "login" | "offline" | "online";

type CloudConfig = {
  appwriteEndpoint?: string;
  appwriteProjectId?: string;
  appwritePlatform?: string;
  appwriteDatabaseId?: string;
  appwriteProfileTableId?: string;
  appwriteTelemetryTableId?: string;
};

const extra = (Constants.expoConfig?.extra ?? {}) as CloudConfig;
const endpoint = (extra.appwriteEndpoint ?? "").replace(/\/+$/, "");
const projectId = (extra.appwriteProjectId ?? "").trim();
const platform = (extra.appwritePlatform ?? "").trim();
const databaseId = (extra.appwriteDatabaseId ?? "offgrid-tracker").trim();
const profileTableId = (extra.appwriteProfileTableId ?? "profiles").trim();
const telemetryTableId = (extra.appwriteTelemetryTableId ?? "telemetry").trim();

export const cloudAuthConfigured = Boolean(
  endpoint && projectId && projectId !== "<PROJECT_ID>" && platform,
);

const client = cloudAuthConfigured
  ? new Client().setEndpoint(endpoint).setProject(projectId).setPlatform(platform)
  : null;
const account = client ? new Account(client) : null;
const teams = client ? new Teams(client) : null;
const tables = client ? new TablesDB(client) : null;
const sessionCacheKey = `offgrid-tracker:cloud-session:${projectId || "unconfigured"}`;

type NativeDevice = {
  $id: string;
  $createdAt?: string;
  $updatedAt?: string;
  $permissions?: string[];
  serial: string;
  name?: string;
  status: string;
  enabled: boolean;
  halowMac?: string | null;
  gatewayDeviceId?: string | null;
  gatewayStatus?: string | null;
  lastSeenAt?: string | null;
  location?: [number, number] | null;
  markerType?: string | null;
  markerColor?: string | null;
  metadata?: {
    profileId?: string;
    firmwareTarget?: string;
    mqttGateway?: boolean;
    deviceType?: string;
    iconStyle?: string;
    userUuid?: string;
    userPublicKey?: string;
    _edgezDiscovery?: {
      publicKey?: string;
      requestPayload?: string;
      firstSeenAt?: string;
      lastSeenAt?: string;
      acceptedAt?: string;
    };
  };
};

type CloudProfile = Models.Row & {
  name: string;
  teamId?: string;
};

export type PreparedDeviceCredential = {
  deviceId: string;
  clientId: string;
  username: string;
  password: string;
  projectId: string;
  channel: string;
};

export type CloudDeviceUpdate = {
  name: string;
  markerType: string;
  markerColor: string;
  latitude: number | null;
  longitude: number | null;
};

export type CloudTokenRequest = {
  userId: string;
  phrase: string;
};

export function isCloudAuthError(error: unknown) {
  const value = error as { code?: unknown; status?: unknown } | null;
  return value?.code === 401 || value?.status === 401;
}

function isCloudNotFound(error: unknown) {
  const value = error as { code?: unknown; status?: unknown } | null;
  return value?.code === 404 || value?.status === 404;
}

function rowPermissions(profileId: string) {
  return [
    Permission.read(Role.team(profileId)),
    Permission.update(Role.team(profileId, "owner")),
    Permission.delete(Role.team(profileId, "owner")),
  ];
}

export function cloudErrorMessage(error: unknown) {
  if (error instanceof Error && error.message) return error.message;
  const message = (error as { message?: unknown } | null)?.message;
  return typeof message === "string" && message ? message : "Cloud login failed.";
}

export async function cachedCloudSession() {
  return cloudAuthConfigured && await AsyncStorage.getItem(sessionCacheKey) === "1";
}

export async function verifyCloudSession() {
  if (!account) return false;
  try {
    await account.get();
    await AsyncStorage.setItem(sessionCacheKey, "1");
    return true;
  } catch (error) {
    if (isCloudAuthError(error)) {
      await AsyncStorage.removeItem(sessionCacheKey);
      return false;
    }
    throw error;
  }
}

async function rememberCloudSession() {
  if (!account) throw new Error("Cloud login is not configured in this app build.");
  await account.get();
  await AsyncStorage.setItem(sessionCacheKey, "1");
}

export async function requestCloudEmailOtp(email: string) {
  if (!account) throw new Error("Cloud login is not configured in this app build.");
  const token = await account.createEmailToken({ userId: ID.unique(), email, phrase: true });
  return { userId: token.userId, phrase: token.phrase } satisfies CloudTokenRequest;
}

export async function completeCloudToken(userId: string, secret: string) {
  if (!account) throw new Error("Cloud login is not configured in this app build.");
  await account.createSession({ userId, secret });
  await rememberCloudSession();
}

export async function logoutCloudSession() {
  if (!account) throw new Error("Cloud login is not configured in this app build.");
  try {
    await account.deleteSession({ sessionId: "current" });
  } catch (error) {
    if (!isCloudAuthError(error)) throw error;
  }
  await AsyncStorage.removeItem(sessionCacheKey);
}

async function devicesRequest<T>(path = "", init?: RequestInit): Promise<T> {
  if (!account) throw new Error("Cloud access is not configured in this app build.");
  const jwt = await account.createJWT();
  const response = await fetch(`${endpoint}/devices${path}`, {
    ...init,
    credentials: "omit",
    headers: {
      "content-type": "application/json",
      "x-appwrite-project": projectId,
      "x-appwrite-jwt": jwt.jwt,
      ...init?.headers,
    },
  });
  const body = await response.text();
  const payload = body ? JSON.parse(body) as T & { message?: string } : undefined;
  if (!response.ok) {
    throw new Error(payload?.message || "Appwrite Devices request failed.");
  }
  return payload as T;
}

export type CloudSensorSettings = {
  samplingIntervalMs: number;
  blockSamplingCount: number;
};

export type CloudSensorSettingsState = {
  settings: CloudSensorSettings;
  status: string;
  retryCount: number;
  updatedAt: string;
} | null;

type NativeDeviceDownlink = {
  $updatedAt?: string;
  command: string;
  payload?: unknown;
  status: string;
  retryCount?: number;
};

function validCloudSensorSettings(value: unknown): value is CloudSensorSettings {
  const settings = value as Partial<CloudSensorSettings> | null;
  return Boolean(settings &&
    Number.isInteger(settings.samplingIntervalMs) &&
    Number(settings.samplingIntervalMs) >= 1_000 &&
    Number(settings.samplingIntervalMs) <= 3_600_000 &&
    Number.isInteger(settings.blockSamplingCount) &&
    Number(settings.blockSamplingCount) >= 0 &&
    Number(settings.blockSamplingCount) <= 32);
}

/** Read the newest requested sensor behavior from the reliable downlink log. */
export async function getCloudSensorSettings(deviceId: string): Promise<CloudSensorSettingsState> {
  const result = await devicesRequest<{ downlinks?: NativeDeviceDownlink[] }>(
    `/${encodeURIComponent(deviceId)}/downlinks?limit=50`,
  );
  const downlink = result.downlinks?.find((candidate) =>
    candidate.command === "sensor.settings" && validCloudSensorSettings(candidate.payload));
  if (!downlink || !validCloudSensorSettings(downlink.payload)) return null;
  return {
    settings: downlink.payload,
    status: downlink.status,
    retryCount: downlink.retryCount ?? 0,
    updatedAt: downlink.$updatedAt ?? "",
  };
}

/** Queue reliable settings for delivery through the sensor's assigned gateway. */
export async function updateCloudSensorSettings(
  deviceId: string,
  settings: CloudSensorSettings,
): Promise<void> {
  if (!validCloudSensorSettings(settings)) {
    throw new Error("Sampling interval must be 1-3600 seconds and block sampling count must be 0-32.");
  }
  await devicesRequest<void>(`/${encodeURIComponent(deviceId)}/commands`, {
    method: "POST",
    body: JSON.stringify({ command: "sensor.settings", payload: settings }),
  });
}

/**
 * Create (or reuse) the cloud Device and rotate its MQTT credential before a
 * local provisioning transport is opened. H7608 must complete this while the
 * phone still has internet, because joining its SoftAP temporarily replaces
 * the phone's normal Wi-Fi connection.
 */
export async function prepareDeviceCredential(
  profileId: string,
  serial: string,
  name: string,
  firmwareTarget: string,
  mqttGateway: boolean,
  markerType: string,
  markerColor: string,
): Promise<PreparedDeviceCredential> {
  const normalizedSerial = serial.trim().toUpperCase();
  if (!normalizedSerial) throw new Error("The device did not advertise a serial number.");
  const query = `queries[]=${encodeURIComponent(Query.equal("serial", [normalizedSerial]))}`;
  const listed = await devicesRequest<{ devices: NativeDevice[] }>(`?${query}`);
  let device = listed.devices?.[0];
  if (device?.metadata?.profileId && device.metadata.profileId !== profileId) {
    throw new Error("This device is already assigned to another profile.");
  }
  if (!device) {
    device = await devicesRequest<NativeDevice>("", {
      method: "POST",
      body: JSON.stringify({
        serial: normalizedSerial,
        name: name.trim() || normalizedSerial,
        enabled: true,
        permissions: rowPermissions(profileId),
        metadata: { profileId, firmwareTarget, mqttGateway },
        markerType,
        markerColor,
      }),
    });
  } else {
    device = await devicesRequest<NativeDevice>(`/${encodeURIComponent(device.$id)}`, {
      method: "PATCH",
      body: JSON.stringify({
        name: name.trim() || normalizedSerial,
        markerType,
        markerColor,
      }),
    });
  }
  const credential = await devicesRequest<{
    clientId: string;
    username: string;
    password: string;
  }>(`/${encodeURIComponent(device.$id)}/credentials`, {
    method: "POST",
    body: JSON.stringify({}),
  });
  if (credential.username.toUpperCase() !== normalizedSerial) {
    throw new Error("The server returned an MQTT username that does not match the device serial.");
  }
  return {
    deviceId: device.$id,
    clientId: credential.clientId,
    username: credential.username,
    password: credential.password,
    projectId,
    channel: "status",
  };
}

/** Return cloud discoveries routed through a gateway assigned to this profile. */
function cloudDiscoveredDevices(
  profileId: string,
  devices: NativeDevice[],
): StoredProvisioningRequest[] {
  const byId = new Map(devices.map((device) => [device.$id, device]));
  const requests: StoredProvisioningRequest[] = [];
  for (const device of devices) {
    if (device.status !== "discovered" || !device.gatewayDeviceId) continue;
    const gateway = byId.get(device.gatewayDeviceId);
    if (!gateway) continue;
    const profilePermission = `team:${profileId}`;
    const belongsToProfile = gateway.metadata?.profileId === profileId ||
      [...device.$permissions ?? [], ...gateway.$permissions ?? []]
        .some((permission) => permission.includes(profilePermission));
    if (!belongsToProfile) continue;
    const discovery = device.metadata?._edgezDiscovery;
    if (!discovery?.requestPayload) continue;
    let parsed: ProvisioningRequestDetails | null = null;
    try {
      parsed = provisioningRequestFromPacket(profileId, toByteArray(discovery.requestPayload));
    } catch {
      // Ignore a malformed server record without hiding other valid discoveries.
    }
    if (!parsed) continue;
    const gatewayNodeNum = nodeNumFromSerial(gateway.halowMac || gateway.serial)?.toString() ?? "";
    requests.push({
      ...parsed,
      id: device.$id,
      status: device.enabled ? "accepted" : "pending",
      firstSeenAt: discovery.firstSeenAt || parsed.lastSeenAt,
      lastSeenAt: discovery.lastSeenAt || parsed.lastSeenAt,
      acceptedAt: discovery.acceptedAt || "",
      transport: "mqtt",
      cloudDeviceId: device.$id,
      gatewayNodeNum,
    });
  }
  return requests;
}

function cloudDeviceBelongsToProfile(
  profileId: string,
  device: NativeDevice,
  byId: ReadonlyMap<string, NativeDevice>,
): boolean {
  const gateway = device.gatewayDeviceId ? byId.get(device.gatewayDeviceId) : device;
  const profilePermission = `team:${profileId}`;
  return device.metadata?.profileId === profileId ||
    gateway?.metadata?.profileId === profileId ||
    [...device.$permissions ?? [], ...gateway?.$permissions ?? []]
      .some((permission) => permission.includes(profilePermission));
}

/** Publish an encrypted acceptance through the gateway's system MQTT topic. */
export async function acceptCloudProvisioningRequest(
  deviceId: string,
  packet: Uint8Array,
  markerType: string,
  markerColor: string,
): Promise<void> {
  if (!deviceId) throw new Error("The cloud discovery record is missing.");
  await devicesRequest(`/${encodeURIComponent(deviceId)}/discovery`, {
    method: "PATCH",
    body: JSON.stringify({ payload: fromByteArray(packet), markerType, markerColor }),
  });
}

export async function ensureCloudProfile(profile: StoredProfile): Promise<string> {
  if (!account || !teams || !tables) throw new Error("Cloud access is not configured in this app build.");
  const user = await account.get();

  const normalizedName = profile.name.trim();
  const matching = await tables.listRows<CloudProfile>({
    databaseId,
    tableId: profileTableId,
    queries: [Query.equal("name", [normalizedName]), Query.limit(100)],
  });
  const existingByName = matching.rows
    .filter((candidate) => candidate.name.trim() === normalizedName)
    .sort((left, right) => left.$createdAt.localeCompare(right.$createdAt))[0];
  const cloudProfileId = existingByName?.$id || profile.id;
  const cloudTeamId = existingByName?.teamId || cloudProfileId;

  try {
    await teams.get({ teamId: cloudTeamId });
  } catch (error) {
    if (!isCloudNotFound(error)) throw error;
    if (existingByName) {
      throw new Error("The matching cloud profile has no accessible team.");
    }
    await teams.create({ teamId: cloudTeamId, name: normalizedName });
  }

  const data = {
    name: normalizedName,
    identityId: profile.identityId,
    country: profile.country,
    location: profile.location,
    halowChannel: profile.halowChannel,
    meshId: profile.meshId,
    meshPassphrase: profile.meshPassphrase,
    teamId: cloudTeamId,
    ownerId: user.$id,
  };
  try {
    await tables.getRow({ databaseId, tableId: profileTableId, rowId: cloudProfileId });
    await tables.updateRow({ databaseId, tableId: profileTableId, rowId: cloudProfileId, data });
  } catch (error) {
    if (!isCloudNotFound(error)) throw error;
    await tables.createRow({
      databaseId,
      tableId: profileTableId,
      rowId: cloudProfileId,
      data,
      permissions: [
        Permission.read(Role.team(cloudTeamId)),
        Permission.update(Role.team(cloudTeamId, "owner")),
        Permission.delete(Role.team(cloudTeamId, "owner")),
      ],
    });
  }

  await account.updatePrefs({ prefs: { ...user.prefs, currentProfileId: cloudProfileId } });
  return cloudProfileId;
}

type CloudTelemetryRow = Models.Row & {
  deviceId: string;
  serial: string;
  payload: string;
  receivedAt: string;
};

export type CloudOnlineData = {
  devices: StoredDevice[];
  sensorSamples: EdgezSensorSample[];
  discoveries: StoredProvisioningRequest[];
};

export type CloudSensorHistoryPoint = { timestampMs: number; value: number };
export type CloudSensorHistorySeries = {
  field: string;
  label: string;
  points: CloudSensorHistoryPoint[];
};

type TimeseriesRow = { _time?: unknown; _field?: unknown; _value?: unknown };

function measurementForDevice(deviceId: string) {
  return `device_${deviceId.replace(/[^A-Za-z0-9_-]/g, "_")}`;
}

function cloudSensorLabel(field: string) {
  const name = (field.startsWith("sensor_") ? field.slice("sensor_".length) : field).replaceAll("_", " ");
  return name.replace(/\b\w/g, (letter) => letter.toUpperCase());
}

function timeseriesRange(durationMs: number) {
  if (durationMs <= 60 * 60 * 1000) return "1h";
  if (durationMs <= 6 * 60 * 60 * 1000) return "6h";
  return "24h";
}

/** Query the same per-device Appwrite Time Series Store used by the website. */
export async function getCloudSensorHistory(deviceId: string, durationMs: number): Promise<CloudSensorHistorySeries[]> {
  if (!account) throw new Error("Cloud access is not configured in this app build.");
  const jwt = (await account.createJWT()).jwt;
  const response = await fetch(`${endpoint}/timeseries/stores/${encodeURIComponent(projectId)}/queries`, {
    method: "POST",
    headers: {
      "content-type": "application/json",
      "x-appwrite-project": projectId,
      "x-appwrite-jwt": jwt,
    },
    body: JSON.stringify({
      measurement: measurementForDevice(deviceId),
      range: timeseriesRange(durationMs),
      limit: 2000,
    }),
  });
  const payload = await response.json() as { rows?: TimeseriesRow[]; message?: string };
  if (!response.ok) throw new Error(payload.message || "Could not query sensor history.");
  const since = Date.now() - durationMs;
  const pointsByField = new Map<string, CloudSensorHistoryPoint[]>();
  for (const row of payload.rows ?? []) {
    if (typeof row._field !== "string" || !row._field.startsWith("sensor_")) continue;
    const timestampMs = typeof row._time === "string" ? Date.parse(row._time) : NaN;
    const value = typeof row._value === "number" ? row._value : Number(row._value);
    if (!Number.isFinite(timestampMs) || timestampMs < since || !Number.isFinite(value)) continue;
    const points = pointsByField.get(row._field) ?? [];
    points.push({ timestampMs, value });
    pointsByField.set(row._field, points);
  }
  return [...pointsByField.entries()]
    .sort(([left], [right]) => left.localeCompare(right, undefined, { numeric: true }))
    .map(([field, points]) => ({
      field,
      label: cloudSensorLabel(field),
      points: points.sort((left, right) => left.timestampMs - right.timestampMs),
    }));
}

function storedDeviceFromCloud(profileId: string, device: NativeDevice): StoredDevice {
  const firmwareTarget = device.metadata?.firmwareTarget ?? "";
  const gateway = !device.gatewayDeviceId;
  const kind: StoredDevice["kind"] = gateway
    ? "h7608"
    : firmwareTarget.toLowerCase().includes("nrf54") ? "nrf54" : "esp32";
  const location = Array.isArray(device.location) && device.location.length === 2 ? device.location : null;
  return {
    id: device.$id,
    profileId,
    serial: device.serial,
    name: device.name || device.serial,
    kind,
    firmwareTarget,
    locationSource: location ? "gps" : "none",
    latitude: location?.[1] ?? null,
    longitude: location?.[0] ?? null,
    deviceType: device.metadata?.deviceType || (gateway ? "gateway" : "sensor"),
    marker: device.markerColor || "default",
    iconStyle: device.markerType || device.metadata?.iconStyle || (gateway ? "gateway" : "sensor"),
    userUuid: device.metadata?.userUuid || "",
    lastSeenAt: device.lastSeenAt || "",
    meshMacAddress: device.halowMac || "",
    userPublicKey: device.metadata?.userPublicKey || "",
    createdAt: device.$createdAt || "",
    updatedAt: device.$updatedAt || device.lastSeenAt || "",
  };
}

function sensorSampleFromCloud(row: CloudTelemetryRow, device: NativeDevice): EdgezSensorSample | null {
  const nodeNum = nodeNumFromSerial(device.halowMac || device.serial || row.serial);
  if (nodeNum === null) return null;
  const data: EdgezSensorSample["data"] = {};
  const fields = new Map<number, keyof EdgezSensorSample["data"]>([
    [1, "temperature"], [2, "humidity"], [3, "latitude"], [4, "longitude"],
    [5, "binaryLengthBytes"], [6, "accelX"], [7, "accelY"], [8, "accelZ"],
    [9, "gyroX"], [10, "gyroY"], [11, "gyroZ"], [12, "batteryVoltage"],
    [13, "mcuTemperature"],
  ]);
  try {
    const payload = JSON.parse(row.payload) as { sensors?: { type?: unknown; value?: unknown }[] };
    for (const sensor of payload.sensors ?? []) {
      const field = typeof sensor.type === "number" ? fields.get(sensor.type) : undefined;
      if (field && typeof sensor.value === "number" && Number.isFinite(sensor.value)) data[field] = sensor.value;
    }
  } catch {
    return null;
  }
  if (!Object.keys(data).length) return null;
  return { nodeNum, timestampMs: Date.parse(row.receivedAt) || Date.now(), data };
}

/** Read the cloud view directly, using the same Devices and TablesDB APIs as the website. */
export async function listCloudOnlineData(profileId: string): Promise<CloudOnlineData> {
  if (!tables) throw new Error("Cloud access is not configured in this app build.");
  const deviceQuery = `includeTopology=true&queries[]=${encodeURIComponent(Query.limit(100))}`;
  const [deviceResult, telemetryResult] = await Promise.all([
    devicesRequest<{ devices?: NativeDevice[] }>(`?${deviceQuery}`),
    tables.listRows<CloudTelemetryRow>({
      databaseId,
      tableId: telemetryTableId,
      queries: [Query.orderDesc("receivedAt"), Query.limit(500)],
    }),
  ]);
  const nativeDevices = deviceResult.devices ?? [];
  const devicesById = new Map(nativeDevices.map((device) => [device.$id, device]));
  const profileNativeDevices = nativeDevices.filter((device) =>
    cloudDeviceBelongsToProfile(profileId, device, devicesById));
  const profileDeviceIds = new Set(profileNativeDevices.map((device) => device.$id));
  const devices = profileNativeDevices
    .filter((device) => device.status !== "discovered")
    .map((device) => storedDeviceFromCloud(profileId, device));
  const sensorSamples = telemetryResult.rows.flatMap((row) => {
    if (!profileDeviceIds.has(row.deviceId)) return [];
    const device = devicesById.get(row.deviceId);
    const sample = device ? sensorSampleFromCloud(row, device) : null;
    return sample ? [sample] : [];
  }).sort((left, right) => left.timestampMs - right.timestampMs);
  return {
    devices,
    sensorSamples,
    discoveries: cloudDiscoveredDevices(profileId, nativeDevices),
  };
}

export async function deleteCloudDevice(deviceId: string): Promise<void> {
  await devicesRequest(`/${encodeURIComponent(deviceId)}`, { method: "DELETE" });
}

export async function updateCloudDevice(deviceId: string, update: CloudDeviceUpdate): Promise<void> {
  if (!deviceId) throw new Error("The cloud device record is missing.");
  await devicesRequest(`/${encodeURIComponent(deviceId)}`, {
    method: "PATCH",
    body: JSON.stringify({
      name: update.name,
      markerType: update.markerType,
      markerColor: update.markerColor,
      location: update.latitude === null || update.longitude === null
        ? null
        : [update.longitude, update.latitude],
    }),
  });
}
