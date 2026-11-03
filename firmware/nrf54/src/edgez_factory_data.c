/*
 * Copyright 2026 EdgeZeron AI AB
 * SPDX-License-Identifier: Apache-2.0
 */

#include "edgez_factory_data.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>

LOG_MODULE_REGISTER(edgez_factory, LOG_LEVEL_INF);

#define EDGEZ_FACTORY_MAGIC UINT32_C(0x455a4631)
#define EDGEZ_FACTORY_VERSION 2U
#define EDGEZ_FACTORY_SENSOR_COUNTRY "US"
#define EDGEZ_FACTORY_SENSOR_MESH_ID "edgez-wake-test"
#define EDGEZ_FACTORY_SENSOR_CHANNEL 25U

struct __packed edgez_factory_record {
	uint32_t magic;
	uint16_t version;
	uint16_t length;
	char serial[EDGEZ_FACTORY_SERIAL_MAX + 1U];
	uint8_t public_key[EDGEZ_FACTORY_KEY_SIZE];
	uint8_t private_key[EDGEZ_FACTORY_KEY_SIZE];
	char country[EDGEZ_FACTORY_COUNTRY_SIZE];
	char mesh_id[EDGEZ_FACTORY_MESH_ID_SIZE];
	uint8_t channel;
	char passphrase[EDGEZ_FACTORY_PASSPHRASE_SIZE];
	uint32_t crc32;
};

static struct edgez_factory_identity cached_identity;
static bool identity_ready;

static uint32_t record_crc(const struct edgez_factory_record *record)
{
	return crc32_ieee((const uint8_t *)record,
			  offsetof(struct edgez_factory_record, crc32));
}

static bool record_valid(const struct edgez_factory_record *record)
{
	static const uint8_t zero_key[EDGEZ_FACTORY_KEY_SIZE];
	size_t passphrase_len;
	bool profile_present;
	bool profile_valid;

	if (memchr(record->passphrase, '\0', sizeof(record->passphrase)) == NULL) {
		return false;
	}
	passphrase_len = strlen(record->passphrase);
	profile_present = record->mesh_id[0] != '\0' || passphrase_len != 0U;
	profile_valid = !profile_present ||
		(record->mesh_id[0] != '\0' && record->channel != 0U &&
		 (passphrase_len == 0U || passphrase_len >= 8U));

	return record->magic == EDGEZ_FACTORY_MAGIC &&
		record->version == EDGEZ_FACTORY_VERSION &&
		record->length == sizeof(*record) &&
		record->serial[0] != '\0' &&
		memchr(record->serial, '\0', sizeof(record->serial)) != NULL &&
		memcmp(record->public_key, zero_key, sizeof(zero_key)) != 0 &&
		memcmp(record->private_key, zero_key, sizeof(zero_key)) != 0 &&
		record->country[0] != '\0' && record->country[1] != '\0' &&
		record->country[2] == '\0' &&
		record->channel != 0U &&
		memchr(record->mesh_id, '\0', sizeof(record->mesh_id)) != NULL &&
		profile_valid &&
		record->crc32 == record_crc(record);
}

int edgez_factory_data_init(void)
{
	const struct flash_area *area;
	struct edgez_factory_record record = {0};

	identity_ready = false;
	memset(&cached_identity, 0, sizeof(cached_identity));
	int rc = flash_area_open(FIXED_PARTITION_ID(factory_partition), &area);

	if (rc != 0) {
		return rc;
	}
	rc = flash_area_read(area, 0, &record, sizeof(record));
	flash_area_close(area);
	if (rc != 0) {
		return rc;
	}
	if (!record_valid(&record)) {
		memset(&record, 0, sizeof(record));
		LOG_ERR("factory identity is blank or invalid; provision the factory partition");
		return -ENOENT;
	}
	LOG_INF("factory identity loaded serial=%s", record.serial);
	strcpy(cached_identity.serial, record.serial);
	memcpy(cached_identity.public_key, record.public_key,
	       sizeof(cached_identity.public_key));
	memcpy(cached_identity.private_key, record.private_key,
	       sizeof(cached_identity.private_key));
	/* Keep the immutable per-device identity and keys from flash, but use the
	 * fixed factory mesh bootstrap requested for this sensor build. Channel 25
	 * is the US 914.5 MHz / 1 MHz discovery channel accepted by the unchanged
	 * production sensor bootstrap path. */
	strcpy(cached_identity.country, EDGEZ_FACTORY_SENSOR_COUNTRY);
	strcpy(cached_identity.mesh_id, EDGEZ_FACTORY_SENSOR_MESH_ID);
	cached_identity.channel = EDGEZ_FACTORY_SENSOR_CHANNEL;
	cached_identity.passphrase[0] = '\0';
	cached_identity.has_halow_profile = true;
	identity_ready = true;
	memset(&record, 0, sizeof(record));
	return 0;
}

bool edgez_factory_data_is_ready(void)
{
	return identity_ready;
}

int edgez_factory_data_get(struct edgez_factory_identity *identity)
{
	if (!identity) {
		return -EINVAL;
	}
	if (!identity_ready) {
		return -EAGAIN;
	}
	*identity = cached_identity;
	return 0;
}
