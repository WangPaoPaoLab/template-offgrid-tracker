#!/usr/bin/env python3
"""Request a 640x480 JPEG from EdgeZ-Camera and save a verified BLE transfer.

Install: python3 -m pip install -r scripts/requirements-camera-ble.txt
Run:     python3 scripts/camera_ble_capture.py capture.jpg
"""

import argparse
import asyncio
import os
import struct
import tempfile
import zlib
from pathlib import Path

SERVICE_UUID = "a3632000-b82e-44c2-9b1d-a790675b4ac1"
CONTROL_UUID = "a3632001-b82e-44c2-9b1d-a790675b4ac1"
DATA_UUID = "a3632002-b82e-44c2-9b1d-a790675b4ac1"
MAX_JPEG_SIZE = 512 * 1024


def jpeg_dimensions(image: bytes) -> tuple[int, int]:
    """Read the JPEG SOF dimensions without requiring an image library."""
    if not image.startswith(b"\xff\xd8") or not image.endswith(b"\xff\xd9"):
        raise ValueError("missing JPEG SOI/EOI markers")
    offset = 2
    while offset < len(image) - 2:
        if image[offset] != 0xFF:
            raise ValueError("invalid JPEG marker")
        while offset < len(image) and image[offset] == 0xFF:
            offset += 1
        if offset >= len(image):
            break
        marker = image[offset]
        offset += 1
        if marker in (0xD9, 0xDA):
            break
        if marker == 0x01 or 0xD0 <= marker <= 0xD7:
            continue
        if offset + 2 > len(image):
            break
        size = int.from_bytes(image[offset : offset + 2], "big")
        if size < 2 or offset + size > len(image):
            raise ValueError("truncated JPEG segment")
        if marker in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7,
                      0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
            if size < 8:
                raise ValueError("truncated JPEG SOF")
            height, width = struct.unpack_from(">HH", image, offset + 3)
            return width, height
        offset += size
    raise ValueError("JPEG frame dimensions were not found")


class ImageReceiver:
    """Protocol: type(u8), transfer ID(u32 LE), then type-specific payload."""

    def __init__(self) -> None:
        self.transfer_id: int | None = None
        self.expected_length = 0
        self.width = self.height = 0
        self.image = bytearray()

    def feed(self, packet: bytes) -> bytes | None:
        if len(packet) < 5:
            raise ValueError("short BLE camera frame")
        kind, transfer_id = struct.unpack_from("<BI", packet)
        if kind == 4:
            if len(packet) != 9:
                raise ValueError("invalid error frame")
            error, = struct.unpack_from("<i", packet, 5)
            raise RuntimeError(f"camera capture failed: firmware rc={error}; received {len(self.image)} JPEG bytes")
        if kind == 1:
            if len(packet) != 13 or self.transfer_id is not None:
                raise ValueError("invalid or duplicate START frame")
            length, width, height = struct.unpack_from("<IHH", packet, 5)
            if not 4 <= length <= MAX_JPEG_SIZE or (width, height) != (640, 480):
                raise ValueError("invalid JPEG length or requested dimensions")
            self.transfer_id = transfer_id
            self.expected_length, self.width, self.height = length, width, height
            print(f"Receiving photo {transfer_id}: {length:,} bytes, {width}x{height}")
            return None
        if self.transfer_id is None or transfer_id != self.transfer_id:
            raise ValueError("unexpected transfer ID or missing START frame")
        if kind == 2:
            if len(packet) < 10:
                raise ValueError("empty or short DATA frame")
            offset, = struct.unpack_from("<I", packet, 5)
            if offset != len(self.image):
                raise ValueError(f"missing/out-of-order data: expected offset {len(self.image)}, got {offset}")
            if len(self.image) + len(packet) - 9 > self.expected_length:
                raise ValueError("image exceeds advertised length")
            self.image.extend(packet[9:])
            return None
        if kind != 3 or len(packet) != 13:
            raise ValueError("unknown or malformed BLE camera frame")
        length, expected_crc = struct.unpack_from("<II", packet, 5)
        if length != self.expected_length or len(self.image) != length:
            raise ValueError(f"incomplete JPEG: got {len(self.image)} of {self.expected_length} bytes")
        actual_crc = zlib.crc32(self.image) & 0xFFFFFFFF
        if actual_crc != expected_crc:
            raise ValueError(f"CRC mismatch: expected {expected_crc:08X}, got {actual_crc:08X}")
        result = bytes(self.image)
        if jpeg_dimensions(result) != (self.width, self.height):
            raise ValueError("JPEG dimensions do not match advertised resolution")
        print(f"Verified {length:,} bytes, CRC32={actual_crc:08X}, JPEG=640x480")
        return result


def save_jpeg(output: Path, image: bytes) -> None:
    """Publish the file only after all transfer checks have passed."""
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=output.parent, prefix=f".{output.name}.",
                                         suffix=".part", delete=False) as handle:
            temporary = Path(handle.name)
            handle.write(image)
        os.replace(temporary, output)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


async def capture(args: argparse.Namespace) -> None:
    # Keep the packet validator importable/testable without Bluetooth packages.
    try:
        from bleak import BleakClient, BleakScanner
    except ImportError as exc:
        raise RuntimeError("Install the receiver dependencies: python3 -m pip install -r scripts/requirements-camera-ble.txt") from exc

    print(f"Scanning for {args.name}...")
    if args.address:
        device = await BleakScanner.find_device_by_address(
            args.address, timeout=args.scan_timeout)
    else:
        device = await BleakScanner.find_device_by_filter(
            lambda device, advert: advert.local_name == args.name or device.name == args.name,
            timeout=args.scan_timeout, service_uuids=[SERVICE_UUID])
    if device is None:
        raise RuntimeError("Camera not found; check board power, firmware boot log and Bluetooth permissions")
    print(f"Connecting to {device.name} ({device.address})")
    loop = asyncio.get_running_loop()
    done: asyncio.Future[bytes] = loop.create_future()
    receiver = ImageReceiver()

    def disconnected(_client) -> None:
        if not done.done():
            done.set_exception(ConnectionError("BLE disconnected before JPEG verification"))

    def notification(_characteristic, packet: bytearray) -> None:
        if done.done():
            return
        try:
            image = receiver.feed(bytes(packet))
            if image is not None:
                done.set_result(image)
        except Exception as exc:
            done.set_exception(exc)

    async with BleakClient(device, disconnected_callback=disconnected,
                           timeout=args.scan_timeout) as client:
        await client.start_notify(DATA_UUID, notification)
        print(f"Connected; host reports ATT MTU={client.mtu_size} (firmware maximum 300)")
        print("Requesting a new photo...")
        await client.write_gatt_char(CONTROL_UUID, b"\x01", response=True)
        try:
            image = await asyncio.wait_for(done, timeout=args.timeout)
        except asyncio.TimeoutError as exc:
            raise RuntimeError(f"Timed out after {args.timeout:g}s: received {len(receiver.image):,} JPEG bytes") from exc
        save_jpeg(args.output, image)
        print(f"Saved {args.output.resolve()}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, nargs="?", default=Path("capture.jpg"))
    parser.add_argument("--name", default="EdgeZ-Camera")
    parser.add_argument("--address", help="specific BLE address (device UUID on macOS)")
    parser.add_argument("--scan-timeout", type=float, default=20)
    parser.add_argument("--timeout", type=float, default=120, help="capture/transfer timeout in seconds")
    args = parser.parse_args()
    try:
        asyncio.run(capture(args))
    except KeyboardInterrupt:
        raise SystemExit("Capture cancelled") from None
    except Exception as exc:
        parser.exit(1, f"Error: {exc}\n")


if __name__ == "__main__":
    main()
