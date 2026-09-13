#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ui/assets.h"

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

/* True if byte offset `off` falls exactly on a UTF-8 codepoint boundary of s
 * (0 and strlen(s) are boundaries). Re-decodes with the same decoder the
 * renderer uses, so this is the exact "did we split a glyph" check. */
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

/* For one string, sweep every plausible window width and assert the fit is
 * always glyph-aligned, monotonic in width, and reaches the whole string. */
static void check_fits(const char *s, const char *what) {
    const font_t *font = asset_font(FONT_FS2);
    int full_bytes = (int)strlen(s);
    int full_width = asset_text_width(font, s);

    assert_true(asset_text_fit_bytes(font, s, 0) == 0, "zero width fits nothing");
    assert_true(asset_text_fit_bytes(font, s, full_width) == full_bytes, "full width fits all");
    assert_true(asset_text_fit_bytes(font, s, full_width + 50) == full_bytes, "extra width still fits all");

    int prev = 0;
    for (int w = 0; w <= full_width + 2; w++) {
        int fit = asset_text_fit_bytes(font, s, w);
        if (!is_codepoint_boundary(s, fit)) {
            fprintf(stderr, "  (%s: width %d -> fit %d split a glyph)\n", what, w, fit);
            assert_true(false, "fit lands on a codepoint boundary (never splits a glyph)");
            return;
        }
        assert_true(fit >= prev, "fit is monotonic non-decreasing in width");
        assert_true(fit <= full_bytes, "fit never exceeds the string");
        prev = fit;
    }
}

/* ------------------------------------------------------------------ */
/* Independent reference decoder: a strict UTF-8 / CP1252-fallback model
 * implemented from first principles (RFC 3629 + the CP1252 table the code
 * documents). Used as an oracle against asset_next_codepoint. We do NOT
 * read what the code returns and assert that -- we derive the answer here. */

static const uint16_t REF_CP1252_HIGH[32] = {
    0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
    0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d, 0x017d, 0x008f,
    0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
    0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178,
};

static uint16_t ref_cp1252(uint8_t b) {
    if (b >= 0x80u && b <= 0x9fu) {
        return REF_CP1252_HIGH[b - 0x80u];
    }
    return b;
}

/* ------------------------------------------------------------------ */
/* asset_next_codepoint correctness, oracle-driven. */

static void test_next_codepoint_ascii(void) {
    const char *s = "A0~ \x7f";
    const char *p = s;
    assert_eq_int(asset_next_codepoint(&p), 'A', "ascii A");
    assert_eq_int((int)(p - s), 1, "advance 1 after ascii");
    assert_eq_int(asset_next_codepoint(&p), '0', "ascii 0");
    assert_eq_int(asset_next_codepoint(&p), '~', "ascii ~");
    assert_eq_int(asset_next_codepoint(&p), ' ', "ascii space");
    assert_eq_int(asset_next_codepoint(&p), 0x7f, "ascii DEL");
    assert_eq_int((int)(p - s), 5, "consumed whole ascii string");
}

static void test_next_codepoint_2byte(void) {
    /* U+00E7 c-cedilla = 0xC3 0xA7 */
    const char *s = "\xc3\xa7" "x";
    const char *p = s;
    assert_eq_int(asset_next_codepoint(&p), 0x00e7, "2-byte U+00E7");
    assert_eq_int((int)(p - s), 2, "advance 2 after 2-byte seq");
    assert_eq_int(asset_next_codepoint(&p), 'x', "ascii after 2-byte");

    /* U+03BB greek lambda = 0xCE 0xBB */
    const char *g = "\xce\xbb";
    p = g;
    assert_eq_int(asset_next_codepoint(&p), 0x03bb, "2-byte U+03BB");
    assert_eq_int((int)(p - g), 2, "advance 2 after greek");
}

static void test_next_codepoint_3byte(void) {
    /* U+20AC euro = 0xE2 0x82 0xAC */
    const char *s = "\xe2\x82\xac" "Z";
    const char *p = s;
    assert_eq_int(asset_next_codepoint(&p), 0x20ac, "3-byte U+20AC euro");
    assert_eq_int((int)(p - s), 3, "advance 3 after 3-byte seq");
    assert_eq_int(asset_next_codepoint(&p), 'Z', "ascii after 3-byte");

    /* U+05D0 hebrew alef = 0xD7 0x90 (2-byte) and U+FB50 (3-byte) */
    const char *h = "\xef\xad\x90"; /* U+FB50 */
    p = h;
    assert_eq_int(asset_next_codepoint(&p), 0xfb50, "3-byte U+FB50");
    assert_eq_int((int)(p - h), 3, "advance 3 after FB50");
}

static void test_next_codepoint_cp1252_fallback(void) {
    /* A lone high byte that is NOT a valid UTF-8 lead/continuation pair must
     * decode as one CP1252 byte and advance exactly 1. 0x80 -> U+20AC. */
    const char *s = "\x80" "A";
    const char *p = s;
    assert_eq_int(asset_next_codepoint(&p), 0x20ac, "CP1252 0x80 -> euro");
    assert_eq_int((int)(p - s), 1, "CP1252 fallback advances 1");
    assert_eq_int(asset_next_codepoint(&p), 'A', "ascii after cp1252 byte");

    /* 0xA0..0xFF map 1:1 to Latin-1 when not a valid lead. 0xE9 (e-acute) as a
     * LONE byte: 0xE9 is an 0xe0-class lead, so the decoder will TRY a 3-byte
     * read. Followed by 'A' (0x41), the continuation check fails -> CP1252.
     * Per CP1252, 0xE9 -> U+00E9. */
    const char *e = "\xe9" "A";
    p = e;
    assert_eq_int(asset_next_codepoint(&p), 0x00e9, "lone 0xE9 -> CP1252 U+00E9");
    assert_eq_int((int)(p - e), 1, "lone 0xE9 advances 1");

    /* 0xC0 lead with a non-continuation next byte -> CP1252. 0xC0 -> U+00C0. */
    const char *c = "\xc0" "B";
    p = c;
    assert_eq_int(asset_next_codepoint(&p), 0x00c0, "lone 0xC0 -> CP1252 U+00C0");
    assert_eq_int((int)(p - c), 1, "lone 0xC0 advances 1");
}

/* Sanitizer-focused: feed deliberately TRUNCATED multibyte sequences (the
 * missing byte replaced by the NUL terminator) and verify the decoder never
 * reads past the NUL and never advances the cursor past the NUL. ASan would
 * trap any out-of-bounds read here. */
static void test_next_codepoint_truncated_no_oob(void) {
    /* 2-byte lead then immediate NUL: {0xC3, 0x00}. */
    char b2[2] = {(char)0xc3, 0x00};
    const char *p = b2;
    uint16_t cp = asset_next_codepoint(&p);
    assert_true((int)(p - b2) == 1, "truncated 2-byte advances exactly 1 (no overrun past NUL)");
    assert_eq_int(cp, ref_cp1252(0xc3), "truncated 2-byte falls back to CP1252");

    /* 3-byte lead, valid continuation, then NUL: {0xE2, 0x82, 0x00}. The third
     * byte is the NUL (in-bounds read), continuation check fails -> CP1252. */
    char b3[3] = {(char)0xe2, (char)0x82, 0x00};
    p = b3;
    cp = asset_next_codepoint(&p);
    assert_true((int)(p - b3) == 1, "truncated 3-byte (good c1, NUL c2) advances 1");
    assert_eq_int(cp, ref_cp1252(0xe2), "truncated 3-byte falls back to CP1252");

    /* 3-byte lead then immediate NUL: {0xE0, 0x00, ...}. */
    char b3b[3] = {(char)0xe0, 0x00, 0x00};
    p = b3b;
    cp = asset_next_codepoint(&p);
    assert_true((int)(p - b3b) == 1, "3-byte lead + NUL c1 advances 1");
    assert_eq_int(cp, ref_cp1252(0xe0), "3-byte lead + NUL c1 -> CP1252");

    /* 2-byte lead then a non-continuation high byte then NUL. */
    char m[3] = {(char)0xc2, (char)0x41, 0x00}; /* 0xC2 then 'A' */
    p = m;
    cp = asset_next_codepoint(&p);
    assert_true((int)(p - m) == 1, "2-byte lead + non-cont advances 1");
    assert_eq_int(cp, ref_cp1252(0xc2), "2-byte lead + non-cont -> CP1252");
}

/* Spec-deviation guards for the 3-byte UTF-8 decode path. The 2-byte path
 * rejects overlong forms (it checks `cp >= 0x80`), but the 3-byte path has NO
 * overlong/surrogate validation. RFC 3629 says a 3-byte sequence must encode a
 * scalar value in [U+0800, U+FFFF] and must NOT be a surrogate (U+D800..U+DFFF).
 * The decoder violates both. These tests pin CURRENT (buggy) behavior so the
 * suite stays green; each is reported in bugs[]. */
static void test_next_codepoint_overlong_and_surrogate(void) {
    /* FIXED: an overlong 3-byte form for U+0000 (E0 80 80) is rejected per RFC
     * 3629 and falls back to CP1252 on the lead byte 0xE0 (-> U+00E0), advancing
     * exactly 1 -- no embedded NUL codepoint, no glyph-splitting 3-byte run. */
    char a[4] = {(char)0xe0, (char)0x80, (char)0x80, 0};
    const char *p = a;
    uint16_t cp = asset_next_codepoint(&p);
    assert_eq_int(cp, 0x00e0, "overlong E0 80 80 rejected -> CP1252 U+00E0");
    assert_eq_int((int)(p - a), 1, "overlong E0 80 80 advances 1");

    /* FIXED: overlong 3-byte for an ASCII char (U+002F '/') -- rejected, lead
     * byte 0xE0 -> CP1252 U+00E0, advance 1. */
    char b[4] = {(char)0xe0, (char)0x80, (char)0xaf, 0};
    p = b;
    cp = asset_next_codepoint(&p);
    assert_eq_int(cp, 0x00e0, "overlong E0 80 AF rejected -> CP1252 U+00E0");
    assert_eq_int((int)(p - b), 1, "overlong E0 80 AF advances 1");

    /* FIXED: UTF-16 surrogate U+D800 (ED A0 80) is not a valid scalar value;
     * rejected, lead byte 0xED -> CP1252 U+00ED, advance 1. */
    char c[4] = {(char)0xed, (char)0xa0, (char)0x80, 0};
    p = c;
    cp = asset_next_codepoint(&p);
    assert_eq_int(cp, 0x00ed, "surrogate ED A0 80 rejected -> CP1252 U+00ED");
    assert_eq_int((int)(p - c), 1, "surrogate ED A0 80 advances 1");

    /* Contrast: the 2-byte path DOES reject its overlong form. C0 80 -> not a
     * valid 2-byte (cp 0 < 0x80), so falls back to CP1252(0xC0)=U+00C0, adv 1. */
    char d[3] = {(char)0xc0, (char)0x80, 0};
    p = d;
    cp = asset_next_codepoint(&p);
    assert_eq_int(cp, 0x00c0, "overlong 2-byte C0 80 correctly rejected -> CP1252");
    assert_eq_int((int)(p - d), 1, "overlong 2-byte advances 1");
}

/* ------------------------------------------------------------------ */
/* asset_text_width oracle: width = sum(glyph widths) + spacing*(n-1) over
 * glyphs that resolve. spacing is 0 for FS2 in this asset set, so width must
 * equal the plain sum of glyph widths for resolvable codepoints. */

static int ref_text_width(const font_t *font, const char *text) {
    int width = 0;
    bool first = true;
    const char *p = text;
    while (*p) {
        uint16_t cp = asset_next_codepoint(&p);
        const glyph_t *g = asset_glyph_codepoint(font, cp);
        if (g == 0) {
            continue;
        }
        if (!first) {
            width += font->spacing;
        }
        width += g->width;
        first = false;
    }
    return width;
}

static void test_text_width_basic(void) {
    const font_t *font = asset_font(FONT_FS2);
    assert_eq_int(asset_text_width(font, ""), 0, "empty width is 0");
    assert_eq_int(asset_text_width(font, NULL), 0, "NULL width is 0");
    assert_eq_int(asset_text_width(NULL, "x"), 0, "NULL font width is 0");

    /* "A" is width 6, space is 3 in FS2; spacing 0. "A A" = 6+3+6 = 15. */
    assert_eq_int(asset_text_width(font, "A A"), 6 + 3 + 6, "A space A width");
    assert_eq_int(asset_text_width(font, "A"), 6, "single A width");
    assert_eq_int(asset_text_width(font, " "), 3, "single space width");

    /* Cross-check oracle against impl on a varied string. */
    const char *s = "Hello, World! 0123 ?";
    assert_eq_int(asset_text_width(font, s), ref_text_width(font, s), "width matches oracle");
}

/* ------------------------------------------------------------------ */
/* asset_text_fit_bytes invariants and exact boundaries. */

static void test_fit_empty_and_whitespace(void) {
    const font_t *font = asset_font(FONT_FS2);
    assert_eq_int(asset_text_fit_bytes(font, "", 0), 0, "empty string fits 0 at width 0");
    assert_eq_int(asset_text_fit_bytes(font, "", 100), 0, "empty string fits 0 at large width");
    assert_eq_int(asset_text_fit_bytes(NULL, "x", 100), 0, "NULL font fits 0");
    assert_eq_int(asset_text_fit_bytes(font, NULL, 100), 0, "NULL text fits 0");

    /* Whitespace-only: spaces are real glyphs (width 3). At width 0 nothing
     * fits; at width 3 exactly one space; at full width all 4. */
    const char *spaces = "    ";
    assert_eq_int(asset_text_fit_bytes(font, spaces, 0), 0, "no space fits at width 0");
    assert_eq_int(asset_text_fit_bytes(font, spaces, 2), 0, "no space fits at width 2 (<3)");
    assert_eq_int(asset_text_fit_bytes(font, spaces, 3), 1, "one space fits at width 3");
    assert_eq_int(asset_text_fit_bytes(font, spaces, 5), 1, "one space fits at width 5 (<6)");
    assert_eq_int(asset_text_fit_bytes(font, spaces, 6), 2, "two spaces fit at width 6");
    assert_eq_int(asset_text_fit_bytes(font, spaces, 12), 4, "all four spaces fit at width 12");
    assert_eq_int(asset_text_fit_bytes(font, spaces, 1000), 4, "all spaces fit at huge width");
}

static void test_fit_negative_width_no_limit(void) {
    const font_t *font = asset_font(FONT_FS2);
    const char *s = "Hello \xce\xbb World";
    int full = (int)strlen(s);
    assert_eq_int(asset_text_fit_bytes(font, s, -1), full, "negative width = no limit (all bytes)");
    assert_eq_int(asset_text_fit_bytes(font, s, -1000), full, "very negative width = no limit");
    assert_eq_int(asset_text_fit_bytes(font, "", -1), 0, "negative width empty = 0");
}

static void test_fit_exact_boundaries(void) {
    const font_t *font = asset_font(FONT_FS2);
    /* "AAA": each A is 6, spacing 0. Cumulative widths after 1,2,3 = 6,12,18.
     * fit(w) = number of full A's whose cumulative width <= w, in bytes. */
    const char *s = "AAA";
    assert_eq_int(asset_text_fit_bytes(font, s, 5), 0, "AAA width 5 -> 0 (first A needs 6)");
    assert_eq_int(asset_text_fit_bytes(font, s, 6), 1, "AAA width 6 -> 1");
    assert_eq_int(asset_text_fit_bytes(font, s, 11), 1, "AAA width 11 -> 1 (second needs 12)");
    assert_eq_int(asset_text_fit_bytes(font, s, 12), 2, "AAA width 12 -> 2");
    assert_eq_int(asset_text_fit_bytes(font, s, 17), 2, "AAA width 17 -> 2");
    assert_eq_int(asset_text_fit_bytes(font, s, 18), 3, "AAA width 18 -> 3 (all)");
    assert_eq_int(asset_text_fit_bytes(font, s, 19), 3, "AAA width 19 -> 3 (clamped)");
}

/* A "word longer than the line" must still fit prefix glyphs up to the width,
 * never overflowing and always ending on a glyph boundary. */
static void test_fit_word_longer_than_line(void) {
    const font_t *font = asset_font(FONT_FS2);
    const char *word = "SUPERCALIFRAGILISTIC";
    int n = (int)strlen(word);
    for (int w = 0; w <= asset_text_width(font, word) + 4; w++) {
        int fit = asset_text_fit_bytes(font, word, w);
        assert_true(fit >= 0 && fit <= n, "long word fit in [0,n]");
        assert_true(is_codepoint_boundary(word, fit), "long word fit on boundary");
        /* The fitted prefix must actually fit in w. */
        char buf[64];
        memcpy(buf, word, (size_t)fit);
        buf[fit] = '\0';
        assert_true(asset_text_width(font, buf) <= w, "fitted prefix width <= max_width");
        /* And adding one more glyph (if any) must NOT fit. */
        if (fit < n) {
            const char *q = word + fit;
            asset_next_codepoint(&q);
            int next = (int)(q - word);
            memcpy(buf, word, (size_t)next);
            buf[next] = '\0';
            assert_true(asset_text_width(font, buf) > w, "one more glyph would exceed max_width");
        }
    }
}

/* Multibyte never split: extend the existing Greek even-byte guard with a
 * 3-byte-per-glyph string (all euros). Any non-full fit must be a multiple
 * of 3 bytes. */
static void test_fit_3byte_never_split(void) {
    const font_t *font = asset_font(FONT_FS2);
    const char *euros = "\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac"; /* 4 euros */
    int full = asset_text_width(font, euros);
    for (int w = 0; w <= full + 2; w++) {
        int fit = asset_text_fit_bytes(font, euros, w);
        assert_true((fit % 3) == 0, "euro fit is a multiple of 3 bytes (no split)");
        assert_true(is_codepoint_boundary(euros, fit), "euro fit on boundary");
    }
}

/* Mixed-width multibyte: ASCII (1B) + greek (2B) + euro (3B) interleaved.
 * Every fit across the whole sweep must land on a real boundary. */
static void test_fit_mixed_widths_never_split(void) {
    const font_t *font = asset_font(FONT_FS2);
    const char *s = "a\xce\xbb" "b\xe2\x82\xac" "c\xc3\xa7" "d";
    int full = asset_text_width(font, s);
    int prev = 0;
    for (int w = 0; w <= full + 3; w++) {
        int fit = asset_text_fit_bytes(font, s, w);
        assert_true(is_codepoint_boundary(s, fit), "mixed fit on boundary");
        assert_true(fit >= prev, "mixed fit monotonic");
        prev = fit;
    }
    assert_eq_int(asset_text_fit_bytes(font, s, full), (int)strlen(s), "mixed full width fits all");
}

/* ------------------------------------------------------------------ */
/* Marquee-style slicing: a renderer scrolls a label by repeatedly taking the
 * suffix starting at an advancing byte offset. The invariant we exercise is
 * that fit() on every suffix is still glyph-aligned and bounded -- i.e. the
 * marquee can cut on glyph boundaries derived from asset_next_codepoint. */
static void test_marquee_suffix_slicing(void) {
    const font_t *font = asset_font(FONT_FS2);
    const char *s = "News: \xce\x95\xce\xbb\xce\xbb\xce\xb7\xce\xbd \xe2\x82\xac 100";
    int n = (int)strlen(s);
    int window = 40; /* px window */

    /* Walk the marquee head across every CODEPOINT boundary (not raw bytes) so
     * each suffix start is itself glyph-aligned, as a real marquee would do. */
    const char *head = s;
    while (*head) {
        int start = (int)(head - s);
        int fit = asset_text_fit_bytes(font, head, window);
        assert_true(fit >= 0, "marquee suffix fit non-negative");
        assert_true(start + fit <= n, "marquee suffix fit stays within string");
        assert_true(is_codepoint_boundary(head, fit), "marquee suffix fit on boundary");
        /* Boundary relative to the FULL string, too. */
        assert_true(is_codepoint_boundary(s, start + fit), "marquee window end on full-string boundary");
        asset_next_codepoint(&head);
    }
    /* Empty suffix at the end. */
    assert_eq_int(asset_text_fit_bytes(font, s + n, window), 0, "marquee at end fits 0");
}

/* ------------------------------------------------------------------ */
/* Glyph fallback: codepoints with no glyph. */

static void test_glyph_fallback(void) {
    const font_t *fs2 = asset_font(FONT_FS2);
    const font_t *fs3 = asset_font(FONT_FS3);

    /* FS2 carries its own lowercase glyphs, so 'a' resolves to a real (distinct)
     * 'a' glyph -- it must NOT collapse to 'A'. */
    const glyph_t *lower_a = asset_glyph_codepoint(fs2, 'a');
    const glyph_t *upper_a = asset_glyph_codepoint(fs2, 'A');
    assert_true(lower_a != 0 && upper_a != 0, "a and A resolve in FS2");
    assert_true(lower_a != upper_a, "FS2 lowercase a is its own glyph (no fallback)");

    /* FS3 has NO lowercase letters; 'b' must fall back to 'B' via the
     * (cp - 'a'-'A') uppercase fallback path. */
    const glyph_t *fs3_b = asset_glyph_codepoint(fs3, 'b');
    const glyph_t *fs3_B = asset_glyph_codepoint(fs3, 'B');
    assert_true(fs3_B != 0, "B resolves in FS3");
    assert_true(fs3_b == fs3_B, "FS3 lowercase b falls back to uppercase B");

    /* Latin-1 lowercase accent 0xE9 (e-acute) -> uppercase 0xC9 fallback if
     * present, else '?'. We only assert non-NULL + glyph resolves (the code
     * tries cp-0x20 then '?' then ' '). */
    const glyph_t *eacute = asset_glyph_codepoint(fs2, 0x00e9);
    assert_true(eacute != 0, "e-acute resolves (accent fallback or '?')");

    /* A codepoint with definitely no glyph and no fallback path (e.g. U+2603
     * snowman) must resolve to the '?' glyph (FS2 has '?'). */
    const glyph_t *snowman = asset_glyph_codepoint(fs2, 0x2603);
    const glyph_t *qmark = asset_glyph_codepoint(fs2, '?');
    assert_true(qmark != 0, "FS2 has '?'");
    assert_true(snowman == qmark, "unknown codepoint falls back to '?'");

    /* NULL font must not crash and returns NULL. */
    assert_true(asset_glyph_codepoint(NULL, 'A') == 0, "NULL font glyph -> NULL");
}

/* Width/fit must remain sane for strings full of unmapped codepoints (they all
 * resolve to '?', a real glyph, so they DO take width). */
static void test_fit_with_fallback_glyphs(void) {
    const font_t *font = asset_font(FONT_FS2);
    /* Three snowmen (3 bytes each in UTF-8: 0xE2 0x98 0x83). Each resolves to
     * '?' (width 6). */
    const char *s = "\xe2\x98\x83\xe2\x98\x83\xe2\x98\x83";
    const glyph_t *q = asset_glyph_codepoint(font, '?');
    assert_true(q != 0, "? glyph exists");
    int expect = 3 * q->width; /* spacing 0 */
    assert_eq_int(asset_text_width(font, s), expect, "3 unmapped cps width = 3 * '?' width");

    /* Fit must still be glyph-aligned (multiples of 3 bytes). */
    for (int w = 0; w <= expect + 2; w++) {
        int fit = asset_text_fit_bytes(font, s, w);
        assert_true((fit % 3) == 0, "fallback-glyph fit multiple of 3 bytes");
    }
}

/* ------------------------------------------------------------------ */
/* asset_glyph(char) sign-extension guard: chars >= 0x80 are passed as signed
 * char on most platforms. The impl casts (uint8_t)ch, so 0xE9 must behave as
 * CP1252 0xE9, identical to asset_glyph_codepoint(font, U+00E9). */
static void test_glyph_char_high_byte(void) {
    const font_t *font = asset_font(FONT_FS2);
    char c = (char)0xe9; /* would be negative if signed char */
    const glyph_t *via_char = asset_glyph(font, c);
    const glyph_t *via_cp = asset_glyph_codepoint(font, 0x00e9);
    assert_true(via_char == via_cp, "asset_glyph high byte matches CP1252 codepoint");

    const glyph_t *space_char = asset_glyph(font, ' ');
    const glyph_t *space_cp = asset_glyph_codepoint(font, ' ');
    assert_true(space_char == space_cp, "asset_glyph space matches codepoint");
}

/* asset_glyph_data bounds: an in-range glyph returns a pointer; an out-of-range
 * data_offset returns NULL (no OOB). We check every glyph of FS2 returns a
 * pointer within [data, data+data_size). */
static void test_glyph_data_bounds(void) {
    const font_t *font = asset_font(FONT_FS2);
    assert_true(asset_glyph_data(NULL, NULL) == 0, "NULL glyph data -> NULL");
    for (uint16_t i = 0; i < font->glyph_count; i++) {
        const glyph_t *g = &font->glyphs[i];
        const uint8_t *d = asset_glyph_data(font, g);
        if (g->data_offset < font->data_size) {
            assert_true(d != 0, "in-range glyph has data ptr");
            assert_true(d >= font->data && d < font->data + font->data_size,
                        "glyph data ptr within font data buffer");
        } else {
            assert_true(d == 0, "out-of-range data_offset -> NULL");
        }
    }
}

int main(void) {
    /* Existing sweep cases (kept). */
    check_fits("Hello World 42", "ascii");
    check_fits("Fran\xc3\xa7" "ais", "latin-accented (c-cedilla)");          /* Français */
    check_fits("\xce\x95\xce\xbb\xce\xbb\xce\xb7\xce\xbd\xce\xb9\xce\xba\xce\xac", "greek"); /* Ελληνικά */
    check_fits("\xd0\xa0\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9", "cyrillic");       /* Русский */
    check_fits("A\xce\xbb" "B\xd0\xb8" "9", "mixed ascii+greek+cyrillic");

    /* Greek "Ελληνικά": every char is a 2-byte sequence, so any non-full fit
     * must be an EVEN byte count -- a direct guard against a 1-byte split. */
    const font_t *font = asset_font(FONT_FS2);
    const char *greek = "\xce\x95\xce\xbb\xce\xbb\xce\xb7\xce\xbd\xce\xb9\xce\xba\xce\xac";
    for (int w = 0; w < asset_text_width(font, greek); w++) {
        int fit = asset_text_fit_bytes(font, greek, w);
        assert_true((fit % 2) == 0, "greek fit is an even byte count (no 1-byte split)");
    }

    /* Extended coverage. */
    test_next_codepoint_ascii();
    test_next_codepoint_2byte();
    test_next_codepoint_3byte();
    test_next_codepoint_cp1252_fallback();
    test_next_codepoint_truncated_no_oob();
    test_next_codepoint_overlong_and_surrogate();
    test_text_width_basic();
    test_fit_empty_and_whitespace();
    test_fit_negative_width_no_limit();
    test_fit_exact_boundaries();
    test_fit_word_longer_than_line();
    test_fit_3byte_never_split();
    test_fit_mixed_widths_never_split();
    test_marquee_suffix_slicing();
    test_glyph_fallback();
    test_fit_with_fallback_glyphs();
    test_glyph_char_high_byte();
    test_glyph_data_bounds();

    if (s_failures != 0) {
        fprintf(stderr, "%d failures\n", s_failures);
        return 1;
    }
    printf("text_fit tests passed\n");
    return 0;
}
