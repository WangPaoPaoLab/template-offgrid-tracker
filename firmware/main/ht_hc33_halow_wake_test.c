#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "edgez_pawr_protocol.h"
#include "edgez_pawr_twt.h"
#include "edgez_mesh_s1g.h"
#include "edgez_morse_diagnostics.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/queue.h"
#include "edgez_rf_api.h"
#include "edgez_umac.h"
#include "common/morse_commands.h"
#include "common/morse_command_utils.h"
extern enum mmwlan_status mmdrv_execute_command(uint8_t *command, uint8_t *response, uint32_t *response_len);
extern enum mmwlan_status mmdrv_rm_if(uint16_t vif_id);
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define WAKE_TEST_COUNTRY "US"
#define WAKE_TEST_FREQUENCY_KHZ 914500U
#define WAKE_TEST_BANDWIDTH_MHZ 1U
#define WAKE_TEST_SSID "edgez-wake-test"
#define WAKE_TEST_BUTTON_DEBOUNCE_MS 40U

static const char *TAG = "hc33_wake_test";
static const uint8_t broadcast_mac[6] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};
static const uint8_t wake_payload[] = {'E', 'Z', 'W', 'A', 'K', 'E', '0', '1'};
static struct edgez_umac wake_umac;
static struct edgez_umac_radio_info radio_info;
static SemaphoreHandle_t button_sem;
static uint16_t sequence_number;
static uint32_t request_id;
static uintptr_t tx_token;

struct twt_rx_event {
    bool pawr_request;
    uint32_t request_id, payload_hash;
    struct edgez_pawr_twt msg;
    uint8_t peer[6];
    uint64_t received_us;
};
static QueueHandle_t twt_queue;
static struct edgez_pawr_twt schedule;
static uint8_t twt_peer[6];
static uint64_t offer_received_us, window_start_us, expires_us;
static bool twt_pending, twt_active;
static uint32_t twt_session;
static int32_t phase_adjust_us;
static struct edgez_pawr_twt_clock pace_clock;
static uint64_t pending_probe_us;
static uint32_t pending_probe_sequence;
static int32_t pending_probe_adjust;
static unsigned missing_feedback;
static bool user_wake_pending;
static uint64_t user_wake_at;
static uint32_t user_wake_sequence;
static uint32_t last_pawr_id, last_pawr_hash;
static uint64_t last_pawr_us;
static uint8_t last_pawr_peer[6];

static void capture_mac_diagnostics(void)
{
    static uint8_t response[EDGEZ_MORSE_MAC_DIAGNOSTICS_CAPACITY];
    uint32_t bytes = 0;
    int rc = edgez_morse_read_mac_diagnostics(response, sizeof(response), &bytes);
    ESP_LOGI(TAG, "MAC_SNAPSHOT rc=%d bytes=%lu; diagnostic command wakes transport", rc, (unsigned long)bytes);
    if (rc != 0) return;
    static const char hex[] = "0123456789abcdef";
    for (uint32_t offset = 0; offset < bytes; offset += 32) {
        char line[65];
        uint32_t n = bytes - offset;
        if (n > 32) n = 32;
        for (uint32_t i = 0; i < n; ++i) {
            line[2*i] = hex[response[offset+i] >> 4];
            line[2*i+1] = hex[response[offset+i] & 15];
        }
        line[2*n] = 0;
        ESP_LOGI(TAG, "MAC_SNAPSHOT_DATA offset=%lu hex=%s", (unsigned long)offset, line);
    }
}

/* Explicit lab control only: native S1G timing beacons, never discovery. */
static uint64_t sync_beacon_until;
static bool sync_ap_mode;

static void sync_beacon_stop(void)
{
    if (!sync_beacon_until) return;
    int rc = wake_umac.beaconing ? edgez_umac_disable_beaconing(&wake_umac) : EDGEZ_UMAC_OK;
    if (rc == EDGEZ_UMAC_OK && wake_umac.beacon_template_set) {
        rc = wake_umac.driver->set_beacon_template(wake_umac.driver_ctx, wake_umac.vif_id, NULL);
        if (rc == 0) wake_umac.beacon_template_set = false;
    }
    ESP_LOGI(TAG, "TWT_SYNC_BEACON STOP rc=%d", rc);
    if (rc != EDGEZ_UMAC_OK) {
        sync_beacon_until = esp_timer_get_time() + 1000000;
        return;
    }
    sync_beacon_until = 0;
    rc = wake_umac.driver->configure_bss(wake_umac.driver_ctx, wake_umac.vif_id,
        1000, 1, edgez_pawr_compress_ssid((const uint8_t *)WAKE_TEST_SSID, strlen(WAKE_TEST_SSID)));
    ESP_LOGI(TAG, "TWT_SYNC_BEACON restore_bss rc=%d", rc);
}

static void sync_beacon_start(void)
{
    if (sync_beacon_until) sync_beacon_stop();
    if (sync_beacon_until) return;
    /* S1G full beacon: 15-byte header, firmware-filled timestamp, no optional
     * header fields. Empty TIM, ESS without privacy, 100-TU beacon interval.
     * Sources: SDK dot11_s1g_beacon_hdr / hostapd S1G beacon construction. */
    uint8_t frame[128] = {0x1c, 0x00};
    memcpy(frame + 4, radio_info.mac_addr, 6);
    size_t n = 15;
    /* Follow Morse Linux morse_ext_s1g_beacon_ies_order: compatibility,
     * TIM, capabilities, operation, short interval, SSID. */
    static const uint8_t timing_ies[] = {
        213, 8, 1, 0, 100, 0, 0, 0, 0, 0, /* Beacon compatibility, TSF high filled by firmware. */
        5, 2, 0, 1,                    /* SDK empty S1G TIM: count and period only. */
    };
    memcpy(frame + n, timing_ies, sizeof(timing_ies)); n += sizeof(timing_ies);
    memcpy(frame + n, edgez_mesh_s1g_default_capabilities_ie,
        EDGEZ_MESH_S1G_CAPABILITIES_IE_LEN);
    frame[n + 2 + 4] |= 0x10; /* Non-TIM support. */
    frame[n + 2 + 8] |= 0x40; /* TWT responder support. */
    n += EDGEZ_MESH_S1G_CAPABILITIES_IE_LEN;
    static const uint8_t operation_ies[] = {
        232, 6, 1, 1, 25, 25, 0xfc, 0xff, /* US 914.5 MHz, primary/operating BW 1 MHz. */
        214, 2, 100, 0,
    };
    memcpy(frame + n, operation_ies, sizeof(operation_ies)); n += sizeof(operation_ies);
    frame[n++] = 0; frame[n++] = strlen(WAKE_TEST_SSID);
    memcpy(frame + n, WAKE_TEST_SSID, strlen(WAKE_TEST_SSID)); n += strlen(WAKE_TEST_SSID);
    int rc = wake_umac.driver->configure_bss(wake_umac.driver_ctx, wake_umac.vif_id,
        100, 1, edgez_pawr_compress_ssid((const uint8_t *)WAKE_TEST_SSID, strlen(WAKE_TEST_SSID)));
    if (rc == 0) {
        const struct edgez_rf_beacon_template beacon = {
            .frame = frame, .frame_length = n, .mcs = 0, .bandwidth_mhz = 1,
        };
        rc = edgez_umac_enable_beaconing(&wake_umac, &beacon);
    }
    /* Arm cleanup even on error, so partially applied BSS settings are restored. */
    sync_beacon_until = esp_timer_get_time() + 30000000;
    ESP_LOGI(TAG, "TWT_SYNC_BEACON START rc=%d bytes=%u interval_tu=100 duration_ms=30000", rc, (unsigned)n);
    if (rc != 0) sync_beacon_stop();
}

/* Bounded passive PM audit. Duplicate monitor copies are filtered below. */
struct pm_audit_event {
    uint16_t fc, length, seq, qos;
    uint8_t ra[6], ta[6], bssid[6], vif;
    int16_t rssi;
};
static QueueHandle_t pm_audit_queue;
static atomic_bool pm_audit_enabled;
static atomic_uint pm_audit_monitor = ATOMIC_VAR_INIT(UINT16_MAX);
static atomic_uint pm_audit_frames, pm_audit_nulls, pm_audit_dropped, pm_audit_fcs;
static uint64_t pm_audit_until;
static unsigned pm_audit_printed;

static void pm_audit_receive(const struct edgez_umac_rx_frame *frame)
{
    if (!atomic_load(&pm_audit_enabled) || !frame || !frame->data ||
        frame->format != EDGEZ_UMAC_RX_RAW_80211 || frame->length < 16) return;
    atomic_fetch_add(&pm_audit_frames, 1);
    uint16_t fc = frame->data[0] | ((uint16_t)frame->data[1] << 8);
    unsigned type = (fc >> 2) & 3, subtype = (fc >> 4) & 15;
    bool null = type == 2 && (subtype == 4 || subtype == 12);
    if (null) atomic_fetch_add(&pm_audit_nulls, 1);
    /* Include action frames to verify the observer actually sees this peer. */
    if (!null && type != 0 && !(type == 1 && subtype == 10)) return;
    struct pm_audit_event event = { .fc = fc, .length = frame->length,
        .vif = frame->vif_id, .rssi = frame->rssi_dbm };
    memcpy(event.ra, frame->data + 4, 6);
    memcpy(event.ta, frame->data + 10, 6);
    if (frame->length >= 24) {
        memcpy(event.bssid, frame->data + 16, 6);
        event.seq = frame->data[22] | ((uint16_t)frame->data[23] << 8);
    }
    if (null && subtype == 12 && frame->length >= 26 && (fc & 0x0300) != 0x0300)
        event.qos = frame->data[24] | ((uint16_t)frame->data[25] << 8);
    if (xQueueSend(pm_audit_queue, &event, 0) != pdTRUE)
        atomic_fetch_add(&pm_audit_dropped, 1);
}

static void pm_audit_stop(void)
{
    atomic_store(&pm_audit_enabled, false);
    unsigned vif = atomic_load(&pm_audit_monitor);
    enum mmwlan_status rc = vif == UINT16_MAX ? MMWLAN_SUCCESS : mmdrv_rm_if((uint16_t)vif);
    if (rc == MMWLAN_SUCCESS) atomic_store(&pm_audit_monitor, UINT16_MAX);
    pm_audit_until = 0;
    ESP_LOGI(TAG, "PM_AUDIT STOP remove_sdk=%d frames=%u nulls=%u dropped=%u fcs_stripped=%u; no RF sleep inference",
        rc, atomic_load(&pm_audit_frames), atomic_load(&pm_audit_nulls), atomic_load(&pm_audit_dropped), atomic_load(&pm_audit_fcs));
}

static void pm_audit_start(bool monitor)
{
    if (atomic_load(&pm_audit_monitor) != UINT16_MAX) { pm_audit_stop(); return; }
    uint8_t mac[6]; memcpy(mac, radio_info.mac_addr, 6); mac[0] |= 2; mac[5] ^= 0x80;
    uint16_t vif = UINT16_MAX;
    if (monitor) {
    struct morse_cmd_req_add_interface cmd = MORSE_COMMAND_INIT(cmd,
        MORSE_CMD_ID_ADD_INTERFACE, UINT16_MAX,
        .interface_type = htole32(MORSE_CMD_INTERFACE_TYPE_MON));
    memcpy(cmd.addr.octet, mac, 6);
    struct morse_cmd_resp_add_interface reply = {0};
    uint32_t bytes = sizeof(reply);
    enum mmwlan_status rc = mmdrv_execute_command((uint8_t *)&cmd, (uint8_t *)&reply, &bytes);
    if (rc == MMWLAN_SUCCESS && (bytes != sizeof(reply) ||
        le16toh(reply.hdr.message_id) != MORSE_CMD_ID_ADD_INTERFACE || le32toh(reply.status) != 0))
        rc = MMWLAN_ERROR;
    if (rc == MMWLAN_SUCCESS) vif = le16toh(reply.hdr.vif_id);
    if (rc != MMWLAN_SUCCESS) { ESP_LOGE(TAG, "PM_AUDIT monitor rejected sdk=%d", rc); return; }
    const struct edgez_umac_channel channel = {
        .operating_frequency_hz = WAKE_TEST_FREQUENCY_KHZ * 1000U,
        .operating_bw_mhz = 1, .primary_bw_mhz = 1, .primary_1mhz_channel_index = 0,
    };
    int tuned = wake_umac.driver->set_channel(wake_umac.driver_ctx, &channel);
    if (tuned != 0) {
        ESP_LOGE(TAG, "PM_AUDIT channel reapply failed rc=%d", tuned);
        (void)mmdrv_rm_if(vif); return;
    }
    }

    xQueueReset(pm_audit_queue);
    atomic_store(&pm_audit_frames, 0); atomic_store(&pm_audit_nulls, 0); atomic_store(&pm_audit_dropped, 0);
    atomic_store(&pm_audit_fcs, 0);
    pm_audit_printed = 0;
    atomic_store(&pm_audit_monitor, vif);
    pm_audit_until = esp_timer_get_time() + 30000000;
    atomic_store(&pm_audit_enabled, true);
    ESP_LOGI(TAG, "PM_AUDIT START monitor=%u duration_ms=30000 scan=unchanged no_probe_tx", vif);
}

static void pm_audit_poll(void)
{
    struct pm_audit_event e;
    while (xQueueReceive(pm_audit_queue, &e, 0) == pdTRUE) {
        if (pm_audit_printed++ >= 64) continue;
        ESP_LOGI(TAG, "PM_AUDIT RX fc=%04x pm=%u retry=%u tods=%u fromds=%u len=%u vif=%u rssi=%d seq=%u qos=%04x ra=%02x:%02x:%02x:%02x:%02x:%02x ta=%02x:%02x:%02x:%02x:%02x:%02x addr3=%02x:%02x:%02x:%02x:%02x:%02x",
            e.fc, !!(e.fc & 0x1000), !!(e.fc & 0x0800), !!(e.fc & 0x0100), !!(e.fc & 0x0200),
            e.length, e.vif, e.rssi, e.seq >> 4, e.qos,
            e.ra[0],e.ra[1],e.ra[2],e.ra[3],e.ra[4],e.ra[5],
            e.ta[0],e.ta[1],e.ta[2],e.ta[3],e.ta[4],e.ta[5],
            e.bssid[0],e.bssid[1],e.bssid[2],e.bssid[3],e.bssid[4],e.bssid[5]);
    }
    if (pm_audit_until && (uint64_t)esp_timer_get_time() >= pm_audit_until) pm_audit_stop();
}

static void twt_resync(const char *reason);

static void receive_frame(void *ctx, const struct edgez_umac_rx_frame *frame)
{
    (void)ctx;
    struct edgez_umac_rx_frame normalized;
    if (frame && frame->data && frame->length >= 20 &&
        frame->vif_id == atomic_load(&pm_audit_monitor)) {
        /* MON packets can retain FCS without the metadata flag. Strip only
         * after validating the full IEEE CRC-32, never by length alone. */
        size_t n = frame->length - 4;
        const uint8_t *fcs = frame->data + n;
        uint32_t received = (uint32_t)fcs[0] | ((uint32_t)fcs[1] << 8) |
            ((uint32_t)fcs[2] << 16) | ((uint32_t)fcs[3] << 24);
        if (edgez_pawr_compress_ssid(frame->data, n) == received) {
            normalized = *frame; normalized.length = n; frame = &normalized;
            atomic_fetch_add(&pm_audit_fcs, 1);
        }
    }
    pm_audit_receive(frame);
    if (frame && frame->data && atomic_load(&pm_audit_enabled)) {
        /* Firmware may route all RX to MON, or provide a second copy. Keep
         * PAwR operational in either case without processing copies twice. */
        static struct { uint32_t hash; uint64_t at; } seen[8];
        static unsigned next;
        uint32_t hash = edgez_pawr_payload_hash(frame->data, frame->length);
        uint64_t now = esp_timer_get_time();
        for (unsigned i = 0; i < 8; ++i)
            if (seen[i].at && seen[i].hash == hash && now - seen[i].at < 10000) return;
        seen[next].hash = hash; seen[next].at = now; next = (next + 1) % 8;
    }
    struct edgez_pawr_action_view action;
    struct edgez_pawr_request_view request;
    struct twt_rx_event event = { .received_us = esp_timer_get_time() };
    if (!frame || !twt_queue ||
        !edgez_pawr_decode_action_frame(frame->data, frame->length, &action) ||
        (action.source[0] & 1U) || !memcmp(action.source, "\0\0\0\0\0\0", 6) ||
        (memcmp(action.destination, radio_info.mac_addr, 6) &&
         memcmp(action.destination, broadcast_mac, 6))) return;
    uint32_t ssid = edgez_pawr_compress_ssid((const uint8_t *)WAKE_TEST_SSID, strlen(WAKE_TEST_SSID));
    size_t base;
    if (edgez_pawr_twt_unwrap(action.payload, action.payload_length, &base, &event.msg)) {
        uint32_t hash, received_ssid;
        if (!edgez_pawr_decode_response(action.payload, base, &hash, &received_ssid) ||
            received_ssid != ssid || hash != edgez_pawr_payload_hash(wake_payload, sizeof(wake_payload))) return;
        if (event.msg.kind != EDGEZ_PAWR_TWT_HIT) return;
    } else {
        if (!edgez_pawr_decode_request(action.payload, action.payload_length, &request) ||
            request.compressed_ssid != ssid) return;
        if (request.type == EDGEZ_PAWR_REQUEST_BEACON) {
            event.pawr_request = true;
            event.request_id = request.request_id;
            event.payload_hash = edgez_pawr_payload_hash(request.payload, request.payload_length);
        } else if (request.type != EDGEZ_PAWR_REQUEST_TWT_CONTROL ||
            !edgez_pawr_twt_decode(request.payload, request.payload_length, &event.msg) ||
            event.msg.kind == EDGEZ_PAWR_TWT_HELLO ||
            event.msg.kind == EDGEZ_PAWR_TWT_HIT || event.msg.kind == EDGEZ_PAWR_TWT_PROBE) return;
    }
    memcpy(event.peer, action.source, 6);
    (void)xQueueSend(twt_queue, &event, 0);
}

static bool send_twt(const struct edgez_pawr_twt *msg, const uint8_t peer[6])
{
    uint8_t payload[EDGEZ_PAWR_TWT_LENGTH];
    uint8_t packet[EDGEZ_PAWR_HEADER_LENGTH + sizeof(wake_payload) + EDGEZ_PAWR_TWT_TRAILER_LENGTH];
    uint8_t frame[EDGEZ_PAWR_ACTION_HEADER_LENGTH + sizeof(packet)];
    size_t packet_length, frame_length;
    uint32_t ssid = edgez_pawr_compress_ssid((const uint8_t *)WAKE_TEST_SSID, strlen(WAKE_TEST_SSID));
    if (msg->kind == EDGEZ_PAWR_TWT_PROBE) {
        /* Timing rides on the real USER command; its ordinary hash ACK
         * carries feedback. Never originate a calibration-only request. */
        if (!(msg->sequence & 0x80000000U) ||
            !edgez_pawr_encode_mesh_packet(packet, sizeof(packet), ++request_id,
                ssid, wake_payload, sizeof(wake_payload), &packet_length) ||
            !edgez_pawr_twt_append(packet, sizeof(packet), &packet_length, msg)) return false;
    } else if (!edgez_pawr_twt_encode(payload, sizeof(payload), msg) ||
        !edgez_pawr_encode_typed_request(packet, sizeof(packet), EDGEZ_PAWR_REQUEST_TWT_CONTROL,
            ++request_id, 0, ssid, payload, sizeof(payload), &packet_length)) return false;
    if (!edgez_pawr_encode_action_frame(frame, sizeof(frame), sequence_number++,
            radio_info.mac_addr, peer, packet, packet_length, &frame_length)) return false;
    const struct edgez_umac_tx_request tx = {
        .token = ++tx_token, .frame = frame, .frame_length = frame_length,
        .frame_type = EDGEZ_UMAC_TX_MANAGEMENT, .tid = 8, .mcs = 0,
        .attempts = 1, .bandwidth_mhz = 1, .no_ack = true, .immediate_status = true,
    };
    return edgez_umac_transmit(&wake_umac, &tx) == EDGEZ_UMAC_OK;
}

static bool send_pawr_session_ack(const struct twt_rx_event *event)
{
    uint8_t packet[EDGEZ_PAWR_HEADER_LENGTH + EDGEZ_PAWR_TWT_TRAILER_LENGTH];
    uint8_t frame[EDGEZ_PAWR_ACTION_HEADER_LENGTH + sizeof(packet)];
    size_t packet_length, frame_length;
    uint32_t ssid = edgez_pawr_compress_ssid((const uint8_t *)WAKE_TEST_SSID, strlen(WAKE_TEST_SSID));
    const struct edgez_pawr_twt seed = { .kind = EDGEZ_PAWR_TWT_HELLO, .session = twt_session };
    if (!edgez_pawr_encode_response(packet, sizeof(packet), event->payload_hash, ssid, &packet_length) ||
        !edgez_pawr_twt_append(packet, sizeof(packet), &packet_length, &seed) ||
        !edgez_pawr_encode_action_frame(frame, sizeof(frame), sequence_number++,
            radio_info.mac_addr, event->peer, packet, packet_length, &frame_length)) return false;
    const struct edgez_umac_tx_request tx = {
        .token = ++tx_token, .frame = frame, .frame_length = frame_length,
        .frame_type = EDGEZ_UMAC_TX_MANAGEMENT, .tid = 8, .mcs = 0,
        .attempts = 1, .bandwidth_mhz = 1, .no_ack = true, .immediate_status = true,
    };
    return edgez_umac_transmit(&wake_umac, &tx) == EDGEZ_UMAC_OK;
}

static void twt_receive_pending(void)
{
    struct twt_rx_event event;
    while (xQueueReceive(twt_queue, &event, 0) == pdTRUE) {
        struct edgez_pawr_twt *msg = &event.msg;
        if (event.pawr_request) {
            if ((twt_active || twt_pending) && memcmp(event.peer, twt_peer, 6)) continue;
            /* A retransmission in the same ACK window reuses the session.
             * A later cycle (including reboot with request_id=1) replaces it. */
            bool duplicate = last_pawr_us && event.received_us - last_pawr_us < 100000 &&
                event.request_id == last_pawr_id && event.payload_hash == last_pawr_hash &&
                !memcmp(last_pawr_peer, event.peer, 6);
            if (!duplicate) {
                twt_active = twt_pending = false;
                pending_probe_us = 0;
                user_wake_pending = false;
                user_wake_at = 0;
                twt_session = esp_random() | 1U;
                last_pawr_id = event.request_id;
                last_pawr_hash = event.payload_hash;
                last_pawr_us = event.received_us;
                memcpy(last_pawr_peer, event.peer, 6);
            }
            bool sent = send_pawr_session_ack(&event);
            ESP_LOGI(TAG, "PAWR_TWT PAWR_ACK request=%lu session=%lu queued=%u",
                (unsigned long)event.request_id, (unsigned long)twt_session, sent);
            continue;
        }
        if (msg->session != twt_session) continue;
        if (msg->kind == EDGEZ_PAWR_TWT_HIT) {
            if (!twt_active || !pending_probe_us || memcmp(event.peer, twt_peer, 6) ||
                !edgez_pawr_twt_same_schedule(&schedule, msg) ||
                msg->round_trip_us != schedule.round_trip_us ||
                msg->sequence != pending_probe_sequence || msg->echo_host_us != pending_probe_us)
                continue;
            uint64_t elapsed = event.received_us - pending_probe_us;
            if ((msg->sequence & 0x80000001U) == 0x80000000U) user_wake_pending = false;
            pending_probe_us = 0; /* Duplicate/reordered responses cannot update twice. */
            if (elapsed <= msg->processing_us || elapsed - msg->processing_us > 30000) {
                if (++missing_feedback >= 4) twt_resync("four unusable round trips");
                continue;
            }
            uint32_t rtt = (uint32_t)(elapsed - msg->processing_us);
            expires_us = schedule.lease_ms ? event.received_us + (uint64_t)schedule.lease_ms * 1000 : 0;
            bool updated = edgez_pawr_twt_clock_update(&pace_clock, pending_probe_adjust,
                                                      msg->phase_error_us, rtt);
            missing_feedback = 0;
            if (updated) phase_adjust_us = pace_clock.phase_adjust_us;
            ESP_LOGI(TAG, "PAWR_TWT STATS sequence=%lu rtt_us=%lu error_us=%ld samples=%u good=%u min_rtt_us=%lu jitter_us=%lu adjust_us=%ld updates=%u",
                (unsigned long)msg->sequence, (unsigned long)rtt, (long)msg->phase_error_us,
                pace_clock.count, pace_clock.good, (unsigned long)pace_clock.min_rtt_us,
                (unsigned long)pace_clock.jitter_us, (long)phase_adjust_us, pace_clock.updates);
            if (pace_clock.good >= 3 && !pace_clock.reliable)
                twt_resync("uncertainty exceeds fixed window guard");
            continue;
        }
        if (msg->sequence) continue;
        if (msg->kind == EDGEZ_PAWR_TWT_OFFER && !twt_active && !msg->round_trip_us) {
            if (twt_pending && (memcmp(twt_peer, event.peer, 6) ||
                msg->reference_tsf < schedule.reference_tsf)) continue;
            /* Duplicate OFFER must not move the timing anchor. */
            if (!twt_pending || !edgez_pawr_twt_same_schedule(&schedule, msg)) {
                schedule = *msg;
                offer_received_us = event.received_us;
                memcpy(twt_peer, event.peer, 6);
                twt_pending = true;
            }
            struct edgez_pawr_twt reply = schedule;
            reply.kind = EDGEZ_PAWR_TWT_ACCEPT;
            bool sent = send_twt(&reply, twt_peer);
            ESP_LOGI(TAG, "PAWR_TWT ACCEPT session=%lu target=%llu queued=%u",
                (unsigned long)msg->session, (unsigned long long)msg->target_tsf, sent);
        } else if (twt_pending && !memcmp(twt_peer, event.peer, 6) &&
                   edgez_pawr_twt_same_schedule(&schedule, msg)) {
            if (msg->kind == EDGEZ_PAWR_TWT_COMMIT) {
                uint64_t start;
                if (!edgez_pawr_twt_sender_start(msg, offer_received_us, &start) ||
                    (!twt_active && start <= (uint64_t)esp_timer_get_time() + 100000) ||
                    (twt_active && schedule.round_trip_us != msg->round_trip_us)) continue;
                if (!twt_active) {
                    schedule = *msg;
                    window_start_us = start;
                    expires_us = schedule.lease_ms ? start + (uint64_t)schedule.lease_ms * 1000 : 0;
                    phase_adjust_us = 0;
                    memset(&pace_clock, 0, sizeof(pace_clock));
                    pending_probe_us = 0;
                    user_wake_pending = false;
                    user_wake_at = 0;
                    missing_feedback = 0;
                    twt_active = true;
                    ESP_LOGI(TAG, "PAWR_TWT ACTIVE session=%lu start_host_us=%llu rtt_us=%lu lease_ms=%lu calibration=piggyback",
                        (unsigned long)schedule.session, (unsigned long long)start,
                        (unsigned long)schedule.round_trip_us, (unsigned long)schedule.lease_ms);
                }
                struct edgez_pawr_twt reply = schedule;
                reply.kind = EDGEZ_PAWR_TWT_READY;
                (void)send_twt(&reply, twt_peer);
            } else if (msg->kind == EDGEZ_PAWR_TWT_CANCEL) {
                twt_active = twt_pending = false;
                pending_probe_us = 0;
                user_wake_pending = false;
                user_wake_at = 0;
                twt_session = esp_random() | 1U;
                ESP_LOGI(TAG, "PAWR_TWT cancelled by receiver");
            }
        }
    }
}

static void twt_resync(const char *reason)
{
    struct edgez_pawr_twt cancel = schedule;
    cancel.kind = EDGEZ_PAWR_TWT_CANCEL;
    (void)send_twt(&cancel, twt_peer);
    twt_active = twt_pending = false;
    pending_probe_us = 0;
    user_wake_pending = false;
    user_wake_at = 0;
    twt_session = esp_random() | 1U;
    ESP_LOGW(TAG, "PAWR_TWT resync required: %s; window remains 50176 us", reason);
}

static void tx_status(void *ctx, const struct edgez_umac_tx_status *status)
{
    (void)ctx;
    if ((status->status_flags & EDGEZ_RF_TX_STATUS_FAILED) != 0U) {
        ESP_LOGW(TAG, "wake action TX failed token=%lu attempts=%u",
                 (unsigned long)status->token, status->attempts);
        return;
    }
    ESP_LOGD(TAG, "wake action transmitted token=%lu attempts=%u",
             (unsigned long)status->token, status->attempts);
}

static void hardware_restart(void *ctx)
{
    (void)ctx;
    ESP_LOGE(TAG, "Morse radio requested a hardware restart");
}

static const struct edgez_umac_event_ops radio_events = {
    .rx_frame = receive_frame,
    .tx_status = tx_status,
    .hardware_restart_required = hardware_restart,
};

static int add_native_ap(void *ctx, const uint8_t *mac, uint16_t *vif)
{
    (void)ctx;
    struct morse_cmd_req_add_interface cmd = MORSE_COMMAND_INIT(cmd,
        MORSE_CMD_ID_ADD_INTERFACE, UINT16_MAX, .interface_type = MORSE_CMD_INTERFACE_TYPE_AP);
    memcpy(cmd.addr.octet, mac, sizeof(cmd.addr.octet));
    struct morse_cmd_resp_add_interface reply = {0};
    uint32_t bytes = sizeof(reply);
    enum mmwlan_status sdk = mmdrv_execute_command((uint8_t *)&cmd, (uint8_t *)&reply, &bytes);
    bool valid = bytes == sizeof(reply) && reply.hdr.message_id == MORSE_CMD_ID_ADD_INTERFACE;
    ESP_LOGI(TAG, "TWT_AP_INTERFACE sdk=%d valid=%u fw=%ld vif=%u", sdk, valid,
        valid ? (long)(int32_t)reply.status : (long)INT32_MIN, valid ? reply.hdr.vif_id : UINT16_MAX);
    if (!valid || sdk != MMWLAN_SUCCESS || reply.status != 0) return MMWLAN_ERROR;
    *vif = reply.hdr.vif_id;
    return MMWLAN_SUCCESS;
}

static esp_err_t start_radio(void)
{
    static struct edgez_rf_backend_ops ap_driver;
    const struct edgez_rf_backend_ops *driver = edgez_rf_driver_ops();
    if (sync_ap_mode) {
        ap_driver = *driver;
        ap_driver.add_mesh_interface = add_native_ap;
        driver = &ap_driver;
    }
    const uint32_t compressed_ssid = edgez_pawr_compress_ssid(
        (const uint8_t *)WAKE_TEST_SSID, strlen(WAKE_TEST_SSID));
    const struct edgez_umac_config config = {
        .country_code = WAKE_TEST_COUNTRY,
        .channel = {
            .operating_frequency_hz = WAKE_TEST_FREQUENCY_KHZ * 1000U,
            .operating_bw_mhz = WAKE_TEST_BANDWIDTH_MHZ,
            .primary_bw_mhz = 1U,
            .primary_1mhz_channel_index = 0U,
        },
        .beacon_interval_tu = sync_ap_mode ? 100U : 1000U,
        .dtim_period = 1U,
        .compressed_ssid = compressed_ssid,
        .rate_control = {
            .enabled = true,
            .initial_mcs = 0U,
            .maximum_mcs = 2U,
            .successes_to_increase = 4U,
        },
        .enable_beaconing = false,
    };

    int rc = edgez_umac_bind(&wake_umac, driver, NULL);
    if (rc == EDGEZ_UMAC_OK) {
        rc = edgez_umac_set_event_ops(&wake_umac, &radio_events, NULL);
    }
    if (rc == EDGEZ_UMAC_OK) {
        rc = edgez_rf_bind_umac(&wake_umac);
    }
    if (rc == EDGEZ_UMAC_OK) {
        rc = edgez_umac_start(&wake_umac, &config, &radio_info);
    }
    if (rc != EDGEZ_UMAC_OK) {
        edgez_rf_unbind_umac();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "ready: USER button or serial 'w' sends a wake only with an active agreement on %s %lu kHz/%u MHz",
             WAKE_TEST_COUNTRY, (unsigned long)WAKE_TEST_FREQUENCY_KHZ,
             WAKE_TEST_BANDWIDTH_MHZ);
    return ESP_OK;
}

/* Explicit lab switch, before a session starts. Reset HC33 to restore MESH. */
static void switch_to_ap_test(void)
{
    if (twt_active || sync_beacon_until || sync_ap_mode) {
        ESP_LOGW(TAG, "TWT_AP_MODE requires fresh HC33 boot before beacon/session start");
        return;
    }
    int rc = edgez_umac_stop(&wake_umac);
    if (rc != EDGEZ_UMAC_OK) {
        ESP_LOGE(TAG, "TWT_AP_MODE stop failed rc=%d", rc);
        return;
    }
    edgez_rf_unbind_umac();
    twt_pending = false;
    xQueueReset(twt_queue);
    sync_ap_mode = true;
    rc = start_radio();
    ESP_LOGI(TAG, "TWT_AP_MODE start rc=%d; raw PAwR retained; reset HC33 to restore mesh", rc);
}

static void IRAM_ATTR button_isr(void *arg)
{
    (void)arg;
    BaseType_t task_woken = pdFALSE;

    xSemaphoreGiveFromISR(button_sem, &task_woken);
    if (task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void wake_sender_task(void *arg)
{
    (void)arg;
    const gpio_num_t button = (gpio_num_t)CONFIG_OFFGRID_HT_HC33_HALOW_WAKE_TEST_BUTTON_GPIO;
    for (;;) {
        pm_audit_poll();
        twt_receive_pending();
        uint64_t now = esp_timer_get_time();
        if (sync_beacon_until && now >= sync_beacon_until) sync_beacon_stop();
        if (twt_pending && !twt_active && now - offer_received_us > 5000000) {
            twt_pending = false;
        }
        if (twt_active && expires_us && now >= expires_us) {
            ESP_LOGI(TAG, "PAWR_TWT lease expired");
            twt_active = twt_pending = false;
            pending_probe_us = 0;
            user_wake_pending = false;
            twt_session = esp_random() | 1U;
            user_wake_at = 0;
        }
        uint8_t command = 0;
        (void)uart_read_bytes(UART_NUM_0, &command, 1, pdMS_TO_TICKS(2));
        if (command == 'p') pm_audit_start(true);
        if (command == 'o') pm_audit_start(false);
        if (command == 'P') pm_audit_stop();
        if (command == 'b') sync_beacon_start();
        if (command == 'B') sync_beacon_stop();
        if (command == 'd') capture_mac_diagnostics();
        if (command == 'a') switch_to_ap_test();
        bool button_trigger = xSemaphoreTake(button_sem, 0) == pdTRUE;
        if (button_trigger) {
            vTaskDelay(pdMS_TO_TICKS(WAKE_TEST_BUTTON_DEBOUNCE_MS));
            button_trigger = gpio_get_level(button) == 0;
        }
        now = esp_timer_get_time();
        if (button_trigger || command == 'w') {
            ESP_LOGI(TAG, "PAWR_TWT USER trigger source=%s session=%lu active=%u",
                button_trigger ? "gpio" : "serial", (unsigned long)twt_session, twt_active);
            if (twt_active) user_wake_pending = true;
            else ESP_LOGW(TAG, "USER request dropped: no agreement; waiting for sensor PAwR");
        }
        if (command == 'x' && twt_active) {
            twt_resync("test stopped via serial");
            continue;
        }
        if (twt_active) {
            if (pending_probe_us && now - pending_probe_us > 100000) {
                pending_probe_us = 0;
                if (++missing_feedback >= 4) {
                    twt_resync("four missing calibration responses");
                    continue;
                }
            }
            bool manual_waiting = pending_probe_us && (pending_probe_sequence & 0x80000000U);
            if ((!user_wake_at && user_wake_pending && !manual_waiting) || button_trigger || command == 'w') {
                uint64_t first = (uint64_t)((int64_t)window_start_us +
                    schedule.duration_us / 2 + phase_adjust_us);
                uint64_t slot = now + 2000 <= first ? 0 :
                    (now + 2000 - first + schedule.interval_us - 1) / schedule.interval_us;
                user_wake_at = first + slot * schedule.interval_us;
                user_wake_sequence = 0x80000000U | ((uint32_t)(slot % 15) * 2);
                ESP_LOGI(TAG, "PAWR_TWT USER/serial queued planned=inside host_us=%llu",
                    (unsigned long long)user_wake_at);
            }
            bool manual_due = user_wake_at && now >= user_wake_at;
            /* Only application demand schedules a transmission. */
            if (!pending_probe_us && manual_due) {
                uint64_t deadline = user_wake_at;
                struct edgez_pawr_twt probe = schedule;
                probe.kind = EDGEZ_PAWR_TWT_PROBE;
                probe.sequence = user_wake_sequence;
                /* Never catch up by bursting after a stalled task. */
                probe.echo_host_us = esp_timer_get_time();
                bool sent = now - deadline <= 5000 && send_twt(&probe, twt_peer);
                if (sent) {
                    pending_probe_us = probe.echo_host_us;
                    pending_probe_sequence = probe.sequence;
                    pending_probe_adjust = phase_adjust_us;
                }
                ESP_LOGI(TAG, "PAWR_TWT TX sequence=%lu planned=%s late_us=%llu queued=%u",
                    (unsigned long)probe.sequence, (probe.sequence & 1) ? "outside" : "inside",
                    (unsigned long long)(now - deadline), sent);
                user_wake_at = 0;
            }
            continue;
        }
        user_wake_at = 0;
    }
}

void app_main(void)
{
    const gpio_num_t button =
        (gpio_num_t)CONFIG_OFFGRID_HT_HC33_HALOW_WAKE_TEST_BUTTON_GPIO;
    const gpio_config_t button_config = {
        .pin_bit_mask = UINT64_C(1) << button,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };

    ESP_ERROR_CHECK(edgez_rf_platform_init() == 0 ? ESP_OK : ESP_FAIL);
    twt_session = esp_random() | 1U;
    twt_queue = xQueueCreate(8, sizeof(struct twt_rx_event));
    pm_audit_queue = xQueueCreate(24, sizeof(struct pm_audit_event));
    if (!twt_queue || !pm_audit_queue) abort();
    ESP_ERROR_CHECK(start_radio());
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0));
    button_sem = xSemaphoreCreateBinary();
    if (button_sem == NULL) {
        ESP_LOGE(TAG, "could not allocate button semaphore");
        abort();
    }
    ESP_ERROR_CHECK(gpio_config(&button_config));
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(button, button_isr, NULL));
    if (xTaskCreate(wake_sender_task, "halow_wake_tx", 4096, NULL, 6, NULL) !=
        pdPASS) {
        ESP_LOGE(TAG, "could not start wake sender task");
        abort();
    }
}
