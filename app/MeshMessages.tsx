import Ionicons from "@expo/vector-icons/Ionicons";
import MaterialCommunityIcons from "@expo/vector-icons/MaterialCommunityIcons";
import { Interface, Mime, Operation, edgezNodeDisplayName, edgezNodeId, edgezNodeOpensConversation, edgezPublicChannelNode, edgezPublicChannelPorts, encodeNetworkPacket } from "@edgez/react-native-sdk";
import AsyncStorage from "@react-native-async-storage/async-storage";
import { toByteArray } from "base64-js";
import type { EdgezMeshSession, EdgezMeshState, EdgezSensorData, EdgezSensorSample } from "@edgez/react-native-sdk";
import React, { useEffect, useMemo, useState } from "react";
import { ActivityIndicator, Alert, Modal, Pressable, ScrollView, StyleSheet, Text, TextInput, View } from "react-native";
import { SafeAreaView } from "react-native-safe-area-context";
import { SwipeableDeviceRow, SwipeableListRow } from "./DeviceComponents";
import { localMessageId, nodeNumFromSerial, normalizedDeviceSerial } from "./mesh-utils";
import { displayedMessageStatus, isPeerOnline, peerLastSeenMs } from "./message-delivery";
import { deleteProfileProvisioningRequest, markProvisioningRequestAccepted, profileIdentity, saveProfileDevice, saveProfileMessage } from "./profile-store";
import type { StoredDevice, StoredMessage, StoredProfile, StoredProvisioningRequest } from "./profile-store";
import { bandwidthForChannel, channelsForCountry } from "./halowChannels";
import { applyProvisionedProfileExtensions, provisioningNodeAddress } from "./halow-provisioning";
import { encryptProvisioningEnvelope } from "./halow-provisioning-crypto";
import { beaconColorValues, beaconIconColors, beaconIconNames, beaconIconTypes } from "./beacon-options";
import type { BeaconIconColor, BeaconIconType } from "./beacon-options";
import { coordinatesFromLocation } from "./mesh-utils";
import { ProfileLocationPicker } from "./ProfileScreens";
import { acceptCloudProvisioningRequest, deleteCloudDevice, getCloudSensorHistory } from "./cloud-auth";
import type { CloudSensorHistorySeries } from "./cloud-auth";

type InboxTab = "channels" | "people" | "gateways" | "sensors" | "discovered";
type ConversationPeer = { id: string; name: string; serial: string; nodeNum: bigint | null; publicChannel: boolean; device: StoredDevice | null };

const inboxTabStorageKey = "offgrid-tracker:last-inbox-tab:v1";
const personTypes = new Set(["", "unspecified", "user", "device_type_user", "person", "hiker", "explorer"]);
const sensorFields: ReadonlyArray<{ key: keyof EdgezSensorData; label: string; unit: string; decimals: number }> = [
  { key: "temperature", label: "Temperature", unit: "°C", decimals: 1 },
  { key: "humidity", label: "Humidity", unit: "%", decimals: 1 },
  { key: "pressure", label: "Pressure", unit: " hPa", decimals: 1 },
  { key: "altitude", label: "Altitude", unit: " m", decimals: 1 },
  { key: "vibrationAverage", label: "Vibration", unit: "", decimals: 2 },
  { key: "accelX", label: "Accel X", unit: "", decimals: 2 },
  { key: "accelY", label: "Accel Y", unit: "", decimals: 2 },
  { key: "accelZ", label: "Accel Z", unit: "", decimals: 2 },
  { key: "gyroX", label: "Gyro X", unit: "", decimals: 2 },
  { key: "gyroY", label: "Gyro Y", unit: "", decimals: 2 },
  { key: "gyroZ", label: "Gyro Z", unit: "", decimals: 2 },
  { key: "batteryVoltage", label: "Battery Voltage", unit: " V", decimals: 2 },
  { key: "mcuTemperature", label: "MCU Temperature", unit: "°C", decimals: 1 },
  { key: "latitude", label: "Latitude", unit: "°", decimals: 5 },
  { key: "longitude", label: "Longitude", unit: "°", decimals: 5 },
];
const sensorHistoryRanges = [
  { key: "5m", label: "5 min", durationMs: 5 * 60 * 1000 },
  { key: "15m", label: "15 min", durationMs: 15 * 60 * 1000 },
  { key: "30m", label: "30 min", durationMs: 30 * 60 * 1000 },
] as const;
const sensorChartColors = ["#0a8c87", "#d47a1f", "#7c5ce5", "#d14b78", "#3977c3", "#6c8f2d"];

export function MeshMessages({ visible, profile, profileId, session, meshState, devices, messages, provisioningRequests, cloudSensorSamples, onlineMode, initialPeerSerial, onMessagesChanged, onDevicesChanged, onProvisioningRequestsChanged, onResendMessage, onShowDevice, onRemoveDevice, onClose }: {
  visible: boolean;
  profile: StoredProfile;
  profileId: string;
  session: EdgezMeshSession | null;
  meshState: EdgezMeshState | null;
  devices: StoredDevice[];
  messages: StoredMessage[];
  provisioningRequests: StoredProvisioningRequest[];
  cloudSensorSamples: EdgezSensorSample[];
  onlineMode: boolean;
  initialPeerSerial: string | null;
  onMessagesChanged: () => Promise<void>;
  onDevicesChanged: () => Promise<void>;
  onProvisioningRequestsChanged: () => Promise<void>;
  onResendMessage: (message: StoredMessage) => Promise<void>;
  onShowDevice: (device: StoredDevice) => void;
  onRemoveDevice: (device: StoredDevice) => void;
  onClose: () => void;
}) {
  const channels: ConversationPeer[] = (onlineMode ? [] : edgezPublicChannelPorts)
    .map((nodeNum, index) => meshState?.nodes.get(nodeNum) ?? edgezPublicChannelNode(index + 1))
    .filter((node) => node.enabled !== false)
    .map((node) => ({ id: `channel-${node.nodeNum}`, name: edgezNodeDisplayName(node), serial: edgezNodeId(node.nodeNum).toUpperCase(), nodeNum: node.nodeNum, publicChannel: true, device: null }));
  const gateways = devices.filter((device) => device.kind === "h7608" || device.deviceType.trim().toLowerCase() === "gateway");
  const gatewayIds = new Set(gateways.map((device) => device.id));
  const people = onlineMode ? [] : devices.filter((device) => {
    const nodeNum = nodeNumFromSerial(device.meshMacAddress || device.serial);
    const liveNode = nodeNum === null ? undefined : meshState?.nodes.get(nodeNum);
    return liveNode ? edgezNodeOpensConversation(liveNode) : personTypes.has(device.deviceType.trim().toLowerCase());
  });
  const peopleIds = new Set(people.map((device) => device.id));
  const sensors = devices.filter((device) => !gatewayIds.has(device.id) && (onlineMode || !peopleIds.has(device.id)));
  const peoplePeers: ConversationPeer[] = people.map((device) => ({ id: device.id, name: device.name, serial: device.meshMacAddress || device.serial, nodeNum: nodeNumFromSerial(device.meshMacAddress || device.serial), publicChannel: false, device }));
  const peers = [...channels, ...peoplePeers];
  const [tab, setTab] = useState<InboxTab>(onlineMode ? "gateways" : "channels");
  const [peerSerial, setPeerSerial] = useState<string | null>(null);
  const [sensorId, setSensorId] = useState<string | null>(null);
  const [requestId, setRequestId] = useState<string | null>(null);
  const [channelInfo, setChannelInfo] = useState(false);
  const [message, setMessage] = useState("");
  const [sending, setSending] = useState(false);
  const [acceptingRequest, setAcceptingRequest] = useState(false);
  const [resendingUuid, setResendingUuid] = useState<string | null>(null);
  const [error, setError] = useState("");
  const [sensorHistoryRange, setSensorHistoryRange] = useState<(typeof sensorHistoryRanges)[number]["key"]>("5m");
  const [sensorHistory, setSensorHistory] = useState<CloudSensorHistorySeries[]>([]);
  const [selectedSensorFields, setSelectedSensorFields] = useState<string[]>([]);
  const [sensorHistoryLoading, setSensorHistoryLoading] = useState(false);
  const [sensorHistoryError, setSensorHistoryError] = useState("");
  const [requestName, setRequestName] = useState("");
  const [requestLocationMode, setRequestLocationMode] = useState<"none" | "map" | "live">("none");
  const [requestLocation, setRequestLocation] = useState("");
  const [requestMapOpen, setRequestMapOpen] = useState(false);
  const [requestIcon, setRequestIcon] = useState<BeaconIconType>("explorer");
  const [requestColor, setRequestColor] = useState<BeaconIconColor>("blue");
  const selectedPeer = peerSerial ? peers.find((peer) => sameSerial(peer.serial, peerSerial)) ?? null : null;
  const selectedSensor = sensorId ? sensors.find((device) => device.id === sensorId) ?? null : null;
  const selectedRequest = requestId ? provisioningRequests.find((request) => request.id === requestId) ?? null : null;

  useEffect(() => {
    if (!selectedRequest) return;
    setRequestName(selectedRequest.serial);
    setRequestLocationMode("none");
    setRequestLocation("");
    setRequestIcon("explorer");
    setRequestColor("blue");
  }, [selectedRequest?.id]);

  const selectedNode = selectedPeer?.nodeNum == null ? null : meshState?.nodes.get(selectedPeer.nodeNum) ?? null;
  const conversation = selectedPeer ? messages.filter((item) => !item.retryOfUuid && sameSerial(item.peerSerial, selectedPeer.serial)).sort((a, b) => a.timestampMs - b.timestampMs) : [];
  const sensorNodeNum = selectedSensor ? nodeNumFromSerial(selectedSensor.meshMacAddress || selectedSensor.serial) : null;
  const onlineSamples = useMemo(() => {
    const grouped = new Map<bigint, EdgezSensorSample[]>();
    for (const sample of cloudSensorSamples) grouped.set(sample.nodeNum, [...grouped.get(sample.nodeNum) ?? [], sample]);
    return grouped;
  }, [cloudSensorSamples]);
  const samples = sensorNodeNum === null ? [] : onlineMode
    ? onlineSamples.get(sensorNodeNum) ?? []
    : meshState?.sensorSamples.get(sensorNodeNum) ?? [];
  const activeSensorRange = sensorHistoryRanges.find((range) => range.key === sensorHistoryRange)!;
  const offlineSensorHistory = useMemo(
    () => sensorHistoryFromSamples(samples, activeSensorRange.durationMs),
    [samples, activeSensorRange.durationMs],
  );
  const displayedSensorHistory = onlineMode ? sensorHistory : offlineSensorHistory;
  const selectedHistorySeries = displayedSensorHistory.filter((series) => selectedSensorFields.includes(series.field));
  const latestSensorTimestamp = samples[samples.length - 1]?.timestampMs ?? 0;

  useEffect(() => {
    if (!visible || !selectedSensor || !onlineMode) {
      setSensorHistory([]);
      setSensorHistoryError("");
      setSensorHistoryLoading(false);
      return;
    }
    let active = true;
    setSensorHistoryLoading(true);
    setSensorHistoryError("");
    void getCloudSensorHistory(selectedSensor.id, activeSensorRange.durationMs)
      .then((series) => { if (active) setSensorHistory(series); })
      .catch((caught) => { if (active) setSensorHistoryError(caught instanceof Error ? caught.message : "Could not load sensor history."); })
      .finally(() => { if (active) setSensorHistoryLoading(false); });
    return () => { active = false; };
  }, [activeSensorRange.durationMs, latestSensorTimestamp, onlineMode, selectedSensor?.id, visible]);

  useEffect(() => {
    const available = new Set(displayedSensorHistory.map((series) => series.field));
    setSelectedSensorFields((current) => {
      const retained = current.filter((field) => available.has(field));
      return retained.length ? retained : displayedSensorHistory.slice(0, 3).map((series) => series.field);
    });
  }, [displayedSensorHistory]);

  useEffect(() => {
    let mounted = true;
    void AsyncStorage.getItem(inboxTabStorageKey).then((storedTab) => {
      if (!mounted) return;
      if (onlineMode) setTab(storedTab === "sensors" || storedTab === "discovered" || storedTab === "gateways" ? storedTab : "gateways");
      else if (storedTab === "channels" || storedTab === "people" || storedTab === "sensors" || storedTab === "discovered") setTab(storedTab);
    }).catch(() => undefined);
    return () => { mounted = false; };
  }, [onlineMode]);

  useEffect(() => {
    if (!visible) return;
    setChannelInfo(false);
    setError("");
    if (!initialPeerSerial) return;
    const sensor = sensors.find((device) => sameSerial(device.meshMacAddress || device.serial, initialPeerSerial));
    if (sensor) {
      setTab("sensors"); setSensorId(sensor.id); setPeerSerial(null); setRequestId(null);
      void AsyncStorage.setItem(inboxTabStorageKey, "sensors").catch(() => undefined);
      return;
    }
    const peer = peers.find((item) => sameSerial(item.serial, initialPeerSerial));
    if (peer) {
      const peerTab = peer.publicChannel ? "channels" : "people";
      setTab(peerTab); setPeerSerial(peer.serial); setSensorId(null); setRequestId(null);
      void AsyncStorage.setItem(inboxTabStorageKey, peerTab).catch(() => undefined);
    }
  }, [initialPeerSerial, visible]);

  const latestBySerial = useMemo(() => {
    const latest = new Map<string, StoredMessage>();
    messages.filter((item) => !item.retryOfUuid).forEach((item) => {
      const key = normalizedDeviceSerial(item.peerSerial);
      if (!latest.has(key) || (latest.get(key)?.timestampMs ?? 0) < item.timestampMs) latest.set(key, item);
    });
    return latest;
  }, [messages]);

  const selectTab = (next: InboxTab) => {
    setTab(next); setPeerSerial(null); setSensorId(null); setRequestId(null); setChannelInfo(false); setError("");
    void AsyncStorage.setItem(inboxTabStorageKey, next).catch(() => undefined);
  };
  const closeDetail = () => { setPeerSerial(null); setSensorId(null); setRequestId(null); setChannelInfo(false); setError(""); };

  const send = async () => {
    const text = message.trim();
    if (!session || !selectedNode || !selectedPeer || !text || sending) return;
    setSending(true); setError("");
    try {
      if (selectedPeer.publicChannel) {
        const from = meshState?.status?.macAddress;
        if (!from) throw new Error("Local HaLow address is not ready.");
        const id = localMessageId();
        const packet = encodeNetworkPacket({ from, to: selectedNode.nodeNum, operation: Operation.request, interface: Interface.halow, msg: { messageIdHigh: id.high, messageIdLow: id.low, sequence: 1, mime: Mime.text, payload: new TextEncoder().encode(text) } });
        await session.sdk.transport.invoke("sendPacket", { label: `Public channel message to ${selectedPeer.name}`, packet: Array.from(packet), waitForDrainMs: 3000 });
        await saveProfileMessage(profileId, selectedPeer.serial, { nodeNum: selectedNode.nodeNum, text, mine: true, timestampMs: Date.now(), messageUuid: id.uuid, status: "Sent", voiceBytes: new Uint8Array(), voiceCodec: 0, durationMs: 0 });
        await onMessagesChanged();
      } else {
        await session.sendTextMessage(selectedNode.nodeNum, text);
      }
      setMessage("");
    } catch (caught) {
      setError(caught instanceof Error ? caught.message : "Could not send the mesh message.");
    } finally { setSending(false); }
  };

  const resend = async (item: StoredMessage) => {
    if (resendingUuid) return;
    setResendingUuid(item.messageUuid); setError("");
    try {
      await onResendMessage(item);
    } catch (caught) {
      setError(caught instanceof Error ? caught.message : "Could not resend the mesh message.");
    } finally {
      setResendingUuid(null);
    }
  };

  const acceptProvisioningRequest = async () => {
    if (!selectedRequest || acceptingRequest) return;
    const cloudRequest = selectedRequest.transport === "mqtt";
    if (!cloudRequest && !session) return;
    const channel = channelsForCountry(profile.country)
      .find(({ number }) => number === profile.halowChannel);
    if (!channel) {
      setError("The active profile has no valid HaLow channel.");
      return;
    }
    setAcceptingRequest(true);
    setError("");
    try {
      const name = requestName.trim();
      if (!name) throw new Error("Enter a device name.");
      if (name.length > 64) throw new Error("The device name must not exceed 64 characters.");
      const coordinates = requestLocationMode === "map" ? coordinatesFromLocation(requestLocation) : null;
      if (requestLocationMode === "map" && !coordinates) throw new Error("Choose a valid device location.");
      const from = meshState?.status?.macAddress ??
        (selectedRequest.gatewayNodeNum ? BigInt(selectedRequest.gatewayNodeNum) : null);
      if (!from) throw new Error(cloudRequest
        ? "The discovery gateway has no HaLow address."
        : "Local HaLow address is not ready.");
      const to = BigInt(selectedRequest.nodeNum);
      const identity = profileIdentity(profile);
      const provisionedUserId = cloudRequest
        ? cloudDeviceUserId(selectedRequest.cloudDeviceId)
        : {
            high: BigInt(selectedRequest.userIdHigh),
            low: BigInt(selectedRequest.userIdLow),
          };
      const encodedProfile = encodeNetworkPacket({
        from,
        to,
        operation: Operation.request,
        interface: Interface.halow,
        init: {
          countryCode: profile.country,
          meshId: profile.meshId,
          passphrase: profile.meshPassphrase,
          maxHop: 4,
          userIdHigh: provisionedUserId.high,
          userIdLow: provisionedUserId.low,
          userName: name,
          userPublicKey: toByteArray(selectedRequest.userPublicKey),
          marker: requestColor,
          hasLocation: Boolean(coordinates),
          latitude: coordinates?.latitude ?? 0,
          longitude: coordinates?.longitude ?? 0,
          meshBandwidthMhz: bandwidthForChannel(channel),
          meshFrequencyKhz: Math.round(channel.frequencyMHz * 1000),
        },
      });
      const innerPacket = applyProvisionedProfileExtensions(encodedProfile, {
        deviceGpsEnabled: requestLocationMode === "live",
        iconStyle: requestIcon,
      });
      const encrypted = encryptProvisioningEnvelope({
        from,
        to,
        senderPrivateKey: identity.privateKey,
        senderPublicKey: identity.publicKey,
        recipientPublicKey: toByteArray(selectedRequest.userPublicKey),
        plaintext: innerPacket,
      });
      const messageId = localMessageId();
      const packet = encodeNetworkPacket({
        from,
        to,
        operation: Operation.request,
        interface: Interface.halow,
        msg: {
          messageIdHigh: messageId.high,
          messageIdLow: messageId.low,
          sequence: 1,
          mime: Mime.binary,
          payload: encrypted,
        },
      });
      if (cloudRequest) {
        await acceptCloudProvisioningRequest(
          selectedRequest.cloudDeviceId ?? "",
          packet,
          requestIcon,
          requestColor,
        );
      } else {
        await session!.sdk.transport.invoke("sendPacket", {
          label: `Accept zero-touch sensor ${selectedRequest.serial}`,
          packet: Array.from(packet),
          waitForDrainMs: 3000,
        });
      }
      if (!cloudRequest) {
        await saveProfileDevice({
          profileId,
          serial: selectedRequest.serial,
          name,
          kind: "nrf54",
          firmwareTarget: "edgez-nrf54l15-fgh200m-sensor",
          locationSource: requestLocationMode === "live" ? "gps" : requestLocationMode,
          latitude: coordinates?.latitude ?? null,
          longitude: coordinates?.longitude ?? null,
          deviceType: "sensor",
          marker: requestColor,
          iconStyle: requestIcon,
          userUuid: `${selectedRequest.userIdHigh}:${selectedRequest.userIdLow}`,
          lastSeenAt: selectedRequest.lastSeenAt,
          meshMacAddress: provisioningNodeAddress(selectedRequest.nodeNum),
          userPublicKey: selectedRequest.userPublicKey,
        });
        await markProvisioningRequestAccepted(profileId, selectedRequest.id);
      }
      if (cloudRequest) await onProvisioningRequestsChanged();
      else {
        await onDevicesChanged();
        await onProvisioningRequestsChanged();
      }
      setRequestId(null);
      Alert.alert(
        cloudRequest ? "Provisioning queued" : "Provisioning sent",
        cloudRequest
          ? "The encrypted profile was sent to the gateway. This sensor becomes active after the gateway receives its first beacon or bulk block."
          : "The encrypted profile was sent to the sensor.",
      );
    } catch (caught) {
      setError(caught instanceof Error ? caught.message : "Could not accept the provisioning request.");
    } finally {
      setAcceptingRequest(false);
    }
  };

  const removeProvisioningRequest = (request: StoredProvisioningRequest) => {
    Alert.alert(
      "Remove discovery?",
      `${request.serial} will be removed from this profile. It can appear again if the sensor sends another provisioning request.`,
      [
        { text: "Cancel", style: "cancel" },
        {
          text: "Remove",
          style: "destructive",
          onPress: () => {
            void (async () => {
              setError("");
              if (request.transport === "mqtt" && request.cloudDeviceId) await deleteCloudDevice(request.cloudDeviceId);
              else await deleteProfileProvisioningRequest(profileId, request.id);
              if (requestId === request.id) setRequestId(null);
              await onProvisioningRequestsChanged();
            })().catch((caught) => {
              setError(caught instanceof Error ? caught.message : "Could not remove the discovery request.");
            });
          },
        },
      ],
    );
  };

  if (requestMapOpen) {
    return <Modal visible={visible} animationType="slide" onRequestClose={() => setRequestMapOpen(false)}>
      <ProfileLocationPicker
        location={requestLocation}
        country={profile.country}
        subject="device"
        onCancel={() => setRequestMapOpen(false)}
        onSelect={(location) => { setRequestLocation(location); setRequestLocationMode("map"); setRequestMapOpen(false); }}
      />
    </Modal>;
  }

  return <Modal visible={visible} animationType="slide" onRequestClose={onClose}>
    <SafeAreaView style={styles.page}>
      <View style={styles.header}>
        <View><Text style={styles.eyebrow}>OFFGRID</Text><Text style={styles.title}>{selectedPeer?.name ?? selectedSensor?.name ?? selectedRequest?.serial ?? (onlineMode ? "Devices" : "Messages")}</Text></View>
        <View style={styles.headerActions}>
          {selectedPeer || selectedSensor ? <Pressable style={styles.iconButton} onPress={() => {
            const device = selectedPeer?.device ?? selectedSensor;
            if (device) onShowDevice(device); else setChannelInfo((value) => !value);
          }} hitSlop={8} accessibilityRole="button" accessibilityLabel="Show details"><Ionicons name="information-circle-outline" size={25} color="#0a8c87" /></Pressable> : null}
          <Pressable style={styles.closeButton} onPress={onClose} hitSlop={8} accessibilityRole="button"><Text style={styles.close}>CLOSE</Text></Pressable>
        </View>
      </View>

      {!selectedPeer && !selectedSensor && !selectedRequest ? <View style={styles.tabs}>{(onlineMode
        ? (["gateways", "sensors", "discovered"] as const)
        : (["channels", "people", "sensors", "discovered"] as const)
      ).map((item) => <Pressable key={item} style={[styles.tab, tab === item && styles.activeTab]} onPress={() => selectTab(item)} accessibilityRole="tab" accessibilityState={{ selected: tab === item }}><Text style={[styles.tabText, tab === item && styles.activeTabText]}>{item.toUpperCase()}</Text></Pressable>)}</View> : null}

      <ScrollView contentContainerStyle={styles.content} keyboardShouldPersistTaps="handled">
        {!selectedPeer && !selectedSensor && !selectedRequest && tab === "channels" ? <View style={styles.list}>{channels.map((peer) => <ChannelRow key={peer.id} peer={peer} latest={latestBySerial.get(normalizedDeviceSerial(peer.serial))} onOpen={() => setPeerSerial(peer.serial)} />)}</View> : null}

        {!selectedPeer && !selectedSensor && !selectedRequest && tab === "people" ? people.length ? <View style={styles.list}>{people.map((device) => {
          const serial = device.meshMacAddress || device.serial;
          const latest = latestBySerial.get(normalizedDeviceSerial(serial));
          const lastSeen = peerLastSeenMs(serial, devices, meshState);
          return <SwipeableDeviceRow key={device.id} device={device} icon="person-outline" subtitle={latest?.text || "Start a mesh conversation"} meta={lastSeenLabel(lastSeen, isPeerOnline(serial, devices, meshState))} badge={latest?.mine ? displayedMessageStatus(latest).toUpperCase() : undefined} onOpen={() => setPeerSerial(serial)} onRemove={onRemoveDevice} />;
        })}</View> : <EmptyState icon="people-outline" title="No people discovered" text="People appear here after their beacon reaches this profile. They remain available before the first message." /> : null}

        {!selectedPeer && !selectedSensor && !selectedRequest && tab === "gateways" ? gateways.length ? <View style={styles.list}>{gateways.map((device) => <SwipeableDeviceRow
          key={device.id}
          device={device}
          icon="git-network-outline"
          subtitle={device.meshMacAddress || device.serial}
          meta={device.lastSeenAt ? `LAST SEEN ${timestamp(Date.parse(device.lastSeenAt))}` : "WAITING FOR STATUS"}
          onOpen={() => onShowDevice(device)}
          onRemove={onRemoveDevice}
        />)}</View> : <EmptyState icon="git-network-outline" title="No gateways" text="Provisioned H7608 gateways assigned to this profile appear here." /> : null}

        {!selectedPeer && !selectedSensor && !selectedRequest && tab === "sensors" ? sensors.length ? <View style={styles.list}>{sensors.map((device) => {
          const nodeNum = nodeNumFromSerial(device.meshMacAddress || device.serial);
          const deviceSamples = nodeNum === null ? [] : onlineMode
            ? onlineSamples.get(nodeNum) ?? []
            : meshState?.sensorSamples.get(nodeNum) ?? [];
          const latest = deviceSamples[deviceSamples.length - 1];
          const serial = device.meshMacAddress || device.serial;
          const lastSeen = peerLastSeenMs(serial, devices, meshState);
          return <SwipeableDeviceRow key={device.id} device={device} icon="pulse-outline" subtitle={latest ? sensorSummary(latest.data) : "No sensor data yet"} meta={lastSeenLabel(lastSeen, isPeerOnline(serial, devices, meshState))} badge={deviceSamples.length ? `${deviceSamples.length} SAMPLE${deviceSamples.length === 1 ? "" : "S"}` : undefined} onOpen={() => setSensorId(device.id)} onRemove={onRemoveDevice} />;
        })}</View> : <EmptyState icon="pulse-outline" title="No sensors discovered" text="Sensors and other mesh devices appear here after their beacon reaches this profile." /> : null}

        {!selectedPeer && !selectedSensor && !selectedRequest && tab === "discovered" ? provisioningRequests.length ? <View style={styles.list}>{provisioningRequests.map((request) => <SwipeableListRow
          key={request.id}
          accessibilityName={request.serial}
          icon="radio-outline"
          name={request.serial}
          subtitle={provisioningNodeAddress(request.nodeNum)}
          meta={`LAST SEEN ${timestamp(Date.parse(request.lastSeenAt))}`}
          badge={request.status === "accepted" ? "ACCEPTED" : "WAITING"}
          onOpen={() => setRequestId(request.id)}
          onRemove={() => removeProvisioningRequest(request)}
        />)}</View> : <EmptyState icon="radio-outline" title="No sensors waiting" text="Unprovisioned HaLow sensors appear here when a companion receives their zero-touch request." /> : null}

        {selectedPeer ? <>
          <BackButton label={tab} onPress={closeDetail} />
          {channelInfo ? <View style={styles.infoCard}><View style={styles.infoIcon}><Ionicons name="people-outline" size={28} color="#0a8c87" /></View><Text style={styles.infoTitle}>{selectedPeer.name}</Text><Text style={styles.muted}>Public clear-text mesh channel</Text><InfoRow label="ADDRESS" value={selectedPeer.serial} /><InfoRow label="MESSAGES" value={String(conversation.length)} /></View> : <>
            <View style={styles.conversation}>{conversation.length ? conversation.map((item) => {
              const status = displayedMessageStatus(item);
              const failed = item.mine && status === "Failed";
              return <View key={item.messageUuid} style={[styles.bubble, item.mine ? styles.mine : styles.peer]}><Text style={styles.messageText}>{item.text}</Text><View style={styles.messageFooter}><Text style={styles.messageStatus}>{item.mine ? status : selectedPeer.name} · {timestamp(item.timestampMs)}</Text>{failed ? <Pressable style={styles.resendButton} disabled={resendingUuid === item.messageUuid} onPress={() => void resend(item)} hitSlop={8} accessibilityRole="button" accessibilityLabel="Resend message">{resendingUuid === item.messageUuid ? <ActivityIndicator size="small" color="#0a8c87" /> : <><Ionicons name="refresh" size={13} color="#0a8c87" /><Text style={styles.resendText}>RESEND</Text></>}</Pressable> : null}</View></View>;
            }) : <Text style={styles.muted}>No messages yet. Send the first message below.</Text>}</View>
            <TextInput style={styles.input} value={message} onChangeText={setMessage} placeholder="Message over HaLow mesh" placeholderTextColor="#718783" multiline />
            <Pressable style={[styles.send, (!message.trim() || sending || !meshState?.bleReady || !selectedNode) && styles.disabled]} disabled={!message.trim() || sending || !meshState?.bleReady || !selectedNode} onPress={() => void send()} accessibilityRole="button">{sending ? <ActivityIndicator color="white" /> : <Text style={styles.sendText}>SEND OVER MESH</Text>}</Pressable>
            {!selectedNode ? <Text style={styles.error}>The cached device identity is incomplete. Wait for one beacon to refresh it.</Text> : null}
            {selectedNode && !meshState?.bleReady ? <Text style={styles.error}>BLE control channel is not ready.</Text> : null}
            {error ? <Text style={styles.error}>{error}</Text> : null}
          </>}
        </> : null}

        {selectedSensor ? <>
          <BackButton label="sensors" onPress={closeDetail} />
          <View style={styles.sensorHero}><Ionicons name="pulse-outline" size={28} color="#0a8c87" /><View style={styles.sensorHeroText}><Text style={styles.sensorTitle}>Data statistics</Text><Text style={styles.muted}>{samples.length} received sample{samples.length === 1 ? "" : "s"}</Text></View></View>
          <View style={styles.sensorChartCard}>
            <View style={styles.sensorChartHeader}>
              <View><Text style={styles.sensorChartTitle}>Sensor history</Text><Text style={styles.sensorChartSubtitle}>{selectedHistorySeries.reduce((total, series) => total + series.points.length, 0)} selected readings</Text></View>
              {sensorHistoryLoading ? <ActivityIndicator size="small" color="#0a8c87" /> : null}
            </View>
            <View style={styles.sensorRangeRow}>{sensorHistoryRanges.map((range) => <Pressable key={range.key} style={[styles.sensorRangeButton, sensorHistoryRange === range.key && styles.sensorRangeButtonActive]} onPress={() => setSensorHistoryRange(range.key)} accessibilityRole="button"><Text style={[styles.sensorRangeText, sensorHistoryRange === range.key && styles.sensorRangeTextActive]}>{range.label}</Text></Pressable>)}</View>
            <View style={styles.sensorPicker}>{displayedSensorHistory.map((series, index) => {
              const selected = selectedSensorFields.includes(series.field);
              return <Pressable key={series.field} style={[styles.sensorPickerItem, selected && styles.sensorPickerItemActive]} onPress={() => setSelectedSensorFields((current) => current.includes(series.field) ? current.filter((field) => field !== series.field) : [...current, series.field])} accessibilityRole="checkbox" accessibilityState={{ checked: selected }}><View style={[styles.sensorPickerDot, { backgroundColor: sensorChartColors[index % sensorChartColors.length] }]} /><Text style={styles.sensorPickerText}>{series.label}</Text><Text style={styles.sensorPickerCount}>{series.points.length}</Text></Pressable>;
            })}</View>
            <SensorHistoryChart series={selectedHistorySeries} durationMs={activeSensorRange.durationMs} allSeries={displayedSensorHistory} />
            <View style={styles.sensorChartFooter}><Text>{activeSensorRange.label} ago</Text><Text>Now</Text></View>
            {sensorHistoryError ? <Text style={styles.error}>{sensorHistoryError}</Text> : null}
          </View>
          {!samples.length ? <EmptyState icon="analytics-outline" title="No data received yet" text="This device is saved in the profile. Statistics appear when a sensor beacon arrives." /> : <View style={styles.statsGrid}>
            {sensorFields.map((field) => {
              const values = samples.map((sample) => sample.data[field.key]).filter((value): value is number => typeof value === "number" && Number.isFinite(value));
              if (!values.length) return null;
              const latest = values[values.length - 1]; const min = Math.min(...values); const max = Math.max(...values); const average = values.reduce((sum, value) => sum + value, 0) / values.length;
              const format = (value: number) => `${value.toFixed(field.decimals)}${field.unit}`;
              return <View key={field.key} style={styles.statCard}><Text style={styles.statLabel}>{field.label.toUpperCase()}</Text><Text style={styles.statValue}>{format(latest)}</Text><Text style={styles.statRange}>AVG {format(average)} · MIN {format(min)} · MAX {format(max)}</Text></View>;
            })}
            <Text style={styles.sampleTime}>Latest sample · {timestamp(samples[samples.length - 1].timestampMs)}</Text>
          </View>}
        </> : null}

        {selectedRequest ? <>
          <BackButton label="discovered" onPress={closeDetail} />
          <View style={styles.infoCard}>
            <View style={styles.infoIcon}><Ionicons name="radio-outline" size={28} color="#0a8c87" /></View>
            <Text style={styles.infoTitle}>{selectedRequest.serial}</Text>
            <Text style={styles.muted}>Zero-touch Wi-Fi HaLow provisioning request</Text>
            <InfoRow label="HALOW ADDRESS" value={provisioningNodeAddress(selectedRequest.nodeNum)} />
            <InfoRow label="FIRST SEEN" value={timestamp(Date.parse(selectedRequest.firstSeenAt))} />
            <InfoRow label="LAST SEEN" value={timestamp(Date.parse(selectedRequest.lastSeenAt))} />
            <InfoRow label="PUBLIC KEY" value={`${selectedRequest.userPublicKey.slice(0, 12)}…`} />
            <InfoRow label="TRANSPORT" value={selectedRequest.transport === "mqtt" ? "CLOUD · MQTT" : "LOCAL · BLE"} />
            <InfoRow label="STATUS" value={selectedRequest.status.toUpperCase()} />
          </View>
          <View style={styles.setupCard}>
            <Text style={styles.setupTitle}>SENSOR PROFILE</Text>
            <TextInput
              style={styles.setupInput}
              value={requestName}
              onChangeText={setRequestName}
              maxLength={64}
              placeholder="Device name"
              placeholderTextColor="#8aa09d"
            />
            <Text style={styles.setupLabel}>LOCATION</Text>
            <View style={styles.choiceRow}>
              <ChoiceButton label="OFF" selected={requestLocationMode === "none"} onPress={() => { setRequestLocationMode("none"); setRequestLocation(""); }} />
              <ChoiceButton label="MAP" selected={requestLocationMode === "map"} onPress={() => setRequestMapOpen(true)} />
              <ChoiceButton label="LIVE" selected={requestLocationMode === "live"} onPress={() => { setRequestLocationMode("live"); setRequestLocation(""); }} />
            </View>
            {requestLocationMode === "map" && requestLocation ? <Text style={styles.locationValue}>{requestLocation}</Text> : null}
            {requestLocationMode === "live" ? <Text style={styles.locationValue}>Uses the device L76K GPS</Text> : null}
            <Text style={styles.setupLabel}>ICON</Text>
            <View style={styles.iconGrid}>
              {beaconIconTypes.map((icon) => <Pressable key={icon} style={[styles.iconChoice, requestIcon === icon && styles.iconChoiceSelected]} onPress={() => setRequestIcon(icon)} accessibilityRole="radio" accessibilityState={{ selected: requestIcon === icon }} accessibilityLabel={icon}>
                <MaterialCommunityIcons name={beaconIconNames[icon]} size={21} color={requestIcon === icon ? "white" : "#0a8c87"} />
              </Pressable>)}
            </View>
            <Text style={styles.setupLabel}>COLOR</Text>
            <View style={styles.colorRow}>
              {beaconIconColors.map((color) => <Pressable key={color} style={[styles.colorChoice, { backgroundColor: beaconColorValues[color] }, requestColor === color && styles.colorChoiceSelected]} onPress={() => setRequestColor(color)} accessibilityRole="radio" accessibilityState={{ selected: requestColor === color }} accessibilityLabel={color} />)}
            </View>
            <View style={styles.encryptionNotice}><Ionicons name="lock-closed" size={14} color="#0a8c87" /><Text style={styles.encryptionText}>Mesh ID, passphrase, channel and sensor settings are encrypted for this sensor.</Text></View>
          </View>
          <Pressable style={[styles.send, (acceptingRequest || (selectedRequest.transport !== "mqtt" && !meshState?.bleReady)) && styles.disabled]} disabled={acceptingRequest || (selectedRequest.transport !== "mqtt" && !meshState?.bleReady)} onPress={() => void acceptProvisioningRequest()} accessibilityRole="button">
            {acceptingRequest ? <ActivityIndicator color="white" /> : <Text style={styles.sendText}>{selectedRequest.status === "accepted" ? "SEND PROFILE AGAIN" : "ACCEPT AND PROVISION"}</Text>}
          </Pressable>
          {selectedRequest.transport !== "mqtt" && !meshState?.bleReady ? <Text style={styles.error}>BLE control channel is not ready.</Text> : null}
          {error ? <Text style={styles.error}>{error}</Text> : null}
        </> : null}
      </ScrollView>
    </SafeAreaView>
  </Modal>;
}

function ChannelRow({ peer, latest, onOpen }: { peer: ConversationPeer; latest?: StoredMessage; onOpen: () => void }) {
  return <Pressable style={styles.row} onPress={onOpen} accessibilityRole="button"><View style={styles.rowIcon}><Ionicons name="people-outline" size={20} color="#0a8c87" /></View><View style={styles.rowBody}><Text style={styles.peerName}>{peer.name}</Text><Text style={styles.preview} numberOfLines={1}>{latest?.text || "Send to everyone on this channel"}</Text></View><View style={styles.rowMeta}><Text style={styles.time}>PUBLIC CHANNEL</Text><Ionicons name="chevron-forward" size={17} color="#718783" /></View></Pressable>;
}
function BackButton({ label, onPress }: { label: string; onPress: () => void }) { return <Pressable style={styles.back} onPress={onPress} accessibilityRole="button"><Ionicons name="chevron-back" size={18} color="#0a8c87" /><Text style={styles.actionText}>{label.toUpperCase()}</Text></Pressable>; }
function EmptyState({ icon, title, text }: { icon: React.ComponentProps<typeof Ionicons>["name"]; title: string; text: string }) { return <View style={styles.empty}><Ionicons name={icon} size={25} color="#0a8c87" /><Text style={styles.emptyTitle}>{title}</Text><Text style={styles.muted}>{text}</Text></View>; }
function InfoRow({ label, value }: { label: string; value: string }) { return <View style={styles.infoRow}><Text style={styles.infoLabel}>{label}</Text><Text style={styles.infoValue}>{value}</Text></View>; }
function ChoiceButton({ label, selected, loading, onPress }: { label: string; selected: boolean; loading?: boolean; onPress: () => void }) { return <Pressable style={[styles.choiceButton, selected && styles.choiceButtonSelected]} onPress={onPress} accessibilityRole="radio" accessibilityState={{ selected }}>{loading ? <ActivityIndicator size="small" color={selected ? "white" : "#0a8c87"} /> : <Text style={[styles.choiceText, selected && styles.choiceTextSelected]}>{label}</Text>}</Pressable>; }
function sameSerial(a: string, b: string) { return normalizedDeviceSerial(a) === normalizedDeviceSerial(b); }
function cloudDeviceUserId(deviceId?: string) {
  const hex = (deviceId ?? "").replace(/-/g, "");
  if (!/^[0-9a-fA-F]{32}$/.test(hex)) {
    throw new Error("The cloud discovery record has no valid Device ID.");
  }
  return {
    high: BigInt(`0x${hex.slice(0, 16)}`),
    low: BigInt(`0x${hex.slice(16)}`),
  };
}
function timestamp(value: number) { return new Date(value).toLocaleString([], { month: "short", day: "numeric", hour: "2-digit", minute: "2-digit" }); }
function lastSeenLabel(value: number, online: boolean) { return value > 0 ? `${online ? "ONLINE · " : ""}LAST SEEN ${timestamp(value)}` : "LAST SEEN UNKNOWN"; }
function sensorSummary(data: EdgezSensorData) {
  if (typeof data.temperature === "number") return `${data.temperature.toFixed(1)}°C${typeof data.humidity === "number" ? ` · ${data.humidity.toFixed(0)}% RH` : ""}`;
  if (typeof data.mcuTemperature === "number") return `MCU ${data.mcuTemperature.toFixed(1)}°C`;
  if (typeof data.batteryVoltage === "number") return `Battery ${data.batteryVoltage.toFixed(2)} V`;
  if (typeof data.vibrationAverage === "number") return `Vibration ${data.vibrationAverage.toFixed(2)}`;
  if (typeof data.latitude === "number" && typeof data.longitude === "number") return `${data.latitude.toFixed(5)}, ${data.longitude.toFixed(5)}`;
  return "Sensor sample received";
}

function sensorFieldName(key: keyof EdgezSensorData) {
  return `sensor_${String(key).replace(/[A-Z]/g, (letter) => `_${letter.toLowerCase()}`)}`;
}

function sensorHistoryFromSamples(samples: readonly EdgezSensorSample[], durationMs: number): CloudSensorHistorySeries[] {
  const since = Date.now() - durationMs;
  return sensorFields.flatMap((field) => {
    const points = samples.flatMap((sample) => {
      const value = sample.data[field.key];
      return sample.timestampMs >= since && typeof value === "number" && Number.isFinite(value)
        ? [{ timestampMs: sample.timestampMs, value }]
        : [];
    });
    return points.length ? [{ field: sensorFieldName(field.key), label: field.label, points }] : [];
  });
}

function SensorHistoryChart({ series, durationMs, allSeries }: {
  series: CloudSensorHistorySeries[];
  durationMs: number;
  allSeries: CloudSensorHistorySeries[];
}) {
  const [width, setWidth] = useState(0);
  const height = 190;
  const inset = 18;
  if (!series.length) return <View style={styles.sensorChartEmpty}><Text style={styles.muted}>{allSeries.length ? "Select a sensor field to display." : "No sensor history in this range."}</Text></View>;
  const values = series.flatMap((item) => item.points.map((point) => point.value));
  const rawMin = Math.min(...values);
  const rawMax = Math.max(...values);
  const padding = Math.max(0.05, (rawMax - rawMin) * 0.15);
  const min = rawMin - padding;
  const max = rawMax + padding;
  const end = Date.now();
  const start = end - durationMs;
  return <View style={styles.sensorChartPlot} onLayout={(event) => setWidth(event.nativeEvent.layout.width)}>
    {[0, 1, 2, 3].map((line) => <View key={line} style={[styles.sensorChartGridLine, { top: inset + line * (height - inset * 2) / 3 }]} />)}
    {width > 0 ? series.map((item) => {
      const colorIndex = Math.max(0, allSeries.findIndex((candidate) => candidate.field === item.field));
      const color = sensorChartColors[colorIndex % sensorChartColors.length];
      const sampled = item.points.length <= 160 ? item.points : item.points.filter((_, index) => index % Math.ceil(item.points.length / 160) === 0 || index === item.points.length - 1);
      const points = sampled.map((point) => ({
        x: inset + Math.max(0, Math.min(1, (point.timestampMs - start) / durationMs)) * (width - inset * 2),
        y: inset + (1 - (point.value - min) / (max - min)) * (height - inset * 2),
      }));
      return <React.Fragment key={item.field}>
        {points.slice(1).map((point, index) => {
          const previous = points[index];
          const dx = point.x - previous.x;
          const dy = point.y - previous.y;
          const length = Math.sqrt(dx * dx + dy * dy);
          const angle = Math.atan2(dy, dx) * 180 / Math.PI;
          return <View key={index} style={[styles.sensorChartSegment, { backgroundColor: color, width: length, left: (previous.x + point.x) / 2 - length / 2, top: (previous.y + point.y) / 2 - 1, transform: [{ rotateZ: `${angle}deg` }] }]} />;
        })}
        {points.map((point, index) => <View key={`dot-${index}`} style={[styles.sensorChartDot, { backgroundColor: color, left: point.x - 2.5, top: point.y - 2.5 }]} />)}
      </React.Fragment>;
    }) : null}
    <Text style={[styles.sensorChartAxis, { top: 2 }]}>{rawMax.toFixed(2)}</Text>
    <Text style={[styles.sensorChartAxis, { bottom: 2 }]}>{rawMin.toFixed(2)}</Text>
  </View>;
}

const styles = StyleSheet.create({
  page: { flex: 1, backgroundColor: "#f7faf9" },
  header: { minHeight: 84, paddingHorizontal: 22, flexDirection: "row", alignItems: "center", justifyContent: "space-between", borderBottomWidth: 1, borderBottomColor: "#dce6e5" },
  headerActions: { flexDirection: "row", alignItems: "center", gap: 6 },
  eyebrow: { color: "#0a8c87", fontSize: 9, fontWeight: "900", letterSpacing: 1 }, title: { color: "#0a3037", fontSize: 24, fontWeight: "900", marginTop: 3 },
  iconButton: { width: 44, height: 44, alignItems: "center", justifyContent: "center" }, closeButton: { minWidth: 52, minHeight: 44, alignItems: "center", justifyContent: "center" }, close: { color: "#59716f", fontSize: 10, fontWeight: "900", letterSpacing: 0.6 },
  tabs: { marginHorizontal: 22, marginTop: 16, padding: 4, borderRadius: 14, backgroundColor: "#e5efed", flexDirection: "row" }, tab: { flex: 1, minHeight: 42, borderRadius: 10, alignItems: "center", justifyContent: "center" }, activeTab: { backgroundColor: "white", shadowColor: "#0a3037", shadowOpacity: 0.09, shadowRadius: 4, elevation: 2 }, tabText: { color: "#718783", fontSize: 9, fontWeight: "900", letterSpacing: 0.7 }, activeTabText: { color: "#0a8c87" },
  content: { padding: 22, paddingBottom: 42, gap: 12 }, list: { gap: 9 },
  empty: { padding: 20, borderRadius: 16, backgroundColor: "#e9f7f5", alignItems: "center", gap: 7 }, emptyTitle: { color: "#0a3037", fontSize: 17, fontWeight: "900" }, muted: { color: "#59716f", fontSize: 11, lineHeight: 16, textAlign: "center" },
  row: { minHeight: 76, padding: 13, borderWidth: 1, borderColor: "#cedbdc", borderRadius: 14, backgroundColor: "white", flexDirection: "row", alignItems: "center", gap: 11 }, rowIcon: { width: 42, height: 42, borderRadius: 21, backgroundColor: "#e9f7f5", alignItems: "center", justifyContent: "center" }, rowBody: { flex: 1, gap: 4 }, rowMeta: { alignItems: "flex-end", gap: 6 }, peerName: { color: "#385753", fontSize: 13, fontWeight: "900" }, preview: { color: "#718783", fontSize: 11 }, time: { color: "#718783", fontSize: 8 },
  back: { minHeight: 42, alignSelf: "flex-start", flexDirection: "row", alignItems: "center", gap: 4 }, actionText: { color: "#0a8c87", fontSize: 9, fontWeight: "900", letterSpacing: 0.7 },
  conversation: { minHeight: 220, padding: 12, borderWidth: 1, borderColor: "#dce6e5", borderRadius: 14, backgroundColor: "white", gap: 8 }, bubble: { maxWidth: "86%", paddingHorizontal: 12, paddingVertical: 9, borderRadius: 12, gap: 3 }, mine: { alignSelf: "flex-end", backgroundColor: "#ccefeb" }, peer: { alignSelf: "flex-start", backgroundColor: "#edf3f2" }, messageText: { color: "#0a3037", fontSize: 14, lineHeight: 19 }, messageFooter: { flexDirection: "row", alignItems: "center", justifyContent: "space-between", gap: 10 }, messageStatus: { flexShrink: 1, color: "#718783", fontSize: 8, textTransform: "uppercase" }, resendButton: { minHeight: 30, paddingHorizontal: 8, flexDirection: "row", alignItems: "center", justifyContent: "center", gap: 3 }, resendText: { color: "#0a8c87", fontSize: 8, fontWeight: "900", letterSpacing: 0.5 },
  input: { minHeight: 72, maxHeight: 140, padding: 14, borderWidth: 1, borderColor: "#cedbdc", borderRadius: 12, color: "#0a3037", backgroundColor: "white", textAlignVertical: "top" }, send: { minHeight: 52, borderRadius: 12, alignItems: "center", justifyContent: "center", backgroundColor: "#0a8c87" }, sendText: { color: "white", fontSize: 10, fontWeight: "900", letterSpacing: 0.8 }, disabled: { opacity: 0.5 }, error: { color: "#b9472f", fontSize: 11, textAlign: "center", marginTop: 4 },
  infoCard: { padding: 22, borderWidth: 1, borderColor: "#dce6e5", borderRadius: 16, backgroundColor: "white", alignItems: "center", gap: 8 }, infoIcon: { width: 62, height: 62, borderRadius: 31, backgroundColor: "#e9f7f5", alignItems: "center", justifyContent: "center", marginBottom: 4 }, infoTitle: { color: "#0a3037", fontSize: 21, fontWeight: "900" }, infoRow: { width: "100%", minHeight: 48, marginTop: 8, borderTopWidth: StyleSheet.hairlineWidth, borderTopColor: "#dce6e5", flexDirection: "row", alignItems: "center", justifyContent: "space-between", gap: 12 }, infoLabel: { color: "#718783", fontSize: 9, fontWeight: "900", letterSpacing: 0.7 }, infoValue: { flex: 1, color: "#0a3037", fontSize: 11, fontWeight: "700", textAlign: "right" },
  setupCard: { padding: 16, borderWidth: 1, borderColor: "#dce6e5", borderRadius: 16, backgroundColor: "white", gap: 11 }, setupTitle: { color: "#0a3037", fontSize: 13, fontWeight: "900", letterSpacing: 0.7 }, setupLabel: { color: "#718783", fontSize: 9, fontWeight: "900", letterSpacing: 0.7, marginTop: 2 }, setupInput: { minHeight: 48, paddingHorizontal: 13, borderWidth: 1, borderColor: "#cedbdc", borderRadius: 11, color: "#0a3037", backgroundColor: "#f7faf9" }, choiceRow: { flexDirection: "row", gap: 7 }, choiceButton: { flex: 1, minHeight: 40, borderWidth: 1, borderColor: "#bcd0cd", borderRadius: 10, alignItems: "center", justifyContent: "center" }, choiceButtonSelected: { borderColor: "#0a8c87", backgroundColor: "#0a8c87" }, choiceText: { color: "#59716f", fontSize: 9, fontWeight: "900" }, choiceTextSelected: { color: "white" }, locationValue: { color: "#59716f", fontSize: 10 }, iconGrid: { flexDirection: "row", flexWrap: "wrap", gap: 8 }, iconChoice: { width: 40, height: 40, borderWidth: 1, borderColor: "#bcd0cd", borderRadius: 10, alignItems: "center", justifyContent: "center", backgroundColor: "#f7faf9" }, iconChoiceSelected: { borderColor: "#0a8c87", backgroundColor: "#0a8c87" }, colorRow: { flexDirection: "row", flexWrap: "wrap", gap: 10 }, colorChoice: { width: 28, height: 28, borderRadius: 14, borderWidth: 2, borderColor: "white" }, colorChoiceSelected: { borderColor: "#0a3037", transform: [{ scale: 1.12 }] }, encryptionNotice: { padding: 10, borderRadius: 10, backgroundColor: "#e9f7f5", flexDirection: "row", alignItems: "center", gap: 8 }, encryptionText: { flex: 1, color: "#385753", fontSize: 10, lineHeight: 14 },
  sensorHero: { padding: 17, borderRadius: 14, backgroundColor: "#e9f7f5", flexDirection: "row", alignItems: "center", gap: 13 }, sensorHeroText: { flex: 1, gap: 3 }, sensorTitle: { color: "#0a3037", fontSize: 18, fontWeight: "900" }, statsGrid: { gap: 9 }, statCard: { padding: 15, borderWidth: 1, borderColor: "#dce6e5", borderRadius: 14, backgroundColor: "white", gap: 5 }, statLabel: { color: "#0a8c87", fontSize: 9, fontWeight: "900", letterSpacing: 0.7 }, statValue: { color: "#0a3037", fontSize: 24, fontWeight: "900" }, statRange: { color: "#718783", fontSize: 9, lineHeight: 14 }, sampleTime: { color: "#718783", fontSize: 9, textAlign: "center", marginTop: 4 },
  sensorChartCard: { padding: 14, borderWidth: 1, borderColor: "#dce6e5", borderRadius: 16, backgroundColor: "white", gap: 11 },
  sensorChartHeader: { flexDirection: "row", alignItems: "center", justifyContent: "space-between" }, sensorChartTitle: { color: "#0a3037", fontSize: 15, fontWeight: "900" }, sensorChartSubtitle: { color: "#718783", fontSize: 9, marginTop: 2 },
  sensorRangeRow: { flexDirection: "row", gap: 6 }, sensorRangeButton: { minWidth: 46, minHeight: 34, paddingHorizontal: 11, borderWidth: 1, borderColor: "#cedbdc", borderRadius: 9, alignItems: "center", justifyContent: "center" }, sensorRangeButtonActive: { borderColor: "#0a8c87", backgroundColor: "#0a8c87" }, sensorRangeText: { color: "#59716f", fontSize: 9, fontWeight: "900" }, sensorRangeTextActive: { color: "white" },
  sensorPicker: { flexDirection: "row", flexWrap: "wrap", gap: 6 }, sensorPickerItem: { minHeight: 32, paddingHorizontal: 9, borderWidth: 1, borderColor: "#dce6e5", borderRadius: 9, flexDirection: "row", alignItems: "center", gap: 5, opacity: 0.58 }, sensorPickerItemActive: { borderColor: "#9bcac4", backgroundColor: "#eef9f7", opacity: 1 }, sensorPickerDot: { width: 7, height: 7, borderRadius: 4 }, sensorPickerText: { color: "#385753", fontSize: 9, fontWeight: "800" }, sensorPickerCount: { color: "#718783", fontSize: 8 },
  sensorChartPlot: { height: 190, overflow: "hidden", borderRadius: 10, backgroundColor: "#f8fbfa" }, sensorChartEmpty: { height: 150, borderRadius: 10, backgroundColor: "#f8fbfa", alignItems: "center", justifyContent: "center", padding: 16 }, sensorChartGridLine: { position: "absolute", left: 18, right: 18, height: StyleSheet.hairlineWidth, backgroundColor: "#dce6e5" }, sensorChartSegment: { position: "absolute", height: 2, borderRadius: 1 }, sensorChartDot: { position: "absolute", width: 5, height: 5, borderRadius: 3 }, sensorChartAxis: { position: "absolute", right: 5, color: "#718783", fontSize: 8 }, sensorChartFooter: { flexDirection: "row", justifyContent: "space-between" },
});
