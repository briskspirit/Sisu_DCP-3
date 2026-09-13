#include "apps/calculator_app.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "apps/dialogs_app.h"
#include "apps/main_menu_app.h"
#include "services/input_keys.h"
#include "services/key_utils.h"
#include "services/strings.h"
#include "storage/store_service.h"
#include "services/timebase.h"

#define CALCULATOR_MAX_INPUT_CHARS 9u
#define CALCULATOR_RATE_MAX_INPUT_CHARS 7u
#define CALCULATOR_RANGE_MAX 999999999.0
#define CALCULATOR_INTEGER_SCALE_THRESHOLD 214748364.0
#define CALCULATOR_CURSOR_MS 512u

typedef enum {
    CALCULATOR_OPTION_EQUALS = 0,
    CALCULATOR_OPTION_ADD,
    CALCULATOR_OPTION_SUBTRACT,
    CALCULATOR_OPTION_MULTIPLY,
    CALCULATOR_OPTION_DIVIDE,
    CALCULATOR_OPTION_TO_DOMESTIC,
    CALCULATOR_OPTION_TO_FOREIGN,
    CALCULATOR_OPTION_EXCHANGE_RATE,
} calculator_option_t;

static const calculator_option_t OPTIONS_MODE_1[] = {
    CALCULATOR_OPTION_EQUALS,
    CALCULATOR_OPTION_ADD,
    CALCULATOR_OPTION_SUBTRACT,
    CALCULATOR_OPTION_MULTIPLY,
    CALCULATOR_OPTION_DIVIDE,
    CALCULATOR_OPTION_TO_DOMESTIC,
    CALCULATOR_OPTION_TO_FOREIGN,
    CALCULATOR_OPTION_EXCHANGE_RATE,
};
static const calculator_option_t OPTIONS_MODE_3[] = {
    CALCULATOR_OPTION_SUBTRACT,
    CALCULATOR_OPTION_MULTIPLY,
    CALCULATOR_OPTION_DIVIDE,
    CALCULATOR_OPTION_TO_DOMESTIC,
};
static const calculator_option_t OPTIONS_MODE_4[] = {
    CALCULATOR_OPTION_EQUALS,
    CALCULATOR_OPTION_SUBTRACT,
    CALCULATOR_OPTION_MULTIPLY,
    CALCULATOR_OPTION_DIVIDE,
    CALCULATOR_OPTION_TO_DOMESTIC,
};
static const char *const EXCHANGE_LABELS[] = {
    "Foreign unit\nexpressed in\ndomestic units",
    "Domestic unit\nexpressed in\nforeign units",
};
/* v6.00 SIDs for the (multiline) exchange-direction captions above; the English
 * literal stays the fallback. Resolved via ts_or() at render time. */
static const uint16_t EXCHANGE_LABEL_SIDS[] = {0x80u, 0x7au};

static void reset_calculator_state(app_t *app);
static char calculator_digit_for_key(uint16_t key);
static void append_calculator_digit(app_t *app, char digit);
static void insert_calculator_decimal(app_t *app);
static void begin_calculator_operation(app_t *app, uint8_t code, uint32_t now);
static void cycle_calculator_operator(app_t *app);
static void calculator_set_operator(app_t *app, uint8_t code);
static bool calculator_equals(app_t *app, uint32_t now);
static bool calculate_calculator_result(app_t *app, uint32_t now);
static void handle_calculator_back(app_t *app, uint32_t now);
static void clear_calculator_from_hold(app_t *app);
static void clear_exchange_editor_from_hold(app_t *app);
static const calculator_option_t *calculator_options(const app_t *app, uint8_t *out_count);
static const char *calculator_option_label(calculator_option_t option);
static void apply_calculator_option(app_t *app, calculator_option_t option, uint32_t now);
static void calculator_convert(app_t *app, bool to_foreign, uint32_t now);
static void open_exchange_menu(app_t *app);
static void open_exchange_editor(app_t *app, uint8_t direction, uint32_t now);
static void append_exchange_text(app_t *app, char ch);
static void clear_exchange_char(app_t *app);
static void move_exchange_cursor(app_t *app, int8_t delta);
static void save_exchange_rate(app_t *app, uint32_t now);
static const char *exchange_visible_rate(app_t *app, uint8_t direction, char *scratch, size_t cap);
static void show_calculator_error(app_t *app, bool divide_by_zero, uint32_t now);
static double calculator_number(const char *text);
static bool calculator_format_number(double value, char *dst, size_t cap);
static void calculator_trim_decimal_text(char *text);
static void calculator_round_display_text(const char *text, char *dst, size_t cap);
static void render_calculator_display(const app_t *app, framebuffer_t *fb);
static void render_calculator_options(const app_t *app, framebuffer_t *fb);
static void render_exchange_menu(const app_t *app, framebuffer_t *fb);
static void render_exchange_editor(const app_t *app, framebuffer_t *fb);
static void draw_calculator_row(framebuffer_t *fb, const char *text, int y);

void open_calculator(app_t *app, uint32_t now) {
    reset_calculator_state(app);
    store_setting_get_text(STORE_SETTING_CALCULATOR_EXCHANGE_RATE,
                           app->calculator_exchange_rate,
                           (uint8_t)sizeof(app->calculator_exchange_rate));
    if (app->calculator_exchange_rate[0] == '\0') {
        copy_text(app->calculator_exchange_rate, sizeof(app->calculator_exchange_rate), "1");
    }
    app->calculator_last_cursor_ms = now;
    app->route = APP_ROUTE_CALCULATOR;
    app->dirty = true;
}

bool handle_calculator_key(app_t *app, uint16_t key, event_type_t type, uint32_t now) {
    if (type == EVENT_KEY_HOLD && key == KEY_C) {
        if (app->calculator_exchange_mode == 2u) {
            clear_exchange_editor_from_hold(app);
        } else if (app->calculator_exchange_mode != 1u) {
            clear_calculator_from_hold(app);
        }
        app->dirty = true;
        return true;
    }

    if (app->calculator_exchange_mode == 1u) {
        if (key == KEY_UP) {
            app->calculator_exchange_index = app->calculator_exchange_index == 0u ? 1u : 0u;
        } else if (key == KEY_DOWN) {
            app->calculator_exchange_index = (uint8_t)((app->calculator_exchange_index + 1u) % 2u);
        } else if (key == KEY_NAVI) {
            open_exchange_editor(app, app->calculator_exchange_index, now);
            return true;
        } else if (key == KEY_C) {
            app->calculator_exchange_mode = 0u;
        }
        app->dirty = true;
        return true;
    }

    if (app->calculator_exchange_mode == 2u) {
        char digit = calculator_digit_for_key(key);
        if (digit != 0) {
            append_exchange_text(app, digit);
        } else if (key == KEY_HASH) {
            append_exchange_text(app, '.');
        } else if (key == KEY_UP) {
            move_exchange_cursor(app, 1);
        } else if (key == KEY_DOWN) {
            move_exchange_cursor(app, -1);
        } else if (key == KEY_NAVI) {
            save_exchange_rate(app, now);
            return true;
        } else if (key == KEY_C) {
            clear_exchange_char(app);
        }
        app->dirty = true;
        return true;
    }

    if (app->calculator_options_open) {
        uint8_t count = 0u;
        const calculator_option_t *options = calculator_options(app, &count);
        if (count == 0u) {
            app->calculator_options_open = false;
            app->dirty = true;
            return true;
        }
        if (app->calculator_option_index >= count) {
            app->calculator_option_index = 0u;
        }
        if (key == KEY_UP) {
            app->calculator_option_index = app->calculator_option_index == 0u
                ? (uint8_t)(count - 1u)
                : (uint8_t)(app->calculator_option_index - 1u);
        } else if (key == KEY_DOWN) {
            app->calculator_option_index = (uint8_t)((app->calculator_option_index + 1u) % count);
        } else if (key == KEY_NAVI) {
            apply_calculator_option(app, options[app->calculator_option_index], now);
            return true;
        } else if (key == KEY_C) {
            app->calculator_options_open = false;
        }
        app->dirty = true;
        return true;
    }

    char digit = calculator_digit_for_key(key);
    if (digit != 0) {
        append_calculator_digit(app, digit);
    } else if (key == KEY_HASH) {
        insert_calculator_decimal(app);
    } else if (key == KEY_STAR) {
        if (app->calculator_mode == 3u) {
            cycle_calculator_operator(app);
        } else {
            begin_calculator_operation(app, 1u, now);
        }
    } else if (key == KEY_NAVI) {
        /* Builder 0x0028e7c0 clears+rebuilds list id 1 each open -> selection
         * always starts at the first row. */
        app->calculator_option_index = 0u;
        app->calculator_options_open = true;
    } else if (key == KEY_C) {
        handle_calculator_back(app, now);
    }
    app->dirty = true;
    return true;
}

bool tick_calculator(app_t *app, uint32_t now) {
    if (app->route != APP_ROUTE_CALCULATOR || app->calculator_exchange_mode != 2u) {
        return false;
    }
    if (time_diff_ms(now, app->calculator_last_cursor_ms + CALCULATOR_CURSOR_MS) < 0) {
        return false;
    }
    app->calculator_cursor_visible = !app->calculator_cursor_visible;
    app->calculator_last_cursor_ms = now;
    return true;
}

void render_calculator(const app_t *app, framebuffer_t *fb) {
    if (app->calculator_exchange_mode == 1u) {
        render_exchange_menu(app, fb);
    } else if (app->calculator_exchange_mode == 2u) {
        render_exchange_editor(app, fb);
    } else if (app->calculator_options_open) {
        render_calculator_options(app, fb);
    } else {
        render_calculator_display(app, fb);
    }
}

static void reset_calculator_state(app_t *app) {
    copy_text(app->calculator_value, sizeof(app->calculator_value), "0");
    app->calculator_stored[0] = '\0';
    calculator_set_operator(app, 1u);
    app->calculator_mode = 1u;
    app->calculator_options_open = false;
    app->calculator_option_index = 0u;
    app->calculator_exchange_mode = 0u;
    app->calculator_exchange_index = 0u;
    app->calculator_exchange_direction = 0u;
    app->calculator_exchange_editor_value[0] = '\0';
    app->calculator_exchange_dirty = false;
    app->calculator_exchange_cursor_index = 0u;
    app->calculator_cursor_visible = true;
}

static char calculator_digit_for_key(uint16_t key) {
    char digit = key_digit(key);
    return digit >= '0' && digit <= '9' ? digit : 0;
}

static void append_calculator_digit(app_t *app, char digit) {
    if (app->calculator_mode == 3u) {
        app->calculator_value[0] = digit;
        app->calculator_value[1] = '\0';
        app->calculator_mode = 4u;
        return;
    }
    if (app->calculator_mode == 1u || app->calculator_mode == 5u) {
        app->calculator_value[0] = digit;
        app->calculator_value[1] = '\0';
        app->calculator_mode = 2u;
        return;
    }
    size_t len = strlen(app->calculator_value);
    if (len >= CALCULATOR_MAX_INPUT_CHARS) {
        return;
    }
    if (strcmp(app->calculator_value, "0") == 0) {
        app->calculator_value[0] = digit;
        app->calculator_value[1] = '\0';
        return;
    }
    app->calculator_value[len] = digit;
    app->calculator_value[len + 1u] = '\0';
}

static void insert_calculator_decimal(app_t *app) {
    if (app->calculator_mode == 3u) {
        copy_text(app->calculator_value, sizeof(app->calculator_value), "0.");
        app->calculator_mode = 4u;
        return;
    }
    if (app->calculator_mode == 1u || app->calculator_mode == 5u) {
        copy_text(app->calculator_value, sizeof(app->calculator_value), "0.");
        app->calculator_mode = 2u;
        return;
    }
    if (strchr(app->calculator_value, '.') != 0 || strlen(app->calculator_value) >= CALCULATOR_MAX_INPUT_CHARS) {
        return;
    }
    strncat(app->calculator_value, ".", sizeof(app->calculator_value) - strlen(app->calculator_value) - 1u);
}

static void begin_calculator_operation(app_t *app, uint8_t code, uint32_t now) {
    if (app->calculator_mode == 4u && !calculate_calculator_result(app, now)) {
        return;
    }
    if (app->calculator_mode != 3u) {
        copy_text(app->calculator_stored, sizeof(app->calculator_stored), app->calculator_value[0] != '\0' ? app->calculator_value : "0");
    }
    calculator_set_operator(app, code);
    app->calculator_mode = 3u;
}

static void cycle_calculator_operator(app_t *app) {
    uint8_t next = app->calculator_operator_code >= 4u ? 1u : (uint8_t)(app->calculator_operator_code + 1u);
    calculator_set_operator(app, next);
    app->calculator_mode = 3u;
}

static void calculator_set_operator(app_t *app, uint8_t code) {
    static const char chars[] = {'+', '-', '*', '/'};
    if (code < 1u || code > 4u) {
        code = 1u;
    }
    app->calculator_operator_code = code;
    app->calculator_operator_char = chars[code - 1u];
}

static bool calculator_equals(app_t *app, uint32_t now) {
    if (app->calculator_mode == 4u && !calculate_calculator_result(app, now)) {
        return false;
    }
    app->calculator_mode = 5u;
    return true;
}

static bool calculate_calculator_result(app_t *app, uint32_t now) {
    double left = calculator_number(app->calculator_stored[0] != '\0' ? app->calculator_stored : app->calculator_value);
    double right = calculator_number(app->calculator_value);
    double result = 0.0;
    if (app->calculator_operator_code == 1u) {
        result = left + right;
    } else if (app->calculator_operator_code == 2u) {
        result = left - right;
    } else if (app->calculator_operator_code == 3u) {
        result = left * right;
    } else {
        if (right == 0.0) {
            show_calculator_error(app, true, now);
            return false;
        }
        result = left / right;
    }
    char formatted[16];
    if (!calculator_format_number(result, formatted, sizeof(formatted))) {
        show_calculator_error(app, false, now);
        return false;
    }
    copy_text(app->calculator_value, sizeof(app->calculator_value), formatted);
    copy_text(app->calculator_stored, sizeof(app->calculator_stored), formatted);
    return true;
}

static void handle_calculator_back(app_t *app, uint32_t now) {
    if (app->calculator_mode == 3u) {
        copy_text(app->calculator_value, sizeof(app->calculator_value), app->calculator_stored[0] != '\0' ? app->calculator_stored : "0");
        app->calculator_mode = 2u;
        return;
    }
    if (app->calculator_mode == 4u || app->calculator_mode == 2u || app->calculator_mode == 5u) {
        size_t len = strlen(app->calculator_value);
        if (len <= 1u) {
            copy_text(app->calculator_value, sizeof(app->calculator_value), "0");
            app->calculator_mode = app->calculator_mode == 4u ? 3u : 1u;
            return;
        }
        app->calculator_value[len - 1u] = '\0';
        return;
    }
    open_main_menu_at(app, 6u, now);
}

static void clear_calculator_from_hold(app_t *app) {
    copy_text(app->calculator_value, sizeof(app->calculator_value), "0");
    app->calculator_stored[0] = '\0';
    calculator_set_operator(app, 1u);
    app->calculator_mode = 1u;
    app->calculator_options_open = false;
    app->calculator_exchange_mode = 0u;
}

static void clear_exchange_editor_from_hold(app_t *app) {
    app->calculator_exchange_editor_value[0] = '\0';
    app->calculator_exchange_dirty = true;
    app->calculator_exchange_cursor_index = 0u;
}

static const calculator_option_t *calculator_options(const app_t *app, uint8_t *out_count) {
    if (app->calculator_mode == 3u) {
        *out_count = (uint8_t)ARRAY_COUNT(OPTIONS_MODE_3);
        return OPTIONS_MODE_3;
    }
    if (app->calculator_mode == 4u) {
        *out_count = (uint8_t)ARRAY_COUNT(OPTIONS_MODE_4);
        return OPTIONS_MODE_4;
    }
    *out_count = (uint8_t)ARRAY_COUNT(OPTIONS_MODE_1);
    return OPTIONS_MODE_1;
}

static const char *calculator_option_label(calculator_option_t option) {
    switch (option) {
    case CALCULATOR_OPTION_EQUALS: return ts_or(0x85u, "Equals");
    case CALCULATOR_OPTION_ADD: return ts_or(0x79u, "Add");
    case CALCULATOR_OPTION_SUBTRACT: return ts_or(0x87u, "Subtract");
    case CALCULATOR_OPTION_MULTIPLY: return ts_or(0x82u, "Multiply");
    case CALCULATOR_OPTION_DIVIDE: return ts_or(0x7cu, "Divide");
    case CALCULATOR_OPTION_TO_DOMESTIC: return ts_or(0x7eu, "To domestic");
    case CALCULATOR_OPTION_TO_FOREIGN: return ts_or(0x81u, "To foreign");
    case CALCULATOR_OPTION_EXCHANGE_RATE: return ts_or(0x86u, "Exchange rate");
    default: return "";
    }
}

static void apply_calculator_option(app_t *app, calculator_option_t option, uint32_t now) {
    app->calculator_options_open = false;
    if (option == CALCULATOR_OPTION_EQUALS) {
        (void)calculator_equals(app, now);
    } else if (option == CALCULATOR_OPTION_ADD) {
        begin_calculator_operation(app, 1u, now);
    } else if (option == CALCULATOR_OPTION_SUBTRACT) {
        begin_calculator_operation(app, 2u, now);
    } else if (option == CALCULATOR_OPTION_MULTIPLY) {
        begin_calculator_operation(app, 3u, now);
    } else if (option == CALCULATOR_OPTION_DIVIDE) {
        begin_calculator_operation(app, 4u, now);
    } else if (option == CALCULATOR_OPTION_TO_DOMESTIC) {
        calculator_convert(app, false, now);
    } else if (option == CALCULATOR_OPTION_TO_FOREIGN) {
        calculator_convert(app, true, now);
    } else if (option == CALCULATOR_OPTION_EXCHANGE_RATE) {
        open_exchange_menu(app);
    }
    app->dirty = true;
}

static void calculator_convert(app_t *app, bool to_foreign, uint32_t now) {
    double rate = calculator_number(app->calculator_exchange_rate[0] != '\0' ? app->calculator_exchange_rate : "1");
    if (to_foreign && rate == 0.0) {
        show_calculator_error(app, true, now);
        return;
    }
    double value = calculator_number(app->calculator_value);
    double result = to_foreign ? value / rate : value * rate;
    char formatted[16];
    if (!calculator_format_number(result, formatted, sizeof(formatted))) {
        show_calculator_error(app, false, now);
        return;
    }
    copy_text(app->calculator_value, sizeof(app->calculator_value), formatted);
    app->calculator_mode = 5u;
}

static void open_exchange_menu(app_t *app) {
    app->calculator_options_open = false;
    app->calculator_exchange_mode = 1u;
    app->calculator_exchange_index = 0u;
}

static void open_exchange_editor(app_t *app, uint8_t direction, uint32_t now) {
    char scratch[16];
    app->calculator_exchange_mode = 2u;
    app->calculator_exchange_direction = direction == 0u ? 0u : 1u;
    copy_text(app->calculator_exchange_editor_value,
              sizeof(app->calculator_exchange_editor_value),
              exchange_visible_rate(app, app->calculator_exchange_direction, scratch, sizeof(scratch)));
    app->calculator_exchange_dirty = false;
    app->calculator_exchange_cursor_index = (uint8_t)strlen(app->calculator_exchange_editor_value);
    app->calculator_cursor_visible = true;
    app->calculator_last_cursor_ms = now;
    app->dirty = true;
}

static void append_exchange_text(app_t *app, char ch) {
    size_t len = strlen(app->calculator_exchange_editor_value);
    if (len >= CALCULATOR_RATE_MAX_INPUT_CHARS) {
        return;
    }
    if (ch == '.' && strchr(app->calculator_exchange_editor_value, '.') != 0) {
        return;
    }
    uint8_t index = app->calculator_exchange_cursor_index;
    if (index > len) {
        index = (uint8_t)len;
    }
    memmove(&app->calculator_exchange_editor_value[index + 1u],
            &app->calculator_exchange_editor_value[index],
            len - index + 1u);
    app->calculator_exchange_editor_value[index] = ch;
    app->calculator_exchange_cursor_index = (uint8_t)(index + 1u);
    app->calculator_exchange_dirty = true;
}

static void clear_exchange_char(app_t *app) {
    size_t len = strlen(app->calculator_exchange_editor_value);
    if (len == 0u) {
        app->calculator_exchange_mode = 1u;
        return;
    }
    uint8_t index = app->calculator_exchange_cursor_index;
    if (index == 0u) {
        return;
    }
    if (index > len) {
        index = (uint8_t)len;
    }
    memmove(&app->calculator_exchange_editor_value[index - 1u],
            &app->calculator_exchange_editor_value[index],
            len - index + 1u);
    app->calculator_exchange_cursor_index = (uint8_t)(index - 1u);
    app->calculator_exchange_dirty = true;
}

static void move_exchange_cursor(app_t *app, int8_t delta) {
    int next = (int)app->calculator_exchange_cursor_index + delta;
    int max = (int)strlen(app->calculator_exchange_editor_value);
    if (next < 0) {
        next = 0;
    } else if (next > max) {
        next = max;
    }
    app->calculator_exchange_cursor_index = (uint8_t)next;
}

static void save_exchange_rate(app_t *app, uint32_t now) {
    char value[16];
    copy_text(value, sizeof(value), app->calculator_exchange_editor_value);
    if (value[0] == '.') {
        size_t len = strlen(value);
        if (len + 1u < sizeof(value)) {
            memmove(&value[1], value, len + 1u);
            value[0] = '0';
        }
    }
    double visible = calculator_number(value);
    if (value[0] == '\0' || visible <= 0.0) {
        app->calculator_exchange_mode = 0u;
        app->calculator_exchange_dirty = false;
        show_calculator_error(app, true, now);
        return;
    }
    if (app->calculator_exchange_dirty) {
        char stored[16];
        if (app->calculator_exchange_direction == 1u) {
            if (!calculator_format_number(1.0 / visible, stored, sizeof(stored))) {
                app->calculator_exchange_mode = 0u;
                show_calculator_error(app, false, now);
                return;
            }
        } else {
            copy_text(stored, sizeof(stored), value);
        }
        calculator_trim_decimal_text(stored);
        if (store_setting_set_text(STORE_SETTING_CALCULATOR_EXCHANGE_RATE, stored) !=
            STORE_STATUS_OK) {
            app->calculator_exchange_mode = 0u;
            app->calculator_exchange_dirty = false;
            open_display_sid(app, 0u, 0x280u, "Memory\nfull", APP_ROUTE_CALCULATOR, now);
            return;
        }
        /* Publish only after persistence succeeds, so this boot and the next
         * one cannot disagree about the active conversion rate. */
        copy_text(app->calculator_exchange_rate, sizeof(app->calculator_exchange_rate), stored);
    }
    app->calculator_exchange_mode = 0u;
    app->calculator_exchange_dirty = false;
    open_display_sid(app, 3u, 0x84u, "Rate\nsaved", APP_ROUTE_CALCULATOR, now);
}

static const char *exchange_visible_rate(app_t *app, uint8_t direction, char *scratch, size_t cap) {
    if (direction == 1u) {
        double rate = calculator_number(app->calculator_exchange_rate[0] != '\0' ? app->calculator_exchange_rate : "1");
        if (rate == 0.0 || !calculator_format_number(1.0 / rate, scratch, cap)) {
            copy_text(scratch, cap, "0");
        }
        return scratch;
    }
    return app->calculator_exchange_rate[0] != '\0' ? app->calculator_exchange_rate : "1";
}

static void show_calculator_error(app_t *app, bool divide_by_zero, uint32_t now) {
    app->calculator_exchange_mode = 0u;
    app->calculator_options_open = false;
    if (divide_by_zero) {
        open_display_sid(app, 2u, 0x7du, "Dividing\nby zero\nnot allowed", APP_ROUTE_CALCULATOR, now);
    } else {
        open_display_sid(app, 2u, 0x83u, "Out of\nrange", APP_ROUTE_CALCULATOR, now);
    }
}

static double calculator_number(const char *text) {
    if (text == 0) {
        return 0.0;
    }
    bool negative = false;
    if (*text == '-') {
        negative = true;
        text++;
    }
    double value = 0.0;
    while (*text >= '0' && *text <= '9') {
        value = value * 10.0 + (double)(*text - '0');
        text++;
    }
    if (*text == '.') {
        double scale = 0.1;
        text++;
        while (*text >= '0' && *text <= '9') {
            value += (double)(*text - '0') * scale;
            scale *= 0.1;
            text++;
        }
    }
    return negative ? -value : value;
}

static bool calculator_format_number(double value, char *dst, size_t cap) {
    if (dst == 0 || cap == 0u || value != value) {
        return false;
    }
    bool negative = value < 0.0;
    double magnitude = negative ? -value : value;
    if (magnitude > CALCULATOR_RANGE_MAX) {
        return false;
    }
    if (magnitude == 0.0) {
        copy_text(dst, cap, "0");
        return true;
    }
    int decimal_shift = 0;
    uint8_t guard = 9u;
    while (magnitude < CALCULATOR_INTEGER_SCALE_THRESHOLD && guard > 0u) {
        magnitude *= 10.0;
        decimal_shift--;
        guard--;
    }
    uint64_t integer = (uint64_t)magnitude;
    if (integer == 0u) {
        copy_text(dst, cap, "0");
        return true;
    }
    while (decimal_shift < 0 && (integer % 10u) == 0u) {
        integer /= 10u;
        decimal_shift++;
    }
    char digits[24];
    snprintf(digits, sizeof(digits), "%llu", (unsigned long long)integer);

    char raw[32];
    raw[0] = '\0';
    size_t digit_len = strlen(digits);
    if (decimal_shift < 0) {
        int places = -decimal_shift;
        int split = (int)digit_len - places;
        if (split <= 0) {
            copy_text(raw, sizeof(raw), "0.");
            for (int i = 0; i < -split && strlen(raw) + 1u < sizeof(raw); i++) {
                strncat(raw, "0", sizeof(raw) - strlen(raw) - 1u);
            }
            strncat(raw, digits, sizeof(raw) - strlen(raw) - 1u);
        } else {
            size_t pos = 0u;
            for (int i = 0; i < split && pos + 1u < sizeof(raw); i++) {
                raw[pos++] = digits[i];
            }
            if (pos + 1u < sizeof(raw)) {
                raw[pos++] = '.';
            }
            raw[pos] = '\0';
            strncat(raw, &digits[split], sizeof(raw) - strlen(raw) - 1u);
        }
    } else {
        copy_text(raw, sizeof(raw), digits);
        for (int i = 0; i < decimal_shift && strlen(raw) + 1u < sizeof(raw); i++) {
            strncat(raw, "0", sizeof(raw) - strlen(raw) - 1u);
        }
    }

    char rounded[32];
    calculator_round_display_text(raw, rounded, sizeof(rounded));
    calculator_trim_decimal_text(rounded);
    if (negative && strcmp(rounded, "0") != 0) {
        char signed_text[32];
        snprintf(signed_text, sizeof(signed_text), "-%s", rounded);
        copy_text(dst, cap, signed_text);
    } else {
        copy_text(dst, cap, rounded);
    }
    return true;
}

static void calculator_trim_decimal_text(char *text) {
    char *dot = strchr(text, '.');
    if (dot == 0) {
        return;
    }
    size_t len = strlen(text);
    while (len > 0u && text[len - 1u] == '0') {
        text[--len] = '\0';
    }
    if (len > 0u && text[len - 1u] == '.') {
        text[--len] = '\0';
    }
    if (text[0] == '\0') {
        copy_text(text, 2u, "0");
    }
}

static void calculator_round_display_text(const char *text, char *dst, size_t cap) {
    bool saw_decimal = false;
    size_t end = strlen(text);
    uint8_t count = 0u;
    for (size_t i = 0u; text[i] != '\0'; i++) {
        if (text[i] == '.') {
            saw_decimal = true;
        }
        count++;
        if (count == 10u) {
            end = i + 1u;
            break;
        }
    }
    char clipped[32];
    if (end >= sizeof(clipped)) {
        end = sizeof(clipped) - 1u;
    }
    memcpy(clipped, text, end);
    clipped[end] = '\0';
    if (!saw_decimal || count <= 9u || strlen(clipped) <= 9u) {
        copy_text(dst, cap, clipped);
        return;
    }

    char chars[32];
    size_t base_len = 9u;
    if (base_len >= sizeof(chars)) {
        base_len = sizeof(chars) - 1u;
    }
    memcpy(chars, clipped, base_len);
    chars[base_len] = '\0';
    if (clipped[9] < '5') {
        copy_text(dst, cap, chars);
        return;
    }
    int index = (int)strlen(chars) - 1;
    bool left_of_decimal = false;
    bool incremented = false;
    while (index >= 0 && !incremented) {
        char ch = chars[index];
        if (ch == '.') {
            left_of_decimal = true;
            chars[index] = '\0';
        } else if (ch == '9') {
            chars[index] = left_of_decimal ? '0' : '\0';
        } else {
            chars[index] = (char)(ch + 1);
            incremented = true;
        }
        index--;
    }
    if (!incremented) {
        char out[32];
        snprintf(out, sizeof(out), "1%s", chars);
        copy_text(dst, cap, out);
    } else {
        copy_text(dst, cap, chars);
    }
}

static void render_calculator_display(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    if (app->calculator_mode == 3u) {
        draw_calculator_row(fb, app->calculator_stored[0] != '\0' ? app->calculator_stored : "0", 18);
        char op[2] = {app->calculator_operator_char == '\0' ? '+' : app->calculator_operator_char, '\0'};
        draw_calculator_row(fb, op, 28);
    } else if (app->calculator_mode == 4u) {
        draw_calculator_row(fb, app->calculator_stored[0] != '\0' ? app->calculator_stored : "0", 8);
        char op[2] = {app->calculator_operator_char == '\0' ? '+' : app->calculator_operator_char, '\0'};
        draw_calculator_row(fb, op, 18);
        draw_calculator_row(fb, app->calculator_value[0] != '\0' ? app->calculator_value : "0", 28);
    } else {
        draw_calculator_row(fb, app->calculator_value[0] != '\0' ? app->calculator_value : "0", 28);
    }
    draw_softkey(fb, ts_or(0x2eau, "Options"));
}

static void render_calculator_options(const app_t *app, framebuffer_t *fb) {
    uint8_t count = 0u;
    const calculator_option_t *options = calculator_options(app, &count);
    const char *labels[8];
    for (uint8_t i = 0u; i < count && i < ARRAY_COUNT(labels); i++) {
        labels[i] = calculator_option_label(options[i]);
    }
    uint8_t selected = app->calculator_option_index >= count ? 0u : app->calculator_option_index;
    draw_flat_list(fb, labels, count, selected, "", ts_or(0x2e9u, "OK"));
}

static void render_exchange_menu(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    const font_t *font = asset_font(FONT_FS2);
    uint8_t selected = app->calculator_exchange_index > 1u ? 0u : app->calculator_exchange_index;
    char scratch[64];
    copy_text(scratch, sizeof(scratch),
              ts_or(EXCHANGE_LABEL_SIDS[selected], EXCHANGE_LABELS[selected]));
    char *line = scratch;
    for (uint8_t row = 0u; row < 3u; row++) {
        char *next = strchr(line, '\n');
        if (next != 0) {
            *next = '\0';
        }
        fb_text(fb, font, line, 0, 8 + row * 10, true, 78);
        if (next == 0) {
            break;
        }
        line = next + 1;
    }
    draw_position_indicator(fb, selected, 2u, "");
    draw_softkey(fb, ts_or(0x2e9u, "OK"));
}

static void render_exchange_editor(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    const font_t *title_font = asset_font(FONT_FS2);
    const font_t *font = asset_font(FONT_FS0);
    const char *value = app->calculator_exchange_editor_value;
    uint8_t cursor = app->calculator_exchange_cursor_index;
    size_t len = strlen(value);
    if (cursor > len) {
        cursor = (uint8_t)len;
    }
    fb_bitmap(fb, 14u, 0, 0, true, true);
    fb_text(fb, title_font, ts_or(0x7fu, "Exchange rate:"), 1, 7, true, 83);
    fb_rect(fb, 0, 17, 84, 20, true);

    char visible[16];
    copy_text(visible, sizeof(visible), value[0] != '\0' ? value : " ");
    int w = asset_text_width(font, visible);
    int x = 2 + 80 - w + 1;
    if (x < 2) {
        x = 2;
    }
    fb_text(fb, font, visible, x, 23, true, 84 - x);

    if (app->calculator_cursor_visible) {
        char prefix[16];
        if (cursor >= sizeof(prefix)) {
            cursor = (uint8_t)(sizeof(prefix) - 1u);
        }
        memcpy(prefix, value, cursor);
        prefix[cursor] = '\0';
        int cx = x + asset_text_width(font, prefix) + 1;
        if (cx < 1) {
            cx = 1;
        } else if (cx > 82) {
            cx = 82;
        }
        fb_vline(fb, cx, 22, font->height + 1, true);
    }
    draw_softkey(fb, ts_or(0x2e9u, "OK"));
}

static void draw_calculator_row(framebuffer_t *fb, const char *text, int y) {
    draw_right_text_box(fb, asset_font(FONT_FS2), text, 0, y, 84);
}
