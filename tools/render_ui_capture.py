#!/usr/bin/env python3
"""Render complete `ui dump` frames from a CDC log (requires Pillow)."""

import argparse
import re
from pathlib import Path


def read_frames(text):
    frames = []
    frame = None
    label = ""
    for line in text.splitlines():
        if line.startswith("[ui] route="):
            label = line
            frame = bytearray()
        match = re.fullmatch(r"\[fb\] (\d+):([0-9a-f]+)", line)
        if match and frame is not None:
            if len(match[2]) % 2:
                frame = None
                continue
            data = bytes.fromhex(match[2])
            if int(match[1]) != len(frame) or len(frame) + len(data) > 504:
                frame = None
                continue
            frame.extend(data)
            if len(frame) == 504:
                frames.append((label, bytes(frame)))
                frame = None
    return frames


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    frames = read_frames(args.log.read_text(errors="replace"))
    if not frames:
        parser.error("no complete framebuffer captures in log")

    from PIL import Image, ImageDraw

    sheet = Image.new("RGB", (720, 250 * len(frames)), "white")
    draw = ImageDraw.Draw(sheet)
    for index, (label, data) in enumerate(frames):
        image = Image.new("RGB", (84, 48))
        image.putdata([
            (28, 39, 19) if data[(y // 8) * 84 + x] & (1 << (y % 8))
            else (183, 201, 145)
            for y in range(48) for x in range(84)
        ])
        draw.text((10, index * 250 + 5), label, fill="black")
        sheet.paste(image.resize((336, 192), Image.Resampling.NEAREST),
                    (10, index * 250 + 30))
    sheet.save(args.output)
    print(args.output)


if __name__ == "__main__":
    main()
