#!/usr/bin/env python3
"""Stage 0: derive every intermediate the emitters need from the .fls alone.

Reproduces the relevant parts of reverse_3210's run_extractors.sh in a
self-contained work directory:

  work/assets/bitmaps/3210600c/bitmaps/*.pbm   (pjmor export-dct3-bitmap)
  work/assets/fonts/3210600c_full/<style>/*.pbm (pjmor export-dct3-font)
  work/assets/ringtones/tone_data/*.tone.bin    (extract_ppm_chunks)
  work/assets/system_tones/raw/*_system.bin     (extract_system_tones)
  work/assets/ppm_strings/by_language/*.json    (extract_ppm_strings)
  work/firmware_assets/fonts/*.json             (build_firmware_assets)
  work/firmware_assets/bitmaps/3210600c_bitmaps.json
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
VENDOR = HERE / "vendor"


def run(cmd: list, cwd: Path | None = None, check: bool = True) -> None:
    print("+", " ".join(str(c) for c in cmd), f"(cwd={cwd})" if cwd else "")
    subprocess.run([str(c) for c in cmd], cwd=cwd, check=check,
                   stdout=subprocess.DEVNULL)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fls", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    args.fls = args.fls.resolve()
    args.work = args.work.resolve()

    work = args.work
    assets = work / "assets"
    assets.mkdir(parents=True, exist_ok=True)

    py = sys.executable
    bitmap_dir = assets / "bitmaps" / "3210600c"
    shutil.rmtree(bitmap_dir, ignore_errors=True)
    font_dir = assets / "fonts" / "3210600c_full"
    shutil.rmtree(font_dir, ignore_errors=True)
    run([py, HERE / "extract_dct3.py", args.fls,
         "--bitmaps", bitmap_dir / "bitmaps", "--fonts", font_dir])
    if not list((bitmap_dir / "bitmaps").glob("*.pbm")):
        raise SystemExit("bitmap extraction produced no PBM files")
    if not list(font_dir.glob("*/*.pbm")):
        raise SystemExit("font extraction produced no PBM files")

    # FS4 (ftest/plain) is not in the FONT chunk; a dedicated extractor reads
    # it from an embedded font record. It resolves the flash under
    # <root>/unpacked/..., so mirror that layout with a copy.
    fls_mirror = work / "unpacked/full_flash_600/Nokia3210_6.00/3210/3210600c/3210600c.fls"
    fls_mirror.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.fls, fls_mirror)
    run([py, VENDOR / "extract_embedded_ftest_font.py", "--root", work])

    run([py, VENDOR / "extract_ppm_chunks.py", args.fls, assets])
    run([py, VENDOR / "extract_system_tones.py", args.fls, assets])
    run([py, VENDOR / "extract_ppm_strings.py", args.fls, assets])
    run([py, VENDOR / "build_firmware_assets.py", work])

    for probe in (
        assets / "ringtones" / "tone_data",
        assets / "system_tones" / "raw",
        assets / "ppm_strings" / "by_language",
        work / "firmware_assets" / "fonts" / "3210600c_full_largebold.json",
        work / "firmware_assets" / "bitmaps" / "3210600c_bitmaps.json",
    ):
        if not probe.exists():
            raise SystemExit(f"stage-0 output missing: {probe}")
    print("stage-0 intermediates complete")


if __name__ == "__main__":
    main()
