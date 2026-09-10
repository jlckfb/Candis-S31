#!/usr/bin/env python3
"""Host regressions for the camera diagnostic wire format."""

import binascii
import importlib.util
import io
import unittest
from pathlib import Path

SPEC = importlib.util.spec_from_file_location(
    "capture_camera_frame", Path(__file__).with_name("capture_camera_frame.py"))
capture = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(capture)


class CameraDumpTests(unittest.TestCase):
    def make_dump(self, order="RGB565X"):
        raw = bytes(range(256)) * 3750
        rgb = bytes(range(256)) * 1653 + bytes(range(32))
        self.assertEqual(len(rgb), 460 * 460 * 2)
        header = (
            "CAMERA_FRAME_DUMP_BEGIN version=2 baud=2000000 raw=960000 "
            "rgb=423200 stride=1600 width=800 height=600 crop_x=170 crop_y=70 "
            f"order={order} rgb_order=big "
            f"raw_crc32={binascii.crc32(raw):08x} "
            f"rgb_crc32={binascii.crc32(rgb):08x}\n"
        ).encode()
        return header, raw, rgb, b"\nCAMERA_FRAME_DUMP_END status=PASS\n"

    def test_binary_control_characters_round_trip(self):
        header, raw, rgb, trailer = self.make_dump()
        fields, actual_raw, actual_rgb = capture.receive_frame(
            io.BytesIO(b"boot log\n" + header + raw + rgb + trailer), 2)
        self.assertEqual(fields["order"], "RGB565X")
        self.assertEqual((actual_raw, actual_rgb), (raw, rgb))

    def test_yuv_format_round_trip(self):
        parts = self.make_dump("UYVY")
        fields, _, _ = capture.receive_frame(io.BytesIO(b"".join(parts)), 2)
        self.assertEqual(fields["order"], "UYVY")

    def test_crc_rejects_corrupted_source(self):
        header, raw, rgb, trailer = self.make_dump()
        raw = bytes([raw[0] ^ 1]) + raw[1:]
        with self.assertRaisesRegex(ValueError, "raw CRC32 mismatch"):
            capture.receive_frame(io.BytesIO(header + raw + rgb + trailer), 2)

    def test_crc_rejects_corrupted_output(self):
        header, raw, rgb, trailer = self.make_dump()
        rgb = bytes([rgb[0] ^ 1]) + rgb[1:]
        with self.assertRaisesRegex(ValueError, "rgb CRC32 mismatch"):
            capture.receive_frame(io.BytesIO(header + raw + rgb + trailer), 2)

    def test_rejects_text_expansion(self):
        wire = b"".join(self.make_dump()).replace(b"\n", b"\r\n")
        with self.assertRaisesRegex(ValueError, "invalid frame trailer"):
            capture.receive_frame(io.BytesIO(wire), 2)

    def test_rejects_unverified_legacy_dump(self):
        with self.assertRaisesRegex(ValueError, "unverified legacy dump"):
            capture.receive_frame(io.BytesIO(
                b"CAMERA_FRAME_DUMP_BEGIN raw=960000 rgb=423200\n"), 2)

    def test_rejects_geometry_before_reading_payload(self):
        header, _, _, _ = self.make_dump()
        with self.assertRaisesRegex(ValueError, "invalid frame geometry"):
            capture.receive_frame(io.BytesIO(header.replace(
                b"raw=960000", b"raw=960001")), 2)

    def test_rgb565_primary_colors(self):
        pixels = bytes.fromhex("f80007e0001f")
        self.assertEqual(capture.render_rgb565(pixels, 3, 1, "big"),
                         bytes((255, 0, 0, 0, 255, 0, 0, 0, 255)))


if __name__ == "__main__":
    unittest.main()
