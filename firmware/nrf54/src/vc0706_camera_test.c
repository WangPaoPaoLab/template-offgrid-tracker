#include <zephyr/autoconf.h>

#if defined(CONFIG_OFFGRID_NRF54_CAMERA_TEST)

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <hal/nrf_gpio.h>

#define CAMERA_NODE DT_ALIAS(camera0)
#define CAMERA_RESPONSE_TIMEOUT_MS 2000
#define CAMERA_CAPTURE_TIMEOUT_MS 10000
#define CAMERA_STARTUP_DELAY_MS 4000
#define CAMERA_READ_CHUNK_SIZE 256U
#define CAMERA_MAX_JPEG_SIZE (512U * 1024U)
#define CAMERA_RX_RING_SIZE 1024U

static const struct device *const camera_uart = DEVICE_DT_GET(CAMERA_NODE);
static uint8_t camera_rx_ring[CAMERA_RX_RING_SIZE];
static volatile uint16_t camera_rx_head;
static volatile uint16_t camera_rx_tail;
static volatile uint32_t camera_rx_overruns;

static bool camera_rx_pop(uint8_t *value)
{
	unsigned int key = irq_lock();

	if (camera_rx_tail == camera_rx_head) {
		irq_unlock(key);
		return false;
	}
	*value = camera_rx_ring[camera_rx_tail];
	camera_rx_tail = (camera_rx_tail + 1U) % CAMERA_RX_RING_SIZE;
	irq_unlock(key);
	return true;
}

static void camera_uart_isr(const struct device *dev, void *user_data)
{
	uint8_t data[32];

	ARG_UNUSED(user_data);
	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (!uart_irq_rx_ready(dev)) {
			continue;
		}
		int count = uart_fifo_read(dev, data, sizeof(data));

		for (int i = 0; i < count; i++) {
			uint16_t next = (camera_rx_head + 1U) % CAMERA_RX_RING_SIZE;

			if (next == camera_rx_tail) {
				camera_rx_overruns++;
				continue;
			}
			camera_rx_ring[camera_rx_head] = data[i];
			camera_rx_head = next;
		}
	}
}

static int camera_uart_rx_start(void)
{
	int rc = uart_irq_callback_user_data_set(camera_uart, camera_uart_isr,
					     NULL);

	if (rc != 0) {
		return rc;
	}
	uart_irq_rx_enable(camera_uart);
	return 0;
}

static int camera_power_on(void)
{
	const uint32_t pin = NRF_GPIO_PIN_MAP(1, 9);

	/* Set the output latch before changing direction to avoid a low pulse. */
	nrf_gpio_pin_set(pin);
	nrf_gpio_cfg_output(pin);
	return 0;
}

static int camera_read_byte(uint8_t *value, int32_t timeout_ms)
{
	const int64_t deadline = k_uptime_get() + timeout_ms;

	do {
		if (camera_rx_pop(value)) {
			return 0;
		}
		k_msleep(1);
	} while (k_uptime_get() < deadline);

	return -ETIMEDOUT;
}

static void camera_drain_rx(void)
{
	uint8_t value;
	int64_t quiet_deadline = k_uptime_get() + 20;

	while (k_uptime_get() < quiet_deadline) {
		if (camera_rx_pop(&value)) {
			quiet_deadline = k_uptime_get() + 20;
		} else {
			k_msleep(1);
		}
	}
}

static void camera_write(const uint8_t *data, size_t length)
{
	for (size_t i = 0; i < length; i++) {
		uart_poll_out(camera_uart, data[i]);
	}
}

static int camera_read_exact(uint8_t *data, size_t length, int32_t timeout_ms)
{
	for (size_t i = 0; i < length; i++) {
		int rc = camera_read_byte(&data[i], timeout_ms);

		if (rc != 0) {
			return rc;
		}
	}
	return 0;
}

static int camera_command_ack(const uint8_t *command, size_t command_length,
			      uint8_t expected_command, int32_t timeout_ms)
{
	uint8_t response[5];
	int rc;

	camera_drain_rx();
	camera_write(command, command_length);
	rc = camera_read_exact(response, sizeof(response), timeout_ms);
	if (rc != 0) {
		return rc;
	}
	if (response[0] != 0x76 || response[1] != 0x00 ||
	    response[2] != expected_command || response[4] != 0x00) {
		printk("camera: unexpected ack %02X %02X %02X %02X %02X\n",
		       response[0], response[1], response[2], response[3], response[4]);
		return -EBADMSG;
	}
	return 0;
}

static int camera_reset(void)
{
	static const uint8_t command[] = {0x56, 0x00, 0x26, 0x00};
	static const uint8_t prefix[] = {0x76, 0x00, 0x26, 0x00};
	size_t matched = 0U;
	int64_t deadline;

	printk("camera: reset TX hex=56002600\n");
	camera_drain_rx();
	camera_write(command, sizeof(command));
	deadline = k_uptime_get() + CAMERA_RESPONSE_TIMEOUT_MS;
	while (matched < sizeof(prefix) && k_uptime_get() < deadline) {
		uint8_t value;

		if (!camera_rx_pop(&value)) {
			k_msleep(1);
			continue;
		}
		if (value == prefix[matched]) {
			matched++;
		} else {
			matched = value == prefix[0] ? 1U : 0U;
		}
	}
	if (matched != sizeof(prefix)) {
		printk("camera: reset RX sync timeout rc=%d\n", -ETIMEDOUT);
		return -ETIMEDOUT;
	}
	printk("camera: reset RX hex=76002600\n");
	return 0;
}

static int camera_query_version(void)
{
	static const uint8_t command[] = {0x56, 0x00, 0x11, 0x00};
	static const uint8_t prefix[] = {0x76, 0x00, 0x11, 0x00};
	uint8_t version_length;
	uint8_t version[64];
	size_t matched = 0U;
	int64_t deadline;
	int rc;

	printk("camera: version TX hex=56001100\n");
	camera_drain_rx();
	camera_write(command, sizeof(command));
	deadline = k_uptime_get() + CAMERA_RESPONSE_TIMEOUT_MS;
	while (matched < sizeof(prefix) && k_uptime_get() < deadline) {
		uint8_t value;

		if (!camera_rx_pop(&value)) {
			k_msleep(1);
			continue;
		}
		if (value == prefix[matched]) {
			matched++;
		} else {
			matched = value == prefix[0] ? 1U : 0U;
		}
	}
	if (matched != sizeof(prefix)) {
		printk("camera: version RX sync timeout rc=%d\n", -ETIMEDOUT);
		return -ETIMEDOUT;
	}

	/* The vendor cmd_test() considers the four-byte prefix sufficient. Read
	 * and print the optional version payload from the newer protocol too.
	 */
	rc = camera_read_byte(&version_length, 250);
	if (rc != 0) {
		printk("camera: version RX prefix=76001100; vendor cmd_test accepted (no payload length)\n");
		return 0;
	}
	if (version_length == 0U || version_length > sizeof(version)) {
		printk("camera: version RX prefix=76001100 invalid length=%u\n",
		       version_length);
		return -EBADMSG;
	}
	rc = camera_read_exact(version, version_length, CAMERA_RESPONSE_TIMEOUT_MS);
	if (rc != 0) {
		printk("camera: version RX prefix=76001100 length=%u; vendor cmd_test accepted (payload timeout rc=%d)\n",
		       version_length, rc);
		return 0;
	}
	printk("camera: version RX hex=76001100%02X", version_length);
	for (size_t i = 0; i < version_length; i++) {
		printk("%02X", version[i]);
	}
	printk("\ncamera: version text=\"");
	for (size_t i = 0; i < version_length; i++) {
		printk("%c", version[i] >= 0x20 && version[i] <= 0x7E ?
		       version[i] : '.');
	}
	printk("\"\n");
	return 0;
}

static int camera_set_640x480(void)
{
	/* Protocol V1.14: field 0x04 plus 0x19,0x00 selects 640x480. */
	static const uint8_t command[] = {
		0x56, 0x00, 0x31, 0x05, 0x04, 0x01, 0x00, 0x19, 0x00,
	};

	return camera_command_ack(command, sizeof(command), 0x31,
				  CAMERA_RESPONSE_TIMEOUT_MS);
}

static int camera_capture(void)
{
	static const uint8_t command[] = {0x56, 0x00, 0x36, 0x01, 0x00};

	return camera_command_ack(command, sizeof(command), 0x36,
				  CAMERA_CAPTURE_TIMEOUT_MS);
}

static int camera_get_jpeg_length(uint32_t *jpeg_length)
{
	static const uint8_t command[] = {0x56, 0x00, 0x34, 0x01, 0x00};
	uint8_t response[9];
	int rc;

	camera_drain_rx();
	camera_write(command, sizeof(command));
	rc = camera_read_exact(response, sizeof(response),
			       CAMERA_RESPONSE_TIMEOUT_MS);
	if (rc != 0) {
		return rc;
	}
	if (response[0] != 0x76 || response[1] != 0x00 ||
	    response[2] != 0x34 || response[3] != 0x00 ||
	    response[4] != 0x04) {
		return -EBADMSG;
	}
	*jpeg_length = sys_get_be32(&response[5]);
	if (*jpeg_length < 4U || *jpeg_length > CAMERA_MAX_JPEG_SIZE) {
		return -EMSGSIZE;
	}
	return 0;
}

static void print_hex_bytes(const uint8_t *data, size_t length,
			    uint32_t *column)
{
	for (size_t i = 0; i < length; i++) {
		printk("%02X", data[i]);
		(*column)++;
		if (*column == 32U) {
			printk("\n");
			*column = 0U;
		}
	}
}

static int camera_read_and_print_jpeg(uint32_t jpeg_length)
{
	uint8_t command[] = {
		0x56, 0x00, 0x32, 0x0C, 0x00, 0x0A,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0xFF,
	};
	uint8_t data[CAMERA_READ_CHUNK_SIZE];
	uint8_t framing[5];
	uint8_t first[2] = {0};
	uint8_t last[2] = {0};
	uint32_t offset = 0U;
	uint32_t column = 0U;

	printk("IMAGE_HEX_BEGIN length=%u resolution=640x480\n", jpeg_length);
	while (offset < jpeg_length) {
		const uint32_t chunk = MIN(CAMERA_READ_CHUNK_SIZE,
					   jpeg_length - offset);
		int rc;

		sys_put_be32(offset, &command[6]);
		sys_put_be32(chunk, &command[10]);
		camera_drain_rx();
		camera_write(command, sizeof(command));
		rc = camera_read_exact(framing, sizeof(framing),
				       CAMERA_RESPONSE_TIMEOUT_MS);
		if (rc != 0) {
			return rc;
		}
		if (framing[0] != 0x76 || framing[1] != 0x00 ||
		    framing[2] != 0x32 || framing[3] != 0x00 ||
		    framing[4] != 0x00) {
			return -EBADMSG;
		}
		rc = camera_read_exact(data, chunk, CAMERA_RESPONSE_TIMEOUT_MS);
		if (rc != 0) {
			return rc;
		}
		rc = camera_read_exact(framing, sizeof(framing),
				       CAMERA_RESPONSE_TIMEOUT_MS);
		if (rc != 0) {
			return rc;
		}
		if (framing[0] != 0x76 || framing[1] != 0x00 ||
		    framing[2] != 0x32 || framing[3] != 0x00 ||
		    framing[4] != 0x00) {
			return -EBADMSG;
		}
		if (offset == 0U) {
			first[0] = data[0];
			first[1] = data[1];
		}
		if (offset + chunk == jpeg_length) {
			last[0] = data[chunk - 2U];
			last[1] = data[chunk - 1U];
		}
		print_hex_bytes(data, chunk, &column);
		offset += chunk;
	}
	if (column != 0U) {
		printk("\n");
	}
	printk("IMAGE_HEX_END length=%u jpeg=%s\n", jpeg_length,
	       first[0] == 0xFF && first[1] == 0xD8 &&
	       last[0] == 0xFF && last[1] == 0xD9 ? "valid" : "invalid");

	return first[0] == 0xFF && first[1] == 0xD8 &&
	       last[0] == 0xFF && last[1] == 0xD9 ? 0 : -EBADMSG;
}

int main(void)
{
	static const uint32_t baud_rates[] = {921600U};
	struct uart_config camera_uart_config = {
		.baudrate = baud_rates[0],
		.parity = UART_CFG_PARITY_NONE,
		.stop_bits = UART_CFG_STOP_BITS_1,
		.data_bits = UART_CFG_DATA_BITS_8,
		.flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
	};
	uint32_t jpeg_length;
	int rc;

	printk("Putal/VC0706-compatible camera test: UART=921600, resolution=640x480\n");
	rc = camera_power_on();
	if (rc != 0) {
		printk("camera: immediate power enable failed rc=%d\n", rc);
		return rc;
	}
	printk("camera: power enabled on P1.09 immediately after boot\n");
	printk("camera: waiting %u ms before any application UART call\n",
	       CAMERA_STARTUP_DELAY_MS);
	k_msleep(CAMERA_STARTUP_DELAY_MS);

	if (!device_is_ready(camera_uart)) {
		printk("camera: UART device %s is not ready (initialized=%u init_res=%u)\n",
		       camera_uart->name, camera_uart->state->initialized,
		       camera_uart->state->init_res);
		return -ENODEV;
	}
	for (size_t i = 0; i < ARRAY_SIZE(baud_rates); i++) {
		camera_uart_config.baudrate = baud_rates[i];
		rc = uart_configure(camera_uart, &camera_uart_config);
		if (rc != 0) {
			printk("camera: UART configure %u failed rc=%d\n",
			       baud_rates[i], rc);
			continue;
		}
		printk("camera: trying UART=%u 8N1\n", baud_rates[i]);
		rc = camera_uart_rx_start();
		if (rc != 0) {
			printk("camera: interrupt RX start failed rc=%d\n", rc);
			continue;
		}
		printk("camera: interrupt-driven RX ring enabled (%u bytes)\n",
		       CAMERA_RX_RING_SIZE);
		k_msleep(50);
		rc = camera_reset();
		if (rc != 0) {
			printk("camera: reset failed at %u, rc=%d\n",
			       baud_rates[i], rc);
			continue;
		}
		printk("camera: reset acknowledged; waiting %u ms for camera reboot\n",
		       CAMERA_STARTUP_DELAY_MS);
		k_msleep(CAMERA_STARTUP_DELAY_MS);
		for (uint32_t attempt = 1U; attempt <= 3U; attempt++) {
			printk("camera: vendor cmd_test attempt %u/3 at %u\n",
			       attempt, baud_rates[i]);
			rc = camera_query_version();
			if (rc == 0) {
				break;
			}
			k_msleep(1000);
		}
		if (rc == 0) {
			printk("camera: detected baud rate %u\n", baud_rates[i]);
			break;
		}
		printk("camera: no valid version response at %u, rc=%d\n",
		       baud_rates[i], rc);
	}
	if (rc != 0) {
		printk("camera: version query failed at 921600; stopping before capture\n");
		return rc;
	}
	printk("camera: version query succeeded; protocol link verified\n");

	rc = camera_set_640x480();
	if (rc != 0) {
		printk("camera: set resolution failed rc=%d\n", rc);
		return rc;
	}
	rc = camera_capture();
	if (rc != 0) {
		printk("camera: capture failed rc=%d\n", rc);
		return rc;
	}
	rc = camera_get_jpeg_length(&jpeg_length);
	if (rc != 0) {
		printk("camera: read JPEG length failed rc=%d\n", rc);
		return rc;
	}
	printk("camera: captured %u-byte JPEG; printing hex\n", jpeg_length);
	rc = camera_read_and_print_jpeg(jpeg_length);
	if (rc != 0) {
		printk("camera: JPEG transfer failed rc=%d\n", rc);
		return rc;
	}
	printk("camera: capture complete\n");
	return 0;
}

#endif /* CONFIG_OFFGRID_NRF54_CAMERA_TEST */
