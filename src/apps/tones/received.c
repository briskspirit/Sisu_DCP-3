#include "apps/tones_app.h"

#include "apps/dialogs_app.h"
#include "audio/audio_levels.h"
#include "audio/ringtone_codec.h"
#include "services/core1_services.h"
#include "services/input_keys.h"
#include "apps/profiles_app.h"
#include "services/strings.h"
#include "services/timebase.h"
#include "storage/store_service.h"

#include <string.h>

static bool playback_owned(const app_t *app) {
    return app->ringtone_playing && app->route == APP_ROUTE_DISPLAY_MESSAGE &&
        app->display_record_id == 37u && app->display_return_route == APP_ROUTE_RECEIVED_TONE;
}

void open_received_tone(app_t *app) {
    uint32_t id = store_ringtone_pending_first();
    if (id == 0u) return;
    app->ringtone_receive_id = id;
    app->ringtone_option = 0u;
    app->ringtone_save_action = 0u;
    app->ringtone_playing = false;
    app->route = APP_ROUTE_RECEIVED_TONE;
    app->dirty = true;
}

static void playback_text(char *dst, size_t cap, const char *name) {
    const char *format = ts_or(0x221u, "Playing tone\n%S");
    size_t used = 0u;
    while (*format != '\0' && used + 1u < cap) {
        if (format[0] == '%' && format[1] == 'S') {
            copy_text(dst + used, cap - used, name);
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
    dst[used] = '\0';
}

bool handle_received_tone_key(app_t *app, uint16_t key, uint32_t now) {
    if (app->ringtone_save_action != 0u) {
        if (key == KEY_C) {
            app->route = APP_ROUTE_STANDBY;
            app->dirty = true;
        }
        return true;
    }
    if (key == KEY_C) {
        app->route = APP_ROUTE_STANDBY;
        app->dirty = true;
    } else if (key == KEY_UP || key == KEY_DOWN) {
        app->ringtone_option = (uint8_t)((app->ringtone_option + (key == KEY_UP ? 2u : 1u)) % 3u);
        app->dirty = true;
    } else if (key == KEY_NAVI) {
        if (app->ringtone_option == 0u) {
            static store_own_tone_t tone;
            ringtone_info_t info;
            if (store_ringtone_pending_get(app->ringtone_receive_id, &tone) != STORE_STATUS_OK ||
                !ringtone_decode(tone.packed, tone.packed_len, &info, NULL, 0u)) {
                open_display_sid(app, 0u, 0x210u, "Not\ndone", APP_ROUTE_RECEIVED_TONE, now);
                return true;
            }
            uint8_t level = audio_level_from_ringing_volume(
                profile_get_tone_setting(profile_active_index(), PROFILE_SETTING_RINGING_VOLUME));
            core1_post_audio_composer_packed(tone.packed, tone.packed_len, level);
            char text[sizeof(app->display_text)];
            playback_text(text, sizeof(text), info.name[0] ? info.name : ts_or(0x227u, "Received tone"));
            open_display(app, 37u, text, NULL, NULL, APP_ROUTE_RECEIVED_TONE, now);
            app->ringtone_playing = true;
            app->ringtone_play_until_ms = now + info.duration_ms + 100u;
        } else {
            bool save = app->ringtone_option == 1u;
            store_status_t status = save ? store_ringtone_pending_save(app->ringtone_receive_id)
                : store_ringtone_pending_discard(app->ringtone_receive_id);
            if (status == STORE_STATUS_OK) app->ringtone_save_action = save ? 1u : 2u;
            else open_display_sid(app, 0u, 0x3b3u, "Not\nsaved", APP_ROUTE_RECEIVED_TONE, now);
        }
    }
    return true;
}

bool handle_received_tone_display_key(app_t *app, uint16_t key) {
    if (!playback_owned(app)) return false;
    if (key == KEY_C || key == KEY_NAVI) {
        core1_post_command(CORE1_CMD_AUDIO_COMPOSER_STOP, 0u);
        app->ringtone_playing = false;
        return_from_display(app);
    }
    return true;
}

bool tick_received_tone(app_t *app, uint32_t now) {
    bool changed = false;
    if (app->ringtone_playing) {
        if (!playback_owned(app)) {
            /* A call/alarm may already own the buzzer. Never stop its audio. */
            app->ringtone_playing = false;
        } else if (time_diff_ms(now, app->ringtone_play_until_ms) >= 0) {
            (void)handle_received_tone_display_key(app, KEY_C);
            changed = true;
        }
    }
    if (app->ringtone_save_action != 0u) {
        store_status_t status = store_ringtone_commit_status();
        if (status != STORE_STATUS_NOT_READY) {
            uint8_t action = app->ringtone_save_action;
            app->ringtone_save_action = 0u;
            if (app->route == APP_ROUTE_RECEIVED_TONE) {
                if (status != STORE_STATUS_OK)
                    open_display_sid(app, 0u, 0x3b3u, "Not\nsaved", APP_ROUTE_RECEIVED_TONE, now);
                else if (action == 1u)
                    open_display_sid(app, 3u, 0x224u, "Ringing\ntone\nsaved", APP_ROUTE_STANDBY, now);
                else app->route = APP_ROUTE_STANDBY;
                app->dirty = true;
            }
            changed = true;
        }
    }
    return changed;
}

void render_received_tone(const app_t *app, framebuffer_t *fb) {
    /* v6.00 menu 0x2dcf9c: Playback, Save, Discard. */
    const char *labels[] = {ts_or(0x220u, "Playback"), ts_or(0x223u, "Save"), ts_or(0x21fu, "Discard")};
    draw_flat_list(fb, labels, 3u, app->ringtone_option, 0, "Select");
}
