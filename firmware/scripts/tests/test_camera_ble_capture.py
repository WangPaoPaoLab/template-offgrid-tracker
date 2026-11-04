"""Exercise the receiver's loss/corruption detection independently of BLE."""
import importlib.util
import struct
import unittest
import zlib
from pathlib import Path

SPEC = importlib.util.spec_from_file_location(
    "camera_ble_capture", Path(__file__).resolve().parents[1] / "camera_ble_capture.py")
camera = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(camera)

# SOI, baseline SOF (640x480, one component), EOI. Only a parser fixture;
# hardware validation additionally decodes a real camera JPEG.
JPEG = bytes.fromhex("FFD8FFC0000B0801E0028001011100FFD9")


class TransferTest(unittest.TestCase):
    def receiver(self):
        receiver = camera.ImageReceiver()
        receiver.feed(struct.pack("<BIIHH", 1, 7, len(JPEG), 640, 480))
        return receiver

    def test_small_mtu_chunk_reassembly(self):
        receiver = self.receiver()
        for offset in range(0, len(JPEG), 11):
            receiver.feed(struct.pack("<BII", 2, 7, offset) + JPEG[offset:offset + 11])
        self.assertEqual(receiver.feed(struct.pack(
            "<BIII", 3, 7, len(JPEG), zlib.crc32(JPEG))), JPEG)

    def test_missing_chunk(self):
        with self.assertRaisesRegex(ValueError, "missing/out-of-order"):
            self.receiver().feed(struct.pack("<BII", 2, 7, 1) + JPEG)

    def test_wrong_transfer(self):
        with self.assertRaisesRegex(ValueError, "transfer ID"):
            self.receiver().feed(struct.pack("<BII", 2, 8, 0) + JPEG)

    def test_corrupt_data(self):
        receiver = self.receiver()
        receiver.feed(struct.pack("<BII", 2, 7, 0) + JPEG)
        with self.assertRaisesRegex(ValueError, "CRC mismatch"):
            receiver.feed(struct.pack("<BIII", 3, 7, len(JPEG), 0))

    def test_truncated_transfer(self):
        with self.assertRaisesRegex(ValueError, "incomplete"):
            self.receiver().feed(struct.pack("<BIII", 3, 7, len(JPEG), 0))

    def test_wrong_dimensions(self):
        receiver = self.receiver()
        image = JPEG.replace(bytes.fromhex("01E00280"), bytes.fromhex("00F00140"))
        receiver.feed(struct.pack("<BII", 2, 7, 0) + image)
        with self.assertRaisesRegex(ValueError, "dimensions"):
            receiver.feed(struct.pack("<BIII", 3, 7, len(image), zlib.crc32(image)))

    def test_selected_720p_resolution(self):
        image = JPEG.replace(bytes.fromhex("01E00280"), bytes.fromhex("02D00500"))
        receiver = camera.ImageReceiver((1280, 720))
        receiver.feed(struct.pack("<BIIHH", 1, 9, len(image), 1280, 720))
        receiver.feed(struct.pack("<BII", 2, 9, 0) + image)
        self.assertEqual(receiver.feed(struct.pack(
            "<BIII", 3, 9, len(image), zlib.crc32(image))), image)

    def test_start_resolution_must_match_request(self):
        with self.assertRaisesRegex(ValueError, "requested dimensions"):
            camera.ImageReceiver((1280, 720)).feed(
                struct.pack("<BIIHH", 1, 9, len(JPEG), 640, 480))

    def test_error_before_start(self):
        with self.assertRaisesRegex(RuntimeError, "rc=-116"):
            camera.ImageReceiver().feed(struct.pack("<BIi", 4, 7, -116))

    def test_bad_markers(self):
        with self.assertRaisesRegex(ValueError, "SOI/EOI"):
            camera.jpeg_dimensions(JPEG[:-2])


if __name__ == "__main__":
    unittest.main()
