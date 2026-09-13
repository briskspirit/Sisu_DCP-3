#ifndef UI_MENU_VISIBLE_H
#define UI_MENU_VISIBLE_H

#include <stdbool.h>
#include <stdint.h>

/* Raw<->visible index mapping for menus whose rows can be hidden (feature
 * gates, capability filters). Every user keeps its own visibility predicate;
 * these helpers own the shared counting/mapping/wraparound-fallback logic so
 * the four menu families cannot drift apart. */
typedef bool (*ui_menu_visible_fn)(uint8_t raw);

/* Number of visible rows (0 when none -- callers wanting a 1 floor add it). */
uint8_t ui_menu_visible_count(ui_menu_visible_fn visible, uint8_t raw_count);
/* Ordinal of `raw` among the visible rows; 0 when raw is hidden/out of range. */
uint8_t ui_menu_visible_index(ui_menu_visible_fn visible, uint8_t raw_count,
                              uint8_t raw);
/* Raw index of the nth visible row; 0 when out of range. */
uint8_t ui_menu_raw_at_visible(ui_menu_visible_fn visible, uint8_t raw_count,
                               uint8_t visible_index);
/* `raw` if visible, else the nearest visible row scanning forward with
 * wraparound; 0 when nothing is visible. */
uint8_t ui_menu_normalized_raw(ui_menu_visible_fn visible, uint8_t raw_count,
                               uint8_t raw);

#endif
