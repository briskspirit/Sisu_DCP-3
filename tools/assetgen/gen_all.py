#!/usr/bin/env python3
"""Regenerate every src/generated + include/generated file from the original
NSE-8 v6.00 firmware archive and verify the outputs byte-exact against the
working tree's previously generated copies. Those copies are build products
-- they are never committed -- so on a fresh checkout there is nothing to
compare yet: run a normal build once (it generates them), then rerun this to
verify drift.

Input resolution (nobody gets locked out by packaging):
  - a bare 3210600c.fls          -> verified by SHA-256
  - nokia_3210_6.00.zip          -> member */3210600c.fls verified by SHA-256
  - any zip containing nokia_3210_6.00.zip (e.g. archive.org's 24 GB
    nokia_firmwares.zip)         -> nested member resolved, then as above

The inner .fls hash is the hard invariant; outer-container hashes are
advisory only.
"""
from __future__ import annotations

import argparse
import filecmp
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import uuid
import zipfile
from pathlib import Path

# Stale-bytecode immunity: .pyc validation keys on source mtime+size only, so
# a same-size edit restored to the same mtime (cp/patch/git can all do this)
# makes Python execute OLD code while every source hash reads NEW — silently
# corrupting generation and the input stamp. Route this process AND every
# generator subprocess to a per-run empty cache prefix; with writing disabled
# it never exists, so imports always compile the actual source.
sys.dont_write_bytecode = True
sys.pycache_prefix = os.path.join(
    tempfile.gettempdir(), f"sisu-assetgen-pyc-{os.getpid()}-{uuid.uuid4().hex}")
FRESH_PYCACHE_ENV = dict(os.environ, PYTHONDONTWRITEBYTECODE="1",
                         PYTHONPYCACHEPREFIX=sys.pycache_prefix)

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent

FLS_SHA256 = "0ab99ed809232d2c14c6587c9fc904323358f223e978b6215d15475a62362f6c"
ZIP_SHA256 = "a635b2c6675077702283fc24f8a160ce6a9a5f77ba1b1c24848ee2ce2092b673"
INSTALLER_SHA256 = "00550789e3870c2b32cbaed2b8e9ff17f121145394e84fafece94fc7963e2c01"
FLS_SIZE = 2 * 1024 * 1024


def verify_installer(path: Path) -> None:
    """The optional WinTesla installer is hash-gated like every other input: a
    stray file with the right name must never feed the language extraction."""
    if sha256(path.read_bytes()) != INSTALLER_SHA256:
        raise SystemExit(
            f"[assets] {path} does not match the known WinTesla installer "
            f"(want SHA-256 {INSTALLER_SHA256}); refusing to extract language "
            f"packs from it")


# Optional T9 donor packages: same-generation DCT-3 firmware whose language
# packs carry v7-layout Tegic dictionaries the 3210 packages never shipped.
# T9 ONLY -- fonts/strings inside are for other models and are never used.
# NPE3_527.exe is the Nokia 6210 v5.27 sales package (publicly archived in
# the archive.org "Nokia_DCT3_firmwares" item); it provides POLI and TURK
# (its CZEC matches the canonical archive's own t9_cz dictionary).
T9_DONORS = {
    "NPE3_527.exe":
        "d2a8197580f4194e70af16bffaa20eca806bfcf240561fa1368cad4b6d176f90",
}


def find_t9_donors(dirs: list[Path]) -> list[Path]:
    """Hash-verified donor packages present in the given directories."""
    found = []
    for d in dirs:
        for name, digest in T9_DONORS.items():
            candidate = d / name
            if candidate.is_file():
                if sha256(candidate.read_bytes()) != digest:
                    raise SystemExit(
                        f"[assets] {candidate} does not match the known T9 "
                        f"donor package (want SHA-256 {digest}); refusing to "
                        f"extract dictionaries from it")
                found.append(candidate)
    return found


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def resolve_fls(source: Path) -> bytes:
    """Return the verified 3210600c.fls bytes from whatever `source` is."""
    if source.name.endswith(".fls") or (source.stat().st_size == FLS_SIZE and not zipfile.is_zipfile(source)):
        data = source.read_bytes()
        if sha256(data) != FLS_SHA256:
            raise SystemExit(f"{source}: not the v6.00 flash (SHA-256 mismatch)")
        print(f"[ok] bare .fls verified: {source}")
        return data
    if not zipfile.is_zipfile(source):
        raise SystemExit(f"{source}: neither a .fls nor a zip archive")

    with zipfile.ZipFile(source) as zf:
        outer_hash = sha256(source.read_bytes()) if source.stat().st_size < 64 * 1024 * 1024 else None
        if outer_hash == ZIP_SHA256:
            print(f"[ok] canonical nokia_3210_6.00.zip verified: {source}")
        # direct member?
        for name in zf.namelist():
            if name.endswith("3210600c.fls"):
                data = zf.read(name)
                if sha256(data) == FLS_SHA256:
                    print(f"[ok] inner .fls verified: {source}!{name}")
                    return data
                raise SystemExit(f"{source}!{name}: SHA-256 mismatch")
        # nested nokia_3210_6.00.zip (collection archive)?
        for name in zf.namelist():
            if name.endswith("nokia_3210_6.00.zip"):
                inner = zf.read(name)
                if sha256(inner) != ZIP_SHA256:
                    print(f"[warn] nested {name} hash differs from the known copy; "
                          "continuing to the .fls check")
                with zipfile.ZipFile(io.BytesIO(inner)) as nested:
                    for sub in nested.namelist():
                        if sub.endswith("3210600c.fls"):
                            data = nested.read(sub)
                            if sha256(data) == FLS_SHA256:
                                print(f"[ok] inner .fls verified: {source}!{name}!{sub}")
                                return data
                raise SystemExit(f"{source}!{name}: no valid 3210600c.fls inside")
    raise SystemExit(f"{source}: no 3210600c.fls found (looked for direct and nested members)")


def run(cmd: list[str]) -> None:
    print("+", " ".join(str(c) for c in cmd))
    subprocess.run([str(c) for c in cmd], check=True, env=FRESH_PYCACHE_ENV)


def compare(generated: Path, existing: Path) -> str:
    """Compare a fresh output against the working tree's generated copy.

    Returns "exact", "differs", or "absent". Absent is a normal state, not
    corruption: the generated files are never committed, so a fresh checkout
    has none until the first build creates them.
    """
    if not existing.exists():
        print(f"[no copy] {existing.relative_to(REPO)} not present in the "
              f"working tree (generated files are never committed; a normal "
              f"build creates them)")
        return "absent"
    if filecmp.cmp(generated, existing, shallow=False):
        print(f"[BYTE-EXACT] {existing.relative_to(REPO)}")
        return "exact"
    print(f"[DIFFERS] {generated} vs {existing.relative_to(REPO)}")
    proc = subprocess.run(["diff", str(existing), str(generated)],
                          capture_output=True, text=True, check=False, timeout=60)
    lines = proc.stdout.splitlines()
    for line in lines[:15]:
        print("   ", line)
    if len(lines) > 15:
        print(f"    ... ({len(lines) - 15} more diff lines)")
    return "differs"


def validate_comparison_results(results: list[str]) -> int:
    """Validate the generated-copy set and return its absent-file count.

    A fresh checkout legitimately has no generated copies, but a partially
    missing set is damage rather than a fresh-checkout state. Treating each
    absent file independently would let an accidental deletion pass.
    """
    unknown = set(results) - {"exact", "differs", "absent"}
    if unknown:
        raise ValueError(f"unknown comparison result(s): {sorted(unknown)}")
    differs = results.count("differs")
    absent = results.count("absent")
    if differs:
        raise SystemExit(f"{differs} of {len(results)} outputs differ from "
                         f"the working tree's generated copies")
    if absent not in (0, len(results)):
        raise SystemExit(
            f"generated-copy set is incomplete: {absent} of {len(results)} "
            f"outputs are missing (a fresh checkout has all copies absent; "
            f"an initialized tree must have all copies present)")
    return absent


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--archive", type=Path,
                        default=REPO / "firmware" / "nokia_3210_6.00.zip")
    parser.add_argument("--installer", type=Path,
                        default=REPO / "firmware" / "nse8_600.exe",
                        help="optional WinTesla installer; adds the regional "
                             "language packs to the extracted inventory")
    parser.add_argument("--out", type=Path, default=HERE / "out")
    args = parser.parse_args()

    # Fail fast: refuse a wrong installer before ANY extraction work, not
    # after minutes of intermediate generation.
    if args.installer.exists():
        verify_installer(args.installer)

    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    fls_bytes = resolve_fls(args.archive)
    fls = out / "3210600c.fls"
    fls.write_bytes(fls_bytes)

    py = sys.executable
    work = out / "work"
    shutil.rmtree(work, ignore_errors=True)  # never reuse stale extractions
    run([py, HERE / "gen_intermediates.py", "--fls", fls, "--work", work])

    # Language inventory: the full flash is authoritative for the shipped
    # languages; the installer's regional packs add the rest (RTL skipped by
    # the extractor). gen_strings picks what the firmware ships via its own
    # language table.
    lang_sources = [fls]
    if args.installer.exists():
        installer_dir = work / "installer"
        run([py, HERE / "vendor" / "extract_freeman_installer.py",
             args.installer, installer_dir])
        lang_sources += sorted(installer_dir.glob("nse8nx06.00[a-z]"))
    else:
        print(f"[note] installer {args.installer} not present; "
              "language inventory limited to the full flash")
    languages = work / "languages"
    run([py, HERE / "extract_text_packs.py", *lang_sources, "--out", languages])
    donors = find_t9_donors([args.archive.parent])
    donor_args = (["--t9-donors", *donors] if donors else [])
    run([py, HERE / "gen_inventory.py", "--fls", fls, "--archive", args.archive,
         *(["--packs", *lang_sources[1:]] if len(lang_sources) > 1 else []),
         *donor_args, "--out", work / "inventory"])

    run([py, HERE / "gen_t9.py", "--fls", fls,
         "--out-c", out / "t9_ldb.c", "--out-h", out / "t9_ldb.h"])
    run([py, HERE / "gen_strings.py",
         languages,
         out / "strings_data.c", out / "strings_data.h"])
    run([py, HERE / "gen_assets.py",
         "--fonts-dir", work / "firmware_assets" / "fonts",
         "--bitmaps", work / "firmware_assets" / "bitmaps" / "3210600c_bitmaps.json",
         "--fls", fls,
         "--output", out / "assets_data.c"])
    run([py, HERE / "gen_tones.py", "--fls", fls,
         "--ringtones", work / "assets" / "ringtones" / "tone_data",
         "--system-tones", work / "assets" / "system_tones" / "raw",
         "--out-c", out / "tones_data.c", "--out-h", out / "tones.h"])

    print()
    results = [
        compare(out / "t9_ldb.c", REPO / "src/generated/t9_ldb.c"),
        compare(out / "t9_ldb.h", REPO / "include/generated/t9_ldb.h"),
        compare(out / "strings_data.c", REPO / "src/generated/strings_data.c"),
        compare(out / "strings_data.h", REPO / "include/generated/strings_data.h"),
        compare(out / "assets_data.c", REPO / "src/generated/assets_data.c"),
        compare(out / "tones_data.c", REPO / "src/generated/tones_data.c"),
        compare(out / "tones.h", REPO / "include/generated/tones.h"),
    ]
    print()
    absent = validate_comparison_results(results)
    if absent:
        print(f"{len(results) - absent} of {len(results)} verified byte-exact; "
              f"{absent} had no working-tree copy to compare (fresh checkout?).")
        print(f"Fresh outputs are under {out} for inspection; run a normal "
              f"build to generate src/generated + include/generated, then "
              f"rerun this script to verify them.")
    else:
        print(f"ALL {len(results)} GENERATED FILES BYTE-EXACT")


if __name__ == "__main__":
    main()
