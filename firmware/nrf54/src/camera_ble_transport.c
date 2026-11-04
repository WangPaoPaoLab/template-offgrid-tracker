/* Small camera-only GATT service; all camera I/O runs in main(), never in
 * the Bluetooth receive thread. Notifications carry binary JPEG data.
 */
#include <zephyr/autoconf.h>

#if defined(CONFIG_OFFGRID_NRF54_CAMERA_TEST)

#include "camera_ble_transport.h"
#include "camera_resolutions.h"

#include <errno.h>
#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#define CAMERA_UUID(value) BT_UUID_128_ENCODE(value, 0xb82e, 0x44c2, 0x9b1d, 0xa790675b4ac1)
#define CAMERA_SERVICE_UUID CAMERA_UUID(0xa3632000)
#define CAMERA_CONTROL_UUID CAMERA_UUID(0xa3632001)
#define CAMERA_DATA_UUID CAMERA_UUID(0xa3632002)
#define FRAME_MAX (CONFIG_BT_L2CAP_TX_MTU - 3U)

static struct bt_uuid_128 service_uuid = BT_UUID_INIT_128(CAMERA_SERVICE_UUID);
static struct bt_uuid_128 control_uuid = BT_UUID_INIT_128(CAMERA_CONTROL_UUID);
static struct bt_uuid_128 data_uuid = BT_UUID_INIT_128(CAMERA_DATA_UUID);
static atomic_t busy;
static struct bt_conn *transfer_conn;
static uint32_t transfer_id;
static uint16_t requested_width = 640;
static uint16_t requested_height = 480;
K_SEM_DEFINE(capture_request, 0, 1);
extern const struct bt_gatt_service_static camera_svc;

static ssize_t capture_write(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr, const void *buf,
			     uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);
	if (offset != 0U) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if ((len != 1U && len != 5U) || ((const uint8_t *)buf)[0] != 1U) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}
	uint16_t width = requested_width;
	uint16_t height = requested_height;
	if (len == 5U) {
		width = sys_get_le16((const uint8_t *)buf + 1);
		height = sys_get_le16((const uint8_t *)buf + 3);
		if (camera_resolution_find(width, height) == NULL) {
			return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
		}
	}
	if (!bt_gatt_is_subscribed(conn, &camera_svc.attrs[4], BT_GATT_CCC_NOTIFY)) {
		return BT_GATT_ERR(BT_ATT_ERR_CCC_IMPROPER_CONF);
	}
	if (!atomic_cas(&busy, 0, 1)) {
		return BT_GATT_ERR(BT_ATT_ERR_PROCEDURE_IN_PROGRESS);
	}
	transfer_conn = bt_conn_ref(conn);
	requested_width = width;
	requested_height = height;
	transfer_id++;
	k_sem_give(&capture_request);
	return len;
}

BT_GATT_SERVICE_DEFINE(camera_svc,
	BT_GATT_PRIMARY_SERVICE(&service_uuid),
	BT_GATT_CHARACTERISTIC(&control_uuid.uuid, BT_GATT_CHRC_WRITE,
		BT_GATT_PERM_WRITE, NULL, capture_write, NULL),
	BT_GATT_CHARACTERISTIC(&data_uuid.uuid, BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE)
);

static const struct bt_data advertising[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, CAMERA_SERVICE_UUID),
};
static const struct bt_data scan_response[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void advertise(struct k_work *work)
{
	ARG_UNUSED(work);
	int rc = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1,
		advertising, ARRAY_SIZE(advertising),
		scan_response, ARRAY_SIZE(scan_response));
	if (rc != 0 && rc != -EALREADY) {
		printk("camera BLE: advertising failed rc=%d\n", rc);
	}
}
K_WORK_DEFINE(advertise_work, advertise);

static void connected(struct bt_conn *conn, uint8_t error)
{
	ARG_UNUSED(conn);
	printk("camera BLE: connection result=%u\n", error);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	printk("camera BLE: disconnected reason=%u\n", reason);
}

static void recycled(void)
{
	k_work_submit(&advertise_work);
}

BT_CONN_CB_DEFINE(camera_conn_cb) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
};

int camera_ble_init(void)
{
	int rc = bt_enable(NULL);
	if (rc != 0) {
		return rc;
	}
	rc = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1,
		advertising, ARRAY_SIZE(advertising),
		scan_response, ARRAY_SIZE(scan_response));
	if (rc == 0) {
		printk("camera BLE: advertising as %s; waiting for Python capture request\n",
		       CONFIG_BT_DEVICE_NAME);
	}
	return rc;
}

void camera_ble_wait_request(uint16_t *width, uint16_t *height)
{
	k_sem_take(&capture_request, K_FOREVER);
	*width = requested_width;
	*height = requested_height;
	printk("camera BLE: capture request id=%u MTU=%u resolution=%ux%u\n",
	       transfer_id, bt_gatt_get_mtu(transfer_conn), *width, *height);
}

/* bt_gatt_notify copies data into the ATT TX buffer before returning.
 * Called only from main; retry buffer pressure with a bounded deadline.
 */
static int send_frame(const uint8_t *frame, size_t length)
{
	int64_t deadline = k_uptime_get() + 10000;
	int rc;
	do {
		if (!bt_gatt_is_subscribed(transfer_conn, &camera_svc.attrs[4],
					  BT_GATT_CCC_NOTIFY)) {
			return -ENOTCONN;
		}
		rc = bt_gatt_notify(transfer_conn, &camera_svc.attrs[4], frame, length);
		if (rc != -ENOMEM && rc != -EAGAIN && rc != -ENOBUFS) {
			return rc;
		}
		k_msleep(5);
	} while (k_uptime_get() < deadline);
	return -ETIMEDOUT;
}

int camera_ble_begin(uint32_t length, uint16_t width, uint16_t height)
{
	uint8_t frame[13] = {1};
	sys_put_le32(transfer_id, &frame[1]);
	sys_put_le32(length, &frame[5]);
	sys_put_le16(width, &frame[9]);
	sys_put_le16(height, &frame[11]);
	return send_frame(frame, sizeof(frame));
}

int camera_ble_data(uint32_t offset, const uint8_t *data, size_t length)
{
	uint8_t frame[FRAME_MAX] = {2};
	const uint16_t mtu = bt_gatt_get_mtu(transfer_conn);
	if (mtu < 23U) {
		return -EMSGSIZE;
	}
	const size_t payload_max = MIN(sizeof(frame), mtu - 3U) - 9U;

	sys_put_le32(transfer_id, &frame[1]);
	while (length != 0U) {
		size_t chunk = MIN(length, payload_max);
		sys_put_le32(offset, &frame[5]);
		memcpy(&frame[9], data, chunk);
		int rc = send_frame(frame, chunk + 9U);
		if (rc != 0) {
			return rc;
		}
		offset += chunk;
		data += chunk;
		length -= chunk;
	}
	return 0;
}

int camera_ble_end(uint32_t length, uint32_t crc)
{
	uint8_t frame[13] = {3};
	sys_put_le32(transfer_id, &frame[1]);
	sys_put_le32(length, &frame[5]);
	sys_put_le32(crc, &frame[9]);
	return send_frame(frame, sizeof(frame));
}

void camera_ble_finish(int error)
{
	if (error != 0) {
		uint8_t frame[9] = {4};
		sys_put_le32(transfer_id, &frame[1]);
		sys_put_le32((uint32_t)error, &frame[5]);
		(void)send_frame(frame, sizeof(frame));
	}
	bt_conn_unref(transfer_conn);
	transfer_conn = NULL;
	atomic_clear(&busy);
	printk("camera BLE: request complete rc=%d\n", error);
}

#endif /* CONFIG_OFFGRID_NRF54_CAMERA_TEST */
