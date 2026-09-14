#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "apps/snake_app.h"
#include "storage/store_service.h"

static int s_failures;
static unsigned s_tone_count;
static uint8_t s_last_tone;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

void play_game_system_tone(uint8_t tone_index) {
    s_tone_count++;
    s_last_tone = tone_index;
}

int32_t time_diff_ms(uint32_t a, uint32_t b) {
    return (int32_t)(a - b);
}

uint32_t time_ticks8(void) {
    return 0u;
}

store_status_t store_setting_set_u8(store_setting_key_t key, uint8_t value) {
    (void)key;
    (void)value;
    return STORE_STATUS_OK;
}

store_status_t store_setting_set_u16(store_setting_key_t key, uint16_t value) {
    (void)key;
    (void)value;
    return STORE_STATUS_OK;
}

const char *ts_or(uint16_t sid, const char *fallback) {
    (void)sid;
    return fallback;
}

static void reset_tones(void) {
    s_tone_count = 0u;
    s_last_tone = 0u;
}

static app_t playing_snake(void) {
    app_t app = {0};
    app.route = APP_ROUTE_SNAKE_PLAY;
    app.snake_level_byte = 1u;
    app.snake_direction = 1u;
    app.snake_pending_direction = 1u;
    app.snake_next_tick_ms = 1000u;
    app.snake_food_x = 10u;
    app.snake_food_y = 5u;
    return app;
}

static void test_wall_collision_audio(void) {
    app_t app = playing_snake();
    app.snake_body_len = 1u;
    app.snake_body_x[0] = 19u;
    app.snake_body_y[0] = 5u;
    reset_tones();

    check(tick_snake(&app, 1000u), "wall collision reports a state change");
    check(app.snake_game_over, "wall collision freezes the game");
    check(app.snake_next_tick_ms == 0u, "wall collision stops movement ticks");
    check(app.snake_crash_deadline_ms == 1200u,
          "wall collision retains the 200 ms result settle");
    check(s_tone_count == 0u,
          "wall collision does not replay the food tone");

    check(!tick_snake(&app, 1199u), "result remains pending before the settle deadline");
    check(s_tone_count == 0u, "settle wait remains silent");
    check(tick_snake(&app, 1200u) && app.route == APP_ROUTE_SNAKE_RESULT,
          "settle deadline opens the result screen");
    check(s_tone_count == 1u && s_last_tone == 17u,
          "ordinary score result plays the game-over melody");
}

static void test_self_collision_audio(void) {
    app_t app = playing_snake();
    app.snake_body_len = 4u;
    app.snake_body_x[0] = 5u;
    app.snake_body_y[0] = 6u;
    app.snake_body_x[1] = 5u;
    app.snake_body_y[1] = 5u;
    app.snake_body_x[2] = 6u;
    app.snake_body_y[2] = 5u;
    app.snake_body_x[3] = 6u;
    app.snake_body_y[3] = 6u;
    app.snake_direction = 0u;
    app.snake_pending_direction = 0u;
    reset_tones();

    check(tick_snake(&app, 1000u), "self collision reports a state change");
    check(app.snake_game_over, "self collision freezes the game");
    check(s_tone_count == 0u, "self collision remains silent during the settle");
    check(tick_snake(&app, 1200u) && app.route == APP_ROUTE_SNAKE_RESULT,
          "self collision opens the result screen after the settle");
    check(s_tone_count == 1u && s_last_tone == 17u,
          "self-collision result plays the game-over melody");
}

static void test_food_and_result_tones(void) {
    app_t app = playing_snake();
    app.snake_body_len = 1u;
    app.snake_body_x[0] = 9u;
    app.snake_body_y[0] = 5u;
    reset_tones();

    check(tick_snake(&app, 1000u), "eating food reports a state change");
    check(s_tone_count == 1u && s_last_tone == 16u,
          "tone 16 remains exclusive to eating food");

    app = playing_snake();
    app.snake_body_len = 1u;
    app.snake_body_x[0] = 19u;
    app.snake_body_y[0] = 5u;
    app.snake_score = 4u;
    app.snake_top_score = 2u;
    reset_tones();

    check(tick_snake(&app, 1000u), "record-setting collision enters the settle delay");
    check(s_tone_count == 0u, "record-setting collision is initially silent");
    check(tick_snake(&app, 1200u) && app.route == APP_ROUTE_SNAKE_RESULT,
          "record-setting result opens after the stock delay");
    check(app.snake_result_top_score && app.snake_top_score == 4u,
          "result records the new top score");
    check(s_tone_count == 1u && s_last_tone == 20u,
          "new top-score result plays the winning melody");
}

int main(void) {
    test_wall_collision_audio();
    test_self_collision_audio();
    test_food_and_result_tones();

    if (s_failures != 0) {
        fprintf(stderr, "%d Snake app test(s) failed\n", s_failures);
        return 1;
    }
    puts("Snake app tests passed");
    return 0;
}
