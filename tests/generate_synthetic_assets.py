#!/usr/bin/env python3
"""Generate small, non-Nokia host-test assets into a temporary directory.

The production assets remain hash-verified outputs of the original firmware and
are never committed. These fixtures only provide deterministic public data for
source-level host tests in a fresh checkout and CI.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def c_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=False)


def glyph_width(font_id: int, codepoint: int) -> int:
    if font_id == 0:
        return 4 if codepoint == 0x20 else 8
    if font_id == 1:
        return 3 if codepoint == 0x20 else 5
    if font_id == 2:
        if codepoint == 0x20:
            return 3
        if codepoint == 0x20AC:
            return 2
        if codepoint == 0x0394:
            return 8
        if codepoint in (ord("i"), ord("l"), ord("I")):
            return 3
        if codepoint == ord("W"):
            return 8
        return 6
    if font_id == 3:
        return 3 if codepoint == 0x20 else 6
    return 3 if codepoint == 0x20 else 4


def emit_assets(root: Path) -> None:
    heights = (8, 7, 7, 7, 6)
    names = ("FS0", "FS1", "FS2", "FS3", "FS4")
    chunks: list[str] = [
        "/* Synthetic public host-test assets. Generated in a temporary directory. */",
        '#include "ui/assets.h"',
        "",
    ]
    font_rows: list[str] = []
    for font_id, name in enumerate(names):
        codepoints = list(range(0x20, 0x7F))
        if font_id == 2:
            codepoints.extend((0x0394, 0x20AC))
        if font_id == 3:
            codepoints = [cp for cp in codepoints if not (ord("a") <= cp <= ord("z"))]
        glyphs: list[str] = []
        data: list[int] = []
        for cp in codepoints:
            width = glyph_width(font_id, cp)
            offset = len(data)
            columns = width * ((heights[font_id] + 7) // 8)
            data.extend([0x00 if cp == 0x20 else 0x7F] * columns)
            glyphs.append(
                f"    {{{cp}u, {width}u, {heights[font_id]}u, {offset}u}},"
            )
        chunks.extend(
            [
                f"static const glyph_t SYNTH_{name}_GLYPHS[] = {{",
                *glyphs,
                "};",
                f"static const uint8_t SYNTH_{name}_DATA[] = {{",
                "    " + ", ".join(f"0x{byte:02x}u" for byte in data),
                "};",
                "",
            ]
        )
        font_rows.append(
            f'    {{"Synthetic {name}", SYNTH_{name}_GLYPHS, '
            f'(uint16_t)(sizeof(SYNTH_{name}_GLYPHS) / sizeof(SYNTH_{name}_GLYPHS[0])), '
            f"SYNTH_{name}_DATA, (uint16_t)sizeof(SYNTH_{name}_DATA), "
            f"{heights[font_id]}u, 0u}},"
        )

    bitmap_specs = ((1, 16, 7), (2, 10, 7), (26, 18, 7), (31, 7, 7))
    bitmap_rows: list[str] = []
    bitmap_data: list[int] = []
    for bitmap_id, width, height in bitmap_specs:
        offset = len(bitmap_data)
        bitmap_data.extend([0x7F] * (width * ((height + 7) // 8)))
        bitmap_rows.append(f"    {{{bitmap_id}u, {width}u, {height}u, {offset}u}},")

    chunks.extend(
        [
            "const font_t g_fonts[FONT_COUNT] = {",
            *font_rows,
            "};",
            "",
            "static const bitmap_t SYNTH_BITMAPS[] = {",
            *bitmap_rows,
            "};",
            "static const uint8_t SYNTH_BITMAP_DATA[] = {",
            "    " + ", ".join(f"0x{byte:02x}u" for byte in bitmap_data),
            "};",
            "const bitmap_store_t g_bitmaps = {",
            "    SYNTH_BITMAPS,",
            "    (uint16_t)(sizeof(SYNTH_BITMAPS) / sizeof(SYNTH_BITMAPS[0])),",
            "    SYNTH_BITMAP_DATA,",
            "    (uint16_t)sizeof(SYNTH_BITMAP_DATA),",
            "};",
            "",
        ]
    )
    write(root / "src/generated/assets_data.c", "\n".join(chunks))


def emit_strings(root: Path) -> None:
    header = """/* Synthetic public host-test string registry. */
#ifndef GENERATED_STRINGS_DATA_H
#define GENERATED_STRINGS_DATA_H

#include <stdint.h>

#define STRINGS_RECORD_COUNT 986u
#define STRINGS_SID_BASE 58u
#define STRINGS_COMPILED_LANGS "SYNTHETIC"

typedef struct {
    uint8_t lang_id;
    const char *self_name;
    const char *const *records;
} string_table_t;

typedef struct {
    const char *english;
    uint16_t index;
} string_key_t;

extern const string_table_t g_string_tables[];
extern const uint8_t g_string_table_count;
extern const string_key_t g_string_keys[];
extern const uint16_t g_string_key_count;

#endif
"""
    write(root / "include/generated/strings_data.h", header)

    english = {
        0x043: "Activate\nphone\nfor calls?",
        0x139: "New e-mail\nmessage",
        0x13A: "%S\nnew e-mail\nmessages",
        0x146: "New fax\nmessage",
        0x147: "%S\nnew fax\nmessages",
        0x17F: "Synthetic localized text",
        0x187: "Calling",
        0x18B: "Serial No.\n%S",
        0x216: "1\nmissed\ncall",
        0x2A4: "Requesting",
        0x2CE: "Phone book",
        0x2D6: "Answer",
        0x2D8: "Back",
        0x2E9: "OK",
        0x2EA: "Options",
        0x2EE: "Select",
        0x337: "Message",
        0x33A: "No number\nfound\non this screen",
    }
    localized = {
        "GREE": (11, "Greek", {0x043: "Ενεργοποίηση\nτηλεφώνου για\nκλήσεις;", 0x2A4: "Αίτημα"}),
        "HUNG": (12, "Hungarian", {0x043: "A telefon\nfogadjon\nhívásokat?"}),
        "RUSS": (15, "Russian", {0x043: "Включить\nтелефон\nна прием?", 0x216: "1\nпропущен\nзвонок"}),
    }

    def table(name: str, values: dict[int, str]) -> list[str]:
        rows = [f"static const char *const {name}[STRINGS_RECORD_COUNT] = {{"]
        for sid, value in sorted(values.items()):
            rows.append(
                f"    [{sid}u - STRINGS_SID_BASE] = {c_string(value)},"
            )
        rows.extend(["};", ""])
        return rows

    chunks = [
        "/* Synthetic public host-test strings. Generated in a temporary directory. */",
        '#include "generated/strings_data.h"',
        "",
        *table("SYNTH_ENGLISH", english),
    ]
    for stem, (_, _, values) in localized.items():
        chunks.extend(table(f"SYNTH_{stem}", values))
    chunks.extend(
        [
            "const string_table_t g_string_tables[] = {",
            '    {1u, "English", SYNTH_ENGLISH},',
        ]
    )
    for stem, (lang_id, self_name, _) in localized.items():
        chunks.append(
            f"    {{{lang_id}u, {c_string(self_name)}, SYNTH_{stem}}},"
        )
    chunks.extend(
        [
            "};",
            "const uint8_t g_string_table_count =",
            "    (uint8_t)(sizeof(g_string_tables) / sizeof(g_string_tables[0]));",
            "const string_key_t g_string_keys[] = {{0, 0u}};",
            "const uint16_t g_string_key_count = 0u;",
            "",
        ]
    )
    write(root / "src/generated/strings_data.c", "\n".join(chunks))


def emit_tones(root: Path) -> None:
    header = """/* Synthetic public host-test tones. */
#ifndef GENERATED_TONES_H
#define GENERATED_TONES_H

#include <stdint.h>

typedef struct {
    uint8_t index;
    uint8_t value;
    const char *name;
    const uint8_t *data;
    uint16_t length;
} ringtone_t;

uint8_t ringtone_count(void);
const ringtone_t *ringtone_at(uint8_t index);
const ringtone_t *ringtone_by_index(uint8_t index);
const ringtone_t *ringtone_by_value(uint8_t value);
const uint8_t *system_tone_data(uint8_t index, uint16_t *out_len);
uint16_t tone_frequency_hz(uint8_t pitch);

#endif
"""
    source = """/* Synthetic public host-test tones. */
#include "generated/tones.h"

#include <stddef.h>

static const uint8_t SYNTH_RING[] = {0x00u, 0x4au, 0x10u, 0x0bu};
static const uint8_t SYNTH_ASCENDING[] = {
    0x00u, 0x09u, 0xfcu, 0x4au, 0x10u, 0x0bu,
};
static const ringtone_t SYNTH_RINGTONES[] = {
    {0u, 1u, "Synthetic", SYNTH_RING, (uint16_t)sizeof(SYNTH_RING)},
    {6u, 7u, "Synthetic rising", SYNTH_ASCENDING,
     (uint16_t)sizeof(SYNTH_ASCENDING)},
};

uint8_t ringtone_count(void) {
    return (uint8_t)(sizeof(SYNTH_RINGTONES) / sizeof(SYNTH_RINGTONES[0]));
}

const ringtone_t *ringtone_at(uint8_t index) {
    return index < ringtone_count() ? &SYNTH_RINGTONES[index] : NULL;
}

const ringtone_t *ringtone_by_index(uint8_t index) {
    for (uint8_t i = 0u; i < ringtone_count(); i++) {
        if (SYNTH_RINGTONES[i].index == index) {
            return &SYNTH_RINGTONES[i];
        }
    }
    return NULL;
}

const ringtone_t *ringtone_by_value(uint8_t value) {
    for (uint8_t i = 0u; i < ringtone_count(); i++) {
        if (SYNTH_RINGTONES[i].value == value) {
            return &SYNTH_RINGTONES[i];
        }
    }
    return NULL;
}

const uint8_t *system_tone_data(uint8_t index, uint16_t *out_len) {
    if (out_len != NULL) {
        *out_len = 0u;
    }
    if (index != 10u && index != 12u && index != 14u && index != 28u &&
        index != 29u && index != 32u) {
        return NULL;
    }
    if (out_len != NULL) {
        *out_len = (uint16_t)sizeof(SYNTH_ASCENDING);
    }
    return SYNTH_ASCENDING;
}

uint16_t tone_frequency_hz(uint8_t pitch) {
    if (pitch <= 0x40u || pitch > 0xa4u) {
        return 0u;
    }
    if (pitch == 0x7eu) {
        return 523u;
    }
    if (pitch == 0x8eu) {
        return 1319u;
    }
    return (uint16_t)(220u + (uint16_t)(pitch - 0x40u) * 5u);
}
"""
    write(root / "include/generated/tones.h", header)
    write(root / "src/generated/tones_data.c", source)


def emit_t9(root: Path) -> None:
    header = """/* Synthetic public host-test T9 registry. */
#ifndef GENERATED_T9_LDB_H
#define GENERATED_T9_LDB_H

#include <stdint.h>

#define T9_COMPILED_LANGS "SYNTHETIC"

typedef struct {
    const char *tag;
    const char *label;
    uint8_t lang_id;
    const unsigned char *data;
    unsigned int data_len;
    uint16_t dictionary_id;
    uint16_t char_table_offset;
    uint16_t trie_root_offset;
    uint16_t short_pointer_offsets[4];
    uint16_t block_pointer_base_offset;
    uint16_t payload_base_offset;
    uint16_t block_pointer_thresholds[6];
} t9_ldb_desc_t;

extern const t9_ldb_desc_t g_t9_ldbs[];
extern const uint8_t g_t9_ldb_count;
extern const unsigned char g_t9_ldb_engl[1024];
extern const unsigned int g_t9_ldb_engl_len;

#endif
"""
    source = """/* Synthetic public host-test T9 registry. */
#include "generated/t9_ldb.h"

const unsigned char g_t9_ldb_engl[1024] = {0u};
const unsigned int g_t9_ldb_engl_len = sizeof(g_t9_ldb_engl);

const t9_ldb_desc_t g_t9_ldbs[] = {
    {
        "ENGL", "English", 1u, g_t9_ldb_engl, sizeof(g_t9_ldb_engl),
        0x0009u, 0x005cu, 0x031cu,
        {0x0358u, 0x0371u, 0x0373u, 0x0386u},
        0x03a3u, 0x0ba3u,
        {0x0040u, 0u, 0u, 0u, 0u, 0u},
    },
};
const uint8_t g_t9_ldb_count = 1u;
"""
    write(root / "include/generated/t9_ldb.h", header)
    write(root / "src/generated/t9_ldb.c", source)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: generate_synthetic_assets.py OUTPUT_ROOT", file=sys.stderr)
        return 2
    root = Path(sys.argv[1]).resolve()
    emit_assets(root)
    emit_strings(root)
    emit_tones(root)
    emit_t9(root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
