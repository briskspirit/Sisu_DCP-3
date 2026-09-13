#!/usr/bin/env python3
"""Carve Tegic T9 LDB dictionaries and emit the generated T9 registry.

Sources: the NSE-8 v6.00 flash PPM section (ENGL/GER/FREN) plus any
inventory .ldb files carved from the regional language packs. The emitted
files carry both the dictionary byte arrays and a registry of descriptors
(g_t9_ldbs[]) whose layout parameters are parsed from each LDB's own
header — the firmware lists and uses exactly what was generated, nothing
hardcoded. English is the guaranteed minimum and is always included.

Layout parameters (verified to reproduce the previously hand-derived
values for ENGL/GER/FREN exactly): with native offsets starting after the
16-byte subchunk header,
  dictionary_id            big-endian u16 at blob offset 69
  char_table_offset        LE u16 at native 60
  trie_root_offset         LE u16 at native 72
  short_pointer_offsets[4] LE u16 at native 74..81
  block_pointer_base       LE u16 at native 82
  payload_base             LE u16 at native 84
  thresholds[6]            bytes at native 86..91

The header is a self-describing region table (offset/size pairs at native
60..71), which is how both DCT-3 Tegic generations parse uniformly: the
v6.00 layout has region 3 (a keymap our runtime never reads) at 0x1FC+0x120
so the trie root lands at 0x31C; the v7.00 layout doubles that region to
0x240, shifting the trie to 0x43C. Everything from the trie root on is
format-identical (verified byte-exact on a same-build v6/v7 DANI pair), so
the runtime needs no version awareness. Truly foreign layouts (e.g. the
DCT4-era 5210 dictionaries) fail the structural check and are rejected.
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "vendor"))
from extract_ppm_chunks import parse_ppm  # noqa: E402

# tag -> (dictionary menu label, strings-registry language id).
# The trio labels match the original hand-written table exactly.
T9_LANGS = {
    "ENGL": ("English", 1),
    "GER": ("Deutsch", 2),
    "FREN": ("Francais", 3),
    "ITAL": ("Italiano", 4),
    "SPAN": ("Espanol", 5),
    "PORT": ("Portugues", 6),
    "DUTC": ("Nederlands", 7),
    "DANI": ("Dansk", 8),
    "SWED": ("Svenska", 9),
    "FINN": ("Suomi", 10),
    "NORW": ("Norsk", 14),
    # v7-layout dictionaries carved from the optional NPE3_527 donor package
    # (see gen_all.T9_DONORS) or the canonical archive's own t9_cz files.
    # Latin-script only: GREE/RUSS exist in donor material but the candidate
    # path is ASCII (latin_unit_to_ascii), so non-Latin T9 stays out until
    # the service grows a unit-based candidate pipeline.
    "CZEC": ("Cestina", 21),
    "POLI": ("Polski", 28),
    "TURK": ("Turkce", 13),
}
DEFAULT_LANGS = ("ENGL", "GER", "FREN")
CODEC_BASE = 16


def normalize_t9_langs(spec: str) -> list[str]:
    """English is the runtime fallback (dictionary index 0 everywhere), so it
    is always FIRST, not merely present, regardless of the spelling."""
    tags = [t.strip().upper() for t in spec.split(",") if t.strip()]
    tags = ["ENGL"] + [t for t in dict.fromkeys(tags) if t != "ENGL"]
    unknown = [t for t in tags if t not in T9_LANGS]
    if unknown:
        raise SystemExit(f"gen_t9.py: no metadata for T9 languages: {unknown}")
    return tags


# Mirror of the firmware's latin_unit_to_ascii() fold domain: every non-ASCII
# unit a dictionary's character table may carry must fold to a base letter or
# candidates silently drop characters. Kept in sync with t9_service.c.
FOLDABLE_UNITS = set(range(0x20, 0x7F)) | {
    0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6,
    0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6,
    0x00C7, 0x00E7,
    0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00E8, 0x00E9, 0x00EA, 0x00EB,
    0x00CC, 0x00CD, 0x00CE, 0x00CF, 0x00EC, 0x00ED, 0x00EE, 0x00EF,
    0x00D1, 0x00F1,
    0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D8,
    0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F8,
    0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00F9, 0x00FA, 0x00FB, 0x00FC,
    0x00DD, 0x0178, 0x00FD, 0x00FF,
    0x00DF,
    0x00A1, 0x00BF,  # Spanish inverted punctuation on the key-1 class
    0x011E, 0x011F, 0x0130, 0x0131, 0x015E, 0x015F,  # Turkish G-breve, I, S-cedilla
    0x008A, 0x009A, 0x009F,  # S-caron/Y-diaeresis, CP1252-coded in the 6210 tables
}


def check_char_table(tag: str, blob: bytes, params) -> None:
    """Every unit in the dictionary's key character table must be foldable.

    The table layout mirrors the firmware's slot_char(): 9 key classes
    (punctuation + keys 2..9), each 0x20 bytes = two case rows of 8 slots.
    """
    _did, char, _trie, *_rest = params
    unmapped = set()
    for off in range(char, char + 9 * 0x20, 2):
        unit = le16(blob, CODEC_BASE + off)
        if unit != 0 and unit not in FOLDABLE_UNITS:
            unmapped.add(unit)
    if unmapped:
        listing = " ".join(f"U+{u:04X}" for u in sorted(unmapped))
        raise SystemExit(
            f"gen_t9.py: {tag} character table uses units the firmware fold "
            f"cannot map: {listing} — extend latin_unit_to_ascii() (and this "
            f"mirror) before shipping this dictionary")

KNOWN_PARAMS = {  # generation-time self-check: hand-derived v6 trio + the
    # bench-derived v7 set (CZEC from the canonical archive, POLI/TURK from
    # the NPE3_527 donor)
    "ENGL": (0x0009, 0x005C, 0x031C, (0x0358, 0x0371, 0x0373, 0x0386),
             0x03A3, 0x0BA3, (0x40, 0, 0, 0, 0, 0)),
    "GER":  (0x0007, 0x005C, 0x031C, (0x035D, 0x037C, 0x039A, 0x03BA),
             0x03D6, 0x0C16, (0x3E, 0x02, 0, 0, 0, 0)),
    "FREN": (0x000C, 0x005C, 0x031C, (0x036F, 0x038A, 0x03A1, 0x03B4),
             0x03C9, 0x0BC9, (0x40, 0, 0, 0, 0, 0)),
    "CZEC": (0x0007, 0x005C, 0x043C, (0x0471, 0x0487, 0x04A0, 0x04BF),
             0x04DB, 0x0CFB, (0x3F, 0x01, 0, 0, 0, 0)),
    "POLI": (0x0015, 0x005C, 0x043C, (0x0471, 0x0491, 0x04A8, 0x04C4),
             0x04C6, 0x0CE6, (0x3F, 0x01, 0, 0, 0, 0)),
    "TURK": (0x001F, 0x005C, 0x043C, (0x0471, 0x0473, 0x048F, 0x04AE),
             0x04C8, 0x0CC8, (0x40, 0, 0, 0, 0, 0)),
}


def le16(b: bytes, off: int) -> int:
    return b[off] | (b[off + 1] << 8)


def parse_params(tag: str, blob: bytes):
    did = (blob[69] << 8) | blob[70]
    char = le16(blob, CODEC_BASE + 60)
    char_size = le16(blob, CODEC_BASE + 62)
    r2_off = le16(blob, CODEC_BASE + 64)
    r2_size = le16(blob, CODEC_BASE + 66)
    r3_off = le16(blob, CODEC_BASE + 68)
    r3_size = le16(blob, CODEC_BASE + 70)
    trie = le16(blob, CODEC_BASE + 72)
    shorts = tuple(le16(blob, CODEC_BASE + 74 + 2 * i) for i in range(4))
    blkbase = le16(blob, CODEC_BASE + 82)
    paybase = le16(blob, CODEC_BASE + 84)
    thr = tuple(blob[CODEC_BASE + 86 + i] for i in range(6))
    params = (did, char, trie, shorts, blkbase, paybase, thr)
    # The region table must be self-consistent: contiguous regions with the
    # trie root right after region 3. v6 (r3 0x120, trie 0x31C) and v7
    # (r3 0x240, trie 0x43C) both satisfy this; foreign layouts (DCT4-era
    # dictionaries) read garbage here and fail.
    structural = (char == 0x5C and char_size == 0x120
                  and r2_off == char + char_size
                  and r3_off == r2_off + r2_size
                  and trie == r3_off + r3_size
                  and list(shorts) == sorted(shorts))
    if not structural:
        raise SystemExit(
            f"{tag}: unsupported LDB layout (region table inconsistent; "
            f"parsed char=0x{char:04x} trie=0x{trie:04x} -- DCT4-generation "
            f"dictionaries are not usable)")
    if tag in KNOWN_PARAMS and params != KNOWN_PARAMS[tag]:
        raise SystemExit(f"{tag}: parsed params diverge from the verified set: {params}")
    return params


def carve_from_fls(fls: bytes) -> dict[str, bytes]:
    _, chunks = parse_ppm(fls)
    out: dict[str, bytes] = {}
    for chunk in chunks:
        if chunk["type"] != "LDB":
            continue
        for sub in chunk["subchunks"]:
            out[sub["type"]] = sub["payload"]
    return out


def emit_array(out: list[str], name: str, data: bytes) -> None:
    out.append(f"const unsigned char {name}[] = {{")
    for offset in range(0, len(data), 12):
        chunk = data[offset:offset + 12]
        line = "  " + ", ".join(f"0x{b:02x}" for b in chunk)
        if offset + 12 < len(data):
            line += ","
        out.append(line)
    out.append("};")
    out.append(f"const unsigned int {name}_len = {len(data)};")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fls", type=Path)
    parser.add_argument("--ldb-dir", type=Path,
                        help="inventory dir with <TAG>.ldb files for languages "
                             "not present in the flash PPM section")
    parser.add_argument("--t9-langs", default=",".join(DEFAULT_LANGS))
    parser.add_argument("--out-c", type=Path)
    parser.add_argument("--out-h", type=Path)
    parser.add_argument("--census", type=Path, nargs="+",
                        help="list T9 LDBs available in these images and exit")
    args = parser.parse_args()
    if args.census:
        census(args.census)
        return
    if not (args.fls and args.out_c and args.out_h):
        parser.error("--fls, --out-c and --out-h are required unless --census")

    langs = normalize_t9_langs(args.t9_langs)

    fls_ldbs = carve_from_fls(args.fls.read_bytes())
    entries = []  # (tag, label, lang_id, blob, params)
    for tag in langs:
        blob = fls_ldbs.get(tag)
        if blob is None and args.ldb_dir:
            candidate = args.ldb_dir / f"{tag}.ldb"
            if candidate.exists():
                blob = candidate.read_bytes()
        if blob is None:
            sys.exit(f"gen_t9.py: dictionary {tag} not in the flash PPM and no "
                     f"inventory file provided (--ldb-dir)")
        label, lang_id = T9_LANGS[tag]
        params = parse_params(tag, blob)
        check_char_table(tag, blob, params)
        entries.append((tag, label, lang_id, blob, params))

    src: list[str] = ['#include "generated/t9_ldb.h"', ""]
    for index, (tag, _label, _lid, blob, _params) in enumerate(entries):
        emit_array(src, f"g_t9_ldb_{tag.lower()}", blob)
        if index + 1 < len(entries):
            src.append("")
    src.append("")
    src.append("const t9_ldb_desc_t g_t9_ldbs[] = {")
    for tag, label, lang_id, blob, params in entries:
        did, char, trie, shorts, blkbase, paybase, thr = params
        name = f"g_t9_ldb_{tag.lower()}"
        src.append(f'    {{ "{tag}", "{label}", {lang_id}u, {name}, {len(blob)}u,')
        src.append(f"      0x{did:04x}u, 0x{char:04x}u, 0x{trie:04x}u,")
        src.append("      { " + ", ".join(f"0x{v:04x}u" for v in shorts) + " },")
        src.append(f"      0x{blkbase:04x}u, 0x{paybase:04x}u,")
        src.append("      { " + ", ".join(f"0x{v:04x}u" for v in thr) + " } },")
    src.append("};")
    src.append(f"const uint8_t g_t9_ldb_count = {len(entries)}u;")
    src.append("")

    hdr: list[str] = [
        "#ifndef GENERATED_T9_LDB_H",
        "#define GENERATED_T9_LDB_H",
        "",
        "#include <stdint.h>",
        "",
        '#define T9_COMPILED_LANGS "%s" /* generation manifest */'
        % ",".join(tag for tag, *_ in entries),
        "",
        "/* One compiled-in Tegic T9 dictionary. Layout parameters are parsed",
        " * from the LDB's own header at generation time; the firmware simply",
        " * lists this registry. lang_id matches the strings registry (0 =",
        " * no matching UI language compiled in). */",
        "typedef struct {",
        "    const char *tag;             /* pack tag, e.g. \"ENGL\" */",
        "    const char *label;           /* dictionary menu label */",
        "    uint8_t lang_id;",
        "    const unsigned char *data;",
        "    unsigned int data_len;",
        "    uint16_t dictionary_id;",
        "    uint16_t char_table_offset;",
        "    uint16_t trie_root_offset;",
        "    uint16_t short_pointer_offsets[4];",
        "    uint16_t block_pointer_base_offset;",
        "    uint16_t payload_base_offset;",
        "    uint16_t block_pointer_thresholds[6];",
        "} t9_ldb_desc_t;",
        "",
        "extern const t9_ldb_desc_t g_t9_ldbs[];",
        "extern const uint8_t g_t9_ldb_count;",
        "",
    ]
    for tag, _label, _lid, blob, _params in entries:
        name = f"g_t9_ldb_{tag.lower()}"
        hdr.append(f"extern const unsigned char {name}[{len(blob)}];")
        hdr.append(f"extern const unsigned int {name}_len;")
    hdr.append("")
    hdr.append("#endif")
    hdr.append("")

    args.out_c.parent.mkdir(parents=True, exist_ok=True)
    args.out_c.write_text("\n".join(src), encoding="ascii")
    args.out_h.write_text("\n".join(hdr), encoding="ascii")
    print(f"wrote {args.out_c} ({len(entries)} dictionaries: "
          f"{' '.join(tag for tag, *_ in entries)})")


TEGIC_SIGNATURE = b"Copyright \xa9 1999 Tegic"


def census(images: list[Path]) -> None:
    """List every T9 LDB present in the given images (any pack format)."""
    for image in images:
        data = image.read_bytes()
        found = []
        pos = 0
        while True:
            pos = data.find(TEGIC_SIGNATURE, pos + 1)
            if pos < 0:
                break
            rec = pos - 16
            lang = data[rec + 8:rec + 12].rstrip(b"\x00").decode("ascii", "replace")
            length = int.from_bytes(data[rec + 4:rec + 8], "big")
            found.append(f"{lang}({length})")
        print(f"{image.name}: {' '.join(found) if found else '(no T9 LDBs)'}")


if __name__ == "__main__":
    main()
