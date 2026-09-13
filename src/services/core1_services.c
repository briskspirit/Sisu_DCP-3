#include "services/core1_services.h"

#include "audio/audio_service.h"
#include "audio/audio_bridge.h"
#include "audio/audio_levels.h"
#include "audio/buzzer_hal.h"
#include "audio/nau88c22_codec.h"
#include "hal/accessory_hal.h"
#include "hal/board.h"
#include "services/log.h"
#include "services/shared_3v8_service.h"
#include "services/stack_monitor.h"
#include "services/timebase.h"
#include "storage/store_service.h"
#include "audio/vibra_hal.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/sync.h"
#include "hardware/watchdog.h"

#include "audio/audio_i2s_hal.h"
#include "audio/modem_i2s_hal.h"

#include <string.h>

#define CORE1_COMMAND_QUEUE_LEN 32u
#define AUDIO_3V8_POWER_GOOD_TIMEOUT_US 25000u
#define AUDIO_3V8_RELEASE_HOLDOFF_MS 100u

/* Bounded park handshake: core1's loop checks the request each ~1 ms iteration,
 * so 100 ms is generous. On timeout core0 skips the commit (begin) or reboots
 * (end) rather than hard-hanging. */
#define CORE1_FLASH_PARK_TIMEOUT_MS 100u

typedef struct {
    core1_cmd_t cmd;
    uint16_t arg;
    uint32_t posted_ms;
} core1_command_t;

static void core1_main(void);
static bool pop_command(core1_command_t *out);
static void finish_command_dispatch(void);
static void push_command_locked(core1_cmd_t cmd, uint16_t arg);

static critical_section_t s_command_lock;
static core1_command_t s_command_queue[CORE1_COMMAND_QUEUE_LEN];
static uint8_t s_command_head;
static uint8_t s_command_tail;
static uint8_t s_command_count;
static uint8_t s_command_high_water;
static uint32_t s_command_drops;
/* Protected by s_command_lock. Remains set from dequeue until the command has
 * finished dispatching, closing the queue-empty/audio-not-yet-active handoff
 * window seen by core0's shared-rail release tick. */
static bool s_command_dispatching;
/* Written by core0 in core1_services_start() before core1 launches, then read
 * by core1 on every pop. Marked volatile so the compiler cannot hoist/cache the
 * read; a barrier in core1_services_start() publishes it before core1 starts. */
static volatile bool s_command_lock_ready;
/* Published with the queue state before core1 launches, then immutable. */
static bool s_modem_voice_transport_available;
static uint8_t s_composer_packed[CORE1_AUDIO_COMPOSER_PACKED_MAX];
static uint16_t s_composer_packed_len;
static uint8_t s_composer_packed_level;
/* Core0-side owner latch. A short holdoff covers the post-to-core1-dispatch gap
 * and very short buzzer chirps; release also waits for the command queue and all
 * audio activity to drain, so +3V8 cannot disappear under a live waveform. */
static bool s_audio_3v8_owned;
static uint32_t s_audio_3v8_release_not_before_ms;

/* --- Flash-write coordination (see header) ------------------------------- *
 * core0 sets s_flash_pause_req; core1, from its main loop, parks in the RAM-
 * resident core1_flash_park() with interrupts disabled (so neither its loop nor
 * its DMA IRQ handler -- both in XIP flash -- executes while core0 erases). We
 * poll a flag through core1's main loop instead of the SDK's FIFO-IRQ lockout,
 * which the audio-resident core1 was not acknowledging. */
static volatile bool s_flash_pause_req;
static volatile bool s_flash_parked;
static volatile bool s_shutdown_ack;
static volatile bool s_core1_idle_waiting;
static volatile uint32_t s_core1_heartbeat_ms;
static volatile uint32_t s_core1_loop_count;
static volatile uint32_t s_flash_pause_attempts;
static volatile uint32_t s_flash_pause_successes;
static volatile uint32_t s_flash_pause_timeouts;
static volatile uint32_t s_flash_resume_timeouts;
static volatile uint32_t s_flash_park_entries;
static volatile uint32_t s_flash_park_last_flags;
static volatile uint32_t s_gate_requests;
static volatile uint32_t s_gate_successes;
static volatile uint32_t s_gate_races;
static volatile uint32_t s_codec_recovery_attempts;
static volatile uint32_t s_codec_recovery_successes;
static volatile uint32_t s_codec_recovery_failures;
static volatile uint32_t s_bridge_start_posts;
static volatile uint32_t s_bridge_stop_posts;
static volatile uint32_t s_bridge_last_dispatch_ms;
static volatile uint32_t s_bridge_max_dispatch_ms;

/* --- The audio idle gate ------------------------------------------------ *
 * Core1 owns the I2S park state (s_audio_gated: written only on core1, read
 * by core0 as an ack). s_audio_resume_count increments on every core1 resume
 * -- core0 snapshots it when requesting a gate and refuses to power down the
 * codec if it moved, which closes the "sound got queued while the gate was in
 * flight" race (a tone must never play into a muted/stopped codec). All codec
 * I2C and the MCLK gpout stay core0-only, per the cross-core rules. */
#define AUDIO_GATE_HOLDOFF_MS 1500u /* [BP] silence before parking */

static volatile bool s_audio_gated;
static volatile uint32_t s_audio_resume_count;
/* core0: `augate` console toggle. Default ON (user decision 2026-07-06 after
 * the pop-tests passed). NOTE: an unreproduced ~340 mA post-incoming-call
 * draw was seen once with the gate on (modem-involved, not root-caused) -- if
 * it recurs, `augate off` is the first thing to try and `modemrail off`
 * isolates the modem. */
static bool s_gate_enabled = true;
static bool s_codec_idled;        /* core0: DAC/mixers down + MCLK stopped */
static uint32_t s_codec_recover_ms; /* core0: last codec re-init retry (elapsed-since) */
/* Semantic call-volume intent survives a runtime codec reset. calls_app can
 * publish it just before core1 has dispatched BRIDGE_START, so an explicit
 * latch is stronger than audio_bridge_active() for recovery. Call teardown
 * and codec standby clear it before restoring the ordinary tone baselines. */
static uint8_t s_call_volume_level = 5u;
static bool s_call_volume_active;
static bool s_gate_requested;     /* core0: GATE posted, waiting for the core1 ack */
static uint32_t s_gate_resume_snapshot;
static uint32_t s_audio_quiet_since_ms;
static bool s_audio_quiet_valid;

/* Which commands START sound (need the codec live). Excluded: STOP (must not
 * wake the codec on every key release), vibra (PWM, no I2S), calibration/gain
 * setters (core1 software mixer variables), and the gate plumbing itself. */
static bool cmd_starts_audio(core1_cmd_t cmd) {
    switch (cmd) {
    case CORE1_CMD_AUDIO_CLICK:
    case CORE1_CMD_AUDIO_DTMF:
    case CORE1_CMD_AUDIO_SYSTEM_TONE:
    case CORE1_CMD_AUDIO_SYSTEM_TONE_QUIET:
    case CORE1_CMD_AUDIO_SYSTEM_TONE_QUIET_LOOP:
    case CORE1_CMD_AUDIO_RINGTONE_PREVIEW:
    case CORE1_CMD_AUDIO_RINGTONE_LOOP:
    case CORE1_CMD_AUDIO_SYSTEM_TONE_LOOP:
    case CORE1_CMD_AUDIO_PACMAN_TONE:
    case CORE1_CMD_AUDIO_COMPOSER_NOTE:
    case CORE1_CMD_AUDIO_COMPOSER_PACKED:
    case CORE1_CMD_AUDIO_COMPOSER_PACKED_LOOP:
    case CORE1_CMD_AUDIO_RINGTONE_MENU_PREVIEW:
    case CORE1_CMD_AUDIO_TONES_SYSTEM_PREVIEW:
    case CORE1_CMD_AUDIO_TONES_CLICK_PREVIEW:
    case CORE1_CMD_AUDIO_DEBUG_BUZZER_TEST:
    case CORE1_CMD_AUDIO_BRIDGE_START:
        return true;
    default:
        return false;
    }
}

/* core0: bring the codec/MCLK back before a sound-starting command is queued.
 * Order: MCLK first (the codec needs it to process I2S), then DACs/mixers +
 * unmute. The I2S PIO/DMA resume happens on core1 when it dispatches the
 * command. Also cancels any in-flight gate request. */
static void audio_gate_unidle(void) {
    if (s_codec_idled) {
        board_start_mclk_output();
        /* Only clear the idled flag if the DAC/mixer resume actually took. A
         * failed set_playback_idle(false) (a transient I2C glitch, or the codec
         * mid-wedge) previously still cleared it -- leaving the codec DAC-down
         * while core1 believed it live (silent audio, and the gate would never
         * retry). Leaving it set means the next sound-start re-attempts the
         * resume; a recovered codec (already not-idle) makes the retry a no-op
         * that then clears the flag. */
        if (nau88c22_codec_set_playback_idle(false)) {
            s_codec_idled = false;
        }
    }
    s_gate_requested = false;
    s_audio_quiet_valid = false;
}

/* Bounded rate for the codec re-init retry after an I2C bus-wedge latch. A
 * failed attempt costs only the device-ID probe (~2 x BOARD_I2C_TIMEOUT_US);
 * the ~500 ms depop ramp is paid once, when the bus is actually back. */
#define CODEC_RECOVER_INTERVAL_MS 1000u

bool core1_services_codec_init(void) {
    /* The NAU output-driver depop delays are MCLK-counted. Soft-off can leave
     * the audio gate parked with clk_gpout0 stopped, so running codec_init()
     * directly merely waits 500 ms of wall time with no codec clocks. The
     * first later sound then starts MCLK and disappears inside the deferred
     * depop interval. Establish the complete clock contract here first. */
    board_exit_xosc_lowpower();
    board_start_mclk_output();
    if (!nau88c22_codec_init()) {
        board_stop_mclk_output();
        return false;
    }

    /* init() resets the codec to its handset route. That is correct for the
     * earliest cold-boot call (accessory_hal has not been initialized yet),
     * but wrong after soft-off: the accessory detector keeps its debounced
     * state while codec standby drops the analog block. Reconcile that state
     * here so the first key tone after power-on/alarm wake cannot leak through
     * the handset receiver while a headset is already inserted. A later
     * physical insert/remove edge still follows the canonical modem-service
     * reroute path. */
    bool headset = accessory_hal_headset_inserted();
    bool reconciled = nau88c22_codec_set_mic_power(false, headset);
    reconciled = nau88c22_codec_set_route(headset ? NAU_ROUTE_HEADSET
                                                  : NAU_ROUTE_HANDSET) && reconciled;
    if (!reconciled) {
        nau88c22_codec_set_recover_pending(true);
    }

    /* A full init leaves DACs/mixers and the selected output live. The prior
     * soft-off instance may have left both gate latches true; publish the real
     * hardware state so the next sound can resume I2S immediately and the
     * ordinary quiet timer can park the new instance again. */
    s_codec_idled = false;
    s_gate_requested = false;
    s_audio_quiet_valid = false;
    return true;
}

void core1_services_codec_standby(void) {
    s_call_volume_active = false;
    nau88c22_codec_power_standby();
}

void core1_services_codec_set_call_volume(uint8_t level) {
    if (level < AUDIO_CALL_VOLUME_MIN) {
        level = AUDIO_CALL_VOLUME_MIN;
    } else if (level > AUDIO_CALL_VOLUME_MAX) {
        level = AUDIO_CALL_VOLUME_MAX;
    }
    /* Cache before I2C: a failed write is exactly when recovery needs the
     * desired semantic level. */
    s_call_volume_level = level;
    s_call_volume_active = true;
    (void)nau88c22_codec_set_call_volume(level);
}

void core1_services_codec_reset_call_volume(void) {
    /* Clear first so a failure that arms recovery cannot reapply stale call
     * attenuation after the call has already ended. */
    if (!s_call_volume_active) {
        return;
    }
    s_call_volume_active = false;
    (void)nau88c22_codec_reset_output_gains();
}

/* core0 main-loop tick (gated on NOT power-off by the caller): self-heal the
 * codec after a runtime I2C bus wedge, the analog of tca8418_hal/rv8803_hal's
 * self-heal which the codec lacked (dead until a power cycle otherwise). Lives
 * here, not in the codec driver, because a correct recovery has to touch the
 * things this module owns: restore full clock (MCLK = clk_sys/25, so it is the
 * wrong frequency under Phase-4 clock-down), start MCLK, and -- on success --
 * reconcile the idle-gate bookkeeping so the gate re-idles the now fully-powered
 * codec on the next quiet window instead of believing it is still idle. */
void core1_services_codec_recover_tick(uint32_t now_ms) {
    if (!nau88c22_codec_recover_pending()) {
        return;
    }
    if ((uint32_t)(now_ms - s_codec_recover_ms) < CODEC_RECOVER_INTERVAL_MS) {
        return; /* elapsed-since gate: wrap-safe, re-based every attempt */
    }
    s_codec_recover_ms = now_ms;
    s_codec_recovery_attempts++;
    /* Same clock bring-up a sound-start does (audio_start_prologue): exit
     * clock-down, then start MCLK. board_exit is a no-op when not clocked down
     * and MUST run on core0 (pll_init); every caller here is core0. */
    board_exit_xosc_lowpower();
    board_start_mclk_output();
    if (nau88c22_codec_init()) {
        /* init reset the codec to its DEFAULT (handset route, mic-off, default
         * gains). Reconcile it with the CURRENT accessory + call state, mirroring
         * the canonical call-start / accessory-change paths -- the ACTIVE-state
         * bridge follower only reconfigures on an IDLE->ACTIVE edge, which a
         * mid-session recovery is not. ALWAYS restore the route from accessory_hal
         * (not just during a call): otherwise a recovery while idle with a headset
         * inserted leaves earpiece-path sounds (keytones/tones) on the handset
         * transducer until the next insert/remove or call. During a call, also
         * bring the mic chain up so the recovered call is not left mute-uplink.
         * Route is derived from the physical accessory, NOT audio_bridge_route():
         * a set_route that failed mid-wedge never republished the bridge format,
         * so the cached route can be stale; set_route re-publishes it, re-syncing
         * core1's uplink channel. Reapply the latched semantic call level too:
         * calls_app may have published it just before BRIDGE_START was dispatched,
         * so audio_bridge_active() alone is not a sufficient ownership test. */
        bool headset = accessory_hal_headset_inserted();
        bool reconciled = nau88c22_codec_set_mic_power(audio_bridge_active(), headset);
        reconciled = nau88c22_codec_set_route(headset ? NAU_ROUTE_HEADSET
                                                      : NAU_ROUTE_HANDSET) && reconciled;
        if (s_call_volume_active) {
            reconciled = nau88c22_codec_set_call_volume(s_call_volume_level) && reconciled;
        }
        /* init() cleared s_recover_pending, but the route/mic reconcile above can
         * still NAK -- and a SINGLE fault won't re-trip the 8-fail latch, so
         * recovery would not otherwise retry, stranding a call on the wrong route /
         * mute-uplink. Re-arm on a reconcile failure so the next tick retries
         * (loop-safe: init() no-ops while s_ready, the reconcile re-issues, and a
         * success clears it). */
        nau88c22_codec_set_recover_pending(!reconciled);
        if (reconciled) {
            s_codec_recovery_successes++;
        } else {
            s_codec_recovery_failures++;
        }
        /* Clear the stale idle-gate state so the gate re-idles it on the next
         * quiet window rather than short-circuiting on s_codec_idled. */
        s_codec_idled = false;
        s_gate_requested = false;
        s_audio_quiet_valid = false;
    } else {
        s_codec_recovery_failures++;
        if (!audio_bridge_active() && !audio_service_is_active()) {
            /* Failed attempt with NOTHING using the codec: don't strand MCLK
             * running across a prolonged wedge. A live voice call or local
             * playback still needs it, so only stop it when truly idle. */
            board_stop_mclk_output();
        }
    }
}

/* core0-only prologue shared by EVERY sound-starting post path (core1_post_command
 * and post_composer_packed -- the composer "Own tone" ring goes through the latter,
 * not the former). If we're in the quiet standby clock-down (6 MHz), restore full
 * clock BEFORE core1 can dequeue: core1 bakes the buzzer / codec-I2S dividers from
 * live clock_get_hz(clk_sys) (buzzer_hal.c:66, audio_i2s_hal.c:61), so a 6 MHz read
 * latches the wrong frequency and stays wrong after the main loop restores 153.6 MHz
 * -- a mis-pitched ring. Doing it before the push_command_locked() publish point
 * guarantees core1 reads the restored clk_sys. board_exit is a no-op when not clocked
 * down, and MUST run on core0 (it does pll_init); every caller here is core0. Then
 * unpark the codec/MCLK. Keep BOTH post sites routed through here so cmd_starts_audio()
 * can't silently diverge from the clock-restore again. */
static void audio_start_prologue(bool buzzer_required) {
    board_exit_xosc_lowpower();
    if (buzzer_required) {
        shared_3v8_service_set_required(SHARED_3V8_OWNER_AUDIO, true);
        s_audio_3v8_owned = true;
        s_audio_3v8_release_not_before_ms =
            time_ms() + AUDIO_3V8_RELEASE_HOLDOFF_MS;
    }
    audio_gate_unidle();
    if (buzzer_required &&
        !shared_3v8_service_wait_power_good(AUDIO_3V8_POWER_GOOD_TIMEOUT_US)) {
        LOGW("audio", "+3V8 PG timeout before buzzer playback");
    }
}

static void audio_3v8_release_tick(uint32_t now_ms) {
    if (!s_audio_3v8_owned ||
        time_diff_ms(now_ms, s_audio_3v8_release_not_before_ms) < 0 ||
        !s_command_lock_ready) {
        return;
    }
    critical_section_enter_blocking(&s_command_lock);
    /* Read all three parts while core1 cannot cross a dequeue/dispatch boundary.
     * With no queued/in-flight command, core1 can only take active audio toward
     * idle; it cannot start new playback until core0 posts another command. */
    bool audio_idle = s_command_count == 0u && !s_command_dispatching &&
                      !audio_service_is_active();
    critical_section_exit(&s_command_lock);
    if (!audio_idle) {
        return;
    }
    shared_3v8_service_set_required(SHARED_3V8_OWNER_AUDIO, false);
    s_audio_3v8_owned = false;
}

void core1_services_audio_gate_set_enabled(bool enabled) {
    if (!enabled && s_gate_enabled) {
        /* Tear the gate down symmetrically: codec back up, I2S resumed. */
        audio_gate_unidle();
        core1_post_command(CORE1_CMD_AUDIO_UNGATE, 0u);
    }
    s_gate_enabled = enabled;
    s_audio_quiet_valid = false;
    s_gate_requested = false;
}

bool core1_services_audio_gate_enabled(void) {
    return s_gate_enabled;
}

bool core1_services_audio_gated(void) {
    return s_audio_gated;
}

bool core1_services_standby_ready(void) {
    if (!s_command_lock_ready) {
        return true;
    }
    critical_section_enter_blocking(&s_command_lock);
    bool command_idle = s_command_count == 0u && !s_command_dispatching;
    critical_section_exit(&s_command_lock);

    return command_idle && !s_flash_pause_req && !s_flash_parked &&
           !s_gate_requested && s_audio_gated && s_codec_idled &&
           !s_audio_3v8_owned && !audio_service_is_active() &&
           !audio_bridge_active() && !modem_i2s_hal_running() &&
           !nau88c22_codec_recover_pending();
}

void core1_services_audio_gate_tick(uint32_t now_ms) {
    audio_3v8_release_tick(now_ms);
    if (!s_gate_enabled || s_codec_idled || !s_command_lock_ready) {
        return;
    }
    if (audio_service_is_active() || audio_bridge_active()) {
        s_audio_quiet_valid = false;
        s_gate_requested = false;
        return;
    }
    if (!s_audio_quiet_valid) {
        s_audio_quiet_valid = true;
        s_audio_quiet_since_ms = now_ms;
        return;
    }
    if (!s_gate_requested) {
        if ((int32_t)(now_ms - (s_audio_quiet_since_ms + AUDIO_GATE_HOLDOFF_MS)) >= 0) {
            s_gate_resume_snapshot = s_audio_resume_count;
            s_gate_requested = true;
            core1_post_command(CORE1_CMD_AUDIO_GATE, 0u);
        }
        return;
    }
    /* Waiting for the ack. Anything resumed the I2S since our request means a
     * sound raced in: re-arm from scratch, never touch the codec. */
    if (s_audio_resume_count != s_gate_resume_snapshot) {
        s_gate_races++;
        s_gate_requested = false;
        s_audio_quiet_valid = false;
        return;
    }
    if (s_audio_gated) {
        s_gate_successes++;
        /* core1 parked the I2S and nothing has resumed it: safe to power the
         * playback path down. core0 is single-threaded, so no new sound can
         * be posted between this check and the MCLK stop below. s_codec_idled is
         * marked regardless of the idle-write result on purpose: audio_gate_unidle
         * gates its un-mute retry on s_codec_idled, so forcing it true here
         * guarantees the NEXT sound-start re-issues the un-mute and recovers the
         * codec even after a PARTIAL idle write (e.g. mute applied but the DAC
         * power write NAK'd) -- clearing it on failure instead would strand that
         * sound silent. The residual (DAC briefly powered in a quiet window on a
         * failed idle) is a minor power leak, cheaper than a silent ring/call. */
        nau88c22_codec_set_playback_idle(true);
        board_stop_mclk_output();
        s_codec_idled = true;
        s_gate_requested = false;
    }
}

static void __no_inline_not_in_flash_func(core1_flash_park)(void) {
    uint32_t save = save_and_disable_interrupts();
    __dmb(); /* order the IRQ-mask (kills the XIP-resident DMA ISR) BEFORE we
              * publish parked, so core0 can never observe parked==true while
              * core1's interrupts are still enabled. */
    /* STOP the codec DMA/PIO while its refill ISR is masked. Its address rings
     * now contain even an unexpectedly delayed ISR, while the clean stop avoids
     * stale audio and pending completions across the erase. Runs while XIP is
     * still up: core0 starts erasing only after s_flash_parked becomes true.
     * A2+R2: when the audio gate already parked the codec I2S, skip its
     * quiesce AND (crucially) its resume -- the unconditional resume here was
     * the documented reason a one-shot audio stop could never stick. */
    bool audio_was_gated = s_audio_gated;
    /* The modem transport starts before the mixer bridge while BCLK is being
     * qualified. Hardware ownership, rather than the later bridge-visible
     * state, must decide whether its DMA ISR needs parking across XIP loss. */
    bool modem_was_active = modem_i2s_hal_running();
    uint32_t park_flags =
        (audio_was_gated ? CORE1_FLASH_PARK_AUDIO_GATED : 0u) |
        (modem_was_active ? CORE1_FLASH_PARK_MODEM_RUNNING : 0u);
    s_flash_park_last_flags = park_flags;
    s_flash_park_entries++;
    __dmb();
    bool audio_stopped = true;
    if (!audio_was_gated) {
        audio_stopped = audio_i2s_hal_flash_quiesce();
    }
    /* Only a started transport owns live DMA/PIO now. Park it even during the
     * pre-bridge BCLK qualification window: its SRAM IRQ handler calls helpers
     * in XIP, so no live DMA completion may cross a flash operation. */
    bool modem_stopped = true;
    if (modem_was_active) {
        modem_stopped = modem_i2s_hal_flash_quiesce();
    }
    if (!audio_stopped || !modem_stopped) {
        park_flags |= !audio_stopped ? CORE1_FLASH_PARK_CODEC_FAIL : 0u;
        park_flags |= !modem_stopped ? CORE1_FLASH_PARK_MODEM_FAIL : 0u;
        s_flash_park_last_flags = park_flags;
        __dmb();
        /* Never acknowledge a flash park unless every DMA channel reached a
         * hardware-confirmed safe state. Resume only sides that are known safe;
         * core0 will time out this generation without touching flash. */
        if (!audio_was_gated && audio_stopped) {
            audio_i2s_hal_flash_resume();
        }
        if (modem_was_active && modem_stopped) {
            modem_i2s_hal_flash_resume();
        }
        restore_interrupts(save);
        return;
    }
    s_flash_park_last_flags = park_flags | CORE1_FLASH_PARK_ACKED;
    __dmb();
    s_flash_parked = true;
    __dmb();
    while (s_flash_pause_req) {
        tight_loop_contents();
    }
    __dmb();
    /* core0 has finished the op and re-enabled XIP before clearing the request,
     * so it is safe to run the XIP-resident re-arm here. */
    if (!audio_was_gated) {
        audio_i2s_hal_flash_resume();
    }
    if (modem_was_active) {
        modem_i2s_hal_flash_resume();
    }
    __dmb();
    s_flash_parked = false; /* clear BEFORE re-enabling IRQs so core0's end()
                             * wait cannot release while our ISRs are re-armed. */
    __dmb();
    restore_interrupts(save);
}

bool core1_flash_pause_begin(void) {
    /* Before core1 is launched it is not executing, so nothing to park. */
    if (!s_command_lock_ready) {
        return true;
    }
    s_flash_pause_attempts++;
    /* Restore full clock before the commit. The Phase-4 standby clock-down runs
     * clk_sys at 6 MHz, and store_service_tick (hence a flash commit) can fire while
     * clocked down -- the clock-down gate keys off the UI dirty flag, not the separate
     * flash-dirty units. Flash erase/program are only bench-validated at 153.6 MHz, and
     * the DMA-abort spin bound in *_flash_quiesce() is sized for full clock; pull clk_sys
     * up here so both hold, mirroring audio_start_prologue(). No-op when not clocked down;
     * the main loop re-enters clock-down after the commit. Core0-only (does pll_init). */
    board_exit_xosc_lowpower();
    /* TOCTOU guard: s_flash_parked is a level flag reused across handshake
     * generations. If a PRIOR generation timed out (begin() returned false while
     * core1 was still slow to park), that core1 park can set s_flash_parked=true
     * a moment later and then self-unpark. If this begin() armed and read that
     * STALE true as our ack, core0 would start erasing flash while core1 is mid-
     * unpark running the XIP-resident *_flash_resume() -> HardFault/.bss
     * corruption. So drain any residual park BEFORE arming: a parked==true here is
     * always a self-clearing abandoned park (core0 is the sole, single-threaded
     * caller, and the prior generation's request was already cleared, so core1's
     * while(req) spin has exited). If it does NOT clear, core1 is wedged -- skip
     * the op (same conservative outcome as the park-timeout below). */
    absolute_time_t drain = make_timeout_time_ms(CORE1_FLASH_PARK_TIMEOUT_MS);
    while (s_flash_parked && !time_reached(drain)) {
        tight_loop_contents();
    }
    if (s_flash_parked) {
        s_flash_pause_timeouts++;
        return false;
    }
    __dmb();  /* order the drained parked==false before we arm the new request */
    s_flash_pause_req = true;
    __dmb();  /* publish the request before we start waiting on the ack */
    __sev();  /* nudge core1 out of any sleep_ms() WFE (latency hint only) */
    /* Bounded in PRODUCTION too: if core1 cannot confirm it is parked (with its
     * DMA quiesced) in time, DO NOT run the op -- core1 never quiesced, so its
     * DMA is still healthy; the caller skips and retries. A non-parking core1
     * must never hard-hang core0. */
    absolute_time_t deadline = make_timeout_time_ms(CORE1_FLASH_PARK_TIMEOUT_MS);
    while (!s_flash_parked && !time_reached(deadline)) {
        tight_loop_contents();
    }
    if (!s_flash_parked) {
        s_flash_pause_req = false;
        __dmb();
        s_flash_pause_timeouts++;
        uint32_t timeout_count = s_flash_pause_timeouts;
        if (timeout_count == 1u || (timeout_count & (timeout_count - 1u)) == 0u) {
            LOGW("core1",
                 "flash park timeout #%lu entries=%lu flags=0x%02lx hb=%lu loop=%lu",
                 (unsigned long)timeout_count,
                 (unsigned long)s_flash_park_entries,
                 (unsigned long)s_flash_park_last_flags,
                 (unsigned long)s_core1_heartbeat_ms,
                 (unsigned long)s_core1_loop_count);
        }
        return false;
    }
    __dmb();  /* acquire: order the parked==true observation before the caller
               * disconnects XIP / erases flash. */
    s_flash_pause_successes++;
    return true;
}

void core1_flash_pause_end(void) {
    s_flash_pause_req = false;
    __dmb();  /* publish the release before waiting for core1 to unpark */
    __sev();
    absolute_time_t deadline = make_timeout_time_ms(CORE1_FLASH_PARK_TIMEOUT_MS);
    while (s_flash_parked && !time_reached(deadline)) {
        tight_loop_contents();
    }
    __dmb();
    if (s_flash_parked) {
        s_flash_resume_timeouts++;
        /* core1 wedged mid-commit: reboot rather than hard-hang core0. The
         * dual-slot CRC journal recovers the last consistent state on boot. */
        watchdog_reboot(0, 0, 0);
        while (true) {
            tight_loop_contents();
        }
    }
}

/* POWMAN power-off terminal state: never returns. Runs on core1. */
static void core1_shutdown_park(void) {
    audio_service_command(CORE1_CMD_AUDIO_STOP, 0u);
    audio_service_bridge_stop();
    /* The quiesce helpers stop the codec/modem DMA+PIO cold; with no resume to
     * follow (the "sticky audio off" the park path can't provide), the I2S
     * plumbing stays dead until the wake reboot re-inits everything. */
    if (!s_audio_gated) {
        audio_i2s_hal_flash_quiesce();
    }
    modem_i2s_hal_flash_quiesce();
    (void)save_and_disable_interrupts();
    __dmb();
    s_shutdown_ack = true;
    __dmb();
    while (true) {
        __wfi();
    }
}

bool core1_services_shutdown(uint32_t timeout_ms) {
    if (!s_command_lock_ready) {
        return true; /* core1 never launched: nothing to shut down */
    }
    core1_post_command(CORE1_CMD_SHUTDOWN, 0u);
    __sev(); /* nudge core1 out of its sleep_ms() WFE (latency hint only) */
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!s_shutdown_ack && !time_reached(deadline)) {
        tight_loop_contents();
    }
    if (!s_shutdown_ack) {
        LOGW("core1", "shutdown ack timeout; powering off regardless");
    }
    return s_shutdown_ack;
}

void core1_services_start(bool modem_voice_transport_available) {
    critical_section_init(&s_command_lock);
    s_command_head = 0;
    s_command_tail = 0;
    s_command_count = 0;
    s_command_high_water = 0u;
    s_command_drops = 0u;
    s_command_dispatching = false;
    s_core1_idle_waiting = false;
    s_core1_heartbeat_ms = time_ms();
    s_core1_loop_count = 0u;
    s_flash_pause_attempts = 0u;
    s_flash_pause_successes = 0u;
    s_flash_pause_timeouts = 0u;
    s_flash_resume_timeouts = 0u;
    s_flash_park_entries = 0u;
    s_flash_park_last_flags = 0u;
    s_gate_requests = 0u;
    s_gate_successes = 0u;
    s_gate_races = 0u;
    s_codec_recovery_attempts = 0u;
    s_codec_recovery_successes = 0u;
    s_codec_recovery_failures = 0u;
    s_bridge_start_posts = 0u;
    s_bridge_stop_posts = 0u;
    s_bridge_last_dispatch_ms = 0u;
    s_bridge_max_dispatch_ms = 0u;
    s_modem_voice_transport_available = modem_voice_transport_available;
    s_command_lock_ready = true;
    __dmb(); /* publish the queue/ready state before core1 observes it */
    multicore_launch_core1(core1_main);
}

void core1_post_command(core1_cmd_t cmd, uint16_t arg) {
    if (!s_command_lock_ready || cmd == CORE1_CMD_NONE) {
        return;
    }
    if (cmd_starts_audio(cmd)) {
        /* Full clock + codec/MCLK and, for magnetic-buzzer routes, shared +3V8
         * are live BEFORE the command is queued. */
        audio_start_prologue(audio_service_command_uses_buzzer(cmd, arg));
    }
    critical_section_enter_blocking(&s_command_lock);
    push_command_locked(cmd, arg);
    critical_section_exit(&s_command_lock);
    __sev(); /* R3: wake core1 if it is parked in WFE (idle) */
    /* Arm the commit guard synchronously on core0 the instant audio is
     * requested, before core1 pops/starts it, so a flash commit can't slip into
     * the post-to-playback gap. The main-loop audio-active gate then sustains it
     * for the rest of (longer-than-guard) playback. */
    store_service_defer_commits_until(time_ms() + STORE_COMMIT_AUDIO_GUARD_MS);
}

static void post_composer_packed(core1_cmd_t cmd, const uint8_t *data, uint16_t len, uint8_t level) {
    if (!s_command_lock_ready || data == 0 || len == 0u || level == AUDIO_LEVEL_SILENT) {
        return;
    }
    /* Both composer kinds start sound; the "Own tone" incoming ring reaches here
     * directly from a clocked-down standby (calls_app.c ringtone value 19), so this
     * needs the same full-clock restore as core1_post_command -- not just unidle. */
    audio_start_prologue(true); /* every packed own-tone route uses the buzzer */
    if (level > AUDIO_LEVEL_MAX) {
        level = AUDIO_LEVEL_MAX;
    }
    if (len > CORE1_AUDIO_COMPOSER_PACKED_MAX) {
        len = CORE1_AUDIO_COMPOSER_PACKED_MAX;
    }
    critical_section_enter_blocking(&s_command_lock);
    memcpy(s_composer_packed, data, len);
    s_composer_packed_len = len;
    s_composer_packed_level = level;
    push_command_locked(cmd, audio_arg(0u, level));
    critical_section_exit(&s_command_lock);
    __sev(); /* R3: wake core1 if parked */
    store_service_defer_commits_until(time_ms() + STORE_COMMIT_AUDIO_GUARD_MS);
}

void core1_post_audio_composer_packed(const uint8_t *data, uint16_t len, uint8_t level) {
    post_composer_packed(CORE1_CMD_AUDIO_COMPOSER_PACKED, data, len, level);
}

void core1_post_audio_composer_packed_loop(const uint8_t *data, uint16_t len, uint8_t level) {
    post_composer_packed(CORE1_CMD_AUDIO_COMPOSER_PACKED_LOOP, data, len, level);
}

static void core1_main(void) {
    stack_monitor_core1_init();
    /* We coordinate flash writes with our own RAM-park handshake, not the SDK
     * multicore lockout, so do NOT install the FIFO-IRQ victim handler here (it
     * spins IRQ-off awaiting a resume token this code never sends). */
    LOGI("core1", "audio core up (RAM-park flash coordination)");
    vibra_hal_init();
    buzzer_hal_init();
    audio_service_init(s_modem_voice_transport_available);
    LOGI("core1", "audio service loop started");
    while (true) {
        s_core1_idle_waiting = false;
        s_core1_heartbeat_ms = time_ms();
        s_core1_loop_count++;
        if (s_flash_pause_req) {
            core1_flash_park();
        }
        core1_command_t command;
        while (pop_command(&command)) {
            /* A2+R2: never dispatch sound against parked plumbing. Resume via
             * the SAME sequence the flash-park uses (IRQs off, full rebuild
             * incl. the alignment-safe SM restart), then count it so core0's
             * gate orchestration knows the world moved. */
            if (s_audio_gated &&
                (cmd_starts_audio(command.cmd) || command.cmd == CORE1_CMD_AUDIO_UNGATE)) {
                uint32_t save = save_and_disable_interrupts();
                audio_i2s_hal_flash_resume();
                restore_interrupts(save);
                s_audio_gated = false;
                s_audio_resume_count++;
            }
            if (command.cmd == CORE1_CMD_AUDIO_GATE) {
                if (!s_audio_gated && !audio_service_is_active() && !audio_bridge_active()) {
                    uint32_t save = save_and_disable_interrupts();
                    bool stopped = audio_i2s_hal_flash_quiesce();
                    restore_interrupts(save);
                    s_audio_gated = stopped;
                }
                /* Ack (or refusal) is visible via s_audio_gated. */
            } else if (command.cmd == CORE1_CMD_AUDIO_UNGATE) {
                /* Resume was handled above. */
            } else if (command.cmd == CORE1_CMD_AUDIO_CLICK || command.cmd == CORE1_CMD_AUDIO_DTMF ||
                command.cmd == CORE1_CMD_AUDIO_STOP || command.cmd == CORE1_CMD_AUDIO_SYSTEM_TONE ||
                command.cmd == CORE1_CMD_AUDIO_SYSTEM_TONE_QUIET ||
                command.cmd == CORE1_CMD_AUDIO_SYSTEM_TONE_QUIET_LOOP ||
                command.cmd == CORE1_CMD_AUDIO_RINGTONE_PREVIEW ||
                command.cmd == CORE1_CMD_AUDIO_RINGTONE_LOOP ||
                command.cmd == CORE1_CMD_AUDIO_SYSTEM_TONE_LOOP ||
                command.cmd == CORE1_CMD_AUDIO_PACMAN_TONE ||
                command.cmd == CORE1_CMD_AUDIO_RINGTONE_MENU_PREVIEW ||
                command.cmd == CORE1_CMD_AUDIO_VIBRA_PULSE ||
                command.cmd == CORE1_CMD_AUDIO_VIBRA_PULSE_LOOP ||
                command.cmd == CORE1_CMD_AUDIO_COMPOSER_NOTE ||
                command.cmd == CORE1_CMD_AUDIO_SET_EARPIECE_GAIN ||
                command.cmd == CORE1_CMD_AUDIO_SET_CLICK_GAIN ||
                command.cmd == CORE1_CMD_AUDIO_SET_CLICK_H2 ||
                command.cmd == CORE1_CMD_AUDIO_SET_CLICK_H2_PHASE ||
                command.cmd == CORE1_CMD_AUDIO_SET_CLICK_H3 ||
                command.cmd == CORE1_CMD_AUDIO_SET_CLICK_H3_PHASE ||
                command.cmd == CORE1_CMD_AUDIO_SET_CLICK_H4 ||
                command.cmd == CORE1_CMD_AUDIO_SET_CLICK_H4_PHASE ||
                command.cmd == CORE1_CMD_AUDIO_SET_CLICK_H5 ||
                command.cmd == CORE1_CMD_AUDIO_SET_CLICK_H5_PHASE ||
                command.cmd == CORE1_CMD_AUDIO_SET_CLICK_ATTACK_MS ||
                command.cmd == CORE1_CMD_AUDIO_SET_CLICK_RELEASE_MS ||
                command.cmd == CORE1_CMD_AUDIO_SET_DTMF_GAIN ||
                command.cmd == CORE1_CMD_AUDIO_SET_DTMF_WEIGHTS ||
                command.cmd == CORE1_CMD_AUDIO_SET_DTMF_HEADSET_WEIGHTS ||
                command.cmd == CORE1_CMD_AUDIO_SET_BUZZER_DUTY ||
                command.cmd == CORE1_CMD_AUDIO_DEBUG_BUZZER_TEST ||
                command.cmd == CORE1_CMD_AUDIO_DEBUG_VIBRA_TEST ||
                command.cmd == CORE1_CMD_AUDIO_DEBUG_STOP ||
                command.cmd == CORE1_CMD_AUDIO_COMPOSER_PREVIEW_STOP ||
                command.cmd == CORE1_CMD_AUDIO_COMPOSER_STOP ||
                command.cmd == CORE1_CMD_AUDIO_TONES_SYSTEM_PREVIEW ||
                command.cmd == CORE1_CMD_AUDIO_TONES_CLICK_PREVIEW ||
                command.cmd == CORE1_CMD_AUDIO_TONES_PREVIEW_STOP) {
                audio_service_command(command.cmd, command.arg);
            } else if (command.cmd == CORE1_CMD_AUDIO_COMPOSER_PACKED ||
                       command.cmd == CORE1_CMD_AUDIO_COMPOSER_PACKED_LOOP) {
                /* Snapshot the shared composer buffer under the lock so a
                 * concurrent core0 re-post (memcpy into s_composer_packed)
                 * cannot tear the data while core1 reads it. */
                uint8_t packed[CORE1_AUDIO_COMPOSER_PACKED_MAX];
                uint16_t packed_len;
                uint8_t packed_level;
                critical_section_enter_blocking(&s_command_lock);
                packed_len = s_composer_packed_len;
                if (packed_len > CORE1_AUDIO_COMPOSER_PACKED_MAX) {
                    packed_len = CORE1_AUDIO_COMPOSER_PACKED_MAX;
                }
                packed_level = s_composer_packed_level;
                memcpy(packed, s_composer_packed, packed_len);
                critical_section_exit(&s_command_lock);
                if (command.cmd == CORE1_CMD_AUDIO_COMPOSER_PACKED_LOOP) {
                    audio_service_start_composer_packed_loop(packed, packed_len, packed_level);
                } else {
                    audio_service_start_composer_packed(packed, packed_len, packed_level);
                }
            } else if (command.cmd == CORE1_CMD_AUDIO_BRIDGE_START) {
                uint32_t latency = time_ms() - command.posted_ms;
                s_bridge_last_dispatch_ms = latency;
                if (latency > s_bridge_max_dispatch_ms) {
                    s_bridge_max_dispatch_ms = latency;
                }
                audio_service_bridge_start();
            } else if (command.cmd == CORE1_CMD_AUDIO_BRIDGE_STOP) {
                uint32_t latency = time_ms() - command.posted_ms;
                s_bridge_last_dispatch_ms = latency;
                if (latency > s_bridge_max_dispatch_ms) {
                    s_bridge_max_dispatch_ms = latency;
                }
                audio_service_bridge_stop();
            } else if (command.cmd == CORE1_CMD_SHUTDOWN) {
                finish_command_dispatch();
                core1_shutdown_park(); /* never returns */
            }
            finish_command_dispatch();
        }
        uint32_t now = time_ms();
        audio_service_tick(now);
        stack_monitor_core1_sample(now);
        /* R3: while audio is producing (tone/vibra/crescendo/bridge), keep the
         * 1 ms cadence its timing needs. While idle, park in WFE until a posted
         * command (core0 __sev()), a flash-pause request (__sev()), or -- when
         * the audio gate has NOT parked the I2S -- the ~2 ms codec DMA IRQ.
         * audio_service self-schedules no future audio (loops/two-stage alarms
         * are re-posted as commands), so nothing is missed by sleeping here.
         * SEV sets a sticky per-core event that WFE checks-and-clears, so a
         * post landing between the idle check and the WFE cannot be lost. */
        if (audio_service_is_active()) {
            sleep_ms(1);
        } else {
            s_core1_idle_waiting = true;
            __dmb();
            __wfe();
            s_core1_idle_waiting = false;
        }
    }
}

static bool pop_command(core1_command_t *out) {
    if (out == 0 || !s_command_lock_ready) {
        return false;
    }
    bool ok = false;
    critical_section_enter_blocking(&s_command_lock);
    if (s_command_count > 0u) {
        *out = s_command_queue[s_command_head];
        s_command_head = (uint8_t)((s_command_head + 1u) % CORE1_COMMAND_QUEUE_LEN);
        s_command_count--;
        s_command_dispatching = true;
        ok = true;
    }
    critical_section_exit(&s_command_lock);
    return ok;
}

static void finish_command_dispatch(void) {
    critical_section_enter_blocking(&s_command_lock);
    s_command_dispatching = false;
    critical_section_exit(&s_command_lock);
}

static void push_command_locked(core1_cmd_t cmd, uint16_t arg) {
    if (s_command_count >= CORE1_COMMAND_QUEUE_LEN) {
        s_command_head = (uint8_t)((s_command_head + 1u) % CORE1_COMMAND_QUEUE_LEN);
        s_command_count--;
        s_command_drops++;
    }
    s_command_queue[s_command_tail].cmd = cmd;
    s_command_queue[s_command_tail].arg = arg;
    s_command_queue[s_command_tail].posted_ms = time_ms();
    s_command_tail = (uint8_t)((s_command_tail + 1u) % CORE1_COMMAND_QUEUE_LEN);
    s_command_count++;
    if (s_command_count > s_command_high_water) {
        s_command_high_water = s_command_count;
    }
    if (cmd == CORE1_CMD_AUDIO_BRIDGE_START) {
        s_bridge_start_posts++;
    } else if (cmd == CORE1_CMD_AUDIO_BRIDGE_STOP) {
        s_bridge_stop_posts++;
    } else if (cmd == CORE1_CMD_AUDIO_GATE) {
        s_gate_requests++;
    }
}

void core1_services_get_diag(core1_services_diag_t *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->started = s_command_lock_ready;
    if (s_command_lock_ready) {
        critical_section_enter_blocking(&s_command_lock);
        out->command_queue_depth = s_command_count;
        out->command_queue_high_water = s_command_high_water;
        out->command_queue_drops = s_command_drops;
        out->command_dispatching = s_command_dispatching;
        critical_section_exit(&s_command_lock);
    }
    out->idle_waiting = s_core1_idle_waiting;
    out->flash_pause_requested = s_flash_pause_req;
    out->flash_parked = s_flash_parked;
    out->audio_gate_enabled = s_gate_enabled;
    out->audio_gated = s_audio_gated;
    out->heartbeat_ms = s_core1_heartbeat_ms;
    out->loop_count = s_core1_loop_count;
    out->flash_pause_attempts = s_flash_pause_attempts;
    out->flash_pause_successes = s_flash_pause_successes;
    out->flash_pause_timeouts = s_flash_pause_timeouts;
    out->flash_resume_timeouts = s_flash_resume_timeouts;
    out->flash_park_entries = s_flash_park_entries;
    out->flash_park_last_flags = s_flash_park_last_flags;
    out->audio_gate_requests = s_gate_requests;
    out->audio_gate_successes = s_gate_successes;
    out->audio_gate_races = s_gate_races;
    out->audio_resume_count = s_audio_resume_count;
    out->codec_recovery_attempts = s_codec_recovery_attempts;
    out->codec_recovery_successes = s_codec_recovery_successes;
    out->codec_recovery_failures = s_codec_recovery_failures;
    out->bridge_start_posts = s_bridge_start_posts;
    out->bridge_stop_posts = s_bridge_stop_posts;
    out->bridge_last_dispatch_ms = s_bridge_last_dispatch_ms;
    out->bridge_max_dispatch_ms = s_bridge_max_dispatch_ms;
}
