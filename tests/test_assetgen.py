#!/usr/bin/env python3
"""Regression tests for the asset-generation pipeline's pure logic.

Runs without any firmware: everything here is either pure normalization,
synthetic-data parsing, or validation against temporary synthetic manifests.
Firmware-dependent behavior (full generation, glyph rejection against real
fonts, private emission) is exercised by gen_build itself at build time.
"""
from __future__ import annotations

import contextlib
import hashlib
import io
import os
import py_compile
import subprocess
import sys
import tempfile
import uuid
import zipfile
from pathlib import Path

# Stale-bytecode immunity (rationale in tools/assetgen/gen_all.py): a
# same-size/same-mtime source edit leaves an adjacent .pyc "valid", so the
# suite could green-light OLD generator code. Point the cache at a per-run
# empty prefix BEFORE importing anything under test.
sys.dont_write_bytecode = True
sys.pycache_prefix = os.path.join(
    tempfile.gettempdir(), f"sisu-assetgen-pyc-{os.getpid()}-{uuid.uuid4().hex}")

REPO = Path(__file__).resolve().parent.parent

failures = 0


def check(cond: bool, message: str) -> None:
    global failures
    if not cond:
        print(f"FAIL: {message}", file=sys.stderr)
        failures += 1


def expect_exit(fn, message: str) -> None:
    try:
        fn()
    except SystemExit:
        return
    check(False, message)


def plant_stale_probe(probe_dir: Path, name: str, old: str, new: str) -> Path:
    """The reproduced hole: an adjacent .pyc compiled from OLD content stays
    "valid" for same-size NEW content restored to the exact old mtime."""
    mod = probe_dir / f"{name}.py"
    mod.write_text(old)
    st = mod.stat()
    pyc = probe_dir / "__pycache__" / f"{name}.{sys.implementation.cache_tag}.pyc"
    pyc.parent.mkdir()
    py_compile.compile(str(mod), cfile=str(pyc), doraise=True)
    check(pyc.is_file(), f"probe setup: stale .pyc planted for {name}")
    assert len(new) == len(old)
    mod.write_text(new)
    os.utime(mod, ns=(st.st_atime_ns, st.st_mtime_ns))
    return mod


# --- stale-bytecode immunity: this process ----------------------------------
# MUST run before the local imports below: gen_all/gen_build re-point
# sys.pycache_prefix at import time, so a later probe would pass on their
# protection even with this file's own pre-import header removed.

with tempfile.TemporaryDirectory() as td:
    probe_dir = Path(td)
    plant_stale_probe(probe_dir, "assetgen_pyc_probe",
                      'VALUE = "old"\n', 'VALUE = "new"\n')
    sys.path.insert(0, str(probe_dir))
    try:
        import assetgen_pyc_probe
        check(assetgen_pyc_probe.VALUE == "new",
              "pre-import: the edited source wins over a stale-but-'valid' .pyc")
    finally:
        sys.path.remove(str(probe_dir))
        sys.modules.pop("assetgen_pyc_probe", None)

sys.path.insert(0, str(REPO / "tools" / "assetgen"))
sys.path.insert(0, str(REPO / "tools" / "assetgen" / "vendor"))

import gen_strings  # noqa: E402
import gen_t9  # noqa: E402
import gen_all  # noqa: E402
import gen_build  # noqa: E402
import gen_inventory  # noqa: E402
from extract_text_packs import deframe_wintesla  # noqa: E402

# The bootstrap invariant must survive the imports themselves (they re-point
# the prefix; it must land on another per-run empty path, never back on None).
check(sys.dont_write_bytecode is True,
      "post-import: bytecode writing stays disabled")
check(sys.pycache_prefix is not None and not os.path.exists(sys.pycache_prefix),
      "post-import: pycache prefix is a per-run empty path")


# --- language selection: faithful menu order -------------------------------

TRACED_CEE = ["ENGL", "GERM", "FREN", "GREE", "BULG", "HUNG", "ROMA", "POLI",
              "CZEC", "SLVA", "CROA", "SERB", "SLVE", "RUSS", "ESTO", "LATV",
              "LITH"]
WESTERN_PACK = ["DUTC", "ITAL", "DANI", "SWED", "NORW", "FINN", "SPAN",
                "PORT", "TURK"]
APAC_PACK = ["INDO", "MALA", "HIND", "THAI", "VIET", "CHNT", "CHNS", "TAGA"]

all_stems = [stem for _, stem, _ in gen_strings.ALL_LANGS]
check(all_stems == TRACED_CEE + WESTERN_PACK + APAC_PACK,
      "ALL_LANGS is the verified menu order: traced CEE, then the western "
      "and APAC packs in TEXT-chunk enumeration order")

shipped = [stem for _, stem, _ in gen_strings.selected_langs("shipped")]
check(shipped == TRACED_CEE, "shipped selection is the traced v6.00 order")

superset = [stem for _, stem, _ in gen_strings.selected_langs("all")]
check(superset == all_stems, "'all' preserves table order")

plus = [stem for _, stem, _ in gen_strings.selected_langs("shipped,ITAL")]
check(plus == TRACED_CEE + ["ITAL"], "set tokens expand inside comma lists")

engl_forced = [stem for _, stem, _ in gen_strings.selected_langs("GERM,FREN")]
check(engl_forced[0] == "ENGL", "English is always included and first")

reordered = [stem for _, stem, _ in gen_strings.selected_langs("FREN,ENGL,GERM")]
check(reordered == ["ENGL", "GERM", "FREN"],
      "selection spelling never changes emission order")

expect_exit(lambda: gen_strings.selected_langs("HEBR"),
            "RTL/unknown stems are rejected")

# The v6.00 resource tables use U+0000 as an empty slot only inside the
# length-delimited upper/lower multi-tap records. A C string cannot preserve
# that hole: normalize it away, retain the characters after it, and reject a
# NUL in every ordinary localized record.
check(gen_strings.normalize_record_text(
          "LATV", 918, "ABC2ĀČ\x00ÀÁÂÃÆÇ") == "ABC2ĀČÀÁÂÃÆÇ",
      "localized multi-tap normalization removes an empty glyph slot without "
      "truncating the tail")
check(gen_strings.normalize_record_text(
          "ESTO", 921, "def3èé\x00ê") == "def3èéê",
      "localized multi-tap normalization preserves characters after the slot")
expect_exit(lambda: gen_strings.normalize_record_text(
                "SYNTH", 100, "ordinary\x00record"),
            "an embedded NUL outside the multi-tap record range is rejected")
try:
    gen_strings.c_string("cannot\x00emit")
except ValueError:
    pass
else:
    check(False, "the C-string emitter rejects every residual embedded NUL")

# --- T9 normalization ------------------------------------------------------

check(gen_t9.normalize_t9_langs("FREN,ENGL,GER") == ["ENGL", "FREN", "GER"],
      "T9: English moves to the FRONT regardless of spelling")
check(gen_t9.normalize_t9_langs("GER,GER,FREN") == ["ENGL", "GER", "FREN"],
      "T9: dedupe and forced English")
expect_exit(lambda: gen_t9.normalize_t9_langs("ENGL,KLIN"),
            "T9: unknown tags are rejected")
check(gen_t9.normalize_t9_langs("CZEC,POLI,TURK") == ["ENGL", "CZEC", "POLI", "TURK"],
      "T9: the v7 donor languages are selectable")
check(gen_t9.T9_LANGS["CZEC"][1] == 21 and gen_t9.T9_LANGS["POLI"][1] == 28
      and gen_t9.T9_LANGS["TURK"][1] == 13,
      "T9: donor-language ids match the strings registry")

# --- T9 header parsing: both DCT-3 generations, foreign layouts rejected ----

def synth_ldb_header(r3_size: int, break_region: bool = False) -> bytes:
    """Minimal header with a consistent region table (v6: r3=0x120, v7: 0x240)."""
    trie = 0x1FC + r3_size
    blob = bytearray(0x2000)
    blob[69:71] = (0x0009).to_bytes(2, "big")
    def put(off, val):
        blob[gen_t9.CODEC_BASE + off:gen_t9.CODEC_BASE + off + 2] = \
            val.to_bytes(2, "little")
    put(60, 0x5C)
    put(62, 0x120)
    put(64, 0x17C)
    put(66, 0x80)
    put(68, 0x1FC if not break_region else 0x2E00)
    put(70, r3_size)
    put(72, trie)
    for i in range(4):
        put(74 + 2 * i, trie + 0x40 + 0x10 * i)  # ascending short pointers
    put(82, trie + 0x9E)
    put(84, trie + 0x89E)
    return bytes(blob)

v6_params = gen_t9.parse_params("SYNTH", synth_ldb_header(0x120))
check(v6_params[2] == 0x31C, "v6 layout parses (trie at 0x31C)")
v7_params = gen_t9.parse_params("SYNTH", synth_ldb_header(0x240))
check(v7_params[2] == 0x43C, "v7 layout parses (trie at 0x43C)")
check(v6_params[1] == v7_params[1] == 0x5C, "char table stays at 0x5C in both")
expect_exit(lambda: gen_t9.parse_params("SYNTH", synth_ldb_header(0x240, break_region=True)),
            "a foreign (DCT4-style) region table is rejected")

# --- fallback manifest gate ------------------------------------------------

with tempfile.TemporaryDirectory() as td:
    manifests = Path(td)
    strings_header = manifests / "strings_data.h"
    t9_header = manifests / "t9_ldb.h"
    strings_header.write_text(
        f'#define STRINGS_COMPILED_LANGS "{gen_build.normalized_stems("shipped")}"\n')
    t9_header.write_text('#define T9_COMPILED_LANGS "ENGL,GER,FREN"\n')
    check(gen_build.verify_fallback_selection(
              "shipped", "ENGL,GER,FREN", strings_header, t9_header),
          "matching generated manifests satisfy the default selection")
    with contextlib.redirect_stderr(io.StringIO()):
        rejected = not gen_build.verify_fallback_selection(
            "ENGL", "ENGL", strings_header, t9_header)
    check(rejected, "generated manifests reject a mismatched fallback selection")

# --- T9 char-table gate (synthetic dictionary) -----------------------------

def synth_t9_blob(bad_unit: int | None) -> bytes:
    char = 0x5C
    blob = bytearray(gen_t9.CODEC_BASE + char + 9 * 0x20)
    for i in range(9 * 16):
        off = gen_t9.CODEC_BASE + char + 2 * i
        blob[off:off + 2] = (0x61).to_bytes(2, "little")  # 'a'
    if bad_unit is not None:
        blob[gen_t9.CODEC_BASE + char:gen_t9.CODEC_BASE + char + 2] = \
            bad_unit.to_bytes(2, "little")
    return bytes(blob)

params = (1, 0x5C, 0x31C, (0, 0, 0, 0), 0, 0, (0,) * 6)
gen_t9.check_char_table("SYNTH", synth_t9_blob(None), params)
gen_t9.check_char_table("SYNTH", synth_t9_blob(0x00E6), params)  # ae folds
gen_t9.check_char_table("SYNTH", synth_t9_blob(0x0131), params)  # dotless i folds
expect_exit(lambda: gen_t9.check_char_table("SYNTH", synth_t9_blob(0x0110), params),
            "char-table gate rejects unfoldable units")

# --- inventory rerun over an existing directory ----------------------------

def synth_ldb_image(tag: bytes) -> bytes:
    sig = gen_inventory.TEGIC_SIGNATURE
    payload_len = 16 + len(sig) + 16
    rec = bytearray()
    rec += (1).to_bytes(4, "big")
    rec += payload_len.to_bytes(4, "big")
    rec += tag.ljust(4, b"\x00")
    rec += b"\x00" * 4
    rec += sig
    rec += b"\x00" * 16
    return b"\xaa" * 64 + bytes(rec) + b"\xbb" * 64

with tempfile.TemporaryDirectory() as td:
    out = Path(td) / "t9"
    image = synth_ldb_image(b"TSTL")
    first: list = []
    gen_inventory.carve_t9("image-one", image, out, first)
    check(len(first) == 1 and not first[0].get("reused"),
          "first inventory run writes and reports the dictionary")
    second: list = []
    gen_inventory.carve_t9("image-two", image, out, second)
    check(len(second) == 1 and second[0].get("reused") is True,
          "a rerun still reports existing dictionaries (marked reused)")

# --- archive-carried Czech T9 member integrity ------------------------------

with tempfile.TemporaryDirectory() as td:
    root = Path(td)
    fls = b"same verified flash in both archives"
    donor = b"canonical Czech T9 donor member"
    donor_name = "0700NSE8.B"
    original_fls_hash = gen_all.FLS_SHA256
    original_members = gen_inventory.ARCHIVE_T9_MEMBERS
    gen_all.FLS_SHA256 = hashlib.sha256(fls).hexdigest()
    gen_inventory.ARCHIVE_T9_MEMBERS = {
        donor_name: hashlib.sha256(donor).hexdigest(),
    }
    try:
        good = root / "good.zip"
        with zipfile.ZipFile(good, "w") as zf:
            zf.writestr("bundle/3210600c.fls", fls)
            zf.writestr("repacked/layout/" + donor_name, donor)
        check(gen_all.resolve_fls(good) == fls,
              "synthetic archive carries the verified flash")
        images = gen_inventory.load_verified_archive_t9_images(good)
        check(images == [("0700NSE8.B", donor)],
              "the hash-pinned Czech donor member is accepted")

        altered = root / "altered.zip"
        with zipfile.ZipFile(altered, "w") as zf:
            zf.writestr("bundle/3210600c.fls", fls)
            zf.writestr("other/layout/" + donor_name, donor + b" altered")
        check(gen_all.resolve_fls(altered) == fls,
              "the altered archive still carries the same verified flash")
        expect_exit(
            lambda: gen_inventory.load_verified_archive_t9_images(altered),
            "same-FLS archive cannot inject an altered Czech dictionary member")
    finally:
        gen_all.FLS_SHA256 = original_fls_hash
        gen_inventory.ARCHIVE_T9_MEMBERS = original_members

# --- stale-bytecode immunity: generator subprocesses ------------------------
# The generation pipeline runs the generators as child processes under
# gen_all.FRESH_PYCACHE_ENV; a child facing the same planted stale .pyc must
# also read the edited source, or generation itself can execute old code.

with tempfile.TemporaryDirectory() as td:
    probe_dir = Path(td)
    plant_stale_probe(probe_dir, "assetgen_child_probe",
                      'VALUE = "old"\n', 'VALUE = "new"\n')
    child = subprocess.run(
        [sys.executable, "-c",
         f"import sys; sys.path.insert(0, {str(probe_dir)!r}); "
         f"import assetgen_child_probe; print(assetgen_child_probe.VALUE)"],
        capture_output=True, text=True, env=gen_all.FRESH_PYCACHE_ENV)
    check(child.returncode == 0 and child.stdout.strip() == "new",
          "FRESH_PYCACHE_ENV subprocess reads the edited source, not the .pyc")

# --- installer hash gate ----------------------------------------------------

with tempfile.TemporaryDirectory() as td:
    bogus = Path(td) / "nse8_600.exe"
    bogus.write_bytes(b"not the real installer")
    expect_exit(lambda: gen_all.verify_installer(bogus),
                "a wrong-hash installer is refused")
real_installer = REPO / "firmware" / "nse8_600.exe"
if real_installer.is_file():
    gen_all.verify_installer(real_installer)  # SystemExit here = FAIL

# --- generated-copy set integrity ------------------------------------------

check(gen_all.validate_comparison_results(["exact"] * 7) == 0,
      "a complete byte-exact generated-copy set is accepted")
check(gen_all.validate_comparison_results(["absent"] * 7) == 7,
      "an all-absent generated-copy set is accepted for a fresh checkout")
expect_exit(
    lambda: gen_all.validate_comparison_results(["exact"] * 6 + ["absent"]),
    "a partially missing generated-copy set is rejected")
expect_exit(
    lambda: gen_all.validate_comparison_results(["exact"] * 6 + ["differs"]),
    "a differing generated copy is rejected")

# --- WinTesla de-framing (synthetic) ---------------------------------------

def synth_pack(blocks: list[bytes], break_addr: bool = False) -> bytes:
    data = bytearray()
    addr = 0x2F0000
    for block in blocks:
        data.append(0x0B)
        data += addr.to_bytes(3, "big")
        data.append(0x00)
        data += len(block).to_bytes(3, "big")
        data.append(0x00)
        data += block
        addr += len(block) + (0x1000 if break_addr else 0)
    return bytes(data)

payload = (b"PPM\x00V " + b"x" * 0x3FFA, b"tail-block")
check(deframe_wintesla(synth_pack(list(payload))) == b"".join(payload),
      "de-framing strips 9-byte transfer records")
check(deframe_wintesla(synth_pack(list(payload), break_addr=True)) is None,
      "de-framing rejects non-contiguous record addresses")
check(deframe_wintesla(b"PPM\x00 plain image") is None,
      "plain images are not mistaken for packs")

# ---------------------------------------------------------------------------

if failures:
    print(f"{failures} assetgen assertion(s) failed", file=sys.stderr)
    sys.exit(1)
print("assetgen pipeline logic tests passed")
