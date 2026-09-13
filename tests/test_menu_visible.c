#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "ui/menu_visible.h"

/* Predicate over a 6-row menu: rows 1 and 4 hidden. */
static bool gap_visible(uint8_t raw) {
    return raw < 6u && raw != 1u && raw != 4u;
}
static bool all_visible(uint8_t raw) {
    return raw < 4u;
}
static bool none_visible(uint8_t raw) {
    (void)raw;
    return false;
}

int main(void) {
    /* All-visible: identity mapping. */
    assert(ui_menu_visible_count(all_visible, 4u) == 4u);
    for (uint8_t i = 0u; i < 4u; i++) {
        assert(ui_menu_visible_index(all_visible, 4u, i) == i);
        assert(ui_menu_raw_at_visible(all_visible, 4u, i) == i);
        assert(ui_menu_normalized_raw(all_visible, 4u, i) == i);
    }

    /* Gapped: visibles are raw {0,2,3,5} -> ordinals {0,1,2,3}. */
    assert(ui_menu_visible_count(gap_visible, 6u) == 4u);
    assert(ui_menu_visible_index(gap_visible, 6u, 0u) == 0u);
    assert(ui_menu_visible_index(gap_visible, 6u, 2u) == 1u);
    assert(ui_menu_visible_index(gap_visible, 6u, 3u) == 2u);
    assert(ui_menu_visible_index(gap_visible, 6u, 5u) == 3u);
    assert(ui_menu_visible_index(gap_visible, 6u, 1u) == 0u);  /* hidden -> 0 */
    assert(ui_menu_visible_index(gap_visible, 6u, 9u) == 0u);  /* range -> 0 */
    assert(ui_menu_raw_at_visible(gap_visible, 6u, 0u) == 0u);
    assert(ui_menu_raw_at_visible(gap_visible, 6u, 1u) == 2u);
    assert(ui_menu_raw_at_visible(gap_visible, 6u, 3u) == 5u);
    assert(ui_menu_raw_at_visible(gap_visible, 6u, 4u) == 0u); /* range -> 0 */

    /* Normalize: visible raws pass through; hidden raws scan FORWARD with
     * wraparound (raw 1 -> 2; raw 4 -> 5; out-of-range 6 -> wraps to 0). */
    assert(ui_menu_normalized_raw(gap_visible, 6u, 1u) == 2u);
    assert(ui_menu_normalized_raw(gap_visible, 6u, 4u) == 5u);
    assert(ui_menu_normalized_raw(gap_visible, 6u, 6u) == 0u);
    assert(ui_menu_normalized_raw(gap_visible, 6u, 5u) == 5u);

    /* Nothing visible: everything degrades to 0. */
    assert(ui_menu_visible_count(none_visible, 6u) == 0u);
    assert(ui_menu_visible_index(none_visible, 6u, 3u) == 0u);
    assert(ui_menu_raw_at_visible(none_visible, 6u, 0u) == 0u);
    assert(ui_menu_normalized_raw(none_visible, 6u, 3u) == 0u);

    puts("test_menu_visible: all assertions passed");
    return 0;
}
