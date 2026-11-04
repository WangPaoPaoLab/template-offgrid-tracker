#ifndef CAMERA_BLE_TRANSPORT_H
#define CAMERA_BLE_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

int camera_ble_init(void);
void camera_ble_wait_request(uint16_t *width, uint16_t *height);
int camera_ble_begin(uint32_t length, uint16_t width, uint16_t height);
int camera_ble_data(uint32_t offset, const uint8_t *data, size_t length);
int camera_ble_end(uint32_t length, uint32_t crc);
void camera_ble_finish(int error);

#endif
