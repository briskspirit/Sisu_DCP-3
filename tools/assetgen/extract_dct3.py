#!/usr/bin/env python3
"""Export DCT-3 system bitmaps and fonts from a Nokia flash image as PBMs.

Original-license-free reimplementation of the community DCT-3 extraction
knowledge, written for this project:

System bitmaps
  The Thumb bitmap-blit routine references three literal-pool pointers: an
  0xFF filler block, a bitmap list block, and the bitmap metadata block. The
  routine is located by a known instruction signature, the enclosing
  push/pop (0xB5/0xBD) delimits the literal scan, and each PC-relative LDR
  (opcode 0x48..0x4F) names a big-endian 32-bit CPU pointer; values >=
  0x200000 point into ROM. Metadata is an array of 12-byte records:
  BE32 pixel-data CPU address at +0 (0 = unused slot), width at +8, height
  at +9. Pixel data is column-major VLSB banks (8 rows per byte bank, top
  bank first). Some firmwares compress pixel data with a nibble RLE (a count
  byte whose high/low nibbles repeat the following two literal bytes); the
  routine contains a BEQ (0xD0) when RLE decoding is present.

Fonts (FONTfconv chunk)
  The chunk body starts 28 bytes after the ASCII tag "FONTfconv". Byte +3 is
  the style count; styles are 44-byte descriptors carrying the char-group
  index offset (BE16 at +14, relative +4), the group-count word (BE16 at
  +18; (n+1)*8 bytes of 8-byte groups), a trailing-group count (byte +17,
  +1), and a NUL-padded family/weight name at +28 (17 bytes). Each 8-byte
  char group maps a [first, last] BE16 codepoint range to a packed 24-bit
  cell locator (bytes +4..6) and a height nibble (byte +7 high nibble).
  Glyph cells live in per-width column-strip bitmaps described by 12-byte
  records that follow the last group table: BE32 strip data offset (relative
  to the width table, corrected by the record index), strip width at +7 and
  strip pixel height as BE32 at +8. The locator encodes the glyph's row
  within its width strip: subtracting the height low bits (locator mod 4)
  gives a base; a base whose quarter plus the first strip's width equals a
  strip's width starts that strip, and subsequent bases advance in 64-unit
  steps, with consecutive chars in a group stacking by glyph height.

Both exporters write ASCII P1 PBMs identical to the historical extraction
(header "P1\\n <w>\\n <h>\\n" followed by "<bit> " per pixel).
"""
from __future__ import annotations

import argparse
from pathlib import Path

CPU_BASE = 0x200000


def be16(data: bytes, off: int) -> int:
    return (data[off] << 8) | data[off + 1]


def be32(data: bytes, off: int) -> int:
    return (data[off] << 24) | (data[off + 1] << 16) | (data[off + 2] << 8) | data[off + 3]


def write_pbm(path: Path, width: int, height: int, bits) -> None:
    body = "".join(f"{bit} " for bit in bits)
    path.write_text(f"P1\n {width}\n {height}\n{body}", encoding="ascii")


def vlsb_bits(banked: bytes, width: int, height_banks: int):
    """Yield pixels row-band by row-band from column-major VLSB banks."""
    for bank_start in range(0, height_banks * width, width):
        for bit in range(8):
            for col in range(bank_start, bank_start + width):
                yield (banked[col] >> bit) & 1


# --------------------------------------------------------------------------
# System bitmaps
# --------------------------------------------------------------------------

BLIT_SIGNATURES = (b"\xd1\x07\x1c", b"\xd1\x06\x1c")


def locate_blit_routine(flash: bytes) -> int:
    for needle in BLIT_SIGNATURES:
        pos = flash.find(needle)
        while pos >= 0:
            before = flash[pos - 1]
            after = flash[pos + 3]
            if ((before, after) in ((0x0C, 0x01), (0x00, 0x28), (0x00, 0x29), (0x03, 0x20))
                    and flash[pos + 4] == 0x1C and flash[pos + 5] >= 0x41):
                return pos
            pos = flash.find(needle, pos + 1)
    raise SystemExit("bitmap blit routine signature not found")


def blit_pointers(flash: bytes, sig: int) -> tuple[list[int], bool]:
    """Collect the routine's ROM literal pointers and whether it RLE-decodes."""
    pointers: list[int] = []
    rle = False

    def visit(pos: int) -> None:
        nonlocal rle
        if flash[pos] == 0xD0:
            rle = True
        if 0x48 <= flash[pos] <= 0x4F:
            literal = pos + (flash[pos + 1] << 2) + 4 - (pos & 2)
            value = be32(flash, literal)
            if value >= CPU_BASE:
                pointers.append(value - CPU_BASE)

    # Scan byte-wise backwards to the push (0xB5) that opens the routine,
    # then forwards from past the signature to the pop (0xBD) that closes it.
    pos = sig
    while flash[pos] != 0xB5:
        visit(pos)
        pos -= 2
    pos = sig + 10
    while flash[pos] != 0xBD:
        visit(pos)
        pos += 2

    if len(pointers) < 2:
        raise SystemExit("bitmap blit routine yielded too few ROM pointers")
    if len(pointers) < 3:
        # Some firmwares don't reference the metadata block in the routine;
        # it is then reachable through a pointer stored just before the
        # 0xFF filler block.
        meta_ref = be32(flash, pointers[0] - 20) - CPU_BASE
        pointers.append(be32(flash, meta_ref - 4) - CPU_BASE)
    return pointers[:3], rle


def rle_expand(flash: bytes, start: int, want: int) -> bytes:
    out = bytearray()
    pos = start
    while len(out) < want:
        count, first, second = flash[pos], flash[pos + 1], flash[pos + 2]
        out += bytes([first]) * (count >> 4) + bytes([second]) * (count & 0x0F)
        pos += 3
    return bytes(out)


def export_bitmaps(flash: bytes, outdir: Path) -> int:
    sig = locate_blit_routine(flash)
    (ff_block, _list_block, meta_block), rle = blit_pointers(flash, sig)
    outdir.mkdir(parents=True, exist_ok=True)
    exported = 0
    for index in range((ff_block - meta_block) // 12):
        rec = meta_block + 12 * index
        data_addr = be32(flash, rec)
        if data_addr == 0:
            continue
        width = flash[rec + 8]
        height = flash[rec + 9]
        banks = (height + 7) // 8
        data_off = data_addr - CPU_BASE
        if rle:
            packed = rle_expand(flash, data_off, banks * width)
        else:
            packed = flash[data_off:data_off + banks * width]
        write_pbm(outdir / f"{index:04d}.pbm", width, height,
                  vlsb_bits(packed, width, banks))
        exported += 1
    return exported


# --------------------------------------------------------------------------
# Fonts
# --------------------------------------------------------------------------

FONT_TAG = b"FONTfconv"
STYLE_STRIDE = 44
GROUP_SIZE = 8
WIDTH_REC = 12


def export_fonts(flash: bytes, outroot: Path) -> int:
    tag = flash.find(FONT_TAG)
    if tag < 0:
        raise SystemExit("FONTfconv chunk not found")
    base = tag + 28
    style_count = flash[base + 3]

    styles = []  # (group_table_off, group_table_len, name)
    trailing_groups = 0
    for i in range(style_count):
        d = base + STYLE_STRIDE * i
        table_off = be16(flash, d + 14) + 4 + STYLE_STRIDE * i
        table_len = (be16(flash, d + 18) + 1) * GROUP_SIZE
        trailing_groups += flash[d + 17] + 1
        name = bytes(b for b in flash[d + 28:d + 28 + 17] if b).decode("ascii")
        styles.append((table_off, table_len, name))
        (outroot / name).mkdir(parents=True, exist_ok=True)

    width_table = base + styles[-1][0] + styles[-1][1] + trailing_groups * GROUP_SIZE
    strips = []  # (data_off, width, pixel_height)
    while True:
        rec = width_table + WIDTH_REC * len(strips)
        if flash[rec + 4] == 0 and flash[rec + 5] == 1 and flash[rec + 6] == 0:
            strips.append((be32(flash, rec) + WIDTH_REC * len(strips),
                           flash[rec + 7], be32(flash, rec + 8)))
        else:
            break

    # Decode each width strip into a pixel matrix once.
    matrices = []
    for data_off, width, pixel_height in strips:
        strip_len = (width * pixel_height) // 8
        banked = flash[width_table + data_off:width_table + data_off + strip_len]
        bits = list(vlsb_bits(banked, width, strip_len // width))
        rows = len(bits) // width
        matrices.append([bits[r * width:(r + 1) * width] for r in range(rows)])

    # Assign glyphs to strip rows. Locator bases are discovered across two
    # passes: pass one primes each strip's base locator, pass two places
    # every glyph relative to it (group order alone does not guarantee the
    # strip-opening group is seen before its followers).
    glyphs: dict[int, list[tuple[int, int, int, int]]] = {k: [] for k in range(len(strips))}
    first_base = [-1] * len(strips)
    exported = 0
    for _ in range(2):
        for k in glyphs:
            glyphs[k] = []
        placed = [0] * len(strips)
        for style_index, (table_off, table_len, _name) in enumerate(styles):
            for g in range(table_len // GROUP_SIZE):
                rec = base + table_off + GROUP_SIZE * g
                first_char = be16(flash, rec)
                last_char = be16(flash, rec + 2)
                locator = (flash[rec + 4] << 16) | (flash[rec + 5] << 8) | flash[rec + 6]
                h_low = locator % 4
                h_high = (flash[rec + 7] >> 4) & ~1
                height = ((h_low << 4) | h_high) // 2
                base_loc = locator - h_low
                for k, (_off, width, _ph) in enumerate(strips):
                    if base_loc // 4 + strips[0][1] == width:
                        row = 0
                        first_base[k] = base_loc
                    elif first_base[k] >= 0 and (base_loc - first_base[k]) % 64 == 0:
                        row = (base_loc - first_base[k]) // 64
                    else:
                        continue
                    for n in range(last_char - first_char + 1):
                        # Historical-extraction quirk kept for byte parity: a
                        # strip whose base locator is 0 never enables stacking
                        # (the original guard was `> 0`), so characters after
                        # the first in such a group fall back to row 0.
                        if n == 0:
                            glyph_row = row
                        elif first_base[k] > 0:
                            glyph_row = row + n * height
                        else:
                            glyph_row = 0
                        glyphs[k].append((first_char + n, style_index, height, glyph_row))
                    placed[k] += last_char - first_char + 1

    for k, (_off, width, _ph) in enumerate(strips):
        matrix = matrices[k]
        for codepoint, style_index, height, row in glyphs[k]:
            bits = [bit for r in range(row, row + height) for bit in matrix[r]]
            name = styles[style_index][2]
            write_pbm(outroot / name / f"{codepoint:04x}.pbm", width, height, bits)
            exported += 1
    return exported


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("flash", type=Path, help=".fls full flash or PPM image")
    parser.add_argument("--bitmaps", type=Path, help="output dir for system bitmap PBMs")
    parser.add_argument("--fonts", type=Path, help="output root for per-style font PBMs")
    args = parser.parse_args()
    flash = args.flash.read_bytes()
    if args.bitmaps:
        print(f"bitmaps: {export_bitmaps(flash, args.bitmaps)}")
    if args.fonts:
        print(f"font glyphs: {export_fonts(flash, args.fonts)}")


if __name__ == "__main__":
    main()
