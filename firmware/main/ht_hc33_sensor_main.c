#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/temperature_sensor.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "cJSON.h"
#include "edgez_esp_factory_data.h"
#include "edgez_pawr_bulk.h"
#include "edgez_pawr_protocol.h"
#include "edgez_rf_api.h"
#include "edgez_sensor_settings.h"
#include "edgez_umac.h"
#include "esp_err.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "mmwlan.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "psa/crypto.h"
#include "usb_control.pb.h"

#ifndef EDGEZ_PAWR_RESPONSE_DOWNLINK_PENDING
#define EDGEZ_PAWR_RESPONSE_DOWNLINK_PENDING 0x01U
#endif

#define SENSOR_PROFILE_MAGIC UINT32_C(0x455a5331)
#define SENSOR_PROFILE_VERSION 3U
#define SENSOR_PACKET_MAX 512U
#define SENSOR_ACCEPT_QUEUE_DEPTH 2U
#define SENSOR_RESPONSE_TIMEOUT_MS 100U
#define SENSOR_ACCEPT_WINDOW_MS 200U
#define SENSOR_MCS 0U
#define SENSOR_ATTEMPTS 1U
#define SENSOR_NVS_NAMESPACE "edgez_sensor"
#define SENSOR_NVS_PROFILE_KEY "profile_v1"
#define SENSOR_NVS_DOWNLINK_ACK_KEY "dl_ack_v1"
#define SENSOR_NVS_SETTINGS_KEY "settings_v2"
#define SENSOR_RTC_CACHE_MAGIC UINT32_C(0x455a5254)
#define SENSOR_RTC_CACHE_VERSION 4U
#define SENSOR_SETTINGS_MAGIC UINT32_C(0x455a5353)
#define SENSOR_BULK_ACK_ATTEMPTS 4U
#define IEEE80211_MGMT_BSSID_OFFSET 16U
#define DOWNLINK_HEADER_LENGTH 12U
#define PROVISIONING_ENVELOPE_HEADER_LEN 48U
#define PROVISIONING_NONCE_LEN 12U
#define PROVISIONING_TAG_LEN 16U
#define BATTERY_ENABLE_GPIO GPIO_NUM_20
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_0

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t length;
    char country[3];
    char mesh_id[33];
    char passphrase[65];
    char user_name[65];
    uint8_t user_public_key[32];
    uint64_t user_id_high;
    uint64_t user_id_low;
    uint32_t frequency_khz;
    uint32_t beacon_interval_seconds;
    uint8_t bandwidth_mhz;
    uint8_t marker;
    bool cloud_accepted;
    bool share_location;
    float latitude;
    float longitude;
    char icon_style[17];
    uint8_t gateway_mac[6];
    uint32_t crc32;
} sensor_profile_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t length;
    struct edgez_sensor_settings value;
    uint32_t crc32;
} sensor_settings_record_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t length;
    bool has_profile;
    sensor_profile_t profile;
    sensor_settings_record_t settings;
    uint32_t downlink_ack_id;
    uint32_t block_sequence;
    uint8_t sample_count;
    float voltage_samples[EDGEZ_SENSOR_SETTINGS_MAX_SAMPLES_PER_BLOCK];
    uint32_t crc32;
} sensor_rtc_cache_t;

typedef struct {
    size_t length;
    uint8_t source[6];
    uint8_t payload[SENSOR_PACKET_MAX];
} acceptance_item_t;

static const char *TAG = "hc33_sensor";
static const uint8_t envelope_magic[] = {'E', 'Z', 'P', 1};
static const uint8_t conversation_label[] = "EdgeZ conversation v1";
static const uint8_t broadcast_mac[6] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};
static const uint8_t zero_mac[6];
static struct edgez_umac sensor_umac;
static struct edgez_umac_radio_info radio_info;
static sensor_profile_t profile;
static edgez_esp_factory_identity_t factory_identity;
static SemaphoreHandle_t response_sem;
static QueueHandle_t acceptance_queue;
static uint32_t expected_hash;
static uint32_t active_compressed_ssid;
static char active_mesh_id[33];
static uint16_t sequence_number;
static uintptr_t tx_token;
static uint32_t request_id;
static bool waiting_for_response;
static bool radio_running;
static uint8_t active_bandwidth_mhz = 1U;
static bool response_downlink_pending;
static uint32_t downlink_ack_id;
static sensor_settings_record_t sensor_settings;
static uint32_t block_sequence;
static uint8_t voltage_sample_count;
static float voltage_samples[EDGEZ_SENSOR_SETTINGS_MAX_SAMPLES_PER_BLOCK];
static adc_oneshot_unit_handle_t battery_adc;
static adc_cali_handle_t battery_calibration;
static bool battery_ready;
static SemaphoreHandle_t bulk_ack_sem;
static uint8_t bulk_ack_payload[EDGEZ_PAWR_BULK_CONTROL_SIZE];
static size_t bulk_ack_length;
static bool nvs_ready;
static RTC_DATA_ATTR sensor_rtc_cache_t rtc_cache;
static esp_err_t ensure_nvs_ready(void);
static uint32_t ieee_crc32(const void *data, size_t length);
static bool profile_valid(const sensor_profile_t *candidate);
static void update_rtc_cache(bool has_profile);

static esp_err_t battery_init(void)
{
    const gpio_config_t enable = {
        .pin_bit_mask = UINT64_C(1) << BATTERY_ENABLE_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&enable);
    if (err == ESP_OK) err = gpio_set_level(BATTERY_ENABLE_GPIO, 0);
    const adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = ADC_UNIT_1,
    };
    if (err == ESP_OK) err = adc_oneshot_new_unit(&unit_config, &battery_adc);
    const adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (err == ESP_OK) {
        err = adc_oneshot_config_channel(battery_adc, BATTERY_ADC_CHANNEL,
                                         &channel_config);
    }
    const adc_cali_curve_fitting_config_t calibration_config = {
        .unit_id = ADC_UNIT_1,
        .chan = BATTERY_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (err == ESP_OK) {
        err = adc_cali_create_scheme_curve_fitting(&calibration_config,
                                                    &battery_calibration);
    }
    battery_ready = err == ESP_OK;
    return err;
}

static esp_err_t battery_read(float *volts)
{
    if (!battery_ready || volts == NULL) return ESP_ERR_INVALID_STATE;
    esp_err_t err = gpio_set_level(BATTERY_ENABLE_GPIO, 1);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(10));
    int millivolts = 0;
    for (unsigned sample = 0U; sample < 16U && err == ESP_OK; ++sample) {
        int raw = 0;
        int sample_mv = 0;
        err = adc_oneshot_read(battery_adc, BATTERY_ADC_CHANNEL, &raw);
        if (err == ESP_OK) {
            err = adc_cali_raw_to_voltage(
                battery_calibration, raw, &sample_mv);
        }
        if (err == ESP_OK) millivolts += sample_mv;
    }
    (void)gpio_set_level(BATTERY_ENABLE_GPIO, 0);
    if (err == ESP_OK) {
        const int battery_mv = (millivolts / 16) * 2;
        if (battery_mv < 2500 || battery_mv > 5000) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        *volts = battery_mv / 1000.0f;
    }
    return err;
}
extern enum mmwlan_status mmdrv_set_duty_cycle(
    uint32_t duty_cycle, bool duty_cycle_omit_ctrl_resp,
    enum mmwlan_duty_cycle_mode mode);
extern enum mmwlan_status mmdrv_get_duty_cycle(
    struct mmwlan_duty_cycle_stats *stats);

#if CONFIG_OFFGRID_HT_HC33_FACTORY_RESET_BUTTON
static bool factory_reset_button_pressed(void)
{
    return gpio_get_level((gpio_num_t)
        CONFIG_OFFGRID_HT_HC33_FACTORY_RESET_GPIO) == 0;
}

static esp_err_t configure_factory_reset_button(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = UINT64_C(1) <<
            CONFIG_OFFGRID_HT_HC33_FACTORY_RESET_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&config);
}

static void handle_factory_reset_wakeup(void)
{
    ESP_ERROR_CHECK(configure_factory_reset_button());
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1 ||
        !factory_reset_button_pressed()) {
        return;
    }

    const TickType_t started = xTaskGetTickCount();
    const TickType_t required = pdMS_TO_TICKS(
        CONFIG_OFFGRID_HT_HC33_FACTORY_RESET_HOLD_MS);
    ESP_LOGI(TAG, "Reset button woke sensor; hold for %u ms to erase NVS",
             CONFIG_OFFGRID_HT_HC33_FACTORY_RESET_HOLD_MS);
    while (factory_reset_button_pressed()) {
        if (xTaskGetTickCount() - started >= required) {
            ESP_LOGW(TAG, "Factory reset armed; release button to continue");
            while (factory_reset_button_pressed()) {
                vTaskDelay(pdMS_TO_TICKS(25));
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            ESP_ERROR_CHECK(ensure_nvs_ready());
            ESP_ERROR_CHECK(nvs_flash_erase());
            nvs_ready = false;
            memset(&profile, 0, sizeof(profile));
            memset(&rtc_cache, 0, sizeof(rtc_cache));
            downlink_ack_id = 0U;
            ESP_LOGW(TAG, "NVS erased; factory_data preserved; restarting");
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    ESP_LOGI(TAG, "Factory reset cancelled before hold threshold");
}

static void enable_factory_reset_wakeup(void)
{
    const gpio_num_t gpio = (gpio_num_t)
        CONFIG_OFFGRID_HT_HC33_FACTORY_RESET_GPIO;
    ESP_ERROR_CHECK(configure_factory_reset_button());
    ESP_ERROR_CHECK(rtc_gpio_pullup_en(gpio));
    ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(gpio));
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(
        UINT64_C(1) << CONFIG_OFFGRID_HT_HC33_FACTORY_RESET_GPIO,
        ESP_EXT1_WAKEUP_ANY_LOW));
}
#else
static void handle_factory_reset_wakeup(void) {}
static void enable_factory_reset_wakeup(void) {}
#endif

static esp_err_t read_mcu_temperature(float *celsius)
{
    if (celsius == NULL) return ESP_ERR_INVALID_ARG;
    temperature_sensor_handle_t sensor = NULL;
    const temperature_sensor_config_t config =
        TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t err = temperature_sensor_install(&config, &sensor);
    if (err != ESP_OK) return err;
    err = temperature_sensor_enable(sensor);
    if (err == ESP_OK) {
        err = temperature_sensor_get_celsius(sensor, celsius);
        (void)temperature_sensor_disable(sensor);
    }
    (void)temperature_sensor_uninstall(sensor);
    return err;
}

static uint32_t ieee_crc32(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    uint32_t crc = UINT32_C(0xffffffff);
    while (length-- != 0U) {
        crc ^= *bytes++;
        for (unsigned bit = 0; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ?
                UINT32_C(0xedb88320) : 0U);
        }
    }
    return crc ^ UINT32_C(0xffffffff);
}

static bool sensor_settings_record_valid(
    const sensor_settings_record_t *record)
{
    return record != NULL && record->magic == SENSOR_SETTINGS_MAGIC &&
        record->version == EDGEZ_SENSOR_SETTINGS_VERSION &&
        record->length == sizeof(*record) &&
        edgez_sensor_settings_valid(&record->value) &&
        record->crc32 == ieee_crc32(record,
            offsetof(sensor_settings_record_t, crc32));
}

static void sensor_settings_default(uint32_t interval_ms)
{
    memset(&sensor_settings, 0, sizeof(sensor_settings));
    sensor_settings.magic = SENSOR_SETTINGS_MAGIC;
    sensor_settings.version = EDGEZ_SENSOR_SETTINGS_VERSION;
    sensor_settings.length = sizeof(sensor_settings);
    sensor_settings.value.sampling_interval_ms = interval_ms;
    sensor_settings.value.block_sampling_count = 0U;
    voltage_sample_count = 0U;
    sensor_settings.crc32 = ieee_crc32(&sensor_settings,
        offsetof(sensor_settings_record_t, crc32));
}

static esp_err_t save_sensor_settings(
    const struct edgez_sensor_settings *value)
{
    if (!edgez_sensor_settings_valid(value)) return ESP_ERR_INVALID_ARG;
    sensor_settings_record_t updated = {
        .magic = SENSOR_SETTINGS_MAGIC,
        .version = EDGEZ_SENSOR_SETTINGS_VERSION,
        .length = sizeof(sensor_settings_record_t),
        .value = *value,
    };
    updated.crc32 = ieee_crc32(&updated,
        offsetof(sensor_settings_record_t, crc32));

    nvs_handle_t handle = 0;
    esp_err_t err = ensure_nvs_ready();
    if (err == ESP_OK) {
        err = nvs_open(SENSOR_NVS_NAMESPACE, NVS_READWRITE, &handle);
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, SENSOR_NVS_SETTINGS_KEY,
                           &updated, sizeof(updated));
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle != 0) nvs_close(handle);
    if (err == ESP_OK) {
        sensor_settings = updated;
        voltage_sample_count = 0U;
        update_rtc_cache(profile_valid(&profile));
    }
    return err;
}

esp_err_t edgez_sensor_handle_downlink(
    const char *command, const uint8_t *payload, size_t payload_length)
{
    if (command == NULL || strcmp(command, EDGEZ_SENSOR_SETTINGS_COMMAND) != 0) {
        ESP_LOGW(TAG, "unsupported downlink command=%s",
                 command != NULL ? command : "(null)");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (payload == NULL || payload_length == 0U || payload_length > 256U) {
        return ESP_ERR_INVALID_SIZE;
    }
    char json[257];
    memcpy(json, payload, payload_length);
    json[payload_length] = '\0';
    cJSON *root = cJSON_ParseWithLength(json, payload_length);
    if (root == NULL) return ESP_ERR_INVALID_ARG;
    const cJSON *interval = cJSON_GetObjectItemCaseSensitive(
        root, "samplingIntervalMs");
    const cJSON *samples = cJSON_GetObjectItemCaseSensitive(
        root, "blockSamplingCount");
    struct edgez_sensor_settings updated = {0};
    bool parsed = cJSON_IsNumber(interval) && cJSON_IsNumber(samples) &&
        interval->valuedouble >= EDGEZ_SENSOR_SETTINGS_MIN_INTERVAL_MS &&
        interval->valuedouble <= EDGEZ_SENSOR_SETTINGS_MAX_INTERVAL_MS &&
        samples->valuedouble >= 0.0 &&
        samples->valuedouble <=
            EDGEZ_SENSOR_SETTINGS_MAX_SAMPLES_PER_BLOCK;
    if (parsed) {
        updated.sampling_interval_ms = (uint32_t)interval->valuedouble;
        updated.block_sampling_count = (uint16_t)samples->valuedouble;
        parsed = interval->valuedouble ==
            (double)updated.sampling_interval_ms &&
            samples->valuedouble ==
                (double)updated.block_sampling_count &&
            edgez_sensor_settings_valid(&updated);
    }
    cJSON_Delete(root);
    if (!parsed) {
        ESP_LOGW(TAG, "invalid sensor.settings payload");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = save_sensor_settings(&updated);
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "sensor settings saved interval=%lu ms block_samples=%u",
                 (unsigned long)updated.sampling_interval_ms,
                 (unsigned)updated.block_sampling_count);
    }
    return err;
}

static uint64_t mac_to_node(const uint8_t mac[6])
{
    uint64_t node = 0;
    for (size_t i = 0; i < 6U; ++i) node = (node << 8U) | mac[i];
    return node;
}

static void put_le64(uint8_t output[8], uint64_t value)
{
    for (size_t i = 0; i < 8U; ++i) output[i] = (uint8_t)(value >> (8U * i));
}

static int compare_identity(uint64_t left_node, const uint8_t left_key[32],
                            uint64_t right_node, const uint8_t right_key[32])
{
    if (left_node < right_node) return -1;
    if (left_node > right_node) return 1;
    return memcmp(left_key, right_key, 32U);
}

static bool profile_valid(const sensor_profile_t *candidate)
{
    return candidate != NULL && candidate->magic == SENSOR_PROFILE_MAGIC &&
        candidate->version == SENSOR_PROFILE_VERSION &&
        candidate->length == sizeof(*candidate) &&
        strlen(candidate->country) == 2U && candidate->mesh_id[0] != '\0' &&
        candidate->frequency_khz != 0U && candidate->bandwidth_mhz != 0U &&
        (!candidate->cloud_accepted ||
            ((candidate->gateway_mac[0] & 0x01U) == 0U &&
             memcmp(candidate->gateway_mac, zero_mac, sizeof(zero_mac)) != 0)) &&
        candidate->crc32 == ieee_crc32(candidate,
            offsetof(sensor_profile_t, crc32));
}

static esp_err_t ensure_nvs_ready(void)
{
    if (nvs_ready) return ESP_OK;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    nvs_ready = err == ESP_OK;
    return err;
}

static bool rtc_cache_valid(void)
{
    return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER &&
        rtc_cache.magic == SENSOR_RTC_CACHE_MAGIC &&
        rtc_cache.version == SENSOR_RTC_CACHE_VERSION &&
        rtc_cache.length == sizeof(rtc_cache) &&
        (!rtc_cache.has_profile || profile_valid(&rtc_cache.profile)) &&
        sensor_settings_record_valid(&rtc_cache.settings) &&
        rtc_cache.sample_count <=
            EDGEZ_SENSOR_SETTINGS_MAX_SAMPLES_PER_BLOCK &&
        (rtc_cache.settings.value.block_sampling_count > 0U ||
            rtc_cache.sample_count == 0U) &&
        rtc_cache.crc32 == ieee_crc32(&rtc_cache,
                                      offsetof(sensor_rtc_cache_t, crc32));
}

static void update_rtc_cache(bool has_profile)
{
    memset(&rtc_cache, 0, sizeof(rtc_cache));
    rtc_cache.magic = SENSOR_RTC_CACHE_MAGIC;
    rtc_cache.version = SENSOR_RTC_CACHE_VERSION;
    rtc_cache.length = sizeof(rtc_cache);
    rtc_cache.has_profile = has_profile;
    if (has_profile) rtc_cache.profile = profile;
    rtc_cache.settings = sensor_settings;
    rtc_cache.downlink_ack_id = downlink_ack_id;
    rtc_cache.block_sequence = block_sequence;
    rtc_cache.sample_count = voltage_sample_count;
    memcpy(rtc_cache.voltage_samples, voltage_samples,
           sizeof(voltage_samples));
    rtc_cache.crc32 = ieee_crc32(&rtc_cache,
                                 offsetof(sensor_rtc_cache_t, crc32));
}

static esp_err_t load_persistent_state(void)
{
    esp_err_t err = ensure_nvs_ready();
    if (err != ESP_OK) return err;
    nvs_handle_t handle;
    err = nvs_open(SENSOR_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        sensor_settings_default(
            CONFIG_OFFGRID_HT_HC33_SENSOR_INTERVAL_SECONDS * 1000U);
        return err;
    }

    size_t length = sizeof(profile);
    esp_err_t profile_err = nvs_get_blob(handle, SENSOR_NVS_PROFILE_KEY,
                                         &profile, &length);
    if (profile_err != ESP_OK || length != sizeof(profile) ||
        !profile_valid(&profile)) {
        memset(&profile, 0, sizeof(profile));
        profile_err = profile_err == ESP_OK ? ESP_ERR_INVALID_CRC : profile_err;
    }
    (void)nvs_get_u32(handle, SENSOR_NVS_DOWNLINK_ACK_KEY,
                      &downlink_ack_id);
    length = sizeof(sensor_settings);
    esp_err_t settings_err = nvs_get_blob(handle, SENSOR_NVS_SETTINGS_KEY,
                                           &sensor_settings, &length);
    nvs_close(handle);
    if (settings_err != ESP_OK || length != sizeof(sensor_settings) ||
        !sensor_settings_record_valid(&sensor_settings)) {
        uint32_t interval_ms = profile_err == ESP_OK &&
            profile.beacon_interval_seconds != 0U ?
            profile.beacon_interval_seconds * 1000U :
            CONFIG_OFFGRID_HT_HC33_SENSOR_INTERVAL_SECONDS * 1000U;
        sensor_settings_default(interval_ms);
    }
    return profile_err;
}

static esp_err_t save_profile(sensor_profile_t *candidate)
{
    candidate->magic = SENSOR_PROFILE_MAGIC;
    candidate->version = SENSOR_PROFILE_VERSION;
    candidate->length = sizeof(*candidate);
    candidate->crc32 = ieee_crc32(candidate, offsetof(sensor_profile_t, crc32));
    nvs_handle_t handle = 0;
    esp_err_t err = ensure_nvs_ready();
    if (err == ESP_OK) {
        err = nvs_open(SENSOR_NVS_NAMESPACE, NVS_READWRITE, &handle);
    }
    if (err == ESP_OK) err = nvs_set_blob(handle, SENSOR_NVS_PROFILE_KEY,
                                           candidate, sizeof(*candidate));
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle != 0) nvs_close(handle);
    if (err == ESP_OK) {
        profile = *candidate;
        update_rtc_cache(true);
    }
    return err;
}

static esp_err_t save_downlink_ack(uint32_t message_id)
{
    nvs_handle_t handle = 0;
    esp_err_t err = ensure_nvs_ready();
    if (err == ESP_OK) {
        err = nvs_open(SENSOR_NVS_NAMESPACE, NVS_READWRITE, &handle);
    }
    if (err == ESP_OK) {
        err = message_id == 0U
            ? nvs_erase_key(handle, SENSOR_NVS_DOWNLINK_ACK_KEY)
            : nvs_set_u32(handle, SENSOR_NVS_DOWNLINK_ACK_KEY, message_id);
    }
    if (handle != 0 && (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND)) {
        err = nvs_commit(handle);
    }
    if (handle != 0) nvs_close(handle);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        downlink_ack_id = message_id;
        update_rtc_cache(profile_valid(&profile));
        return ESP_OK;
    }
    return err;
}

static esp_err_t load_factory_profile(void)
{
    uint32_t frequency_khz = 0U;
    uint8_t bandwidth_mhz = 0U;
    if (!factory_identity.has_halow_profile ||
        !edgez_rf_resolve_s1g_channel(factory_identity.country,
            factory_identity.channel, &frequency_khz, &bandwidth_mhz)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&profile, 0, sizeof(profile));
    strlcpy(profile.country, factory_identity.country,
            sizeof(profile.country));
    strlcpy(profile.mesh_id, factory_identity.mesh_id,
            sizeof(profile.mesh_id));
    strlcpy(profile.passphrase, factory_identity.passphrase,
            sizeof(profile.passphrase));
    strlcpy(profile.user_name, factory_identity.serial,
            sizeof(profile.user_name));
    memcpy(profile.user_public_key, factory_identity.public_key,
           sizeof(profile.user_public_key));
    memcpy(&profile.user_id_high, factory_identity.public_key,
           sizeof(profile.user_id_high));
    memcpy(&profile.user_id_low,
           factory_identity.public_key + sizeof(profile.user_id_high),
           sizeof(profile.user_id_low));
    profile.frequency_khz = frequency_khz;
    profile.bandwidth_mhz = bandwidth_mhz;
    profile.cloud_accepted = false;
    profile.beacon_interval_seconds =
        CONFIG_OFFGRID_HT_HC33_SENSOR_INTERVAL_SECONDS;
    strlcpy(profile.icon_style, "sensor", sizeof(profile.icon_style));
    profile.magic = SENSOR_PROFILE_MAGIC;
    profile.version = SENSOR_PROFILE_VERSION;
    profile.length = sizeof(profile);
    profile.crc32 = ieee_crc32(&profile, offsetof(sensor_profile_t, crc32));
    ESP_LOGI(TAG,
             "using factory HaLow profile country=%s mesh=%s channel=%u frequency=%lu kHz bandwidth=%u MHz; discovery skipped",
             profile.country, profile.mesh_id,
             (unsigned)factory_identity.channel,
             (unsigned long)profile.frequency_khz,
             (unsigned)profile.bandwidth_mhz);
    return ESP_OK;
}

static esp_err_t build_discovery(uint8_t *output, size_t capacity,
                                 size_t *length)
{
    ai_edgez_halow_NetworkPacket packet =
        ai_edgez_halow_NetworkPacket_init_zero;
    packet.from = mac_to_node(radio_info.mac_addr);
    packet.operation = ai_edgez_halow_Operation_REQUEST;
    packet.interface = ai_edgez_halow_Interface_HALOW;
    packet.which_body = ai_edgez_halow_NetworkPacket_device_settings_tag;
    packet.body.device_settings.action =
        ai_edgez_halow_DeviceSettingsAction_DEVICE_SETTINGS_REPORT;
    strlcpy(packet.body.device_settings.user_name, factory_identity.serial,
            sizeof(packet.body.device_settings.user_name));
    memcpy(&packet.body.device_settings.user_id_high,
           factory_identity.public_key, sizeof(uint64_t));
    memcpy(&packet.body.device_settings.user_id_low,
           factory_identity.public_key + sizeof(uint64_t), sizeof(uint64_t));
    packet.body.device_settings.user_public_key.size = 32U;
    memcpy(packet.body.device_settings.user_public_key.bytes,
           factory_identity.public_key, 32U);
    packet.body.device_settings.device_type =
        ai_edgez_halow_DeviceType_DEVICE_TYPE_SENSOR;
    packet.body.device_settings.sleep_mode_enabled = true;
    pb_ostream_t stream = pb_ostream_from_buffer(output, capacity);
    if (!pb_encode(&stream, ai_edgez_halow_NetworkPacket_fields, &packet)) {
        return ESP_ERR_INVALID_SIZE;
    }
    *length = stream.bytes_written;
    return ESP_OK;
}

static esp_err_t build_beacon(uint8_t *output, size_t capacity, size_t *length,
                              float mcu_temperature,
                              bool temperature_valid)
{
    ai_edgez_halow_Beacon beacon = ai_edgez_halow_Beacon_init_zero;
    beacon.user_id_high = profile.user_id_high;
    beacon.user_id_low = profile.user_id_low;
    strlcpy(beacon.user_name, profile.user_name, sizeof(beacon.user_name));
    beacon.user_public_key.size = sizeof(profile.user_public_key);
    memcpy(beacon.user_public_key.bytes, profile.user_public_key,
           sizeof(profile.user_public_key));
    beacon.marker = (ai_edgez_halow_MarkerColor)profile.marker;
    beacon.device_type = ai_edgez_halow_DeviceType_DEVICE_TYPE_SENSOR;
    strlcpy(beacon.icon_style, profile.icon_style, sizeof(beacon.icon_style));
    if (profile.share_location) {
        beacon.latitude = profile.latitude;
        beacon.longitude = profile.longitude;
    }
    if (temperature_valid &&
        beacon.sensor_data_count <
            sizeof(beacon.sensor_data) / sizeof(beacon.sensor_data[0])) {
        ai_edgez_halow_SensorData *temperature =
            &beacon.sensor_data[beacon.sensor_data_count++];
        temperature->type =
            ai_edgez_halow_SensorType_SENSOR_MCU_TEMPERATURE;
        temperature->which_value =
            ai_edgez_halow_SensorData_float_value_tag;
        temperature->value.float_value = mcu_temperature;
        ESP_LOGI(TAG, "MCU temperature %.1f C", (double)mcu_temperature);
    } else if (!temperature_valid) {
        ESP_LOGW(TAG, "MCU temperature unavailable");
    }
    beacon.downlink_ack_id = downlink_ack_id;
    pb_ostream_t stream = pb_ostream_from_buffer(output, capacity);
    if (!pb_encode(&stream, ai_edgez_halow_Beacon_fields, &beacon)) {
        return ESP_ERR_INVALID_SIZE;
    }
    *length = stream.bytes_written;
    return ESP_OK;
}

static esp_err_t decrypt_envelope(const ai_edgez_halow_NetworkPacket *outer,
                                  uint8_t *plaintext, size_t capacity,
                                  size_t *plaintext_length)
{
    if (outer == NULL || plaintext == NULL || plaintext_length == NULL ||
        outer->which_body != ai_edgez_halow_NetworkPacket_msg_tag ||
        outer->body.msg.mime != ai_edgez_halow_Mime_MIME_BINARY ||
        outer->body.msg.payload.size <
            PROVISIONING_ENVELOPE_HEADER_LEN + PROVISIONING_TAG_LEN ||
        memcmp(outer->body.msg.payload.bytes, envelope_magic,
               sizeof(envelope_magic)) != 0 ||
        outer->to != mac_to_node(radio_info.mac_addr)) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t *envelope = outer->body.msg.payload.bytes;
    const size_t envelope_length = outer->body.msg.payload.size;
    const uint8_t *sender_key = envelope + sizeof(envelope_magic);
    const uint8_t *nonce = sender_key + 32U;
    const uint8_t *ciphertext = nonce + PROVISIONING_NONCE_LEN;
    const size_t ciphertext_length = envelope_length -
        PROVISIONING_ENVELOPE_HEADER_LEN;
    uint8_t shared[32] = {0};
    uint8_t sender_identity[40];
    uint8_t recipient_identity[40];
    uint8_t material[sizeof(conversation_label) - 1U + 32U + 40U + 40U];
    uint8_t aes_key[32] = {0};
    uint8_t aad[18U + PROVISIONING_NONCE_LEN];
    size_t shared_length = 0U;
    size_t offset = 0U;
    psa_key_id_t key_id = 0;
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) return ESP_FAIL;

    psa_set_key_type(&attributes,
        PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
    psa_set_key_bits(&attributes, 255);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DERIVE);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDH);
    status = psa_import_key(&attributes, factory_identity.private_key, 32U,
                            &key_id);
    psa_reset_key_attributes(&attributes);
    if (status == PSA_SUCCESS) {
        status = psa_raw_key_agreement(PSA_ALG_ECDH, key_id, sender_key, 32U,
                                       shared, sizeof(shared), &shared_length);
    }
    if (key_id != 0) (void)psa_destroy_key(key_id);
    key_id = 0;
    if (status != PSA_SUCCESS || shared_length != sizeof(shared)) goto fail;

    put_le64(sender_identity, outer->from);
    memcpy(sender_identity + 8U, sender_key, 32U);
    put_le64(recipient_identity, outer->to);
    memcpy(recipient_identity + 8U, factory_identity.public_key, 32U);
    memcpy(material + offset, conversation_label,
           sizeof(conversation_label) - 1U);
    offset += sizeof(conversation_label) - 1U;
    memcpy(material + offset, shared, sizeof(shared));
    offset += sizeof(shared);
    if (compare_identity(outer->from, sender_key, outer->to,
                         factory_identity.public_key) <= 0) {
        memcpy(material + offset, sender_identity, sizeof(sender_identity));
        offset += sizeof(sender_identity);
        memcpy(material + offset, recipient_identity,
               sizeof(recipient_identity));
    } else {
        memcpy(material + offset, recipient_identity,
               sizeof(recipient_identity));
        offset += sizeof(recipient_identity);
        memcpy(material + offset, sender_identity, sizeof(sender_identity));
    }
    status = psa_hash_compute(PSA_ALG_SHA_256, material, sizeof(material),
                              aes_key, sizeof(aes_key), &offset);
    if (status != PSA_SUCCESS || offset != sizeof(aes_key)) goto fail;
    put_le64(aad, outer->from);
    put_le64(aad + 8U, outer->to);
    aad[16] = PROVISIONING_NONCE_LEN;
    aad[17] = 0U;
    memcpy(aad + 18U, nonce, PROVISIONING_NONCE_LEN);

    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 256);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
    status = psa_import_key(&attributes, aes_key, sizeof(aes_key), &key_id);
    psa_reset_key_attributes(&attributes);
    if (status == PSA_SUCCESS) {
        status = psa_aead_decrypt(key_id, PSA_ALG_GCM, nonce,
            PROVISIONING_NONCE_LEN, aad, sizeof(aad), ciphertext,
            ciphertext_length, plaintext, capacity, plaintext_length);
    }
    if (key_id != 0) (void)psa_destroy_key(key_id);
    memset(shared, 0, sizeof(shared));
    memset(material, 0, sizeof(material));
    memset(aes_key, 0, sizeof(aes_key));
    return status == PSA_SUCCESS ? ESP_OK : ESP_ERR_INVALID_CRC;

fail:
    if (key_id != 0) (void)psa_destroy_key(key_id);
    memset(shared, 0, sizeof(shared));
    memset(material, 0, sizeof(material));
    memset(aes_key, 0, sizeof(aes_key));
    return ESP_ERR_INVALID_CRC;
}

static esp_err_t apply_acceptance(const uint8_t *payload, size_t length,
                                  const uint8_t acceptance_source[6])
{
    ai_edgez_halow_NetworkPacket outer =
        ai_edgez_halow_NetworkPacket_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(payload, length);
    if (!pb_decode(&stream, ai_edgez_halow_NetworkPacket_fields, &outer)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t plaintext[SENSOR_PACKET_MAX];
    size_t plaintext_length = 0U;
    esp_err_t err = decrypt_envelope(&outer, plaintext, sizeof(plaintext),
                                     &plaintext_length);
    if (err != ESP_OK) return err;

    ai_edgez_halow_NetworkPacket inner =
        ai_edgez_halow_NetworkPacket_init_zero;
    stream = pb_istream_from_buffer(plaintext, plaintext_length);
    if (!pb_decode(&stream, ai_edgez_halow_NetworkPacket_fields, &inner)) {
        memset(plaintext, 0, sizeof(plaintext));
        return ESP_ERR_INVALID_ARG;
    }
    sensor_profile_t accepted = {0};
    if (inner.which_body == ai_edgez_halow_NetworkPacket_init_tag) {
        const ai_edgez_halow_HaLowInitConfig *init = &inner.body.init;
        if (strlen(init->country_code) == 2U) {
            strlcpy(accepted.country, init->country_code,
                    sizeof(accepted.country));
        }
        strlcpy(accepted.mesh_id, init->mesh_id, sizeof(accepted.mesh_id));
        strlcpy(accepted.passphrase, init->passphrase,
                sizeof(accepted.passphrase));
        strlcpy(accepted.user_name, init->user_name,
                sizeof(accepted.user_name));
        accepted.user_id_high = init->user_id_high;
        accepted.user_id_low = init->user_id_low;
        if (init->user_public_key.size == 32U) {
            memcpy(accepted.user_public_key, init->user_public_key.bytes, 32U);
        }
        accepted.frequency_khz = init->mesh_frequency_khz;
        accepted.bandwidth_mhz = (uint8_t)init->mesh_bandwidth_mhz;
        accepted.share_location = init->has_location;
        accepted.latitude = init->latitude;
        accepted.longitude = init->longitude;
        strlcpy(accepted.icon_style, init->icon_style,
                sizeof(accepted.icon_style));
    } else if (inner.which_body ==
               ai_edgez_halow_NetworkPacket_device_settings_tag) {
        const ai_edgez_halow_DeviceSettings *settings =
            &inner.body.device_settings;
        strlcpy(accepted.country, factory_identity.country,
                sizeof(accepted.country));
        strlcpy(accepted.mesh_id, settings->mesh_id,
                sizeof(accepted.mesh_id));
        strlcpy(accepted.passphrase, settings->passphrase,
                sizeof(accepted.passphrase));
        strlcpy(accepted.user_name, settings->user_name,
                sizeof(accepted.user_name));
        accepted.user_id_high = settings->user_id_high;
        accepted.user_id_low = settings->user_id_low;
        if (settings->user_public_key.size == 32U) {
            memcpy(accepted.user_public_key,
                   settings->user_public_key.bytes, 32U);
        }
        accepted.frequency_khz = settings->mesh_frequency_khz;
        accepted.bandwidth_mhz = (uint8_t)settings->mesh_bandwidth_mhz;
        accepted.beacon_interval_seconds =
            settings->beacon_interval_seconds;
        accepted.marker = (uint8_t)settings->marker;
        accepted.share_location = settings->share_location;
        accepted.latitude = settings->latitude;
        accepted.longitude = settings->longitude;
        strlcpy(accepted.icon_style, settings->icon_style,
                sizeof(accepted.icon_style));
    } else {
        memset(plaintext, 0, sizeof(plaintext));
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (accepted.country[0] == '\0') {
        strlcpy(accepted.country, factory_identity.country,
                sizeof(accepted.country));
    }
    if (accepted.user_name[0] == '\0') {
        strlcpy(accepted.user_name, factory_identity.serial,
                sizeof(accepted.user_name));
    }
    uint8_t key_or = 0U;
    for (size_t i = 0; i < 32U; ++i) key_or |= accepted.user_public_key[i];
    if (key_or == 0U) {
        memcpy(accepted.user_public_key, factory_identity.public_key, 32U);
    }
    if (accepted.beacon_interval_seconds == 0U) {
        accepted.beacon_interval_seconds =
            CONFIG_OFFGRID_HT_HC33_SENSOR_INTERVAL_SECONDS;
    }
    if (accepted.icon_style[0] == '\0') {
        strlcpy(accepted.icon_style, "sensor", sizeof(accepted.icon_style));
    }
    accepted.cloud_accepted = true;
    if (acceptance_source == NULL ||
        (acceptance_source[0] & 0x01U) != 0U) {
        memset(plaintext, 0, sizeof(plaintext));
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(accepted.gateway_mac, acceptance_source,
           sizeof(accepted.gateway_mac));
    memset(plaintext, 0, sizeof(plaintext));
    if (strlen(accepted.country) != 2U || accepted.mesh_id[0] == '\0' ||
        accepted.frequency_khz == 0U || accepted.bandwidth_mhz == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    err = save_profile(&accepted);
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "encrypted sensor profile accepted country=%s mesh=%s frequency=%lu kHz bandwidth=%u MHz",
                 profile.country, profile.mesh_id,
                 (unsigned long)profile.frequency_khz,
                 (unsigned)profile.bandwidth_mhz);
    }
    return err;
}

static void receive_frame(void *ctx, const struct edgez_umac_rx_frame *radio)
{
    (void)ctx;
    struct edgez_pawr_action_view action;
    if (radio == NULL || !edgez_pawr_decode_action_frame(
            radio->data, radio->length, &action)) return;

    struct edgez_pawr_bulk_control bulk_control;
    if (edgez_pawr_bulk_control_decode(action.payload, action.payload_length,
                                       &bulk_control) &&
        bulk_control.compressed_ssid == active_compressed_ssid &&
        bulk_control.kind == EDGEZ_PAWR_BULK_KIND_BLOCK_ACK) {
        memcpy(bulk_ack_payload, action.payload,
               EDGEZ_PAWR_BULK_CONTROL_SIZE);
        bulk_ack_length = EDGEZ_PAWR_BULK_CONTROL_SIZE;
        xSemaphoreGive(bulk_ack_sem);
        return;
    }

    uint32_t hash;
    uint32_t compressed_ssid;
    if (edgez_pawr_decode_response(action.payload, action.payload_length,
                                   &hash, &compressed_ssid) &&
        waiting_for_response && hash == expected_hash &&
        compressed_ssid == active_compressed_ssid) {
        response_downlink_pending =
            (action.payload[7] & EDGEZ_PAWR_RESPONSE_DOWNLINK_PENDING) != 0U;
        waiting_for_response = false;
        xSemaphoreGive(response_sem);
        return;
    }

    struct edgez_pawr_request_view request;
    if (edgez_pawr_decode_request(action.payload, action.payload_length,
                                  &request) &&
        request.type == EDGEZ_PAWR_REQUEST_MESH_PACKET &&
        request.compressed_ssid == active_compressed_ssid &&
        request.payload_length <= SENSOR_PACKET_MAX) {
        acceptance_item_t item = {.length = request.payload_length};
        memcpy(item.source, action.source, sizeof(item.source));
        memcpy(item.payload, request.payload, request.payload_length);
        (void)xQueueSend(acceptance_queue, &item, 0);
    }
}

static void hardware_restart(void *ctx)
{
    (void)ctx;
    ESP_LOGE(TAG, "Morse radio requested restart");
}

static const struct edgez_umac_event_ops radio_events = {
    .rx_frame = receive_frame,
    .hardware_restart_required = hardware_restart,
};

static esp_err_t start_radio(const char country[3], uint32_t frequency_khz,
                             uint8_t bandwidth_mhz, const char *mesh_id)
{
    if (radio_running) {
        (void)edgez_umac_stop(&sensor_umac);
        edgez_rf_unbind_umac();
        memset(&sensor_umac, 0, sizeof(sensor_umac));
        radio_running = false;
    }
    const struct edgez_rf_backend_ops *driver = edgez_rf_driver_ops();
    int rc = edgez_umac_bind(&sensor_umac, driver, NULL);
    if (rc == EDGEZ_UMAC_OK) {
        rc = edgez_umac_set_event_ops(&sensor_umac, &radio_events, NULL);
    }
    if (rc == EDGEZ_UMAC_OK) rc = edgez_rf_bind_umac(&sensor_umac);
    active_compressed_ssid = edgez_pawr_compress_ssid(
        (const uint8_t *)mesh_id, strlen(mesh_id));
    strlcpy(active_mesh_id, mesh_id, sizeof(active_mesh_id));
    const struct edgez_umac_config config = {
        .country_code = {country[0], country[1], '\0'},
        .channel = {
            .operating_frequency_hz = frequency_khz * 1000U,
            .operating_bw_mhz = bandwidth_mhz,
            .primary_bw_mhz = 1U,
            .primary_1mhz_channel_index = 0U,
        },
        .beacon_interval_tu = 1000U,
        .dtim_period = 1U,
        .compressed_ssid = active_compressed_ssid,
        .rate_control = {
            .enabled = true,
            .initial_mcs = SENSOR_MCS,
            .maximum_mcs = 2U,
            .successes_to_increase = 4U,
        },
        .enable_beaconing = false,
    };
    if (rc == EDGEZ_UMAC_OK) {
        rc = edgez_umac_start(&sensor_umac, &config, &radio_info);
    }
    if (rc == EDGEZ_UMAC_OK &&
        sensor_settings.value.block_sampling_count > 0U) {
        struct edgez_rf_regulatory_channel policy = {0};
        if (!edgez_rf_resolve_regulatory_channel(
                country, frequency_khz * 1000U, bandwidth_mhz, &policy) ||
            mmdrv_set_duty_cycle(policy.duty_cycle_centi_percent,
                policy.duty_cycle_omit_control_response,
                MMWLAN_DUTY_CYCLE_MODE_BURST) != MMWLAN_SUCCESS) {
            ESP_LOGW(TAG, "could not enable regulatory burst mode");
        }
    }
    if (rc != EDGEZ_UMAC_OK) {
        edgez_rf_unbind_umac();
        memset(&sensor_umac, 0, sizeof(sensor_umac));
        return ESP_FAIL;
    }
    radio_running = true;
    active_bandwidth_mhz = bandwidth_mhz;
    ESP_LOGI(TAG,
             "PAwR-only radio active country=%s frequency=%lu kHz bandwidth=%u MHz beaconing=0 peering=0",
             country, (unsigned long)frequency_khz, (unsigned)bandwidth_mhz);
    return ESP_OK;
}

static void stop_radio(void)
{
    if (!radio_running) return;
    (void)edgez_umac_stop(&sensor_umac);
    edgez_rf_unbind_umac();
    memset(&sensor_umac, 0, sizeof(sensor_umac));
    radio_running = false;
}

static void sensor_sleep(uint32_t interval_ms)
{
    stop_radio();

    /* Keep the MM6108 in reset throughout ESP32-S3 deep sleep. mmhal_init()
     * releases this hold on the next timer wake before initializing HaLow. */
    ESP_ERROR_CHECK(gpio_set_level((gpio_num_t)CONFIG_MM_RESET_N, 0));
    ESP_ERROR_CHECK(gpio_hold_en((gpio_num_t)CONFIG_MM_RESET_N));
    gpio_deep_sleep_hold_en();
    enable_factory_reset_wakeup();
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(
        (uint64_t)interval_ms * UINT64_C(1000)));
    ESP_LOGI(TAG, "sensor cycle complete; deep sleep for %lu ms",
             (unsigned long)interval_ms);
    esp_deep_sleep_start();
    abort();
}

static esp_err_t send_request(const uint8_t *payload, size_t payload_length,
                              uint32_t interval_ms)
{
    uint8_t packet[EDGEZ_PAWR_HEADER_LENGTH + SENSOR_PACKET_MAX];
    uint8_t frame[EDGEZ_PAWR_ACTION_HEADER_LENGTH + sizeof(packet)];
    size_t packet_length = 0U;
    size_t frame_length = 0U;
    const uint32_t hash = edgez_pawr_payload_hash(payload, payload_length);
    const bool discovery = !profile.cloud_accepted;
    if (!edgez_pawr_encode_typed_request(packet, sizeof(packet),
            EDGEZ_PAWR_REQUEST_BEACON, ++request_id, interval_ms,
            active_compressed_ssid, payload, payload_length, &packet_length)) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint8_t *destination = discovery ? broadcast_mac :
        profile.gateway_mac;
    if (!edgez_pawr_encode_action_frame(
            frame, sizeof(frame), sequence_number++, radio_info.mac_addr,
            destination, packet, packet_length, &frame_length)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!discovery) {
        /* A provisioned sensor sends a directed Public Action frame through
         * the gateway's active mesh BSS.  Keep addr3 aligned with that BSS;
         * using the sensor's own MAC here makes MM6108 classify the otherwise
         * valid unicast frame as belonging to another BSS before the host
         * PAwR handler can inspect its CSSID. */
        memcpy(frame + IEEE80211_MGMT_BSSID_OFFSET, profile.gateway_mac,
               sizeof(profile.gateway_mac));
    }
    const struct edgez_umac_tx_request request = {
        .token = ++tx_token,
        .frame = frame,
        .frame_length = frame_length,
        .frame_type = EDGEZ_UMAC_TX_MANAGEMENT,
        .tid = 8U,
        .mcs = SENSOR_MCS,
        .attempts = SENSOR_ATTEMPTS,
        .bandwidth_mhz = active_bandwidth_mhz,
        .no_ack = discovery,
        .immediate_status = true,
    };
    xSemaphoreTake(response_sem, 0);
    expected_hash = hash;
    response_downlink_pending = false;
    waiting_for_response = true;
    if (edgez_umac_transmit(&sensor_umac, &request) != EDGEZ_UMAC_OK) {
        waiting_for_response = false;
        return ESP_FAIL;
    }
    if (xSemaphoreTake(response_sem, pdMS_TO_TICKS(SENSOR_RESPONSE_TIMEOUT_MS)) !=
        pdTRUE) {
        waiting_for_response = false;
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static uint32_t fnv1a32(const uint8_t *data, size_t length)
{
    uint32_t digest = UINT32_C(2166136261);
    for (size_t i = 0U; i < length; ++i) {
        digest ^= data[i];
        digest *= UINT32_C(16777619);
    }
    return digest;
}

static esp_err_t send_sensor_block(const uint8_t *data, size_t length,
                                   uint8_t sample_kind,
                                   uint32_t interval_ms)
{
    if (data == NULL || length == 0U ||
        length > EDGEZ_PAWR_BULK_MAX_CHUNK_DATA) {
        return ESP_ERR_INVALID_SIZE;
    }
    struct edgez_rf_regulatory_channel policy = {0};
    struct mmwlan_duty_cycle_stats duty = {0};
    if (!edgez_rf_resolve_regulatory_channel(
            profile.country, profile.frequency_khz * 1000U,
            profile.bandwidth_mhz, &policy) ||
        mmdrv_get_duty_cycle(&duty) != MMWLAN_SUCCESS) {
        ESP_LOGE(TAG, "bulk regulatory state unavailable");
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t session_id = esp_random();
    if (session_id == 0U) session_id = 1U;
    const struct edgez_pawr_bulk_chunk chunk = {
        .compressed_ssid = active_compressed_ssid,
        .session_id = session_id,
        .total_length = (uint32_t)length,
        .offset = 0U,
        .expected_digest = fnv1a32(data, length),
        .chunk_index = 0U,
        .chunk_count = 1U,
        .data_length = (uint16_t)length,
        .window_id = 0U,
        .window_base_chunk = 0U,
        .window_count = 1U,
        .test_index = sample_kind,
        .flags = EDGEZ_PAWR_BULK_FLAG_FIRST |
                 EDGEZ_PAWR_BULK_FLAG_LAST |
                 EDGEZ_PAWR_BULK_FLAG_ACK_REQUEST,
        .regulatory = {
            .country_code = {profile.country[0], profile.country[1], '\0'},
            .frequency_khz = profile.frequency_khz,
            .duty_cycle_centi_percent =
                (uint16_t)duty.duty_cycle,
            .max_eirp_dbm = policy.max_eirp_dbm,
            .bandwidth_mhz = profile.bandwidth_mhz,
            .duty_cycle_mode = (uint8_t)duty.mode,
            .flags = EDGEZ_PAWR_BULK_REG_FLAG_REGDB_VALID |
                     EDGEZ_PAWR_BULK_REG_FLAG_CHIP_DUTY_VALID |
                     EDGEZ_PAWR_BULK_REG_FLAG_TERMINAL_NODE,
        },
        .data = data,
    };
    uint8_t encoded[EDGEZ_PAWR_BULK_HEADER_SIZE +
                    EDGEZ_PAWR_BULK_MAX_CHUNK_DATA];
    const size_t encoded_length = edgez_pawr_bulk_encode(
        encoded, sizeof(encoded), &chunk);
    if (encoded_length == 0U) return ESP_ERR_INVALID_SIZE;

    for (unsigned attempt = 0U; attempt < SENSOR_BULK_ACK_ATTEMPTS; ++attempt) {
        uint8_t packet[EDGEZ_PAWR_HEADER_LENGTH + sizeof(encoded)];
        uint8_t action[EDGEZ_PAWR_ACTION_HEADER_LENGTH + sizeof(packet)];
        size_t packet_length = 0U;
        size_t action_length = 0U;
        if (!edgez_pawr_encode_typed_request(
                packet, sizeof(packet), EDGEZ_PAWR_REQUEST_BULK_STREAM,
                ++request_id, interval_ms, active_compressed_ssid,
                encoded, encoded_length, &packet_length) ||
            !edgez_pawr_encode_action_frame(
                action, sizeof(action), sequence_number++,
                radio_info.mac_addr, profile.gateway_mac,
                packet, packet_length, &action_length)) {
            return ESP_ERR_INVALID_SIZE;
        }
        /* Blocks use the same directed management path as the normal beacon. */
        memcpy(action + IEEE80211_MGMT_BSSID_OFFSET, profile.gateway_mac,
               sizeof(profile.gateway_mac));
        const struct edgez_umac_tx_request request = {
            .token = ++tx_token,
            .frame = action,
            .frame_length = action_length,
            .frame_type = EDGEZ_UMAC_TX_MANAGEMENT,
            .tid = 8U,
            .mcs = SENSOR_MCS,
            .attempts = 1U,
            .bandwidth_mhz = active_bandwidth_mhz,
            .no_ack = false,
            .immediate_status = true,
        };
        xSemaphoreTake(bulk_ack_sem, 0);
        bulk_ack_length = 0U;
        if (edgez_umac_transmit(&sensor_umac, &request) != EDGEZ_UMAC_OK) {
            continue;
        }
        const uint32_t timeout_ms = edgez_pawr_bulk_ack_timeout_ms(
            (uint16_t)length, 1U);
        if (xSemaphoreTake(bulk_ack_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE &&
            bulk_ack_length == EDGEZ_PAWR_BULK_CONTROL_SIZE) {
            struct edgez_pawr_bulk_control control;
            if (edgez_pawr_bulk_control_decode(
                    bulk_ack_payload, bulk_ack_length, &control) &&
                edgez_pawr_bulk_control_matches(
                    &control, active_compressed_ssid, session_id,
                    sample_kind, 0U, 0U, 1U)) {
                ESP_LOGI(TAG, "sensor block delivered sequence=%lu bytes=%u",
                         (unsigned long)block_sequence, (unsigned)length);
                return ESP_OK;
            }
        }
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t send_voltage_block(uint32_t interval_ms)
{
    const uint8_t count = voltage_sample_count;
    if (sensor_settings.value.block_sampling_count == 0U || count == 0U) {
        return ESP_ERR_NOT_FINISHED;
    }
    uint8_t block[EDGEZ_SENSOR_BLOCK_HEADER_SIZE +
                  EDGEZ_SENSOR_SETTINGS_MAX_SAMPLES_PER_BLOCK * sizeof(float)];
    size_t offset = edgez_sensor_block_encode_header(
        block, sizeof(block), EDGEZ_SENSOR_BLOCK_KIND_BATTERY_VOLTAGE,
        sizeof(float), count, block_sequence, interval_ms,
        &sensor_settings.value);
    if (offset == 0U) return ESP_ERR_INVALID_STATE;
    for (uint8_t i = 0U; i < count; ++i) {
        edgez_sensor_put_float_le(block + offset, voltage_samples[i]);
        offset += sizeof(float);
    }
    esp_err_t err = send_sensor_block(
        block, offset, EDGEZ_SENSOR_BLOCK_KIND_BATTERY_VOLTAGE, interval_ms);
    if (err == ESP_OK) {
        voltage_sample_count = 0U;
        ++block_sequence;
        update_rtc_cache(profile_valid(&profile));
    }
    return err;
}

static uint16_t downlink_read_le16(const uint8_t *value)
{
    return (uint16_t)value[0] | (uint16_t)value[1] << 8;
}

static uint32_t downlink_read_le32(const uint8_t *value)
{
    return (uint32_t)value[0] | (uint32_t)value[1] << 8 |
        (uint32_t)value[2] << 16 | (uint32_t)value[3] << 24;
}

static bool apply_downlink(const uint8_t *payload, size_t length)
{
    char command[256];
    uint8_t command_length;
    uint16_t body_length;
    uint32_t message_id;

    if (length < DOWNLINK_HEADER_LENGTH || memcmp(payload, "EZDL", 4U) != 0 ||
        payload[4] != 1U)
        return false;
    command_length = payload[5];
    body_length = downlink_read_le16(payload + 6U);
    message_id = downlink_read_le32(payload + 8U);
    if (command_length == 0U || message_id == 0U ||
        DOWNLINK_HEADER_LENGTH + (size_t)command_length + body_length != length)
        return false;
    if (message_id == downlink_ack_id)
        return true;
    memcpy(command, payload + DOWNLINK_HEADER_LENGTH, command_length);
    command[command_length] = '\0';
    if (edgez_sensor_handle_downlink(command,
            payload + DOWNLINK_HEADER_LENGTH + command_length,
            body_length) != ESP_OK)
        return false;
    if (save_downlink_ack(message_id) != ESP_OK)
        return false;
    ESP_LOGI(TAG, "durably accepted downlink id=%lu command=%s",
             (unsigned long)message_id, command);
    return true;
}

static bool receive_pending_packet(void)
{
    acceptance_item_t item;
    const TickType_t deadline = xTaskGetTickCount() +
        pdMS_TO_TICKS(SENSOR_ACCEPT_WINDOW_MS);

    do {
        const TickType_t now = xTaskGetTickCount();
        const TickType_t remaining = deadline > now ? deadline - now : 0;
        if (xQueueReceive(acceptance_queue, &item, remaining) != pdTRUE)
            break;
        /* A sensor bootstrapped from factory HaLow settings already emits
         * beacons, so its cloud acceptance arrives through the same pending
         * packet window as ordinary downlinks.  Apply the encrypted profile
         * first: it carries the Appwrite Device UUID that must replace the
         * temporary public-key-derived beacon identity. */
        if (apply_acceptance(item.payload, item.length, item.source) == ESP_OK) {
            ESP_LOGI(TAG, "cloud identity accepted from beacon downlink window");
            return true;
        }
        if (apply_downlink(item.payload, item.length))
            return true;
    } while (xTaskGetTickCount() < deadline);
    return false;
}

static bool try_acceptance(void)
{
    acceptance_item_t item;
    const TickType_t deadline = xTaskGetTickCount() +
        pdMS_TO_TICKS(SENSOR_ACCEPT_WINDOW_MS);
    do {
        const TickType_t now = xTaskGetTickCount();
        const TickType_t remaining = deadline > now ? deadline - now : 0;
        if (xQueueReceive(acceptance_queue, &item, remaining) != pdTRUE) break;
        if (apply_acceptance(item.payload, item.length, item.source) == ESP_OK)
            return true;
    } while (xTaskGetTickCount() < deadline);
    return false;
}

static bool advance_bootstrap_channel(const char *country, uint8_t *channel)
{
    uint32_t frequency_khz;
    uint8_t bandwidth_mhz;
    const uint8_t first = *channel;
    do {
        (*channel)++;
        if (*channel == 0U) *channel = 1U;
        if (edgez_rf_resolve_s1g_channel(country, *channel,
                &frequency_khz, &bandwidth_mhz) && bandwidth_mhz == 1U) {
            return true;
        }
    } while (*channel != first);
    return false;
}

static void sensor_task(void *arg)
{
    (void)arg;
    const bool has_network_profile = profile_valid(&profile);
    const bool provisioned = has_network_profile && profile.cloud_accepted;
    uint8_t bootstrap_channel = factory_identity.channel;
    uint32_t interval_ms = sensor_settings.value.sampling_interval_ms;

    if (provisioned) {
        uint8_t payload[SENSOR_PACKET_MAX];
        size_t payload_length = 0U;
        float temperature = 0.0f;
        const esp_err_t temperature_err = read_mcu_temperature(&temperature);
        const bool temperature_valid = temperature_err == ESP_OK;

        /* Voltage sampling belongs to the low-power phase, before MM6108
         * startup. blockSamplingCount is a per-cycle cap; zero disables ADC
         * block sampling. Preserve unsent samples and fill only free slots. */
        const uint16_t requested_samples =
            sensor_settings.value.block_sampling_count;
        if (requested_samples == 0U && voltage_sample_count != 0U) {
            voltage_sample_count = 0U;
            update_rtc_cache(true);
        } else if (requested_samples > 0U) {
            while (voltage_sample_count < requested_samples) {
                float voltage = 0.0f;
                const esp_err_t voltage_err = battery_read(&voltage);
                if (voltage_err != ESP_OK) {
                    ESP_LOGW(TAG,
                             "battery voltage sample stopped at %u/%u: %s",
                             (unsigned)voltage_sample_count,
                             (unsigned)requested_samples,
                             esp_err_to_name(voltage_err));
                    break;
                }
                voltage_samples[voltage_sample_count++] = voltage;
            }
            update_rtc_cache(true);
        }
        if (!temperature_valid) {
            ESP_LOGW(TAG, "MCU temperature sample failed before HaLow: %s",
                     esp_err_to_name(temperature_err));
        }
        if (requested_samples > 0U) {
            ESP_LOGI(TAG, "battery voltage samples cached=%u requested=%u",
                     (unsigned)voltage_sample_count,
                     (unsigned)requested_samples);
        }

        esp_err_t err = start_radio(profile.country, profile.frequency_khz,
                                    profile.bandwidth_mhz, profile.mesh_id);
        if (err == ESP_OK) {
            err = build_beacon(payload, sizeof(payload), &payload_length,
                               temperature, temperature_valid);
        }
        if (err == ESP_OK) {
            err = send_request(payload, payload_length, interval_ms);
        }
		if (err == ESP_OK && response_downlink_pending &&
		    !receive_pending_packet()) {
			ESP_LOGW(TAG, "gateway advertised a downlink but none was received");
		}
        interval_ms = sensor_settings.value.sampling_interval_ms;
        if (err == ESP_OK &&
            sensor_settings.value.block_sampling_count > 0U &&
            voltage_sample_count > 0U) {
            esp_err_t block_err = send_voltage_block(interval_ms);
            if (block_err != ESP_OK) {
                ESP_LOGW(TAG, "sensor block retained for retry: %s",
                         esp_err_to_name(block_err));
            }
        }
        ESP_LOGI(TAG, "Beacon PAwR request %s bytes=%u",
                 err == ESP_OK ? "acknowledged" : "unacknowledged",
                 (unsigned)payload_length);
        sensor_sleep(sensor_settings.value.sampling_interval_ms);
    }

    /* Factory data is a zero-touch network bootstrap, not a cloud identity.
     * Use its exact regulatory/channel/mesh settings to advertise discovery;
     * the encrypted acceptance supplies and durably stores the Appwrite UUID. */
    if (has_network_profile) {
        uint8_t payload[SENSOR_PACKET_MAX];
        size_t payload_length = 0U;
        esp_err_t err = start_radio(profile.country, profile.frequency_khz,
                                    profile.bandwidth_mhz, profile.mesh_id);
        if (err == ESP_OK) {
            err = build_discovery(payload, sizeof(payload), &payload_length);
        }
        if (err == ESP_OK) {
            err = send_request(payload, payload_length, interval_ms);
        }
        bool accepted = err == ESP_OK && try_acceptance();
        ESP_LOGI(TAG,
                 "factory-network discover PAwR request %s accepted=%u bytes=%u",
                 err == ESP_OK ? "acknowledged" : "unacknowledged",
                 accepted, (unsigned)payload_length);
        stop_radio();
        sensor_sleep(interval_ms);
    }

    uint32_t factory_frequency_khz = 0U;
    uint8_t factory_bandwidth_mhz = 0U;
    if (strlen(factory_identity.country) != 2U || bootstrap_channel == 0U ||
        !edgez_rf_resolve_s1g_channel(factory_identity.country,
            bootstrap_channel, &factory_frequency_khz,
            &factory_bandwidth_mhz) || factory_bandwidth_mhz != 1U) {
        ESP_LOGE(TAG,
                 "factory discovery country/channel unavailable or invalid country=%s channel=%u",
                 factory_identity.country, (unsigned)bootstrap_channel);
        vTaskDelete(NULL);
        return;
    }

    const uint8_t first_channel = bootstrap_channel;
    bool accepted = false;
    do {
        uint32_t frequency_khz = 0U;
        uint8_t bandwidth_mhz = 0U;
        if (!edgez_rf_resolve_s1g_channel(factory_identity.country,
                bootstrap_channel, &frequency_khz, &bandwidth_mhz) ||
            bandwidth_mhz != 1U) {
            ESP_LOGW(TAG, "skipping invalid discovery channel=%u country=%s",
                     (unsigned)bootstrap_channel, factory_identity.country);
        } else {
            uint8_t payload[SENSOR_PACKET_MAX];
            size_t payload_length = 0U;
            esp_err_t err = start_radio(factory_identity.country,
                frequency_khz, bandwidth_mhz, "edgez-zero-touch");
            if (err == ESP_OK) {
                err = build_discovery(payload, sizeof(payload),
                                      &payload_length);
            }
            if (err == ESP_OK) {
                err = send_request(payload, payload_length, interval_ms);
            }
            ESP_LOGI(TAG,
                     "discover PAwR request %s channel=%u frequency=%lu kHz bytes=%u",
                     err == ESP_OK ? "acknowledged" : "unacknowledged",
                     (unsigned)bootstrap_channel,
                     (unsigned long)frequency_khz, (unsigned)payload_length);
            if (err == ESP_OK) accepted = try_acceptance();
            stop_radio();
            if (accepted) break;
        }

        if (!advance_bootstrap_channel(factory_identity.country,
                                       &bootstrap_channel)) {
            break;
        }
    } while (bootstrap_channel != first_channel);

    if (accepted) {
        ESP_LOGI(TAG, "discovery accepted; saved profile for next wake");
    }
    sensor_sleep(interval_ms);
}

void app_main(void)
{
    ESP_ERROR_CHECK(edgez_esp_factory_data_init());
    ESP_ERROR_CHECK(edgez_esp_factory_data_get(&factory_identity));
    handle_factory_reset_wakeup();
    ESP_ERROR_CHECK(edgez_rf_platform_init() == 0 ? ESP_OK : ESP_FAIL);
    esp_err_t profile_err = ESP_ERR_NOT_FOUND;
    if (rtc_cache_valid()) {
        downlink_ack_id = rtc_cache.downlink_ack_id;
        sensor_settings = rtc_cache.settings;
        block_sequence = rtc_cache.block_sequence;
        voltage_sample_count = rtc_cache.sample_count;
        memcpy(voltage_samples, rtc_cache.voltage_samples,
               sizeof(voltage_samples));
        if (rtc_cache.has_profile) {
            profile = rtc_cache.profile;
            profile_err = ESP_OK;
        }
        ESP_LOGI(TAG, "restored sensor state from RTC; NVS read skipped");
    } else {
        /* A cloud acceptance contains the authoritative Device UUID.  Keep
         * the factory profile as the zero-touch fallback only; otherwise a
         * valid factory mesh profile would overwrite the accepted identity on
         * every cold boot and H7608 would reconstruct a stale clientId. */
        profile_err = load_persistent_state();
        if (profile_err != ESP_OK) profile_err = load_factory_profile();
        update_rtc_cache(profile_err == ESP_OK);
    }
    ESP_LOGI(TAG, "HT-HC33 sensor boot factory=%s profile=%s",
             factory_identity.serial,
             profile_err == ESP_OK ? "provisioned" : "discover");
    const esp_err_t battery_err = battery_init();
    if (battery_err != ESP_OK) {
        ESP_LOGW(TAG, "battery voltage sampling unavailable: %s",
                 esp_err_to_name(battery_err));
    }
    response_sem = xSemaphoreCreateBinary();
    bulk_ack_sem = xSemaphoreCreateBinary();
    acceptance_queue = xQueueCreate(SENSOR_ACCEPT_QUEUE_DEPTH,
                                    sizeof(acceptance_item_t));
    if (response_sem == NULL || bulk_ack_sem == NULL ||
        acceptance_queue == NULL ||
        xTaskCreate(sensor_task, "hc33_sensor", 12288, NULL, 6, NULL) !=
            pdPASS) {
        ESP_LOGE(TAG, "could not start sensor task");
        abort();
    }
}
