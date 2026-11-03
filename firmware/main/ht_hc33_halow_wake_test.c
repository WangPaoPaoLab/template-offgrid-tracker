#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "edgez_pawr_protocol.h"
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

static void receive_frame(void *ctx, const struct edgez_umac_rx_frame *frame)
{
    (void)ctx;
    (void)frame;
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
             "ready: USER button broadcasts wake actions on %s %lu kHz/%u MHz",
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
    const gpio_num_t button =
        (gpio_num_t)CONFIG_OFFGRID_HT_HC33_HALOW_WAKE_TEST_BUTTON_GPIO;

    for (;;) {
        xSemaphoreTake(button_sem, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(WAKE_TEST_BUTTON_DEBOUNCE_MS));
        if (gpio_get_level(button) != 0) {
            continue;
        }
        const TickType_t burst_started = xTaskGetTickCount();
        unsigned int queued = 0U;
        unsigned int failed = 0U;

        do {
            const esp_err_t err = send_wake_action();

            if (err == ESP_OK) {
                queued++;
            } else {
                failed++;
            }
            vTaskDelay(pdMS_TO_TICKS(WAKE_TEST_BURST_INTERVAL_MS));
        } while ((xTaskGetTickCount() - burst_started) <
                 pdMS_TO_TICKS(WAKE_TEST_BURST_DURATION_MS));
        ESP_LOGI(TAG, "USER button wake burst complete queued=%u failed=%u",
                 queued, failed);
        while (gpio_get_level(button) == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
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
    ESP_ERROR_CHECK(start_radio());
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
