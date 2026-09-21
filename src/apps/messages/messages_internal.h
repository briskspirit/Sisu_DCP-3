#ifndef APPS_MESSAGES_INTERNAL_H
#define APPS_MESSAGES_INTERNAL_H

#include <stdint.h>
#include <string.h>

#include "apps/messages_app.h"
#include "ui/ui.h"

static inline void messages_format(char *dst, size_t cap, const char *format,
                                    char token, const char *value) {
    size_t used = 0u;
    while (*format != '\0' && used + 1u < cap) {
        if (format[0] == '%' && format[1] == token) {
            copy_text(dst + used, cap - used, value);
            used += strlen(dst + used);
            format += 2;
        } else {
            const char *next = format;
            asset_next_codepoint(&next);
            size_t bytes = (size_t)(next - format);
            if (used + bytes >= cap) break;
            memcpy(dst + used, format, bytes);
            used += bytes;
            format = next;
        }
    }
    if (cap != 0u) dst[used] = '\0';
}

void messages_composer_init(app_t *app);
void messages_composer_open_picture(app_t *app, const char *text, uint32_t now);
void messages_composer_clear_text(app_t *app);
void messages_save_composed_message(app_t *app, uint32_t now);

void messages_picture_open_menu(app_t *app, uint32_t now);
void messages_picture_open_list(app_t *app);
void messages_picture_render(const app_t *app, framebuffer_t *fb);
void messages_picture_draw_received_preview(framebuffer_t *fb,
                                            const store_picture_message_t *picture);
bool messages_picture_handle_key(app_t *app, uint16_t key, uint32_t now);
bool messages_picture_poll_send(app_t *app, uint32_t now);
bool messages_picture_poll_storage(app_t *app, uint32_t now);
void messages_picture_save_received(app_t *app,
                                    const store_picture_message_t *picture,
                                    uint32_t now);
uint16_t messages_picture_composer_max_len(const app_t *app);
uint8_t messages_picture_editor_option_count(void);
const char *messages_picture_editor_option_label(uint8_t index);
void messages_picture_select_editor_option(app_t *app, uint8_t index, uint32_t now);
void messages_picture_return_to_preview(app_t *app);

#endif
