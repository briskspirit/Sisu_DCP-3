#!/usr/bin/env python3
"""Build firmware-friendly atlases and PNG previews from extracted PBMs."""

from __future__ import annotations

import argparse
import json
import math
import re
import struct
import zlib
from pathlib import Path


RGBA_TRANSPARENT = (0, 0, 0, 0)
RGBA_BLACK = (24, 28, 24, 255)
RGBA_DARK = (52, 62, 52, 255)
RGBA_LCD = (190, 205, 176, 255)
RGBA_GRID = (144, 158, 132, 255)

TINY_FONT = {
    "0": ("111", "101", "101", "101", "111"),
    "1": ("010", "110", "010", "010", "111"),
    "2": ("111", "001", "111", "100", "111"),
    "3": ("111", "001", "111", "001", "111"),
    "4": ("101", "101", "111", "001", "001"),
    "5": ("111", "100", "111", "001", "111"),
    "6": ("111", "100", "111", "101", "111"),
    "7": ("111", "001", "010", "010", "010"),
    "8": ("111", "101", "111", "101", "111"),
    "9": ("111", "101", "111", "001", "111"),
    "A": ("010", "101", "111", "101", "101"),
    "B": ("110", "101", "110", "101", "110"),
    "C": ("111", "100", "100", "100", "111"),
    "D": ("110", "101", "101", "101", "110"),
    "E": ("111", "100", "110", "100", "111"),
    "F": ("111", "100", "110", "100", "100"),
    "_": ("000", "000", "000", "000", "111"),
    "-": ("000", "000", "111", "000", "000"),
    ".": ("000", "000", "000", "000", "010"),
}


def sanitize(text: str) -> str:
    text = re.sub(r"[^A-Za-z0-9._-]+", "_", text).strip("._-")
    return text or "unnamed"


def read_pbm(path: Path) -> dict[str, object]:
    raw = path.read_text(encoding="ascii", errors="ignore").splitlines()
    tokens: list[str] = []
    for line in raw:
        line = line.split("#", 1)[0]
        tokens.extend(line.split())
    if len(tokens) < 3 or tokens[0] != "P1":
        raise ValueError(f"{path} is not an ASCII P1 PBM")
    width = int(tokens[1])
    height = int(tokens[2])
    values = [1 if tok == "1" else 0 for tok in tokens[3:3 + width * height]]
    if len(values) != width * height:
        raise ValueError(f"{path} pixel count mismatch")
    pixels = [values[y * width:(y + 1) * width] for y in range(height)]
    return {"path": path, "width": width, "height": height, "pixels": pixels}


def make_canvas(width: int, height: int, fill: tuple[int, int, int, int] = RGBA_TRANSPARENT) -> bytearray:
    return bytearray(fill * (width * height))


def set_px(canvas: bytearray, width: int, height: int, x: int, y: int, color: tuple[int, int, int, int]) -> None:
    if 0 <= x < width and 0 <= y < height:
        offset = (y * width + x) * 4
        canvas[offset:offset + 4] = bytes(color)


def blit_pbm(
    canvas: bytearray,
    canvas_width: int,
    canvas_height: int,
    pbm: dict[str, object],
    x: int,
    y: int,
    scale: int = 1,
    color: tuple[int, int, int, int] = RGBA_BLACK,
) -> None:
    pixels = pbm["pixels"]
    assert isinstance(pixels, list)
    for py, row in enumerate(pixels):
        assert isinstance(row, list)
        for px, bit in enumerate(row):
            if bit:
                for sy in range(scale):
                    for sx in range(scale):
                        set_px(canvas, canvas_width, canvas_height, x + px * scale + sx, y + py * scale + sy, color)


def draw_tiny_text(canvas: bytearray, width: int, height: int, x: int, y: int, text: str, color=RGBA_DARK) -> None:
    cx = x
    for ch in text.upper():
        glyph = TINY_FONT.get(ch)
        if glyph is None:
            cx += 4
            continue
        for gy, row in enumerate(glyph):
            for gx, bit in enumerate(row):
                if bit == "1":
                    set_px(canvas, width, height, cx + gx, y + gy, color)
        cx += 4


def write_png(path: Path, width: int, height: int, rgba: bytearray) -> None:
    rows = []
    stride = width * 4
    for y in range(height):
        rows.append(b"\x00" + bytes(rgba[y * stride:(y + 1) * stride]))
    raw = b"".join(rows)

    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload))
            + kind
            + payload
            + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
        )

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


def pack_vertical_lsb(pbm: dict[str, object]) -> str:
    width = int(pbm["width"])
    height = int(pbm["height"])
    pixels = pbm["pixels"]
    assert isinstance(pixels, list)
    data = bytearray()
    for page in range(math.ceil(height / 8)):
        for x in range(width):
            byte = 0
            for bit in range(8):
                y = page * 8 + bit
                if y < height and pixels[y][x]:
                    byte |= 1 << bit
            data.append(byte)
    return data.hex()


def shelf_pack(items: list[dict[str, object]], max_width: int = 1024, padding: int = 1) -> tuple[int, int, list[dict[str, object]]]:
    x = padding
    y = padding
    row_h = 0
    used_w = 0
    placements = []
    for item in items:
        w = int(item["width"])
        h = int(item["height"])
        if x + w + padding > max_width and x > padding:
            x = padding
            y += row_h + padding
            row_h = 0
        placed = dict(item)
        placed["x"] = x
        placed["y"] = y
        placements.append(placed)
        x += w + padding
        row_h = max(row_h, h)
        used_w = max(used_w, x)
    return max(1, min(max_width, used_w + padding)), y + row_h + padding, placements


def write_atlas(pbm_paths: list[Path], out_png: Path, out_json: Path, root: Path, atlas_name: str) -> dict[str, object]:
    items = []
    for path in sorted(pbm_paths):
        pbm = read_pbm(path)
        code = path.stem
        try:
            codepoint = int(code, 16)
        except ValueError:
            codepoint = None
        items.append(
            {
                "id": code,
                "codepoint": codepoint,
                "width": pbm["width"],
                "height": pbm["height"],
                "source": str(path.relative_to(root)),
                "pixels": pbm["pixels"],
                "packed_vertical_lsb_hex": pack_vertical_lsb(pbm),
            }
        )

    width, height, placements = shelf_pack(items, max_width=1024, padding=1)
    canvas = make_canvas(width, height)
    glyphs = []
    for item in placements:
        pbm = {
            "width": item["width"],
            "height": item["height"],
            "pixels": item["pixels"],
        }
        blit_pbm(canvas, width, height, pbm, int(item["x"]), int(item["y"]))
        glyphs.append({k: v for k, v in item.items() if k != "pixels"})
    write_png(out_png, width, height, canvas)

    manifest = {
        "name": atlas_name,
        "image": str(out_png.relative_to(root)),
        "width": width,
        "height": height,
        "format": "1bpp vertical_lsb",
        "glyph_count": len(glyphs),
        "glyphs": glyphs,
    }
    out_json.parent.mkdir(parents=True, exist_ok=True)
    out_json.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return manifest


def write_contact_sheet(pbm_paths: list[Path], out_png: Path, title: str, scale: int = 3, columns: int = 16) -> dict[str, object]:
    pbms = [(path, read_pbm(path)) for path in sorted(pbm_paths)]
    if not pbms:
        return {"image": str(out_png), "count": 0}
    max_w = max(int(pbm["width"]) for _path, pbm in pbms)
    max_h = max(int(pbm["height"]) for _path, pbm in pbms)
    label_h = 7
    pad = 5
    cell_w = max_w * scale + pad * 2
    cell_h = max_h * scale + label_h + pad * 3
    rows = math.ceil(len(pbms) / columns)
    width = columns * cell_w
    height = rows * cell_h
    canvas = make_canvas(width, height, RGBA_LCD)

    for index, (path, pbm) in enumerate(pbms):
        col = index % columns
        row = index // columns
        x0 = col * cell_w
        y0 = row * cell_h
        for x in range(x0, x0 + cell_w):
            set_px(canvas, width, height, x, y0, RGBA_GRID)
        for y in range(y0, y0 + cell_h):
            set_px(canvas, width, height, x0, y, RGBA_GRID)
        image_x = x0 + (cell_w - int(pbm["width"]) * scale) // 2
        image_y = y0 + pad
        blit_pbm(canvas, width, height, pbm, image_x, image_y, scale=scale)
        draw_tiny_text(canvas, width, height, x0 + pad, y0 + cell_h - label_h - 2, path.stem[:8])

    write_png(out_png, width, height, canvas)
    return {"title": title, "image": str(out_png), "count": len(pbms), "columns": columns, "scale": scale}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    args = parser.parse_args()
    root = args.root.resolve()

    assets = root / "assets"
    firmware = root / "firmware_assets"
    previews = assets / "previews"
    firmware.mkdir(exist_ok=True)

    preview_records = []
    bitmap_paths = sorted((assets / "bitmaps").glob("3210600c/bitmaps/*.pbm"))
    if bitmap_paths:
        preview_records.append(
            write_contact_sheet(bitmap_paths, previews / "bitmaps_3210600c_contact.png", "3210 system bitmaps", scale=4)
        )
        write_atlas(
            bitmap_paths,
            firmware / "bitmaps" / "3210600c_bitmaps.png",
            firmware / "bitmaps" / "3210600c_bitmaps.json",
            root,
            "3210600c_bitmaps",
        )

    font_records = []
    for style_dir in sorted((assets / "fonts").glob("*/*")):
        if not style_dir.is_dir():
            continue
        pbm_paths = sorted(style_dir.glob("*.pbm"))
        if not pbm_paths:
            continue
        source = style_dir.parent.name
        style = style_dir.name
        name = sanitize(f"{source}_{style}")
        atlas = write_atlas(
            pbm_paths,
            firmware / "fonts" / f"{name}.png",
            firmware / "fonts" / f"{name}.json",
            root,
            name,
        )
        font_records.append(
            {
                "source": source,
                "style": style,
                "json": str((firmware / "fonts" / f"{name}.json").relative_to(root)),
                "image": atlas["image"],
                "glyph_count": atlas["glyph_count"],
            }
        )
        if source == "3210600c_full":
            preview_records.append(
                write_contact_sheet(pbm_paths, previews / f"font_{name}_contact.png", name, scale=4, columns=20)
            )

    manifest = {
        "bitmaps": {
            "json": "firmware_assets/bitmaps/3210600c_bitmaps.json" if bitmap_paths else "",
            "image": "firmware_assets/bitmaps/3210600c_bitmaps.png" if bitmap_paths else "",
            "count": len(bitmap_paths),
        },
        "fonts": font_records,
        "previews": [
            {
                **record,
                "image": str(Path(record["image"]).relative_to(root)) if Path(record["image"]).is_absolute() else record["image"],
            }
            for record in preview_records
        ],
    }
    (firmware / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"built {len(font_records)} font atlases and {len(bitmap_paths)} bitmap records")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
