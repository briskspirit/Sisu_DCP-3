#!/usr/bin/env python3
"""Build-time asset generation from the original NSE-8 v6.00 firmware.

Policy (invoked from CMake before every firmware build):

  firmware present   -> assets are (re)generated from it and synced into
                        src/generated + include/generated. An input stamp
                        (firmware hash + generator sources + language
                        selection + output hashes) makes the common no-change
                        case cost only a hash check; any change regenerates.
  firmware absent    -> the existing generated assets are used as-is; the
                        build proceeds if all of them are present.
  neither present    -> the build fails with instructions.

Firmware discovery: the first of --fw-dir (repo firmware/ by default)
containing nokia_3210_6.00.zip, 3210600c.fls, or any
zip/fls that hash-verifies to the known v6.00 flash. The optional WinTesla
installer (nse8_600.exe) beside it enables the full 34-language inventory;
without it only the flash's own languages are available.
"""
from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent

# Stale-bytecode immunity (rationale in gen_all.py): set BEFORE the local
# imports so even gen_all itself is compiled from source, never from a
# stale-but-"valid" .pyc. gen_all re-points the prefix at import; both are
# per-run empty, which is all that matters.
sys.dont_write_bytecode = True
sys.pycache_prefix = os.path.join(
    tempfile.gettempdir(), f"sisu-assetgen-pyc-{os.getpid()}-{uuid.uuid4().hex}")

sys.path.insert(0, str(HERE))
from gen_all import (FRESH_PYCACHE_ENV, FLS_SHA256, ZIP_SHA256,  # noqa: E402
                     find_t9_donors, resolve_fls, verify_installer)
from gen_strings import selected_langs  # noqa: E402
from gen_t9 import normalize_t9_langs  # noqa: E402

GENERATED = (
    ("src/generated/strings_data.c", "strings_data.c"),
    ("include/generated/strings_data.h", "strings_data.h"),
    ("src/generated/t9_ldb.c", "t9_ldb.c"),
    ("include/generated/t9_ldb.h", "t9_ldb.h"),
    ("src/generated/assets_data.c", "assets_data.c"),
    ("src/generated/tones_data.c", "tones_data.c"),
    ("include/generated/tones.h", "tones.h"),
)

MISSING_HELP = f"""
[assets] FAILED: no firmware and no generated assets.

Place ONE of these in <repo>/firmware/:
  - nokia_3210_6.00.zip   SHA-256 {ZIP_SHA256}
  - 3210600c.fls          SHA-256 {FLS_SHA256}
  - any archive containing either (verified by hash, so repackaging is fine)

The canonical archive is publicly available; the archive.org item
"nokia-phone-firmwares" carries a byte-identical nokia_3210_6.00.zip inside
nokia_firmwares.zip. Optionally add nse8_600.exe next to it for the full
regional language inventory.
"""


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def find_firmware(fw_dirs: list[Path]) -> Path | None:
    preferred = ("nokia_3210_6.00.zip", "3210600c.fls")
    for d in fw_dirs:
        if not d.is_dir():
            continue
        for name in preferred:
            if (d / name).is_file():
                return d / name
        for candidate in sorted(d.iterdir()):
            if candidate.suffix.lower() in (".zip", ".fls") and candidate.is_file():
                try:
                    resolve_fls(candidate)
                    return candidate
                except SystemExit:
                    continue
    return None


def stamp_key(firmware: Path, installer: Path | None, donors: list[Path],
              langs: str, t9_langs: str) -> str:
    h = hashlib.sha256()
    h.update(sha256_file(firmware).encode())
    h.update(sha256_file(installer).encode() if installer else b"no-installer")
    for donor in donors:
        h.update(sha256_file(donor).encode())
    for script in sorted(list(HERE.glob("*.py")) + list((HERE / "vendor").glob("*.py"))):
        h.update(sha256_file(script).encode())
    h.update(f"|{langs}|{t9_langs}|".encode())
    return h.hexdigest()


def normalized_stems(langs: str) -> str:
    return ",".join(stem for _, stem, _ in selected_langs(langs))


def normalized_t9(t9_langs: str) -> str:
    return ",".join(normalize_t9_langs(t9_langs))


def compiled_manifest(header: Path, define: str) -> str | None:
    if not header.is_file():
        return None
    for line in header.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(f"#define {define} "):
            return line.split('"')[1]
    return None


def verify_fallback_selection(langs: str, t9_langs: str,
                              strings_header: Path | None = None,
                              t9_header: Path | None = None) -> bool:
    """Without firmware the existing assets must match the requested sets."""
    want_langs = normalized_stems(langs)
    want_t9 = normalized_t9(t9_langs)
    strings_header = strings_header or REPO / "include/generated/strings_data.h"
    t9_header = t9_header or REPO / "include/generated/t9_ldb.h"
    have_langs = compiled_manifest(strings_header,
                                   "STRINGS_COMPILED_LANGS")
    have_t9 = compiled_manifest(t9_header,
                                "T9_COMPILED_LANGS")
    ok = True
    if have_langs != want_langs:
        sys.stderr.write(f"[assets] language selection mismatch: assets carry "
                         f"[{have_langs}], requested [{want_langs}]\n")
        ok = False
    if have_t9 != want_t9:
        sys.stderr.write(f"[assets] T9 selection mismatch: assets carry "
                         f"[{have_t9}], requested [{want_t9}]\n")
        ok = False
    if not ok:
        sys.stderr.write("[assets] supply the original firmware (see "
                         "firmware/README.md) to regenerate with this selection\n")
    return ok


# Glyph coverage: a selected language may only ship if every codepoint of its
# records and self-name exists in ALL main text fonts (FS0/FS1/FS2) — the
# strictest rule, which the entire Latin/Greek/Cyrillic inventory satisfies.
# CJK/Thai/Hindi/Vietnamese fail here until their regional font sets are
# merged, which is the honest outcome.
COVERAGE_STYLES = ("largebold", "smallplain", "smallbold")


def check_glyph_coverage(work: Path, langs: str) -> None:
    fonts_dir = work / "firmware_assets" / "fonts"
    covered: set[int] | None = None
    for style in COVERAGE_STYLES:
        data = json.loads((fonts_dir / f"3210600c_full_{style}.json").read_text())
        cps = {int(g["codepoint"]) for g in data["glyphs"]}
        covered = cps if covered is None else covered & cps
    assert covered is not None

    failures = []
    for lang_id, stem, self_name in selected_langs(langs):
        entry = json.loads((work / "languages" / f"{stem}.json").read_text())
        used = {ord(ch) for r in entry["records"] for ch in r.get("text", "")}
        used |= {ord(ch) for ch in self_name}
        missing = sorted(cp for cp in used - covered if cp >= 0x20)
        if missing:
            sample = " ".join(f"U+{cp:04X}" for cp in missing[:8])
            failures.append(f"  {stem}: {len(missing)} missing glyphs ({sample} ...)")
    if failures:
        raise SystemExit(
            "[assets] glyph coverage check FAILED — these languages need font "
            "sets the build does not include:\n" + "\n".join(failures) +
            "\n[assets] deselect them or merge their regional fonts first")


def outputs_hash() -> str | None:
    h = hashlib.sha256()
    for rel, _ in GENERATED:
        path = REPO / rel
        if not path.is_file():
            return None
        h.update(sha256_file(path).encode())
    return h.hexdigest()


def sync_outputs(out: Path, dest_src: Path, dest_inc: Path) -> int:
    changed = 0
    for rel, name in GENERATED:
        src = out / name
        dst = (dest_inc if rel.startswith("include/") else dest_src) / name
        if not dst.is_file() or src.read_bytes() != dst.read_bytes():
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dst)
            changed += 1
    return changed


def generate(firmware: Path, installer: Path | None, donors: list[Path],
             work: Path, langs: str, t9_langs: str,
             emit_dir: Path | None = None) -> None:
    py = sys.executable
    # Always start clean: stale pack extractions or language JSONs from a
    # previous configuration must never leak into this generation.
    shutil.rmtree(work, ignore_errors=True)
    fls = work / "3210600c.fls"
    work.mkdir(parents=True, exist_ok=True)
    fls.write_bytes(resolve_fls(firmware))

    subprocess.run([py, str(HERE / "gen_intermediates.py"),
                    "--fls", str(fls), "--work", str(work)], check=True, env=FRESH_PYCACHE_ENV,
                   stdout=subprocess.DEVNULL)

    lang_sources = [fls]
    if installer is not None:
        installer_dir = work / "installer"
        subprocess.run([py, str(HERE / "vendor" / "extract_freeman_installer.py"),
                        str(installer), str(installer_dir)], check=True, env=FRESH_PYCACHE_ENV,
                       stdout=subprocess.DEVNULL)
        lang_sources += sorted(installer_dir.glob("nse8nx06.00[a-z]"))
    languages = work / "languages"
    subprocess.run([py, str(HERE / "extract_text_packs.py"),
                    *[str(s) for s in lang_sources], "--out", str(languages)],
                   check=True, env=FRESH_PYCACHE_ENV, stdout=subprocess.DEVNULL)

    subprocess.run([py, str(HERE / "gen_inventory.py"), "--fls", str(fls),
                    "--archive", str(firmware),
                    *(["--packs"] + [str(s) for s in lang_sources[1:]]
                      if len(lang_sources) > 1 else []),
                    *(["--t9-donors"] + [str(d) for d in donors]
                      if donors else []),
                    "--out", str(work / "inventory")], check=True, env=FRESH_PYCACHE_ENV,
                   stdout=subprocess.DEVNULL)

    out = work / "generated"
    out.mkdir(exist_ok=True)
    subprocess.run([py, str(HERE / "gen_strings.py"), str(languages),
                    str(out / "strings_data.c"), str(out / "strings_data.h"),
                    f"--langs={langs}"], check=True, env=FRESH_PYCACHE_ENV, stdout=subprocess.DEVNULL)
    subprocess.run([py, str(HERE / "gen_t9.py"), "--fls", str(fls),
                    "--ldb-dir", str(work / "inventory" / "t9"),
                    "--t9-langs", t9_langs,
                    "--out-c", str(out / "t9_ldb.c"),
                    "--out-h", str(out / "t9_ldb.h")], check=True, env=FRESH_PYCACHE_ENV,
                   stdout=subprocess.DEVNULL)
    subprocess.run([py, str(HERE / "gen_assets.py"),
                    "--fonts-dir", str(work / "firmware_assets" / "fonts"),
                    "--bitmaps", str(work / "firmware_assets" / "bitmaps" / "3210600c_bitmaps.json"),
                    "--fls", str(fls),
                    "--output", str(out / "assets_data.c")], check=True, env=FRESH_PYCACHE_ENV,
                   stdout=subprocess.DEVNULL)
    subprocess.run([py, str(HERE / "gen_tones.py"), "--fls", str(fls),
                    "--ringtones", str(work / "assets" / "ringtones" / "tone_data"),
                    "--system-tones", str(work / "assets" / "system_tones" / "raw"),
                    "--out-c", str(out / "tones_data.c"),
                    "--out-h", str(out / "tones.h")], check=True, env=FRESH_PYCACHE_ENV,
                   stdout=subprocess.DEVNULL)

    check_glyph_coverage(work, langs)

    if emit_dir is not None:
        changed = sync_outputs(out, emit_dir / "src", emit_dir / "include" / "generated")
        where = f"into {emit_dir}"
    else:
        changed = sync_outputs(out, REPO / "src" / "generated",
                               REPO / "include" / "generated")
        where = "in-repo"
    print(f"[assets] generated from {firmware.name} {where}: "
          f"{changed} file(s) updated, {len(GENERATED) - changed} unchanged")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fw-dir", type=Path, action="append", default=None)
    parser.add_argument("--langs", default="shipped")
    parser.add_argument("--t9-langs", default="ENGL,GER,FREN")
    parser.add_argument("--stamp", type=Path,
                        default=REPO / "build" / "sisu_assets.stamp")
    parser.add_argument("--work", type=Path, default=None)
    parser.add_argument("--emit-dir", type=Path, default=None,
                        help="emit into this directory instead of the repo "
                             "(used for non-default selections so build "
                             "directories never fight over src/generated)")
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()

    fw_dirs = args.fw_dir or [REPO / "firmware"]
    firmware = find_firmware([Path(d) for d in fw_dirs])

    if firmware is None:
        if args.emit_dir is not None:
            sys.stderr.write("[assets] a non-default language/dictionary "
                             "selection requires the original firmware (see "
                             "firmware/README.md); there is no fallback for "
                             "custom sets\n")
            return 1
        if outputs_hash() is None:
            sys.stderr.write(MISSING_HELP)
            return 1
        if not verify_fallback_selection(args.langs, args.t9_langs):
            return 1
        print("[assets] no firmware found; using existing generated assets")
        return 0

    installer = None
    for d in fw_dirs:
        candidate = Path(d) / "nse8_600.exe"
        if candidate.is_file():
            verify_installer(candidate)  # SystemExit(1) on hash mismatch
            installer = candidate
            break
    donors = find_t9_donors([Path(d) for d in fw_dirs])  # SystemExit(1) on mismatch

    def emitted_hash() -> str | None:
        if args.emit_dir is None:
            return outputs_hash()
        h = hashlib.sha256()
        for rel, name in GENERATED:
            sub = "include/generated" if rel.startswith("include/") else "src"
            path = args.emit_dir / sub / name
            if not path.is_file():
                return None
            h.update(sha256_file(path).encode())
        return h.hexdigest()

    def run_generation() -> int:
        key = stamp_key(firmware, installer, donors, args.langs, args.t9_langs)
        out_hash = emitted_hash()
        if not args.force and args.stamp.is_file() and out_hash is not None:
            try:
                stamp = json.loads(args.stamp.read_text())
            except (ValueError, OSError):
                stamp = {}
            if stamp.get("key") == key and stamp.get("outputs") == out_hash:
                print(f"[assets] up to date (firmware {firmware.name})")
                return 0
        try:
            if args.work is not None:
                generate(firmware, installer, donors, args.work, args.langs,
                         args.t9_langs, args.emit_dir)
            else:
                with tempfile.TemporaryDirectory(prefix="sisu-assetgen-") as td:
                    generate(firmware, installer, donors, Path(td) / "work",
                             args.langs, args.t9_langs, args.emit_dir)
        except subprocess.CalledProcessError as err:
            sys.stderr.write(f"[assets] generation step failed: "
                             f"{Path(str(err.cmd[1])).name} (see message above)\n")
            return 1
        args.stamp.parent.mkdir(parents=True, exist_ok=True)
        args.stamp.write_text(json.dumps({"key": key, "outputs": emitted_hash(),
                                          "firmware": firmware.name}) + "\n")
        return 0

    if args.emit_dir is not None:
        # Emission is private to this build directory: no repo lock needed.
        return run_generation()

    # Repo-mode stamp check, generation and sync all happen under the
    # repo-wide lock so a concurrent regeneration can never interleave with
    # another build directory's check-then-compile of src/generated.
    lock_path = REPO / ".assetgen.lock"
    with lock_path.open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        return run_generation()


if __name__ == "__main__":
    sys.exit(main())
