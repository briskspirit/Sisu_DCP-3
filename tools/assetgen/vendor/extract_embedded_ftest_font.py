#!/usr/bin/env python3
"""Extract the embedded Nokia 3210 FS4 ftest/plain font.

FS0-FS3 are stored in the FONTfconv chunk. FS4 is different: the font
resolver fallback pointer at 0x00260010 points to an embedded font record
at 0x002e0a24 whose family/weight strings are "ftest/plain".
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
FLASH = ROOT / "unpacked/full_flash_600/Nokia3210_6.00/3210/3210600c/3210600c.fls"
OUT_DIR = ROOT / "assets/fonts/3210600c_full/ftestplain"
TRACE = ROOT / "assets/fonts/embedded_ftest_trace.json"

BASE = 0x00200000
FONT_RECORD = 0x002E0A24
DEFAULT_FONT_LITERAL = 0x00260010
GLYPH_INFO_HELPER = 0x0025E4FE
FONT_RESOLVE_HELPER = 0x0025FD48


def addr(value: int) -> str:
    return f"0x{value:08x}"


def off(address: int) -> int:
    return address - BASE


def u8(data: bytes, address: int) -> int:
    return data[off(address)]


def u16(data: bytes, address: int) -> int:
    return int.from_bytes(data[off(address) : off(address) + 2], "big")


def u32(data: bytes, address: int) -> int:
    return int.from_bytes(data[off(address) : off(address) + 4], "big")


def fixed_ascii(data: bytes, address: int, size: int) -> str:
    raw = data[off(address) : off(address) + size]
    return raw.split(b"\x00", 1)[0].decode("ascii", errors="replace")


def code_label(codepoint: int) -> str:
    return f"{codepoint:04x}"


def parse_char_range(data: bytes, address: int) -> dict[str, Any]:
    packed = u32(data, address + 4)
    metrics = u16(data, address + 6)
    return {
        "address": addr(address),
        "start": u16(data, address),
        "end": u16(data, address + 2),
        "start_hex": f"0x{u16(data, address):04x}",
        "end_hex": f"0x{u16(data, address + 2):04x}",
        "base_column_offset": packed >> 14,
        "shape_index": (u8(data, address + 6) >> 2) & 0x0F,
        "width": (metrics >> 5) & 0x1F,
        "advance": u8(data, address + 7) & 0x1F,
        "packed_hex": f"0x{packed:08x}",
    }


def parse_metric_range(data: bytes, address: int) -> dict[str, Any]:
    return {
        "address": addr(address),
        "start": u16(data, address),
        "end": u16(data, address + 2),
        "start_hex": f"0x{u16(data, address):04x}",
        "end_hex": f"0x{u16(data, address + 2):04x}",
        "byte4": u8(data, address + 4),
        "byte5": u8(data, address + 5),
        "packed_hex": data[off(address) : off(address) + 8].hex(),
    }


def glyph_rows(data: bytes, shape: dict[str, Any], char_range: dict[str, Any], codepoint: int) -> list[list[int]]:
    width = int(char_range["width"])
    height = int(shape["height"])
    stride = int(shape["stride"])
    column_offset = int(char_range["base_column_offset"]) + (codepoint - int(char_range["start"])) * width
    byte_index = column_offset >> 3
    shift = column_offset & 0x07
    bitmap_off = off(int(shape["bitmap"]))
    rows = [[0 for _x in range(width)] for _y in range(height)]

    for x in range(width):
        pos = bitmap_off + stride * byte_index + x
        column_bits = (((data[pos + stride] << 8) | data[pos]) >> shift) & 0xFF
        for y in range(height):
            rows[y][x] = 1 if (column_bits & (1 << y)) else 0
    return rows


def write_pbm(path: Path, rows: list[list[int]]) -> None:
    height = len(rows)
    width = len(rows[0]) if rows else 0
    values = " ".join(str(bit) for row in rows for bit in row)
    path.write_text(f"P1\n {width}\n {height}\n{values}\n", encoding="ascii")


def extract(root: Path) -> dict[str, Any]:
    data = (root / FLASH.relative_to(ROOT)).read_bytes()
    out_dir = root / OUT_DIR.relative_to(ROOT)
    out_dir.mkdir(parents=True, exist_ok=True)

    for stale in out_dir.glob("*.pbm"):
        stale.unlink()

    glyph_table = u32(data, FONT_RECORD)
    metric_table = u32(data, FONT_RECORD + 4)
    char_table = u32(data, FONT_RECORD + 8)
    metric_last_index = u16(data, FONT_RECORD + 0x0C)
    char_last_index = u16(data, FONT_RECORD + 0x0E)
    fallback_codepoint = u16(data, FONT_RECORD + 0x12)
    flags = u8(data, FONT_RECORD + 0x14)

    shape = {
        "address": addr(glyph_table),
        "bitmap": u32(data, glyph_table),
        "bitmap_hex": addr(u32(data, glyph_table)),
        "stride": u16(data, glyph_table + 6),
        "height": u8(data, glyph_table + 7),
        "raw_hex": data[off(glyph_table) : off(glyph_table) + 12].hex(),
    }
    metric_ranges = [parse_metric_range(data, metric_table + index * 8) for index in range(metric_last_index + 1)]
    char_ranges = [parse_char_range(data, char_table + index * 8) for index in range(char_last_index + 1)]

    glyphs = []
    for char_range in char_ranges:
        for codepoint in range(int(char_range["start"]), int(char_range["end"]) + 1):
            rows = glyph_rows(data, shape, char_range, codepoint)
            path = out_dir / f"{code_label(codepoint)}.pbm"
            write_pbm(path, rows)
            glyphs.append(
                {
                    "codepoint": codepoint,
                    "codepoint_hex": f"0x{codepoint:04x}",
                    "path": str(path.relative_to(root)),
                    "width": int(char_range["width"]),
                    "height": int(shape["height"]),
                    "advance": int(char_range["advance"]),
                    "source_range": char_range["address"],
                }
            )

    literal_value = u32(data, DEFAULT_FONT_LITERAL)
    report = {
        "status": "embedded FS4 ftest/plain extracted",
        "font_record": addr(FONT_RECORD),
        "font_resolver_default_literal": addr(DEFAULT_FONT_LITERAL),
        "font_resolver_default_value": addr(literal_value),
        "font_resolver": addr(FONT_RESOLVE_HELPER),
        "glyph_info_helper": addr(GLYPH_INFO_HELPER),
        "family": fixed_ascii(data, FONT_RECORD + 0x18, 11),
        "weight": fixed_ascii(data, FONT_RECORD + 0x23, 6),
        "flags_byte_0x14": f"0x{flags:02x}",
        "fallback_codepoint": f"0x{fallback_codepoint:04x}",
        "glyph_table": addr(glyph_table),
        "metric_table": addr(metric_table),
        "char_table": addr(char_table),
        "shape": shape,
        "metric_ranges": metric_ranges,
        "char_ranges": char_ranges,
        "glyph_count": len(glyphs),
        "glyphs": glyphs,
        "decoder": (
            "0x0025e4fe resolves a character range, computes base_column_offset + "
            "(codepoint - range_start) * width, then reads columns from shape.bitmap "
            "using shape.stride and the low three bits as the bit shift."
        ),
    }
    trace_path = root / TRACE.relative_to(ROOT)
    trace_path.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args()
    root = args.root.resolve()
    report = extract(root)
    print(f"Extracted {report['glyph_count']} glyphs to {root / OUT_DIR.relative_to(ROOT)}")
    print(f"Wrote {root / TRACE.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
