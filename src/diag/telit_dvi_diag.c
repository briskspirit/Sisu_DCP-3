#include "diag/telit_dvi_diag.h"

#include <string.h>

#include "audio/nau88c22_codec.h"
#include "dvi_clock_probe.pio.h"
#include "hal/board.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#define TELIT_DVI_BCLK_STABLE_MS 20u
#define TELIT_DVI_CLOCK_CYCLES 1024u
#define TELIT_DVI_CLOCK_TIMEOUT_US 400000u

static bool s_initialized;
static bool s_codec_ready;
static bool s_codec_i2s_ready;
static bool s_modem_i2s_ready;
static bool s_clock_probe_ready;
static bool s_start_pending;
static bool s_bclk_qualifying;
static bool s_bclk_live;
static bool s_bclk_fresh;
static bool s_saw_bclk;
static bool s_path_enabled;
static uint32_t s_bclk_since_ms;
static telit_dvi_route_t s_route = TELIT_DVI_ROUTE_HANDSET;
static PIO s_probe_pio = pio1;
static uint s_probe_sm;
static uint s_probe_offset;

static nau_route_t codec_route(telit_dvi_route_t route) {
    return route == TELIT_DVI_ROUTE_HEADSET ? NAU_ROUTE_HEADSET
                                           : NAU_ROUTE_HANDSET;
}

static bool route_is_valid(telit_dvi_route_t route) {
    return route == TELIT_DVI_ROUTE_HANDSET ||
           route == TELIT_DVI_ROUTE_HEADSET;
}

static void fill_codec_audio(void *ctx, int16_t *dst,
                             uint32_t frame_count) {
    (void)ctx;
    int16_t downlink[AUDIO_BRIDGE_BLOCK_FRAMES];
    uint32_t voice_frames = 0u;
    if (audio_bridge_active()) {
        voice_frames = frame_count < AUDIO_BRIDGE_BLOCK_FRAMES
                           ? frame_count
                           : AUDIO_BRIDGE_BLOCK_FRAMES;
        audio_bridge_downlink_pull(downlink, (uint16_t)voice_frames);
    }
    bool right_invert = audio_bridge_right_invert();
    for (uint32_t i = 0u; i < frame_count; i++) {
        int16_t mono = i < voice_frames ? downlink[i] : 0;
        dst[(i * 2u) + 0u] = mono;
        dst[(i * 2u) + 1u] = right_invert ? (int16_t)-mono : mono;
    }
}

static bool clock_probe_init(void) {
    if (s_clock_probe_ready) {
        return true;
    }
    int sm = pio_claim_unused_sm(s_probe_pio, false);
    if (sm < 0) {
        return false;
    }
    if (!pio_can_add_program(s_probe_pio, &dvi_clock_probe_program)) {
        pio_sm_unclaim(s_probe_pio, (uint)sm);
        return false;
    }
    s_probe_sm = (uint)sm;
    s_probe_offset =
        pio_add_program(s_probe_pio, &dvi_clock_probe_program);
    s_clock_probe_ready = true;
    return true;
}

static bool measure_clock(uint pin, uint32_t *out_hz) {
    pio_sm_set_enabled(s_probe_pio, s_probe_sm, false);
    pio_sm_clear_fifos(s_probe_pio, s_probe_sm);
    pio_sm_restart(s_probe_pio, s_probe_sm);

    pio_sm_config config =
        dvi_clock_probe_program_get_default_config(s_probe_offset);
    sm_config_set_in_pins(&config, pin);
    sm_config_set_wrap(&config,
                       s_probe_offset + dvi_clock_probe_wrap_target,
                       s_probe_offset + dvi_clock_probe_wrap);
    pio_sm_set_config(s_probe_pio, s_probe_sm, &config);

    uint64_t start_us = time_us_64();
    uint64_t deadline_us = start_us + TELIT_DVI_CLOCK_TIMEOUT_US;
    pio_sm_set_enabled(s_probe_pio, s_probe_sm, true);
    while (pio_sm_is_rx_fifo_empty(s_probe_pio, s_probe_sm)) {
        if (time_us_64() >= deadline_us) {
            pio_sm_set_enabled(s_probe_pio, s_probe_sm, false);
            return false;
        }
        tight_loop_contents();
    }
    uint64_t elapsed_us = time_us_64() - start_us;
    (void)pio_sm_get(s_probe_pio, s_probe_sm);
    pio_sm_set_enabled(s_probe_pio, s_probe_sm, false);
    if (elapsed_us == 0u) {
        return false;
    }
    uint64_t numerator =
        (uint64_t)TELIT_DVI_CLOCK_CYCLES * 1000000u;
    *out_hz = (uint32_t)((numerator + (elapsed_us / 2u)) / elapsed_us);
    return true;
}

static bool set_path_enabled(bool enabled) {
    bool headset = s_route == TELIT_DVI_ROUTE_HEADSET;
    if (enabled) {
        bool ok = nau88c22_codec_set_route(codec_route(s_route));
        ok = nau88c22_codec_set_mic_power(true, headset) && ok;
        ok = nau88c22_codec_set_playback_idle(false) && ok;
        if (ok) {
            s_path_enabled = true;
        } else {
            /* A failed multi-register transition may still have enabled an
             * earlier stage. Best-effort rollback keeps a rejected diagnostic
             * start from leaving the analog path powered behind a false idle
             * state; a later start reissues the full route either way. */
            (void)nau88c22_codec_set_playback_idle(true);
            (void)nau88c22_codec_set_mic_power(false, headset);
            s_path_enabled = false;
        }
        return ok;
    }
    bool ok = nau88c22_codec_set_playback_idle(true);
    ok = nau88c22_codec_set_mic_power(false, headset) && ok;
    s_path_enabled = false;
    return ok;
}

bool telit_dvi_diag_init(telit_dvi_route_t route) {
    if (!route_is_valid(route)) {
        return false;
    }
    if (s_initialized) {
        return telit_dvi_diag_set_route(route);
    }

    audio_bridge_init();
    s_route = route;
    s_codec_ready = nau88c22_codec_init();
    if (s_codec_ready) {
        bool ok = nau88c22_codec_set_playback_idle(true);
        ok = nau88c22_codec_set_mic_power(
                 false, route == TELIT_DVI_ROUTE_HEADSET) &&
             ok;
        ok = nau88c22_codec_set_route(codec_route(route)) && ok;
        s_codec_ready = ok;
    }
    s_codec_i2s_ready = audio_i2s_hal_init(fill_codec_audio, NULL);
    s_modem_i2s_ready = modem_i2s_hal_init();
    if (s_modem_i2s_ready) {
        (void)clock_probe_init();
    }
    s_initialized =
        s_codec_ready && s_codec_i2s_ready && s_modem_i2s_ready;
    return s_initialized;
}

bool telit_dvi_diag_set_route(telit_dvi_route_t route) {
    if (!s_initialized || !route_is_valid(route) || s_start_pending ||
        audio_bridge_active()) {
        return false;
    }
    s_route = route;
    return nau88c22_codec_set_route(codec_route(route));
}

bool telit_dvi_diag_start(void) {
    if (!s_initialized) {
        return false;
    }
    audio_bridge_stop();
    s_start_pending = false;
    s_bclk_qualifying = false;
    s_saw_bclk = false;
    if (!modem_i2s_hal_start()) {
        return false;
    }
    if (!set_path_enabled(true)) {
        (void)modem_i2s_hal_stop();
        return false;
    }
    modem_i2s_hal_bclk_reset();
    s_start_pending = true;
    return true;
}

void telit_dvi_diag_stop(void) {
    bool was_running = s_start_pending || audio_bridge_active() ||
                       s_path_enabled;
    s_start_pending = false;
    s_bclk_qualifying = false;
    s_bclk_live = false;
    s_bclk_fresh = false;
    s_saw_bclk = false;
    audio_bridge_stop();
    (void)modem_i2s_hal_stop();
    if (s_initialized && was_running) {
        (void)set_path_enabled(false);
    }
}

void telit_dvi_diag_tick(uint32_t now_ms) {
    if (!s_initialized) {
        return;
    }
    s_bclk_live = modem_i2s_hal_bclk_poll(&s_bclk_fresh);
    if (s_start_pending) {
        if (!s_bclk_fresh) {
            s_bclk_qualifying = false;
        } else if (!s_bclk_qualifying) {
            s_bclk_qualifying = true;
            s_bclk_since_ms = now_ms;
        } else if ((int32_t)(now_ms - s_bclk_since_ms) >=
                   (int32_t)TELIT_DVI_BCLK_STABLE_MS) {
            if (modem_i2s_hal_resync()) {
                audio_bridge_start();
                s_start_pending = false;
                s_bclk_qualifying = false;
                s_saw_bclk = true;
            } else {
                s_bclk_qualifying = false;
            }
        }
    } else if (audio_bridge_active()) {
        if (s_bclk_live) {
            s_saw_bclk = true;
        } else if (s_saw_bclk) {
            telit_dvi_diag_stop();
        }
    }
}

bool telit_dvi_diag_measure_clocks(
    telit_dvi_clock_measurement_t *out) {
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);
    if (!s_initialized || !s_clock_probe_ready) {
        return false;
    }
    out->bclk_valid = measure_clock(MODEM_I2S_PIN_CLK, &out->bclk_hz);
    out->wa_valid = measure_clock(MODEM_I2S_PIN_WA, &out->wa_hz);
    if (out->bclk_valid && out->wa_valid && out->wa_hz != 0u) {
        out->bclk_per_frame_x1000 =
            (uint32_t)(((uint64_t)out->bclk_hz * 1000u +
                        (out->wa_hz / 2u)) /
                       out->wa_hz);
    }
    return out->bclk_valid && out->wa_valid;
}

void telit_dvi_diag_get_status(telit_dvi_diag_status_t *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    out->initialized = s_initialized;
    out->codec_ready = s_codec_ready;
    out->codec_i2s_ready = s_codec_i2s_ready;
    out->modem_i2s_ready = s_modem_i2s_ready;
    out->clock_probe_ready = s_clock_probe_ready;
    out->start_pending = s_start_pending;
    out->bclk_qualifying = s_bclk_qualifying;
    out->bclk_live = s_bclk_live;
    out->bclk_fresh = s_bclk_fresh;
    out->saw_bclk = s_saw_bclk;
    out->route = s_route;
    audio_bridge_get_stats(&out->bridge);
    if (s_codec_i2s_ready) {
        audio_i2s_hal_get_stats(&out->codec_i2s);
    }
    if (s_modem_i2s_ready) {
        modem_i2s_hal_get_stats(&out->modem_i2s);
        modem_i2s_hal_tx_probe(&out->modem_tx);
    }
}

const char *telit_dvi_diag_route_text(telit_dvi_route_t route) {
    return route == TELIT_DVI_ROUTE_HEADSET ? "headset" : "handset";
}
