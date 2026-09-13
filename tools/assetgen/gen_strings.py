#!/usr/bin/env python3
"""Generate the localized UI string tables for the Sisu DCP-3 clone.

Usage: gen_strings.py <by_language_dir> <out_c> <out_h> [--langs=SET]

Reads the v6.00 per-language PPM string tables (<by_language_dir>/*.json,
extracted from a user-supplied ROM dump) and emits
src/generated/strings_data.c + include/generated/strings_data.h.

--langs selects the compiled language set: 'shipped' (the 17-language CEE
set, default), 'all' (every extracted LTR language), or a comma list of
pack stems (e.g. ENGL,GERM,ITAL). English is the fallback for every
untranslated record and the guaranteed minimum, so ENGL is always included
and always emitted first regardless of the selection. The emitted registry
(g_string_tables[] with per-language native self-names) is what the
firmware lists at runtime: the Language menu shows exactly the compiled
set. Right-to-left languages (Hebrew, Arabic) are not eligible: the
firmware has no RTL text pipeline.

Model: v6.00 SIDs. record index i == SID (i + 58). The clone code localizes
by English text: tr("English literal") binary-searches the English key table
for the record index, then returns the active language's record (falling back
to the English literal when there is no translation).
"""
import json
import os
import sys

# (runtime language id, by_language file stem, native self-name). Ids match
# each JSON's numeric_id and are what the firmware persists. Self-names render
# in each language's own script and are never translated; CEE names follow the
# traced v6.00 menu, western names the packs' COMM chunk, the rest the
# languages' standard native names.
#
# ORDER IS THE MENU ORDER. Registry emission (and therefore the Language menu)
# follows this table, never a numeric sort. The original selector simply lists
# the PPM pack's TEXT chunks in pack order, so: the first 17 rows are the
# exact traced v6.00c (CEE pack) sequence, followed by the western-pack and
# then APAC-pack languages in their verified TEXT-chunk enumeration order.
# English (1) MUST be first: it is the fallback and the reverse-key source.
ALL_LANGS = [
    (1,  "ENGL", "English"),
    (2,  "GERM", "Deutsch"),
    (3,  "FREN", "Français"),
    (11, "GREE", "Ελληνικά"),
    (19, "BULG", "Български"),
    (12, "HUNG", "Magyar"),
    (29, "ROMA", "Română"),
    (28, "POLI", "Polski"),
    (21, "CZEC", "Čeština"),
    (31, "SLVA", "Slovenčina"),
    (20, "CROA", "Hrvatski"),
    (30, "SERB", "Srpski"),
    (32, "SLVE", "Slovenščina"),
    (15, "RUSS", "Русский"),
    (24, "ESTO", "Eesti"),
    (26, "LATV", "Latviešu"),
    (27, "LITH", "Lietuvių"),
    (7,  "DUTC", "Nederlands"),
    (4,  "ITAL", "Italiano"),
    (8,  "DANI", "Dansk"),
    (9,  "SWED", "Svenska"),
    (14, "NORW", "Norsk"),
    (10, "FINN", "Suomi"),
    (5,  "SPAN", "Español"),
    (6,  "PORT", "Português"),
    (13, "TURK", "Türkçe"),
    (17, "INDO", "Bahasa Indonesia"),
    (18, "MALA", "Bahasa Melayu"),
    (41, "HIND", "हिन्दी"),
    (33, "THAI", "ภาษาไทย"),
    (34, "VIET", "Tiếng Việt"),
    (23, "CHNT", "繁體中文"),
    (22, "CHNS", "中文"),
    (40, "TAGA", "Filipino"),
]

SHIPPED_STEMS = [
    "ENGL", "GERM", "FREN", "GREE", "BULG", "HUNG", "ROMA", "POLI", "CZEC",
    "SLVA", "CROA", "SERB", "SLVE", "RUSS", "ESTO", "LATV", "LITH",
]

# v6.00 records 914..933 are the length-delimited upper/lower multi-tap
# character tables for keys 0..9. Regional packs use U+0000 inside a few of
# these records as an empty glyph slot (the paired table and other language
# packs continue with ordinary characters after that position). The generated
# ABI uses NUL-terminated C strings, so preserve the intended cycle by omitting
# that empty slot. A NUL anywhere else is malformed input and must fail closed.
MULTITAP_RECORD_FIRST = 914
MULTITAP_RECORD_LAST = 933


def selected_langs(spec):
    stems = []
    for token in spec.split(","):
        token = token.strip()
        if not token:
            continue
        if token.lower() == "shipped":
            stems.extend(SHIPPED_STEMS)
        elif token.lower() == "all":
            stems.extend(stem for _, stem, _ in ALL_LANGS)
        else:
            stems.append(token.upper())
    by_stem = {row[1]: row for row in ALL_LANGS}
    unknown = [s for s in stems if s not in by_stem]
    if unknown:
        sys.exit("gen_strings.py: unknown/ineligible language stems: %s "
                 "(RTL languages are not eligible; see ALL_LANGS)" % unknown)
    # Emission order == ALL_LANGS order (the faithful menu order), English
    # always first regardless of how the selection was spelled.
    wanted = set(stems) | {"ENGL"}
    return [row for row in ALL_LANGS if row[1] in wanted]


def normalize_record_text(stem: str, index: int, text: str) -> str:
    if "\x00" not in text:
        return text
    if not MULTITAP_RECORD_FIRST <= index <= MULTITAP_RECORD_LAST:
        sys.exit(
            "gen_strings.py: language %s record %u contains an unexpected "
            "embedded U+0000" % (stem, index))
    return text.replace("\x00", "")


def c_string(s: str) -> str:
    """UTF-8 text -> a safe C string-literal body (no surrounding quotes)."""
    if "\x00" in s:
        raise ValueError("embedded NUL cannot be emitted as a C string")
    out = []
    for b in s.encode("utf-8"):
        c = chr(b)
        if b == 0x22:      # "
            out.append('\\"')
        elif b == 0x5C:    # backslash
            out.append("\\\\")
        elif b == 0x0A:
            out.append("\\n")
        elif b == 0x09:
            out.append("\\t")
        elif 0x20 <= b < 0x7F:
            out.append(c)
        else:
            out.append("\\%03o" % b)  # 3-digit octal: unambiguous next-char
    return "".join(out)


def main():
    spec = "shipped"
    args = []
    for a in sys.argv[1:]:
        if a.startswith("--langs="):
            spec = a.split("=", 1)[1]
        else:
            args.append(a)
    if len(args) != 3:
        sys.exit("usage: gen_strings.py <by_language_dir> <out_c> <out_h> [--langs=shipped|all|A,B,...]")
    re_dir = args[0]
    if not os.path.isdir(re_dir):
        sys.exit("gen_strings.py: not a directory: %s" % re_dir)
    tables = []
    for lang_id, stem, self_name in selected_langs(spec):
        path = os.path.join(re_dir, stem + ".json")
        if not os.path.isfile(path):
            sys.exit("gen_strings.py: language %s is not in the extracted set "
                     "(%s missing) -- languages beyond the flash's own pack "
                     "require the WinTesla installer (nse8_600.exe) next to "
                     "the firmware" % (stem, path))
        d = json.load(open(path))
        assert d["numeric_id"] == lang_id, (stem, d["numeric_id"], lang_id)
        recs = [normalize_record_text(stem, i, r.get("text", ""))
                for i, r in enumerate(d["records"])]
        tables.append((lang_id, stem, self_name, recs))

    count = len(tables[0][3])
    for lang_id, stem, self_name, recs in tables:
        assert len(recs) == count, (stem, len(recs), count)

    eng = tables[0][3]  # English records, index-aligned

    # Reverse key table: English text -> record index. Dedup (first wins),
    # skip empties. Sorted by UTF-8 bytes so the runtime can strcmp/bsearch.
    seen = {}
    for i, t in enumerate(eng):
        if t and t not in seen:
            seen[t] = i
    keys = sorted(seen.items(), key=lambda kv: kv[0].encode("utf-8"))

    hdr = []
    hdr.append("/* Auto-generated by tools/gen_strings.py. DO NOT EDIT. */")
    hdr.append("/* Source: Nokia 3210 v6.00 reversed per-language PPM string tables. */")
    hdr.append("#ifndef GENERATED_STRINGS_DATA_H")
    hdr.append("#define GENERATED_STRINGS_DATA_H")
    hdr.append("")
    hdr.append("#include <stdint.h>")
    hdr.append("")
    hdr.append("#define STRINGS_RECORD_COUNT %uu /* SID = index + 58 */" % count)
    hdr.append("#define STRINGS_SID_BASE 58u")
    hdr.append('#define STRINGS_COMPILED_LANGS "%s" /* generation manifest */'
               % ",".join(stem for _, stem, _, _ in tables))
    hdr.append("")
    hdr.append("typedef struct {")
    hdr.append("    uint8_t lang_id;")
    hdr.append("    const char *self_name;      /* native script; menus show it verbatim */")
    hdr.append("    const char *const *records; /* [STRINGS_RECORD_COUNT] */")
    hdr.append("} string_table_t;")
    hdr.append("")
    hdr.append("typedef struct {")
    hdr.append("    const char *english;")
    hdr.append("    uint16_t index;")
    hdr.append("} string_key_t;")
    hdr.append("")
    hdr.append("extern const string_table_t g_string_tables[];")
    hdr.append("extern const uint8_t g_string_table_count;")
    hdr.append("extern const string_key_t g_string_keys[];")
    hdr.append("extern const uint16_t g_string_key_count;")
    hdr.append("")
    hdr.append("#endif /* GENERATED_STRINGS_DATA_H */")
    hdr.append("")

    src = []
    src.append("/* Auto-generated by tools/gen_strings.py. DO NOT EDIT. */")
    src.append("/* Source: Nokia 3210 v6.00 reversed per-language PPM string tables. */")
    src.append("")
    src.append('#include "generated/strings_data.h"')
    src.append("")
    for lang_id, stem, self_name, recs in tables:
        src.append("static const char *const REC_%s[STRINGS_RECORD_COUNT] = {" % stem)
        for t in recs:
            src.append('    "%s",' % c_string(t))
        src.append("};")
        src.append("")
    src.append("const string_table_t g_string_tables[] = {")
    for lang_id, stem, self_name, _ in tables:
        src.append('    { %uu, "%s", REC_%s },' % (lang_id, c_string(self_name), stem))
    src.append("};")
    src.append("const uint8_t g_string_table_count = %uu;" % len(tables))
    src.append("")
    src.append("const string_key_t g_string_keys[] = {")
    for text, idx in keys:
        src.append('    { "%s", %uu },' % (c_string(text), idx))
    src.append("};")
    src.append("const uint16_t g_string_key_count = %uu;" % len(keys))
    src.append("")

    out_c = args[1]
    out_h = args[2]
    open(out_h, "w").write("\n".join(hdr))
    open(out_c, "w").write("\n".join(src))
    print("wrote %s (%d langs, %d records, %d keys)" % (out_c, len(tables), count, len(keys)))


if __name__ == "__main__":
    main()
