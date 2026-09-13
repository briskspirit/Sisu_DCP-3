#include "ui/menu_visible.h"

uint8_t ui_menu_visible_count(ui_menu_visible_fn visible, uint8_t raw_count) {
    uint8_t count = 0u;
    for (uint8_t i = 0u; i < raw_count; i++) {
        if (visible(i)) {
            count++;
        }
    }
    return count;
}

uint8_t ui_menu_visible_index(ui_menu_visible_fn visible, uint8_t raw_count,
                              uint8_t raw) {
    uint8_t ordinal = 0u;
    for (uint8_t i = 0u; i < raw_count; i++) {
        if (!visible(i)) {
            continue;
        }
        if (i == raw) {
            return ordinal;
        }
        ordinal++;
    }
    return 0u;
}

uint8_t ui_menu_raw_at_visible(ui_menu_visible_fn visible, uint8_t raw_count,
                               uint8_t visible_index) {
    uint8_t seen = 0u;
    for (uint8_t i = 0u; i < raw_count; i++) {
        if (!visible(i)) {
            continue;
        }
        if (seen == visible_index) {
            return i;
        }
        seen++;
    }
    return 0u;
}

uint8_t ui_menu_normalized_raw(ui_menu_visible_fn visible, uint8_t raw_count,
                               uint8_t raw) {
    if (raw < raw_count && visible(raw)) {
        return raw;
    }
    for (uint8_t offset = 0u; offset < raw_count; offset++) {
        uint8_t candidate = (uint8_t)((raw + offset) % raw_count);
        if (visible(candidate)) {
            return candidate;
        }
    }
    return 0u;
}
