#include "apps/messages_app.h"
#include "messages_internal.h"

#include <stdio.h>
#include <string.h>

#include "app_internal.h"
#include "apps/dialogs_app.h"
#include "services/input_keys.h"
#include "services/key_utils.h"
#include "services/strings.h"
#include "services/t9_service.h"
#include "services/timebase.h"
#include "storage/store_service.h"
#include "ui/text_layout.h"
#include "ui/ui.h"

#define SMS_COMPOSER_MAX MODEM_SMS_TEXT_MAX
/* [BP] Multi-tap commit window: model value (900 ms). Not in the v6.00 timer
 * record table; armed via the inline timer-set helper 0x243180 in the editor,
 * still to be traced. */
#define SMS_MULTITAP_MS 900u

static const char *const SMS_DICTIONARY_OFF_LABEL = "Dictionary off";
/* The persisted dictionary is a stable LANGUAGE id, not a registry index: the
 * compiled dictionary set (and its order) is a build choice, so an index would
 * silently become a different language after a rebuild. Encoding: bit7 set =
 * language id in the low bits (current format); bit7 clear = a legacy value
 * from builds that stored the index of the fixed ENGL/GER/FREN trio. */
#define SMS_DICTIONARY_LANG_ID_FLAG 0x80u

typedef enum {
    SMS_INPUT_NONE = 0,
    SMS_INPUT_HANDLED,
    SMS_INPUT_INSERTED,
    SMS_INPUT_CYCLED,
    SMS_INPUT_T9,
} sms_input_result_t;

/* C7b/C7c: the full original 30-char special set in stock order
 * (.,?!:;-+#*()'"_@&$£%/<>¿¡§=¤€¥) as CP1252 bytes, 10x3 grid. */
static const char SMS_SPECIAL_CHARS[] = ".,?!:;-+#*()'\"_@&$\xa3%/<>\xbf\xa1\xa7=\xa4\x80\xa5";

static void load_sms_dictionary_settings(app_t *app);
static void sms_composer_reset_multitap(app_t *app);
static void sms_composer_clear_digit_hold(app_t *app);
static void sms_t9_reset_composition(app_t *app);
static bool sms_t9_text_mode_active(const app_t *app);
static bool sms_t9_composition_active(const app_t *app);
static bool sms_t9_has_matches(const app_t *app);
static sms_input_result_t sms_t9_handle_digit(app_t *app, uint16_t key);
static bool sms_t9_cycle_candidate(app_t *app);
static bool sms_t9_backspace_composition(app_t *app);
static void sms_t9_replace_composition(app_t *app, const char *word);
static void sms_t9_refresh_candidates(app_t *app);
static void sms_t9_add_user_word(app_t *app, const char *word);
static bool sms_t9_set_dictionary(app_t *app, bool active, uint8_t dictionary_index);
static uint8_t sms_composer_option_count(const app_t *app);
static const char *sms_composer_option_label(const app_t *app, uint8_t index);
static uint8_t sms_dictionary_option_count(void);
static const char *sms_dictionary_option_label(uint8_t index);
static uint16_t sms_composer_max_len(const app_t *app);
static sms_input_result_t sms_composer_input_key(app_t *app, uint16_t key,
                                                  uint32_t now);
static bool sms_composer_apply_digit_hold(app_t *app, uint16_t key);
static bool sms_composer_delete_one(app_t *app);
static bool sms_composer_insert_char(app_t *app, char ch);
static void sms_composer_cycle_mode(app_t *app);
static uint16_t sms_composer_clamped_cursor(const app_t *app);
static void sms_composer_move_cursor_line(app_t *app, int8_t delta);
static bool sms_composer_at_sentence_start(const app_t *app, uint16_t pos);
static const char *sms_chars_for_key(uint16_t key, sms_mode_t mode);
static uint16_t sms_mode_bitmap_id(sms_mode_t mode);
static void draw_sms_composer_text(framebuffer_t *fb, const app_t *app);

static uint16_t sms_composer_option_sid(const char *label) {
    if (strcmp(label, "Matches") == 0) return 0x3bcu;
    if (strcmp(label, "Insert word") == 0) return 0x3c6u;
    if (strcmp(label, "Insert number") == 0) return 0x3bfu;
    if (strcmp(label, "Insert symbol") == 0) return 0x3c7u;
    if (strcmp(label, "Send by set") == 0) return 0x35bu;
    if (strcmp(label, "Dictionary off") == 0) return 0x3c4u;
    if (strcmp(label, "Save") == 0) return 0x17eu;
    if (strcmp(label, "Exit") == 0) return 0x173u;
    return 0u; /* Send / Dictionary / Clear screen -> absent from the map (English) */
}
void render_sms_composer(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    bool t9_active = sms_t9_text_mode_active(app);
    fb_bitmap(fb, t9_active ? 8u : 14u, 0, 0, true, true);
    fb_bitmap(fb, sms_mode_bitmap_id((sms_mode_t)app->sms_composer_mode), t9_active ? 21 : 14, 0, true, true);
    const font_t *counter = asset_font(FONT_FS3);
    char remaining[6];
    uint16_t len = (uint16_t)strlen(app->sms_composer_text);
    uint16_t max_len = sms_composer_max_len(app);
    uint16_t remain = len >= max_len ? 0u : (uint16_t)(max_len - len);
    snprintf(remaining, sizeof(remaining), "%u", (unsigned)remain);
    draw_right_text_box(fb, counter, remaining, 0, 0, FB_WIDTH);
    draw_sms_composer_text(fb, app);
    draw_softkey(fb, app->sms_t9_spell_offer && sms_t9_composition_active(app) ? ts_or(0x2f3u, "Spell") : "Options");
}

void render_sms_options(const app_t *app, framebuffer_t *fb) {
    const char *labels[12];
    uint8_t count = 0u;
    if (app->messages_picture_text_editing) {
        count = messages_picture_editor_option_count();
        for (uint8_t i = 0u; i < count && i < ARRAY_COUNT(labels); i++) {
            labels[i] = messages_picture_editor_option_label(i);
        }
    } else if (app->sms_options_dictionary) {
        count = sms_dictionary_option_count();
        for (uint8_t i = 0u; i < count && i < ARRAY_COUNT(labels); i++) {
            const char *en = sms_dictionary_option_label(i);
            labels[i] = ts_or(sms_composer_option_sid(en), en);
        }
    } else {
        count = sms_composer_option_count(app);
        for (uint8_t i = 0u; i < count && i < ARRAY_COUNT(labels); i++) {
            const char *en = sms_composer_option_label(app, i);
            labels[i] = ts_or(sms_composer_option_sid(en), en);
        }
    }
    draw_flat_list(fb, labels, count, app->sms_options_selected, "", "Select");
}

void render_sms_symbols(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    const font_t *font = asset_font(FONT_FS2);
    /* S6.5: badge follows the input mode — T9 indicator 0008 when the
     * dictionary drives input, pencil 0014 otherwise. */
    fb_bitmap(fb, sms_t9_text_mode_active(app) ? 8u : 14u, 0, 0, true, true);
    fb_bitmap(fb, 13u, sms_t9_text_mode_active(app) ? 21 : 14, 0, true, true);
    uint8_t count = (uint8_t)strlen(SMS_SPECIAL_CHARS);
    for (uint8_t i = 0; i < count && i < 30u; i++) {
        int x = 2 + (i % 10u) * 8;
        int y = 8 + (i / 10u) * 9;
        bool selected = i == app->sms_symbols_selected;
        if (selected) {
            /* Original inverts a FILLED cell; an outline left the white glyph
             * invisible on the white interior. */
            fb_fill_rect(fb, x - 1, y - 1, 8, 9, true);
        }
        char ch[2] = {SMS_SPECIAL_CHARS[i], '\0'};
        int w = asset_text_width(font, ch);
        fb_text(fb, font, ch, x + ((7 - w) / 2), y, !selected, 7);
    }
    draw_softkey(fb, "Use");
}
bool tick_sms_composer(app_t *app, uint32_t now) {
    if (app->route != APP_ROUTE_SMS_COMPOSER) {
        return false;
    }
    if (time_diff_ms(now, app->sms_composer_cursor_blink_ms + 512u) < 0) {
        return false;
    }
    app->sms_composer_cursor_visible = !app->sms_composer_cursor_visible;
    app->sms_composer_cursor_blink_ms = now;
    return true;
}

bool handle_sms_composer_key(app_t *app, uint16_t key, event_type_t event_type, uint32_t now) {
    /* Keep the cursor solid while typing, like the generic editor. */
    app->sms_composer_cursor_visible = true;
    app->sms_composer_cursor_blink_ms = now;
    char hold_digit = key_digit(key);
    if (event_type == EVENT_KEY_DOWN) {
        sms_composer_clear_digit_hold(app);
    } else if (event_type == EVENT_KEY_HOLD &&
               hold_digit >= '0' && hold_digit <= '9') {
        if (sms_composer_apply_digit_hold(app, key)) {
            app->dirty = true;
        }
        return true;
    }
    if (key == KEY_NAVI) {
        if (app->sms_t9_spell_offer && sms_t9_composition_active(app)) {
            app->sms_t9_insert_replace_active = true;
            open_editor(app,
                        ts_or(0x3c8u, "Insert word:"),
                        "",
                        T9_WORD_MAX,
                        EDITOR_KIND_TEXT,
                        EDITOR_CONTEXT_SMS_T9_INSERT_WORD,
                        true,
                        now);
            app->editor_mode = EDITOR_MODE_UPPER;
            return true;
        }
        if (app->messages_picture_text_editing) {
            copy_text(app->messages_picture_draft,
                      sizeof(app->messages_picture_draft),
                      app->sms_composer_text);
            app->sms_options_selected = 0u;
            app->route = APP_ROUTE_SMS_OPTIONS;
            app->dirty = true;
            return true;
        }
        app->sms_options_selected = 0u;
        app->sms_options_dictionary = false;
        app->route = APP_ROUTE_SMS_OPTIONS;
        app->dirty = true;
        return true;
    }
    if (key == KEY_C) {
        if (event_type == EVENT_KEY_HOLD) {
            if (app->sms_composer_text[0] != '\0') {
                app->sms_composer_text[0] = '\0';
                app->sms_composer_cursor = 0u;
                sms_t9_reset_composition(app);
                sms_composer_reset_multitap(app);
                app->dirty = true;
                return true;
            }
        } else if (sms_composer_delete_one(app)) {
            app->dirty = true;
            return true;
        } else {
            if (app->messages_picture_text_editing) {
                messages_picture_return_to_preview(app);
            } else {
                open_messages_menu(app, 2u);
            }
        }
        return true;
    }
    if (key == KEY_STAR) {
        if (event_type == EVENT_KEY_HOLD) {
            app->sms_symbols_selected = 0u;
            sms_t9_reset_composition(app);
            app->route = APP_ROUTE_SMS_SYMBOLS;
            app->dirty = true;
        } else if (sms_t9_composition_active(app)) {
            sms_t9_cycle_candidate(app);
            app->dirty = true;
        } else if (app->sms_composer_mode == SMS_MODE_NUMERIC) {
            /* S6.5: 123-mode * cycles "* + p w" like the number editor. */
            static const char STAR_SYMBOLS[] = {'*', '+', 'p', 'w'};
            uint16_t cursor = sms_composer_clamped_cursor(app);
            uint16_t previous = (uint16_t)ui_text_previous_boundary(
                app->sms_composer_text, cursor);
            bool cycled = false;
            if (cursor > 0u && app->sms_composer_last_key == KEY_STAR &&
                cursor - previous == 1u &&
                time_diff_ms(now, app->sms_composer_last_key_ms + SMS_MULTITAP_MS) < 0) {
                char previous_char = app->sms_composer_text[previous];
                for (uint8_t i = 0u; i < (uint8_t)ARRAY_COUNT(STAR_SYMBOLS); i++) {
                    if (previous_char == STAR_SYMBOLS[i]) {
                        app->sms_composer_text[previous] =
                            STAR_SYMBOLS[(i + 1u) % ARRAY_COUNT(STAR_SYMBOLS)];
                        cycled = true;
                        break;
                    }
                }
            }
            if (!cycled) {
                sms_composer_insert_char(app, '*');
            }
            app->sms_composer_last_key = KEY_STAR;
            app->sms_composer_last_key_ms = now;
            app->dirty = true;
        } else {
            app->sms_symbols_selected = 0u;
            sms_t9_reset_composition(app);
            app->route = APP_ROUTE_SMS_SYMBOLS;
            app->dirty = true;
        }
        return true;
    }
    if (key == KEY_HASH) {
        if (event_type == EVENT_KEY_HOLD) {
            sms_t9_reset_composition(app);
            app->sms_composer_mode = app->sms_composer_mode == SMS_MODE_NUMERIC ? SMS_MODE_SENTENCE : SMS_MODE_NUMERIC;
            if (app->sms_dictionary_active && app->sms_composer_mode != SMS_MODE_NUMERIC) {
                app->sms_t9_manual_mode = false;
            }
            sms_composer_reset_multitap(app);
        } else if (app->sms_composer_mode == SMS_MODE_NUMERIC) {
            sms_composer_insert_char(app, '#');
        } else {
            sms_composer_cycle_mode(app);
        }
        app->dirty = true;
        return true;
    }
    if (key == KEY_UP || key == KEY_DOWN) {
        /* S6.2: editable inputs carry DF3_CURSOR_MOVABLE 0x0002 — Up/Down
         * move the insertion point by display line. */
        if (sms_t9_composition_active(app)) {
            sms_t9_reset_composition(app);
        }
        sms_composer_reset_multitap(app);
        sms_composer_move_cursor_line(app, key == KEY_UP ? -1 : 1);
        app->dirty = true;
        return true;
    }
    sms_input_result_t input = sms_composer_input_key(app, key, now);
    if (input == SMS_INPUT_INSERTED || input == SMS_INPUT_CYCLED ||
        input == SMS_INPUT_T9) {
        app->sms_composer_hold_key = key;
        app->sms_composer_hold_undo = (uint8_t)input;
        app->dirty = true;
    }
    return true;
}

bool handle_sms_options_key(app_t *app, uint16_t key, uint32_t now) {
    uint8_t count = app->messages_picture_text_editing
        ? messages_picture_editor_option_count()
        : (app->sms_options_dictionary ? sms_dictionary_option_count() : sms_composer_option_count(app));
    if (count == 0u) {
        app->route = APP_ROUTE_SMS_COMPOSER;
        app->dirty = true;
        return true;
    }
    if (app->sms_options_selected >= count) {
        app->sms_options_selected = 0u;
    }
    if (key == KEY_UP) {
        app->sms_options_selected = app->sms_options_selected == 0u ? (uint8_t)(count - 1u) : (uint8_t)(app->sms_options_selected - 1u);
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN) {
        app->sms_options_selected = (uint8_t)((app->sms_options_selected + 1u) % count);
        app->dirty = true;
        return true;
    }
    if (key == KEY_C) {
        if (app->messages_picture_text_editing) {
            app->route = APP_ROUTE_SMS_COMPOSER;
            app->dirty = true;
            return true;
        }
        if (app->sms_options_dictionary) {
            app->sms_options_dictionary = false;
            app->sms_options_selected = 0u;
            app->dirty = true;
            return true;
        }
        app->route = APP_ROUTE_SMS_COMPOSER;
        app->dirty = true;
        return true;
    }
    if (key != KEY_NAVI) {
        return true;
    }
    if (app->messages_picture_text_editing) {
        if (app->sms_options_selected == 0u) {
            sms_t9_reset_composition(app);
        }
        messages_picture_select_editor_option(app, app->sms_options_selected, now);
        return true;
    }
    if (app->sms_options_dictionary) {
        if (app->sms_options_selected == 0u) {
            sms_t9_set_dictionary(app, false, app->sms_dictionary_index);
            store_setting_set_u8(STORE_SETTING_SMS_DICTIONARY_ACTIVE, 0u);
            open_display_sid(app, 3u, 0x3bau, "T9 dictionary deactivated", APP_ROUTE_SMS_COMPOSER, now);
        } else {
            uint8_t dictionary_index = (uint8_t)(app->sms_options_selected - 1u);
            sms_t9_set_dictionary(app, true, dictionary_index);
            store_setting_set_u8(STORE_SETTING_SMS_DICTIONARY_ACTIVE, 1u);
            store_setting_set_u8(STORE_SETTING_SMS_DICTIONARY_LANGUAGE,
                                 (uint8_t)(SMS_DICTIONARY_LANG_ID_FLAG |
                                           t9_dictionary_lang_id(dictionary_index)));
            open_display_sid(app, 3u, 0x3bbu, "T9 dictionary activated", APP_ROUTE_SMS_COMPOSER, now);
        }
        app->sms_options_dictionary = false;
        app->sms_options_selected = 0u;
        return true;
    }
    const char *label = sms_composer_option_label(app, app->sms_options_selected);
    if (strcmp(label, "Send") == 0 || strcmp(label, "Send by set") == 0) {
        sms_t9_reset_composition(app);
        open_sms_recipient_editor(app, app->sms_recipient_prefill, now);
    } else if (strcmp(label, "Matches") == 0) {
        sms_t9_cycle_candidate(app);
        app->route = APP_ROUTE_SMS_COMPOSER;
        app->dirty = true;
    } else if (strcmp(label, "Insert word") == 0) {
        app->sms_t9_insert_replace_active = false;
        open_editor(app,
                    ts_or(0x3c8u, "Insert word:"),
                    "",
                    T9_WORD_MAX,
                    EDITOR_KIND_TEXT,
                    EDITOR_CONTEXT_SMS_T9_INSERT_WORD,
                    true,
                    now);
        app->editor_mode = EDITOR_MODE_UPPER;
    } else if (strcmp(label, "Insert number") == 0) {
        app->sms_t9_insert_replace_active = false;
        open_editor(app,
                    ts_or(0x3c0u, "Insert number:"),
                    "",
                    30u,
                    EDITOR_KIND_NUMBER,
                    EDITOR_CONTEXT_SMS_T9_INSERT_NUMBER,
                    true,
                    now);
    } else if (strcmp(label, "Insert symbol") == 0) {
        app->sms_symbols_selected = 0u;
        sms_t9_reset_composition(app);
        app->route = APP_ROUTE_SMS_SYMBOLS;
        app->dirty = true;
    } else if (strcmp(label, "Dictionary") == 0) {
        sms_t9_reset_composition(app);
        app->sms_options_dictionary = true;
        app->sms_options_selected = app->sms_dictionary_active ? (uint8_t)(app->sms_dictionary_index + 1u) : 0u;
        app->dirty = true;
    } else if (strcmp(label, "Save") == 0) {
        sms_t9_reset_composition(app);
        messages_save_composed_message(app, now);
    } else if (strcmp(label, "Clear screen") == 0) {
        app->sms_composer_text[0] = '\0';
        app->sms_composer_cursor = 0u;
        sms_t9_reset_composition(app);
        sms_composer_reset_multitap(app);
        app->route = APP_ROUTE_SMS_COMPOSER;
        app->dirty = true;
    } else if (strcmp(label, "Exit") == 0) {
        app->sms_composer_text[0] = '\0';
        app->sms_composer_cursor = 0u;
        app->sms_recipient_prefill[0] = '\0';
        sms_t9_reset_composition(app);
        app->route = APP_ROUTE_STANDBY;
        app->dirty = true;
    }
    return true;
}

bool handle_sms_symbols_key(app_t *app, uint16_t key, uint32_t now) {
    (void)now;
    uint8_t count = (uint8_t)strlen(SMS_SPECIAL_CHARS);
    if (key == KEY_UP) {
        app->sms_symbols_selected = (uint8_t)((app->sms_symbols_selected + 1u) % count);
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN) {
        app->sms_symbols_selected = app->sms_symbols_selected == 0u ? (uint8_t)(count - 1u) : (uint8_t)(app->sms_symbols_selected - 1u);
        app->dirty = true;
        return true;
    }
    if (key == KEY_HASH) {
        uint8_t next = (uint8_t)(app->sms_symbols_selected + 10u);
        app->sms_symbols_selected = next < count ? next : (uint8_t)(next % 10u);
        app->dirty = true;
        return true;
    }
    if (key == KEY_C) {
        app->route = APP_ROUTE_SMS_COMPOSER;
        app->dirty = true;
        return true;
    }
    if (key == KEY_NAVI) {
        sms_composer_insert_char(app, SMS_SPECIAL_CHARS[app->sms_symbols_selected]);
        app->route = APP_ROUTE_SMS_COMPOSER;
        app->dirty = true;
    }
    return true;
}
static void sms_composer_open_state(app_t *app, const char *text, uint32_t now,
                                    bool cursor_visible) {
    (void)now;
    copy_text(app->sms_composer_text, sizeof(app->sms_composer_text), text != 0 ? text : "");
    app->sms_composer_cursor = (uint16_t)strlen(app->sms_composer_text);
    app->sms_composer_cursor_visible = cursor_visible;
    load_sms_dictionary_settings(app);
    app->sms_composer_mode = SMS_MODE_SENTENCE;
    app->sms_t9_manual_mode = false;
    sms_t9_reset_composition(app);
    sms_composer_reset_multitap(app);
    app->sms_options_selected = 0u;
    app->sms_options_dictionary = false;
    app->sms_symbols_selected = 0u;
    app->route = APP_ROUTE_SMS_COMPOSER;
    app->dirty = true;
}

void open_sms_composer(app_t *app, const char *text, const char *recipient, uint32_t now) {
    app->messages_picture_text_editing = false;
    app->messages_picture_pending_valid = false;
    copy_text(app->sms_recipient_prefill,
              sizeof(app->sms_recipient_prefill),
              recipient != 0 ? recipient : "");
    sms_composer_open_state(app, text, now, true);
}

void messages_composer_open_picture(app_t *app, const char *text, uint32_t now) {
    bool cursor_visible = app->sms_composer_cursor_visible;
    app->messages_picture_text_editing = true;
    sms_composer_open_state(app, text, now, cursor_visible);
}

void messages_composer_clear_text(app_t *app) {
    app->sms_composer_text[0] = '\0';
    app->sms_composer_cursor = 0u;
    sms_t9_reset_composition(app);
    sms_composer_reset_multitap(app);
}

static uint8_t sms_dictionary_stored_to_index(uint8_t stored) {
    if (stored & SMS_DICTIONARY_LANG_ID_FLAG) {
        return t9_dictionary_index_for_lang_id(
            (uint8_t)(stored & (uint8_t)~SMS_DICTIONARY_LANG_ID_FLAG));
    }
    /* Legacy trio index 0/1/2 -> language ids 1/2/3 (ENGL/GER/FREN). */
    return t9_dictionary_index_for_lang_id(
        stored <= 2u ? (uint8_t)(stored + 1u) : 1u);
}

static void load_sms_dictionary_settings(app_t *app) {
    uint8_t active = 0u;
    uint8_t stored = SMS_DICTIONARY_LANG_ID_FLAG | 1u; /* default English */
    store_setting_get_u8(STORE_SETTING_SMS_DICTIONARY_ACTIVE, &active);
    store_setting_get_u8(STORE_SETTING_SMS_DICTIONARY_LANGUAGE, &stored);
    app->sms_dictionary_active = active != 0u;
    app->sms_dictionary_index = sms_dictionary_stored_to_index(stored);
    if (!app->sms_dictionary_active) {
        app->sms_t9_manual_mode = false;
    }
}

void messages_composer_init(app_t *app) {
    app->sms_composer_mode = SMS_MODE_SENTENCE;
    app->sms_send_return_route = APP_ROUTE_SMS_COMPOSER;
    load_sms_dictionary_settings(app);
    uint8_t user_word_count = 0u;
    if (store_t9_user_words_load(app->sms_t9_user_words,
                                 APP_SMS_T9_USER_WORD_LIMIT,
                                 &user_word_count) == STORE_STATUS_OK) {
        app->sms_t9_user_word_count = user_word_count;
    }
}

static void sms_t9_reset_composition(app_t *app) {
    app->sms_t9_sequence[0] = '\0';
    app->sms_t9_word_start = 0u;
    app->sms_t9_word_end = 0u;
    app->sms_t9_candidate_index = 0u;
    app->sms_t9_candidate_count = 0u;
    app->sms_t9_spell_offer = false;
    for (uint8_t i = 0u; i < T9_CANDIDATE_LIMIT; i++) {
        app->sms_t9_candidates[i][0] = '\0';
    }
}

static bool sms_t9_text_mode_active(const app_t *app) {
    return app->sms_dictionary_active &&
           !app->sms_t9_manual_mode &&
           app->sms_composer_mode != SMS_MODE_NUMERIC;
}

static bool sms_t9_composition_active(const app_t *app) {
    /* Region-based: composition may sit mid-text now that the cursor moves
     * (S6.2); the bounds check guards against stale regions after the text
     * is replaced underneath. */
    return sms_t9_text_mode_active(app) &&
           app->sms_t9_sequence[0] != '\0' &&
           app->sms_t9_word_start <= app->sms_t9_word_end &&
           app->sms_t9_word_end <= strlen(app->sms_composer_text);
}

static bool sms_t9_has_matches(const app_t *app) {
    return sms_t9_composition_active(app) &&
           app->sms_t9_candidate_count > 1u &&
           !app->sms_t9_spell_offer;
}

static sms_input_result_t sms_t9_handle_digit(app_t *app, uint16_t key) {
    char digit = key_digit(key);
    if (digit == 0 || !sms_t9_text_mode_active(app)) {
        return SMS_INPUT_NONE;
    }
    if (digit == '0') {
        sms_t9_reset_composition(app);
        return sms_composer_insert_char(app, ' ')
            ? SMS_INPUT_INSERTED
            : SMS_INPUT_HANDLED;
    }
    if (digit < '2' || digit > '9') {
        return SMS_INPUT_NONE;
    }

    size_t text_len = strlen(app->sms_composer_text);
    if (!sms_t9_composition_active(app)) {
        sms_t9_reset_composition(app);
        app->sms_t9_word_start = sms_composer_clamped_cursor(app);
        app->sms_t9_word_end = app->sms_t9_word_start;
    }
    size_t sequence_len = strlen(app->sms_t9_sequence);
    if (sequence_len >= T9_SEQUENCE_MAX) {
        return SMS_INPUT_HANDLED;
    }
    uint16_t active_len = app->sms_t9_word_end >= app->sms_t9_word_start
        ? (uint16_t)(app->sms_t9_word_end - app->sms_t9_word_start)
        : 0u;
    uint16_t max_len = sms_composer_max_len(app);
    if (text_len - active_len + sequence_len + 1u > max_len) {
        return SMS_INPUT_HANDLED;
    }
    app->sms_t9_sequence[sequence_len] = digit;
    app->sms_t9_sequence[sequence_len + 1u] = '\0';
    sms_t9_refresh_candidates(app);
    const char *word = app->sms_t9_candidate_count != 0u
        ? app->sms_t9_candidates[app->sms_t9_candidate_index]
        : "";
    char fallback[T9_WORD_MAX + 1u];
    if (word[0] == '\0') {
        t9_fallback_word(app->sms_t9_sequence, fallback, sizeof(fallback));
        word = fallback;
    }
    sms_t9_replace_composition(app, word);
    return SMS_INPUT_T9;
}

static bool sms_t9_cycle_candidate(app_t *app) {
    if (!sms_t9_composition_active(app) || app->sms_t9_candidate_count == 0u) {
        return false;
    }
    if (app->sms_t9_spell_offer) {
        return true;
    }
    if (app->sms_t9_candidate_index + 1u >= app->sms_t9_candidate_count) {
        app->sms_t9_spell_offer = true;
        return true;
    }
    app->sms_t9_candidate_index++;
    sms_t9_replace_composition(app, app->sms_t9_candidates[app->sms_t9_candidate_index]);
    return true;
}

static bool sms_t9_backspace_composition(app_t *app) {
    if (!sms_t9_composition_active(app)) {
        return false;
    }
    size_t sequence_len = strlen(app->sms_t9_sequence);
    if (sequence_len <= 1u) {
        size_t len = strlen(app->sms_composer_text);
        if (app->sms_t9_word_start < len && app->sms_t9_word_end <= len) {
            memmove(&app->sms_composer_text[app->sms_t9_word_start],
                    &app->sms_composer_text[app->sms_t9_word_end],
                    len - app->sms_t9_word_end + 1u);
        }
        app->sms_composer_cursor = app->sms_t9_word_start;
        sms_t9_reset_composition(app);
        return true;
    }
    app->sms_t9_sequence[sequence_len - 1u] = '\0';
    sms_t9_refresh_candidates(app);
    const char *word = app->sms_t9_candidate_count != 0u
        ? app->sms_t9_candidates[app->sms_t9_candidate_index]
        : "";
    char fallback[T9_WORD_MAX + 1u];
    if (word[0] == '\0') {
        t9_fallback_word(app->sms_t9_sequence, fallback, sizeof(fallback));
        word = fallback;
    }
    sms_t9_replace_composition(app, word);
    return true;
}

static void sms_t9_replace_composition(app_t *app, const char *word) {
    size_t len = strlen(app->sms_composer_text);
    uint16_t start = app->sms_t9_word_start <= len ? app->sms_t9_word_start : (uint16_t)len;
    uint16_t end = app->sms_t9_word_end <= len && app->sms_t9_word_end >= start ? app->sms_t9_word_end : start;
    char display[T9_WORD_MAX + 1u];
    t9_display_word(word,
                    app->sms_composer_mode,
                    sms_composer_at_sentence_start(app, start),
                    display,
                    sizeof(display));
    size_t insert_len = strlen(display);
    size_t suffix_len = len - end;
    uint16_t max_len = sms_composer_max_len(app);
    if (start + insert_len + suffix_len > max_len) {
        if (start + suffix_len >= max_len) {
            insert_len = 0u;
        } else {
            insert_len = max_len - start - suffix_len;
        }
    }
    memmove(&app->sms_composer_text[start + insert_len],
            &app->sms_composer_text[end],
            suffix_len + 1u);
    memcpy(&app->sms_composer_text[start], display, insert_len);
    app->sms_t9_word_start = start;
    app->sms_t9_word_end = (uint16_t)(start + insert_len);
    app->sms_composer_cursor = app->sms_t9_word_end;
    app->sms_t9_spell_offer = false;
}

static void sms_t9_refresh_candidates(app_t *app) {
    t9_candidate_list_t native;
    memset(&native, 0, sizeof(native));
    app->sms_t9_candidate_count = 0u;

    char seen[T9_CANDIDATE_LIMIT][T9_WORD_MAX + 1u];
    uint8_t seen_count = 0u;
    for (uint8_t i = 0u; i < app->sms_t9_user_word_count && app->sms_t9_candidate_count < T9_CANDIDATE_LIMIT; i++) {
        char signature[T9_SEQUENCE_MAX + 1u];
        if (t9_signature(app->sms_t9_user_words[i], signature, sizeof(signature)) &&
            strcmp(signature, app->sms_t9_sequence) == 0) {
            copy_text(app->sms_t9_candidates[app->sms_t9_candidate_count],
                      sizeof(app->sms_t9_candidates[app->sms_t9_candidate_count]),
                      app->sms_t9_user_words[i]);
            copy_text(seen[seen_count], sizeof(seen[seen_count]), app->sms_t9_user_words[i]);
            app->sms_t9_candidate_count++;
            seen_count++;
        }
    }

    (void)t9_candidates_for_sequence(app->sms_t9_sequence, app->sms_dictionary_index, &native);
    for (uint8_t i = 0u; i < native.count && app->sms_t9_candidate_count < T9_CANDIDATE_LIMIT; i++) {
        bool duplicate = false;
        for (uint8_t j = 0u; j < seen_count; j++) {
            if (strcmp(seen[j], native.words[i]) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            copy_text(app->sms_t9_candidates[app->sms_t9_candidate_count],
                      sizeof(app->sms_t9_candidates[app->sms_t9_candidate_count]),
                      native.words[i]);
            if (seen_count < T9_CANDIDATE_LIMIT) {
                copy_text(seen[seen_count], sizeof(seen[seen_count]), native.words[i]);
                seen_count++;
            }
            app->sms_t9_candidate_count++;
        }
    }
    if (app->sms_t9_candidate_index >= app->sms_t9_candidate_count) {
        app->sms_t9_candidate_index = app->sms_t9_candidate_count == 0u
            ? 0u
            : (uint8_t)(app->sms_t9_candidate_count - 1u);
    }
    app->sms_t9_spell_offer = false;
}

static void sms_t9_add_user_word(app_t *app, const char *word) {
    char signature[T9_SEQUENCE_MAX + 1u];
    if (!t9_signature(word, signature, sizeof(signature))) {
        return;
    }
    char normalized[T9_WORD_MAX + 1u];
    copy_text(normalized, sizeof(normalized), word);
    for (uint8_t i = 0u; normalized[i] != '\0'; i++) {
        if (normalized[i] >= 'A' && normalized[i] <= 'Z') {
            normalized[i] = (char)(normalized[i] + ('a' - 'A'));
        }
    }
    for (uint8_t i = 0u; i < app->sms_t9_user_word_count; i++) {
        if (strcmp(app->sms_t9_user_words[i], normalized) == 0) {
            for (uint8_t j = i; j > 0u; j--) {
                copy_text(app->sms_t9_user_words[j], sizeof(app->sms_t9_user_words[j]), app->sms_t9_user_words[j - 1u]);
            }
            copy_text(app->sms_t9_user_words[0], sizeof(app->sms_t9_user_words[0]), normalized);
            (void)store_t9_user_words_save(app->sms_t9_user_words, app->sms_t9_user_word_count);
            return;
        }
    }
    uint8_t last = app->sms_t9_user_word_count < APP_SMS_T9_USER_WORD_LIMIT
        ? app->sms_t9_user_word_count
        : (uint8_t)(APP_SMS_T9_USER_WORD_LIMIT - 1u);
    for (uint8_t i = last; i > 0u; i--) {
        copy_text(app->sms_t9_user_words[i], sizeof(app->sms_t9_user_words[i]), app->sms_t9_user_words[i - 1u]);
    }
    copy_text(app->sms_t9_user_words[0], sizeof(app->sms_t9_user_words[0]), normalized);
    if (app->sms_t9_user_word_count < APP_SMS_T9_USER_WORD_LIMIT) {
        app->sms_t9_user_word_count++;
    }
    (void)store_t9_user_words_save(app->sms_t9_user_words, app->sms_t9_user_word_count);
}

bool sms_t9_insert_text(app_t *app, const char *text, bool replace_active, bool add_user_word) {
    if (text == 0 || text[0] == '\0') {
        sms_t9_reset_composition(app);
        return false;
    }
    size_t len = strlen(app->sms_composer_text);
    uint16_t start = sms_composer_clamped_cursor(app);
    uint16_t end = start;
    if (replace_active && sms_t9_composition_active(app)) {
        start = app->sms_t9_word_start;
        end = app->sms_t9_word_end;
    }
    if (start > len) {
        start = (uint16_t)len;
    }
    if (end > len || end < start) {
        end = start;
    }
    size_t insert_len = strlen(text);
    size_t suffix_len = len - end;
    uint16_t max_len = sms_composer_max_len(app);
    if (start + insert_len + suffix_len > max_len) {
        return false;
    }
    if (add_user_word) {
        sms_t9_add_user_word(app, text);
    }
    sms_t9_reset_composition(app);
    memmove(&app->sms_composer_text[start + insert_len],
            &app->sms_composer_text[end],
            suffix_len + 1u);
    memcpy(&app->sms_composer_text[start], text, insert_len);
    app->sms_composer_cursor = (uint16_t)(start + insert_len);
    sms_composer_reset_multitap(app);
    return insert_len > 0u;
}

static bool sms_t9_set_dictionary(app_t *app, bool active, uint8_t dictionary_index) {
    if (dictionary_index >= t9_dictionary_count()) {
        dictionary_index = 0u;
    }
    sms_t9_reset_composition(app);
    app->sms_dictionary_active = active;
    app->sms_dictionary_index = dictionary_index;
    app->sms_t9_manual_mode = false;
    app->sms_composer_mode = SMS_MODE_SENTENCE;
    sms_composer_reset_multitap(app);
    return true;
}

static uint8_t sms_composer_option_count(const app_t *app) {
    uint8_t count = 1u;
    if (app->sms_dictionary_active) {
        if (sms_t9_has_matches(app)) {
            count++;
        }
        count += 4u;
    } else {
        count++;
    }
    count += 4u;
    return count;
}

static const char *sms_composer_option_label(const app_t *app, uint8_t index) {
    if (index == 0u) {
        return "Send";
    }
    index--;
    if (app->sms_dictionary_active) {
        if (sms_t9_has_matches(app)) {
            if (index == 0u) {
                return "Matches";
            }
            index--;
        }
        if (index == 0u) {
            return "Insert word";
        }
        if (index == 1u) {
            return "Insert number";
        }
        if (index == 2u) {
            return "Insert symbol";
        }
        if (index == 3u) {
            return "Dictionary";
        }
        index = (uint8_t)(index - 4u);
    } else {
        if (index == 0u) {
            return "Dictionary";
        }
        index--;
    }
    if (index == 0u) {
        return "Send by set";
    }
    if (index == 1u) {
        return "Save";
    }
    if (index == 2u) {
        return "Clear screen";
    }
    return "Exit";
}

static uint8_t sms_dictionary_option_count(void) {
    return (uint8_t)(t9_dictionary_count() + 1u);
}

static const char *sms_dictionary_option_label(uint8_t index) {
    if (index == 0u) {
        return SMS_DICTIONARY_OFF_LABEL;
    }
    const t9_dictionary_info_t *info = t9_dictionary_info((uint8_t)(index - 1u));
    return info != 0 ? info->label : "English";
}

static uint16_t sms_composer_max_len(const app_t *app) {
    if (app != 0 && app->messages_picture_text_editing) {
        return messages_picture_composer_max_len(app);
    }
    return SMS_COMPOSER_MAX;
}

static void sms_composer_reset_multitap(app_t *app) {
    app->sms_composer_last_key = 0u;
    app->sms_composer_last_key_ms = 0u;
    app->sms_composer_tap_index = 0u;
    sms_composer_clear_digit_hold(app);
}

static void sms_composer_clear_digit_hold(app_t *app) {
    app->sms_composer_hold_key = 0u;
    app->sms_composer_hold_undo = SMS_INPUT_NONE;
    app->sms_composer_hold_previous_char = '\0';
}

static sms_input_result_t sms_composer_input_key(app_t *app, uint16_t key,
                                                  uint32_t now) {
    sms_input_result_t t9_result = sms_t9_handle_digit(app, key);
    if (t9_result != SMS_INPUT_NONE) {
        return t9_result;
    }
    if (sms_t9_composition_active(app)) {
        sms_t9_reset_composition(app);
    }
    sms_mode_t mode = (sms_mode_t)app->sms_composer_mode;
    if (mode == SMS_MODE_NUMERIC) {
        char digit = key_digit(key);
        if (digit == 0) {
            return SMS_INPUT_NONE;
        }
        sms_composer_reset_multitap(app);
        return sms_composer_insert_char(app, digit)
            ? SMS_INPUT_INSERTED
            : SMS_INPUT_HANDLED;
    }

    size_t len = strlen(app->sms_composer_text);
    uint16_t cursor = sms_composer_clamped_cursor(app);
    uint16_t previous = (uint16_t)ui_text_previous_boundary(
        app->sms_composer_text, cursor);
    bool continuing = app->sms_composer_last_key == key &&
                      cursor > 0u &&
                      cursor - previous == 1u &&
                      time_diff_ms(now, app->sms_composer_last_key_ms + SMS_MULTITAP_MS) < 0;
    sms_mode_t effective = mode;
    if (mode == SMS_MODE_SENTENCE) {
        /* The Abc badge stays put; case is derived per keypress from the
         * sentence position (text start or after ". ! ?" + spaces). */
        uint16_t case_pos = continuing ? previous : cursor;
        effective = sms_composer_at_sentence_start(app, case_pos) ? SMS_MODE_UPPER : SMS_MODE_LOWER;
    }
    const char *chars = sms_chars_for_key(key, effective);
    if (chars[0] == '\0') {
        return SMS_INPUT_NONE;
    }
    if (continuing) {
        app->sms_composer_hold_previous_char = app->sms_composer_text[previous];
        app->sms_composer_tap_index = (uint8_t)((app->sms_composer_tap_index + 1u) % strlen(chars));
        app->sms_composer_text[previous] = chars[app->sms_composer_tap_index];
    } else {
        if (len >= sms_composer_max_len(app)) {
            return SMS_INPUT_HANDLED;
        }
        app->sms_composer_tap_index = 0u;
        memmove(&app->sms_composer_text[cursor + 1u],
                &app->sms_composer_text[cursor],
                len - cursor + 1u);
        app->sms_composer_text[cursor] = chars[0];
        app->sms_composer_cursor = (uint16_t)(cursor + 1u);
    }
    app->sms_composer_last_key = key;
    app->sms_composer_last_key_ms = now;
    return continuing ? SMS_INPUT_CYCLED : SMS_INPUT_INSERTED;
}

static bool sms_composer_apply_digit_hold(app_t *app, uint16_t key) {
    char digit = key_digit(key);
    if (digit < '0' || digit > '9' || app->sms_composer_hold_key != key) {
        return false;
    }

    sms_input_result_t undo = (sms_input_result_t)app->sms_composer_hold_undo;
    char previous_char = app->sms_composer_hold_previous_char;
    sms_composer_clear_digit_hold(app);

    bool changed = false;
    if (undo == SMS_INPUT_INSERTED) {
        if (!sms_composer_delete_one(app)) {
            return false;
        }
        changed = true;
    } else if (undo == SMS_INPUT_CYCLED) {
        uint16_t cursor = sms_composer_clamped_cursor(app);
        uint16_t previous = (uint16_t)ui_text_previous_boundary(
            app->sms_composer_text, cursor);
        if (cursor == 0u || cursor - previous != 1u) {
            return false;
        }
        app->sms_composer_text[previous] = previous_char;
        sms_composer_reset_multitap(app);
        changed = true;
    } else if (undo == SMS_INPUT_T9) {
        if (!sms_t9_backspace_composition(app)) {
            return false;
        }
        /* The preceding T9 sequence becomes ordinary committed text; only the
         * key-down belonging to this hold is replaced by the literal digit. */
        sms_t9_reset_composition(app);
        sms_composer_reset_multitap(app);
        changed = true;
    } else {
        return false;
    }

    return sms_composer_insert_char(app, digit) || changed;
}

static bool sms_composer_delete_one(app_t *app) {
    sms_composer_reset_multitap(app);
    if (sms_t9_backspace_composition(app)) {
        return true;
    }
    size_t len = strlen(app->sms_composer_text);
    uint16_t cursor = sms_composer_clamped_cursor(app);
    if (len == 0u || cursor == 0u) {
        return false;
    }
    uint16_t previous = (uint16_t)ui_text_previous_boundary(
        app->sms_composer_text, cursor);
    memmove(&app->sms_composer_text[previous],
            &app->sms_composer_text[cursor],
            len - cursor + 1u);
    app->sms_composer_cursor = previous;
    return true;
}

static bool sms_composer_insert_char(app_t *app, char ch) {
    if (sms_t9_composition_active(app)) {
        sms_t9_reset_composition(app);
    }
    size_t len = strlen(app->sms_composer_text);
    if (len >= sms_composer_max_len(app)) {
        return false;
    }
    sms_composer_reset_multitap(app);
    uint16_t cursor = sms_composer_clamped_cursor(app);
    memmove(&app->sms_composer_text[cursor + 1u],
            &app->sms_composer_text[cursor],
            len - cursor + 1u);
    app->sms_composer_text[cursor] = ch;
    app->sms_composer_cursor = (uint16_t)(cursor + 1u);
    return true;
}

static uint16_t sms_composer_clamped_cursor(const app_t *app) {
    return (uint16_t)ui_text_clamp_boundary(app->sms_composer_text,
                                           app->sms_composer_cursor);
}

static void sms_composer_move_cursor_line(app_t *app, int8_t delta) {
    const font_t *font = asset_font(FONT_FS2);
    uint16_t cursor = sms_composer_clamped_cursor(app);
    uint16_t row = 0u;
    ui_text_span_t line;
    if (!ui_glyph_line_for_offset(font, app->sms_composer_text, 82,
                                  cursor, &row, &line)) {
        return;
    }
    int cursor_x = ui_text_range_width(font, app->sms_composer_text,
                                       line.start, cursor);
    if (delta < 0) {
        if (row == 0u) {
            app->sms_composer_cursor = 0u;
            return;
        }
        if (!ui_glyph_line_at(font, app->sms_composer_text, 82,
                              (uint16_t)(row - 1u), &line)) {
            return;
        }
        app->sms_composer_cursor = (uint16_t)ui_text_offset_for_x(
            font, app->sms_composer_text, line, cursor_x);
    } else {
        if (!ui_glyph_line_at(font, app->sms_composer_text, 82,
                              (uint16_t)(row + 1u), &line)) {
            app->sms_composer_cursor = (uint16_t)strlen(
                app->sms_composer_text);
            return;
        }
        app->sms_composer_cursor = (uint16_t)ui_text_offset_for_x(
            font, app->sms_composer_text, line, cursor_x);
    }
}

/* S6.3: sentence case applies only at a sentence start (text begin or after
 * terminating punctuation + spaces). */
static bool sms_composer_at_sentence_start(const app_t *app, uint16_t pos) {
    while (pos > 0u) {
        char ch = app->sms_composer_text[pos - 1u];
        if (ch == ' ') {
            pos--;
            continue;
        }
        return ch == '.' || ch == '!' || ch == '?';
    }
    return true;
}

static void sms_composer_cycle_mode(app_t *app) {
    sms_t9_reset_composition(app);
    if (app->sms_dictionary_active) {
        if (!app->sms_t9_manual_mode && app->sms_composer_mode == SMS_MODE_SENTENCE) {
            app->sms_composer_mode = SMS_MODE_LOWER;
        } else if (!app->sms_t9_manual_mode && app->sms_composer_mode == SMS_MODE_LOWER) {
            app->sms_composer_mode = SMS_MODE_UPPER;
        } else if (!app->sms_t9_manual_mode && app->sms_composer_mode == SMS_MODE_UPPER) {
            app->sms_t9_manual_mode = true;
            app->sms_composer_mode = SMS_MODE_SENTENCE;
        } else if (app->sms_t9_manual_mode && app->sms_composer_mode == SMS_MODE_SENTENCE) {
            app->sms_composer_mode = SMS_MODE_LOWER;
        } else if (app->sms_t9_manual_mode && app->sms_composer_mode == SMS_MODE_LOWER) {
            app->sms_composer_mode = SMS_MODE_UPPER;
        } else {
            app->sms_t9_manual_mode = false;
            app->sms_composer_mode = SMS_MODE_SENTENCE;
        }
    } else if (app->sms_composer_mode == SMS_MODE_SENTENCE) {
        app->sms_composer_mode = SMS_MODE_LOWER;
    } else if (app->sms_composer_mode == SMS_MODE_LOWER) {
        app->sms_composer_mode = SMS_MODE_UPPER;
    } else {
        app->sms_composer_mode = SMS_MODE_SENTENCE;
    }
    sms_composer_reset_multitap(app);
}

static const char *sms_chars_for_key(uint16_t key, sms_mode_t mode) {
    editor_mode_t mapped = mode == SMS_MODE_LOWER ? EDITOR_MODE_LOWER : EDITOR_MODE_UPPER;
    return editor_chars_for_key(key, mapped);
}

static uint16_t sms_mode_bitmap_id(sms_mode_t mode) {
    if (mode == SMS_MODE_LOWER) {
        return 10u;
    }
    if (mode == SMS_MODE_UPPER) {
        return 11u;
    }
    if (mode == SMS_MODE_NUMERIC) {
        return 12u;
    }
    return 7u;
}

static void draw_sms_composer_text(framebuffer_t *fb, const app_t *app) {
    const font_t *font = asset_font(FONT_FS2);
    uint16_t line_count = ui_glyph_line_count(
        font, app->sms_composer_text, 82);
    if (line_count == 0u) {
        return;
    }
    uint16_t cursor = sms_composer_clamped_cursor(app);

    uint16_t cursor_row = 0u;
    ui_text_span_t cursor_line;
    if (!ui_glyph_line_for_offset(font, app->sms_composer_text, 82,
                                  cursor, &cursor_row, &cursor_line)) {
        return;
    }
    /* The 4-row window follows the insertion point (cursor on the bottom
     * row when scrolled, matching the old append-at-end view). */
    uint16_t first_visible = cursor_row > 3u
        ? (uint16_t)(cursor_row - 3u) : 0u;

    for (uint8_t row = 0u; row < 4u && first_visible + row < line_count; row++) {
        uint16_t abs_row = (uint16_t)(first_visible + row);
        ui_text_span_t line;
        if (!ui_glyph_line_at(font, app->sms_composer_text, 82,
                              abs_row, &line)) {
            continue;
        }
        char text[MODEM_SMS_TEXT_MAX + 1u];
        if (!ui_text_span_copy(app->sms_composer_text, line,
                               text, sizeof(text))) {
            continue;
        }
        int y = 8 + row * 10;
        fb_text(fb, font, text, 1, y, true, 82);
        if (sms_t9_composition_active(app) &&
            app->sms_t9_word_end > line.start &&
            app->sms_t9_word_start < line.end) {
            uint16_t underline_start = app->sms_t9_word_start > line.start
                ? app->sms_t9_word_start : line.start;
            uint16_t underline_end = app->sms_t9_word_end < line.end
                ? app->sms_t9_word_end : line.end;
            int ux = 1 + ui_text_range_width(font,
                                              app->sms_composer_text,
                                              line.start,
                                              underline_start);
            int uw = ui_text_range_width(font,
                                         app->sms_composer_text,
                                         underline_start,
                                         underline_end);
            if (uw > 0) {
                fb_hline(fb, ux, y + font->height, uw, true);
            }
        }
        if (abs_row == cursor_row && app->sms_composer_cursor_visible) {
            int cursor_x = 1 + ui_text_range_width(font,
                                                    app->sms_composer_text,
                                                    line.start,
                                                    cursor);
            if (cursor_x > 82) {
                cursor_x = 82;
            }
            fb_vline(fb, cursor_x, y - 1, font->height + 2, true);
        }
    }
}
