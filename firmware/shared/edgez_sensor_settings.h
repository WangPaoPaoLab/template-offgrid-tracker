/*
 * Copyright 2026 EdgeZeron AI AB
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define EDGEZ_SENSOR_SETTINGS_COMMAND "sensor.settings"
#define EDGEZ_SENSOR_SETTINGS_VERSION 2U
#define EDGEZ_SENSOR_SETTINGS_MIN_INTERVAL_MS 1000U
#define EDGEZ_SENSOR_SETTINGS_MAX_INTERVAL_MS 3600000U
#define EDGEZ_SENSOR_SETTINGS_MAX_SAMPLES_PER_BLOCK 32U

#define EDGEZ_SENSOR_BLOCK_VERSION 1U
#define EDGEZ_SENSOR_BLOCK_HEADER_SIZE 24U
#define EDGEZ_SENSOR_BLOCK_FLAG_SAMPLING_ENABLED 0x01U
#define EDGEZ_SENSOR_BLOCK_KIND_MCU_TEMPERATURE 1U
#define EDGEZ_SENSOR_BLOCK_KIND_IMU 2U
#define EDGEZ_SENSOR_BLOCK_KIND_BATTERY_VOLTAGE 3U
#define EDGEZ_SENSOR_BLOCK_IMU_RECORD_SIZE 25U

struct edgez_sensor_settings {
    uint32_t sampling_interval_ms;
    uint16_t block_sampling_count;
};

static inline bool edgez_sensor_settings_valid(
    const struct edgez_sensor_settings *settings)
{
    return settings != NULL &&
        settings->sampling_interval_ms >=
            EDGEZ_SENSOR_SETTINGS_MIN_INTERVAL_MS &&
        settings->sampling_interval_ms <=
            EDGEZ_SENSOR_SETTINGS_MAX_INTERVAL_MS &&
        settings->block_sampling_count <=
            EDGEZ_SENSOR_SETTINGS_MAX_SAMPLES_PER_BLOCK;
}

static inline void edgez_sensor_put_le16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
}

static inline void edgez_sensor_put_le32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
    output[2] = (uint8_t)(value >> 16U);
    output[3] = (uint8_t)(value >> 24U);
}

static inline void edgez_sensor_put_float_le(uint8_t *output, float value)
{
    uint32_t bits = 0U;

    memcpy(&bits, &value, sizeof(bits));
    edgez_sensor_put_le32(output, bits);
}

/** Encode a versioned sensor block header. Settings are copied into every
 * block so a consumer can interpret historical samples even after a later
 * settings downlink changes the sensor's behavior. */
static inline size_t edgez_sensor_block_encode_header(
    uint8_t *output, size_t capacity, uint8_t sample_kind,
    uint16_t record_size, uint16_t sample_count, uint32_t block_sequence,
    uint32_t sample_period_ms, const struct edgez_sensor_settings *settings)
{
    if (output == NULL || capacity < EDGEZ_SENSOR_BLOCK_HEADER_SIZE ||
        record_size == 0U || sample_count == 0U || sample_period_ms == 0U ||
        !edgez_sensor_settings_valid(settings)) {
        return 0U;
    }
    memcpy(output, "EZSB", 4U);
    output[4] = EDGEZ_SENSOR_BLOCK_VERSION;
    output[5] = sample_kind;
    output[6] = settings->block_sampling_count > 0U ?
        EDGEZ_SENSOR_BLOCK_FLAG_SAMPLING_ENABLED : 0U;
    output[7] = EDGEZ_SENSOR_BLOCK_HEADER_SIZE;
    /* This is the interval between samples inside this block, not the
     * device's beacon/wake interval. Sample zero is the block time origin. */
    edgez_sensor_put_le32(output + 8U, sample_period_ms);
    edgez_sensor_put_le16(output + 12U, settings->block_sampling_count);
    edgez_sensor_put_le16(output + 14U, sample_count);
    edgez_sensor_put_le32(output + 16U, block_sequence);
    edgez_sensor_put_le16(output + 20U, record_size);
    edgez_sensor_put_le16(output + 22U, 0U);
    return EDGEZ_SENSOR_BLOCK_HEADER_SIZE;
}

