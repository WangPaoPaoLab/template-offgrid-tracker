#!/usr/bin/env python3
"""Rebuild a JPEG from the VC0706 camera test's RTT console output."""

import argparse
import re
from pathlib import Path


BEGIN = re.compile(r"^IMAGE_HEX_BEGIN length=(\d+)\b")
END = re.compile(r"^IMAGE_HEX_END length=(\d+) jpeg=(valid|invalid)\b")
HEX_LINE = re.compile(r"^[0-9A-Fa-f]+$")


def extract_jpeg(log_text: str) -> bytes:
    expected_length = None
    hex_lines: list[str] = []
    end_length = None
    end_status = None
    capturing = False

    for raw_line in log_text.splitlines():
        line = raw_line.strip()
        begin_match = BEGIN.match(line)
        if begin_match:
            if capturing:
                raise ValueError("found a second IMAGE_HEX_BEGIN before IMAGE_HEX_END")
            expected_length = int(begin_match.group(1))
            capturing = True
            continue
        if not capturing:
            continue
        end_match = END.match(line)
        if end_match:
            end_length = int(end_match.group(1))
            end_status = end_match.group(2)
            break
        if not HEX_LINE.fullmatch(line) or len(line) % 2:
            raise ValueError(f"invalid hex line inside image: {line!r}")
        hex_lines.append(line)

    if expected_length is None:
        raise ValueError("IMAGE_HEX_BEGIN was not found")
    if end_length is None:
        raise ValueError("IMAGE_HEX_END was not found")
    if end_status != "valid":
        raise ValueError("firmware reported an invalid JPEG marker sequence")
    if end_length != expected_length:
        raise ValueError(
            f"marker length mismatch: begin={expected_length}, end={end_length}"
        )

    image = bytes.fromhex("".join(hex_lines))
    if len(image) != expected_length:
        raise ValueError(
            f"image length mismatch: expected={expected_length}, decoded={len(image)}"
        )
    if not image.startswith(b"\xff\xd8") or not image.endswith(b"\xff\xd9"):
        raise ValueError("decoded data does not have JPEG SOI/EOI markers")
    return image


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("console_log", type=Path, help="saved RTT console output")
    parser.add_argument("output_jpeg", type=Path, help="JPEG file to create")
    args = parser.parse_args()

    image = extract_jpeg(args.console_log.read_text(encoding="utf-8"))
    args.output_jpeg.write_bytes(image)
    print(f"wrote {len(image)} bytes to {args.output_jpeg}")


if __name__ == "__main__":
    main()
