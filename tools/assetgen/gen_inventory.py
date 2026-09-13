#!/usr/bin/env python3
"""Materialize the full per-language asset inventory from every source image.

Beyond the shipped-set generation, this collects everything a language needs
so adding one later is a data decision, not an archaeology project:

  inventory/t9/<LANG>.ldb        every Tegic T9 dictionary found anywhere
                                 (full flash, WinTesla packs, the v7.00 T9
                                 files inside the source archive)
  inventory/fonts/<set>/<style>/ per-pack font PBMs for every UNIQUE
                                 FONTfconv chunk (regional packs ship
                                 different glyph sets, e.g. APAC scripts)
  inventory/manifest.json        what came from where, with hashes

RTL text (HEBR/ARAB) is excluded upstream by extract_text_packs; their T9
LDBs do not exist in these packs, so no skip is needed here.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE / "vendor"))

from extract_text_packs import deframe_wintesla  # noqa: E402
import extract_dct3  # noqa: E402

TEGIC_SIGNATURE = b"Copyright \xa9 1999 Tegic"
FONT_TAG = b"FONTfconv"

# The canonical v6.00 archive also carries one later Czech-T9 PPM image. The
# outer archive may be repackaged, so the donor member itself is the invariant.
# Consume exactly this member instead of trusting every similarly named file
# beside an independently verified 3210600c.fls.
ARCHIVE_T9_MEMBERS = {
    "0700NSE8.B":
        "05a80573bdd7a94951d20a474da1d9153182913c3730366d9a24946ddcb93137",
}


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_verified_archive_t9_images(archive: Path) -> list[tuple[str, bytes]]:
    if not archive.exists() or not zipfile.is_zipfile(archive):
        return []

    images: list[tuple[str, bytes]] = []
    with zipfile.ZipFile(archive) as zf:
        names = zf.namelist()
        for member_name, expected_hash in ARCHIVE_T9_MEMBERS.items():
            matches = [name for name in names
                       if Path(name).name == member_name]
            if len(matches) > 1:
                raise SystemExit(
                    f"gen_inventory.py: {archive} contains multiple members "
                    f"named {member_name}; refusing an ambiguous T9 donor")
            if not matches:
                continue
            name = matches[0]
            data = zf.read(name)
            actual_hash = sha256(data)
            if actual_hash != expected_hash:
                raise SystemExit(
                    f"gen_inventory.py: {archive}!{name} does not match the "
                    f"known Czech T9 member (want SHA-256 {expected_hash}); "
                    "refusing to extract dictionaries from it")
            images.append((Path(name).name, data))
    return images


def carve_t9(name: str, data: bytes, outdir: Path, manifest: list) -> None:
    pos = 0
    while True:
        pos = data.find(TEGIC_SIGNATURE, pos + 1)
        if pos < 0:
            break
        rec = pos - 16
        lang = data[rec + 8:rec + 12].rstrip(b"\x00").decode("ascii", "replace")
        length = int.from_bytes(data[rec + 4:rec + 8], "big")
        payload = data[rec:rec + length]
        if len(payload) != length:
            continue
        digest = sha256(payload)
        target = outdir / f"{lang}.ldb"
        reused = False
        if target.exists():
            if sha256(target.read_bytes()) != digest:
                # same language, different build (e.g. the v7.00 GERM differs
                # from the v6.00 one): keep both, disambiguated by hash
                target = outdir / f"{lang}.{digest[:8]}.ldb"
                reused = target.exists()
            else:
                reused = True
        if not reused:
            outdir.mkdir(parents=True, exist_ok=True)
            target.write_bytes(payload)
        # The manifest always describes the full inventory, whether this run
        # wrote the file or an earlier one did — a rerun over an existing
        # directory must not report an empty inventory.
        manifest.append({"kind": "t9", "language": lang, "bytes": length,
                         "sha256": digest, "source": name,
                         "file": str(target.name), "reused": reused})


def font_chunk_digest(data: bytes) -> str | None:
    tag = data.find(FONT_TAG)
    if tag < 0:
        return None
    # Hash a fixed window: the chunk length field precedes the tag by 4 bytes
    length = int.from_bytes(data[tag - 4:tag], "big")
    if not 0 < length < len(data):
        length = 0x40000
    return sha256(data[tag:tag + length])


def extract_pack_fonts(name: str, data: bytes, outroot: Path,
                       seen: dict[str, str], manifest: list) -> None:
    digest = font_chunk_digest(data)
    if digest is None:
        return
    if digest in seen:
        manifest.append({"kind": "fonts", "source": name,
                         "same_as": seen[digest]})
        return
    seen[digest] = name
    outdir = outroot / name
    glyphs = extract_dct3.export_fonts(data, outdir)
    styles = sorted(p.name for p in outdir.iterdir() if p.is_dir())
    manifest.append({"kind": "fonts", "source": name, "glyphs": glyphs,
                     "styles": styles, "font_chunk_sha256": digest})


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fls", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True,
                        help="nokia_3210_6.00.zip (for the v7.00 T9 members)")
    parser.add_argument("--packs", type=Path, nargs="*", default=[])
    parser.add_argument("--t9-donors", type=Path, nargs="*", default=[],
                        help="donor firmware installers (other DCT-3 models) "
                             "carved for T9 dictionaries ONLY -- their fonts/"
                             "strings belong to other hardware")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    manifest: list = []
    t9_dir = args.out / "t9"
    fonts_dir = args.out / "fonts"
    seen_fonts: dict[str, str] = {}

    images: list[tuple[str, bytes]] = [(args.fls.name, args.fls.read_bytes())]
    for pack in args.packs:
        raw = pack.read_bytes()
        images.append((pack.name, deframe_wintesla(raw) or raw))
    images.extend(load_verified_archive_t9_images(args.archive))

    for name, data in images:
        carve_t9(name, data, t9_dir, manifest)
        extract_pack_fonts(name, data, fonts_dir, seen_fonts, manifest)

    # Donor installers: T9 carve only. Their font/string chunks are for other
    # models' hardware and must never enter this phone's asset inventory.
    for donor in args.t9_donors:
        with tempfile.TemporaryDirectory() as td:
            subprocess.run(
                [sys.executable,
                 str(HERE / "vendor" / "extract_freeman_installer.py"),
                 str(donor), td],
                check=True, stdout=subprocess.DEVNULL)
            for member in sorted(Path(td).iterdir()):
                if not member.is_file() or member.stat().st_size < 0x30000:
                    continue
                raw = member.read_bytes()
                carve_t9(f"{donor.name}!{member.name}",
                         deframe_wintesla(raw) or raw, t9_dir, manifest)

    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    t9_langs = sorted(p.stem.split(".")[0] for p in t9_dir.glob("*.ldb")) if t9_dir.exists() else []
    print(f"inventory: {len(t9_langs)} T9 dictionaries ({' '.join(dict.fromkeys(t9_langs))}), "
          f"{len(seen_fonts)} unique font sets")


if __name__ == "__main__":
    main()
