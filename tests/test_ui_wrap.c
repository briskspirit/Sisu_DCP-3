/* Host tests for the UTF-8-safe text primitives in ui.c: copy_text (must never
 * leave a partial trailing codepoint) and wrap_text_lines_ex (stride-aware wrap
 * that honors '\n', never splits a glyph, and preserves the glyph stream).
 *
 * Oracle discipline: expected values are derived from first principles (the same
 * codepoint decoder the renderer uses, driven independently here), never read
 * back from the code under test. Localized strings come from the real v6.00
 * tables via ts(), so the cases exercise genuine multibyte content. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "generated/strings_data.h"
#include "ui/assets.h"
#include "ui/text_layout.h"
#include "ui/ui.h"
#include "services/strings.h"

/* ui.c pulls in timebase for the marquee; not exercised here. */
uint32_t time_ms(void) { return 0u; }
uint64_t time_ms64(void) { return 0u; }
uint32_t time_ticks8(void) { return 0u; }
int32_t time_diff_ms(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

/* v6.00 OPTIONS_LANGUAGE ids used below. */
#define LANG_ENGL 1u
#define LANG_GREE 11u
#define LANG_HUNG 12u
#define LANG_RUSS 15u
#define LANG_BULG 19u

static int s_failures;

static void assert_true(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static void assert_eq_int(int got, int want, const char *message) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got %d, want %d)\n", message, got, want);
        s_failures++;
    }
}

/* True if byte offset `off` is exactly on a UTF-8 codepoint boundary of s.
 * Re-decodes with the renderer's own decoder: the exact "did we split a glyph"
 * test. */
static bool is_codepoint_boundary(const char *s, int off) {
    if (off <= 0) {
        return off == 0;
    }
    const char *p = s;
    while (*p) {
        int here = (int)(p - s);
        if (here == off) {
            return true;
        }
        if (here > off) {
            return false;
        }
        asset_next_codepoint(&p);
    }
    return (int)(p - s) == off;
}

/* Decode s into a codepoint sequence, optionally dropping ' ' and '\n'. */
static int codepoints_of(const char *s, uint16_t *out, int cap, bool drop_space) {
    int n = 0;
    const char *p = s;
    while (*p != '\0' && n < cap) {
        uint16_t cp = asset_next_codepoint(&p);
        if (drop_space && (cp == (uint16_t)' ' || cp == (uint16_t)'\n')) {
            continue;
        }
        out[n++] = cp;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* copy_text: the result must be the LONGEST codepoint-boundary prefix of src
 * whose byte length is <= cap-1. Never a partial trailing byte. */

static void check_copy_text(const char *src, const char *what) {
    size_t srclen = strlen(src);
    for (size_t cap = 1u; cap <= srclen + 2u; cap++) {
        char buf[256];
        memset(buf, 0x7f, sizeof(buf));
        copy_text(buf, cap, src);

        /* Oracle: largest codepoint boundary offset <= cap-1. */
        size_t expect = 0u;
        const char *p = src;
        while (*p != '\0') {
            const char *q = p;
            asset_next_codepoint(&q);
            size_t off = (size_t)(q - src);
            if (off <= cap - 1u) {
                expect = off;
            } else {
                break;
            }
            p = q;
        }

        size_t got = strlen(buf);
        if (got != expect) {
            fprintf(stderr, "  (%s: cap %zu -> len %zu, want %zu)\n", what, cap, got, expect);
            assert_true(false, "copy_text length equals oracle");
            return;
        }
        assert_true(got <= cap - 1u, "copy_text never exceeds cap-1");
        assert_true(memcmp(buf, src, got) == 0, "copy_text is a byte-exact prefix");
        assert_true(is_codepoint_boundary(src, (int)got), "copy_text stops on a codepoint boundary");
    }
}

static void test_copy_text(void) {
    /* cap 0 must not write (and must not crash). */
    char guard[4] = {'Z', 'Z', 'Z', 'Z'};
    copy_text(guard, 0u, "hi");
    assert_true(guard[0] == 'Z', "copy_text cap 0 writes nothing");
    /* NULL src -> empty string. */
    char e[4] = {'x', 'x', 'x', 'x'};
    copy_text(e, sizeof(e), NULL);
    assert_eq_int((int)strlen(e), 0, "copy_text NULL src -> empty");

    check_copy_text("Hello World", "ascii");
    /* Real localized strings: 2-byte Cyrillic/Greek glyphs are exactly what a
     * byte-wise truncation would split. */
    strings_set_language(LANG_RUSS);
    check_copy_text(ts_or(0x187u, "Calling"), "russian Calling 0x187");
    strings_set_language(LANG_BULG);
    check_copy_text(ts_or(0x17fu, "x"), "bulgarian 0x17f (69 bytes)");
    strings_set_language(LANG_GREE);
    check_copy_text(ts_or(0x2a4u, "Requesting"), "greek Requesting 0x2a4");
    strings_set_language(LANG_ENGL);
}

static void test_glyph_line_spans(void) {
    const font_t *font = asset_font(FONT_FS2);
    char narrow[161];
    memset(narrow, 'i', 160u);
    narrow[160] = '\0';
    ui_text_span_t spans[161];
    uint16_t count = 0u;
    assert_true(ui_glyph_line_spans(font, narrow, 82, spans,
                                    161u, &count),
                "160 narrow glyphs produce a complete span layout");
    assert_eq_int(count, 6, "160 narrow glyphs reach six visual rows");
    for (uint16_t i = 0u; i < 5u; i++) {
        assert_eq_int((int)(spans[i].end - spans[i].start), 27,
                      "each full narrow-glyph row is pixel bounded");
    }
    assert_eq_int((int)(spans[5].end - spans[5].start), 25,
                  "narrow-glyph tail remains visible");

    char wide[21];
    memset(wide, 'W', 20u);
    wide[20] = '\0';
    assert_true(ui_glyph_line_spans(font, wide, 82, spans,
                                    161u, &count),
                "wide-glyph layout succeeds");
    assert_eq_int(count, 2, "wide glyphs wrap by pixels");
    assert_eq_int((int)spans[0].end, 10,
                  "FS2 W fits ten glyphs in 82 pixels");

    const char *multibyte = "A\xce\x94\xe2\x82\xac" "B";
    assert_eq_int((int)ui_text_clamp_boundary(multibyte, 2u), 1,
                  "cursor inside a two-byte glyph clamps before it");
    assert_eq_int((int)ui_text_clamp_boundary(multibyte, 5u), 3,
                  "cursor inside a three-byte glyph clamps before it");
    assert_eq_int((int)ui_text_clamp_boundary(multibyte, 99u), 7,
                  "cursor beyond text clamps to the end");
    assert_eq_int((int)ui_text_previous_boundary(multibyte, 6u), 3,
                  "previous boundary crosses one complete euro glyph");
    assert_eq_int((int)ui_text_next_boundary(multibyte, 2u), 3,
                  "next boundary advances from the clamped glyph start");
    assert_eq_int((int)ui_text_next_boundary(multibyte, 99u), 7,
                  "next boundary at end remains at end");

    const char *explicit_rows = "ii\n\xce\x94\xce\x94";
    assert_true(ui_glyph_line_spans(font, explicit_rows, 82, spans,
                                    161u, &count),
                "explicit-newline layout succeeds");
    assert_eq_int(count, 2, "explicit newline creates a second row");
    assert_eq_int(spans[0].start, 0, "first explicit row starts at zero");
    assert_eq_int(spans[0].end, 2, "newline is excluded from first row");
    assert_eq_int(spans[1].start, 3, "second row starts after newline");
    assert_eq_int(spans[1].end, 7, "second row retains both UTF-8 glyphs");
    char copied[8];
    assert_true(ui_text_span_copy(explicit_rows, spans[1], copied,
                                  sizeof(copied)),
                "multibyte span copies whole");
    assert_true(strcmp(copied, "\xce\x94\xce\x94") == 0,
                "multibyte span bytes are exact");
    assert_eq_int((int)ui_text_offset_for_x(font, explicit_rows,
                                            spans[1], 7),
                  3, "x before first glyph stays at row start");
    assert_eq_int((int)ui_text_offset_for_x(font, explicit_rows,
                                            spans[1], 8),
                  5, "x at one glyph lands after one whole codepoint");

    assert_true(!ui_glyph_line_spans(font, wide, 82, spans, 1u, &count),
                "insufficient span storage fails instead of truncating rows");

    assert_eq_int(ui_glyph_line_count(font, narrow, 82), 6,
                  "streaming line count matches complete span layout");
    ui_text_span_t one;
    assert_true(ui_glyph_line_at(font, narrow, 82, 5u, &one),
                "streaming line lookup reaches the final row");
    assert_eq_int((int)one.start, 135,
                  "streaming final row starts after five complete rows");
    uint16_t row = UINT16_MAX;
    assert_true(ui_glyph_line_for_offset(font, narrow, 82, 27u,
                                         &row, &one),
                "cursor row lookup succeeds at a soft-wrap boundary");
    assert_eq_int(row, 1,
                  "cursor at a soft-wrap boundary belongs to the next row");
    assert_eq_int((int)one.start, 27,
                  "cursor-row span begins at that boundary");
    assert_eq_int(ui_glyph_line_count(font, "", 82), 1,
                  "empty editable text retains one cursor row");
    assert_eq_int(ui_glyph_line_count(font, "A\n", 82), 2,
                  "trailing newline retains its empty cursor row");
    assert_true(!ui_glyph_line_at(font, "A", 82, 1u, &one),
                "out-of-range streaming line lookup fails");
}

/* ------------------------------------------------------------------ */
/* wrap_text_lines_ex */

static void test_wrap_honors_newline(void) {
    const font_t *font = asset_font(FONT_FS2);
    char lines[3][32];
    /* Wide window so nothing wraps on width; only the '\n' breaks act. */
    uint8_t count = wrap_text_lines_ex(font, "Snooze\nactive", 1000, (char *)lines, 32u, 3u);
    assert_eq_int(count, 2, "explicit newline yields two lines");
    assert_true(strcmp(lines[0], "Snooze") == 0, "line 0 is 'Snooze'");
    assert_true(strcmp(lines[1], "active") == 0, "line 1 is 'active'");

    /* Trailing empty segments collapse; leading spaces are eaten. */
    count = wrap_text_lines_ex(font, "  one   two  ", 1000, (char *)lines, 32u, 3u);
    assert_eq_int(count, 1, "spaces collapse to a single line at wide width");
    assert_true(strcmp(lines[0], "one two") == 0, "collapsed spaces -> 'one two'");
}

static void test_dynamic_wrap_preserves_four_detail_rows(void) {
    const font_t *font = asset_font(FONT_FS2);
    char lines[4][32];
    uint8_t count = wrap_text_lines_ex(
        font, "Sender:\nAlexandra\n+1 212 555 0100\nReceived", 1000,
        (char *)lines, sizeof(lines[0]), 4u);

    assert_eq_int(count, 4, "message-detail wrapper returns all four rows");
    assert_true(strcmp(lines[0], "Sender:") == 0,
                "message-detail row 0 is the field label");
    assert_true(strcmp(lines[1], "Alexandra") == 0,
                "message-detail row 1 is the sender name");
    assert_true(strcmp(lines[2], "+1 212 555 0100") == 0,
                "message-detail row 2 is the sender number");
    assert_true(strcmp(lines[3], "Received") == 0,
                "message-detail row 3 is not silently discarded");

    count = wrap_text_lines_ex(font, "От:\nАнна\n+7 123\nПринято", 1000,
                               (char *)lines, sizeof(lines[0]), 4u);
    assert_eq_int(count, 4, "Russian message details preserve four rows");
    assert_true(strcmp(lines[0], "От:") == 0 &&
                    strcmp(lines[1], "Анна") == 0 &&
                    strcmp(lines[2], "+7 123") == 0 &&
                    strcmp(lines[3], "Принято") == 0,
                "Russian message-detail rows remain exact and ordered");

    count = wrap_text_lines_ex(font, "Απο:\nΑννα\n+30 123\nΛηψη", 1000,
                               (char *)lines, sizeof(lines[0]), 4u);
    assert_eq_int(count, 4, "Greek message details preserve four rows");
    assert_true(strcmp(lines[0], "Απο:") == 0 &&
                    strcmp(lines[1], "Αννα") == 0 &&
                    strcmp(lines[2], "+30 123") == 0 &&
                    strcmp(lines[3], "Ληψη") == 0,
                "Greek message-detail rows remain exact and ordered");
}

/* For a string that fits in max_lines at the given width, wrapping must PRESERVE
 * the glyph stream exactly (no glyph split, none dropped or added) and keep every
 * line within width and stride. */
static void check_wrap_preserves(uint8_t lang, uint16_t sid, const char *fallback,
                                  int width, const char *what) {
    strings_set_language(lang);
    const char *src = ts_or(sid, fallback);
    const font_t *font = asset_font(FONT_FS2);

    char lines[3][32];
    uint8_t count = wrap_text_lines_ex(font, src, width, (char *)lines, 32u, 3u);
    assert_true(count <= 3u, "wrap never returns more than max_lines");

    /* Reconstruct the codepoint stream from the wrapped lines (drop the spaces
     * wrap inserts/collapses) and compare to the source stream (drop spaces and
     * newlines). Equal => nothing was split or lost. */
    uint16_t got[128];
    int gn = 0;
    for (uint8_t i = 0; i < count; i++) {
        assert_true(strlen(lines[i]) <= 31u, "wrapped line fits stride-1");
        assert_true(is_codepoint_boundary(lines[i], (int)strlen(lines[i])),
                    "wrapped line ends on a codepoint boundary");
        assert_true(asset_text_width(font, lines[i]) <= width,
                    "wrapped line fits its pixel window");
        uint16_t tmp[128];
        int tn = codepoints_of(lines[i], tmp, 128, true);
        for (int k = 0; k < tn && gn < 128; k++) {
            got[gn++] = tmp[k];
        }
    }
    uint16_t want[128];
    int wn = codepoints_of(src, want, 128, true);
    if (gn != wn || memcmp(got, want, (size_t)gn * sizeof(uint16_t)) != 0) {
        fprintf(stderr, "  (%s: %d cps out, %d in)\n", what, gn, wn);
        assert_true(false, "wrap preserves the whole glyph stream (fits in 3 lines)");
    }
    strings_set_language(LANG_ENGL);
}

static void test_wrap_preserves_localized(void) {
    /* All chosen so the text fits in 3 lines at the given window width. */
    check_wrap_preserves(LANG_BULG, 0x17fu, "x", 84, "bulgarian 0x17f @84");
    check_wrap_preserves(LANG_GREE, 0x2a4u, "Requesting", 84, "greek Requesting @84");
    check_wrap_preserves(LANG_RUSS, 0x216u, "1\nmissed\ncall", 72, "russian missed @72");
    check_wrap_preserves(LANG_ENGL, 0x043u, "Activate\nphone\nfor calls?", 84, "english activate @84");
}

static void check_localized_wrap_golden(uint8_t lang,
                                        const char *const expected[3],
                                        const char *what) {
    strings_set_language(lang);
    const char *source = ts(0x043u); /* "Activate phone for calls?" */
    const font_t *font = asset_font(FONT_FS2);
    char lines[3][UI_WRAP_LINE_BYTES];
    uint8_t count = wrap_text_lines_ex(font, source, 84, (char *)lines,
                                       sizeof(lines[0]), 3u);
    if (count != 3u) {
        fprintf(stderr, "  (%s: got %u rows)\n", what, count);
        assert_true(false, "localized ground-truth prompt has three rows");
    } else {
        for (uint8_t row = 0u; row < 3u; row++) {
            if (strcmp(lines[row], expected[row]) != 0) {
                fprintf(stderr, "  (%s row %u: got [%s], want [%s])\n",
                        what, row, lines[row], expected[row]);
                assert_true(false,
                            "localized ground-truth prompt wraps exactly");
            }
            assert_true(asset_text_width(font, lines[row]) <= 84,
                        "localized golden row fits the LCD width");
        }
    }
    strings_set_language(LANG_ENGL);
}

static void test_localized_wrap_goldens(void) {
    static const char *const russian[3] = {
        "Включить", "телефон", "на прием?",
    };
    static const char *const greek[3] = {
        "Ενεργοποίηση", "τηλεφώνου για", "κλήσεις;",
    };
    static const char *const hungarian[3] = {
        "A telefon", "fogadjon", "hívásokat?",
    };
    check_localized_wrap_golden(LANG_RUSS, russian, "Russian SID 0x043");
    check_localized_wrap_golden(LANG_GREE, greek, "Greek SID 0x043");
    check_localized_wrap_golden(LANG_HUNG, hungarian,
                                "Hungarian SID 0x043");
}

static void test_wrap_serial_like_stock(void) {
    const char *imei = "490154203237518";
    const font_t *large = asset_font(FONT_FS0);
    const font_t *preview = asset_font(FONT_FS1);
    char lines[3][32];

    /* v6.00 record 0x1f is FS0, x=0, width=84, three rows. Each digit is
     * 8 px, so its generic scanner must split the raw 15-digit %S value 10+5. */
    uint8_t count = wrap_text_lines_ex(
        large, "Serial No.\n490154203237518", 84,
        (char *)lines, 32u, 3u);
    assert_eq_int(count, 3, "stock serial dialog occupies all three rows");
    assert_true(strcmp(lines[0], "Serial No.") == 0,
                "serial dialog keeps its title row");
    assert_true(strcmp(lines[1], "4901542032") == 0,
                "FS0 serial first row contains ten digits");
    assert_true(strcmp(lines[2], "37518") == 0,
                "FS0 serial second row retains the final five digits");
    assert_eq_int(asset_text_width(large, lines[1]), 80,
                  "ten FS0 digits fit the 84 px stock window");
    assert_true(asset_text_width(large, imei) > 84,
                "unsplit FS0 IMEI would be clipped");
    assert_true(asset_text_width(preview, imei) <= 78,
                "full FS1 IMEI fits the warranty-menu preview window");
}

static void test_wrap_serial_all_languages(void) {
    const char *imei = "490154203237518";
    const font_t *font = asset_font(FONT_FS0);

    for (uint8_t table = 0u; table < g_string_table_count; table++) {
        strings_set_language(g_string_tables[table].lang_id);
        const char *template = ts(0x18bu);
        const char *token = template != NULL ? strstr(template, "%S") : NULL;
        assert_true(token != NULL,
                    "localized serial template contains its Nokia %S token");
        if (token == NULL) {
            continue;
        }

        char composed[96];
        size_t prefix_len = (size_t)(token - template);
        size_t imei_len = strlen(imei);
        size_t suffix_len = strlen(token + 2);
        assert_true(prefix_len + imei_len + suffix_len < sizeof(composed),
                    "localized serial dialog fits composition buffer");
        if (prefix_len + imei_len + suffix_len >= sizeof(composed)) {
            continue;
        }
        memcpy(composed, template, prefix_len);
        memcpy(&composed[prefix_len], imei, imei_len);
        memcpy(&composed[prefix_len + imei_len], token + 2,
               suffix_len + 1u);

        char lines[3][32];
        uint8_t count = wrap_text_lines_ex(
            font, composed, 84, (char *)lines, 32u, 3u);
        assert_true(count > 0u && count <= 3u,
                    "localized serial dialog fits the stock three rows");

        uint16_t got[128];
        int got_count = 0;
        for (uint8_t line = 0u; line < count; line++) {
            assert_true(asset_text_width(font, lines[line]) <= 84,
                        "localized serial row fits the stock pixel window");
            uint16_t part[64];
            int part_count = codepoints_of(lines[line], part, 64, true);
            for (int i = 0; i < part_count && got_count < 128; i++) {
                got[got_count++] = part[i];
            }
        }
        uint16_t want[128];
        int want_count = codepoints_of(composed, want, 128, true);
        assert_true(got_count == want_count &&
                        memcmp(got, want,
                               (size_t)got_count * sizeof(got[0])) == 0,
                    "localized serial dialog retains title and all IMEI digits");
    }
    strings_set_language(LANG_ENGL);
}

/* Pixel fit and storage fit are independent. A caller whose row is too small
 * must get an explicit failure, never a second hidden wrap or a partial row. */
static void test_wrap_storage_is_explicit(void) {
    const font_t *font = asset_font(FONT_FS2);
    const char *narrow = "iiiiiiiiiiiiiiiiiiiiiiiiiii";
    assert_true(asset_text_width(font, narrow) <= 82,
                "27 narrow glyphs fit one LCD row in pixels");
    char short_row[2][17];
    assert_eq_int(wrap_text_lines_ex(font, narrow, 82, (char *)short_row,
                                     sizeof(short_row[0]), 2u),
                  0, "17-byte row rejects a visually fitting 27-byte line");
    char full_row[2][32];
    assert_eq_int(wrap_text_lines_ex(font, narrow, 82, (char *)full_row,
                                     sizeof(full_row[0]), 2u),
                  1, "adequate row storage preserves the same visual line");
    assert_true(strcmp(full_row[0], narrow) == 0,
                "adequate row storage retains every narrow glyph");

    char decoded_body[321];
    memset(decoded_body, 'i', 320u);
    decoded_body[320] = '\0';
    assert_eq_int(ui_wrap_line_count(font, decoded_body, 84), 12,
                  "maximum decoded SMS body remains fully scrollable");
    char tail[UI_WRAP_LINE_BYTES];
    assert_true(ui_wrap_line_at(font, decoded_body, 84, 11u,
                                tail, sizeof(tail)),
                "maximum decoded SMS exposes its final visual row");
    assert_eq_int((int)strlen(tail), 12,
                  "maximum decoded SMS tail retains its final 12 glyphs");
}

static uint8_t utf8_bytes_for_codepoint(uint16_t cp) {
    if (cp <= 0x7fu) {
        return 1u;
    }
    if (cp <= 0x7ffu) {
        return 2u;
    }
    return 3u;
}

static void test_wrap_line_capacity_tracks_fonts(void) {
    size_t worst = 0u;
    for (uint8_t font_id = 0u; font_id < FONT_COUNT; font_id++) {
        const font_t *font = asset_font((font_id_t)font_id);
        for (uint16_t i = 0u; i < font->glyph_count; i++) {
            const glyph_t *glyph = &font->glyphs[i];
            assert_true(glyph->width != 0u,
                        "generated wrap glyphs have a finite pixel density");
            if (glyph->width == 0u) {
                continue;
            }
            size_t count = 1u;
            size_t advance = (size_t)glyph->width + font->spacing;
            if (glyph->width <= 84u && advance != 0u) {
                count += (84u - glyph->width) / advance;
            }
            size_t bytes = count * utf8_bytes_for_codepoint(glyph->codepoint);
            if (bytes > worst) {
                worst = bytes;
            }
            assert_true(bytes + 1u <= UI_WRAP_LINE_BYTES,
                        "every generated repeated-glyph row fits UI_WRAP_LINE_BYTES");
        }
    }
    assert_eq_int((int)worst, 126,
                  "generated-font byte-density bound remains 126 bytes");
}

/* Guards: degenerate args must not crash, write, or hang. */
static void test_wrap_degenerate(void) {
    const font_t *font = asset_font(FONT_FS2);
    char lines[3][32];
    assert_eq_int(wrap_text_lines_ex(font, "hi", 60, (char *)lines, 32u, 0u), 0, "max_lines 0 -> 0");
    assert_eq_int(wrap_text_lines_ex(font, "", 60, (char *)lines, 32u, 3u), 0, "empty text -> 0 lines");
    /* stride < 4 cannot hold a worst-case codepoint + NUL. This MUST return 0
     * rather than spin forever on multibyte input (a 2-byte glyph can never fit
     * at word_len 0). Without the guard, this test hangs. */
    char tiny[3][3];
    strings_set_language(LANG_RUSS);
    assert_eq_int(wrap_text_lines_ex(font, ts_or(0x187u, "Calling"), 60, (char *)tiny, 3u, 3u), 0,
                  "stride 3 -> 0 (no infinite loop on multibyte)");
    strings_set_language(LANG_ENGL);
    assert_eq_int(wrap_text_lines_ex(font, "AB", 60, (char *)tiny, 3u, 3u), 1,
                  "stride 3 accepts exactly two ASCII bytes plus NUL");
    assert_true(strcmp(tiny[0], "AB") == 0,
                "exact-capacity ASCII row is preserved");
}

static void test_format_named(void) {
    char buf[16];
    /* Every token is substituted, position-agnostic. */
    ui_format_named(buf, sizeof(buf), "a %U b %U c", "X");
    assert_true(strcmp(buf, "a X b X c") == 0, "double token substitution");
    ui_format_named(buf, sizeof(buf), "no token", "X");
    assert_true(strcmp(buf, "no token") == 0, "token-free template copies verbatim");
    /* Truncation stays bounded and NUL-terminated, mid-value included. */
    ui_format_named(buf, 6u, "ab%Ucd", "12345678");
    assert_true(strcmp(buf, "ab123") == 0, "value truncation is bounded");
    ui_format_named(buf, 1u, "abc", "X");
    assert_true(strcmp(buf, "") == 0, "cap 1 yields an empty string");
    /* NULL inputs are inert. */
    ui_format_named(buf, sizeof(buf), 0, "X");
    assert_true(strcmp(buf, "") == 0, "NULL template");
    ui_format_named(buf, sizeof(buf), "%U!", 0);
    assert_true(strcmp(buf, "!") == 0, "NULL value substitutes empty");
    ui_format_named(0, 4u, "x", "y"); /* must not crash */
}

static void test_circular_list_step_3rows(void) {
    uint8_t selected = 2u;
    uint8_t start = 2u;

    ui_circular_list_step_3rows(5u, 1, &selected, &start);
    assert_eq_int(selected, 3, "circular Down selects the second row");
    assert_eq_int(start, 2, "circular Down keeps the entry viewport");
    ui_circular_list_step_3rows(5u, 1, &selected, &start);
    assert_eq_int(selected, 4, "second circular Down selects the third row");
    assert_eq_int(start, 2, "second circular Down still does not scroll");
    ui_circular_list_step_3rows(5u, 1, &selected, &start);
    assert_eq_int(selected, 0, "third circular Down wraps the selection");
    assert_eq_int(start, 3, "third circular Down scrolls by one row");

    ui_circular_list_step_3rows(5u, -1, &selected, &start);
    assert_eq_int(selected, 4, "circular Up selects the second row");
    assert_eq_int(start, 3, "circular Up keeps the viewport from row three");
    ui_circular_list_step_3rows(5u, -1, &selected, &start);
    assert_eq_int(selected, 3, "second circular Up selects the first row");
    assert_eq_int(start, 3, "second circular Up still does not scroll");
    ui_circular_list_step_3rows(5u, -1, &selected, &start);
    assert_eq_int(selected, 2, "third circular Up selects the preceding item");
    assert_eq_int(start, 2, "third circular Up scrolls by one row");

    selected = 1u;
    start = 1u;
    ui_circular_list_step_3rows(3u, -1, &selected, &start);
    assert_eq_int(selected, 0, "three-item Up changes selection");
    assert_eq_int(start, 1, "complete three-item viewport never scrolls");
}

static void test_flat_list_circular_view(void) {
    static const char *const labels[] = {
        "Alpha", "Bravo", "Charlie", "Delta", "Echo",
    };
    static const char *const rotated[] = {
        "Delta", "Echo", "Alpha", "Bravo", "Charlie",
    };
    framebuffer_t actual;
    framebuffer_t expected_rows;
    framebuffer_t expected_indicator;

    draw_flat_list_circular_view(&actual, labels, 5u, 0u, 3u,
                                 "9-2-1", "OK");
    draw_flat_list(&expected_rows, rotated, 5u, 2u, "9-2-1", "OK");
    fb_clear(&expected_indicator, false);
    draw_position_indicator(&expected_indicator, 0u, 5u, "9-2-1");

    bool rows_match = true;
    for (int y = 8; y < 38; y++) {
        for (int x = 0; x < 78; x++) {
            rows_match = rows_match &&
                fb_get_pixel(&actual, x, y) == fb_get_pixel(&expected_rows, x, y);
        }
    }
    assert_true(rows_match,
                "circular list renders three rows from its wrapped viewport");
    bool indicator_matches = true;
    for (int y = 0; y < 38; y++) {
        for (int x = 81; x < 84; x++) {
            indicator_matches = indicator_matches &&
                fb_get_pixel(&actual, x, y) ==
                    fb_get_pixel(&expected_indicator, x, y);
        }
    }
    assert_true(indicator_matches,
                "circular list keeps the selected logical scrollbar ordinal");
}

int main(void) {
    test_format_named();
    test_circular_list_step_3rows();
    test_flat_list_circular_view();
    test_copy_text();
    test_glyph_line_spans();
    test_wrap_honors_newline();
    test_dynamic_wrap_preserves_four_detail_rows();
    test_wrap_preserves_localized();
    test_localized_wrap_goldens();
    test_wrap_serial_like_stock();
    test_wrap_serial_all_languages();
    test_wrap_storage_is_explicit();
    test_wrap_line_capacity_tracks_fonts();
    test_wrap_degenerate();

    if (s_failures != 0) {
        fprintf(stderr, "%d failures\n", s_failures);
        return 1;
    }
    printf("ui_wrap tests passed\n");
    return 0;
}
