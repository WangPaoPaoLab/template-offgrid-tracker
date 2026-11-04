#ifndef CAMERA_RESOLUTIONS_H
#define CAMERA_RESOLUTIONS_H

#include <stddef.h>
#include <stdint.h>

struct camera_resolution {
	uint16_t width;
	uint16_t height;
	uint8_t field;
	uint8_t code;
};

/* Putal V1.14 modes up to the attached 3 MP camera's native resolution.
 * Firmware support still depends on the camera model; the receiver validates
 * the actual JPEG dimensions instead of assuming a set-command succeeded.
 */
static inline const struct camera_resolution *camera_resolution_find(
	uint16_t width, uint16_t height)
{
	static const struct camera_resolution modes[] = {
		{160, 120, 0x04, 0x22},
		{320, 240, 0x04, 0x11},
		{640, 480, 0x04, 0x00},
		{1024, 768, 0x05, 0x33},
		{1280, 720, 0x05, 0x44},
		{1280, 960, 0x05, 0x55},
		{1600, 1200, 0x05, 0x77},
		{1920, 1080, 0x05, 0x66},
		{2304, 1296, 0x05, 0x88},
	};
	for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		if (modes[i].width == width && modes[i].height == height) {
			return &modes[i];
		}
	}
	return NULL;
}

#endif
