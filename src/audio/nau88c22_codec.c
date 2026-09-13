#include "audio/nau88c22_codec.h"

#include "audio/audio_bridge.h"
#include "audio/audio_levels.h"
#include "hardware/i2c.h"
#include "hal/accessory_hal.h"
#include "hal/board.h"
#include "services/log.h"
#include "pico/stdlib.h"

#define NAU_REG_RESET 0x00u
#define NAU_REG_POWER1 0x01u
#define NAU_REG_POWER2 0x02u
#define NAU_REG_POWER3 0x03u
#define NAU_REG_AUDIO_IF 0x04u
#define NAU_REG_CLOCK1 0x06u
#define NAU_REG_CLOCK2 0x07u
#define NAU_REG_DAC_CTRL 0x0au
#define NAU_REG_LDAC_VOL 0x0bu
#define NAU_REG_RDAC_VOL 0x0cu
#define NAU_REG_RSPK_SUBMIX 0x2bu
#define NAU_REG_INPUT_CTRL 0x2cu
#define NAU_REG_LPGA_GAIN 0x2du
#define NAU_REG_RPGA_GAIN 0x2eu
#define NAU_REG_LEFT_ADC_BOOST 0x2fu
#define NAU_REG_RIGHT_ADC_BOOST 0x30u
#define NAU_REG_OUTPUT_CTRL 0x31u
#define NAU_REG_LEFT_MIXER 0x32u
#define NAU_REG_RIGHT_MIXER 0x33u
#define NAU_REG_LHP_VOL 0x34u
#define NAU_REG_RHP_VOL 0x35u
#define NAU_REG_LSPK_VOL 0x36u
#define NAU_REG_RSPK_VOL 0x37u
#define NAU_REG_DEVICE_ID 0x3fu

#define NAU_POWER1_DCBUFEN (1u << 8)
#define NAU_POWER1_MICBIASEN (1u << 4)
#define NAU_POWER1_ABIASEN (1u << 3)
#define NAU_POWER1_IOBUFEN (1u << 2)
#define NAU_POWER1_REFIMP_80K 0x001u

#define NAU_POWER2_RHPEN (1u << 8)
#define NAU_POWER2_LHPEN (1u << 7) /* [BP] Appendix D labels bit7 "NHPEN"; treated as left-HP enable */
#define NAU_POWER2_RBSTEN (1u << 5)
#define NAU_POWER2_LBSTEN (1u << 4)
#define NAU_POWER2_RPGAEN (1u << 3)
#define NAU_POWER2_LPGAEN (1u << 2)
#define NAU_POWER2_RADCEN (1u << 1)
#define NAU_POWER2_LADCEN (1u << 0)

#define NAU_POWER3_LSPKEN (1u << 6)
#define NAU_POWER3_RSPKEN (1u << 5)
#define NAU_POWER3_RMIXEN (1u << 3)
#define NAU_POWER3_LMIXEN (1u << 2)
#define NAU_POWER3_RDACEN (1u << 1)
#define NAU_POWER3_LDACEN (1u << 0)

/* Every mic-chain enable across both channels (Mic-chain gate: gated off outside
 * calls -- the chain plus MICBIAS is ~3.8 mA of VDDA doing nothing at standby). */
#define NAU_POWER2_MIC_BITS                                                     \
    (NAU_POWER2_RBSTEN | NAU_POWER2_LBSTEN | NAU_POWER2_RPGAEN |                \
     NAU_POWER2_LPGAEN | NAU_POWER2_RADCEN | NAU_POWER2_LADCEN)

/* AIFMT=I2S, WLEN=16-bit, bit0 (MONO) CLEAR. Datasheet Rev1.6 erratum: the R4
 * MONO field text is value-swapped -- on real silicon 1 = MONO (ADC data framed
 * into the left phase only, right slot driven as zeros; DAC side unaffected),
 * 0 = stereo, matching the WM898x lineage. Proven on the bench 2026-07-10 via
 * ADCPHS crossover: 0x011 killed the right ADC slot (dead headset mic uplink),
 * 0x010 restores it. */
#define NAU_AUDIO_IF_I2S_16BIT_STEREO 0x010u
#define NAU_CLOCK1_MCLK_DIV_1_5_SLAVE 0x020u
#define NAU_CLOCK2_SAMPLE_16KHZ 0x006u

#define NAU_OUTPUT_CTRL_LOW_VOLTAGE 0x002u
#define NAU_RSPK_SUBMIX_BTL_INVERT_RMIX 0x011u
#define NAU_RSPK_SUBMIX_NO_INVERT 0x001u
#define NAU_INPUT_CTRL_HANDSET_LEFT_MIC 0x003u
#define NAU_INPUT_CTRL_HEADSET_RIGHT_MIC 0x030u
#define NAU_PGA_GAIN_DEFAULT 0x010u
#define NAU_ADC_BOOST_DEFAULT 0x100u
#define NAU_LEFT_MIXER_LDAC 0x001u
#define NAU_RIGHT_MIXER_RDAC 0x001u
#define NAU_DAC_VOL_0DB 0x0ffu
#define NAU_ANALOG_GAIN_0DB 0x39u
/* Bench A/B against a stock 3210 selected 0 dB as the clean global earpiece
 * baseline. Per-sound digital gains own loudness calibration; in-call volume
 * still deliberately drives this stage through its independent 1..10 curve. */
#define NAU_SPK_GAIN_DEFAULT NAU_ANALOG_GAIN_0DB
/* Keep both output stages neutral at idle. HDC-5's -30 dB local-tone
 * calibration belongs in the local PCM path, before it is mixed with Telit
 * downlink; otherwise the same analog register cannot represent both quiet
 * local tones and a usable voice-call level. */
#define NAU_HP_GAIN_DEFAULT NAU_ANALOG_GAIN_0DB
#define NAU_HP_CALL_GAIN_MAX NAU_HP_GAIN_DEFAULT
#define NAU_ANALOG_MUTE (1u << 6)
#define NAU_ANALOG_UPDATE (1u << 8)
#define NAU_DEVICE_ID_EXPECTED 0x01au

_Static_assert(NAU_SPK_GAIN_DEFAULT >= AUDIO_CALL_HANDSET_MAX_ATTENUATION_DB,
               "handset call ladder must fit the codec gain field");
_Static_assert(NAU_HP_CALL_GAIN_MAX >= AUDIO_CALL_HEADSET_MAX_ATTENUATION_DB,
               "headset call ladder must fit the codec gain field");

#define NAU_I2C_REG_MASK 0x7fu
#define NAU_I2C_VALUE_MASK 0x1ffu

static bool write_reg_checked(uint8_t reg, uint16_t value);
static bool set_speaker_volume(uint8_t gain, bool muted);
static bool set_headphone_volume(uint8_t gain, bool muted);
static void note_i2c_result(bool ok);

/* After this many consecutive timed-out/failed I2C transfers the shared bus is
 * treated as wedged: further codec transfers short-circuit (return false fast)
 * instead of each burning the full per-transfer timeout. 8 consecutive failures
 * means the bus is genuinely dead (the TCA8418/RV-8803 latches will have tripped
 * too); the latch is cleared by nau88c22_codec_init() (recovery is re-init + the
 * shared-bus recovery path). Mirrors tca8418_hal's latch. */
#define NAU_I2C_FAIL_LIMIT 8u

static bool s_ready;
static bool s_bus_failed;
static uint8_t s_i2c_fail_count;
/* Runtime-wedge recovery (mirrors tca8418_hal/rv8803_hal self-heal): armed ONLY
 * when the fail-latch trips at runtime (not a boot/standby !s_ready), so the
 * core1_services recovery tick re-inits when the shared bus comes back and never
 * fights an intentional power-off standby. Cleared on a successful init. The
 * retry orchestration (rate-limit, clock-down exit, MCLK, idle-gate reconcile)
 * lives in core1_services, which owns those; this driver only exposes the flag
 * via nau88c22_codec_recover_pending(). */
static bool s_recover_pending;
static uint8_t s_speaker_gain = NAU_SPK_GAIN_DEFAULT;
static bool s_speaker_muted;
static uint8_t s_headphone_gain = NAU_HP_GAIN_DEFAULT;
/* HP starts muted (init + every non-headset route mute it); set_route(HEADSET)
 * clears this. Tracked so gain changes on the live headset route apply unmuted. */
static bool s_headphone_muted = true;
/* Mic-chain gate mic-power state: POWER1/POWER2 are derived from (route, in-call,
 * bias-hold) so set_route and set_mic_power can each rewrite them without
 * clobbering the other's bits (the driver performs no I2C reads). */
static nau_route_t s_route = NAU_ROUTE_HANDSET;
static bool s_mic_in_call;
static bool s_mic_bias_hold; /* headset inserted: the HDC-5 hook sense hangs off MICBIAS */
static bool s_playback_idle; /* Playback idle: DACs/mixers down behind the mutes */
static bool s_playback_idle_synced = true; /* s_playback_idle actually matches HW;
                                            * cleared when a transition write fails
                                            * so the caller's retry re-issues it */

static uint16_t power1_value(void) {
    uint16_t v = NAU_POWER1_DCBUFEN | NAU_POWER1_ABIASEN | NAU_POWER1_IOBUFEN |
                 NAU_POWER1_REFIMP_80K;
    if (s_mic_in_call || s_mic_bias_hold) {
        v |= NAU_POWER1_MICBIASEN;
    }
    return v;
}

/* Full POWER2 for a route, mic chain included (the caller masks
 * NAU_POWER2_MIC_BITS out when idle). Non-headset routes share the BTL
 * speaker path with the internal left mic; no HP drivers. */
static uint16_t power2_for_route(nau_route_t route) {
    if (route == NAU_ROUTE_HEADSET) {
        return NAU_POWER2_RHPEN | NAU_POWER2_LHPEN | NAU_POWER2_RBSTEN |
               NAU_POWER2_RPGAEN | NAU_POWER2_RADCEN;
    }
    return NAU_POWER2_LBSTEN | NAU_POWER2_LPGAEN | NAU_POWER2_LADCEN;
}

/* Full POWER3 for a route. Playback idle masks the DAC/mixer bits out during
 * playback idle; the speaker/HP DRIVER enables always stay -- re-enabling a
 * driver re-triggers the chip's 250 ms depop sequence, which must never land
 * on the keypad-click path. */
#define NAU_POWER3_PLAYBACK_BITS \
    (NAU_POWER3_RDACEN | NAU_POWER3_LDACEN | NAU_POWER3_RMIXEN | NAU_POWER3_LMIXEN)

static uint16_t power3_for_route(nau_route_t route) {
    if (route == NAU_ROUTE_HEADSET) {
        return NAU_POWER3_RMIXEN | NAU_POWER3_LMIXEN | NAU_POWER3_RDACEN | NAU_POWER3_LDACEN;
    }
    return NAU_POWER3_LSPKEN | NAU_POWER3_RSPKEN | NAU_POWER3_RMIXEN |
           NAU_POWER3_LMIXEN | NAU_POWER3_RDACEN | NAU_POWER3_LDACEN;
}

bool nau88c22_codec_init(void) {
    if (s_ready) {
        return true;
    }
    s_bus_failed = false;
    s_i2c_fail_count = 0u;
    /* Arm the self-heal up front so ANY failure return below leaves recovery
     * pending. A single boot-time device-ID NAK (a transient shared-bus glitch or
     * a codec not yet settled at that instant) otherwise left s_ready=false with
     * s_recover_pending never set -- it only latched after NAU_I2C_FAIL_LIMIT
     * RUNTIME failures -- so all codec audio stayed dead until a power cycle. A
     * successful init clears this below; power_standby() drops s_ready WITHOUT
     * calling init (so never arms this), and the recover tick is gated off while
     * powered down, so this cannot fight an intentional standby. */
    s_recover_pending = true;
    /* Mic-chain gate defaults: mic chain down until a call brings it up; MICBIAS
     * held only for an inserted headset (hook sense). At the earliest cold
     * boot accessory_hal is not up yet and reports empty -- benign, the
     * insert-change event re-applies via set_mic_power. */
    s_mic_in_call = false;
    s_mic_bias_hold = accessory_hal_headset_inserted();
    s_route = NAU_ROUTE_HANDSET;
    s_playback_idle = false; /* init brings the playback path fully up */

    uint16_t device_id = 0u;
    if (!nau88c22_codec_read_reg(NAU_REG_DEVICE_ID, &device_id)) {
        LOGW("codec", "NAU88C22 not responding at 0x%02x", NAU88C22_I2C_ADDR);
        return false;
    }
    if (device_id != NAU_DEVICE_ID_EXPECTED) {
        LOGW("codec", "unexpected NAU88C22 id 0x%03x", device_id);
    }

    if (!nau88c22_codec_write_reg(NAU_REG_RESET, 0u)) {
        LOGW("codec", "software reset failed");
        return false;
    }
    sleep_ms(1);

    /* Datasheet Rev1.6 section 11.2: configure low-voltage output mode before
     * depop/reference bring-up, keep outputs muted while the reference settles. */
    if (!write_reg_checked(NAU_REG_OUTPUT_CTRL, NAU_OUTPUT_CTRL_LOW_VOLTAGE) ||
        !set_headphone_volume(NAU_HP_GAIN_DEFAULT, true) ||
        !set_speaker_volume(NAU_SPK_GAIN_DEFAULT, true) ||
        !write_reg_checked(NAU_REG_POWER1, NAU_POWER1_DCBUFEN | NAU_POWER1_IOBUFEN) ||
        !write_reg_checked(NAU_REG_POWER1,
                           NAU_POWER1_DCBUFEN |
                               NAU_POWER1_ABIASEN |
                               NAU_POWER1_IOBUFEN |
                               NAU_POWER1_REFIMP_80K)) {
        LOGW("codec", "depop precharge failed");
        return false;
    }
    sleep_ms(250);

    /* 6.144 MHz MCLK from RP2354, divided by 1.5 inside the codec to produce
     * 4.096 MHz IMCLK = 256 * 16 kHz. Slave I2S, normal 16-bit stereo slots. */
    if (!write_reg_checked(NAU_REG_CLOCK1, NAU_CLOCK1_MCLK_DIV_1_5_SLAVE) ||
        !write_reg_checked(NAU_REG_CLOCK2, NAU_CLOCK2_SAMPLE_16KHZ) ||
        !write_reg_checked(NAU_REG_AUDIO_IF, NAU_AUDIO_IF_I2S_16BIT_STEREO) ||
        !write_reg_checked(NAU_REG_DAC_CTRL, 0u) ||
        !write_reg_checked(NAU_REG_LDAC_VOL, NAU_DAC_VOL_0DB) ||
        !write_reg_checked(NAU_REG_RDAC_VOL, NAU_ANALOG_UPDATE | NAU_DAC_VOL_0DB)) {
        LOGW("codec", "digital audio config failed");
        return false;
    }

    /* Default handset path:
     * - TX samples are duplicated mono by audio_service.
     * - RSPK submixer inverts RMIX once for BTL receiver drive.
     * - Internal handset mic uses left differential input pair.
     * Headset route is intentionally left for the accessory-detect slice. */
    if (!write_reg_checked(NAU_REG_RSPK_SUBMIX, NAU_RSPK_SUBMIX_BTL_INVERT_RMIX) ||
        !write_reg_checked(NAU_REG_LEFT_MIXER, NAU_LEFT_MIXER_LDAC) ||
        !write_reg_checked(NAU_REG_RIGHT_MIXER, NAU_RIGHT_MIXER_RDAC) ||
        !write_reg_checked(NAU_REG_INPUT_CTRL, NAU_INPUT_CTRL_HANDSET_LEFT_MIC) ||
        !write_reg_checked(NAU_REG_LPGA_GAIN, 0x010u) ||
        !write_reg_checked(NAU_REG_LEFT_ADC_BOOST, 0x100u) ||
        /* Mic-chain gate: the mic chain (MICBIASEN + LBST/LPGA/LADC, ~3.8 mA) is
         * NOT powered at init -- it comes up per-call via set_mic_power(),
         * with MICBIAS alone held while a headset is inserted (hook sense). */
        !write_reg_checked(NAU_REG_POWER1, power1_value()) ||
        !write_reg_checked(NAU_REG_POWER2, 0u) ||
        !write_reg_checked(NAU_REG_POWER3,
                           NAU_POWER3_LSPKEN |
                               NAU_POWER3_RSPKEN |
                               NAU_POWER3_RMIXEN |
                               NAU_POWER3_LMIXEN |
                               NAU_POWER3_RDACEN |
                               NAU_POWER3_LDACEN)) {
        LOGW("codec", "handset route config failed");
        return false;
    }

    /* Output-driver depop delay: keep I2S stopped and outputs muted until the
     * speaker drivers have had time to settle with MCLK present. */
    sleep_ms(250);
    if (!set_speaker_volume(NAU_SPK_GAIN_DEFAULT, false)) {
        LOGW("codec", "speaker unmute failed");
        return false;
    }

    s_speaker_gain = NAU_SPK_GAIN_DEFAULT;
    s_speaker_muted = false;
    s_headphone_gain = NAU_HP_GAIN_DEFAULT;
    s_headphone_muted = true; /* init leaves HP muted; set_route(HEADSET) unmutes */
    s_ready = true;
    s_recover_pending = false; /* a good init ends any pending wedge recovery */
    s_playback_idle_synced = true; /* init brought playback fully up; cache matches HW */
    LOGI("codec", "NAU88C22 ready id=0x%03x", device_id);
    return true;
}

bool nau88c22_codec_ready(void) {
    return s_ready;
}

void nau88c22_codec_get_diag(nau88c22_codec_diag_t *out) {
    if (out == NULL) {
        return;
    }
    out->ready = s_ready;
    out->recover_pending = s_recover_pending;
    out->route = s_route;
    out->mic_in_call = s_mic_in_call;
    out->mic_bias_hold = s_mic_bias_hold;
    out->playback_idle = s_playback_idle;
    out->speaker_gain = s_speaker_gain;
    out->speaker_muted = s_speaker_muted;
    out->headphone_gain = s_headphone_gain;
    out->headphone_muted = s_headphone_muted;
}

/* All codec control I2C shares the keypad/RTC bus and therefore MUST run on
 * core 0 only (no cross-core locking exists). Transfers are time-bounded so a
 * wedged bus cannot hang the caller, and a consecutive-failure latch (mirroring
 * tca8418_hal) short-circuits further transfers once the bus is dead so runtime
 * route/gain changes do not each burn the full timeout. Runtime gain/mute/
 * source-switch keeps to core 0 and passes only the sample-format flags to
 * core 1. */
static void note_i2c_result(bool ok) {
    if (ok) {
        s_i2c_fail_count = 0u;
        return;
    }
    if (s_i2c_fail_count < 0xffu) {
        s_i2c_fail_count++;
    }
    if (s_i2c_fail_count >= NAU_I2C_FAIL_LIMIT && !s_bus_failed) {
        s_bus_failed = true;
        s_ready = false; /* nau88c22_codec_ready() now reports the outage */
        s_recover_pending = true; /* arm the runtime re-init retry */
        LOGW("codec", "NAU88C22 I2C unresponsive; disabling until re-init");
    }
}

bool nau88c22_codec_recover_pending(void) {
    return s_recover_pending;
}

void nau88c22_codec_set_recover_pending(bool pending) {
    s_recover_pending = pending;
}

bool nau88c22_codec_read_reg(uint8_t reg, uint16_t *out_value) {
    if (out_value == 0 || reg > NAU_I2C_REG_MASK) {
        return false;
    }
    if (s_bus_failed) {
        return false;
    }

    uint8_t address = (uint8_t)(reg << 1);
    int written = i2c_write_timeout_us(BOARD_I2C_PORT, NAU88C22_I2C_ADDR, &address, 1u, true,
                                       BOARD_I2C_TIMEOUT_US);
    if (written != 1) {
        note_i2c_result(false);
        return false;
    }

    uint8_t data[2] = {0u, 0u};
    int read = i2c_read_timeout_us(BOARD_I2C_PORT, NAU88C22_I2C_ADDR, data, sizeof(data), false,
                                   BOARD_I2C_TIMEOUT_US);
    if (read != (int)sizeof(data)) {
        note_i2c_result(false);
        return false;
    }

    note_i2c_result(true);
    *out_value = (uint16_t)(((uint16_t)(data[0] & 0x01u) << 8) | data[1]);
    return true;
}

bool nau88c22_codec_write_reg(uint8_t reg, uint16_t value) {
    if (reg > NAU_I2C_REG_MASK || value > NAU_I2C_VALUE_MASK) {
        return false;
    }
    if (s_bus_failed) {
        return false;
    }

    uint8_t data[2] = {
        (uint8_t)((reg << 1) | ((value >> 8) & 0x01u)),
        (uint8_t)(value & 0xffu),
    };
    int written = i2c_write_timeout_us(BOARD_I2C_PORT, NAU88C22_I2C_ADDR, data, sizeof(data), false,
                                       BOARD_I2C_TIMEOUT_US);
    bool ok = (written == (int)sizeof(data));
    note_i2c_result(ok);
    return ok;
}

bool nau88c22_codec_set_speaker_gain(uint8_t gain) {
    if (!s_ready || gain > 0x3fu) {
        return false;
    }
    if (!set_speaker_volume(gain, s_speaker_muted)) {
        return false;
    }
    s_speaker_gain = gain;
    return true;
}

bool nau88c22_codec_set_headphone_gain(uint8_t gain) {
    if (!s_ready || gain > 0x3fu) {
        return false;
    }
    if (!set_headphone_volume(gain, s_headphone_muted)) {
        return false;
    }
    s_headphone_gain = gain;
    return true;
}

bool nau88c22_codec_set_call_volume(uint8_t level) {
    uint8_t speaker_gain =
        (uint8_t)(NAU_SPK_GAIN_DEFAULT - audio_call_handset_attenuation_db(level));
    uint8_t headphone_gain =
        (uint8_t)(NAU_HP_CALL_GAIN_MAX - audio_call_headset_attenuation_db(level));
    bool ok = nau88c22_codec_set_speaker_gain(speaker_gain);
    ok = nau88c22_codec_set_headphone_gain(headphone_gain) && ok;
    return ok;
}

bool nau88c22_codec_reset_output_gains(void) {
    bool ok = nau88c22_codec_set_speaker_gain(NAU_SPK_GAIN_DEFAULT);
    ok = nau88c22_codec_set_headphone_gain(NAU_HP_GAIN_DEFAULT) && ok;
    return ok;
}

bool nau88c22_codec_power_standby(void) {
    if (!s_ready) {
        return false;
    }
    /* Phone-off analog shutdown: mute first (pop), then drop every analog
     * supply -- output drivers, mixers, DAC/ADC, PGA/boost, mic bias, VREF.
     * The register/I2C block stays alive on +3V3 and MCLK may keep toggling; a
     * powered-down codec ignores both at uA scale. Resume is a fresh
     * nau88c22_codec_init() (full reset + depop ramp, ~500 ms) -- power-on and
     * the alarm-while-off wake both go through it. */
    set_speaker_volume(s_speaker_gain, true);
    set_headphone_volume(s_headphone_gain, true);
    s_speaker_muted = true;
    s_headphone_muted = true;
    bool ok = write_reg_checked(NAU_REG_POWER3, 0u);
    ok = write_reg_checked(NAU_REG_POWER2, 0u) && ok;
    ok = write_reg_checked(NAU_REG_POWER1, 0u) && ok;
    /* Drop s_ready LAST (the writes above need it): nau88c22_codec_init()
     * short-circuits while s_ready is set, so leaving it true would make the
     * resume re-init a silent no-op -> codec dead until an RP reboot. Clearing
     * it also turns the deferred call-teardown codec writes (set_route /
     * reset call-volume request from modem_bridge_follow_call_state, up to ~8 s
     * later when powering off mid-call) into harmless early-returns. */
    s_ready = false;
    return ok;
}

bool nau88c22_codec_set_mic_power(bool in_call, bool headset_inserted) {
    s_mic_in_call = in_call;
    s_mic_bias_hold = headset_inserted;
    if (!s_ready) {
        return false; /* cached: the next init/set_route derives from it */
    }
    uint16_t power2 = power2_for_route(s_route);
    if (!in_call) {
        power2 &= (uint16_t)~NAU_POWER2_MIC_BITS;
    }
    /* Bias before chain on the way up; chain before bias on the way down. */
    bool ok;
    if (in_call) {
        ok = write_reg_checked(NAU_REG_POWER1, power1_value()) &&
             write_reg_checked(NAU_REG_POWER2, power2);
    } else {
        ok = write_reg_checked(NAU_REG_POWER2, power2) &&
             write_reg_checked(NAU_REG_POWER1, power1_value());
    }
    if (!ok) {
        LOGW("codec", "mic power %u/%u config failed", (unsigned)in_call,
             (unsigned)headset_inserted);
    }
    return ok;
}

bool nau88c22_codec_set_route(nau_route_t route) {
    if (!s_ready) {
        return false;
    }

    uint16_t power2;
    uint16_t power3;
    uint16_t submix;
    uint16_t input_ctrl;
    uint8_t pga_reg;
    uint8_t boost_reg;
    uint8_t mic_channel;
    bool right_invert;
    bool speaker_output;

    switch (route) {
    case NAU_ROUTE_HEADSET:
        /* Differential headphone out (no speaker), right differential mic in.
         * RP inverts the right slot for the differential drive (single inversion),
         * so the codec keeps DACPL/RDACPL non-inverted. Headset specifics are
         * [BP] pending accessory-detect + bench bring-up. */
        power2 = power2_for_route(NAU_ROUTE_HEADSET);
        power3 = power3_for_route(NAU_ROUTE_HEADSET);
        submix = NAU_RSPK_SUBMIX_NO_INVERT;
        input_ctrl = NAU_INPUT_CTRL_HEADSET_RIGHT_MIC;
        pga_reg = NAU_REG_RPGA_GAIN;
        boost_reg = NAU_REG_RIGHT_ADC_BOOST;
        mic_channel = 1u; /* right ADC */
        right_invert = true;
        speaker_output = false;
        break;
    case NAU_ROUTE_LOUDSPEAKER:
    case NAU_ROUTE_HANDSET:
    default:
        /* BTL receiver/loudspeaker out, internal left differential mic in. The
         * single BTL inversion is the RSPK submixer (RSUBBYP); RP feeds both
         * slots non-inverted. Loudspeaker shares the BTL pair (gain profile
         * differences are [BP]). */
        power2 = power2_for_route(NAU_ROUTE_HANDSET);
        power3 = power3_for_route(NAU_ROUTE_HANDSET);
        submix = NAU_RSPK_SUBMIX_BTL_INVERT_RMIX;
        input_ctrl = NAU_INPUT_CTRL_HANDSET_LEFT_MIC;
        pga_reg = NAU_REG_LPGA_GAIN;
        boost_reg = NAU_REG_LEFT_ADC_BOOST;
        mic_channel = 0u; /* left ADC */
        right_invert = false;
        speaker_output = true;
        break;
    }

    /* Mute both outputs across the power/routing change to avoid pops. No
     * sidetone/ADC-bypass path is enabled here (R5 ADDAP and the output-mixer
     * bypass bits stay 0), so downlink audio cannot leak into the uplink.
     * Keep BOTH tracked mute flags in lockstep with the physical writes here:
     * the s_playback_idle early-return below skips the per-route unmute block,
     * and a stale "unmuted" speaker flag would let a later set_speaker_gain()
     * (call-volume apply) silently unmute the earpiece on the headset route
     * (latent; kept impossible by construction). */
    set_speaker_volume(s_speaker_gain, true);
    set_headphone_volume(s_headphone_gain, true);
    s_speaker_muted = true;
    s_headphone_muted = true;

    s_route = route;
    if (!s_mic_in_call) {
        /* Mic-chain gate: the route defines WHICH mic chain would run; whether it
         * runs at all is the in-call state's call. */
        power2 &= (uint16_t)~NAU_POWER2_MIC_BITS;
    }
    if (s_playback_idle) {
        /* Playback idle: route changed while the playback path sleeps (rare;
         * e.g. a debug reroute) -- keep the DACs down and the outputs muted;
         * the next set_playback_idle(false) restores both for the new route. */
        power3 &= (uint16_t)~NAU_POWER3_PLAYBACK_BITS;
    }

    if (!write_reg_checked(NAU_REG_INPUT_CTRL, input_ctrl) ||
        !write_reg_checked(pga_reg, NAU_PGA_GAIN_DEFAULT) ||
        !write_reg_checked(boost_reg, NAU_ADC_BOOST_DEFAULT) ||
        !write_reg_checked(NAU_REG_RSPK_SUBMIX, submix) ||
        !write_reg_checked(NAU_REG_POWER2, power2) ||
        !write_reg_checked(NAU_REG_POWER3, power3)) {
        LOGW("codec", "route %d config failed", (int)route);
        return false;
    }

    /* Publish the sample format to core1 before unmuting so the mixer formats
     * the new route correctly. */
    audio_bridge_set_format((uint8_t)route, mic_channel, right_invert);

    if (s_playback_idle) {
        return true; /* A2: stay muted; set_playback_idle(false) unmutes */
    }
    bool ok;
    if (speaker_output) {
        ok = set_speaker_volume(s_speaker_gain, false);
        s_speaker_muted = false;
    } else {
        ok = set_headphone_volume(s_headphone_gain, false);
        s_headphone_muted = false;
        s_speaker_muted = true; /* speaker stays muted on the headset route */
    }
    if (!ok) {
        LOGW("codec", "route %d unmute failed", (int)route);
        return false;
    }
    return true;
}

bool nau88c22_codec_set_playback_idle(bool idle) {
    /* Only short-circuit when the cache is known to MATCH hardware. A prior
     * transition that failed mid-write (a transient I2C glitch) leaves the cache
     * ahead of hardware with s_playback_idle_synced=false; the caller
     * (audio_gate_unidle) deliberately retries, and that retry must re-issue the
     * writes rather than return a false success here -- otherwise the codec is
     * stranded DAC-down + muted (silent audio) until a full re-init. */
    if (s_playback_idle == idle && s_playback_idle_synced) {
        return true;
    }
    s_playback_idle = idle;
    if (!s_ready) {
        s_playback_idle_synced = false; /* HW cannot be synced while down; force a retry */
        return false; /* cached; the next init starts live (it resets the flag) */
    }
    uint16_t power3 = power3_for_route(s_route);
    bool ok;
    if (idle) {
        /* Analog-mute the live output FIRST, then drop DACs+mixers behind it.
         * Driver enables stay set (the 250 ms depop rule). [BP] bench: confirm
         * the DAC power toggle is inaudible behind the mute. */
        if (s_route == NAU_ROUTE_HEADSET) {
            ok = set_headphone_volume(s_headphone_gain, true);
            s_headphone_muted = true;
        } else {
            ok = set_speaker_volume(s_speaker_gain, true);
            s_speaker_muted = true;
        }
        power3 &= (uint16_t)~NAU_POWER3_PLAYBACK_BITS;
        ok = write_reg_checked(NAU_REG_POWER3, power3) && ok;
    } else {
        /* DACs+mixers up first, then unmute into them. */
        ok = write_reg_checked(NAU_REG_POWER3, power3);
        if (s_route == NAU_ROUTE_HEADSET) {
            ok = set_headphone_volume(s_headphone_gain, false) && ok;
            s_headphone_muted = false;
        } else {
            ok = set_speaker_volume(s_speaker_gain, false) && ok;
            s_speaker_muted = false;
        }
    }
    if (!ok) {
        LOGW("codec", "playback idle %u config failed", (unsigned)idle);
    }
    s_playback_idle_synced = ok; /* cache matches HW only if the writes actually landed */
    return ok;
}

static bool write_reg_checked(uint8_t reg, uint16_t value) {
    if (!nau88c22_codec_write_reg(reg, value)) {
        return false;
    }

    /* R0 is write-only reset, and volume update bits are write-only. Avoid a
     * misleading compare on those cases; plain I2C ACK is the only check. */
    if (reg == NAU_REG_RESET ||
        reg == NAU_REG_LDAC_VOL ||
        reg == NAU_REG_RDAC_VOL ||
        reg == NAU_REG_LHP_VOL ||
        reg == NAU_REG_RHP_VOL ||
        reg == NAU_REG_LSPK_VOL ||
        reg == NAU_REG_RSPK_VOL) {
        return true;
    }

    uint16_t readback = 0u;
    if (!nau88c22_codec_read_reg(reg, &readback)) {
        return false;
    }
    return readback == (value & NAU_I2C_VALUE_MASK);
}

static bool set_speaker_volume(uint8_t gain, bool muted) {
    if (gain > 0x3fu) {
        return false;
    }
    uint16_t value = gain;
    if (muted) {
        value |= NAU_ANALOG_MUTE;
    }
    return nau88c22_codec_write_reg(NAU_REG_LSPK_VOL, value) &&
           nau88c22_codec_write_reg(NAU_REG_RSPK_VOL, value | NAU_ANALOG_UPDATE);
}

static bool set_headphone_volume(uint8_t gain, bool muted) {
    if (gain > 0x3fu) {
        return false;
    }
    uint16_t value = gain;
    if (muted) {
        value |= NAU_ANALOG_MUTE;
    }
    return nau88c22_codec_write_reg(NAU_REG_LHP_VOL, value) &&
           nau88c22_codec_write_reg(NAU_REG_RHP_VOL, value | NAU_ANALOG_UPDATE);
}
