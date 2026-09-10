#!/usr/bin/env python3
"""Capture a length-delimited camera frame dump and render it as PPM files."""

from __future__ import annotations

import argparse
import binascii
import re
import time
from pathlib import Path

import serial


BEGIN = b"CAMERA_FRAME_DUMP_BEGIN "


def read_exact(port: serial.Serial, length: int, deadline: float) -> bytes:
    data = bytearray()
    while len(data) < length:
        if time.monotonic() >= deadline:
            raise TimeoutError(f"serial payload stopped at {len(data)}/{length} bytes")
        chunk = port.read(min(65536, length - len(data)))
        if chunk:
            data.extend(chunk)
    return bytes(data)


def clip(value: int) -> int:
    return 0 if value < 0 else 255 if value > 255 else value


def yuv_pixel(y: int, u: int, v: int) -> tuple[int, int, int]:
    c = max(0, y - 16)
    d = u - 128
    e = v - 128
    return (
        clip((298 * c + 409 * e + 128) >> 8),
        clip((298 * c - 100 * d - 208 * e + 128) >> 8),
        clip((298 * c + 516 * d + 128) >> 8),
    )


def render_yuv422(data: bytes, width: int, height: int, stride: int,
                  order: str) -> bytes:
    if order not in {"UYVY", "YUYV", "VYUY", "YVYU"}:
        raise ValueError(f"unsupported YUV422 order: {order}")
    rgb = bytearray(width * height * 3)
    output = 0
    for row in range(height):
        source = row * stride
        for column in range(0, width, 2):
            a, b, c, d = data[source:source + 4]
            if order == "UYVY":
                u, y0, v, y1 = a, b, c, d
            elif order == "YUYV":
                y0, u, y1, v = a, b, c, d
            elif order == "VYUY":
                v, y0, u, y1 = a, b, c, d
            else:
                y0, v, y1, u = a, b, c, d
            rgb[output:output + 3] = bytes(yuv_pixel(y0, u, v))
            rgb[output + 3:output + 6] = bytes(yuv_pixel(y1, u, v))
            output += 6
            source += 4
    return bytes(rgb)


def render_rgb565(data: bytes, width: int, height: int,
                  byteorder: str) -> bytes:
    rgb = bytearray(width * height * 3)
    output = 0
    for offset in range(0, width * height * 2, 2):
        if byteorder == "little":
            pixel = data[offset] | data[offset + 1] << 8
        else:
            pixel = data[offset] << 8 | data[offset + 1]
        red = ((pixel >> 11) & 0x1F) * 255 // 31
        green = ((pixel >> 5) & 0x3F) * 255 // 63
        blue = (pixel & 0x1F) * 255 // 31
        rgb[output:output + 3] = bytes((red, green, blue))
        output += 3
    return bytes(rgb)


def write_ppm(path: Path, width: int, height: int, rgb: bytes) -> None:
    path.write_bytes(f"P6\n{width} {height}\n255\n".encode() + rgb)


def crop_rgb(rgb: bytes, source_width: int, x: int, y: int,
             width: int, height: int) -> bytes:
    source_stride = source_width * 3
    output_stride = width * 3
    output = bytearray(output_stride * height)
    for row in range(height):
        source = (y + row) * source_stride + x * 3
        target = row * output_stride
        output[target:target + output_stride] = \
            rgb[source:source + output_stride]
    return bytes(output)


def receive_frame(port: serial.Serial, timeout: float) -> tuple[dict, bytes, bytes]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = port.readline()
        marker = line.find(BEGIN)
        if marker < 0:
            continue
        header = line[marker:].decode("ascii", errors="strict").strip()
        fields = dict(re.findall(r"([a-z][a-z0-9_]*)=([^ ]+)", header))
        if fields.get("version") != "2":
            raise ValueError("unverified legacy dump; rebuild camera-test for protocol v2")
        width, height, stride = (int(fields[key]) for key in ("width", "height", "stride"))
        raw_bytes, rgb_bytes = (int(fields[key]) for key in ("raw", "rgb"))
        crop_x, crop_y = (int(fields[key]) for key in ("crop_x", "crop_y"))
        if (width != 800 or height != 600 or stride < width * 2 or
                stride > 4096 or raw_bytes != stride * height or
                rgb_bytes != 460 * 460 * 2 or
                not 0 <= crop_x <= width - 460 or
                not 0 <= crop_y <= height - 460):
            raise ValueError("invalid frame geometry or payload size")
        if fields["order"] not in {"UYVY", "YUYV", "VYUY", "YVYU", "RGB565X"}:
            raise ValueError("unsupported source format")
        if fields["rgb_order"] not in {"little", "big"}:
            raise ValueError("unsupported output byte order")
        payload_deadline = time.monotonic() + timeout
        # Drain both payloads before any rendering or disk I/O.
        raw = read_exact(port, raw_bytes, payload_deadline)
        converted = read_exact(port, rgb_bytes, payload_deadline)
        trailer = b"\nCAMERA_FRAME_DUMP_END status=PASS\n"
        actual_trailer = read_exact(port, len(trailer), payload_deadline)
        if actual_trailer != trailer:
            raise ValueError("invalid frame trailer (lost bytes or text conversion): "
                             + actual_trailer.hex())
        for label, payload in (("raw", raw), ("rgb", converted)):
            expected = int(fields[label + "_crc32"], 16)
            actual = binascii.crc32(payload) & 0xFFFFFFFF
            if actual != expected:
                raise ValueError(f"{label} CRC32 mismatch: {actual:08x} != {expected:08x}")
        return fields, raw, converted
    raise TimeoutError("camera dump header was not received")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=460800)
    parser.add_argument("--boot-baud", type=int, default=115_200)
    parser.add_argument("--timeout", type=float, default=40.0)
    parser.add_argument("--reset", action="store_true")
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()

    initial_baud = args.boot_baud if args.reset else args.baud
    with serial.Serial(args.port, initial_baud, timeout=0.25) as port:
        if args.reset:
            port.dtr = False
            port.rts = False
            port.reset_input_buffer()
            port.rts = True
            time.sleep(0.15)
            port.rts = False
            time.sleep(0.15)
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline:
                line = port.readline()
                if line:
                    print(line.decode("utf-8", errors="replace"), end="", flush=True)
                if b"CAMERA_FRAME_DUMP preparing " in line:
                    port.baudrate = args.baud
                    break
            else:
                raise TimeoutError("camera did not reach frame export; inspect boot log")
        fields, raw, converted = receive_frame(port, args.timeout)

    width, height, stride = (int(fields[key]) for key in ("width", "height", "stride"))
    crop_x, crop_y = (int(fields[key]) for key in ("crop_x", "crop_y"))
    order = fields["order"]
    if order == "RGB565X":
        packed = b"".join(raw[row * stride:row * stride + width * 2]
                          for row in range(height))
        source_rgb = render_rgb565(packed, width, height, "big")
    else:
        source_rgb = render_yuv422(raw, width, height, stride, order)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "source.bin").write_bytes(raw)
    (args.output_dir / "display-rgb565.bin").write_bytes(converted)
    (args.output_dir / "frame.txt").write_text(
        "\n".join(f"{key}={value}" for key, value in fields.items()) + "\n")
    write_ppm(args.output_dir / "source.ppm", width, height, source_rgb)
    write_ppm(args.output_dir / "source-center-crop.ppm", 460, 460,
              crop_rgb(source_rgb, width, crop_x, crop_y, 460, 460))
    write_ppm(args.output_dir / "display-rgb565.ppm", 460, 460,
              render_rgb565(converted, 460, 460, fields["rgb_order"]))
    print("CAPTURE_OK crc32=verified "
          f"raw={len(raw)} rgb={len(converted)} order={order} "
          f"source={width}x{height} crop={crop_x},{crop_y} "
          f"output=460x460 dir={args.output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
