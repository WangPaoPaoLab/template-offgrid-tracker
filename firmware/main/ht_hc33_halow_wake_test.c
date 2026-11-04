#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "edgez_pawr_protocol.h"
#include "edgez_pawr_twt.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/queue.h"
#include "edgez_rf_api.h"
#include "edgez_umac.h"
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
#define WAKE_TEST_BURST_DURATION_MS 2500U
#define WAKE_TEST_BURST_INTERVAL_MS 75U

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

static void twt_resync(const char *reason);

static void receive_frame(void *ctx, const struct edgez_umac_rx_frame *frame)
{
    (void)ctx;
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
    } else if (!edgez_pawr_decode_request(action.payload, action.payload_length, &request) ||
        request.type != EDGEZ_PAWR_REQUEST_TWT_CONTROL || request.compressed_ssid != ssid ||
        !edgez_pawr_twt_decode(request.payload, request.payload_length, &event.msg) ||
        event.msg.kind == EDGEZ_PAWR_TWT_HIT || event.msg.kind == EDGEZ_PAWR_TWT_PROBE) return;
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

static void twt_receive_pending(void)
{
    struct twt_rx_event event;
    while (xQueueReceive(twt_queue, &event, 0) == pdTRUE) {
        struct edgez_pawr_twt *msg = &event.msg;
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

static esp_err_t start_radio(void)
{
    const struct edgez_rf_backend_ops *driver = edgez_rf_driver_ops();
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
        .beacon_interval_tu = 1000U,
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
             "ready: USER button or serial 'w' broadcasts wake actions on %s %lu kHz/%u MHz",
             WAKE_TEST_COUNTRY, (unsigned long)WAKE_TEST_FREQUENCY_KHZ,
             WAKE_TEST_BANDWIDTH_MHZ);
    return ESP_OK;
}

static esp_err_t send_wake_action(void)
{
    uint8_t packet[EDGEZ_PAWR_HEADER_LENGTH + sizeof(wake_payload)];
    uint8_t frame[EDGEZ_PAWR_ACTION_HEADER_LENGTH + sizeof(packet)];
    size_t packet_length = 0U;
    size_t frame_length = 0U;
    const uint32_t compressed_ssid = edgez_pawr_compress_ssid(
        (const uint8_t *)WAKE_TEST_SSID, strlen(WAKE_TEST_SSID));

    if (!edgez_pawr_encode_typed_request(
            packet, sizeof(packet), EDGEZ_PAWR_REQUEST_MESH_PACKET,
            ++request_id, 0U, compressed_ssid, wake_payload,
            sizeof(wake_payload), &packet_length) ||
        !edgez_pawr_encode_action_frame(
            frame, sizeof(frame), sequence_number++, radio_info.mac_addr,
            broadcast_mac, packet, packet_length, &frame_length)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const struct edgez_umac_tx_request request = {
        .token = ++tx_token,
        .frame = frame,
        .frame_length = frame_length,
        .frame_type = EDGEZ_UMAC_TX_MANAGEMENT,
        .tid = 8U,
        .mcs = 0U,
        .attempts = 1U,
        .bandwidth_mhz = WAKE_TEST_BANDWIDTH_MHZ,
        .no_ack = true,
        .immediate_status = true,
    };
    if (edgez_umac_transmit(&wake_umac, &request) != EDGEZ_UMAC_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
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
    uint64_t hello_at = 0, manual_at = 0;
    uint32_t manual_sequence = 0;
    for (;;) {
        twt_receive_pending();
        uint64_t now = esp_timer_get_time();
        if (twt_pending && !twt_active && now - offer_received_us > 5000000) {
            twt_pending = false;
        }
        if (twt_active && expires_us && now >= expires_us) {
            ESP_LOGI(TAG, "PAWR_TWT lease expired");
            twt_active = twt_pending = false;
            twt_session = esp_random() | 1U;
            manual_at = 0;
        }
        if (!twt_active && now >= hello_at) {
            struct edgez_pawr_twt hello = { .kind = EDGEZ_PAWR_TWT_HELLO, .session = twt_session };
            (void)send_twt(&hello, broadcast_mac);
            hello_at = now + 1000000;
        }
        uint8_t command = 0;
        (void)uart_read_bytes(UART_NUM_0, &command, 1, pdMS_TO_TICKS(2));
        bool button_trigger = xSemaphoreTake(button_sem, 0) == pdTRUE;
        if (button_trigger) {
            vTaskDelay(pdMS_TO_TICKS(WAKE_TEST_BUTTON_DEBOUNCE_MS));
            button_trigger = gpio_get_level(button) == 0;
        }
        now = esp_timer_get_time();
        if (button_trigger || command == 'w') user_wake_pending = true;
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
            if ((!manual_at && user_wake_pending && !manual_waiting) || button_trigger || command == 'w') {
                uint64_t first = (uint64_t)((int64_t)window_start_us +
                    schedule.duration_us / 2 + phase_adjust_us);
                uint64_t slot = now + 2000 <= first ? 0 :
                    (now + 2000 - first + schedule.interval_us - 1) / schedule.interval_us;
                manual_at = first + slot * schedule.interval_us;
                manual_sequence = 0x80000000U | ((uint32_t)(slot % 15) * 2);
                ESP_LOGI(TAG, "PAWR_TWT USER/serial queued planned=inside host_us=%llu",
                    (unsigned long long)manual_at);
            }
            bool manual_due = manual_at && now >= manual_at;
            /* Only application demand schedules a transmission. */
            if (!pending_probe_us && manual_due) {
                uint64_t deadline = manual_at;
                struct edgez_pawr_twt probe = schedule;
                probe.kind = EDGEZ_PAWR_TWT_PROBE;
                probe.sequence = manual_sequence;
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
                manual_at = 0;
            }
            continue;
        }
        manual_at = 0;
        if (!button_trigger && command != 'w') continue;
        ESP_LOGI(TAG, "no negotiated schedule; legacy discovery wake burst");
        const TickType_t started = xTaskGetTickCount();
        do {
            (void)send_wake_action();
            vTaskDelay(pdMS_TO_TICKS(WAKE_TEST_BURST_INTERVAL_MS));
        } while (xTaskGetTickCount() - started < pdMS_TO_TICKS(WAKE_TEST_BURST_DURATION_MS));
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
    if (!twt_queue) abort();
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
