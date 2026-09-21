#include <stdio.h>
#include <string.h>

#include "apps/calls_app.h"
#include "services/strings.h"
#include "ui/ui.h"

static int failures;
static uint32_t now_ms;
uint32_t time_ms(void) { return now_ms; }
int32_t time_diff_ms(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

static void check(bool ok, const char *message) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", message); failures++; }
}

static void dump(const framebuffer_t *fb, const char *prefix, unsigned index) {
    char path[256];
    snprintf(path, sizeof(path), "%s-%u.pbm", prefix, index);
    FILE *file = fopen(path, "wb");
    if (file == NULL) { failures++; return; }
    fprintf(file, "P1\n84 48\n");
    for (int y = 0; y < 48; y++) {
        for (int x = 0; x < 84; x++) fprintf(file, "%d ", fb_get_pixel(fb, x, y));
        fputc('\n', file);
    }
    fclose(file);
}

int main(int argc, char **argv) {
    static app_t app;
    app.call_phase = CALL_PHASE_REDIAL_WAIT;
    app.call_redial_attempts = 1u;
    app.signal_bars = 4u;
    for (uint8_t language = 0u; language < strings_language_count(); language++) {
        strings_set_language(strings_language_id_at(language));
        for (unsigned frame = 0u; frame <= 16u; frame++) {
            framebuffer_t got, expected;
            app.call_frame_index = (uint8_t)frame;
            render_call(&app, &got);
            fb_clear(&expected, false);
            fb_rect(&expected, 0, 7, 84, 8, true);
            draw_text_block(&expected, asset_font(FONT_FS2), ts_or(0x5cu, "Redialling"),
                            0, 18, FB_WIDTH, 9, 2u);
            unsigned fraction = frame == 0u ? 1u : (frame * 255u + 255u) / 16u;
            if (fraction > 255u) fraction = 255u;
            const bitmap_t *bar = asset_bitmap(165u);
#ifndef SISU_SYNTHETIC_ASSETS
            check(bar != NULL && bar->width == 80u && bar->height == 4u,
                  "original animation-2 bitmap is an 80x4 solid bar");
            if (bar != NULL) fb_fill_rect(&expected, 2, 9, 80u * fraction / 255u, 4, true);
            check(memcmp(got.data, expected.data, sizeof(got.data)) == 0,
                  "redial pixels match full-width stock progress, translated text and blank softkey");
#else
            (void)bar; (void)fraction;
            for (int y = 16; y < 48; y++) {
                for (int x = 0; x < 84; x++) {
                    check(fb_get_pixel(&got, x, y) == fb_get_pixel(&expected, x, y),
                          "fallback redial label and blank softkey use normal renderer");
                }
            }
#endif
            if (argc == 2 && (frame == 0u || frame == 8u || frame == 16u)) {
                dump(&got, argv[1], language * 100u + frame);
            }
        }
    }
    if (failures != 0) return 1;
    puts("call_redial_render tests passed");
    return 0;
}
