/* Host render harness: exercises pico's real framebuffer/assets/ui/status
 * rendering primitives to produce screenshots of the binary-derived UI
 * changes, so they can be eyeballed without flashing hardware. Each screen
 * reproduces the exact draw calls from the corresponding render_*() body, so
 * the output is pixel-identical to what the firmware draws.
 *
 * Build: see tools/render_harness/build.sh. Output: raw 84x48 dumps under
 * tools/render_harness/out/, converted to scaled PNGs by make_pngs.py. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ui/assets.h"
#include "ui/framebuffer.h"
#include "ui/status_chrome.h"
#include "ui/ui.h"

/* timebase stub: the marquee reads a clock; a fixed value gives step 0. */
uint32_t time_ms(void) { return 0u; }
int32_t time_diff_ms(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline);
}

static void dump(const framebuffer_t *fb, const char *name) {
    char path[256];
    snprintf(path, sizeof(path), "tools/render_harness/out/%s.pbm", name);
    FILE *f = fopen(path, "wb");
    if (f == 0) {
        fprintf(stderr, "cannot write %s\n", path);
        return;
    }
    /* PBM P1: 1 = black pixel (LCD on). fb pixel true = lit. */
    fprintf(f, "P1\n%u %u\n", (unsigned)FB_WIDTH, (unsigned)FB_HEIGHT);
    for (int y = 0; y < (int)FB_HEIGHT; y++) {
        for (int x = 0; x < (int)FB_WIDTH; x++) {
            fputc(fb_get_pixel(fb, x, y) ? '1' : '0', f);
            fputc(' ', f);
        }
        fputc('\n', f);
    }
    fclose(f);
    printf("  %s\n", name);
}

/* ---- screens ---- */

/* Main-menu title: hardware-validated FS0 large/bold, centered with the +1px
 * ceiling bias (render_main_menu; MENU_TEXT_X=0 Y=7 W=76). A 2026-06-10 FS2
 * "binary pin" was wrong — the menu composer overrides the window font; the
 * handset shows FS0. */
static void screen_menu_title(const char *label, const char *name) {
    framebuffer_t fb;
    fb_clear(&fb, false);
    const font_t *f = asset_font(FONT_FS0);
    int w = asset_text_width(f, label);
    int x = 0 + ((76 - w + 1) / 2) + 1;
    if (x < 0) x = 0;
    fb_text(&fb, f, label, x, 7, true, 76 - x);
    /* the 64x14 animated selector icon would sit at x10 y23 below the title. */
    fb_rect(&fb, 10, 23, 64, 14, true);
    draw_softkey(&fb, "Select");
    dump(&fb, name);
}

/* C7c native-script language list via the real draw_flat_list + marquee. */
static void screen_language_list(uint8_t selected, const char *name) {
    framebuffer_t fb;
    static const char *const LANGS[] = {
        "English", "Deutsch", "Fran\xc3\xa7" "ais", "\xce\x95\xce\xbb\xce\xbb\xce\xb7\xce\xbd\xce\xb9\xce\xba\xce\xac",
        "\xd0\x91\xd1\x8a\xd0\xbb\xd0\xb3\xd0\xb0\xd1\x80\xd1\x81\xd0\xba\xd0\xb8", "Magyar", "Rom\xc3\xa2n\xc4\x83", "Polski",
        "\xc4\x8c" "e\xc5\xa1tina", "Sloven\xc4\x8dina", "Hrvatski", "Srpski",
        "Sloven\xc5\xa1\xc4\x8dina", "\xd0\xa0\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9", "Eesti", "Latvie\xc5\xa1u",
        "Lietuvi\xc5\xb3", "\xef\xba\x94\xef\xbb\xb4\xef\xba\x91\xef\xba\xae\xef\xbb\x8c\xef\xbb\x9f\xef\xba\x8d", "\xd7\xaa\xd7\x99\xd7\xa8\xd7\x91\xd7\xa2",
    };
    draw_flat_list(&fb, LANGS, (uint8_t)(sizeof(LANGS) / sizeof(LANGS[0])), selected, "4-1", "Select");
    dump(&fb, name);
}

/* C7c special-character grid (render_sms_symbols, filled-cell selection). */
static void screen_special_chars(uint8_t selected, const char *name) {
    framebuffer_t fb;
    fb_clear(&fb, false);
    const font_t *font = asset_font(FONT_FS2);
    fb_bitmap(&fb, 8u, 0, 0, true, true);
    fb_bitmap(&fb, 13u, 21, 0, true, true);
    static const char CH[] = ".,?!:;-+#*()'\"_@&$\xc2\xa3%/<>\xc2\xbf\xc2\xa1\xc2\xa7=\xc2\xa4\xe2\x82\xac\xc2\xa5";
    const char *p = CH;
    for (uint8_t i = 0; i < 30u && *p != '\0'; i++) {
        char glyph[5] = {0};
        int n = (*p & 0x80) ? ((*p & 0x20) ? 3 : 2) : 1;
        memcpy(glyph, p, n);
        p += n;
        int x = 2 + (i % 10u) * 8;
        int y = 8 + (i / 10u) * 9;
        bool sel = i == selected;
        if (sel) {
            fb_fill_rect(&fb, x - 1, y - 1, 8, 9, true);
        }
        int w = asset_text_width(font, glyph);
        fb_text(&fb, font, glyph, x + ((7 - w) / 2), y, !sel, 7);
    }
    draw_softkey(&fb, "Use");
    dump(&fb, name);
}

/* C7c accent multi-tap output sample. */
static void screen_accents(const char *name) {
    framebuffer_t fb;
    fb_clear(&fb, false);
    const font_t *f = asset_font(FONT_FS2);
    fb_text(&fb, f, "\xc3\x84\xc3\x85\xc3\x86\xc3\x87 \xc3\x89 \xc2\xa3 $", 1, 2, true, 82);   /* ÄÅÆÇ É £ $ */
    fb_text(&fb, f, "\xc3\xa4\xc3\xa5\xc3\xa0\xc3\xa6 \xc3\xa9\xc3\xa8", 1, 11, true, 82);       /* äåàæ éè */
    fb_text(&fb, f, "\xc3\xb1\xc3\xb6\xc3\xb8\xc3\xb2 \xc3\x9f", 1, 20, true, 82);               /* ñöøò ß */
    fb_text(&fb, f, "\xc3\xbc\xc3\xb9 \xc2\xbf\xc2\xa1\xc2\xa7 \xc2\xa4\xe2\x82\xac\xc2\xa5", 1, 29, true, 82); /* üù ¿¡§ ¤€¥ */
    dump(&fb, name);
}

/* C4 confirm dialog: FS2 window-44 layout (replicates render_confirm). */
static void screen_confirm(const char *l0, const char *l1, int first_y, const char *name) {
    framebuffer_t fb;
    fb_clear(&fb, false);
    const font_t *font = asset_font(FONT_FS2);
    int y = first_y;
    const char *lines[2] = {l0, l1};
    for (int i = 0; i < 2 && lines[i] != 0; i++) {
        fb_text(&fb, font, lines[i], 0, y, true, FB_WIDTH);
        y = (i == 0 && first_y == 4) ? 15 : y + 9;
    }
    draw_softkey(&fb, "OK");
    dump(&fb, name);
}

/* Incoming network-diverted call: the two-line "Diverted\ncall" no-detail
 * label (SID 0x0130), drawn at the stacked detail position (FS2, x6 y8/y17).
 * Confirms both lines clear the softkey row at y=40. */
static void screen_incoming_diverted(const char *name) {
    framebuffer_t fb;
    fb_clear(&fb, false);
    draw_status(&fb, 4u);
    const font_t *f = asset_font(FONT_FS2);
    fb_text(&fb, f, "Diverted", 6, 8, true, 72);
    fb_text(&fb, f, "call", 6, 17, true, 72);
    draw_softkey(&fb, "Answer");
    dump(&fb, name);
}

/* Battery/charging status strip at a given bar level. */
static void screen_battery(uint8_t bars, uint8_t batt, const char *name) {
    framebuffer_t fb;
    fb_clear(&fb, false);
    status_chrome_set_battery(batt);
    draw_status(&fb, bars);
    dump(&fb, name);
}

/* Call duration sample (the binary-pinned always-HH:MM:SS zero-padded). */
static void screen_call_timer(uint32_t secs, const char *name) {
    framebuffer_t fb;
    fb_clear(&fb, false);
    draw_status(&fb, 4u);
    fb_bitmap(&fb, 19u, 6, 0, true, true);
    const font_t *small = asset_font(FONT_FS1);
    fb_text(&fb, small, "Dark Star", 6, 7, true, 72);
    char dur[12];
    uint32_t h = secs / 3600u, m = (secs / 60u) % 60u, s = secs % 60u;
    snprintf(dur, sizeof(dur), "%02lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)s);
    draw_text_right(&fb, small, dur, 28, 78);
    draw_softkey(&fb, "Options");
    dump(&fb, name);
}

int main(void) {
    printf("rendering screens:\n");
    screen_menu_title("Phone book", "menu_title");
    screen_menu_title("Call register", "menu_title_long");
    screen_language_list(3u, "language_list_greek");      /* Ελληνικά selected */
    screen_language_list(17u, "language_list_arabic");    /* العربية selected */
    screen_language_list(13u, "language_list_russian");   /* Русский selected */
    screen_special_chars(17u, "special_chars_pound");     /* £ selected */
    screen_special_chars(28u, "special_chars_euro");      /* € selected */
    screen_accents("accents_sample");
    screen_incoming_diverted("incoming_diverted");
    screen_confirm("Erase?", "Dark Star", 4, "confirm_erase");
    screen_confirm("Are you", "sure?", 2, "confirm_sure");
    screen_battery(4u, 4u, "battery_full");
    screen_battery(4u, 1u, "battery_low");
    screen_battery(4u, 2u, "battery_charging_mid");
    screen_call_timer(35u, "call_timer_35s");
    screen_call_timer(3725u, "call_timer_1h2m5s");
    printf("done.\n");
    return 0;
}
