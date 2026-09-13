#!/usr/bin/env python3
"""Extract per-language UI string JSONs from Nokia language-pack images.

Accepts plain PPM/full-flash images and WinTesla download packs. The
WinTesla ``nse8nx*`` format wraps the payload in 9-byte transfer records —
0x0B, 24-bit big-endian flash address, a check byte, 24-bit block length
(0x2000 except the final block), and a trailing check byte — before every
block; stripping them yields the contiguous PPM image, which is then decoded
by the standard extract_ppm_strings tooling.

Right-to-left languages (Hebrew, Arabic) are extracted from the packs but
deliberately NOT exported: the firmware has no RTL text pipeline, and
half-supporting them would ship a broken reading experience. They are
skipped by name here so the exclusion is a stated decision, not an
accident of the pack lineup.
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
VENDOR = HERE / "vendor"

RTL_LANGUAGES = {"HEBR", "ARAB"}
RECORD_TYPE = 0x0B
BLOCK_LEN = 0x2000


def be24(data: bytes, off: int) -> int:
    return (data[off] << 16) | (data[off + 1] << 8) | data[off + 2]


def deframe_wintesla(data: bytes) -> bytes | None:
    """Strip 9-byte transfer records; None if the file isn't in this format."""
    if len(data) < 9 or data[0] != RECORD_TYPE:
        return None
    payload = bytearray()
    pos = 0
    expect_addr = None
    while pos < len(data):
        if data[pos] != RECORD_TYPE or pos + 9 > len(data):
            return None
        addr = be24(data, pos + 1)
        length = be24(data, pos + 5)
        if expect_addr is not None and addr != expect_addr:
            return None
        if length <= 0 or pos + 9 + length > len(data):
            return None
        payload += data[pos + 9:pos + 9 + length]
        expect_addr = addr + length
        pos += 9 + length
    return bytes(payload)


def extract_pack(image: Path, outdir: Path) -> list[dict]:
    """Run the standard PPM string extraction; return language summaries."""
    data = image.read_bytes()
    deframed = deframe_wintesla(data)
    with tempfile.TemporaryDirectory() as td:
        if deframed is not None:
            src = Path(td) / (image.name + ".ppm")
            src.write_bytes(deframed)
        else:
            src = image
        workdir = Path(td) / "assets"
        subprocess.run(
            [sys.executable, str(VENDOR / "extract_ppm_strings.py"), str(src), str(workdir)],
            check=True, stdout=subprocess.DEVNULL)
        by_language = workdir / "ppm_strings" / "by_language"
        summaries = []
        for jf in sorted(by_language.glob("*.json")):
            entry = json.loads(jf.read_text(encoding="utf-8"))
            name = str(entry["name"])
            summary = {
                "name": name,
                "numeric_id": entry.get("numeric_id"),
                "records": len(entry.get("records", [])),
                "pack": image.name,
            }
            if name in RTL_LANGUAGES:
                summary["skipped"] = "RTL (no firmware RTL text pipeline)"
                summaries.append(summary)
                continue
            outdir.mkdir(parents=True, exist_ok=True)
            target = outdir / jf.name
            if target.exists():
                summary["skipped"] = "already extracted from an earlier image"
            else:
                shutil.copyfile(jf, target)
            summaries.append(summary)
        return summaries


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("images", type=Path, nargs="+",
                        help="full flash, PPM image, or WinTesla pack — "
                             "earlier images win for duplicate languages")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    all_rows = []
    for image in args.images:
        all_rows.extend(extract_pack(image, args.out))
    for row in all_rows:
        note = f"  [{row['skipped']}]" if "skipped" in row else ""
        print(f"{row['pack']}: {row['name']} id={row['numeric_id']} "
              f"records={row['records']}{note}")


if __name__ == "__main__":
    main()
