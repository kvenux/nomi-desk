import argparse
import re
import struct
import time
from pathlib import Path

import serial


HEADER_RE = re.compile(rb"SCREENSHOT_START:(\d+):(\d+):(\d+)\n")


def write_bmp(path: Path, fb: bytes, width: int, height: int) -> None:
    # Match CrossPoint's ScreenshotUtil: rotate 90 degrees counter-clockwise
    # from the physical framebuffer into the portrait image.
    out_width = height
    out_height = width
    row_size = ((out_width + 31) // 32) * 4
    image_size = row_size * out_height
    file_size = 62 + image_size

    header = bytearray()
    header += b"BM"
    header += struct.pack("<IHHI", file_size, 0, 0, 62)
    header += struct.pack("<IiiHHIIiiII", 40, out_width, out_height, 1, 1, 0, image_size, 2835, 2835, 2, 2)
    header += bytes([0, 0, 0, 0, 255, 255, 255, 0])

    width_bytes = width // 8
    rows = bytearray()
    for out_y in range(out_height):
        row = bytearray(row_size)
        for out_x in range(out_width):
            src_x = width - 1 - out_y
            src_y = out_width - 1 - out_x
            fb_index = src_y * width_bytes + (src_x // 8)
            pixel = (fb[fb_index] >> (7 - (src_x % 8))) & 0x01
            row[out_x // 8] |= pixel << (7 - (out_x % 8))
        rows += row

    path.write_bytes(header + rows)


def read_until_header(ser: serial.Serial, timeout_s: float) -> tuple[int, int, int]:
    deadline = time.time() + timeout_s
    buf = bytearray()
    while time.time() < deadline:
        chunk = ser.read(1)
        if not chunk:
            continue
        buf += chunk
        match = HEADER_RE.search(buf)
        if match:
            return int(match.group(1)), int(match.group(2)), int(match.group(3))
        if len(buf) > 512:
            buf = buf[-512:]
    raise TimeoutError("SCREENSHOT_START header not received")


def main() -> None:
    parser = argparse.ArgumentParser(description="Capture XTEINK framebuffer over USB serial as BMP.")
    parser.add_argument("--port", default="COM9")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--out", default="xteink-screenshot.bmp")
    args = parser.parse_args()

    out = Path(args.out)
    with serial.Serial(args.port, args.baud, timeout=0.5) as ser:
        ser.reset_input_buffer()
        ser.write(b"SCREENSHOT\n")
        ser.flush()
        size, width, height = read_until_header(ser, 5)
        fb = ser.read(size)
        if len(fb) != size:
            raise RuntimeError(f"Expected {size} framebuffer bytes, got {len(fb)}")
        tail = ser.read(len(b"\nSCREENSHOT_END\n"))
        if b"SCREENSHOT_END" not in tail:
            raise RuntimeError("Screenshot footer not received")

    write_bmp(out, fb, width, height)
    print(f"Saved {out} ({width}x{height} framebuffer, {size} bytes)")


if __name__ == "__main__":
    main()
