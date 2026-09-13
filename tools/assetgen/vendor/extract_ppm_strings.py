#!/usr/bin/env python3
"""Decode Nokia DCT-3 PPM TEXT resources.

The PPM chunk dumps remain the preservation-grade source. This script produces
UTF-8 convenience exports for building a clone UI: per-language JSON, one CSV
table, and a NokiX-style text file.
"""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

from extract_ppm_chunks import parse_ppm, sanitize, sha256


def decode_lpcs_table(lpcs_payload: bytes | None) -> list[int]:
    if not lpcs_payload or len(lpcs_payload) < 16 + 512:
        return list(range(256))
    body = lpcs_payload[16:16 + 512]
    return [int.from_bytes(body[i:i + 2], "big") for i in range(0, len(body), 2)]


def decode_lpcs(raw: bytes, lpcs: list[int]) -> str:
    chars = []
    for byte in raw:
        codepoint = lpcs[byte] if byte < len(lpcs) else byte
        if 0xE000 <= codepoint <= 0xF8FF:
            chars.append(f"\\x{byte:02X}")
        else:
            chars.append(chr(codepoint))
    return "".join(chars).rstrip("\x00")


def decode_text(raw: bytes, flags: int, wide: bool, lpcs: list[int]) -> str:
    if raw.startswith(b"\x04\xff"):
        return raw[2:].decode("utf-16-be", "replace").rstrip("\x00")
    if wide:
        return raw.decode("utf-16-be", "replace").rstrip("\x00")
    if flags & 0x04:
        return raw.decode("utf-8", "replace").rstrip("\x00")
    return decode_lpcs(raw, lpcs)


def escaped(text: str) -> str:
    return (
        text.replace("\\", "\\\\")
        .replace("\n", "\\n")
        .replace("\t", "\\t")
        .replace("\x01", "\\b")
        .replace("\x0c", "\\f")
    )


def split_text_data(data: bytes, flags: int) -> tuple[list[int], int, bool]:
    candidates: list[tuple[int, bool]] = []
    max_count = min(4096, len(data))
    for count in range(1, max_count + 1):
        lengths = data[:count]
        total = sum(lengths)
        if count + total == len(data):
            candidates.append((count, False))
        if count + total * 2 == len(data):
            candidates.append((count, True))

    if not candidates:
        raise ValueError("could not split TEXT length table")

    preferred_wide = bool(flags & 0x80) and not bool(flags & 0x04)
    for count, wide in candidates:
        if wide == preferred_wide:
            return list(data[:count]), count, wide
    count, wide = candidates[0]
    return list(data[:count]), count, wide


def parse_text_subchunk(payload: bytes, lpcs: list[int]) -> dict[str, object]:
    flags = payload[12]
    data = payload[16:]
    lengths, table_size, wide = split_text_data(data, flags)
    unit = 2 if wide else 1
    cursor = table_size
    records = []

    for index, length in enumerate(lengths):
        raw = data[cursor:cursor + length * unit]
        cursor += length * unit
        text = decode_text(raw, flags, wide, lpcs)
        records.append(
            {
                "index": index,
                "length": length,
                "raw_hex": raw.hex(),
                "text": text,
                "escaped": escaped(text),
            }
        )

    return {
        "name": payload[8:12].rstrip(b"\x00 ").decode("latin1", "replace"),
        "numeric_id": int.from_bytes(payload[:4], "big"),
        "flags": flags,
        "wide_lengths": wide,
        "count": len(records),
        "records": records,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("firmware", type=Path)
    parser.add_argument("outdir", type=Path)
    args = parser.parse_args()

    data = args.firmware.read_bytes()
    _ppm_start, chunks = parse_ppm(data)

    lpcs_payload = None
    text_chunk = None
    for chunk in chunks:
        if chunk["type"] == "LPCS" and chunk["subchunks"]:
            lpcs_payload = chunk["subchunks"][0]["payload"]
        if chunk["type"] == "TEXT":
            text_chunk = chunk

    if text_chunk is None:
        raise ValueError("no TEXT chunk found")

    lpcs = decode_lpcs_table(lpcs_payload if isinstance(lpcs_payload, bytes) else None)
    strings_dir = args.outdir / "ppm_strings"
    by_language = strings_dir / "by_language"
    by_language.mkdir(parents=True, exist_ok=True)

    parsed = []
    for subchunk in text_chunk["subchunks"]:
        payload = subchunk["payload"]
        assert isinstance(payload, bytes)
        lang = parse_text_subchunk(payload, lpcs)
        lang["offset"] = subchunk["offset"]
        lang["subchunk_length"] = subchunk["length"]
        lang["sha256"] = sha256(payload)
        parsed.append(lang)

    comm = next((entry for entry in parsed if entry["name"] == "COMM"), None)
    comm_count = int(comm["count"]) if comm else 0
    languages = [entry for entry in parsed if entry["name"] != "COMM"]

    for entry in parsed:
        out = by_language / f"{sanitize(str(entry['name']))}.json"
        out.write_text(json.dumps(entry, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    csv_path = strings_dir / "all_strings.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=[
                "scope",
                "system_id",
                "language",
                "index",
                "text",
                "escaped",
                "raw_hex",
            ],
        )
        writer.writeheader()
        for entry in parsed:
            name = str(entry["name"])
            for record in entry["records"]:
                assert isinstance(record, dict)
                idx = int(record["index"])
                writer.writerow(
                    {
                        "scope": "common" if name == "COMM" else "language",
                        "system_id": idx if name == "COMM" else comm_count + idx,
                        "language": name,
                        "index": idx,
                        "text": record["text"],
                        "escaped": record["escaped"],
                        "raw_hex": record["raw_hex"],
                    }
                )

    nokix_path = strings_dir / "nokix_style_export.txt"
    with nokix_path.open("w", encoding="utf-8") as handle:
        handle.write("# PPM TEXT CHUNK\n")
        handle.write("# Exported from Nokia 3210 v6.00 resources\n\n")
        if comm:
            for record in comm["records"]:
                assert isinstance(record, dict)
                handle.write(f"@text system={record['index']}\n")
                handle.write(f"COMM: {record['escaped']}\n\n")
        max_count = max((int(entry["count"]) for entry in languages), default=0)
        for index in range(max_count):
            handle.write(f"@text system={comm_count + index}\n")
            for entry in languages:
                records = entry["records"]
                assert isinstance(records, list)
                if index >= len(records):
                    continue
                record = records[index]
                assert isinstance(record, dict)
                suffix = "\\0" if index == 0 else ""
                handle.write(f"{entry['name']}: {record['escaped']}{suffix}\n")
            handle.write("\n")

    manifest = {
        "source": str(args.firmware),
        "source_sha256": sha256(data),
        "comm_count": comm_count,
        "languages": [
            {
                "name": entry["name"],
                "numeric_id": entry["numeric_id"],
                "flags": entry["flags"],
                "count": entry["count"],
                "wide_lengths": entry["wide_lengths"],
                "sha256": entry["sha256"],
                "file": str((by_language / f"{sanitize(str(entry['name']))}.json").relative_to(args.outdir)),
            }
            for entry in parsed
        ],
        "csv": str(csv_path.relative_to(args.outdir)),
        "nokix_style_export": str(nokix_path.relative_to(args.outdir)),
    }
    (strings_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )

    print(f"decoded {sum(int(entry['count']) for entry in parsed)} TEXT strings across {len(parsed)} subchunks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
