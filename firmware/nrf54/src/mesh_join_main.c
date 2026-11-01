#include <zephyr/autoconf.h>

#if defined(CONFIG_WIFI_MORSEMICRO_EDGEZ_JOIN_ONLY)

/*
 * Copyright 2026 EdgeZeron AI AB
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/data/json.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/reboot.h>
#include <helpers/nrfx_ram_ctrl.h>
#include <soc.h>

#include "pb_decode.h"
#include "pb_encode.h"

#include "edgez_diagnostics.h"
#include "edgez_factory_data.h"
#include "edgez_batman_adv_lite.h"
#include "edgez_ble.h"
#include "edgez_battery.h"
#include "edgez_config.h"
#include "edgez_gps.h"
#include "edgez_imu.h"
#include "edgez_mesh_channel_policy.h"
#include "edgez_mesh_data_frame.h"
#include "edgez_mesh_msdu.h"
#include "edgez_mesh_relay.h"
#include "edgez_mesh_zephyr.h"
#include "edgez_pawr_protocol.h"
#include "edgez_sensor_settings.h"
#include "edgez_mesh_sae_provider.h"
#include "edgez_rf_api.h"
#include "edgez_umac_mesh_runtime.h"
#include "edgez_reboot.h"
#include "livestocking_config.h"
#include "usb_control.pb.h"

#ifndef EDGEZ_PAWR_RESPONSE_DOWNLINK_PENDING
#define EDGEZ_PAWR_RESPONSE_DOWNLINK_PENDING 0x01U
#endif

#if defined(CONFIG_OFFGRID_NRF54_MESH_PS_TEST)
#include "mmhal_wlan.h"
#endif

#if defined(CONFIG_OFFGRID_NRF54_MESH_PS_TEST) || \
	defined(CONFIG_OFFGRID_NRF54_SENSOR_HALOW_POWER_SAVE) || \
	defined(CONFIG_OFFGRID_NRF54_ROLE_SENSOR)
#include "mmwlan.h"

extern enum mmwlan_status mmdrv_set_wake_enabled(bool enabled);
extern enum mmwlan_status mmdrv_set_chip_power_save_enabled(uint16_t vif_id,
							     bool enabled);
extern enum mmwlan_status mmdrv_set_dynamic_ps_timeout(uint32_t timeout_ms);
#if defined(CONFIG_OFFGRID_NRF54_ROLE_SENSOR)
extern enum mmwlan_status mmdrv_set_duty_cycle(
	uint32_t duty_cycle, bool duty_cycle_omit_ctrl_resp,
	enum mmwlan_duty_cycle_mode mode);
extern enum mmwlan_status mmdrv_get_duty_cycle(
	struct mmwlan_duty_cycle_stats *stats);
#endif
#endif

#if defined(CONFIG_OFFGRID_NRF54_ROLE_SENSOR)
#include "edgez_pawr_bulk.h"
#include "mmregdb.h"
#endif

LOG_MODULE_REGISTER(edgez_mesh_join, LOG_LEVEL_INF);

#define EDGEZ_MESH_PRIMARY_BANDWIDTH_MHZ 1U
#define EDGEZ_MESH_BEACON_INTERVAL_TU 100U
#define EDGEZ_MESH_TICK_MS 100U
#define EDGEZ_MESH_REPORT_MS 2000U
#define EDGEZ_SENSOR_RESPONSE_TIMEOUT_MS 100U
#define EDGEZ_SENSOR_PROVISIONING_ACK_TIMEOUT_MS 100U
#define EDGEZ_SENSOR_PROVISIONING_WINDOW_MS 200U
#define EDGEZ_SENSOR_UPDATE_INTERVAL_MS 15000U
#define EDGEZ_SENSOR_PENDING_PACKET_MAX 512U
#define EDGEZ_SENSOR_PENDING_QUEUE_DEPTH 2U
#define EDGEZ_SENSOR_DOWNLINK_HEADER_LENGTH 12U
#define EDGEZ_SENSOR_DOWNLINK_SETTINGS_KEY "edgez/downlink_ack/v1"
#define EDGEZ_SENSOR_BEHAVIOR_SETTINGS_KEY "edgez/sensor_settings/v2"
#define EDGEZ_SENSOR_SETTINGS_MAGIC UINT32_C(0x455a5353)
#define EDGEZ_SENSOR_BULK_ACK_ATTEMPTS 4U
#define EDGEZ_SENSOR_GPS_TIMEOUT_MS 10000U
#define EDGEZ_BUTTON_DEBOUNCE_MS 50U
#define EDGEZ_BUTTON_RESET_HOLD_MS 5000U
#define EDGEZ_USER_NODE DT_PATH(zephyr_user)

#if defined(CONFIG_OFFGRID_NRF54_MESH_PS_TEST)
#define EDGEZ_PS_BASELINE_MS 30000U
#define EDGEZ_PS_REPORT_MS 30000U
#define EDGEZ_PS_SETTLE_MS 2000U
#define EDGEZ_PS_SAMPLE_MS 10U
#define EDGEZ_PS_DYNAMIC_TIMEOUT_MS 100U
#endif

static struct edgez_umac_mesh_runtime mesh_runtime;
static struct edgez_mesh_sae_provider sae_provider;
static uint32_t tx_status_count;
static uint32_t tx_failure_count;
static uint32_t batman_tx_count;
static uint32_t batman_tx_failure_count;
static uint32_t previous_operation_failures;
static uint32_t previous_handler_failures;
static uint32_t applied_generation;
static struct edgez_halow_profile applied_profile;
static bool mesh_started;
static bool mesh_secure;
static bool sensor_radio_only;
#if defined(CONFIG_OFFGRID_NRF54_SENSOR_HALOW_POWER_SAVE)
static bool sensor_halow_ps_configured;
static bool sensor_halow_sleep_allowed;
#endif
static uint16_t open_data_sequence;
static uint32_t open_mesh_sequence;
static uintptr_t open_tx_token;
static uintptr_t pawr_tx_token;
static uint16_t pawr_sequence_number;
static uint32_t pawr_request_id;
static bool mesh_services_initialized;
static struct k_spinlock sensor_pawr_lock;
static uint32_t sensor_pawr_expected_hash;
static bool sensor_pawr_waiting;
static bool sensor_pawr_downlink_pending;
static uint32_t sensor_downlink_ack_id;
struct sensor_settings_record {
	uint32_t magic;
	uint16_t version;
	uint16_t length;
	struct edgez_sensor_settings value;
};
static struct sensor_settings_record sensor_settings;
#if !defined(CONFIG_OFFGRID_NRF54_SENSOR_PAWR_BULK_TEST)
static struct edgez_imu_sample sensor_block_samples[
	EDGEZ_SENSOR_SETTINGS_MAX_SAMPLES_PER_BLOCK];
static uint8_t sensor_block_sample_count;
static uint32_t sensor_block_sequence;
#endif
struct sensor_pending_packet {
	uint16_t length;
	uint8_t source[6];
	uint8_t payload[EDGEZ_SENSOR_PENDING_PACKET_MAX];
};
K_MSGQ_DEFINE(sensor_pending_packet_queue, sizeof(struct sensor_pending_packet),
	      EDGEZ_SENSOR_PENDING_QUEUE_DEPTH, 4);
#if defined(CONFIG_OFFGRID_NRF54_ROLE_SENSOR)
#if defined(CONFIG_OFFGRID_NRF54_SENSOR_PAWR_BULK_TEST)
static struct edgez_pawr_bulk_sender sensor_bulk_sender;
#endif
static uint8_t sensor_bulk_ack_payload[EDGEZ_PAWR_BULK_CONTROL_SIZE];
static size_t sensor_bulk_ack_length;
K_SEM_DEFINE(sensor_bulk_ack_sem, 0, 1);
#endif
static char sensor_bootstrap_country[3];
static char sensor_bootstrap_mesh_id[EDGEZ_FACTORY_MESH_ID_SIZE];
static char sensor_bootstrap_passphrase[EDGEZ_FACTORY_PASSPHRASE_SIZE];
static uint8_t sensor_bootstrap_channel;
K_SEM_DEFINE(sensor_pawr_response_sem, 0, 1);
#if DT_NODE_HAS_PROP(EDGEZ_USER_NODE, key_gpios)
static const struct gpio_dt_spec user_button =
	GPIO_DT_SPEC_GET(EDGEZ_USER_NODE, key_gpios);
static bool user_button_ready;
static struct gpio_callback user_button_callback;
K_SEM_DEFINE(user_button_sem, 0, 1);
#endif
K_MUTEX_DEFINE(batman_state_lock);
K_MUTEX_DEFINE(batman_tx_lock);
#define EDGEZ_OPEN_DATA_FRAME_MAX (EDGEZ_MESH_QOS_4ADDR_HEADER_LEN + \
	EDGEZ_MESH_CONTROL_MAX_LEN + EDGEZ_RFC1042_HEADER_LEN + \
	EDGEZ_BATADV_MAX_PACKET_LEN)
static uint8_t open_data_frame[EDGEZ_OPEN_DATA_FRAME_MAX];

static uint64_t mac_to_u64(const uint8_t mac[6])
{
	uint64_t value = 0U;

	for (size_t i = 0; i < 6U; ++i) {
		value = (value << 8U) | mac[i];
	}
	return value;
}

struct sensor_settings_json {
	int32_t sampling_interval_ms;
	int32_t block_sampling_count;
};

static const struct json_obj_descr sensor_settings_json_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct sensor_settings_json,
		"samplingIntervalMs", sampling_interval_ms, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct sensor_settings_json,
		"blockSamplingCount", block_sampling_count, JSON_TOK_NUMBER),
};

static bool sensor_settings_record_valid(
	const struct sensor_settings_record *record)
{
	return record != NULL && record->magic == EDGEZ_SENSOR_SETTINGS_MAGIC &&
		record->version == EDGEZ_SENSOR_SETTINGS_VERSION &&
		record->length == sizeof(*record) &&
		edgez_sensor_settings_valid(&record->value);
}

static void sensor_settings_set_default(uint32_t interval_ms)
{
	sensor_settings = (struct sensor_settings_record) {
		.magic = EDGEZ_SENSOR_SETTINGS_MAGIC,
		.version = EDGEZ_SENSOR_SETTINGS_VERSION,
		.length = sizeof(sensor_settings),
		.value = {
			.sampling_interval_ms = interval_ms,
			.block_sampling_count = 0U,
		},
	};
#if !defined(CONFIG_OFFGRID_NRF54_SENSOR_PAWR_BULK_TEST)
	sensor_block_sample_count = 0U;
#endif
}

static void sensor_settings_load(void)
{
	struct sensor_settings_record loaded = {0};
	struct edgez_halow_profile profile = {0};
	edgez_config_get_profile(&profile);
	uint32_t default_interval_ms = profile.beacon_interval_seconds != 0U ?
		profile.beacon_interval_seconds * 1000U :
		EDGEZ_SENSOR_UPDATE_INTERVAL_MS;
	ssize_t length = settings_load_one(EDGEZ_SENSOR_BEHAVIOR_SETTINGS_KEY,
		&loaded, sizeof(loaded));

	if (length == sizeof(loaded) && sensor_settings_record_valid(&loaded)) {
		sensor_settings = loaded;
#if !defined(CONFIG_OFFGRID_NRF54_SENSOR_PAWR_BULK_TEST)
		/* A cold boot never has retained block samples. Keep this explicit so
		 * disabled block mode always starts with an empty accumulator. */
		sensor_block_sample_count = 0U;
#endif
		LOG_INF("sensor settings loaded interval=%u ms block_samples=%u",
			sensor_settings.value.sampling_interval_ms,
			sensor_settings.value.block_sampling_count);
		return;
	}
	sensor_settings_set_default(default_interval_ms);
	LOG_INF("sensor settings default interval=%u ms block_samples=0",
		default_interval_ms);
}

int edgez_sensor_handle_downlink(
	const char *command, const uint8_t *payload, size_t payload_length)
{
	if (!command || strcmp(command, EDGEZ_SENSOR_SETTINGS_COMMAND) != 0) {
		LOG_WRN("unsupported downlink command=%s",
			command ? command : "(null)");
		return -ENOTSUP;
	}
	if (!payload || payload_length == 0U || payload_length > 256U) {
		return -EMSGSIZE;
	}
	char json[257];
	memcpy(json, payload, payload_length);
	json[payload_length] = '\0';
	struct sensor_settings_json request = {0};
	int64_t fields = json_obj_parse(json, payload_length,
		sensor_settings_json_descr,
		ARRAY_SIZE(sensor_settings_json_descr), &request);
	if (fields != BIT_MASK(ARRAY_SIZE(sensor_settings_json_descr)) ||
		request.sampling_interval_ms < 0 ||
		request.block_sampling_count < 0) {
		return -EINVAL;
	}
	struct sensor_settings_record updated = {
		.magic = EDGEZ_SENSOR_SETTINGS_MAGIC,
		.version = EDGEZ_SENSOR_SETTINGS_VERSION,
		.length = sizeof(updated),
		.value = {
			.sampling_interval_ms =
				(uint32_t)request.sampling_interval_ms,
			.block_sampling_count =
				(uint16_t)request.block_sampling_count,
		},
	};
	if (!edgez_sensor_settings_valid(&updated.value)) {
		return -ERANGE;
	}
	int rc = settings_save_one(EDGEZ_SENSOR_BEHAVIOR_SETTINGS_KEY,
		&updated, sizeof(updated));
	if (rc != 0) {
		return rc;
	}
	sensor_settings = updated;
#if !defined(CONFIG_OFFGRID_NRF54_SENSOR_PAWR_BULK_TEST)
	sensor_block_sample_count = 0U;
#endif
	LOG_INF("sensor settings saved interval=%u ms block_samples=%u",
		sensor_settings.value.sampling_interval_ms,
		sensor_settings.value.block_sampling_count);
	return 0;
}

static uint16_t sensor_downlink_le16(const uint8_t *value)
{
	return (uint16_t)value[0] | (uint16_t)value[1] << 8;
}

static uint32_t sensor_downlink_le32(const uint8_t *value)
{
	return (uint32_t)value[0] | (uint32_t)value[1] << 8 |
	       (uint32_t)value[2] << 16 | (uint32_t)value[3] << 24;
}

static int sensor_downlink_save_ack(uint32_t message_id)
{
	int rc = settings_save_one(EDGEZ_SENSOR_DOWNLINK_SETTINGS_KEY,
				   &message_id, sizeof(message_id));

	if (rc == 0) {
		sensor_downlink_ack_id = message_id;
		edgez_config_set_downlink_ack_id(message_id);
	}
	return rc;
}

static void sensor_downlink_load_ack(void)
{
	uint32_t message_id = 0U;
	ssize_t length = settings_load_one(EDGEZ_SENSOR_DOWNLINK_SETTINGS_KEY,
					   &message_id, sizeof(message_id));

	if (length == sizeof(message_id))
		sensor_downlink_ack_id = message_id;
	edgez_config_set_downlink_ack_id(sensor_downlink_ack_id);
}

static bool sensor_apply_downlink(const uint8_t *payload, size_t length)
{
	char command[256];
	uint8_t command_length;
	uint16_t body_length;
	uint32_t message_id;

	if (!payload || length < EDGEZ_SENSOR_DOWNLINK_HEADER_LENGTH ||
	    memcmp(payload, "EZDL", 4U) != 0 || payload[4] != 1U)
		return false;
	command_length = payload[5];
	body_length = sensor_downlink_le16(payload + 6U);
	message_id = sensor_downlink_le32(payload + 8U);
	if (command_length == 0U || message_id == 0U ||
	    EDGEZ_SENSOR_DOWNLINK_HEADER_LENGTH + (size_t)command_length +
		body_length != length)
		return false;
	if (message_id == sensor_downlink_ack_id)
		return true;
	memcpy(command, payload + EDGEZ_SENSOR_DOWNLINK_HEADER_LENGTH,
	       command_length);
	command[command_length] = '\0';
	if (edgez_sensor_handle_downlink(command,
		payload + EDGEZ_SENSOR_DOWNLINK_HEADER_LENGTH + command_length,
		body_length) != 0 || sensor_downlink_save_ack(message_id) != 0)
		return false;
	LOG_INF("durably accepted downlink id=%u command=%s", message_id, command);
	return true;
}

static bool sensor_apply_zero_touch_acceptance(const uint8_t *payload,
					       size_t payload_length,
					       const uint8_t source[6])
{
	uint8_t response[256];
	size_t response_length = 0U;
	int rc;

	if (!payload || payload_length == 0U ||
	    (payload_length >= 5U && memcmp(payload, "EZDL", 4U) == 0 &&
	     payload[4] == 1U)) {
		return false;
	}

	rc = edgez_config_handle_packet(payload, payload_length, response,
					sizeof(response), &response_length);
	if (rc != 0 || edgez_config_set_gateway_mac(source) != 0 ||
	    !edgez_config_should_autostart()) {
		return false;
	}

	LOG_INF("Direct PAwR zero-touch acceptance applied bytes=%u response=%u",
		(unsigned)payload_length, (unsigned)response_length);
	return true;
}

static bool sensor_receive_pending_packet(bool provisioning)
{
	struct sensor_pending_packet item;
	int64_t deadline = k_uptime_get() + EDGEZ_SENSOR_PROVISIONING_WINDOW_MS;

	do {
		int64_t remaining = deadline - k_uptime_get();

		if (remaining < 0) {
			remaining = 0;
		}
		if (k_msgq_get(&sensor_pending_packet_queue, &item,
			       K_MSEC(remaining)) != 0) {
			break;
		}
		/* Match the proven ESP32 sensor order: a factory-bootstrapped sensor
		 * receives cloud acceptance through the same pending-packet window as
		 * normal commands, so try the encrypted NetworkPacket before EZDL. */
		if (sensor_apply_zero_touch_acceptance(item.payload, item.length,
						 item.source)) {
			LOG_INF("cloud identity accepted from PAwR downlink window");
			return true;
		}
		if (!provisioning && sensor_apply_downlink(item.payload, item.length)) {
			return true;
		}
	} while (k_uptime_get() < deadline);

	return false;
}

static int acknowledge_pawr_beacon(const struct edgez_pawr_action_view *action,
				   const uint8_t *beacon, size_t beacon_len)
{
	uint8_t response[EDGEZ_PAWR_HEADER_LENGTH];
	uint8_t frame[EDGEZ_PAWR_ACTION_HEADER_LENGTH + sizeof(response)];
	size_t response_len = 0U;
	size_t frame_len = 0U;

	if (!edgez_pawr_encode_response(response, sizeof(response),
			edgez_pawr_payload_hash(beacon, beacon_len),
			mesh_runtime.config.compressed_ssid, &response_len) ||
	    !edgez_pawr_encode_action_frame(frame, sizeof(frame),
			pawr_sequence_number++, mesh_runtime.radio_info.mac_addr,
			action->source, response, response_len, &frame_len)) {
		return -EMSGSIZE;
	}

	const struct edgez_umac_tx_request request = {
		.token = pawr_tx_token++,
		.frame = frame,
		.frame_length = frame_len,
		.frame_type = EDGEZ_UMAC_TX_MANAGEMENT,
		.aid = 0U,
		.tid = 8U,
		.mcs = mesh_runtime.config.mcs,
		.attempts = mesh_runtime.config.attempts,
		.bandwidth_mhz = mesh_runtime.config.channel.operating_bw_mhz,
		.short_guard = mesh_runtime.config.short_guard,
		.immediate_status = true,
	};
	return edgez_umac_transmit(&mesh_runtime.umac, &request) == EDGEZ_UMAC_OK ?
		0 : -EIO;
}

static int route_pawr_beacon(const uint8_t source[6], const uint8_t *beacon,
			     size_t beacon_len)
{
	ai_edgez_halow_NetworkPacket *packet =
		k_calloc(1U, sizeof(ai_edgez_halow_NetworkPacket));
	uint8_t encoded[512];
	size_t encoded_len = 0;
	bool zero_touch = false;
	int rc;

	if (!packet) {
		return -ENOMEM;
	}
	pb_istream_t input = pb_istream_from_buffer(beacon, beacon_len);
	if (pb_decode(&input, ai_edgez_halow_NetworkPacket_fields, packet) &&
	    packet->operation == ai_edgez_halow_Operation_REQUEST &&
	    packet->which_body == ai_edgez_halow_NetworkPacket_device_settings_tag &&
	    packet->body.device_settings.device_type ==
		ai_edgez_halow_DeviceType_DEVICE_TYPE_SENSOR &&
	    packet->body.device_settings.user_name[0] != '\0' &&
	    packet->body.device_settings.user_public_key.size == 32U) {
		zero_touch = true;
		encoded_len = beacon_len;
		memcpy(encoded, beacon, beacon_len);
	} else {
		*packet = (ai_edgez_halow_NetworkPacket)
			ai_edgez_halow_NetworkPacket_init_zero;
		input = pb_istream_from_buffer(beacon, beacon_len);
		packet->from = mac_to_u64(source);
		packet->operation = ai_edgez_halow_Operation_BROADCAST;
		packet->interface = ai_edgez_halow_Interface_HALOW;
		packet->which_body = ai_edgez_halow_NetworkPacket_beacon_tag;
		if (!pb_decode(&input, ai_edgez_halow_Beacon_fields,
			       &packet->body.beacon)) {
			LOG_WRN("PAwR payload is neither Beacon nor zero-touch request");
			k_free(packet);
			return -EINVAL;
		}
		pb_ostream_t output = pb_ostream_from_buffer(encoded, sizeof(encoded));
		if (!pb_encode(&output, ai_edgez_halow_NetworkPacket_fields, packet)) {
			LOG_WRN("PAwR NetworkPacket encode failed: %s", PB_GET_ERROR(&output));
			k_free(packet);
			return -EMSGSIZE;
		}
		encoded_len = output.bytes_written;
	}
	k_free(packet);
	if (zero_touch) {
		int replay_rc = edgez_config_replay_provisioning_response(source);
		if (replay_rc != 0 && replay_rc != -ENOENT) {
			LOG_WRN("Cached zero-touch response replay failed rc=%d", replay_rc);
		}
	} else {
		/* Receiving a normal beacon proves provisioning completed. The cached
		 * response is one-shot state and can be released immediately. */
		edgez_config_forget_provisioning_response(source);
	}

	if (edgez_ble_can_notify()) {
		rc = edgez_ble_notify_packet(encoded, encoded_len);
		if (rc == 0) {
			LOG_INF("PAwR %s routed via BLE bytes=%u packet=%u",
				zero_touch ? "zero-touch request" : "Beacon",
				(unsigned)beacon_len, (unsigned)encoded_len);
			return 0;
		}
		LOG_WRN("PAwR Beacon BLE forwarding failed rc=%d; using HaLow mesh", rc);
	}

	rc = edgez_nrf_relay_send(NULL, encoded, encoded_len);
	if (rc == 0) {
		LOG_INF("PAwR %s routed via HaLow mesh bytes=%u packet=%u",
			zero_touch ? "zero-touch request" : "Beacon",
			(unsigned)beacon_len, (unsigned)encoded_len);
	} else {
		LOG_WRN("PAwR Beacon HaLow mesh broadcast failed rc=%d", rc);
	}
	return rc;
}

#if defined(CONFIG_OFFGRID_NRF54_MESH_PS_TEST)
enum mesh_ps_phase {
	MESH_PS_WAITING,
	MESH_PS_SETTLING,
	MESH_PS_BASELINE,
	MESH_PS_ENABLED,
};

static enum mesh_ps_phase mesh_ps_phase = MESH_PS_WAITING;
static uint32_t mesh_ps_phase_started;
static uint32_t mesh_ps_busy_samples;
static uint32_t mesh_ps_total_samples;
static uint32_t mesh_ps_baseline_x10;
#endif

extern bool edgez_halow_power_is_ready(void);
extern int edgez_halow_power_set(bool enabled);

#if EDGEZ_DIAGNOSTICS_ENABLED && !defined(EDGEZ_PLATFORM_ARCHIVE_BUILD)
static void join_diagnostic_sink(void *ctx,
				 enum edgez_diagnostic_level level,
				 const char *component,
				 const char *message)
{
	ARG_UNUSED(ctx);

	switch (level) {
	case EDGEZ_DIAGNOSTIC_ERROR:
		LOG_ERR("[%s] %s", component, message);
		break;
	case EDGEZ_DIAGNOSTIC_WARNING:
		LOG_WRN("[%s] %s", component, message);
		break;
	case EDGEZ_DIAGNOSTIC_INFO:
		LOG_INF("[%s] %s", component, message);
		break;
	case EDGEZ_DIAGNOSTIC_DEBUG:
		LOG_DBG("[%s] %s", component, message);
		break;
	case EDGEZ_DIAGNOSTIC_OFF:
	default:
		break;
	}
}
#endif

void edgez_request_reboot(uint32_t reason)
{
	ARG_UNUSED(reason);
	LOG_WRN("Reboot request ignored by join-only validation image");
}

static int reset_settings_storage(void)
{
	const struct flash_area *storage;
	int rc = flash_area_open(PARTITION_ID(storage_partition), &storage);

	if (rc != 0) {
		return rc;
	}
	LOG_WRN("Erasing settings storage partition (%u bytes)",
		(unsigned)storage->fa_size);
	rc = flash_area_erase(storage, 0, storage->fa_size);
	flash_area_close(storage);
	return rc;
}

#if DT_NODE_HAS_PROP(EDGEZ_USER_NODE, key_gpios)
static void user_button_interrupt(const struct device *port,
				  struct gpio_callback *callback,
				  gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(callback);
	ARG_UNUSED(pins);
	k_sem_give(&user_button_sem);
}
#endif

static void configure_user_button(void)
{
#if DT_NODE_HAS_PROP(EDGEZ_USER_NODE, key_gpios)
	if (!gpio_is_ready_dt(&user_button)) {
		LOG_WRN("KEY GPIO is not ready");
		return;
	}
	int rc = gpio_pin_configure_dt(&user_button, GPIO_INPUT);

	if (rc != 0) {
		LOG_WRN("KEY GPIO configuration failed rc=%d", rc);
		return;
	}
	gpio_init_callback(&user_button_callback, user_button_interrupt,
			   BIT(user_button.pin));
	rc = gpio_add_callback(user_button.port, &user_button_callback);
	if (rc == 0) {
		rc = gpio_pin_interrupt_configure_dt(&user_button,
					     GPIO_INT_EDGE_TO_ACTIVE);
	}
	if (rc != 0) {
		LOG_WRN("KEY wake interrupt configuration failed rc=%d", rc);
		return;
	}
	user_button_ready = true;
	LOG_INF("KEY wakes the sleeping sensor; hold for %u seconds to clear settings",
		EDGEZ_BUTTON_RESET_HOLD_MS / 1000U);
#endif
}

static void poll_user_button(void)
{
#if DT_NODE_HAS_PROP(EDGEZ_USER_NODE, key_gpios)
	static bool was_pressed;
	static bool reset_attempted;
	static int64_t pressed_at_ms;

	if (!user_button_ready) {
		return;
	}
	int pressed = gpio_pin_get_dt(&user_button);
	if (pressed < 0) {
		LOG_ERR("KEY read failed rc=%d", pressed);
		user_button_ready = false;
		return;
	}
	int64_t now_ms = k_uptime_get();
	if (pressed && !was_pressed) {
		pressed_at_ms = now_ms;
		reset_attempted = false;
		LOG_INF("KEY pressed");
	} else if (pressed && was_pressed && !reset_attempted &&
		   now_ms - pressed_at_ms >= EDGEZ_BUTTON_RESET_HOLD_MS) {
		reset_attempted = true;
		int rc = reset_settings_storage();

		if (rc == 0) {
			LOG_WRN("settings erased after KEY hold; rebooting unprovisioned");
			sys_reboot(SYS_REBOOT_COLD);
		}
		LOG_ERR("settings erase failed rc=%d", rc);
	} else if (!pressed && was_pressed) {
		int64_t held_ms = now_ms - pressed_at_ms;

		if (!reset_attempted && held_ms >= EDGEZ_BUTTON_DEBOUNCE_MS) {
			LOG_INF("KEY short press");
		}
	}
	was_pressed = pressed != 0;
#endif
}

static void sensor_sleep_until_button_or_timeout(int32_t remaining_ms)
{
#if DT_NODE_HAS_PROP(EDGEZ_USER_NODE, key_gpios)
	if (user_button_ready && remaining_ms > 0 &&
	    k_sem_take(&user_button_sem, K_MSEC(remaining_ms)) == 0) {
		/* The first press wakes the MCU. Keep polling only while it is held so
		 * the five-second reset duration starts after this wake, matching the
		 * ESP32 sensor behavior without periodic wakeups during normal sleep. */
		do {
			poll_user_button();
			k_msleep(10);
		} while (gpio_pin_get_dt(&user_button) > 0);
		poll_user_button();
		return;
	}
#endif
	k_msleep(remaining_ms > 0 ? remaining_ms : 1);
}

static int deliver_msdu(const uint8_t destination[6], const uint8_t source[6],
			const uint8_t transmitter[6], const uint8_t *payload,
			size_t payload_length)
{
	struct edgez_mesh_msdu msdu;
	uint8_t ethernet[EDGEZ_ETHERNET_HEADER_LEN + EDGEZ_BATADV_MAX_PACKET_LEN];
	size_t ethernet_length = 0;
	int rc = edgez_mesh_msdu_decode(&msdu, destination, source, payload,
					payload_length);
	if (rc == EDGEZ_UMAC_OK) {
		rc = edgez_mesh_msdu_to_ethernet(&msdu, ethernet, sizeof(ethernet),
						 &ethernet_length);
	}
	if (rc == EDGEZ_UMAC_OK) {
		(void)edgez_nrf_mesh_receive_ethernet(ethernet, ethernet_length,
						      transmitter);
	}
	return rc;
}

static int join_receive_data(void *ctx, const struct edgez_umac_rx_frame *radio,
			     const struct edgez_mesh_rx_data *frame)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(radio);
	return deliver_msdu(frame->destination, frame->source, frame->transmitter,
			    frame->payload, frame->payload_length);
}

static int join_receive_protected_data(
	void *ctx, const struct edgez_umac_rx_frame *radio,
	const struct edgez_mesh_rx_protected_data *frame)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(radio);
	uint8_t plaintext[EDGEZ_BATADV_MAX_PACKET_LEN];
	size_t plaintext_length = 0;
	uint64_t packet_number = 0;
	int rc = edgez_mesh_sae_provider_decrypt_data(
		&sae_provider, frame, plaintext, sizeof(plaintext),
		&plaintext_length, &packet_number);
	ARG_UNUSED(packet_number);
	return rc == EDGEZ_UMAC_OK ?
		deliver_msdu(frame->destination, frame->source, frame->transmitter,
			     plaintext, plaintext_length) : rc;
}

static int join_drop_qos_null(void *ctx,
			      const struct edgez_umac_rx_frame *radio,
			      const struct edgez_mesh_rx_qos_null *frame)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(radio);
	ARG_UNUSED(frame);
	return EDGEZ_UMAC_OK;
}

static int join_ignore_unsupported(void *ctx,
				   const struct edgez_umac_rx_frame *radio)
{
	ARG_UNUSED(ctx);
	struct edgez_pawr_action_view action;
	struct edgez_pawr_request_view request;
	uint32_t response_hash;
	uint32_t response_ssid;
	bool decoded_request;
#if defined(CONFIG_OFFGRID_NRF54_ROLE_SENSOR)
	struct edgez_pawr_bulk_control bulk_control;
#endif

	if (!radio || !edgez_pawr_decode_action_frame(
			radio->data, radio->length, &action)) {
		return EDGEZ_UMAC_OK;
	}
	decoded_request = edgez_pawr_decode_request(
		action.payload, action.payload_length, &request);
	if (decoded_request) {
		if (request.compressed_ssid != mesh_runtime.config.compressed_ssid) {
			return EDGEZ_UMAC_OK;
		}
		if (request.type == EDGEZ_PAWR_REQUEST_MESH_PACKET) {
			if (IS_ENABLED(CONFIG_OFFGRID_NRF54_ROLE_SENSOR) &&
			    request.payload_length <=
				    EDGEZ_SENSOR_PENDING_PACKET_MAX) {
				struct sensor_pending_packet item = {
					.length = (uint16_t)request.payload_length,
				};

				memcpy(item.source, action.source, sizeof(item.source));
				memcpy(item.payload, request.payload,
				       request.payload_length);
				if (k_msgq_put(&sensor_pending_packet_queue, &item,
					       K_NO_WAIT) != 0) {
					LOG_WRN("PAwR pending-packet queue full");
				}
				return EDGEZ_UMAC_OK;
			}
			(void)edgez_nrf_mesh_receive_pawr(
				request.payload, request.payload_length, action.source);
			return EDGEZ_UMAC_OK;
		}
	}
	if (decoded_request && request.type == EDGEZ_PAWR_REQUEST_BEACON) {
		LOG_INF("PAwR beacon request=%u from=%02x:%02x:%02x:%02x:%02x:%02x RSSI=%d dBm bytes=%u",
			request.request_id, action.source[0], action.source[1],
			action.source[2], action.source[3], action.source[4],
			action.source[5], radio->rssi_dbm,
			(unsigned)request.payload_length);
		int rc = acknowledge_pawr_beacon(&action, request.payload,
					 request.payload_length);
		if (rc != 0) {
			LOG_WRN("PAwR beacon acknowledgement failed rc=%d", rc);
		}
		if (!IS_ENABLED(CONFIG_OFFGRID_NRF54_ROLE_SENSOR)) {
			rc = route_pawr_beacon(action.source, request.payload,
					       request.payload_length);
			if (rc != 0) {
				LOG_WRN("PAwR Beacon routing exhausted rc=%d", rc);
			}
		}
		return EDGEZ_UMAC_OK;
	}
#if defined(CONFIG_OFFGRID_NRF54_ROLE_SENSOR)
	if (edgez_pawr_bulk_control_decode(action.payload, action.payload_length,
					    &bulk_control) &&
	    bulk_control.compressed_ssid == mesh_runtime.config.compressed_ssid &&
	    bulk_control.kind == EDGEZ_PAWR_BULK_KIND_BLOCK_ACK) {
		k_spinlock_key_t key = k_spin_lock(&sensor_pawr_lock);
		memcpy(sensor_bulk_ack_payload, action.payload,
		       EDGEZ_PAWR_BULK_CONTROL_SIZE);
		sensor_bulk_ack_length = EDGEZ_PAWR_BULK_CONTROL_SIZE;
		k_spin_unlock(&sensor_pawr_lock, key);
		k_sem_give(&sensor_bulk_ack_sem);
		return EDGEZ_UMAC_OK;
	}
#endif
	if (edgez_pawr_decode_response(action.payload, action.payload_length,
				       &response_hash, &response_ssid) &&
	    response_ssid == mesh_runtime.config.compressed_ssid) {
		k_spinlock_key_t key = k_spin_lock(&sensor_pawr_lock);
		bool matched = sensor_pawr_waiting &&
			response_hash == sensor_pawr_expected_hash;

		if (matched) {
			sensor_pawr_waiting = false;
			sensor_pawr_downlink_pending =
				(action.payload[7] & EDGEZ_PAWR_RESPONSE_DOWNLINK_PENDING) != 0U;
		}
		k_spin_unlock(&sensor_pawr_lock, key);
		if (matched) {
			LOG_INF("sensor PAwR response hash=%08x from=%02x:%02x:%02x:%02x:%02x:%02x RSSI=%d dBm",
				response_hash, action.source[0], action.source[1],
				action.source[2], action.source[3], action.source[4],
				action.source[5], radio->rssi_dbm);
			k_sem_give(&sensor_pawr_response_sem);
		}
	}
	return EDGEZ_UMAC_OK;
}

static void join_hardware_restart(void *ctx)
{
	ARG_UNUSED(ctx);
	LOG_ERR("Morse RF requested a hardware restart");
}

static void join_tx_status(void *ctx,
			   const struct edgez_umac_tx_status *status)
{
	ARG_UNUSED(ctx);
	tx_status_count++;
	LOG_INF("mesh TX status token=%" PRIuPTR " len=%u attempts=%u flags=0x%08" PRIx32,
		status->token, (unsigned)status->frame_length,
		status->attempts, status->status_flags);
	if ((status->status_flags & EDGEZ_RF_TX_STATUS_FAILED) != 0U) {
		tx_failure_count++;
		LOG_WRN("mesh TX failed token=%" PRIuPTR " len=%u attempts=%u",
			status->token, (unsigned)status->frame_length,
			status->attempts);
	}
}

static const struct edgez_umac_mesh_runtime_events join_events = {
	.data = join_receive_data,
	.protected_data = join_receive_protected_data,
	.qos_null = join_drop_qos_null,
	.unsupported = join_ignore_unsupported,
	.tx_status = join_tx_status,
	.hardware_restart_required = join_hardware_restart,
};

static const char *peering_state_name(enum edgez_mesh_peering_state state)
{
	switch (state) {
	case EDGEZ_PEERING_IDLE: return "idle";
	case EDGEZ_PEERING_SAE_AUTHENTICATING: return "sae";
	case EDGEZ_PEERING_OPEN_SENT: return "open-sent";
	case EDGEZ_PEERING_OPEN_RECEIVED: return "open-received";
	case EDGEZ_PEERING_CONFIRM_RECEIVED: return "confirm-received";
	case EDGEZ_PEERING_ESTABLISHED: return "established";
	case EDGEZ_PEERING_HOLDING: return "holding";
	default: return "unknown";
	}
}

static bool has_established_peer(void)
{
	for (size_t i = 0; i < EDGEZ_UMAC_MAX_PEERS; ++i) {
		const struct edgez_mesh_peering_slot *slot =
			&mesh_runtime.peering_manager.slots[i];
		if (slot->in_use &&
		    slot->peering.state == EDGEZ_PEERING_ESTABLISHED) {
			return true;
		}
	}
	return false;
}

static size_t batman_snapshot_peers(void *ctx, uint8_t (*peers)[6],
				    size_t capacity)
{
	struct edgez_umac_mesh_runtime *runtime = ctx;
	size_t count = 0;

	for (size_t i = 0; i < EDGEZ_UMAC_MAX_PEERS && count < capacity; ++i) {
		const struct edgez_umac_peer *peer = &runtime->umac.peers[i];
		if (peer->in_use && peer->state == EDGEZ_UMAC_PEER_AUTHORIZED) {
			memcpy(peers[count++], peer->mac_addr, 6);
		}
	}
	return count;
}

static int batman_transmit(void *ctx, const uint8_t *frame, size_t frame_len,
			   const uint8_t receiver[6])
{
	ARG_UNUSED(ctx);
	int rc;

	k_mutex_lock(&batman_tx_lock, K_FOREVER);
	if (mesh_secure) {
		rc = edgez_mesh_sae_provider_transmit_ethernet(
			&sae_provider, receiver, frame, frame_len, 0);
	} else {
		const struct edgez_umac_peer *peer =
			edgez_umac_peer_find(&mesh_runtime.umac, receiver);
		if (!peer || peer->state != EDGEZ_UMAC_PEER_AUTHORIZED) {
			k_mutex_unlock(&batman_tx_lock);
			return -ENOENT;
		}
		struct edgez_mesh_data_header header = {
			.sequence_number = open_data_sequence++ & 0x0fffU,
			.tid = 0,
			.protected_frame = false,
			.mesh_control = {
				.address_extension = EDGEZ_MESH_AE_ADDR5_ADDR6,
				.ttl = 31U,
				.sequence_number = open_mesh_sequence++,
			},
		};
		size_t encoded_length = 0;
		memcpy(header.receiver, receiver, 6);
		memcpy(header.transmitter, mesh_runtime.radio_info.mac_addr, 6);
		memcpy(header.destination, frame, 6);
		memcpy(header.source, mesh_runtime.radio_info.mac_addr, 6);
		memcpy(header.mesh_control.address5, frame, 6);
		memcpy(header.mesh_control.address6, frame + 6, 6);
		rc = edgez_mesh_data_frame_encode(open_data_frame,
						  sizeof(open_data_frame), &header,
						  frame, frame_len, &encoded_length);
		if (rc == EDGEZ_UMAC_OK) {
			const struct edgez_umac_tx_request request = {
				.token = open_tx_token++,
				.frame = open_data_frame,
				.frame_length = encoded_length,
				.frame_type = EDGEZ_UMAC_TX_DATA,
				.aid = peer->aid,
				.tid = 0,
				.mcs = 0,
				.attempts = 2,
				.bandwidth_mhz = (uint8_t)applied_profile.mesh_bandwidth_mhz,
				.immediate_status = true,
			};
			rc = edgez_umac_transmit(&mesh_runtime.umac, &request);
		}
	}
	k_mutex_unlock(&batman_tx_lock);
	batman_tx_count++;
	if (rc != EDGEZ_UMAC_OK) {
		batman_tx_failure_count++;
		LOG_WRN("BATMAN radio TX rejected peer=%02x:%02x:%02x:%02x:%02x:%02x len=%u rc=%d",
			receiver[0], receiver[1], receiver[2], receiver[3],
			receiver[4], receiver[5], (unsigned)frame_len, rc);
	}
	return rc == EDGEZ_UMAC_OK ? 0 : -EIO;
}

static int batman_transmit_pawr(void *ctx, const uint8_t destination[6],
			       const uint8_t *frame, size_t frame_len)
{
	ARG_UNUSED(ctx);
	uint8_t packet[EDGEZ_PAWR_HEADER_LENGTH + EDGEZ_PAWR_MAX_ACTION_PAYLOAD];
	uint8_t action[EDGEZ_PAWR_ACTION_HEADER_LENGTH + sizeof(packet)];
	size_t packet_len = 0U;
	size_t action_len = 0U;
	uint32_t request_id = ++pawr_request_id;

	if (!edgez_pawr_encode_mesh_packet(packet, sizeof(packet), request_id,
					   mesh_runtime.config.compressed_ssid,
					   frame, frame_len, &packet_len) ||
	    !edgez_pawr_encode_action_frame(
			action, sizeof(action), pawr_sequence_number++,
			mesh_runtime.radio_info.mac_addr, destination,
			packet, packet_len, &action_len)) {
		return -EMSGSIZE;
	}
	const struct edgez_umac_tx_request request = {
		.token = pawr_tx_token++,
		.frame = action,
		.frame_length = action_len,
		.frame_type = EDGEZ_UMAC_TX_MANAGEMENT,
		.aid = 0U,
		.tid = 8U,
		.mcs = mesh_runtime.config.mcs,
		.attempts = mesh_runtime.config.attempts,
		.bandwidth_mhz = mesh_runtime.config.channel.operating_bw_mhz,
		.short_guard = mesh_runtime.config.short_guard,
		.immediate_status = true,
	};
	int rc = edgez_umac_transmit(&mesh_runtime.umac, &request);
	if (rc == EDGEZ_UMAC_OK) {
		LOG_INF("PAwR direct packet queued id=%u bytes=%u to=%02x:%02x:%02x:%02x:%02x:%02x",
			request_id, (unsigned)frame_len,
			destination[0], destination[1], destination[2],
			destination[3], destination[4], destination[5]);
		return 0;
	}
	LOG_WRN("PAwR direct packet failed id=%u rc=%d driver=%d",
		request_id, rc, mesh_runtime.umac.last_driver_status);
	return -EIO;
}

static uint32_t batman_clock_ms(void *ctx)
{
	ARG_UNUSED(ctx);
	return k_uptime_get_32();
}

static uint32_t batman_random(void *ctx)
{
	ARG_UNUSED(ctx);
	return sys_rand32_get();
}

static void batman_lock(void *ctx)
{
	ARG_UNUSED(ctx);
	k_mutex_lock(&batman_state_lock, K_FOREVER);
}

static void batman_unlock(void *ctx)
{
	ARG_UNUSED(ctx);
	k_mutex_unlock(&batman_state_lock);
}

static const struct edgez_nrf_mesh_transport batman_transport = {
	.peers = batman_snapshot_peers,
	.transmit = batman_transmit,
	.transmit_pawr = batman_transmit_pawr,
	.clock_ms = batman_clock_ms,
	.random = batman_random,
	.lock = batman_lock,
	.unlock = batman_unlock,
	.ctx = &mesh_runtime,
};

static bool same_radio_profile(const struct edgez_halow_profile *a,
			       const struct edgez_halow_profile *b)
{
	return strcmp(a->country_code, b->country_code) == 0 &&
		strcmp(a->mesh_id, b->mesh_id) == 0 &&
		strcmp(a->passphrase, b->passphrase) == 0 &&
		a->mesh_frequency_khz == b->mesh_frequency_khz &&
		a->mesh_bandwidth_mhz == b->mesh_bandwidth_mhz;
}

static uint32_t sensor_update_interval_ms(void)
{
	return edgez_config_should_autostart() ?
		sensor_settings.value.sampling_interval_ms :
		CONFIG_OFFGRID_NRF54_SENSOR_BOOTSTRAP_INTERVAL_SECONDS * 1000U;
}

static int sensor_load_factory_bootstrap(void)
{
	struct edgez_factory_identity factory = {0};
	uint32_t frequency_khz = 0;
	uint8_t bandwidth_mhz = 0;

	if (edgez_factory_data_get(&factory) != 0 ||
	    strlen(factory.country) != 2U || factory.channel == 0U ||
	    !edgez_rf_resolve_s1g_channel(factory.country, factory.channel,
		&frequency_khz, &bandwidth_mhz) || bandwidth_mhz != 1U) {
		memset(&factory, 0, sizeof(factory));
		return -EINVAL;
	}
	memcpy(sensor_bootstrap_country, factory.country, 2U);
	sensor_bootstrap_country[2] = '\0';
	strcpy(sensor_bootstrap_mesh_id, factory.has_halow_profile ?
	       factory.mesh_id : "edgez-zero-touch");
	if (factory.has_halow_profile) {
		strcpy(sensor_bootstrap_passphrase, factory.passphrase);
	} else {
		sensor_bootstrap_passphrase[0] = '\0';
	}
	sensor_bootstrap_channel = factory.channel;
	LOG_INF("factory discovery profile country=%s mesh_id=%s channel=%u frequency=%u kHz bandwidth=%u MHz",
		sensor_bootstrap_country, sensor_bootstrap_mesh_id,
		sensor_bootstrap_channel,
		frequency_khz, bandwidth_mhz);
	memset(&factory, 0, sizeof(factory));
	return 0;
}

static int sensor_bootstrap_profile(struct edgez_halow_profile *profile)
{
	uint32_t frequency_khz = 0;
	uint8_t bandwidth_mhz = 0;

	if (!profile || strlen(sensor_bootstrap_country) != 2U ||
	    sensor_bootstrap_mesh_id[0] == '\0' ||
	    !edgez_rf_resolve_s1g_channel(
		sensor_bootstrap_country,
		sensor_bootstrap_channel, &frequency_khz, &bandwidth_mhz) ||
	    bandwidth_mhz != 1U) {
		return -EINVAL;
	}
	memset(profile, 0, sizeof(*profile));
	memcpy(profile->country_code, sensor_bootstrap_country, 2U);
	profile->country_code[2] = '\0';
	strcpy(profile->mesh_id, sensor_bootstrap_mesh_id);
	strcpy(profile->passphrase, sensor_bootstrap_passphrase);
	profile->mesh_frequency_khz = frequency_khz;
	profile->mesh_bandwidth_mhz = bandwidth_mhz;
	profile->beacon_interval_seconds =
		CONFIG_OFFGRID_NRF54_SENSOR_BOOTSTRAP_INTERVAL_SECONDS;
	profile->device_type = ai_edgez_halow_DeviceType_DEVICE_TYPE_SENSOR;
	profile->sleep_mode_enabled = true;
	return 0;
}

static bool sensor_advance_bootstrap_channel(void)
{
	const uint8_t previous = sensor_bootstrap_channel;
	uint32_t frequency_khz;
	uint8_t bandwidth_mhz;

	/* Channel numbers are country-specific and may be sparse. Probe through
	 * the public RF policy API so the rotation follows the MM8108 regulatory
	 * database rather than duplicating a US/EU channel table here. Zero-touch
	 * discovery deliberately uses only 1 MHz channels so companions have one
	 * predictable provisioning bandwidth to monitor. */
	for (uint16_t offset = 1U; offset <= UINT8_MAX; ++offset) {
		uint8_t candidate = (uint8_t)(previous + offset);

		if (candidate == 0U) {
			continue;
		}
		if (edgez_rf_resolve_s1g_channel(
			    sensor_bootstrap_country,
			    candidate, &frequency_khz, &bandwidth_mhz) &&
		    bandwidth_mhz == 1U) {
			sensor_bootstrap_channel = candidate;
			LOG_INF("zero-touch discovery channel %u -> %u (%u kHz/%u MHz)",
				previous, candidate, frequency_khz, bandwidth_mhz);
			return true;
		}
	}
	LOG_WRN("no alternate valid zero-touch channel for country=%s",
		sensor_bootstrap_country);
	return false;
}

static int sensor_retune_bootstrap_channel(void)
{
	uint32_t frequency_khz;
	uint8_t bandwidth_mhz;

	if (!mesh_started || !mesh_runtime.umac.driver ||
	    !mesh_runtime.umac.driver->set_channel ||
	    !edgez_rf_resolve_s1g_channel(
		sensor_bootstrap_country,
		sensor_bootstrap_channel, &frequency_khz, &bandwidth_mhz) ||
	    bandwidth_mhz != 1U) {
		return -EINVAL;
	}

	const struct edgez_umac_channel channel = {
		.operating_frequency_hz = frequency_khz * 1000U,
		.operating_bw_mhz = 1U,
		.primary_bw_mhz = EDGEZ_MESH_PRIMARY_BANDWIDTH_MHZ,
		.primary_1mhz_channel_index = 0U,
	};
	int rc = mesh_runtime.umac.driver->set_channel(
		mesh_runtime.umac.driver_ctx, &channel);

	if (rc != 0) {
		LOG_WRN("zero-touch retune failed channel=%u frequency=%u kHz rc=%d",
			sensor_bootstrap_channel, frequency_khz, rc);
		return -EIO;
	}
	mesh_runtime.config.channel = channel;
	applied_profile.mesh_frequency_khz = frequency_khz;
	applied_profile.mesh_bandwidth_mhz = 1U;
	/* mmdrv_set_channel() is synchronous. Leave a short guard for the RX path
	 * before transmitting the next connectionless action frame. */
	k_msleep(10);
	return 0;
}

static int sensor_pawr_send_once(uint32_t wake_interval_ms)
{
	static const uint8_t broadcast[6] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff
	};
	uint8_t vendor_ies[EDGEZ_VENDOR_IES_MAX_LEN];
	uint8_t beacon[EDGEZ_VENDOR_IES_MAX_LEN];
	uint8_t packet[EDGEZ_PAWR_HEADER_LENGTH + EDGEZ_VENDOR_IES_MAX_LEN];
	uint8_t frame[EDGEZ_PAWR_ACTION_HEADER_LENGTH + sizeof(packet)];
	size_t vendor_ies_len = 0U;
	size_t beacon_len = 0U;
	size_t packet_len = 0U;
	size_t frame_len = 0U;
	uint32_t request_id = ++pawr_request_id;
	uint32_t hash;
	const bool provisioning = !edgez_config_should_autostart();
	struct edgez_halow_profile persisted_profile = {0};
	const uint32_t response_timeout_ms = provisioning ?
		EDGEZ_SENSOR_PROVISIONING_ACK_TIMEOUT_MS :
		EDGEZ_SENSOR_RESPONSE_TIMEOUT_MS;
	int rc;

	if (provisioning) {
		rc = edgez_config_build_zero_touch_request(
			mesh_runtime.radio_info.mac_addr, beacon, sizeof(beacon),
			&beacon_len);
		if (rc != 0) {
			LOG_WRN("zero-touch request unavailable rc=%d", rc);
			return rc;
		}
	} else {
		/* Refresh GPS, battery and IMU values, then send only the protobuf
		 * payload carried by the vendor IE. */
		rc = edgez_config_build_vendor_ies(vendor_ies, sizeof(vendor_ies),
					       mesh_runtime.radio_info.mac_addr,
					       &vendor_ies_len);
		if (rc != 0 || edgez_config_copy_halow_beacon(
					beacon, sizeof(beacon), &beacon_len) != 0) {
			LOG_WRN("sensor PAwR payload unavailable rc=%d IE=%u", rc,
				(unsigned)vendor_ies_len);
			return rc != 0 ? rc : -EAGAIN;
		}
	}
	hash = edgez_pawr_payload_hash(beacon, beacon_len);
	edgez_config_get_profile(&persisted_profile);
	if (!edgez_pawr_encode_typed_request(
			packet, sizeof(packet), EDGEZ_PAWR_REQUEST_BEACON,
			request_id, wake_interval_ms,
			mesh_runtime.config.compressed_ssid, beacon, beacon_len,
			&packet_len)) {
		return -EMSGSIZE;
	}
	const uint8_t *destination = provisioning ? broadcast :
		persisted_profile.gateway_mac;
	if (!edgez_pawr_encode_action_frame(
			frame, sizeof(frame), pawr_sequence_number++,
			mesh_runtime.radio_info.mac_addr,
			destination, packet, packet_len,
			&frame_len)) {
		return -EMSGSIZE;
	}

	const struct edgez_umac_tx_request request = {
		.token = pawr_tx_token++,
		.frame = frame,
		.frame_length = frame_len,
		.frame_type = EDGEZ_UMAC_TX_MANAGEMENT,
		.aid = 0U,
		.tid = 8U,
		.mcs = mesh_runtime.config.mcs,
		.attempts = 1U,
		.bandwidth_mhz = mesh_runtime.config.channel.operating_bw_mhz,
		.short_guard = mesh_runtime.config.short_guard,
		.no_ack = provisioning,
		.immediate_status = true,
	};
	k_sem_reset(&sensor_pawr_response_sem);
	k_spinlock_key_t key = k_spin_lock(&sensor_pawr_lock);
	sensor_pawr_expected_hash = hash;
	sensor_pawr_waiting = true;
	sensor_pawr_downlink_pending = false;
	k_spin_unlock(&sensor_pawr_lock, key);
	k_msgq_purge(&sensor_pending_packet_queue);
	rc = edgez_umac_transmit(&mesh_runtime.umac, &request);
	if (rc != EDGEZ_UMAC_OK) {
		key = k_spin_lock(&sensor_pawr_lock);
		sensor_pawr_waiting = false;
		k_spin_unlock(&sensor_pawr_lock, key);
		LOG_WRN("sensor PAwR request=%u rejected rc=%d driver=%d",
			request_id, rc, mesh_runtime.umac.last_driver_status);
		return -EIO;
	}
	edgez_imu_note_halow_tx();
	LOG_INF("sensor PAwR %s request=%u queued payload=%u interval=%ums hash=%08x; waiting up to %ums for acknowledgement",
		provisioning ? "zero-touch" : "telemetry",
		request_id, (unsigned)beacon_len, wake_interval_ms, hash,
		response_timeout_ms);
	if (k_sem_take(&sensor_pawr_response_sem,
			       K_MSEC(response_timeout_ms)) == 0) {
		/* The acknowledgement only confirms that a companion received this
		 * request.  A cached encrypted profile is returned in a separate PAwR
		 * mesh-packet action frame, so an unprovisioned sensor must keep RX up
		 * briefly after the ACK instead of cutting the HaLow rail immediately. */
		if ((provisioning || sensor_pawr_downlink_pending) &&
		    !sensor_receive_pending_packet(provisioning)) {
			LOG_WRN("gateway advertised a %s but none was accepted",
				provisioning ? "zero-touch acceptance" : "downlink");
		}
		return 0;
	}
	key = k_spin_lock(&sensor_pawr_lock);
	sensor_pawr_waiting = false;
	k_spin_unlock(&sensor_pawr_lock, key);
	LOG_WRN("sensor PAwR request=%u acknowledgement timeout", request_id);
	return -ETIMEDOUT;
}

#if defined(CONFIG_OFFGRID_NRF54_ROLE_SENSOR)

#if defined(CONFIG_OFFGRID_NRF54_SENSOR_PAWR_BULK_TEST)
static int sensor_pawr_bulk_profile(struct edgez_halow_profile *profile)
{
	uint32_t frequency_khz = 0U;
	uint8_t bandwidth_mhz = 0U;

	if (profile == NULL ||
	    strlen(sensor_bootstrap_country) != 2U ||
	    !edgez_rf_resolve_s1g_channel(
		sensor_bootstrap_country, sensor_bootstrap_channel,
		&frequency_khz, &bandwidth_mhz)) {
		LOG_ERR("PAWR_REG invalid bulk profile country=%s channel=%u",
			sensor_bootstrap_country,
			(unsigned)sensor_bootstrap_channel);
		return -EINVAL;
	}

	memcpy(profile->country_code, sensor_bootstrap_country, 2U);
	profile->country_code[2] = '\0';
	profile->mesh_frequency_khz = frequency_khz;
	profile->mesh_bandwidth_mhz = bandwidth_mhz;
	LOG_INF("PAWR_REG bulk profile country=%s channel=%u frequency=%u kHz bandwidth=%u MHz",
		profile->country_code,
		(unsigned)sensor_bootstrap_channel,
		(unsigned)frequency_khz, (unsigned)bandwidth_mhz);
	return 0;
}
#endif

static int sensor_pawr_enable_bulk_burst_mode(void)
{
	const struct edgez_umac_mesh_runtime_config *config = &mesh_runtime.config;
	const struct mmwlan_s1g_channel *selected = mmregdb_lookup_channel(
		config->country_code, config->channel.operating_frequency_hz,
		config->channel.operating_bw_mhz);

	if (selected == NULL) {
		LOG_ERR("PAWR_REG burst setup failed: channel missing country=%s frequency=%u Hz bandwidth=%u MHz",
			config->country_code,
			(unsigned)config->channel.operating_frequency_hz,
			(unsigned)config->channel.operating_bw_mhz);
		return -ENOENT;
	}

	enum mmwlan_status status = mmdrv_set_duty_cycle(
		selected->duty_cycle_sta,
		selected->duty_cycle_omit_ctrl_resp,
		MMWLAN_DUTY_CYCLE_MODE_BURST);
	if (status != MMWLAN_SUCCESS) {
		LOG_ERR("PAWR_REG burst setup failed status=%d", status);
		return -EIO;
	}

	struct mmwlan_duty_cycle_stats stats = {0};
	status = mmdrv_get_duty_cycle(&stats);
	if (status != MMWLAN_SUCCESS ||
	    stats.duty_cycle != selected->duty_cycle_sta ||
	    stats.mode != MMWLAN_DUTY_CYCLE_MODE_BURST) {
		LOG_ERR("PAWR_REG burst verification failed status=%d duty=%u mode=%u",
			status, (unsigned)stats.duty_cycle, (unsigned)stats.mode);
		return -EIO;
	}

	LOG_INF("PAWR_REG burst enabled source=mm-iot-regdb country=%s frequency=%u Hz bandwidth=%u MHz regdb_duty=%u.%02u%% duty=%u.%02u%% airtime_remaining_us=%u window_us=%u",
		config->country_code,
		(unsigned)config->channel.operating_frequency_hz,
		(unsigned)config->channel.operating_bw_mhz,
		(unsigned)(selected->duty_cycle_sta / 100U),
		(unsigned)(selected->duty_cycle_sta % 100U),
		(unsigned)(stats.duty_cycle / 100U),
		(unsigned)(stats.duty_cycle % 100U),
		(unsigned)stats.burst_airtime_remaining_us,
		(unsigned)stats.burst_window_duration_us);
	return 0;
}

static struct edgez_pawr_bulk_regulatory sensor_pawr_bulk_regulatory(void)
{
	const struct edgez_umac_mesh_runtime_config *config = &mesh_runtime.config;
	struct edgez_pawr_bulk_regulatory result = {
		.country_code = {
			config->country_code[0], config->country_code[1], '\0'
		},
		.frequency_khz = config->channel.operating_frequency_hz / 1000U,
		.bandwidth_mhz = config->channel.operating_bw_mhz,
		.flags = EDGEZ_PAWR_BULK_REG_FLAG_TERMINAL_NODE,
	};
	struct edgez_rf_regulatory_channel policy = {0};
	if (edgez_rf_resolve_regulatory_channel(
		    result.country_code, result.frequency_khz * 1000U,
		    result.bandwidth_mhz, &policy)) {
		result.duty_cycle_centi_percent =
			policy.duty_cycle_centi_percent;
		result.max_eirp_dbm = policy.max_eirp_dbm;
		result.flags |= EDGEZ_PAWR_BULK_REG_FLAG_REGDB_VALID;
	}

	struct mmwlan_duty_cycle_stats stats = {0};
	if (mmdrv_get_duty_cycle(&stats) == MMWLAN_SUCCESS) {
		result.duty_cycle_centi_percent = (uint16_t)stats.duty_cycle;
		result.duty_cycle_mode = (uint8_t)stats.mode;
		result.flags |= EDGEZ_PAWR_BULK_REG_FLAG_CHIP_DUTY_VALID;
	}

	LOG_INF("PAWR_REG sender country=%s frequency=%u kHz bandwidth=%u MHz duty=%u.%02u%% mode=%u max_eirp=%d dBm flags=0x%02x",
		result.country_code, (unsigned)result.frequency_khz,
		(unsigned)result.bandwidth_mhz,
		(unsigned)(result.duty_cycle_centi_percent / 100U),
		(unsigned)(result.duty_cycle_centi_percent % 100U),
		(unsigned)result.duty_cycle_mode, (int)result.max_eirp_dbm,
		(unsigned)result.flags);
	return result;
}

static int sensor_pawr_queue_bulk_chunk(const uint8_t *payload,
					size_t payload_len,
					uint32_t wake_interval_ms)
{
	uint8_t packet[EDGEZ_PAWR_HEADER_LENGTH + EDGEZ_PAWR_MAX_ACTION_PAYLOAD];
	uint8_t action[EDGEZ_PAWR_ACTION_HEADER_LENGTH + sizeof(packet)];
	size_t packet_len = 0U;
	size_t action_len = 0U;
	uint32_t request_id = ++pawr_request_id;

	if (!edgez_pawr_encode_typed_request(
			packet, sizeof(packet), EDGEZ_PAWR_REQUEST_BULK_STREAM,
			request_id, wake_interval_ms,
			mesh_runtime.config.compressed_ssid, payload, payload_len,
			&packet_len) ||
	    packet_len > EDGEZ_PAWR_MAX_ACTION_PAYLOAD ||
	    !edgez_pawr_encode_action_frame(
			action, sizeof(action), pawr_sequence_number++,
			mesh_runtime.radio_info.mac_addr,
			applied_profile.gateway_mac,
			packet, packet_len, &action_len)) {
		return -EMSGSIZE;
	}

	const uintptr_t token = pawr_tx_token++;
	const struct edgez_umac_tx_request request = {
		.token = token,
		.frame = action,
		.frame_length = action_len,
		.frame_type = EDGEZ_UMAC_TX_MANAGEMENT,
		.aid = 0U,
		.tid = 8U,
		.mcs = mesh_runtime.config.mcs,
		.attempts = 1U,
		.bandwidth_mhz = mesh_runtime.config.channel.operating_bw_mhz,
		.short_guard = mesh_runtime.config.short_guard,
		.no_ack = false,
		.immediate_status = true,
	};

	int rc = edgez_umac_transmit(&mesh_runtime.umac, &request);
	if (payload_len >= EDGEZ_PAWR_BULK_HEADER_SIZE + 1000U ||
	    rc != EDGEZ_UMAC_OK) {
		LOG_INF("PAWR_BULK_TX queue token=%" PRIuPTR
			" data=%u payload=%u action=%u rc=%d",
			token,
			(unsigned)(payload_len >= EDGEZ_PAWR_BULK_HEADER_SIZE ?
				payload_len - EDGEZ_PAWR_BULK_HEADER_SIZE : 0U),
			(unsigned)payload_len, (unsigned)action_len, rc);
	}
	if (rc != EDGEZ_UMAC_OK) {
		return -EIO;
	}
	edgez_imu_note_halow_tx();
	return 0;
}

#if !defined(CONFIG_OFFGRID_NRF54_SENSOR_PAWR_BULK_TEST)
static uint32_t sensor_fnv1a32(const uint8_t *data, size_t length)
{
	uint32_t digest = UINT32_C(2166136261);

	for (size_t i = 0U; i < length; ++i) {
		digest ^= data[i];
		digest *= UINT32_C(16777619);
	}
	return digest;
}

static int sensor_pawr_send_block(const uint8_t *data, size_t length,
				  uint8_t sample_kind,
				  uint32_t wake_interval_ms)
{
	if (!data || length == 0U || length > EDGEZ_PAWR_BULK_MAX_CHUNK_DATA) {
		return -EMSGSIZE;
	}
	int rc = sensor_pawr_enable_bulk_burst_mode();
	if (rc != 0) {
		return rc;
	}
	uint32_t session_id = sys_rand32_get();
	if (session_id == 0U) {
		session_id = 1U;
	}
	const struct edgez_pawr_bulk_regulatory regulatory =
		sensor_pawr_bulk_regulatory();
	const struct edgez_pawr_bulk_chunk chunk = {
		.compressed_ssid = mesh_runtime.config.compressed_ssid,
		.session_id = session_id,
		.total_length = (uint32_t)length,
		.offset = 0U,
		.expected_digest = sensor_fnv1a32(data, length),
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
		.regulatory = regulatory,
		.data = data,
	};
	uint8_t encoded[EDGEZ_PAWR_BULK_HEADER_SIZE +
			EDGEZ_PAWR_BULK_MAX_CHUNK_DATA];
	size_t encoded_length = edgez_pawr_bulk_encode(
		encoded, sizeof(encoded), &chunk);
	if (encoded_length == 0U) {
		return -EMSGSIZE;
	}

	for (unsigned attempt = 0U; attempt < EDGEZ_SENSOR_BULK_ACK_ATTEMPTS;
	     ++attempt) {
		k_sem_reset(&sensor_bulk_ack_sem);
		k_spinlock_key_t key = k_spin_lock(&sensor_pawr_lock);
		sensor_bulk_ack_length = 0U;
		k_spin_unlock(&sensor_pawr_lock, key);
		rc = sensor_pawr_queue_bulk_chunk(encoded, encoded_length,
			wake_interval_ms);
		if (rc != 0) {
			continue;
		}
		uint32_t timeout_ms = edgez_pawr_bulk_ack_timeout_ms(
			(uint16_t)length, 1U);
		if (k_sem_take(&sensor_bulk_ack_sem, K_MSEC(timeout_ms)) != 0) {
			continue;
		}
		uint8_t ack[EDGEZ_PAWR_BULK_CONTROL_SIZE];
		key = k_spin_lock(&sensor_pawr_lock);
		size_t ack_length = sensor_bulk_ack_length;
		if (ack_length == sizeof(ack)) {
			memcpy(ack, sensor_bulk_ack_payload, sizeof(ack));
		}
		k_spin_unlock(&sensor_pawr_lock, key);
		struct edgez_pawr_bulk_control control;
		if (ack_length == sizeof(ack) &&
		    edgez_pawr_bulk_control_decode(ack, ack_length, &control) &&
		    edgez_pawr_bulk_control_matches(
			&control, mesh_runtime.config.compressed_ssid,
			session_id, sample_kind, 0U, 0U, 1U)) {
			LOG_INF("sensor block delivered sequence=%u bytes=%u",
				sensor_block_sequence, (unsigned)length);
			return 0;
		}
	}
	return -ETIMEDOUT;
}

static int sensor_send_imu_block(uint32_t wake_interval_ms)
{
	uint8_t count = sensor_block_sample_count;
	if (sensor_settings.value.block_sampling_count == 0U || count == 0U) {
		return -EAGAIN;
	}
	uint8_t block[EDGEZ_SENSOR_BLOCK_HEADER_SIZE +
		EDGEZ_SENSOR_SETTINGS_MAX_SAMPLES_PER_BLOCK *
		EDGEZ_SENSOR_BLOCK_IMU_RECORD_SIZE];
	size_t offset = edgez_sensor_block_encode_header(
		block, sizeof(block), EDGEZ_SENSOR_BLOCK_KIND_IMU,
		EDGEZ_SENSOR_BLOCK_IMU_RECORD_SIZE, count,
		sensor_block_sequence, EDGEZ_IMU_SAMPLE_PERIOD_MS,
		&sensor_settings.value);
	if (offset == 0U) {
		return -EINVAL;
	}
	for (uint8_t sample_index = 0U; sample_index < count; ++sample_index) {
		const struct edgez_imu_sample *sample =
			&sensor_block_samples[sample_index];
		for (size_t axis = 0U; axis < 3U; ++axis) {
			edgez_sensor_put_float_le(block + offset,
				sample->accel_m_s2[axis]);
			offset += sizeof(float);
		}
		for (size_t axis = 0U; axis < 3U; ++axis) {
			edgez_sensor_put_float_le(block + offset,
				sample->gyro_rad_s[axis]);
			offset += sizeof(float);
		}
		block[offset++] = sample->gyro_valid ? 1U : 0U;
	}
	int rc = sensor_pawr_send_block(block, offset,
		EDGEZ_SENSOR_BLOCK_KIND_IMU, wake_interval_ms);
	if (rc == 0) {
		sensor_block_sample_count = 0U;
		++sensor_block_sequence;
	}
	return rc;
}
#endif

#if defined(CONFIG_OFFGRID_NRF54_SENSOR_PAWR_BULK_TEST)

struct sensor_bulk_test_source {
	uint8_t test_index;
	uint32_t wake_interval_ms;
	uint32_t produced;
};

static uint8_t sensor_bulk_test_byte(uint8_t test_index, uint32_t offset)
{
	return (uint8_t)((offset * 29U + (uint32_t)test_index * 71U + 0x5aU) &
			 0xffU);
}

static uint32_t sensor_bulk_test_digest(uint8_t test_index, uint32_t length)
{
	uint32_t digest = UINT32_C(2166136261);

	for (uint32_t offset = 0U; offset < length; ++offset) {
		digest ^= sensor_bulk_test_byte(test_index, offset);
		digest *= UINT32_C(16777619);
	}
	return digest;
}

static bool sensor_bulk_fill_sender(struct edgez_pawr_bulk_sender *sender,
				    struct sensor_bulk_test_source *source)
{
	uint8_t block[256];

	for (;;) {
		size_t length = edgez_pawr_bulk_sender_space(sender);
		if (length == 0U) {
			return true;
		}
		if (length > sizeof(block)) {
			length = sizeof(block);
		}
		for (size_t i = 0U; i < length; ++i) {
			block[i] = sensor_bulk_test_byte(
				source->test_index, source->produced + (uint32_t)i);
		}
		if (edgez_pawr_bulk_sender_write(sender, block, length) != length) {
			return false;
		}
		source->produced += (uint32_t)length;
	}
}

static bool sensor_bulk_send(void *ctx, const uint8_t *payload, size_t length)
{
	const struct sensor_bulk_test_source *source = ctx;
	return sensor_pawr_queue_bulk_chunk(payload, length,
					    source->wake_interval_ms) == 0;
}

static void sensor_bulk_done(void *ctx, bool success)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(success);
}

static bool sensor_bulk_feed_pending_ack(void)
{
	uint8_t payload[EDGEZ_PAWR_BULK_CONTROL_SIZE];
	size_t length;
	k_spinlock_key_t key = k_spin_lock(&sensor_pawr_lock);

	length = sensor_bulk_ack_length;
	if (length != 0U) {
		memcpy(payload, sensor_bulk_ack_payload, length);
		sensor_bulk_ack_length = 0U;
	}
	k_spin_unlock(&sensor_pawr_lock, key);
	return length != 0U &&
		edgez_pawr_bulk_sender_feed(&sensor_bulk_sender, payload, length);
}

static int sensor_pawr_run_bulk_test(uint32_t wake_interval_ms)
{
	static const uint32_t test_lengths[] = {32768U, 65536U};
	struct sensor_bulk_test_source source = {
		.wake_interval_ms = wake_interval_ms,
	};

	int rc = sensor_pawr_enable_bulk_burst_mode();
	if (rc != 0) {
		return rc;
	}
	const struct edgez_pawr_bulk_regulatory regulatory =
		sensor_pawr_bulk_regulatory();
	const struct edgez_pawr_bulk_sender_config config = {
		.tiers = edgez_pawr_bulk_default_tiers,
		.tier_count = ARRAY_SIZE(edgez_pawr_bulk_default_tiers),
		.initial_tier = 2U,
		.maximum_tier = 4U,
		.max_attempts = EDGEZ_SENSOR_BULK_ACK_ATTEMPTS,
		.send = sensor_bulk_send,
		.done = sensor_bulk_done,
		.ctx = &source,
	};

	if (!edgez_pawr_bulk_sender_init(&sensor_bulk_sender, &config)) {
		return -EINVAL;
	}

	for (size_t test = 0U; test < ARRAY_SIZE(test_lengths); ++test) {
		source.test_index = (uint8_t)test;
		source.produced = 0U;
		const struct edgez_pawr_bulk_transfer transfer = {
			.compressed_ssid = mesh_runtime.config.compressed_ssid,
			.session_id = sys_rand32_get(),
			.total_length = test_lengths[test],
			.expected_digest = sensor_bulk_test_digest(
				(uint8_t)test, test_lengths[test]),
			.test_index = (uint8_t)test,
			.regulatory = regulatory,
		};
		const int64_t started_ms = k_uptime_get();
		k_sem_reset(&sensor_bulk_ack_sem);
		sensor_bulk_ack_length = 0U;
		if (!edgez_pawr_bulk_sender_start(&sensor_bulk_sender, &transfer)) {
			return -EINVAL;
		}

		for (;;) {
			if (!sensor_bulk_fill_sender(&sensor_bulk_sender, &source)) {
				LOG_ERR("PAWR_BULK_TX producer write failed test=%u produced=%u",
					(unsigned)test, (unsigned)source.produced);
				return -EIO;
			}
			const uint32_t now_ms = (uint32_t)k_uptime_get();
			enum edgez_pawr_bulk_result result =
				edgez_pawr_bulk_sender_poll(&sensor_bulk_sender, now_ms);
			if (result == EDGEZ_PAWR_BULK_COMPLETE) {
				break;
			}
			if (result == EDGEZ_PAWR_BULK_BACKPRESSURE) {
				k_yield();
				continue;
			}
			if (result < 0) {
				LOG_ERR("PAWR_BULK_TX FAIL test=%u offset=%u result=%d",
					(unsigned)test,
					(unsigned)sensor_bulk_sender.offset, result);
				return -ETIMEDOUT;
			}
			if (sensor_bulk_feed_pending_ack()) {
				continue;
			}
			if (result == EDGEZ_PAWR_BULK_WAITING) {
				uint32_t deadline =
					edgez_pawr_bulk_sender_next_deadline(
						&sensor_bulk_sender);
				uint32_t wait_ms = (int32_t)(deadline - now_ms) > 0 ?
					deadline - now_ms : 1U;
				if (k_sem_take(&sensor_bulk_ack_sem,
					       K_MSEC(wait_ms)) == 0) {
					(void)sensor_bulk_feed_pending_ack();
				}
			}
		}

		const struct edgez_pawr_bulk_sender_stats *stats =
			&sensor_bulk_sender.stats;
		const struct edgez_pawr_bulk_tier final_tier =
			sensor_bulk_sender.tiers[sensor_bulk_sender.tier];
		LOG_INF("PAWR_BULK_TX PASS test=%u bytes=%u chunks=%u tx=%u selective_retx=%u ack_queries=%u block_acks=%u elapsed_ms=%u final_chunk=%u final_window=%u final_ack_timeout_ms=%u digest=%08x",
			(unsigned)test, (unsigned)test_lengths[test],
			(unsigned)stats->chunks_acked,
			(unsigned)stats->transmissions,
			(unsigned)stats->selective_retries,
			(unsigned)stats->ack_queries,
			(unsigned)stats->block_acks,
			(unsigned)(k_uptime_get() - started_ms),
			(unsigned)final_tier.chunk_size,
			(unsigned)final_tier.window_size,
			(unsigned)edgez_pawr_bulk_ack_timeout_ms(
				final_tier.chunk_size, final_tier.window_size),
			transfer.expected_digest);
	}
	return 0;
}
#endif
#endif /* CONFIG_OFFGRID_NRF54_ROLE_SENSOR */

#if defined(CONFIG_OFFGRID_NRF54_MESH_PS_TEST)
static bool set_mesh_power_save(bool enabled)
{
	enum mmwlan_status status;

	if (enabled) {
		status = mmdrv_set_wake_enabled(false);
		if (status == MMWLAN_SUCCESS) {
			status = mmdrv_set_chip_power_save_enabled(
				mesh_runtime.umac.vif_id, true);
		}
		if (status != MMWLAN_SUCCESS) {
			(void)mmdrv_set_wake_enabled(true);
		}
	} else {
		status = mmdrv_set_wake_enabled(true);
		if (status == MMWLAN_SUCCESS) {
			status = mmdrv_set_chip_power_save_enabled(
				mesh_runtime.umac.vif_id, false);
		}
	}

	if (status != MMWLAN_SUCCESS) {
		LOG_ERR("could not %s mesh power save: status=%d vif=%u",
			enabled ? "enable" : "disable", status,
			mesh_runtime.umac.vif_id);
		return false;
	}
	return true;
}
#endif

#if defined(CONFIG_OFFGRID_NRF54_SENSOR_HALOW_POWER_SAVE)
static bool sensor_configure_halow_power_save(void)
{
	/* CONFIG_PS is required once to let the MM8108 firmware deassert BUSY. It
	 * is normally managed by the associated-STA UMAC flow, which this PAwR-only
	 * mesh VIF does not run. Configure it explicitly once after radio startup,
	 * then use only the cheap host-wake toggle for subsequent report cycles. */
	enum mmwlan_status status = mmdrv_set_dynamic_ps_timeout(0U);

	if (status == MMWLAN_SUCCESS) {
		status = mmdrv_set_chip_power_save_enabled(
			mesh_runtime.umac.vif_id, true);
	}
	if (status != MMWLAN_SUCCESS) {
		LOG_ERR("could not configure sensor HaLow chip sleep: status=%d vif=%u",
			status, mesh_runtime.umac.vif_id);
		return false;
	}
	sensor_halow_ps_configured = true;
	return true;
}

static bool sensor_set_halow_wake(bool awake)
{
	enum mmwlan_status status = mmdrv_set_wake_enabled(awake);

	if (status != MMWLAN_SUCCESS) {
		LOG_ERR("could not %s sensor HaLow: status=%d",
			awake ? "wake" : "sleep", status);
		return false;
	}
	sensor_halow_sleep_allowed = !awake;
	return true;
}
#endif

#if defined(CONFIG_OFFGRID_NRF54_MESH_PS_TEST)
static uint32_t busy_percent_x10(void)
{
	if (mesh_ps_total_samples == 0U) {
		return 0U;
	}
	return (uint32_t)(((uint64_t)mesh_ps_busy_samples * 1000U) /
			  mesh_ps_total_samples);
}

static void reset_samples(uint32_t now)
{
	mesh_ps_phase_started = now;
	mesh_ps_busy_samples = 0U;
	mesh_ps_total_samples = 0U;
}

static void sample_busy(void)
{
	if (mmhal_wlan_busy_is_asserted()) {
		mesh_ps_busy_samples++;
	}
	mesh_ps_total_samples++;
}

static void mesh_power_save_tick(uint32_t now)
{
	const bool connected = has_established_peer();

	if (!connected) {
		if (mesh_ps_phase == MESH_PS_ENABLED) {
			LOG_WRN("peer lost during power save; disabling PS");
			(void)set_mesh_power_save(false);
		} else if (mesh_ps_phase != MESH_PS_WAITING) {
			LOG_WRN("peer lost before power-save measurement completed");
		}
		mesh_ps_phase = MESH_PS_WAITING;
		return;
	}

	switch (mesh_ps_phase) {
	case MESH_PS_WAITING:
		LOG_INF("peer established; starting nRF54/MM6108 power-save A/B cycle");
		if (set_mesh_power_save(false)) {
			mesh_ps_phase = MESH_PS_SETTLING;
			mesh_ps_phase_started = now;
		}
		break;
	case MESH_PS_SETTLING:
		if ((uint32_t)(now - mesh_ps_phase_started) >= EDGEZ_PS_SETTLE_MS) {
			LOG_INF("BASELINE START: PS disabled for 30 seconds");
			reset_samples(now);
			mesh_ps_phase = MESH_PS_BASELINE;
		}
		break;
	case MESH_PS_BASELINE:
		sample_busy();
		if ((uint32_t)(now - mesh_ps_phase_started) >= EDGEZ_PS_BASELINE_MS) {
			mesh_ps_baseline_x10 = busy_percent_x10();
			LOG_INF("BASELINE END: MM_BUSY %u/%u samples (%u.%u%%), link=up",
				mesh_ps_busy_samples, mesh_ps_total_samples,
				mesh_ps_baseline_x10 / 10U,
				mesh_ps_baseline_x10 % 10U);
			enum mmwlan_status status = mmdrv_set_dynamic_ps_timeout(
				EDGEZ_PS_DYNAMIC_TIMEOUT_MS);
			if (status != MMWLAN_SUCCESS || !set_mesh_power_save(true)) {
				LOG_ERR("power-save transition failed: timeout_status=%d",
					status);
				mesh_ps_phase = MESH_PS_WAITING;
				break;
			}
			LOG_INF("POWER SAVE ENABLED: timeout=100 ms; reporting every 30 seconds");
			reset_samples(now);
			mesh_ps_phase = MESH_PS_ENABLED;
		}
		break;
	case MESH_PS_ENABLED:
		sample_busy();
		if ((uint32_t)(now - mesh_ps_phase_started) >= EDGEZ_PS_REPORT_MS) {
			const uint32_t power_save_x10 = busy_percent_x10();
			const int32_t change_x10 =
				(int32_t)power_save_x10 - (int32_t)mesh_ps_baseline_x10;
			const uint32_t magnitude_x10 =
				(uint32_t)(change_x10 < 0 ? -change_x10 : change_x10);
			LOG_INF("POWER SAVE: MM_BUSY %u/%u samples (%u.%u%%), link=up",
				mesh_ps_busy_samples, mesh_ps_total_samples,
				power_save_x10 / 10U, power_save_x10 % 10U);
			LOG_INF("BUSY duty change from baseline: %c%u.%u percentage points",
				change_x10 < 0 ? '-' : '+', magnitude_x10 / 10U,
				magnitude_x10 % 10U);
			reset_samples(now);
		}
		break;
	}
}
#endif

static void report_peers(void)
{
	size_t active = 0U;
	size_t established = 0U;
	size_t authorized = 0U;

	for (size_t i = 0; i < EDGEZ_UMAC_MAX_PEERS; ++i) {
		const struct edgez_mesh_peering_slot *slot =
			&mesh_runtime.peering_manager.slots[i];
		if (!slot->in_use) {
			continue;
		}
		active++;
		if (slot->peering.state == EDGEZ_PEERING_ESTABLISHED) {
			established++;
		}
		LOG_INF("peer %02x:%02x:%02x:%02x:%02x:%02x state=%s retries=%u",
			slot->peering.peer_addr[0], slot->peering.peer_addr[1],
			slot->peering.peer_addr[2], slot->peering.peer_addr[3],
			slot->peering.peer_addr[4], slot->peering.peer_addr[5],
			peering_state_name(slot->peering.state), slot->peering.retries);
	}
	for (size_t i = 0; i < EDGEZ_UMAC_MAX_PEERS; ++i) {
		const struct edgez_umac_peer *peer = &mesh_runtime.umac.peers[i];
		if (!peer->in_use) {
			continue;
		}
		if (peer->state == EDGEZ_UMAC_PEER_AUTHORIZED) {
			authorized++;
		}
		LOG_INF("radio peer %02x:%02x:%02x:%02x:%02x:%02x aid=%u state=%u",
			peer->mac_addr[0], peer->mac_addr[1], peer->mac_addr[2],
			peer->mac_addr[3], peer->mac_addr[4], peer->mac_addr[5],
			peer->aid, (unsigned)peer->state);
	}

	LOG_INF("join-only peers=%u established=%u authorized=%u rx=%" PRIu32
		" beacon=%" PRIu32 " auth=%" PRIu32 " mpm=%" PRIu32
		" malformed=%" PRIu32 " handler_fail=%" PRIu32
		" rejected=%" PRIu32 " state_err=%" PRIu32
		" transitions=%" PRIu32 " op_fail=%" PRIu32
		" tx=%" PRIu32 " tx_fail=%" PRIu32
		" bat_tx=%" PRIu32 " bat_fail=%" PRIu32 " driver=%d",
		(unsigned)active, (unsigned)established, (unsigned)authorized,
		mesh_runtime.rx_pipeline.metrics.received,
		mesh_runtime.rx_pipeline.metrics.beacons,
		mesh_runtime.rx_pipeline.metrics.authentications,
		mesh_runtime.rx_pipeline.metrics.peering_actions,
		mesh_runtime.rx_pipeline.metrics.malformed,
		mesh_runtime.rx_pipeline.metrics.handler_failures,
		mesh_runtime.controller.metrics.rejected_security_frames,
		mesh_runtime.controller.metrics.state_errors,
		mesh_runtime.peering_manager.transitions,
		mesh_runtime.peering_manager.operation_failures,
		tx_status_count, tx_failure_count,
		batman_tx_count, batman_tx_failure_count,
		mesh_runtime.umac.last_driver_status);

	if (mesh_runtime.peering_manager.operation_failures !=
	    previous_operation_failures) {
		LOG_WRN("peering operation failure delta=%" PRIu32
			" total=%" PRIu32 " last_driver=%d",
			mesh_runtime.peering_manager.operation_failures -
				previous_operation_failures,
			mesh_runtime.peering_manager.operation_failures,
			mesh_runtime.umac.last_driver_status);
		previous_operation_failures =
			mesh_runtime.peering_manager.operation_failures;
	}
	if (mesh_runtime.rx_pipeline.metrics.handler_failures !=
	    previous_handler_failures) {
		LOG_WRN("RX handler failure delta=%" PRIu32 " total=%" PRIu32,
			mesh_runtime.rx_pipeline.metrics.handler_failures -
				previous_handler_failures,
			mesh_runtime.rx_pipeline.metrics.handler_failures);
		previous_handler_failures =
			mesh_runtime.rx_pipeline.metrics.handler_failures;
	}
}

static int stop_mesh_profile(bool clear_routes)
{
	int status;

	if (!mesh_started) {
		return EDGEZ_UMAC_OK;
	}
	if (clear_routes && mesh_services_initialized) {
		(void)edgez_nrf_mesh_reset_and_pause();
		edgez_nrf_relay_reset();
	}
#if defined(CONFIG_OFFGRID_NRF54_MESH_PS_TEST)
	(void)set_mesh_power_save(false);
	mesh_ps_phase = MESH_PS_WAITING;
#endif
#if defined(CONFIG_OFFGRID_NRF54_SENSOR_HALOW_POWER_SAVE)
	if (sensor_halow_ps_configured) {
		/* Wake the chip and disable the one-time sleep configuration before
		 * tearing down the connectionless mesh interface. This is not part of
		 * the recurring sensor report path. */
		(void)sensor_set_halow_wake(true);
		(void)mmdrv_set_chip_power_save_enabled(
			mesh_runtime.umac.vif_id, false);
		sensor_halow_ps_configured = false;
		sensor_halow_sleep_allowed = false;
	}
#endif
	status = edgez_umac_mesh_runtime_stop(&mesh_runtime);
	if (status != EDGEZ_UMAC_OK) {
		/* runtime_stop leaves the UMAC in STOPPED state even when a driver
		 * cleanup operation reports an error. Finish local cleanup so a sleepy
		 * device can cut the rail and start from a clean runtime next cycle. */
		LOG_WRN("mesh runtime stop completed with driver error: %d", status);
	}
	edgez_rf_unbind_umac();
	if (mesh_secure) {
		edgez_mesh_sae_provider_deinit(&sae_provider);
	}
	memset(&mesh_runtime, 0, sizeof(mesh_runtime));
	memset(&sae_provider, 0, sizeof(sae_provider));
	open_data_sequence = 0;
	open_mesh_sequence = 0;
	open_tx_token = 0;
	mesh_secure = false;
	sensor_radio_only = false;
	mesh_started = false;
	edgez_config_set_halow_ready(false);
	edgez_config_set_halow_link(false);
	return EDGEZ_UMAC_OK;
}

static int start_mesh_profile(const struct edgez_halow_profile *profile,
			      bool reconfigure)
{
	size_t mesh_id_length = strlen(profile->mesh_id);
	size_t passphrase_length = strlen(profile->passphrase);
	size_t old_peer_count = 0;
	bool secure = passphrase_length > 0;
	const uint32_t control_interval_ms =
		edgez_mesh_country_control_interval_ms(profile->country_code, 1000U);
	int status;

	if (mesh_id_length == 0 || mesh_id_length > 32 ||
	    strlen(profile->country_code) != 2 || profile->mesh_frequency_khz == 0 ||
	    profile->mesh_bandwidth_mhz == 0) return EDGEZ_UMAC_INVALID_ARGUMENT;
	if (reconfigure && mesh_started) {
		old_peer_count = edgez_umac_peer_count(&mesh_runtime.umac);
		status = stop_mesh_profile(true);
		if (status != EDGEZ_UMAC_OK) {
			return status;
		}
		LOG_INF("Profile switch disconnected %u peer(s) and cleared BATMAN routes",
			(unsigned int)old_peer_count);
	}

	struct edgez_umac_mesh_runtime_config config = {
		.radio = edgez_rf_driver_ops(),
		.channel = {
			.operating_frequency_hz = profile->mesh_frequency_khz * 1000U,
			.operating_bw_mhz = profile->mesh_bandwidth_mhz,
			.primary_bw_mhz = EDGEZ_MESH_PRIMARY_BANDWIDTH_MHZ,
			.primary_1mhz_channel_index = 0U,
		},
		.mesh_id_length = mesh_id_length,
		.beacon_interval_tu = edgez_mesh_country_beacon_interval_tu(
			profile->country_code, EDGEZ_MESH_BEACON_INTERVAL_TU),
		.compressed_ssid = edgez_pawr_compress_ssid((const uint8_t *)profile->mesh_id,
						  mesh_id_length),
		.mcs = 0U,
		.attempts = 2U,
		.peer_liveness_timeout_ms =
			edgez_mesh_control_liveness_timeout_ms(
				control_interval_ms,
				EDGEZ_UMAC_MESH_PEER_LIVENESS_TIMEOUT_MS),
		.rate_control = {
			.enabled = true,
			.initial_mcs = 0U,
			.maximum_mcs = 7U,
			.successes_to_increase = 4U,
		},
		.secure = secure,
		.events = join_events,
	};
	memcpy(config.country_code, profile->country_code, 2);
	config.country_code[2] = '\0';
	memcpy(config.mesh_id, profile->mesh_id, mesh_id_length);

	const struct edgez_mesh_sae_provider_config security_config = {
		.umac = &mesh_runtime.umac,
		.password = (const uint8_t *)profile->passphrase,
		.password_length = passphrase_length,
		.mesh_id = (const uint8_t *)profile->mesh_id,
		.mesh_id_length = mesh_id_length,
		.mcs = config.mcs,
		.attempts = config.attempts,
		.bandwidth_mhz = config.channel.operating_bw_mhz,
	};

	status = EDGEZ_UMAC_OK;
	if (secure) {
		status = edgez_mesh_sae_provider_init(&sae_provider, &security_config);
		if (status != EDGEZ_UMAC_OK) LOG_ERR("SAE provider init failed: %d", status);
	}
	if (status == EDGEZ_UMAC_OK && secure) {
		config.security = edgez_mesh_sae_provider_ops();
		config.security_ctx = &sae_provider;
	}
	if (status == EDGEZ_UMAC_OK) status = edgez_umac_mesh_runtime_init(&mesh_runtime, &config);
	if (status == EDGEZ_UMAC_OK) {
		status = edgez_rf_bind_umac(&mesh_runtime.umac);
		if (status != EDGEZ_UMAC_OK) {
			LOG_ERR("Morse RF callback bind failed: %d", status);
		}
	}
	if (status == EDGEZ_UMAC_OK) {
		status = edgez_umac_mesh_runtime_start(&mesh_runtime);
		if (status != EDGEZ_UMAC_OK) {
			LOG_ERR("mesh runtime start failed: %d driver=%d",
				status, mesh_runtime.umac.last_driver_status);
		}
	}
	if (status != EDGEZ_UMAC_OK) {
		LOG_ERR("join-only mesh start failed: edgez=%d driver=%d",
			status, mesh_runtime.umac.last_driver_status);
		if (mesh_runtime.running) {
			(void)edgez_umac_mesh_runtime_stop(&mesh_runtime);
		}
		edgez_rf_unbind_umac();
		if (secure) {
			edgez_mesh_sae_provider_deinit(&sae_provider);
		}
		memset(&mesh_runtime, 0, sizeof(mesh_runtime));
		memset(&sae_provider, 0, sizeof(sae_provider));
		return status;
	}

	if (secure) {
		edgez_mesh_sae_provider_set_local_address(
			&sae_provider, mesh_runtime.radio_info.mac_addr);
	}
	mesh_secure = secure;
	sensor_radio_only = false;
	mesh_started = true;
	applied_profile = *profile;
	if (!mesh_services_initialized) {
		status = edgez_nrf_mesh_init(mesh_runtime.radio_info.mac_addr,
					     &batman_transport,
					     control_interval_ms);
		if (status == 0) status = edgez_nrf_relay_init(mesh_runtime.radio_info.mac_addr);
		if (status != 0) {
			(void)stop_mesh_profile(false);
			return status;
		}
		mesh_services_initialized = true;
	} else {
		edgez_nrf_mesh_resume();
	}
	applied_generation = edgez_config_generation();
	edgez_config_set_halow_mac(mesh_runtime.radio_info.mac_addr);
	edgez_config_set_halow_ready(true);
	edgez_config_set_halow_link(has_established_peer());
	LOG_INF("mesh active id=%s security=%s country=%s frequency=%u kHz bandwidth=%u MHz generation=%u",
		profile->mesh_id, secure ? "SAE" : "open", profile->country_code,
		profile->mesh_frequency_khz, profile->mesh_bandwidth_mhz,
		applied_generation);
	LOG_INF("mesh control beacon=%u TU BATMAN=%u ms liveness=%u ms",
		config.beacon_interval_tu, control_interval_ms,
		config.peer_liveness_timeout_ms);
	LOG_INF("radio backend=%s chip=0x%08" PRIx32
		" vif=%u mac=%02x:%02x:%02x:%02x:%02x:%02x",
		mesh_runtime.umac.driver->name, mesh_runtime.radio_info.chip_id,
		mesh_runtime.umac.vif_id,
		mesh_runtime.radio_info.mac_addr[0], mesh_runtime.radio_info.mac_addr[1],
		mesh_runtime.radio_info.mac_addr[2], mesh_runtime.radio_info.mac_addr[3],
		mesh_runtime.radio_info.mac_addr[4], mesh_runtime.radio_info.mac_addr[5]);
	return EDGEZ_UMAC_OK;
}

static int start_sensor_radio(const struct edgez_halow_profile *profile)
{
	size_t mesh_id_length = strlen(profile->mesh_id);
	const uint32_t control_interval_ms =
		edgez_mesh_country_control_interval_ms(profile->country_code, 1000U);
	int status;

	if (mesh_id_length == 0U || mesh_id_length > 32U ||
	    strlen(profile->country_code) != 2U ||
	    profile->mesh_frequency_khz == 0U ||
	    profile->mesh_bandwidth_mhz == 0U) {
		return EDGEZ_UMAC_INVALID_ARGUMENT;
	}

	struct edgez_umac_mesh_runtime_config config = {
		.radio = edgez_rf_driver_ops(),
		.channel = {
			.operating_frequency_hz = profile->mesh_frequency_khz * 1000U,
			.operating_bw_mhz = profile->mesh_bandwidth_mhz,
			.primary_bw_mhz = EDGEZ_MESH_PRIMARY_BANDWIDTH_MHZ,
			.primary_1mhz_channel_index = 0U,
		},
		.mesh_id_length = mesh_id_length,
		.beacon_interval_tu = edgez_mesh_country_beacon_interval_tu(
			profile->country_code, EDGEZ_MESH_BEACON_INTERVAL_TU),
		.compressed_ssid = edgez_pawr_compress_ssid((const uint8_t *)profile->mesh_id,
						  mesh_id_length),
		.mcs = 0U,
		.attempts = 2U,
		.rate_control = {
			.enabled = true,
			.initial_mcs = 0U,
			.maximum_mcs = 2U,
			.successes_to_increase = 4U,
		},
		.secure = false,
		.events = join_events,
	};
	memcpy(config.country_code, profile->country_code, 2U);
	config.country_code[2] = '\0';
	memcpy(config.mesh_id, profile->mesh_id, mesh_id_length);

	status = edgez_umac_mesh_runtime_init(&mesh_runtime, &config);
	if (status == EDGEZ_UMAC_OK) {
		status = edgez_rf_bind_umac(&mesh_runtime.umac);
	}
	if (status == EDGEZ_UMAC_OK) {
		const struct edgez_umac_config radio_config = {
			.country_code = {
				profile->country_code[0], profile->country_code[1], '\0'
			},
			.channel = config.channel,
			.beacon_interval_tu = config.beacon_interval_tu,
			.dtim_period = 1U,
			.compressed_ssid = config.compressed_ssid,
			.rate_control = config.rate_control,
			.enable_beaconing = false,
		};
		status = edgez_umac_start(&mesh_runtime.umac, &radio_config,
					 &mesh_runtime.radio_info);
	}
	if (status != EDGEZ_UMAC_OK) {
		LOG_ERR("PAwR-only radio start failed edgez=%d driver=%d", status,
			mesh_runtime.umac.last_driver_status);
		edgez_rf_unbind_umac();
		memset(&mesh_runtime, 0, sizeof(mesh_runtime));
		return status;
	}

	/* runtime_init installed the receive dispatcher. Mark it running so the
	 * vendor-action callback is accepted, but deliberately skip the mesh
	 * runtime start that enables beacons and peer discovery. */
	mesh_runtime.running = true;
	mesh_started = true;
	mesh_secure = false;
	sensor_radio_only = true;
	applied_profile = *profile;
	if (!mesh_services_initialized) {
		status = edgez_nrf_mesh_init(mesh_runtime.radio_info.mac_addr,
					     &batman_transport,
					     control_interval_ms);
		if (status == 0) {
			status = edgez_nrf_relay_init(mesh_runtime.radio_info.mac_addr);
		}
		if (status != 0) {
			(void)stop_mesh_profile(false);
			return status;
		}
		mesh_services_initialized = true;
	} else {
		edgez_nrf_mesh_resume();
	}
	applied_generation = edgez_config_generation();
	edgez_config_set_halow_mac(mesh_runtime.radio_info.mac_addr);
	edgez_config_set_halow_ready(true);
	edgez_config_set_halow_link(false);
	LOG_INF("PAwR-only radio active beaconing=0 peering=0 country=%s frequency=%u kHz bandwidth=%u MHz",
		profile->country_code, profile->mesh_frequency_khz,
		profile->mesh_bandwidth_mhz);
	return EDGEZ_UMAC_OK;
}

enum sensor_power_phase {
	SENSOR_POWER_INACTIVE,
	SENSOR_POWER_READY,
	SENSOR_POWER_WAITING_GPS,
	SENSOR_POWER_TRANSMIT,
	SENSOR_POWER_SLEEPING,
};

static struct {
	enum sensor_power_phase phase;
	uint32_t generation;
	uint32_t cycle_started_ms;
	uint32_t next_wake_ms;
	uint32_t gps_deadline_ms;
} sensor_power;

static void sensor_begin_cycle(uint32_t now)
{
	sensor_power.cycle_started_ms = now;
	sensor_power.phase = SENSOR_POWER_READY;
}

#if defined(CONFIG_OFFGRID_NRF54_IMU_TIMER_DRAIN)
#define EDGEZ_UNUSED_RAM_BANK_START ((uintptr_t)0x20038000U)
#define EDGEZ_UNUSED_RAM_BANK_SIZE  ((size_t)0x00008000U)
extern char _end[];

static void sensor_power_down_unused_ram(void)
{
	/* nRF54L15 application RAM is split into eight independently powered 32 KiB
	 * sections. The linker image currently ends below 0x20038000, so section 7
	 * can stay completely off throughout sensor operation. Keep the runtime
	 * guard so a future image growth cannot silently power down live storage. */
	if ((uintptr_t)_end <= EDGEZ_UNUSED_RAM_BANK_START) {
		nrfx_ram_ctrl_retention_enable_set(
			(const void *)EDGEZ_UNUSED_RAM_BANK_START,
			EDGEZ_UNUSED_RAM_BANK_SIZE, false);
		nrfx_ram_ctrl_power_enable_set(
			(const void *)EDGEZ_UNUSED_RAM_BANK_START,
			EDGEZ_UNUSED_RAM_BANK_SIZE, false);
	}
}
#else
static inline void sensor_power_down_unused_ram(void)
{
}
#endif

static void sensor_enter(const struct edgez_halow_profile *profile, uint32_t now)
{
	sensor_power_down_unused_ram();
	if (!edgez_ble_is_stopped()) {
		edgez_ble_stop();
	}
	/* Begin with GPS and its UART off. The power build wakes them only inside
	 * the bounded reporting window and stops them again before tickless sleep. */
	edgez_gps_stop();
	if (mesh_started) {
		int rc = stop_mesh_profile(true);

		if (rc != EDGEZ_UMAC_OK) {
			LOG_WRN("sensor mode could not stop the active mesh rc=%d", rc);
		}
	}
	/* The power-save sensor build reuses the rail enabled by the board's early
	 * Morse initialization. Cycling it off and immediately back on here adds
	 * 600 ms before the first packet and provides no energy benefit. */
#if !defined(CONFIG_OFFGRID_NRF54_SENSOR_HALOW_POWER_SAVE)
	if (!mesh_started && edgez_halow_power_is_ready()) {
		int rc = edgez_halow_power_set(false);

		if (rc != 0 && rc != -ENOTSUP) {
			LOG_WRN("initial sensor HaLow power-off failed rc=%d", rc);
		}
	}
#endif
	sensor_power.generation = edgez_config_generation();
	sensor_begin_cycle(now);
	LOG_INF("low-power sensor mode armed device_type=%u update_interval=%us GPS=%s IMU=hardware-FIFO",
		profile->device_type, sensor_update_interval_ms() / 1000U,
		IS_ENABLED(CONFIG_OFFGRID_NRF54_SENSOR_GPS) ? "cycled" : "disabled");
}

static void sensor_leave(void)
{
	edgez_gps_stop();
	sensor_power.phase = SENSOR_POWER_INACTIVE;
	sensor_power.generation = 0U;
#if defined(CONFIG_OFFGRID_NRF54_SENSOR_HALOW_POWER_SAVE)
	if (mesh_started) {
		(void)stop_mesh_profile(true);
	}
#endif
	if (!edgez_halow_power_is_ready()) {
		int rc = edgez_halow_power_set(true);

		if (rc != 0) {
			LOG_ERR("could not restore HaLow power after sensor mode rc=%d", rc);
		}
	}
	LOG_INF("low-power sensor mode disabled; restoring persistent mesh mode");
}

static void sensor_finish_cycle(uint32_t now)
{
	uint32_t elapsed_ms = now - sensor_power.cycle_started_ms;
	uint32_t interval_ms = sensor_update_interval_ms();

	edgez_gps_stop();
	/* A zero-touch wake may spend several seconds sweeping all regulatory
	 * 1 MHz channels. Its configured interval is sleep time after that sweep,
	 * not a period measured from the beginning of the radio activity. */
	if (!edgez_config_should_autostart()) {
		sensor_power.next_wake_ms = now + interval_ms;
	} else {
		sensor_power.next_wake_ms = elapsed_ms < interval_ms ?
			sensor_power.cycle_started_ms + interval_ms : now + 1000U;
	}
	sensor_power.phase = SENSOR_POWER_SLEEPING;
	LOG_INF("sensor cycle complete; HaLow %s for %u ms",
#if defined(CONFIG_OFFGRID_NRF54_SENSOR_HALOW_POWER_SAVE)
		sensor_halow_sleep_allowed ? "chip sleep" : "off",
#else
		"off",
#endif
		(unsigned)(sensor_power.next_wake_ms - now));
}

static inline void sensor_force_cpu_low_power(void)
{
#if defined(CONFIG_SOC_COMPATIBLE_NRF54LX) && !defined(CONFIG_NRF_SYS_EVENT)
	/* The Nordic Bluetooth controller selects constant-latency mode while its
	 * radio timer is active. Sensor sleep is entered only after bt_disable(),
	 * HaLow shutdown, SPI suspend and radio-pin isolation, so no low-latency
	 * client remains. Explicitly restore variable-latency low-power mode in
	 * case a previous BLE session did not complete that transition. */
	NRF_POWER->TASKS_LOWPWR = 1U;
	__DSB();
	__ISB();
#endif
}

static void sensor_power_tick(const struct edgez_halow_profile *profile,
			      uint32_t now)
{
	if (sensor_power.phase == SENSOR_POWER_INACTIVE) {
		sensor_enter(profile, now);
	}
	if (sensor_power.generation != edgez_config_generation()) {
		edgez_gps_stop();
		if (mesh_started) {
			(void)stop_mesh_profile(true);
		}
		if (edgez_halow_power_is_ready()) {
			(void)edgez_halow_power_set(false);
		}
		sensor_power.generation = edgez_config_generation();
		sensor_begin_cycle(k_uptime_get_32());
		LOG_INF("sensor profile changed; restarting wake cycle generation=%u",
			sensor_power.generation);
		return;
	}
	if (!edgez_config_should_autostart() &&
	    !edgez_factory_data_is_ready()) {
		/* An unprovisioned sensor without its factory X25519 identity cannot
		 * create an actionable request. Keep MM8108 off and sleep. */
		edgez_gps_stop();
		if (mesh_started) {
			(void)stop_mesh_profile(true);
		}
		if (edgez_halow_power_is_ready()) {
			(void)edgez_halow_power_set(false);
		}
		sensor_finish_cycle(now);
		return;
	}

	if (sensor_power.phase == SENSOR_POWER_SLEEPING) {
		if ((int32_t)(now - sensor_power.next_wake_ms) >= 0) {
			sensor_begin_cycle(now);
		}
		return;
	}
	if (sensor_power.phase == SENSOR_POWER_READY) {
#if defined(CONFIG_OFFGRID_NRF54_SENSOR_GPS)
		int gps_rc = profile->device_gps_enabled ? edgez_gps_init() : -ENOTSUP;

		if (gps_rc == 0) {
			sensor_power.gps_deadline_ms = now + EDGEZ_SENSOR_GPS_TIMEOUT_MS;
			sensor_power.phase = SENSOR_POWER_WAITING_GPS;
			LOG_INF("sensor GPS awake; waiting up to %u ms for fix",
				EDGEZ_SENSOR_GPS_TIMEOUT_MS);
			return;
		}
		if (profile->device_gps_enabled) {
			LOG_WRN("sensor GPS wake failed rc=%d; sending without location", gps_rc);
		}
#endif
		sensor_power.phase = SENSOR_POWER_TRANSMIT;
	}
	if (sensor_power.phase == SENSOR_POWER_WAITING_GPS) {
		struct edgez_gps_fix fix;

		edgez_gps_poll();
		if (edgez_gps_get_fix(&fix)) {
			LOG_INF("sensor GPS fix ready lat=%.6f lon=%.6f satellites=%u",
				(double)fix.latitude, (double)fix.longitude, fix.satellites);
			sensor_power.phase = SENSOR_POWER_TRANSMIT;
		} else if ((int32_t)(now - sensor_power.gps_deadline_ms) >= 0) {
			LOG_WRN("sensor GPS fix timeout after %u ms; sending without location",
				EDGEZ_SENSOR_GPS_TIMEOUT_MS);
			sensor_power.phase = SENSOR_POWER_TRANSMIT;
		} else {
			return;
		}
	}
	if (sensor_power.phase != SENSOR_POWER_TRANSMIT) {
		return;
	}
	const bool provisioning_cycle = !edgez_config_should_autostart();

	/* The reporting timer is the only periodic MCU wake source in the power
	 * build. Collect the samples accumulated by the IMU hardware FIFO before
	 * powering HaLow, so IMU traffic cannot overlap radio startup or TX. */
	int imu_rc;
#if !defined(CONFIG_OFFGRID_NRF54_SENSOR_PAWR_BULK_TEST)
	size_t captured_samples = 0U;
	const uint16_t requested_samples = provisioning_cycle ? 0U :
		sensor_settings.value.block_sampling_count;
	if (requested_samples == 0U) {
		sensor_block_sample_count = 0U;
		imu_rc = edgez_imu_drain_fifo();
	} else {
		const size_t free_samples = sensor_block_sample_count < requested_samples ?
			requested_samples - sensor_block_sample_count : 0U;
		imu_rc = edgez_imu_drain_fifo_samples(
			free_samples > 0U ?
				&sensor_block_samples[sensor_block_sample_count] : NULL,
			free_samples, &captured_samples);
		sensor_block_sample_count += (uint8_t)captured_samples;
		LOG_DBG("cached IMU block samples=%u requested=%u before HaLow start",
			sensor_block_sample_count, requested_samples);
	}
#else
	imu_rc = edgez_imu_drain_fifo();
#endif
	if (imu_rc != 0 && imu_rc != -ENOTSUP && imu_rc != -EAGAIN) {
		LOG_WRN("sensor timer-wake IMU FIFO drain failed rc=%d", imu_rc);
	}

	int rc = 0;
	bool radio_cold_started = false;
#if defined(CONFIG_OFFGRID_NRF54_SENSOR_HALOW_POWER_SAVE)
	if (mesh_started && sensor_halow_ps_configured &&
	    sensor_halow_sleep_allowed) {
		if (!sensor_set_halow_wake(true)) {
			LOG_WRN("sensor HaLow power-save wake failed; cold restarting");
			(void)stop_mesh_profile(true);
			if (edgez_halow_power_is_ready()) {
				(void)edgez_halow_power_set(false);
			}
		}
	}
#endif
	if (!mesh_started && !edgez_halow_power_is_ready()) {
		rc = edgez_halow_power_set(true);
	}
	if (rc == 0 && !mesh_started) {
		struct edgez_halow_profile start_profile = *profile;
		const uint8_t first_start_channel = sensor_bootstrap_channel;

#if defined(CONFIG_OFFGRID_NRF54_SENSOR_PAWR_BULK_TEST)
		rc = sensor_pawr_bulk_profile(&start_profile);
#endif
		do {
			if (rc == 0) {
				rc = start_sensor_radio(&start_profile);
			}
			if (rc == EDGEZ_UMAC_OK) {
				radio_cold_started = true;
				break;
			}
			if (!provisioning_cycle ||
			    !sensor_advance_bootstrap_channel() ||
			    sensor_bootstrap_channel == first_start_channel ||
			    sensor_bootstrap_profile(&start_profile) != 0) {
				break;
			}
			LOG_INF("zero-touch cold-start retry on channel=%u",
				sensor_bootstrap_channel);
		} while (!mesh_started);
	}
	if (rc != EDGEZ_UMAC_OK) {
		LOG_ERR("sensor HaLow wake failed rc=%d; retrying next cycle", rc);
	} else {
		/* Radio startup is synchronous. A short settle gives the connectionless
		 * vendor-action receive callback time to arm; no mesh tick, beacon, scan,
		 * or peering operation is run in sensor mode. */
		if (radio_cold_started) {
			k_msleep(100);
		}
		const uint8_t sweep_first_channel = sensor_bootstrap_channel;
		do {
#if defined(CONFIG_OFFGRID_NRF54_SENSOR_PAWR_BULK_TEST)
			const bool run_bulk_test = !provisioning_cycle;

			if (run_bulk_test) {
				rc = sensor_pawr_run_bulk_test(
					sensor_update_interval_ms());
			} else {
				rc = sensor_pawr_send_once(sensor_update_interval_ms());
			}
#else
			rc = sensor_pawr_send_once(sensor_update_interval_ms());
#endif
#if !defined(CONFIG_OFFGRID_NRF54_SENSOR_PAWR_BULK_TEST)
			if (!provisioning_cycle && rc == 0) {
				if (sensor_settings.value.block_sampling_count > 0U &&
				    sensor_block_sample_count > 0U) {
					int block_rc = sensor_send_imu_block(
						sensor_update_interval_ms());
					if (block_rc != 0) {
						LOG_WRN("sensor block retained for retry rc=%d",
							block_rc);
					}
				}
			}
#endif
			/* A provisioning acceptance is returned as a direct PAwR mesh
			 * packet. Drain it before changing channel or sleeping. */
			edgez_nrf_mesh_tick();
			edgez_nrf_relay_tick();
			LOG_INF("sensor PAwR transmission %s channel=%u",
				rc == 0 ? "acknowledged" : "unacknowledged",
				sensor_bootstrap_channel);

			if (!provisioning_cycle || edgez_config_should_autostart() ||
			    !sensor_advance_bootstrap_channel() ||
			    sensor_bootstrap_channel == sweep_first_channel) {
				break;
			}
			/* Visit every regulatory 1 MHz channel while the MM8108 stays
			 * booted. Skip a channel that the backend cannot apply. */
			while (sensor_retune_bootstrap_channel() != 0) {
				if (!sensor_advance_bootstrap_channel() ||
				    sensor_bootstrap_channel == sweep_first_channel) {
					break;
				}
			}
		} while (sensor_bootstrap_channel != sweep_first_channel);
#if defined(CONFIG_OFFGRID_NRF54_SENSOR_HALOW_POWER_SAVE)
		/* Zero-touch discovery tears down after the complete channel sweep.
		 * A sensor that began the cycle provisioned stays on its assigned
		 * channel and uses MM8108 chip sleep between reports. */
		bool keep_radio = !provisioning_cycle &&
			edgez_config_should_autostart();
		bool ps_ready = keep_radio && (sensor_halow_ps_configured ||
			sensor_configure_halow_power_save());
		if (!ps_ready || !sensor_set_halow_wake(false)) {
			LOG_WRN("sensor HaLow power save failed; cutting rail");
			int stop_rc = stop_mesh_profile(true);

			if (stop_rc != EDGEZ_UMAC_OK) {
				LOG_WRN("sensor mesh shutdown reported rc=%d; forcing rail cutoff",
					stop_rc);
			}
		}
#else
		int stop_rc = stop_mesh_profile(true);

		if (stop_rc != EDGEZ_UMAC_OK) {
			LOG_WRN("sensor mesh shutdown reported rc=%d; forcing rail cutoff",
				stop_rc);
		}
#endif
	}
	if (!mesh_started && edgez_halow_power_is_ready()) {
		int power_rc = edgez_halow_power_set(false);

		if (power_rc != 0 && power_rc != -ENOTSUP) {
			LOG_WRN("sensor HaLow rail cutoff failed rc=%d", power_rc);
		}
	}
	int battery_rc = edgez_battery_refresh_async();
	if (battery_rc != 0 && battery_rc != -ENOTSUP &&
	    battery_rc != -EALREADY) {
		LOG_WRN("sensor battery refresh failed rc=%d", battery_rc);
	}
	sensor_finish_cycle(k_uptime_get_32());
}

int main(void)
{
#if EDGEZ_DIAGNOSTICS_ENABLED && !defined(EDGEZ_PLATFORM_ARCHIVE_BUILD)
	edgez_diagnostics_set_sink(join_diagnostic_sink, NULL);
	edgez_diagnostics_set_level(EDGEZ_DIAGNOSTIC_DEBUG);
#endif
	const bool sensor_role = IS_ENABLED(CONFIG_OFFGRID_NRF54_ROLE_SENSOR);
	LOG_INF("EdgeZ nRF54 role=%s; IP services disabled",
		sensor_role ? "low-power HaLow sensor" : "BLE companion + BATMAN");
	configure_user_button();
	int factory_rc = edgez_factory_data_init();
	if (factory_rc != 0) {
		LOG_ERR("factory identity unavailable rc=%d", factory_rc);
	} else if (sensor_role && sensor_load_factory_bootstrap() != 0) {
		LOG_ERR("factory discovery country/channel unavailable or invalid");
	}
	int sensor_rc = edgez_battery_init();
	if (sensor_rc != 0) {
		LOG_WRN("battery sensing unavailable rc=%d", sensor_rc);
	}
	sensor_rc = edgez_imu_init();
	if (sensor_rc != 0) {
		LOG_WRN("IMU unavailable rc=%d; sensor beacons will omit motion", sensor_rc);
	} else {
		LOG_INF("IMU initialized for low-power sensor sampling");
	}
	edgez_config_init(CONFIG_WIFI_SSID, CONFIG_WIFI_PSK);
	if (sensor_role) {
		sensor_settings_load();
		sensor_downlink_load_ack();
	}
	livestock_config_init();
	if (!sensor_role && edgez_ble_start() != 0) {
		LOG_ERR("BLE control service failed to start");
	} else if (sensor_role) {
		LOG_INF("sensor role: Bluetooth is excluded from this build");
	}

	uint32_t next_report = k_uptime_get_32();
	uint32_t next_mesh_tick = next_report;
	uint32_t next_start_attempt = next_report;
	uint32_t next_beacon = next_report;
	uint32_t next_link_check = next_report;
	uint32_t next_ble_status = next_report;
	bool previous_link_up = false;
	while (true) {
		const uint32_t now = k_uptime_get_32();
		struct edgez_halow_profile profile = {0};
		edgez_config_get_profile(&profile);
		poll_user_button();

		if (sensor_role) {
			if (!edgez_config_should_autostart()) {
				if (sensor_bootstrap_profile(&profile) != 0) {
					LOG_ERR("invalid zero-touch bootstrap country=%s channel=%d",
						sensor_bootstrap_country,
						sensor_bootstrap_channel);
					sensor_force_cpu_low_power();
					k_msleep(1000);
					continue;
				}
			}
			sensor_power_tick(&profile, now);
			edgez_ble_process();
			if (sensor_power.phase == SENSOR_POWER_SLEEPING) {
				if (!edgez_ble_is_stopped()) {
					/* Complete an asynchronous disconnect and controller
					 * shutdown before entering the long timer sleep. */
					k_msleep(20);
					continue;
				}
				const uint32_t sleep_now = k_uptime_get_32();
				const int32_t remaining_ms =
					(int32_t)(sensor_power.next_wake_ms - sleep_now);

				/* Zephyr's tickless idle puts the nRF54L15 to sleep until
				 * the next PAwR cycle or a hardware interrupt (including
				 * the IMU FIFO watermark interrupt). */
				sensor_force_cpu_low_power();
				sensor_sleep_until_button_or_timeout(remaining_ms);
			} else {
				k_msleep(1);
			}
			continue;
		}
		if (sensor_power.phase != SENSOR_POWER_INACTIVE) {
			sensor_leave();
			next_start_attempt = k_uptime_get_32();
		}

		if (!mesh_started && edgez_config_should_autostart() &&
		    (int32_t)(now - next_start_attempt) >= 0) {
			next_start_attempt = now + 5000U;
			if (!edgez_halow_power_is_ready()) {
				LOG_WRN("HaLow profile ready, but the radio power rail is unavailable");
			} else if (start_mesh_profile(&profile, false) != EDGEZ_UMAC_OK) {
				LOG_ERR("HaLow profile start failed; retrying in 5 seconds");
			}
		}

		if (mesh_started && edgez_config_generation() != applied_generation) {
			if (same_radio_profile(&profile, &applied_profile)) {
				applied_profile = profile;
				applied_generation = edgez_config_generation();
				LOG_INF("Identity/beacon settings updated without resetting HaLow generation=%u",
					applied_generation);
			} else {
				edgez_config_set_halow_ready(false);
				if (start_mesh_profile(&profile, true) != EDGEZ_UMAC_OK) {
					LOG_ERR("HaLow profile reconfiguration failed");
				}
			}
		}

		if (mesh_started && (int32_t)(now - next_mesh_tick) >= 0) {
			edgez_umac_mesh_runtime_tick(&mesh_runtime, now);
			edgez_nrf_mesh_tick();
			edgez_nrf_relay_tick();
			next_mesh_tick = now + EDGEZ_MESH_TICK_MS;
		}
		if (mesh_started && (int32_t)(now - next_link_check) >= 0) {
			const bool link_up = has_established_peer();

			edgez_config_set_halow_link(link_up);
			if (link_up && !previous_link_up) {
				/* A boot-time beacon can be queued before a usable radio peer
				 * exists. Send a fresh identity beacon as soon as MPM finishes so
				 * the remote phone does not wait for the full profile interval. */
				next_beacon = now;
				LOG_INF("HaLow link established; scheduling immediate NetworkPacket beacon");
			}
			previous_link_up = link_up;
			next_link_check = now + 500U;
		}
#if defined(CONFIG_OFFGRID_NRF54_MESH_PS_TEST)
		if (mesh_started) mesh_power_save_tick(now);
#endif
		if (mesh_started && (int32_t)(now - next_beacon) >= 0) {
			uint8_t beacon[512];
			size_t beacon_len = 0;
			uint32_t interval = MAX(profile.beacon_interval_seconds, 5U) * 1000U;
			if (!previous_link_up) {
				next_beacon = now + 1000U;
			} else if (edgez_config_build_network_beacon(mesh_runtime.radio_info.mac_addr,
							 beacon, sizeof(beacon), &beacon_len) == 0) {
				int rc = edgez_nrf_relay_send(NULL, beacon, beacon_len);
				if (rc == 0) {
					LOG_INF("NetworkPacket beacon queued bytes=%u",
						(unsigned int)beacon_len);
					next_beacon = now + interval;
				} else {
					LOG_WRN("NetworkPacket beacon queue failed rc=%d; retrying", rc);
					next_beacon = now + 3000U;
				}
			} else {
				LOG_WRN("NetworkPacket beacon build failed; retrying");
				next_beacon = now + 3000U;
			}
		}
		if ((int32_t)(now - next_ble_status) >= 0) {
			uint8_t status_packet[256];
			size_t status_len = 0;
			if (edgez_ble_is_connected() &&
			    edgez_config_build_status_packet(status_packet,
						     sizeof(status_packet), &status_len) == 0) {
				(void)edgez_ble_notify_packet(status_packet, status_len);
			}
			next_ble_status = now + 5000U;
		}
		if (mesh_started && (int32_t)(now - next_report) >= 0) {
			report_peers();
			LOG_INF("BATMAN routes=%u BLE=%s", (unsigned)edgez_nrf_mesh_route_count(),
				edgez_ble_is_connected() ? "connected" : "disconnected");
			next_report = now + EDGEZ_MESH_REPORT_MS;
		}
		k_msleep(
#if defined(CONFIG_OFFGRID_NRF54_MESH_PS_TEST)
			EDGEZ_PS_SAMPLE_MS
#else
			EDGEZ_MESH_TICK_MS
#endif
		);
	}
}

#endif /* CONFIG_WIFI_MORSEMICRO_EDGEZ_JOIN_ONLY */
