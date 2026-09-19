#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "apps/powerup_app.h"
#include "services/strings.h"

const char *ts(uint16_t sid) {
    assert(sid == SID_CONTACT_SERVICE_CONTACT || sid == SID_CONTACT_SERVICE_SERVICE);
    return sid == SID_CONTACT_SERVICE_CONTACT ? "CONTACT" : "SERVICE";
}

int main(int argc, char **argv) {
    framebuffer_t actual, expected = {0};
    memset(&actual, 0xff, sizeof(actual));
    app_t app = {.powerup_stage=APP_POWERUP_BLANK};
    render_contact_service(&app, &actual);
    assert(memcmp(actual.data, expected.data, FB_SIZE) == 0);
    app.powerup_stage = APP_POWERUP_DONE;
    render_contact_service(&app, &actual);
    const font_t *font = asset_font(FONT_FS4);
    assert(font != NULL);
    /* Independent pixel placement, from ROM 0x237b62/0x237b6c. This also
     * verifies that rendering clears old chrome and adds no dialog furniture. */
    const char *lines[] = {"CONTACT", "SERVICE"};
    const int top[] = {10, 27};
    for (unsigned line = 0; line < 2; line++) {
        int x = 13;
        for (const char *ch = lines[line]; *ch; ch++) {
            const glyph_t *g = asset_glyph(font, *ch);
            assert(g != NULL);
            const uint8_t *bits = asset_glyph_data(font, g);
            for (unsigned col = 0; col < g->width; col++) {
                for (unsigned row = 0; row < g->height; row++) {
                    if ((bits[(row / 8) * g->width + col] >> (row % 8)) & 1u) {
                        unsigned y = (unsigned)top[line] + row;
                        expected.data[(y / 8) * FB_WIDTH + x + col] |= (uint8_t)(1u << (y % 8));
                    }
                }
            }
            x += g->width + font->spacing;
        }
    }
    assert(memcmp(actual.data, expected.data, FB_SIZE) == 0);
    if (argc == 2) {
        FILE *out = fopen(argv[1], "wb");
        assert(out != NULL && fwrite(actual.data, 1, FB_SIZE, out) == FB_SIZE);
        assert(fclose(out) == 0);
    }
    puts("Contact service framebuffer matches ROM layout");
    return 0;
}
