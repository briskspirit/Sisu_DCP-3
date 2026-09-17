#include "services/debug_console.h"

#include "app_internal.h"
#include "audio/audio_levels.h"
#include "audio/audio_bridge.h"
#include "audio/audio_i2s_hal.h"
#include "audio/nau88c22_codec.h"
#include "hal/accessory_hal.h"
#include "hal/board.h"
#include "hal/lcd_pcd8544.h"
#include "hal/ltc2959_hal.h"
#include "hal/modem_uart_hal.h"
#include "hal/rtc_alarm_hal.h"
#include "hal/tca8418_hal.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include "services/core1_services.h"
#include "services/battery_learning_service.h"
#include "services/battery_charge_supervisor_service.h"
#include "services/input_keys.h"
#include "services/lcd_calibration.h"
#include "audio/modem_i2s_hal.h"
#include "services/modem_service.h"
#include "services/netmon_diag_service.h"
#include "services/power_sleep.h"
#include "services/runtime_watchdog.h"
#include "services/shared_irq_service.h"
#include "services/shared_3v8_service.h"
#include "services/standby_sleep.h"
#include "services/stack_monitor.h"
#include "hardware/structs/pads_bank0.h"
#include "hardware/structs/powman.h"
#include "hardware/structs/sysinfo.h"
#include "services/sms_picture_codec.h"
#include "services/board_diag_service.h"
#include "storage/store_service.h"
#include "services/timebase.h"
#include "services/usb_service.h"
#include "pico/stdio.h"
#include "pico/stdlib.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEBUG_LINE_MAX 128u
#define DEBUG_PICTURE_PORT 0x158au
#define DEBUG_LTC_WINDOW_MAX_DELAY_S 60u
#define DEBUG_LTC_WINDOW_MAX_DURATION_S 3600u

static char s_line[DEBUG_LINE_MAX];
static uint8_t s_line_len;
static bool s_waiting_sms_result;
static uint32_t s_waiting_sms_request_id;
static bool s_waiting_at_result;
static bool s_sleep_gating = true; /* R3b: default on (bench-validated); mirrors board_init */
static uint8_t s_buzzcal_duty_percent = UINT8_MAX;
static bool s_charge_trace_live;
static uint32_t s_charge_trace_seen;

typedef struct {
    uint16_t click_gain_pct;
    int16_t click_h2_pct;
    int16_t click_h2_phase_deg;
    int16_t click_h3_pct;
    int16_t click_h3_phase_deg;
    int16_t click_h4_pct;
    int16_t click_h4_phase_deg;
    int16_t click_h5_pct;
    int16_t click_h5_phase_deg;
    uint16_t click_attack_ms;
    uint16_t click_release_ms;
    uint16_t dtmf_gain_pct;
    uint8_t dtmf_handset_low_weight;
    uint8_t dtmf_handset_high_weight;
    uint8_t dtmf_headset_low_weight;
    uint8_t dtmf_headset_high_weight;
    bool stop_pending;
    uint16_t stop_arg;
    uint32_t stop_at_ms;
} debug_keycal_t;

static debug_keycal_t s_keycal;

typedef struct {
    bool armed;
    bool running;
    bool complete;
    bool valid;
    uint32_t start_at_ms;
    uint32_t duration_ms;
    uint32_t started_ms;
    uint32_t elapsed_ms;
    int64_t delta_nah;
    int32_t average_ua;
    uint32_t gauge_session;
    uint32_t measurement_generation;
    uint32_t modem_ready_start_ms;
    uint32_t modem_requested_start_ms;
    uint32_t modem_confirmed_start_ms;
    uint32_t modem_sleep_entries_start;
    uint32_t modem_wake_attempts_start;
    uint32_t modem_ready_ms;
    uint32_t modem_requested_ms;
    uint32_t modem_confirmed_ms;
    uint32_t modem_sleep_entries;
    uint32_t modem_wake_attempts;
} debug_ltc_window_t;

static debug_ltc_window_t s_ltc_window;
/* The console is polled only by core 0. Reuse these large snapshots across
 * commands/windows instead of nesting them on the 4 KiB production stack. */
static modem_diag_snapshot_t s_modem_diag_scratch;
static netmon_local_diag_snapshot_t s_local_diag_scratch;

static void handle_line(app_t *app, char *line);
static void print_help(void);
static char *skip_spaces(char *text);
static char *next_token(char **cursor);
static void command_status(void);
static void command_hw(const app_t *app);
static void command_wake(void);
static void command_clocks(void);
static void command_pads(void);
static void command_lcd(char *args);
static void command_ltc_window(char *args);
static void command_battery_learning(char *args);
static void command_charge(char *args);
static void command_modem_rx(char *args) __attribute__((noinline));
static void command_dormant(char *args);
static void command_keycal(char *args);
static void command_buzzcal(char *args);
static void poll_ltc_window(uint32_t now_ms);
static void poll_charge_trace(void);
static void poll_keycal(uint32_t now_ms);
static void hw_change_log(const app_t *app);
static void command_ui(app_t *app, char *args);
static void command_text(char *args);
static void command_pdu7(char *args);
static void command_binary(char *args);
static void command_binary_f5(char *args);
static void command_port(char *args);
static void command_picture(char *args);
static bool build_debug_picture_payload(uint8_t *payload, size_t cap, uint16_t *out_len, uint8_t *out_chunks);
static void fill_fallback_picture(store_picture_message_t *picture);

/* The phone power-on entry lives in the app layer; the console (a service)
 * must not call upward. main.c registers it at startup. */
static bool (*s_phone_power_on)(app_t *app, uint32_t now_ms);

void debug_console_set_phone_power_on(bool (*fn)(app_t *app, uint32_t now_ms)) {
    s_phone_power_on = fn;
}

void debug_console_init(void) {
    s_line_len = 0u;
    memset(&s_ltc_window, 0, sizeof(s_ltc_window));
    memset(&s_keycal, 0, sizeof(s_keycal));
    s_keycal.click_gain_pct = AUDIO_KEYPAD_CLICK_GAIN_PERCENT_DEFAULT;
    s_keycal.click_h2_pct = AUDIO_KEYPAD_CLICK_H2_PERCENT_DEFAULT;
    s_keycal.click_h2_phase_deg = AUDIO_KEYPAD_CLICK_H2_PHASE_DEG_DEFAULT;
    s_keycal.click_h3_pct = AUDIO_KEYPAD_CLICK_H3_PERCENT_DEFAULT;
    s_keycal.click_h3_phase_deg = AUDIO_KEYPAD_CLICK_H3_PHASE_DEG_DEFAULT;
    s_keycal.click_h4_pct = AUDIO_KEYPAD_CLICK_H4_PERCENT_DEFAULT;
    s_keycal.click_h4_phase_deg = AUDIO_KEYPAD_CLICK_H4_PHASE_DEG_DEFAULT;
    s_keycal.click_h5_pct = AUDIO_KEYPAD_CLICK_H5_PERCENT_DEFAULT;
    s_keycal.click_h5_phase_deg = AUDIO_KEYPAD_CLICK_H5_PHASE_DEG_DEFAULT;
    s_keycal.click_attack_ms = AUDIO_KEYPAD_CLICK_ATTACK_MS_DEFAULT;
    s_keycal.click_release_ms = AUDIO_KEYPAD_CLICK_RELEASE_MS_DEFAULT;
    s_keycal.dtmf_gain_pct = AUDIO_KEYPAD_DTMF_GAIN_PERCENT_DEFAULT;
    s_keycal.dtmf_handset_low_weight = AUDIO_KEYPAD_DTMF_LOW_WEIGHT_DEFAULT;
    s_keycal.dtmf_handset_high_weight = AUDIO_KEYPAD_DTMF_HIGH_WEIGHT_DEFAULT;
    s_keycal.dtmf_headset_low_weight = AUDIO_KEYPAD_DTMF_HEADSET_LOW_WEIGHT_DEFAULT;
    s_keycal.dtmf_headset_high_weight = AUDIO_KEYPAD_DTMF_HEADSET_HIGH_WEIGHT_DEFAULT;
    s_buzzcal_duty_percent = UINT8_MAX;
    s_charge_trace_live = false;
    s_charge_trace_seen = 0u;
    printf("[debug] USB console ready. Type 'help'.\n");
}

void debug_console_tick(app_t *app) {
    int ch;
    while ((ch = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (ch == '\r' || ch == '\n') {
            if (s_line_len != 0u) {
                s_line[s_line_len] = '\0';
                handle_line(app, s_line);
                s_line_len = 0u;
            }
        } else if (ch == 8 || ch == 127) {
            if (s_line_len != 0u) {
                s_line_len--;
            }
        } else if (ch >= 32 && ch < 127 && s_line_len + 1u < DEBUG_LINE_MAX) {
            s_line[s_line_len++] = (char)ch;
        }
    }

    modem_sms_send_result_t sms_result;
    while (s_waiting_sms_result &&
           modem_service_pop_sms_send_result(s_waiting_sms_request_id,
                                             &sms_result)) {
        s_waiting_sms_result = false;
        s_waiting_sms_request_id = 0u;
        printf("[debug] sms_send outcome=%u\n",
               (unsigned)sms_result.outcome);
    }
    modem_debug_result_t debug_result;
    if (s_waiting_at_result && modem_service_pop_debug_result(&debug_result)) {
        s_waiting_at_result = false;
        printf("[debug] at_result ok=%u\n", debug_result.ok ? 1u : 0u);
    }

    uint32_t now_ms = time_ms();
    poll_keycal(now_ms);
    poll_ltc_window(now_ms);
    poll_charge_trace();
    hw_change_log(app);
}

static void handle_line(app_t *app, char *line) {
    char *cursor = skip_spaces(line);
    char *cmd = next_token(&cursor);
    if (cmd == 0 || cmd[0] == '\0') {
        return;
    }
    if (strcmp(cmd, "help") == 0) {
        print_help();
    } else if (strcmp(cmd, "status") == 0) {
        command_status();
    } else if (strcmp(cmd, "hw") == 0) {
        command_hw(app);
    } else if (strcmp(cmd, "wake") == 0) {
        command_wake();
    } else if (strcmp(cmd, "wdhang") == 0) {
        char *kind = next_token(&cursor);
        char *confirm = next_token(&cursor);
        if (kind == NULL || confirm == NULL ||
            strcmp(confirm, "confirm") != 0 ||
            next_token(&cursor) != NULL ||
            (strcmp(kind, "main") != 0 && strcmp(kind, "flash") != 0)) {
            printf("[watchdog] usage: wdhang <main|flash> confirm\n");
        } else {
            if (strcmp(kind, "flash") == 0) {
                runtime_watchdog_flash_begin();
            } else {
                runtime_watchdog_note_phase(
                    RUNTIME_WATCHDOG_PHASE_DEBUG_HANG);
            }
            printf("[watchdog] intentional %s hang armed; waiting for reset\n",
                   kind);
            stdio_flush();
            while (true) {
                tight_loop_contents();
            }
        }
    } else if (strcmp(cmd, "clocks") == 0) {
        command_clocks();
    } else if (strcmp(cmd, "pads") == 0) {
        command_pads();
    } else if (strcmp(cmd, "ltcalert") == 0) {
        printf("[ltc] force voltage alert ok=%u\n",
               ltc2959_hal_debug_force_voltage_alert() ? 1u : 0u);
    } else if (strcmp(cmd, "ltcnew") == 0) {
        char *arg = next_token(&cursor);
        if (arg == NULL || strcmp(arg, "confirm") != 0 ||
            next_token(&cursor) != NULL) {
            printf("[ltc] usage: ltcnew confirm\n");
        } else {
            printf("[ltc] new battery session ok=%u\n",
                   ltc2959_hal_start_new_session() ? 1u : 0u);
        }
    } else if (strcmp(cmd, "ltcwin") == 0) {
        command_ltc_window(cursor);
    } else if (strcmp(cmd, "battlearn") == 0) {
        command_battery_learning(cursor);
    } else if (strcmp(cmd, "charge") == 0) {
        command_charge(cursor);
    } else if (strcmp(cmd, "modemrx") == 0) {
        command_modem_rx(cursor);
    } else if (strcmp(cmd, "modempoll") == 0) {
        char *arg = next_token(&cursor);
        bool enabled;
        if (arg != NULL && strcmp(arg, "on") == 0) {
            enabled = true;
        } else if (arg != NULL && strcmp(arg, "off") == 0) {
            enabled = false;
        } else {
            printf("[debug] usage: modempoll <on|off>\n");
            return;
        }
        printf("[modempoll] queued=%u target=%s\n",
               modem_service_request_debug_background_polling(enabled) ? 1u : 0u,
               enabled ? "on" : "off");
    } else if (strcmp(cmd, "railhold") == 0) {
        char *arg = next_token(&cursor);
        bool enabled;
        if (arg != NULL && strcmp(arg, "on") == 0) {
            enabled = true;
        } else if (arg != NULL && strcmp(arg, "off") == 0) {
            enabled = false;
        } else {
            printf("[debug] usage: railhold <on|off>\n");
            return;
        }
        shared_3v8_service_set_required(SHARED_3V8_OWNER_DIAGNOSTIC,
                                        enabled);
        printf("[railhold] target=%s rail=%u owners=%02x pg=%u\n",
               enabled ? "on" : "off",
               shared_3v8_service_enabled() ? 1u : 0u,
               (unsigned)shared_3v8_service_owner_mask(),
               board_3v8_rail_power_good() ? 1u : 0u);
    } else if (strcmp(cmd, "irqdrain") == 0) {
        shared_irq_drain_result_t result;
        bool ok = shared_irq_service_drain_now(&result);
        printf("[irq] ok=%u rounds=%u svc=%02x err=%02x released=%u stuck=%u\n",
               ok ? 1u : 0u,
               (unsigned)result.rounds,
               (unsigned)result.serviced_mask,
               (unsigned)result.error_mask,
               result.line_released ? 1u : 0u,
               result.stuck ? 1u : 0u);
    } else if (strcmp(cmd, "lcd") == 0) {
        command_lcd(cursor);
    } else if (strcmp(cmd, "augate") == 0) {
        char *arg = next_token(&cursor);
        if (arg != 0 && strcmp(arg, "on") == 0) {
            core1_services_audio_gate_set_enabled(true);
        } else if (arg != 0 && strcmp(arg, "off") == 0) {
            core1_services_audio_gate_set_enabled(false);
        } else if (arg != 0) {
            printf("[debug] usage: augate <on|off>\n");
        }
        printf("[augate] enabled=%u gated=%u\n",
               (unsigned)core1_services_audio_gate_enabled(),
               (unsigned)core1_services_audio_gated());
    } else if (strcmp(cmd, "sleepen") == 0) {
        char *arg = next_token(&cursor);
        if (arg != 0 && strcmp(arg, "on") == 0) {
            board_set_sleep_gating(true);
            s_sleep_gating = true;
        } else if (arg != 0 && strcmp(arg, "off") == 0) {
            board_set_sleep_gating(false);
            s_sleep_gating = false;
        } else if (arg != 0) {
            printf("[debug] usage: sleepen <on|off>\n");
        }
        printf("[sleepen] %s (R3b: gates XIP/DMA/I2C0/SPI0/PIO0 during SLEEP)\n",
               s_sleep_gating ? "on" : "off");
    } else if (strcmp(cmd, "clkdown") == 0) {
        /* Quiet standby clock-down: XOSC/2 at 6 MHz + PLL_SYS off in quiet
         * backlight-off standby. `on`/`off` to A/B the draw or if a URC/incoming
         * call is ever missed through a clock-down window. (The
         * live clock reads 153.6 MHz over USB by design -- VBUS blocks the
         * clock-down so the console stays fast, verified 2026-07-09: 99% duty.) */
        char *arg = next_token(&cursor);
        if (arg != 0 && strcmp(arg, "off") == 0) {
            board_set_clkdown_enabled(false);
        } else if (arg != 0 && strcmp(arg, "on") == 0) {
            board_set_clkdown_enabled(true);
        }
        printf("[clkdown] %s (%s now)\n",
               board_clkdown_enabled() ? "on" : "off",
               board_is_xosc_lowpower() ? "6 MHz" : "153.6 MHz");
    } else if (strcmp(cmd, "dormant") == 0) {
        command_dormant(cursor);
    } else if (strcmp(cmd, "battmv") == 0) {
        char *arg = next_token(&cursor);
        if (arg == 0) {
            printf("[debug] usage: battmv <mv|off>\n");
        } else if (strcmp(arg, "off") == 0) {
            board_diag_debug_clear_battery_force();
            printf("[debug] battery force cleared\n");
        } else {
            unsigned mv = (unsigned)strtoul(arg, 0, 10);
            board_diag_debug_force_battery_mv((uint16_t)mv);
            printf("[debug] battery forced to %u mv\n", mv);
        }
    } else if (strcmp(cmd, "bridge") == 0) {
        char *a = next_token(&cursor);
        if (a != 0 && strcmp(a, "on") == 0) {
            nau88c22_codec_set_mic_power(true, false); /* A1: mic chain up for the loopback */
            nau88c22_codec_set_route(NAU_ROUTE_HANDSET); /* route + publish format */
            core1_post_command(CORE1_CMD_AUDIO_BRIDGE_START, (uint16_t)NAU_ROUTE_HANDSET);
            printf("[debug] voice bridge START (handset).\n");
        } else if (a != 0 && strcmp(a, "off") == 0) {
            core1_post_command(CORE1_CMD_AUDIO_BRIDGE_STOP, 0u);
            nau88c22_codec_set_mic_power(false, accessory_hal_headset_inserted());
            nau88c22_codec_set_route(NAU_ROUTE_HANDSET);
            printf("[debug] voice bridge STOP\n");
        } else {
            printf("[debug] usage: bridge <on|off>\n");
        }
    } else if (strcmp(cmd, "spkgain") == 0) {
        char *a = next_token(&cursor);
        if (a == 0) {
            printf("[debug] usage: spkgain <0-63>  (0x39=0dB, 0x3f=+6dB max)\n");
        } else {
            unsigned g = (unsigned)strtoul(a, 0, 0);
            bool ok = nau88c22_codec_set_speaker_gain((uint8_t)g);
            printf("[debug] spkgain %u (0x%02x) ok=%u\n", g, g, ok ? 1u : 0u);
        }
    } else if (strcmp(cmd, "hpgain") == 0) {
        char *a = next_token(&cursor);
        if (a == 0) {
            printf("[debug] usage: hpgain <0-63>  (0x39=0dB, 1dB/step)\n");
        } else {
            unsigned g = (unsigned)strtoul(a, 0, 0);
            bool ok = g <= 0x3fu && nau88c22_codec_set_headphone_gain((uint8_t)g);
            printf("[debug] hpgain %u (0x%02x) ok=%u\n", g, g, ok ? 1u : 0u);
        }
    } else if (strcmp(cmd, "audioroute") == 0) {
        char *a = next_token(&cursor);
        bool applied = false;
        if (a != 0 && next_token(&cursor) == 0) {
            nau_route_t route;
            if (strcmp(a, "auto") == 0) {
                route = accessory_hal_headset_inserted() ? NAU_ROUTE_HEADSET
                                                         : NAU_ROUTE_HANDSET;
                applied = nau88c22_codec_set_route(route);
            } else if (strcmp(a, "handset") == 0) {
                applied = nau88c22_codec_set_route(NAU_ROUTE_HANDSET);
            } else if (strcmp(a, "headset") == 0) {
                applied = nau88c22_codec_set_route(NAU_ROUTE_HEADSET);
            } else {
                printf("[debug] usage: audioroute [auto|handset|headset]\n");
                return;
            }
        } else if (a != 0) {
            printf("[debug] usage: audioroute [auto|handset|headset]\n");
            return;
        }
        nau88c22_codec_diag_t diag;
        nau88c22_codec_get_diag(&diag);
        printf("[debug] audioroute codec=%u bridge=%u invert=%u applied=%u\n",
               (unsigned)diag.route,
               (unsigned)audio_bridge_route(),
               audio_bridge_right_invert() ? 1u : 0u,
               applied ? 1u : 0u);
    } else if (strcmp(cmd, "digain") == 0) {
        char *a = next_token(&cursor);
        if (a == 0) {
            printf("[debug] usage: digain <percent>  (100=unity, earpiece digital gain)\n");
        } else {
            unsigned pct = (unsigned)strtoul(a, 0, 0);
            if (pct > 400u) {
                pct = 400u;
            }
            uint16_t q8 = (uint16_t)((pct * 256u) / 100u);
            core1_post_command(CORE1_CMD_AUDIO_SET_EARPIECE_GAIN, q8);
            printf("[debug] earpiece digital gain %u%% (q8=%u)\n", pct, (unsigned)q8);
        }
    } else if (strcmp(cmd, "keycal") == 0) {
        command_keycal(cursor);
    } else if (strcmp(cmd, "buzzcal") == 0) {
        command_buzzcal(cursor);
    } else if (strcmp(cmd, "gp11") == 0) {
        /* Sample GP11 (modem RXD, driven by modem_i2s_tx). Many transitions = the RP
         * is driving the uplink I2S; ~0 transitions = not driven / static (so the
         * fault is on the RP side, not the shifter). Needs the loopback/call active
         * (the TX SM only clocks while the modem drives WA/BCLK). */
        bool bclk_ie =
            (pads_bank0_hw->io[MODEM_I2S_PIN_CLK] &
             PADS_BANK0_GPIO0_IE_BITS) != 0u;
        bool rxd_ie =
            (pads_bank0_hw->io[MODEM_I2S_PIN_RXD] &
             PADS_BANK0_GPIO0_IE_BITS) != 0u;
        gpio_set_input_enabled(MODEM_I2S_PIN_CLK, true);
        gpio_set_input_enabled(MODEM_I2S_PIN_RXD, true);
        uint32_t bclk_tr = 0u, rxd_tr = 0u, rxd_hi = 0u;
        bool pb = gpio_get(MODEM_I2S_PIN_CLK);
        bool pr = gpio_get(MODEM_I2S_PIN_RXD);
        absolute_time_t end = make_timeout_time_ms(20);
        while (!time_reached(end)) {
            bool b = gpio_get(MODEM_I2S_PIN_CLK);
            bool r = gpio_get(MODEM_I2S_PIN_RXD);
            if (b != pb) {
                bclk_tr++;
            }
            if (r != pr) {
                rxd_tr++;
            }
            if (r) {
                rxd_hi++;
            }
            pb = b;
            pr = r;
        }
        gpio_set_input_enabled(MODEM_I2S_PIN_CLK, bclk_ie);
        gpio_set_input_enabled(MODEM_I2S_PIN_RXD, rxd_ie);
        printf("[debug] gp8(BCLK ref) tr=%lu | gp11(RXD) tr=%lu hi=%lu  (BCLK tr>0 = readback OK)\n",
               (unsigned long)bclk_tr, (unsigned long)rxd_tr, (unsigned long)rxd_hi);
    } else if (strcmp(cmd, "txprobe") == 0) {
        modem_i2s_tx_probe_t p;
        memset(&p, 0, sizeof(p));
        modem_i2s_hal_tx_probe(&p);
        printf("[debug] txprobe: en=%d irq=%lu fifo=%lu pc=%lu..%lu(entry=%lu) "
               "dma0(busy=%d cnt=%lu ctrl=%08lx read=%08lx) dma1(busy=%d cnt=%lu) fdebug=%08lx\n",
               (int)p.tx_sm_enabled, (unsigned long)p.tx_irq_count,
               (unsigned long)p.tx_fifo_level, (unsigned long)p.pc_min,
               (unsigned long)p.pc_max, (unsigned long)p.tx_prog_entry,
               (int)p.dma0_busy, (unsigned long)p.dma0_tcount,
               (unsigned long)p.dma0_ctrl, (unsigned long)p.dma0_read,
               (int)p.dma1_busy, (unsigned long)p.dma1_tcount,
               (unsigned long)p.pio_fdebug);
    } else if (strcmp(cmd, "tcar") == 0) {
        char *a = next_token(&cursor);
        if (a == 0) {
            printf("[debug] usage: tcar <reg>  (TCA8418; e.g. 0x14 GPIO_DAT1, 0x2c PULL1)\n");
        } else {
            unsigned r = (unsigned)strtoul(a, 0, 0);
            uint8_t v = 0u;
            bool ok = tca8418_hal_debug_read_reg((uint8_t)r, &v);
            printf("[debug] tcar 0x%02x = 0x%02x ok=%u\n", r, (unsigned)v, ok ? 1u : 0u);
        }
    } else if (strcmp(cmd, "tcaw") == 0) {
        char *a = next_token(&cursor);
        char *b = next_token(&cursor);
        if (a == 0 || b == 0) {
            printf("[debug] usage: tcaw <reg> <val>\n");
        } else {
            unsigned r = (unsigned)strtoul(a, 0, 0);
            unsigned v = (unsigned)strtoul(b, 0, 0);
            bool ok = tca8418_hal_debug_write_reg((uint8_t)r, (uint8_t)v);
            printf("[debug] tcaw 0x%02x <= 0x%02x ok=%u\n", r, v, ok ? 1u : 0u);
        }
    } else if (strcmp(cmd, "codecr") == 0) {
        char *a = next_token(&cursor);
        if (a == 0) {
            printf("[debug] usage: codecr <reg>\n");
        } else {
            unsigned r = (unsigned)strtoul(a, 0, 0);
            uint16_t v = 0u;
            bool ok = nau88c22_codec_read_reg((uint8_t)r, &v);
            printf("[debug] codecr 0x%02x = 0x%03x ok=%u\n", r, (unsigned)v, ok ? 1u : 0u);
        }
    } else if (strcmp(cmd, "codecw") == 0) {
        char *a = next_token(&cursor);
        char *b = next_token(&cursor);
        if (a == 0 || b == 0) {
            printf("[debug] usage: codecw <reg> <val>  (val may need 0x100 update bit)\n");
        } else {
            unsigned r = (unsigned)strtoul(a, 0, 0);
            unsigned v = (unsigned)strtoul(b, 0, 0);
            bool ok = nau88c22_codec_write_reg((uint8_t)r, (uint16_t)v);
            printf("[debug] codecw 0x%02x <= 0x%03x ok=%u\n", r, v, ok ? 1u : 0u);
        }
    } else if (strcmp(cmd, "ui") == 0) {
        command_ui(app, cursor);
    } else if (strcmp(cmd, "phoneon") == 0) {
        bool was_off = app->route == APP_ROUTE_POWER_OFF;
        bool started = s_phone_power_on != 0 && s_phone_power_on(app, time_ms());
        if (s_phone_power_on == 0) {
            printf("[debug] phone power-on hook not registered\n");
        } else if (started) {
            printf("[debug] phone power-on started\n");
        } else if (!was_off) {
            printf("[debug] phone already on\n");
        } else {
            printf("[debug] phone power-on refused by battery gate\n");
        }
    } else if (strcmp(cmd, "poweron") == 0) {
        modem_service_power_on();
        printf("[debug] modem power-on requested\n");
    } else if (strcmp(cmd, "poweroff") == 0) {
        modem_service_power_off();
        printf("[debug] modem power-off requested\n");
    } else if (strcmp(cmd, "at") == 0) {
        char *at = skip_spaces(cursor);
        bool ok = modem_service_request_debug_at(at);
        if (ok) {
            s_waiting_at_result = true;
        }
        printf("[debug] queued AT ok=%u cmd=%s\n", ok ? 1u : 0u, at);
    } else if (strcmp(cmd, "txt") == 0) {
        command_text(cursor);
    } else if (strcmp(cmd, "pdu7") == 0) {
        command_pdu7(cursor);
    } else if (strcmp(cmd, "bin") == 0) {
        command_binary(cursor);
    } else if (strcmp(cmd, "binf5") == 0) {
        command_binary_f5(cursor);
    } else if (strcmp(cmd, "port") == 0) {
        command_port(cursor);
    } else if (strcmp(cmd, "pic") == 0) {
        command_picture(cursor);
    } else {
        printf("[debug] unknown command: %s\n", cmd);
    }
}

static void command_modem_rx(char *args) {
    char *arg = next_token(&args);
    if (arg == NULL || next_token(&args) != NULL ||
        (strcmp(arg, "on") != 0 && strcmp(arg, "off") != 0 &&
         strcmp(arg, "dump") != 0)) {
        printf("[modemrx] usage: modemrx <on|off|dump>\n");
        return;
    }
    if (strcmp(arg, "on") == 0) {
        printf("[modemrx] started=%u\n",
               modem_service_rx_trace_start() ? 1u : 0u);
        return;
    }
    modem_service_rx_trace_stop();
    modem_rx_trace_status_t trace;
    modem_service_rx_trace_status(&trace);
    printf("[modemrx] stopped received=%lu retained=%u overwritten=%lu\n",
           (unsigned long)trace.received, (unsigned)trace.retained,
           (unsigned long)(trace.received - trace.retained));
    if (strcmp(arg, "dump") == 0) {
        uint8_t bytes[16];
        for (size_t offset = 0u; offset < trace.retained; offset += sizeof(bytes)) {
            size_t count = modem_service_rx_trace_read(offset, bytes, sizeof(bytes));
            if (count == 0u) break;
            printf("[modemrx] %04x:", (unsigned)offset);
            for (size_t i = 0u; i < count; i++) printf(" %02x", bytes[i]);
            printf("\n");
        }
        printf("[modemrx] end\n");
    }
}

static void print_help(void) {
    printf("[debug] commands:\n");
    printf("[debug]   status\n");
    printf("[debug]   hw                        ; battery/charger/headset snapshot\n");
    printf("[debug]   wake                      ; last-boot wake evidence + scratch probe\n");
    printf("[debug]   wdhang <main|flash> confirm ; intentional watchdog reset test\n");
    printf("[debug]   clocks                    ; clock freqs + wake/sleep/enabled gate masks\n");
    printf("[debug]   ltcalert                  ; force one LTC voltage threshold alert\n");
    printf("[debug]   ltcnew confirm            ; reset ACR for a confirmed new battery\n");
    printf("[debug]   ltcwin [<delay_s> <duration_s>|cancel] ; timed ACR average\n");
    printf("[debug]   battlearn [status|provision <capacity_mAh> [<high_mOhm> <mid_mOhm> <low_mOhm>] [empty] confirm]\n");
    printf("[debug]   battlearn recover-cycle <capacity_mAh> <expected_ok_count> confirm\n");
    printf("[debug]   battlearn recover-full <acr_raw> <session_delta_nAh> <expected_ok_count> confirm\n");
    printf("[debug]   charge [status|trace <on|off>|csv|history]\n");
    printf("[debug]   charge configure <observe|anchored|bootstrap> <1000-2000> <candidate|trusted> confirm\n");
    printf("[debug]   modempoll <on|off>        ; periodic modem backstops (RAM only)\n");
    printf("[debug]   modemrx <on|off|dump>    ; opt-in raw RX capture (RAM only)\n");
    printf("[debug]   railhold <on|off>         ; hold shared +3V8 without modem start\n");
    printf("[debug]   irqdrain                  ; bounded GP42 source drain + evidence\n");
    printf("[debug]   lcd [status|cal <0-31>|save|stock|set <vop> <tc> <bias>|contrast <0-127>|temp <0-3>|bias <0-7>|reinit]\n");
    printf("[debug]   augate <on|off>           ; audio idle gate (A2+R2 bench toggle)\n");
    printf("[debug]   sleepen <on|off>          ; R3b SLEEP-state clock gating (bench URC-soak first)\n");
    printf("[debug]   clkdown <on|off>          ; 6 MHz quiet standby clock\n");
    printf("[debug]   dormant [on|off]          ; powered-on clock-DORMANT + wake counters\n");
    printf("[debug]   battmv <mv|off>           ; force battery mv (test low-batt path)\n");
    printf("[debug]   spkgain <0-63>            ; earpiece analog gain (0x39=0dB 0x3f=+6dB)\n");
    printf("[debug]   hpgain <0-63>             ; headset analog gain (0x39=0dB, 1dB/step)\n");
    printf("[debug]   audioroute [auto|handset|headset] ; volatile route + core1 format probe\n");
    printf("[debug]   digain <percent>          ; earpiece digital gain (100=unity)\n");
    printf("[debug]   keycal [status|reset|click <gain%%> <h3%%> <phase_deg> <attack_ms> <release_ms>|\n");
    printf("[debug]           harmonic <2|3|4|5> <percent> <phase_deg>|\n");
    printf("[debug]           dtmf <gain%%> <low_w> <high_w>|play <click|key> [ms]|stop]\n");
    printf("[debug]   buzzcal [status|duty <1-50|auto>|play [1-5]|stop] ; volatile buzzer trim\n");
    printf("[debug]   codecr <reg> | codecw <reg> <val>  ; raw NAU88C22 register access\n");
    printf("[debug]   tcar <reg> | tcaw <reg> <val>      ; raw TCA8418 register access\n");
    printf("[debug]   bridge <on|off>           ; start/stop voice bridge (loopback test)\n");
    printf("[debug]   ui <standby|number|keyguard|call|callopts|incoming>\n");
    printf("[debug]   phoneon                  ; start phone through normal app power-on path\n");
    printf("[debug]   poweron | poweroff       ; modem-only power controls\n");
    printf("[debug]   at AT+COMMAND\n");
    printf("[debug]   txt <number> <message>\n");
    printf("[debug]   pdu7 <number>             ; GSM-7 DCS 00 PDU-mode control\n");
    printf("[debug]   bin <number>              ; 8-bit DCS 04, no UDH\n");
    printf("[debug]   binf5 <number>            ; 8-bit DCS F5, no UDH\n");
    printf("[debug]   port <number>             ; 8-bit DCS 04, port UDH 158A/0000\n");
    printf("[debug]   pic <number> <mode>       ; 0 dcs04 port-first, 1 dcs04 concat-first,\n");
    printf("[debug]                              2 f5 port-first, 3 old f5 concat-first+VP\n");
}

static char *skip_spaces(char *text) {
    while (text != 0 && *text == ' ') {
        text++;
    }
    return text;
}

static char *next_token(char **cursor) {
    if (cursor == 0 || *cursor == 0) {
        return 0;
    }
    char *start = skip_spaces(*cursor);
    if (*start == '\0') {
        *cursor = start;
        return 0;
    }
    char *end = start;
    while (*end != '\0' && *end != ' ') {
        end++;
    }
    if (*end != '\0') {
        *end++ = '\0';
    }
    *cursor = end;
    return start;
}

static bool parse_seconds(const char *text, uint32_t minimum,
                          uint32_t maximum, uint32_t *value_out) {
    if (text == NULL || text[0] == '\0' || value_out == NULL) {
        return false;
    }
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value < minimum || value > maximum) {
        return false;
    }
    *value_out = (uint32_t)value;
    return true;
}

static bool parse_signed_range(const char *text, int32_t minimum,
                               int32_t maximum, int32_t *value_out) {
    if (text == NULL || text[0] == '\0' || value_out == NULL) {
        return false;
    }
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < minimum || value > maximum) {
        return false;
    }
    *value_out = (int32_t)value;
    return true;
}

static bool parse_u32_auto(const char *text, uint32_t *value_out) {
    if (text == NULL || text[0] == '\0' || value_out == NULL) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 0);
    if (errno == ERANGE || end == text || *end != '\0' ||
        value > UINT32_MAX) {
        return false;
    }
    *value_out = (uint32_t)value;
    return true;
}

static bool parse_i64_decimal(const char *text, int64_t *value_out) {
    if (text == NULL || text[0] == '\0' || value_out == NULL) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    long long value = strtoll(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0') {
        return false;
    }
    *value_out = (int64_t)value;
    return true;
}

static const char *battery_soc_provenance_text(
    battery_soc_provenance_t provenance) {
    static const char *const TEXT[] = {
        "unknown", "bootstrap-voltage", "tracked", "anchored-full",
        "anchored-empty",
    };
    uint8_t index = (uint8_t)provenance;
    return index < sizeof(TEXT) / sizeof(TEXT[0]) ? TEXT[index] : "invalid";
}

static const char *battery_soc_confidence_text(
    battery_soc_confidence_t confidence) {
    static const char *const TEXT[] = {
        "none", "provisional", "anchored",
    };
    uint8_t index = (uint8_t)confidence;
    return index < sizeof(TEXT) / sizeof(TEXT[0]) ? TEXT[index] : "invalid";
}

static const char *charge_policy_text(
    battery_charge_supervisor_policy_t policy) {
    static const char *const TEXT[] = {
        "observe", "enforce-anchored", "enforce-bootstrap",
    };
    uint8_t index = (uint8_t)policy;
    return index < sizeof(TEXT) / sizeof(TEXT[0]) ? TEXT[index] : "invalid";
}

static void command_battery_learning(char *args) {
    char *sub = next_token(&args);
    if (sub == NULL || strcmp(sub, "status") == 0) {
        if (sub != NULL && next_token(&args) != NULL) {
            printf("[battlearn] usage: battlearn [status]\n");
            return;
        }
        battery_learning_service_snapshot_t snapshot;
        battery_learning_service_get_snapshot(&snapshot);
        const battery_learning_snapshot_t *model = &snapshot.model;
        static const char *const CONFIDENCE[] = {
            "prior", "observed", "learned", "conflicted",
        };
        uint8_t confidence = (uint8_t)model->capacity_confidence;
        const char *confidence_text =
            confidence < sizeof(CONFIDENCE) / sizeof(CONFIDENCE[0])
                ? CONFIDENCE[confidence] : "unknown";
        store_diag_snapshot_t store;
        store_service_get_diag(&store);
        board_diag_snapshot_t board;
        board_diag_get_snapshot(&board);
        const char *voltage_relation = !board.battery_valid
            ? "unknown"
            : board.battery_empty ? "empty"
            : board.battery_low ? "low" : "healthy";
        uint16_t unit_mask =
            (uint16_t)(UINT16_C(1) << STORE_UNIT_BATTERY_LEARNING);
        if (model->learned_capacity_valid) {
            printf("[battlearn] cap=%u conf=%s ok=%u bad=%u "
                   "anchor=%u/%u empty=%u generation=%lu\n",
                   (unsigned)model->learned_capacity_mah,
                   confidence_text,
                   (unsigned)model->accepted_capacity_cycles,
                   (unsigned)model->rejected_capacity_cycles,
                   model->full_anchor_valid ? 1u : 0u,
                   model->capacity_cycle_qualified ? 1u : 0u,
                   model->natural_empty_valid ? 1u : 0u,
                   (unsigned long)model->pack_generation);
        } else {
            printf("[battlearn] cap=-- conf=%s ok=%u bad=%u "
                   "anchor=%u/%u empty=%u generation=%lu\n",
                   confidence_text,
                   (unsigned)model->accepted_capacity_cycles,
                   (unsigned)model->rejected_capacity_cycles,
                   model->full_anchor_valid ? 1u : 0u,
                   model->capacity_cycle_qualified ? 1u : 0u,
                   model->natural_empty_valid ? 1u : 0u,
                   (unsigned long)model->pack_generation);
        }
        printf("[battlearn] resistance H/M/L=%u/%u/%u mOhm "
               "store dirty=%u degraded=%u handoff=%u\n",
               (unsigned)model->resistance_mohm[0],
               (unsigned)model->resistance_mohm[1],
               (unsigned)model->resistance_mohm[2],
               (store.dirty_mask & unit_mask) != 0u ? 1u : 0u,
               (store.degraded_mask & unit_mask) != 0u ? 1u : 0u,
               snapshot.persistence_pending ? 1u : 0u);
        printf("[battlearn] history");
        for (uint8_t i = 0u; i < model->capacity_history_count; i++) {
            printf(" %u", (unsigned)model->capacity_history_mah[i]);
        }
        printf(" next=%u\n", (unsigned)model->capacity_history_next);
        if (model->remaining_capacity_valid) {
            printf("[battlearn] soc=%u%% remaining=%llumAh.%03llu "
                   "capacity=%umAh bars=%u/%u source=%s "
                   "confidence=%s segment=%s bootstrap=%umV\n",
                   (unsigned)model->state_of_charge_percent,
                   (unsigned long long)(model->remaining_capacity_nah /
                                        UINT64_C(1000000)),
                   (unsigned long long)((model->remaining_capacity_nah /
                                         UINT64_C(1000)) % UINT64_C(1000)),
                   (unsigned)model->soc_capacity_mah,
                   (unsigned)model->soc_bars,
                   model->soc_bars_valid ? 1u : 0u,
                   battery_soc_provenance_text(model->soc_provenance),
                   battery_soc_confidence_text(model->soc_confidence),
                   model->soc_charge_segment ? "charge" : "discharge",
                   (unsigned)model->soc_bootstrap_reference_mv);
        } else {
            printf("[battlearn] soc=-- remaining=-- source=%s confidence=%s\n",
                   battery_soc_provenance_text(model->soc_provenance),
                   battery_soc_confidence_text(model->soc_confidence));
        }
        printf("[battlearn] endpoint prediction=%u "
               "overrun=%llumAh.%03llu voltage=%s\n",
               model->capacity_prediction_exhausted ? 1u : 0u,
               (unsigned long long)(model->capacity_overrun_nah /
                                    UINT64_C(1000000)),
               (unsigned long long)((model->capacity_overrun_nah /
                                     UINT64_C(1000)) % UINT64_C(1000)),
               voltage_relation);
        if (model->learned_capacity_valid) {
            printf("[battlearn] restore: battlearn provision %u %u %u %u%s confirm\n",
                   (unsigned)model->learned_capacity_mah,
                   (unsigned)model->resistance_mohm[0],
                   (unsigned)model->resistance_mohm[1],
                   (unsigned)model->resistance_mohm[2],
                   model->natural_empty_valid ? " empty" : "");
        } else {
            printf("[battlearn] restore unavailable: no observed capacity\n");
        }
        return;
    }

    if (strcmp(sub, "recover-full") == 0) {
        char *acr_text = next_token(&args);
        char *delta_text = next_token(&args);
        char *count_text = next_token(&args);
        char *confirm = next_token(&args);
        uint32_t acr_raw = 0u;
        int64_t session_delta_nah = 0;
        uint32_t expected_count = 0u;
        bool syntax_ok = acr_text != NULL && delta_text != NULL &&
            count_text != NULL && confirm != NULL &&
            strcmp(confirm, "confirm") == 0 && next_token(&args) == NULL &&
            parse_u32_auto(acr_text, &acr_raw) &&
            parse_i64_decimal(delta_text, &session_delta_nah) &&
            parse_seconds(count_text, 0u, UINT16_MAX, &expected_count);
        if (!syntax_ok) {
            printf("[battlearn] usage: battlearn recover-full <acr_raw> "
                   "<session_delta_nAh> <expected_ok_count> confirm\n");
            return;
        }

        battery_learning_recover_status_t status =
            battery_learning_service_recover_full_endpoint(
                acr_raw, session_delta_nah, (uint16_t)expected_count);
        if (status != BATTERY_LEARNING_RECOVER_OK) {
            static const char *const ERRORS[] = {
                "ok", "invalid parameters", "accepted count changed",
                "charger/session active", "current gauge evidence unavailable",
                "store not ready", "store handoff failed",
            };
            uint8_t index = (uint8_t)status;
            printf("[battlearn] recover-full refused: %s\n",
                   index < sizeof(ERRORS) / sizeof(ERRORS[0])
                       ? ERRORS[index] : "unknown error");
            return;
        }

        store_diag_snapshot_t store;
        store_service_get_diag(&store);
        uint16_t unit_mask = (uint16_t)(
            UINT16_C(1) << STORE_UNIT_BATTERY_LEARNING);
        printf("[battlearn] restored full acr=%08lx dQ=%lldnAh "
               "expected_ok=%u; journal dirty=%u "
               "(wait for 0 before power removal)\n",
               (unsigned long)acr_raw, (long long)session_delta_nah,
               (unsigned)expected_count,
               (store.dirty_mask & unit_mask) != 0u ? 1u : 0u);
        return;
    }

    if (strcmp(sub, "recover-cycle") == 0) {
        const battery_learning_profile_t *profile =
            battery_learning_nimh_profile();
        char *capacity_text = next_token(&args);
        char *count_text = next_token(&args);
        char *confirm = next_token(&args);
        uint32_t capacity = 0u;
        uint32_t expected_count = 0u;
        bool syntax_ok = capacity_text != NULL && count_text != NULL &&
            confirm != NULL && strcmp(confirm, "confirm") == 0 &&
            next_token(&args) == NULL &&
            parse_seconds(capacity_text, profile->capacity_min_mah,
                          profile->capacity_max_mah, &capacity) &&
            parse_seconds(count_text, 0u, UINT16_MAX, &expected_count);
        if (!syntax_ok) {
            printf("[battlearn] usage: battlearn recover-cycle <%u-%u mAh> "
                   "<expected_ok_count> confirm\n",
                   (unsigned)profile->capacity_min_mah,
                   (unsigned)profile->capacity_max_mah);
            return;
        }

        battery_learning_recover_status_t status =
            battery_learning_service_recover_capacity_cycle(
                (uint16_t)capacity, (uint16_t)expected_count);
        if (status != BATTERY_LEARNING_RECOVER_OK) {
            static const char *const ERRORS[] = {
                "ok", "invalid parameters", "accepted count changed",
                "charger/session active", "current gauge evidence unavailable",
                "store not ready", "store handoff failed",
            };
            uint8_t index = (uint8_t)status;
            printf("[battlearn] recover-cycle refused: %s\n",
                   index < sizeof(ERRORS) / sizeof(ERRORS[0])
                       ? ERRORS[index] : "unknown error");
            return;
        }

        store_diag_snapshot_t store;
        store_service_get_diag(&store);
        uint16_t unit_mask = (uint16_t)(
            UINT16_C(1) << STORE_UNIT_BATTERY_LEARNING);
        printf("[battlearn] appended recovered cycle=%u expected_ok=%u; "
               "journal dirty=%u (wait for 0 before power removal)\n",
               (unsigned)capacity, (unsigned)expected_count,
               (store.dirty_mask & unit_mask) != 0u ? 1u : 0u);
        return;
    }

    if (strcmp(sub, "provision") != 0) {
        printf("[battlearn] usage: battlearn "
               "[status|provision ...|recover-cycle ...|recover-full ...]\n");
        return;
    }

    const battery_learning_profile_t *profile =
        battery_learning_nimh_profile();
    char *capacity_text = next_token(&args);
    char *next = next_token(&args);
    uint32_t capacity = 0u;
    battery_learning_provision_t provision = {0};
    bool syntax_ok = capacity_text != NULL && next != NULL &&
        parse_seconds(capacity_text, profile->capacity_min_mah,
                      profile->capacity_max_mah, &capacity);
    if (syntax_ok && strcmp(next, "confirm") == 0) {
        syntax_ok = next_token(&args) == NULL;
    } else if (syntax_ok && strcmp(next, "empty") == 0) {
        char *confirm = next_token(&args);
        syntax_ok = confirm != NULL && strcmp(confirm, "confirm") == 0 &&
            next_token(&args) == NULL;
        provision.natural_empty_valid = syntax_ok;
    } else if (syntax_ok) {
        char *mid_text = next_token(&args);
        char *low_text = next_token(&args);
        char *tail = next_token(&args);
        uint32_t resistance[3] = {0u, 0u, 0u};
        syntax_ok = mid_text != NULL && low_text != NULL && tail != NULL &&
            parse_seconds(next, 0u, profile->resistance_max_mohm,
                          &resistance[0]) &&
            parse_seconds(mid_text, 0u, profile->resistance_max_mohm,
                          &resistance[1]) &&
            parse_seconds(low_text, 0u, profile->resistance_max_mohm,
                          &resistance[2]);
        if (syntax_ok && strcmp(tail, "empty") == 0) {
            char *confirm = next_token(&args);
            syntax_ok = confirm != NULL && strcmp(confirm, "confirm") == 0;
            provision.natural_empty_valid = syntax_ok;
        } else {
            syntax_ok = syntax_ok && strcmp(tail, "confirm") == 0;
        }
        syntax_ok = syntax_ok && next_token(&args) == NULL;
        for (uint8_t i = 0u;
             syntax_ok && i < BATTERY_LEARNING_RESISTANCE_BIN_COUNT; i++) {
            provision.resistance_mohm[i] = (uint16_t)resistance[i];
        }
    }
    if (!syntax_ok) {
        printf("[battlearn] usage: battlearn provision <%u-%u mAh> "
               "[<H M L: 0 or %u-%u mOhm>] [empty] confirm\n",
               (unsigned)profile->capacity_min_mah,
               (unsigned)profile->capacity_max_mah,
               (unsigned)profile->resistance_min_mohm,
               (unsigned)profile->resistance_max_mohm);
        return;
    }
    provision.capacity_mah = (uint16_t)capacity;

    battery_learning_provision_status_t status =
        battery_learning_service_provision(&provision);
    if (status != BATTERY_LEARNING_PROVISION_OK) {
        static const char *const ERRORS[] = {
            "ok", "invalid parameters", "charge/cycle active",
            "current gauge evidence unavailable", "store not ready",
            "store handoff failed",
        };
        uint8_t index = (uint8_t)status;
        printf("[battlearn] provision refused: %s\n",
               index < sizeof(ERRORS) / sizeof(ERRORS[0])
                   ? ERRORS[index] : "unknown error");
        return;
    }

    store_diag_snapshot_t store;
    store_service_get_diag(&store);
    uint16_t unit_mask =
        (uint16_t)(UINT16_C(1) << STORE_UNIT_BATTERY_LEARNING);
    printf("[battlearn] applied capacity=%u resistance=%u/%u/%u empty=%u; "
           "journal dirty=%u (wait for 0 before battery removal)\n",
           (unsigned)provision.capacity_mah,
           (unsigned)provision.resistance_mohm[0],
           (unsigned)provision.resistance_mohm[1],
           (unsigned)provision.resistance_mohm[2],
           provision.natural_empty_valid ? 1u : 0u,
           (store.dirty_mask & unit_mask) != 0u ? 1u : 0u);
}

static const char *charge_phase_text(
    battery_charge_supervisor_phase_t phase) {
    static const char *const TEXT[] = {
        "detached", "qualifying", "charging", "complete-bq",
        "complete-sw", "stopped-safety", "fault", "degraded", "stopping",
    };
    uint8_t index = (uint8_t)phase;
    return index < sizeof(TEXT) / sizeof(TEXT[0]) ? TEXT[index] : "unknown";
}

static const char *charge_terminal_text(
    battery_charge_supervisor_terminal_t terminal) {
    static const char *const TEXT[] = {
        "none", "bq-complete", "bq-already-full", "sw-coulomb",
        "sw-curve", "sw-coulomb+curve", "safety-charge",
        "safety-time", "board-temperature", "bq-fault",
        "control-readback", "gauge-continuity", "manual-debug",
        "detached", "bq-fault+coulomb-full", "maintenance-rearm",
    };
    uint8_t index = (uint8_t)terminal;
    return index < sizeof(TEXT) / sizeof(TEXT[0]) ? TEXT[index] : "unknown";
}

static void print_charge_csv_row(
    const battery_charge_supervisor_minute_t *minute) {
    printf("[charge.csv] %lu,%lu,%u,%u,%ld,%ld,%lld\n",
           (unsigned long)minute->at_ms,
           (unsigned long)minute->elapsed_ms,
           (unsigned)minute->terminal_median_mv,
           (unsigned)minute->compensated_median_mv,
           (long)minute->current_average_ua,
           (long)minute->temperature_average_mdegc,
           (long long)minute->net_input_nah);
}

static void poll_charge_trace(void) {
    battery_charge_supervisor_service_snapshot_t service;
    battery_charge_supervisor_service_get_snapshot(&service);
    if (!s_charge_trace_live) {
        s_charge_trace_seen = service.minute_events;
        return;
    }
    if (service.minute_events == s_charge_trace_seen) {
        return;
    }
    if (service.minute_events - s_charge_trace_seen > 1u) {
        printf("[charge] trace skipped=%lu\n",
               (unsigned long)(service.minute_events -
                               s_charge_trace_seen - 1u));
    }
    s_charge_trace_seen = service.minute_events;
    print_charge_csv_row(&service.model.latest_minute);
}

static void command_charge(char *args) {
    char *sub = next_token(&args);
    if (sub == NULL || strcmp(sub, "status") == 0) {
        if (sub != NULL && next_token(&args) != NULL) {
            printf("[charge] usage: charge [status]\n");
            return;
        }
        battery_charge_supervisor_service_snapshot_t service;
        battery_charge_supervisor_service_get_snapshot(&service);
        const battery_charge_supervisor_snapshot_t *model = &service.model;
        const battery_charge_supervisor_profile_t *profile =
            battery_charge_supervisor_revb2_profile();
        board_diag_snapshot_t board;
        board_diag_get_snapshot(&board);
        printf("[charge] policy=%s phase=%s gen=%lu pack=%lu gauge=%lu "
               "elapsed=%lums terminal=%s\n",
               charge_policy_text(model->policy),
               charge_phase_text(model->phase),
               (unsigned long)model->charge_generation,
               (unsigned long)model->pack_generation,
               (unsigned long)model->gauge_session,
               (unsigned long)model->elapsed_ms,
               charge_terminal_text(model->terminal_reason));
        printf("[charge] hardware configured=%u session=%u current=%lumA timer=%lumin\n",
               (unsigned)profile->hardware_profile_id,
               (unsigned)model->hardware_profile_id,
               (unsigned long)(profile->nominal_charge_current_ua / 1000u),
               (unsigned long)(profile->nominal_backup_timer_ms / 60000u));
        printf("[charge] input=%lldnAh positive=%llunAh negative=%llunAh\n",
               (long long)model->net_input_nah,
               (unsigned long long)model->positive_crosscheck_nah,
               (unsigned long long)model->negative_crosscheck_nah);
        printf("[charge] deficit=%s%llunAh (%umAh) target=%s%llunAh "
               "basis=%s safety=%llunAh factor=%u/1000 %s\n",
               model->deficit_valid ? "" : "?",
               (unsigned long long)model->deficit_nah,
               (unsigned)model->deficit_mah,
               model->target_valid ? "" : "?",
               (unsigned long long)model->target_input_nah,
               model->target_full_capacity ? "full-capacity" : "soc-deficit",
               (unsigned long long)model->safety_input_nah,
               (unsigned)model->charge_factor_permille,
               model->charge_factor_confident ? "trusted" : "candidate");
        printf("[charge] frozen capacity=%s%umAh remaining=%s%llunAh "
               "source=%s confidence=%s\n",
               model->frozen_capacity_valid ? "" : "?",
               (unsigned)model->frozen_capacity_mah,
               model->frozen_remaining_valid ? "" : "?",
               (unsigned long long)model->frozen_remaining_nah,
               battery_soc_provenance_text(model->frozen_soc_provenance),
               battery_soc_confidence_text(model->frozen_soc_confidence));
        printf("[charge] voltage=%u/%umV peak=%u drop=%u slope=%d "
               "current=%lduA temp=%ldmC minute=%lu trace=%u/%u\n",
               (unsigned)model->terminal_mv,
               (unsigned)model->compensated_mv,
               (unsigned)model->curve_peak_mv,
               (unsigned)model->curve_drop_mv,
               (int)model->curve_slope_mv_per_min,
               (long)model->current_ua,
               (long)model->temperature_mdegc,
               (unsigned long)model->minute_count,
               (unsigned)service.trace_count,
               (unsigned)BATTERY_CHARGE_SUPERVISOR_TRACE_CAP);
        printf("[charge] candidate=%u blockers=%08lx admitted=%u auth=%u "
               "ce=%u/%c owners=%02x control=%lu/%lu/%lu\n",
               (unsigned)model->candidate,
               (unsigned long)model->blockers,
               model->admitted ? 1u : 0u,
               model->session_authoritative ? 1u : 0u,
               board.charger_enabled_requested ? 1u : 0u,
               board.charger_enable_valid
                   ? (board.charger_enabled ? '1' : '0') : '?',
               (unsigned)board.charger_inhibit_owner_mask,
               (unsigned long)board.charger_control_attempts,
               (unsigned long)board.charger_control_failures,
               (unsigned long)board.charger_control_mismatches);
        printf("[charge] shadow=%lu match=%lu mismatch=%lu last=%u/%u "
               "events=%lu/%lu/%lu/%lu\n",
               (unsigned long)service.shadow_compares,
               (unsigned long)service.shadow_matches,
               (unsigned long)service.shadow_mismatches,
               service.shadow_last_active_match ? 1u : 0u,
               service.shadow_last_completion_match ? 1u : 0u,
               (unsigned long)service.attach_events,
               (unsigned long)service.admission_events,
               (unsigned long)service.terminal_events,
               (unsigned long)service.minute_events);
        store_diag_snapshot_t store;
        store_service_get_diag(&store);
        uint16_t unit_mask = (uint16_t)(
            1u << STORE_UNIT_BATTERY_CHARGE_SUPERVISOR);
        printf("[charge] restore=%s attempts=%lu ok=%lu fail=%lu "
               "persist=%s flash=%s pfail=%lu release=%u\n",
               service.restore_pending ? "pending" :
                   (service.restored_after_reset ? "done" : "none"),
               (unsigned long)service.restore_attempts,
               (unsigned long)service.restore_successes,
               (unsigned long)service.restore_failures,
               service.persistence_pending ? "pending" : "accepted",
               (store.dirty_mask & unit_mask) != 0u ? "dirty" : "clean",
               (unsigned long)service.persistence_failures,
               service.release_inhibit_pending ? 1u : 0u);
        printf("[charge] configured policy=%s factor=%u/1000 %s "
               "latch=%u reason=%s\n",
               charge_policy_text(service.persisted.configured_policy),
               (unsigned)service.persisted.configured_charge_factor_permille,
               service.persisted.configured_charge_factor_confident
                   ? "trusted" : "candidate",
               service.persisted.supervisor_inhibit_latched ? 1u : 0u,
               charge_terminal_text(service.persisted.latched_stop_reason));
        printf("[charge] maintenance phase=%u full=%u soc<=%u=%u "
               "v=%umV<%u=%u pending=%u rearm-used=%u "
               "restarts=%lu bq-recoveries=%lu\n",
               (unsigned)service.maintenance_phase,
               service.maintenance_full_hold ? 1u : 0u,
               (unsigned)BATTERY_CHARGE_MAINTENANCE_RESTART_SOC_PERCENT,
               service.maintenance_soc_ready ? 1u : 0u,
               (unsigned)service.maintenance_voltage_mv,
               (unsigned)BATTERY_CHARGE_MAINTENANCE_RESTART_MV,
               service.maintenance_voltage_ready ? 1u : 0u,
               service.persisted.maintenance_rearm_pending ? 1u : 0u,
               service.persisted.completion_rearm_used ? 1u : 0u,
               (unsigned long)service.maintenance_rearm_events,
               (unsigned long)service.bq_recovery_events);
        return;
    }
    if (strcmp(sub, "trace") == 0) {
        char *value = next_token(&args);
        if (value == NULL || next_token(&args) != NULL ||
            (strcmp(value, "on") != 0 && strcmp(value, "off") != 0)) {
            printf("[charge] usage: charge trace <on|off>\n");
            return;
        }
        battery_charge_supervisor_service_snapshot_t service;
        battery_charge_supervisor_service_get_snapshot(&service);
        s_charge_trace_live = strcmp(value, "on") == 0;
        s_charge_trace_seen = service.minute_events;
        printf("[charge] minute trace %s\n",
               s_charge_trace_live ? "on" : "off");
        return;
    }
    if (strcmp(sub, "csv") == 0 && next_token(&args) == NULL) {
        battery_charge_supervisor_service_snapshot_t service;
        battery_charge_supervisor_service_get_snapshot(&service);
        printf("[charge.csv] at_ms,elapsed_ms,terminal_mv,comp_mv,current_ua,temp_mdegc,net_nah\n");
        for (uint8_t i = 0u; i < service.trace_count; i++) {
            battery_charge_supervisor_minute_t minute;
            if (battery_charge_supervisor_service_get_trace(i, &minute)) {
                print_charge_csv_row(&minute);
            }
        }
        return;
    }
    if (strcmp(sub, "history") == 0 && next_token(&args) == NULL) {
        battery_charge_supervisor_service_snapshot_t service;
        battery_charge_supervisor_service_get_snapshot(&service);
        const battery_charge_supervisor_persisted_t *persisted =
            &service.persisted;
        printf("[charge.history] gen=%lu active=%u profile=%u chem=%u "
               "policy=%s factor=%u/1000 %s latch=%u/%s maint=%u "
               "rearm-used=%u\n",
               (unsigned long)persisted->charge_generation,
               persisted->active_session_valid ? 1u : 0u,
               (unsigned)persisted->hardware_profile_id,
               (unsigned)persisted->chemistry,
               charge_policy_text(persisted->configured_policy),
               (unsigned)persisted->configured_charge_factor_permille,
               persisted->configured_charge_factor_confident
                   ? "trusted" : "candidate",
               persisted->supervisor_inhibit_latched ? 1u : 0u,
               charge_terminal_text(persisted->latched_stop_reason),
               persisted->maintenance_rearm_pending ? 1u : 0u,
               persisted->completion_rearm_used ? 1u : 0u);
        if (persisted->active_session_valid) {
            printf("[charge.history] active pack=%lu gauge=%lu acr=%08lx "
                   "base=%lldnAh cap=%s%u rem=%s%llunAh/%umAh "
                   "soc=%s/%s\n",
                   (unsigned long)persisted->pack_generation,
                   (unsigned long)persisted->gauge_session,
                   (unsigned long)persisted->start_acr_raw,
                   (long long)persisted->start_session_delta_nah,
                   persisted->frozen_capacity_valid ? "" : "?",
                   (unsigned)persisted->frozen_capacity_mah,
                   persisted->frozen_remaining_valid ? "" : "?",
                   (unsigned long long)persisted->frozen_remaining_nah,
                   (unsigned)persisted->frozen_remaining_mah,
                   battery_soc_provenance_text(
                       persisted->frozen_soc_provenance),
                   battery_soc_confidence_text(
                       persisted->frozen_soc_confidence));
            printf("[charge.history] deficit=%s%llunAh/%umAh "
                   "target=%s%llunAh safety=%llunAh\n",
                   persisted->deficit_valid ? "" : "?",
                   (unsigned long long)persisted->deficit_nah,
                   (unsigned)persisted->deficit_mah,
                   persisted->target_valid ? "" : "?",
                   (unsigned long long)persisted->target_input_nah,
                   (unsigned long long)persisted->safety_input_nah);
        }
        if (persisted->last_terminal_valid) {
            printf("[charge.history] last gen=%lu reason=%s full=%u elapsed=%lums "
                   "input=%s%lldnAh v=%s%umV trace=%s peak=%u drop=%u "
                   "slope=%d\n",
                   (unsigned long)persisted->last_terminal_generation,
                   charge_terminal_text(persisted->last_terminal_reason),
                   persisted->last_terminal_full_qualified ? 1u : 0u,
                   (unsigned long)persisted->last_terminal_elapsed_ms,
                   persisted->last_terminal_net_input_valid ? "" : "?",
                   (long long)persisted->last_terminal_net_input_nah,
                   persisted->last_terminal_voltage_valid ? "" : "?",
                   (unsigned)persisted->last_terminal_mv,
                   persisted->last_terminal_trace_complete
                       ? "complete" : "partial",
                   (unsigned)persisted->last_curve_peak_mv,
                   (unsigned)persisted->last_curve_drop_mv,
                   (int)persisted->last_curve_slope_mv_per_min);
        } else {
            printf("[charge.history] last none\n");
        }
        return;
    }
    if (strcmp(sub, "configure") == 0) {
        char *policy_text = next_token(&args);
        char *factor_text = next_token(&args);
        char *confidence_text = next_token(&args);
        char *confirm = next_token(&args);
        battery_charge_supervisor_policy_t policy =
            BATTERY_CHARGE_POLICY_OBSERVE;
        bool policy_valid = policy_text != NULL;
        if (policy_valid && strcmp(policy_text, "observe") == 0) {
            policy = BATTERY_CHARGE_POLICY_OBSERVE;
        } else if (policy_valid && strcmp(policy_text, "anchored") == 0) {
            policy = BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED;
        } else if (policy_valid && strcmp(policy_text, "bootstrap") == 0) {
            policy = BATTERY_CHARGE_POLICY_ENFORCE_BOOTSTRAP;
        } else {
            policy_valid = false;
        }
        uint32_t factor = 0u;
        bool factor_confident = confidence_text != NULL &&
            strcmp(confidence_text, "trusted") == 0;
        bool confidence_valid = confidence_text != NULL &&
            (factor_confident || strcmp(confidence_text, "candidate") == 0);
        bool syntax_ok = policy_valid &&
            parse_seconds(factor_text, BATTERY_CHARGE_FACTOR_MIN_PERMILLE,
                          BATTERY_CHARGE_FACTOR_MAX_PERMILLE, &factor) &&
            confidence_valid && confirm != NULL &&
            strcmp(confirm, "confirm") == 0 && next_token(&args) == NULL;
        if (!syntax_ok) {
            printf("[charge] usage: charge configure "
                   "<observe|anchored|bootstrap> <1000-2000> "
                   "<candidate|trusted> confirm\n");
            return;
        }

        battery_charge_supervisor_config_status_t status =
            battery_charge_supervisor_service_configure(
                policy, (uint16_t)factor, factor_confident);
        if (status != BATTERY_CHARGE_CONFIG_OK) {
            static const char *const ERRORS[] = {
                "ok", "invalid parameters", "charger/session active",
                "store not ready", "store handoff failed",
            };
            uint8_t index = (uint8_t)status;
            printf("[charge] configure refused: %s\n",
                   index < sizeof(ERRORS) / sizeof(ERRORS[0])
                       ? ERRORS[index] : "unknown error");
            return;
        }

        store_diag_snapshot_t store;
        store_service_get_diag(&store);
        uint16_t unit_mask = (uint16_t)(
            UINT16_C(1) << STORE_UNIT_BATTERY_CHARGE_SUPERVISOR);
        printf("[charge] configured policy=%s factor=%u/1000 %s; "
               "journal dirty=%u (wait for 0 before power removal)\n",
               charge_policy_text(policy), (unsigned)factor,
               factor_confident ? "trusted" : "candidate",
               (store.dirty_mask & unit_mask) != 0u ? 1u : 0u);
        return;
    }
    printf("[charge] usage: charge [status|trace <on|off>|csv|history|"
           "configure ...]\n");
}

static uint16_t debug_keycal_key(char ch) {
    switch (ch) {
    case '1': return KEY_1;
    case '2': return KEY_2;
    case '3': return KEY_3;
    case '4': return KEY_4;
    case '5': return KEY_5;
    case '6': return KEY_6;
    case '7': return KEY_7;
    case '8': return KEY_8;
    case '9': return KEY_9;
    case '*': return KEY_STAR;
    case '0': return KEY_0;
    case '#': return KEY_HASH;
    default: return 0u;
    }
}

static uint8_t debug_keycal_level(void) {
    uint8_t setting = 2u; /* Level 3, matching the microphone calibration setup. */
    (void)store_setting_get_u8(STORE_SETTING_PROFILE_KEYPAD_TONES, &setting);
    return audio_level_from_keypad_tones(setting);
}

static int16_t keycal_percent_to_q12(int32_t percent) {
    int32_t scaled = percent * 4096;
    return (int16_t)((scaled >= 0 ? scaled + 50 : scaled - 50) / 100);
}

static void command_keycal(char *args) {
    char *sub = next_token(&args);
    if (sub == NULL || strcmp(sub, "status") == 0) {
        printf("[keycal] click gain=%u%% attack=%ums release=%ums; "
               "h2=%d%%@%ddeg h3=%d%%@%ddeg h4=%d%%@%ddeg h5=%d%%@%ddeg; "
               "dtmf gain=%u%% handset=%u:%u headset=%u:%u\n",
               (unsigned)s_keycal.click_gain_pct,
               (unsigned)s_keycal.click_attack_ms,
               (unsigned)s_keycal.click_release_ms,
               (int)s_keycal.click_h2_pct,
               (int)s_keycal.click_h2_phase_deg,
               (int)s_keycal.click_h3_pct,
               (int)s_keycal.click_h3_phase_deg,
               (int)s_keycal.click_h4_pct,
               (int)s_keycal.click_h4_phase_deg,
               (int)s_keycal.click_h5_pct,
               (int)s_keycal.click_h5_phase_deg,
               (unsigned)s_keycal.dtmf_gain_pct,
               (unsigned)s_keycal.dtmf_handset_low_weight,
               (unsigned)s_keycal.dtmf_handset_high_weight,
               (unsigned)s_keycal.dtmf_headset_low_weight,
               (unsigned)s_keycal.dtmf_headset_high_weight);
        return;
    }

    if (strcmp(sub, "reset") == 0 && next_token(&args) == NULL) {
        core1_post_command(CORE1_CMD_AUDIO_SET_CLICK_GAIN,
                           AUDIO_KEYPAD_CLICK_GAIN_Q8_DEFAULT);
        core1_post_command(CORE1_CMD_AUDIO_SET_CLICK_H2,
                           (uint16_t)(int16_t)AUDIO_KEYPAD_CLICK_H2_Q12_DEFAULT);
        core1_post_command(CORE1_CMD_AUDIO_SET_CLICK_H2_PHASE,
                           (uint16_t)(int16_t)AUDIO_KEYPAD_CLICK_H2_PHASE_DEG_DEFAULT);
        core1_post_command(CORE1_CMD_AUDIO_SET_CLICK_H3,
                           (uint16_t)(int16_t)AUDIO_KEYPAD_CLICK_H3_Q12_DEFAULT);
        core1_post_command(CORE1_CMD_AUDIO_SET_CLICK_H3_PHASE,
                           (uint16_t)(int16_t)AUDIO_KEYPAD_CLICK_H3_PHASE_DEG_DEFAULT);
        core1_post_command(CORE1_CMD_AUDIO_SET_CLICK_H4,
                           (uint16_t)(int16_t)AUDIO_KEYPAD_CLICK_H4_Q12_DEFAULT);
        core1_post_command(CORE1_CMD_AUDIO_SET_CLICK_H4_PHASE,
                           (uint16_t)(int16_t)AUDIO_KEYPAD_CLICK_H4_PHASE_DEG_DEFAULT);
        core1_post_command(CORE1_CMD_AUDIO_SET_CLICK_H5,
                           (uint16_t)(int16_t)AUDIO_KEYPAD_CLICK_H5_Q12_DEFAULT);
        core1_post_command(CORE1_CMD_AUDIO_SET_CLICK_H5_PHASE,
                           (uint16_t)(int16_t)AUDIO_KEYPAD_CLICK_H5_PHASE_DEG_DEFAULT);
        core1_post_command(CORE1_CMD_AUDIO_SET_CLICK_ATTACK_MS,
                           AUDIO_KEYPAD_CLICK_ATTACK_MS_DEFAULT);
        core1_post_command(CORE1_CMD_AUDIO_SET_CLICK_RELEASE_MS,
                           AUDIO_KEYPAD_CLICK_RELEASE_MS_DEFAULT);
        core1_post_command(CORE1_CMD_AUDIO_SET_DTMF_GAIN,
                           AUDIO_KEYPAD_DTMF_GAIN_Q8_DEFAULT);
        core1_post_command(CORE1_CMD_AUDIO_SET_DTMF_WEIGHTS,
                           (uint16_t)((AUDIO_KEYPAD_DTMF_HIGH_WEIGHT_DEFAULT << 8) |
                                      AUDIO_KEYPAD_DTMF_LOW_WEIGHT_DEFAULT));
        core1_post_command(CORE1_CMD_AUDIO_SET_DTMF_HEADSET_WEIGHTS,
                           (uint16_t)((AUDIO_KEYPAD_DTMF_HEADSET_HIGH_WEIGHT_DEFAULT << 8) |
                                      AUDIO_KEYPAD_DTMF_HEADSET_LOW_WEIGHT_DEFAULT));
        s_keycal.click_gain_pct = AUDIO_KEYPAD_CLICK_GAIN_PERCENT_DEFAULT;
        s_keycal.click_h2_pct = AUDIO_KEYPAD_CLICK_H2_PERCENT_DEFAULT;
        s_keycal.click_h2_phase_deg = AUDIO_KEYPAD_CLICK_H2_PHASE_DEG_DEFAULT;
        s_keycal.click_h3_pct = AUDIO_KEYPAD_CLICK_H3_PERCENT_DEFAULT;
        s_keycal.click_h3_phase_deg = AUDIO_KEYPAD_CLICK_H3_PHASE_DEG_DEFAULT;
        s_keycal.click_h4_pct = AUDIO_KEYPAD_CLICK_H4_PERCENT_DEFAULT;
        s_keycal.click_h4_phase_deg = AUDIO_KEYPAD_CLICK_H4_PHASE_DEG_DEFAULT;
        s_keycal.click_h5_pct = AUDIO_KEYPAD_CLICK_H5_PERCENT_DEFAULT;
        s_keycal.click_h5_phase_deg = AUDIO_KEYPAD_CLICK_H5_PHASE_DEG_DEFAULT;
        s_keycal.click_attack_ms = AUDIO_KEYPAD_CLICK_ATTACK_MS_DEFAULT;
        s_keycal.click_release_ms = AUDIO_KEYPAD_CLICK_RELEASE_MS_DEFAULT;
        s_keycal.dtmf_gain_pct = AUDIO_KEYPAD_DTMF_GAIN_PERCENT_DEFAULT;
        s_keycal.dtmf_handset_low_weight = AUDIO_KEYPAD_DTMF_LOW_WEIGHT_DEFAULT;
        s_keycal.dtmf_handset_high_weight = AUDIO_KEYPAD_DTMF_HIGH_WEIGHT_DEFAULT;
        s_keycal.dtmf_headset_low_weight = AUDIO_KEYPAD_DTMF_HEADSET_LOW_WEIGHT_DEFAULT;
        s_keycal.dtmf_headset_high_weight = AUDIO_KEYPAD_DTMF_HEADSET_HIGH_WEIGHT_DEFAULT;
        printf("[keycal] reset to measured production defaults\n");
        return;
    }

    if (strcmp(sub, "click") == 0) {
        char *gain_text = next_token(&args);
        char *h3_text = next_token(&args);
        char *phase_text = next_token(&args);
        char *attack_text = next_token(&args);
        char *release_text = next_token(&args);
        uint32_t gain_pct;
        uint32_t attack_ms;
        uint32_t release_ms;
        int32_t h3_pct;
        int32_t phase_deg;
        if (!parse_seconds(gain_text, 0u, 200u, &gain_pct) ||
            !parse_signed_range(h3_text, -50, 50, &h3_pct) ||
            !parse_signed_range(phase_text, -180, 180, &phase_deg) ||
            !parse_seconds(attack_text, 0u, 20u, &attack_ms) ||
            !parse_seconds(release_text, 0u, 93u, &release_ms) ||
            next_token(&args) != NULL) {
            printf("[keycal] usage: keycal click <gain 0-200%%> <h3 -50..50%%> <phase -180..180deg> <attack 0-20ms> <release 0-93ms>\n");
            return;
        }
        uint16_t gain_q8 = (uint16_t)((gain_pct * 256u + 50u) / 100u);
        int16_t h3_q12 = keycal_percent_to_q12(h3_pct);
        core1_post_command(CORE1_CMD_AUDIO_SET_CLICK_GAIN, gain_q8);
        core1_post_command(CORE1_CMD_AUDIO_SET_CLICK_H3, (uint16_t)h3_q12);
        core1_post_command(CORE1_CMD_AUDIO_SET_CLICK_H3_PHASE,
                           (uint16_t)(int16_t)phase_deg);
        core1_post_command(CORE1_CMD_AUDIO_SET_CLICK_ATTACK_MS,
                           (uint16_t)attack_ms);
        core1_post_command(CORE1_CMD_AUDIO_SET_CLICK_RELEASE_MS,
                           (uint16_t)release_ms);
        s_keycal.click_gain_pct = (uint16_t)gain_pct;
        s_keycal.click_h3_pct = (int16_t)h3_pct;
        s_keycal.click_h3_phase_deg = (int16_t)phase_deg;
        s_keycal.click_attack_ms = (uint16_t)attack_ms;
        s_keycal.click_release_ms = (uint16_t)release_ms;
        printf("[keycal] click gain=%u%% h3=%ld%% phase=%lddeg attack=%lums release=%lums\n",
               (unsigned)gain_pct, (long)h3_pct, (long)phase_deg,
               (unsigned long)attack_ms,
               (unsigned long)release_ms);
        return;
    }

    if (strcmp(sub, "harmonic") == 0) {
        char *number_text = next_token(&args);
        char *percent_text = next_token(&args);
        char *phase_text = next_token(&args);
        uint32_t number;
        int32_t percent;
        int32_t phase_deg;
        if (!parse_seconds(number_text, 2u, 5u, &number) ||
            !parse_signed_range(percent_text, -50, 50, &percent) ||
            !parse_signed_range(phase_text, -180, 180, &phase_deg) ||
            next_token(&args) != NULL) {
            printf("[keycal] usage: keycal harmonic <2|3|4|5> <percent -50..50> <phase -180..180deg>\n");
            return;
        }
        core1_cmd_t amplitude_cmd;
        core1_cmd_t phase_cmd;
        int16_t *saved_percent;
        int16_t *saved_phase;
        switch (number) {
        case 2u:
            amplitude_cmd = CORE1_CMD_AUDIO_SET_CLICK_H2;
            phase_cmd = CORE1_CMD_AUDIO_SET_CLICK_H2_PHASE;
            saved_percent = &s_keycal.click_h2_pct;
            saved_phase = &s_keycal.click_h2_phase_deg;
            break;
        case 3u:
            amplitude_cmd = CORE1_CMD_AUDIO_SET_CLICK_H3;
            phase_cmd = CORE1_CMD_AUDIO_SET_CLICK_H3_PHASE;
            saved_percent = &s_keycal.click_h3_pct;
            saved_phase = &s_keycal.click_h3_phase_deg;
            break;
        case 4u:
            amplitude_cmd = CORE1_CMD_AUDIO_SET_CLICK_H4;
            phase_cmd = CORE1_CMD_AUDIO_SET_CLICK_H4_PHASE;
            saved_percent = &s_keycal.click_h4_pct;
            saved_phase = &s_keycal.click_h4_phase_deg;
            break;
        default:
            amplitude_cmd = CORE1_CMD_AUDIO_SET_CLICK_H5;
            phase_cmd = CORE1_CMD_AUDIO_SET_CLICK_H5_PHASE;
            saved_percent = &s_keycal.click_h5_pct;
            saved_phase = &s_keycal.click_h5_phase_deg;
            break;
        }
        core1_post_command(amplitude_cmd,
                           (uint16_t)keycal_percent_to_q12(percent));
        core1_post_command(phase_cmd, (uint16_t)(int16_t)phase_deg);
        *saved_percent = (int16_t)percent;
        *saved_phase = (int16_t)phase_deg;
        printf("[keycal] h%lu=%ld%% phase=%lddeg\n",
               (unsigned long)number, (long)percent, (long)phase_deg);
        return;
    }

    if (strcmp(sub, "dtmf") == 0) {
        char *gain_text = next_token(&args);
        char *low_text = next_token(&args);
        char *high_text = next_token(&args);
        uint32_t gain_pct;
        uint32_t low;
        uint32_t high;
        if (!parse_seconds(gain_text, 0u, 200u, &gain_pct) ||
            !parse_seconds(low_text, 0u, 255u, &low) ||
            !parse_seconds(high_text, 0u, 255u, &high) ||
            (low == 0u && high == 0u) || next_token(&args) != NULL) {
            printf("[keycal] usage: keycal dtmf <gain 0-200%%> <low 0-255> <high 0-255>; weights cannot both be zero\n");
            return;
        }
        uint16_t gain_q8 = (uint16_t)((gain_pct * 256u + 50u) / 100u);
        bool headset = audio_bridge_route() == (uint8_t)NAU_ROUTE_HEADSET;
        core1_post_command(CORE1_CMD_AUDIO_SET_DTMF_GAIN, gain_q8);
        core1_post_command(headset ? CORE1_CMD_AUDIO_SET_DTMF_HEADSET_WEIGHTS
                                   : CORE1_CMD_AUDIO_SET_DTMF_WEIGHTS,
                           (uint16_t)((high << 8) | low));
        s_keycal.dtmf_gain_pct = (uint16_t)gain_pct;
        if (headset) {
            s_keycal.dtmf_headset_low_weight = (uint8_t)low;
            s_keycal.dtmf_headset_high_weight = (uint8_t)high;
        } else {
            s_keycal.dtmf_handset_low_weight = (uint8_t)low;
            s_keycal.dtmf_handset_high_weight = (uint8_t)high;
        }
        printf("[keycal] dtmf route=%s gain=%u%% weights=%u:%u\n",
               headset ? "headset" : "handset",
               (unsigned)gain_pct, (unsigned)low, (unsigned)high);
        return;
    }

    if (strcmp(sub, "play") == 0) {
        char *tone = next_token(&args);
        char *duration_text = next_token(&args);
        uint8_t level = debug_keycal_level();
        if (tone == NULL || level == AUDIO_LEVEL_SILENT) {
            printf("[keycal] play needs <click|0..9|*|#> and a non-silent keypad-tone setting\n");
            return;
        }
        if (strcmp(tone, "click") == 0) {
            if (duration_text != NULL || next_token(&args) != NULL) {
                printf("[keycal] usage: keycal play click\n");
                return;
            }
            core1_post_command(CORE1_CMD_AUDIO_CLICK, audio_arg(0u, level));
            printf("[keycal] click level=%u\n", (unsigned)level);
            return;
        }
        uint16_t key = tone[0] != '\0' && tone[1] == '\0'
            ? debug_keycal_key(tone[0])
            : 0u;
        uint32_t duration_ms = AUDIO_KEYPAD_DTMF_MAX_HOLD_MS;
        if (key == 0u ||
            (duration_text != NULL &&
             !parse_seconds(duration_text, 100u,
                            AUDIO_KEYPAD_DTMF_MAX_HOLD_MS, &duration_ms)) ||
            next_token(&args) != NULL) {
            printf("[keycal] usage: keycal play <0..9|*|#> [100-%ums]\n",
                   (unsigned)AUDIO_KEYPAD_DTMF_MAX_HOLD_MS);
            return;
        }
        if (s_keycal.stop_pending) {
            core1_post_command(CORE1_CMD_AUDIO_STOP, s_keycal.stop_arg);
        }
        uint16_t arg = audio_arg_for_key(key, level);
        core1_post_command(CORE1_CMD_AUDIO_DTMF, arg);
        s_keycal.stop_pending = true;
        s_keycal.stop_arg = audio_arg_for_key(key, AUDIO_LEVEL_SILENT);
        s_keycal.stop_at_ms = time_ms() + duration_ms;
        printf("[keycal] dtmf=%c level=%u duration=%lums\n",
               tone[0], (unsigned)level, (unsigned long)duration_ms);
        return;
    }

    if (strcmp(sub, "stop") == 0 && next_token(&args) == NULL) {
        core1_post_command(CORE1_CMD_AUDIO_STOP,
                           s_keycal.stop_pending ? s_keycal.stop_arg : 0u);
        s_keycal.stop_pending = false;
        printf("[keycal] stopped\n");
        return;
    }

    printf("[keycal] usage: keycal [status|reset|click|dtmf|play|stop]\n");
}

static void command_buzzcal(char *args) {
    char *sub = next_token(&args);
    if (sub == NULL || strcmp(sub, "status") == 0) {
        if (next_token(&args) != NULL) {
            printf("[buzzcal] usage: buzzcal status\n");
        } else if (s_buzzcal_duty_percent == UINT8_MAX) {
            printf("[buzzcal] duty=auto\n");
        } else {
            printf("[buzzcal] duty=%u%%\n",
                   (unsigned)s_buzzcal_duty_percent);
        }
        return;
    }

    if (strcmp(sub, "duty") == 0) {
        char *value_text = next_token(&args);
        if (value_text != NULL && strcmp(value_text, "auto") == 0 &&
            next_token(&args) == NULL) {
            s_buzzcal_duty_percent = UINT8_MAX;
            core1_post_command(CORE1_CMD_AUDIO_SET_BUZZER_DUTY, UINT16_MAX);
            printf("[buzzcal] duty=auto\n");
            return;
        }
        uint32_t percent;
        if (!parse_seconds(value_text, 1u, 50u, &percent) ||
            next_token(&args) != NULL) {
            printf("[buzzcal] usage: buzzcal duty <1-50|auto>\n");
            return;
        }
        s_buzzcal_duty_percent = (uint8_t)percent;
        core1_post_command(CORE1_CMD_AUDIO_SET_BUZZER_DUTY,
                           (uint16_t)percent);
        printf("[buzzcal] duty=%u%%\n", (unsigned)percent);
        return;
    }

    if (strcmp(sub, "play") == 0) {
        char *level_text = next_token(&args);
        uint32_t level = AUDIO_LEVEL_MAX;
        if ((level_text != NULL && !parse_seconds(level_text, 1u,
                                                   AUDIO_LEVEL_MAX, &level)) ||
            next_token(&args) != NULL) {
            printf("[buzzcal] usage: buzzcal play [1-5]\n");
            return;
        }
        core1_post_command(CORE1_CMD_AUDIO_DEBUG_BUZZER_TEST,
                           audio_arg(6u, (uint8_t)level));
        printf("[buzzcal] playing Nokia tune level=%u\n", (unsigned)level);
        return;
    }

    if (strcmp(sub, "stop") == 0 && next_token(&args) == NULL) {
        core1_post_command(CORE1_CMD_AUDIO_DEBUG_STOP, 0u);
        printf("[buzzcal] stopped\n");
        return;
    }

    printf("[buzzcal] usage: buzzcal [status|duty <1-50|auto>|play [1-5]|stop]\n");
}

static void poll_keycal(uint32_t now_ms) {
    if (s_keycal.stop_pending &&
        time_diff_ms(now_ms, s_keycal.stop_at_ms) >= 0) {
        core1_post_command(CORE1_CMD_AUDIO_STOP, s_keycal.stop_arg);
        s_keycal.stop_pending = false;
        printf("[keycal] timed playback stopped\n");
    }
}

static void command_ltc_window(char *args) {
    char *delay_text = next_token(&args);
    uint32_t now_ms = time_ms();

    if (delay_text == NULL) {
        if (s_ltc_window.armed) {
            int32_t remaining_ms = time_diff_ms(s_ltc_window.start_at_ms,
                                                now_ms);
            if (remaining_ms < 0) {
                remaining_ms = 0;
            }
            printf("[ltcwin] armed start_in=%ldms duration=%lums\n",
                   (long)remaining_ms,
                   (unsigned long)s_ltc_window.duration_ms);
        } else if (s_ltc_window.running) {
            uint32_t elapsed_ms = now_ms - s_ltc_window.started_ms;
            uint32_t remaining_ms = elapsed_ms < s_ltc_window.duration_ms
                                        ? s_ltc_window.duration_ms - elapsed_ms
                                        : 0u;
            printf("[ltcwin] running elapsed=%lums remaining=%lums\n",
                   (unsigned long)elapsed_ms, (unsigned long)remaining_ms);
        } else if (s_ltc_window.complete) {
            printf("[ltcwin] complete valid=%u elapsed=%lums dQ=%lldnAh "
                   "avg=%lduA session=%lu\n",
                   s_ltc_window.valid ? 1u : 0u,
                   (unsigned long)s_ltc_window.elapsed_ms,
                   (long long)s_ltc_window.delta_nah,
                   (long)s_ltc_window.average_ua,
                   (unsigned long)s_ltc_window.gauge_session);
            printf("[ltcwin] modem ready=%lums dtr=%lums cts_sleep=%lums entries=%lu wakes=%lu\n",
                   (unsigned long)s_ltc_window.modem_ready_ms,
                   (unsigned long)s_ltc_window.modem_requested_ms,
                   (unsigned long)s_ltc_window.modem_confirmed_ms,
                   (unsigned long)s_ltc_window.modem_sleep_entries,
                   (unsigned long)s_ltc_window.modem_wake_attempts);
        } else {
            printf("[ltcwin] idle\n");
        }
        return;
    }

    if (strcmp(delay_text, "cancel") == 0 && next_token(&args) == NULL) {
        memset(&s_ltc_window, 0, sizeof(s_ltc_window));
        printf("[ltcwin] cancelled\n");
        return;
    }

    char *duration_text = next_token(&args);
    uint32_t delay_s = 0u;
    uint32_t duration_s = 0u;
    if (!parse_seconds(delay_text, 0u, DEBUG_LTC_WINDOW_MAX_DELAY_S,
                       &delay_s) ||
        !parse_seconds(duration_text, 1u, DEBUG_LTC_WINDOW_MAX_DURATION_S,
                       &duration_s) ||
        next_token(&args) != NULL) {
        printf("[ltcwin] usage: ltcwin <delay_s 0-60> <duration_s 1-3600>\n");
        return;
    }

    memset(&s_ltc_window, 0, sizeof(s_ltc_window));
    s_ltc_window.armed = true;
    s_ltc_window.start_at_ms = now_ms + delay_s * 1000u;
    s_ltc_window.duration_ms = duration_s * 1000u;
    printf("[ltcwin] armed delay=%lus duration=%lus\n",
           (unsigned long)delay_s, (unsigned long)duration_s);
}

static void command_dormant(char *args) {
    char *arg = next_token(&args);
    if (arg != NULL) {
        if (strcmp(arg, "on") == 0 && next_token(&args) == NULL) {
            standby_sleep_set_enabled(true);
        } else if (strcmp(arg, "off") == 0 && next_token(&args) == NULL) {
            standby_sleep_set_enabled(false);
        } else {
            printf("[dormant] usage: dormant [on|off]\n");
            return;
        }
    }

    standby_sleep_diag_t diag;
    standby_sleep_get_diag(&diag);
    uint64_t now_ms = time_ms64();
    uint64_t next_in_ms = diag.next_maintenance_ms > now_ms
                              ? diag.next_maintenance_ms - now_ms
                              : 0u;
    printf("[dormant] enabled=%u tracking=%u blockers=%04lx attempts=%lu entries=%lu\n",
           diag.enabled ? 1u : 0u,
           diag.eligibility_tracking ? 1u : 0u,
           (unsigned long)diag.last_blockers,
           (unsigned long)diag.attempts,
           (unsigned long)diag.entries);
    printf("[dormant] abort prearm=%lu final=%lu core1=%lu lastwake=%02lx\n",
           (unsigned long)diag.prearm_aborts,
           (unsigned long)diag.final_recheck_aborts,
           (unsigned long)diag.core1_pause_failures,
           (unsigned long)diag.last_wake_mask);
    printf("[dormant] wake ri=%lu shared=%lu power=%lu vbus=%lu maint=%lu app=%lu unknown=%lu\n",
           (unsigned long)diag.wake_modem_ri,
           (unsigned long)diag.wake_shared_irq,
           (unsigned long)diag.wake_power_button,
           (unsigned long)diag.wake_service_vbus,
           (unsigned long)diag.wake_maintenance,
           (unsigned long)diag.wake_app_timer,
           (unsigned long)diag.wake_unknown);
    printf("[dormant] last=%llums total=%llums max=%llums next=%llums offset=%llums\n",
           (unsigned long long)diag.last_sleep_ms,
           (unsigned long long)diag.total_sleep_ms,
           (unsigned long long)diag.max_sleep_ms,
           (unsigned long long)next_in_ms,
           (unsigned long long)timebase_offset_ms());
    printf("[dormant] unplug sessions=%lu samples=%lu ready=%lu union=%04lx last=%04lx\n",
           (unsigned long)diag.unplug_sessions,
           (unsigned long)diag.unplug_samples,
           (unsigned long)diag.unplug_ready_samples,
           (unsigned long)diag.unplug_blocker_union,
           (unsigned long)diag.unplug_last_blockers);
    printf("[dormant] block r=%lu bl=%lu app=%lu au=%lu c1=%lu st=%lu mdm=%lu uart=%lu usb=%lu lvl=%lu lat=%lu clk=%lu acc=%lu bat=%lu\n",
           (unsigned long)diag.unplug_blocker_samples[0],
           (unsigned long)diag.unplug_blocker_samples[1],
           (unsigned long)diag.unplug_blocker_samples[2],
           (unsigned long)diag.unplug_blocker_samples[3],
           (unsigned long)diag.unplug_blocker_samples[4],
           (unsigned long)diag.unplug_blocker_samples[5],
           (unsigned long)diag.unplug_blocker_samples[6],
           (unsigned long)diag.unplug_blocker_samples[7],
           (unsigned long)diag.unplug_blocker_samples[8],
           (unsigned long)diag.unplug_blocker_samples[9],
           (unsigned long)diag.unplug_blocker_samples[10],
           (unsigned long)diag.unplug_blocker_samples[11],
           (unsigned long)diag.unplug_blocker_samples[12],
           (unsigned long)diag.unplug_blocker_samples[13]);
}

static void poll_ltc_window(uint32_t now_ms) {
    if (s_ltc_window.armed &&
        time_diff_ms(now_ms, s_ltc_window.start_at_ms) >= 0) {
        modem_service_get_diag_snapshot(&s_modem_diag_scratch);
        netmon_diag_service_reset_measurement_window(now_ms);
        /* Publish the fresh baseline immediately. Otherwise the ordinary
         * NetMonitor poll can establish it a few milliseconds later and make
         * an otherwise valid fixed-duration window look short. */
        netmon_diag_service_poll(now_ms);
        netmon_diag_service_get_snapshot(&s_local_diag_scratch);
        s_ltc_window.armed = false;
        s_ltc_window.running = true;
        s_ltc_window.started_ms = now_ms;
        s_ltc_window.gauge_session =
            s_local_diag_scratch.board.ltc_gauge_session;
        s_ltc_window.measurement_generation =
            s_local_diag_scratch.measurement_generation;
        s_ltc_window.modem_ready_start_ms =
            s_modem_diag_scratch.transport.ready_ms;
        s_ltc_window.modem_requested_start_ms =
            s_modem_diag_scratch.transport.sleep_requested_ms;
        s_ltc_window.modem_confirmed_start_ms =
            s_modem_diag_scratch.transport.sleep_confirmed_ms;
        s_ltc_window.modem_sleep_entries_start =
            s_modem_diag_scratch.transport.sleep_entries;
        s_ltc_window.modem_wake_attempts_start =
            s_modem_diag_scratch.transport.wake_attempts;
    }

    if (!s_ltc_window.running ||
        time_diff_ms(now_ms,
                     s_ltc_window.started_ms + s_ltc_window.duration_ms) < 0) {
        return;
    }

    netmon_diag_service_poll(now_ms);
    netmon_diag_service_get_snapshot(&s_local_diag_scratch);
    modem_service_get_diag_snapshot(&s_modem_diag_scratch);
    bool baseline_unchanged =
        s_local_diag_scratch.board.ltc_gauge_session ==
            s_ltc_window.gauge_session &&
        s_local_diag_scratch.measurement_generation ==
            s_ltc_window.measurement_generation;
    /* NetMonitor snapshots are intentionally rate-limited to 250 ms. The
     * window deadline can land between samples; wait for the first snapshot
     * covering the full interval instead of publishing a false invalid. A
     * changed baseline is terminal immediately because waiting cannot repair
     * continuity. */
    if (baseline_unchanged &&
        s_local_diag_scratch.measurement_elapsed_ms <
            s_ltc_window.duration_ms) {
        return;
    }
    s_ltc_window.running = false;
    s_ltc_window.complete = true;
    s_ltc_window.elapsed_ms = s_local_diag_scratch.measurement_elapsed_ms;
    s_ltc_window.valid = s_local_diag_scratch.board.ltc_sample_valid &&
        s_local_diag_scratch.board.ltc_continuity_valid &&
        baseline_unchanged &&
        s_local_diag_scratch.measurement_elapsed_ms >=
            s_ltc_window.duration_ms;
    if (s_ltc_window.valid) {
        s_ltc_window.delta_nah = s_local_diag_scratch.measurement_delta_nah;
        s_ltc_window.average_ua =
            s_local_diag_scratch.measurement_average_ua;
    }
    s_ltc_window.modem_ready_ms =
        s_modem_diag_scratch.transport.ready_ms -
        s_ltc_window.modem_ready_start_ms;
    s_ltc_window.modem_requested_ms =
        s_modem_diag_scratch.transport.sleep_requested_ms -
        s_ltc_window.modem_requested_start_ms;
    s_ltc_window.modem_confirmed_ms =
        s_modem_diag_scratch.transport.sleep_confirmed_ms -
        s_ltc_window.modem_confirmed_start_ms;
    s_ltc_window.modem_sleep_entries =
        s_modem_diag_scratch.transport.sleep_entries -
        s_ltc_window.modem_sleep_entries_start;
    s_ltc_window.modem_wake_attempts =
        s_modem_diag_scratch.transport.wake_attempts -
        s_ltc_window.modem_wake_attempts_start;
}

static bool parse_lcd_value(const char *text, unsigned maximum, uint8_t *value_out) {
    if (text == NULL || text[0] == '\0' || value_out == NULL) {
        return false;
    }
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 0);
    if (end == text || *end != '\0' || value > maximum) {
        return false;
    }
    *value_out = (uint8_t)value;
    return true;
}

static void command_lcd(char *args) {
    char *op = next_token(&args);
    uint8_t vop = 0u;
    uint8_t temperature_coefficient = 0u;
    uint8_t bias_system = 0u;
    lcd_get_tuning(&vop, &temperature_coefficient, &bias_system);

    bool applied = true;
    bool save_requested = false;
    bool saved = false;
    if (op == NULL || strcmp(op, "status") == 0) {
        /* Query only. */
    } else if (strcmp(op, "cal") == 0 ||
               strcmp(op, "calibration") == 0) {
        uint8_t level = 0u;
        char *value_text = next_token(&args);
        if (!parse_lcd_value(value_text, LCD_CALIBRATION_LEVEL_MAX, &level) ||
            next_token(&args) != NULL) {
            printf("[lcd] usage: lcd cal <0-31>\n");
            return;
        }
        applied = lcd_calibration_preview(level);
    } else if (strcmp(op, "stock") == 0 && next_token(&args) == NULL) {
        applied = lcd_calibration_preview(LCD_CALIBRATION_LEVEL_DEFAULT);
    } else if (strcmp(op, "save") == 0 && next_token(&args) == NULL) {
        save_requested = true;
        saved = lcd_calibration_save_active();
        applied = saved;
    } else if (strcmp(op, "set") == 0) {
        char *vop_text = next_token(&args);
        char *temperature_text = next_token(&args);
        char *bias_text = next_token(&args);
        if (!parse_lcd_value(vop_text, LCD_PCD8544_VOP_MAX, &vop) ||
            !parse_lcd_value(temperature_text,
                             LCD_PCD8544_TEMP_COEFFICIENT_MAX,
                             &temperature_coefficient) ||
            !parse_lcd_value(bias_text, LCD_PCD8544_BIAS_SYSTEM_MAX,
                             &bias_system) ||
            next_token(&args) != NULL) {
            printf("[lcd] usage: lcd set <vop 0-127> <tc 0-3> <bias 0-7>\n");
            return;
        }
        applied = lcd_set_tuning(vop, temperature_coefficient, bias_system);
    } else if (strcmp(op, "contrast") == 0) {
        char *value_text = next_token(&args);
        if (!parse_lcd_value(value_text, LCD_PCD8544_VOP_MAX, &vop) ||
            next_token(&args) != NULL) {
            printf("[lcd] usage: lcd contrast <0-127>\n");
            return;
        }
        applied = lcd_set_tuning(vop, temperature_coefficient, bias_system);
    } else if (strcmp(op, "temp") == 0) {
        char *value_text = next_token(&args);
        if (!parse_lcd_value(value_text,
                             LCD_PCD8544_TEMP_COEFFICIENT_MAX,
                             &temperature_coefficient) ||
            next_token(&args) != NULL) {
            printf("[lcd] usage: lcd temp <0-3>\n");
            return;
        }
        applied = lcd_set_tuning(vop, temperature_coefficient, bias_system);
    } else if (strcmp(op, "bias") == 0) {
        char *value_text = next_token(&args);
        if (!parse_lcd_value(value_text, LCD_PCD8544_BIAS_SYSTEM_MAX,
                             &bias_system) ||
            next_token(&args) != NULL) {
            printf("[lcd] usage: lcd bias <0-7>\n");
            return;
        }
        applied = lcd_set_tuning(vop, temperature_coefficient, bias_system);
    } else if (strcmp(op, "reinit") == 0 && next_token(&args) == NULL) {
        applied = lcd_reapply_configuration();
    } else {
        printf("[lcd] usage: lcd [status|cal <0-31>|save|stock|set <vop> <tc> <bias>|contrast <0-127>|temp <0-3>|bias <0-7>|reinit]\n");
        return;
    }

    lcd_calibration_status_t calibration;
    lcd_calibration_get_status(&calibration);
    printf("[lcd] vop=%u (0x%02x) stored=",
           (unsigned)calibration.vop, (unsigned)calibration.vop);
    if (calibration.stored_vop_valid) {
        printf("%u", (unsigned)calibration.stored_vop);
    } else {
        printf("invalid");
    }
    printf(" nokia=");
    if (calibration.nokia_level_valid) {
        printf("%u", (unsigned)calibration.nokia_level);
    } else {
        printf("outside");
    }
    printf(" tc=%u bias=%u power=%s applied=%u",
           (unsigned)calibration.temperature_coefficient,
           (unsigned)calibration.bias_system,
           lcd_is_powered_down() ? "down" : "on",
           applied ? 1u : 0u);
    if (save_requested) {
        printf(" save=%s", saved ? "queued" : "rejected");
    } else {
        bool matches_stored =
            calibration.stored_vop_valid &&
            calibration.vop == calibration.stored_vop &&
            calibration.temperature_coefficient ==
                LCD_CALIBRATION_STOCK_TEMPERATURE_COEFFICIENT &&
            calibration.bias_system == LCD_CALIBRATION_STOCK_BIAS_SYSTEM;
        printf(" source=%s", matches_stored ? "stored" : "preview");
    }
    printf("\n");
}

static void command_status(void) {
    static modem_status_t status;
    modem_service_get_status(&status);
    char board_imei[STORE_WARRANTY_SERIAL_MAX + 1u] = "";
    (void)store_board_imei_get(board_imei, sizeof(board_imei));
    static modem_diag_snapshot_t diag;
    modem_service_get_diag_snapshot(&diag);
    static audio_i2s_stats_t audio_stats;
    audio_i2s_hal_get_stats(&audio_stats);
    static modem_i2s_stats_t modem_stats;
    modem_i2s_hal_get_stats(&modem_stats);
    const modem_message_waiting_state_t *mwi_v1 =
        &status.message_waiting
             .category[MODEM_MESSAGE_WAITING_VOICE_LINE_1];
    const modem_message_waiting_state_t *mwi_v2 =
        &status.message_waiting
             .category[MODEM_MESSAGE_WAITING_VOICE_LINE_2];
    const modem_message_waiting_state_t *mwi_fax =
        &status.message_waiting.category[MODEM_MESSAGE_WAITING_FAX];
    const modem_message_waiting_state_t *mwi_email =
        &status.message_waiting.category[MODEM_MESSAGE_WAITING_EMAIL];
    printf("[debug] at=%u sim=%u/%u/%u prov=%u/v%u reg=%u cereg=%u rssi=%u ber=%u busy=%u call=%u result=%u ring=%u wait=%u mwi=v1:%u/%u,v2:%u/%u,f:%u/%u,e:%u/%u incoming='%s' op='%s' imei='%s' sms_tx=%lu sms_rx=%lu err=%lu\n",
           status.at_ready ? 1u : 0u,
           status.sim_checked ? 1u : 0u,
           status.sim_present ? 1u : 0u,
           status.sim_ready ? 1u : 0u,
           status.provisioning_verified ? 1u : 0u,
           (unsigned)status.provisioning_schema_version,
           status.network_registered ? 1u : 0u,
           (unsigned)status.cereg,
           (unsigned)status.rssi,
           (unsigned)status.ber,
           status.operation_busy ? 1u : 0u,
           (unsigned)status.call_state,
           (unsigned)status.last_call_result,
           status.ring_active ? 1u : 0u,
           status.waiting_call ? 1u : 0u,
           mwi_v1->active ? 1u : 0u,
           (unsigned)mwi_v1->count,
           mwi_v2->active ? 1u : 0u,
           (unsigned)mwi_v2->count,
           mwi_fax->active ? 1u : 0u,
           (unsigned)mwi_fax->count,
           mwi_email->active ? 1u : 0u,
           (unsigned)mwi_email->count,
           status.incoming_number,
           status.operator_name,
           board_imei,
           (unsigned long)status.sms_sent_count,
           (unsigned long)status.sms_received_count,
           (unsigned long)status.command_errors);
    const char *operator_source =
        status.operator_name_source == MODEM_OPERATOR_NAME_DATABASE ? "db" :
        status.operator_name_source == MODEM_OPERATOR_NAME_SIM ? "sim" :
        status.operator_name_source == MODEM_OPERATOR_NAME_PLMN ? "plmn" : "none";
    bool plmn_valid = (status.signal.valid_fields & MODEM_SIGNAL_VALID_PLMN) != 0u;
    printf("[debug] operator source=%s plmn=%s/%s\n", operator_source,
           plmn_valid ? status.signal.mcc : "--",
           plmn_valid ? status.signal.mnc : "--");
    printf("[debug] signal seq=%lu rat=%u valid=%02x channel=%lu cell=%08lx rssi=%d rsrp=%d rsrq_x2=%d sinr_x10=%d age=%lums\n",
           (unsigned long)status.signal.sequence,
           (unsigned)status.signal.rat,
           (unsigned)status.signal.valid_fields,
           (unsigned long)status.signal.channel,
           (unsigned long)status.signal.cell_id,
           (int)status.signal.rssi_dbm,
           (int)status.signal.rsrp_dbm,
           (int)status.signal.rsrq_db_x2,
           (int)status.signal.sinr_db_x10,
           (unsigned long)(time_ms() - status.signal.updated_ms));
    printf("[debug] modem state=%u kind=%u init=%u retry=%u last_cmd='%s' last_line='%s'\n",
           (unsigned)status.debug_state,
           (unsigned)status.debug_active_kind,
           (unsigned)status.debug_init_index,
           (unsigned)status.debug_init_retries,
           status.debug_last_command,
           status.debug_last_line);
    printf("[debug] modem_uart rx_dropped=%lu rx_overruns=%lu rx_line_errors=%lu flow_pauses=%lu tx_stall_drops=%lu\n",
           (unsigned long)status.rx_dropped,
           (unsigned long)status.rx_overruns,
           (unsigned long)status.rx_line_errors,
           (unsigned long)modem_uart_hal_rx_flow_pauses(),
           (unsigned long)status.tx_stall_drops);
    printf("[debug] transport dtr=%u pend=%u/%u cts=%u ri=%u sleep=%lu wake=%lu timeout=%lu/%lu latency=%lu/%lu\n",
           diag.transport.dtr_sleep_permitted ? 1u : 0u,
           diag.transport.dtr_wake_pending ? 1u : 0u,
           diag.transport.ri_release_pending ? 1u : 0u,
           diag.transport.cts_asserted ? 1u : 0u,
           diag.transport.ri_asserted ? 1u : 0u,
           (unsigned long)diag.transport.sleep_entries,
           (unsigned long)diag.transport.wake_attempts,
           (unsigned long)diag.transport.wake_timeouts,
           (unsigned long)diag.transport.ri_release_timeouts,
           (unsigned long)diag.transport.last_wake_latency_ms,
           (unsigned long)diag.transport.max_wake_latency_ms);
    printf("[debug] transport residency ready=%lums requested=%lums confirmed=%lums\n",
           (unsigned long)diag.transport.ready_ms,
           (unsigned long)diag.transport.sleep_requested_ms,
           (unsigned long)diag.transport.sleep_confirmed_ms);
    printf("[debug] modem background_polling=%u\n",
           diag.scheduler.background_polling_enabled ? 1u : 0u);
    printf("[debug] audio tx_irq=%lu rx_irq=%lu rx_last=(%d,%d) rx_peak=(%d,%d) abort_to=%lu\n",
           (unsigned long)audio_stats.tx_irq_count,
           (unsigned long)audio_stats.rx_irq_count,
           (int)audio_stats.last_left,
           (int)audio_stats.last_right,
           (int)audio_stats.peak_left,
           (int)audio_stats.peak_right,
           (unsigned long)audio_stats.abort_timeouts);
    printf("[debug] modem_i2s rx_irq=%lu rx_last=(%d,%d) slotmiss=%lu relock=%lu/%lu abort_to=%lu\n",
           (unsigned long)modem_stats.rx_irq_count,
           (int)modem_stats.last_wa_low,
           (int)modem_stats.last_wa_high,
           (unsigned long)modem_stats.last_slot_mismatch,
           (unsigned long)modem_stats.relock_attempts,
           (unsigned long)modem_stats.relock_total,
           (unsigned long)modem_stats.abort_timeouts);
    static audio_bridge_stats_t bstats;
    audio_bridge_get_stats(&bstats);
    printf("[debug] bridge active=%u dl(depth=%u under=%lu over=%lu) ul(depth=%u under=%lu over=%lu)\n",
           bstats.active ? 1u : 0u,
           (unsigned)bstats.downlink_depth, (unsigned long)bstats.downlink_underflow,
           (unsigned long)bstats.downlink_overflow,
           (unsigned)bstats.uplink_depth, (unsigned long)bstats.uplink_underflow,
           (unsigned long)bstats.uplink_overflow);
    static core1_services_diag_t core1;
    core1_services_get_diag(&core1);
    printf("[debug] core1 hb=%lu loop=%lu park=%lu/%lu timeout=%lu entries=%lu flags=0x%02lx req=%u ack=%u\n",
           (unsigned long)core1.heartbeat_ms,
           (unsigned long)core1.loop_count,
           (unsigned long)core1.flash_pause_successes,
           (unsigned long)core1.flash_pause_attempts,
           (unsigned long)core1.flash_pause_timeouts,
           (unsigned long)core1.flash_park_entries,
           (unsigned long)core1.flash_park_last_flags,
           core1.flash_pause_requested ? 1u : 0u,
           core1.flash_parked ? 1u : 0u);
    stack_monitor_snapshot_t stack;
    stack_monitor_get_snapshot(&stack);
    printf("[debug] stack c0=%lu/%lu margin=%lu guard=%u c1=%lu/%lu margin=%lu guard=%u\n",
           (unsigned long)stack.core0.peak_used_bytes,
           (unsigned long)stack.core0.size_bytes,
           (unsigned long)stack.core0.minimum_margin_bytes,
           stack.core0.canary_intact ? 1u : 0u,
           (unsigned long)stack.core1.peak_used_bytes,
           (unsigned long)stack.core1.size_bytes,
           (unsigned long)stack.core1.minimum_margin_bytes,
           stack.core1.canary_intact ? 1u : 0u);
}

static void command_clocks(void) {
    /* Clock-gating bench support: generator frequencies + the three gate
     * masks (ENABLED = live view of which branches actually run right now). */
    printf("[clk] sys=%lu ref=%lu peri=%lu usb=%lu adc=%lu\n",
           (unsigned long)clock_get_hz(clk_sys), (unsigned long)clock_get_hz(clk_ref),
           (unsigned long)clock_get_hz(clk_peri), (unsigned long)clock_get_hz(clk_usb),
           (unsigned long)clock_get_hz(clk_adc));
    printf("[clk] wake_en=%08lx,%08lx sleep_en=%08lx,%08lx enabled=%08lx,%08lx\n",
           (unsigned long)clocks_hw->wake_en0, (unsigned long)clocks_hw->wake_en1,
           (unsigned long)clocks_hw->sleep_en0, (unsigned long)clocks_hw->sleep_en1,
           (unsigned long)clocks_hw->enabled0, (unsigned long)clocks_hw->enabled1);
}

static void command_wake(void) {
    power_sleep_evidence_t e;
    power_sleep_get_evidence(&e);
    printf("[wake] cause=%u chip_reset=%08lx scratch=%08lx pwrup0=%03lx pwrup1=%03lx pwrup2=%03lx pwrup3=%03lx last=%02lx req=%02lx\n",
           (unsigned)e.cause, (unsigned long)e.chip_reset, (unsigned long)e.scratch_marker,
           (unsigned long)e.pwrup0, (unsigned long)e.pwrup1,
           (unsigned long)e.pwrup2, (unsigned long)e.pwrup3,
           (unsigned long)e.last_swcore_pwrup, (unsigned long)e.current_pwrup_req);
    runtime_watchdog_boot_evidence_t watchdog_evidence;
    runtime_watchdog_get_boot_evidence(&watchdog_evidence);
    printf("[wake] runtime_watchdog=%u phase=%s(%u) stamp=%08lx live=%u\n",
           watchdog_evidence.timeout_reset ? 1u : 0u,
           runtime_watchdog_phase_name(watchdog_evidence.phase),
           (unsigned)watchdog_evidence.phase,
           (unsigned long)watchdog_evidence.raw_phase_stamp,
           runtime_watchdog_started() ? 1u : 0u);
    /* Dormant-entry abort history for this boot: reconstructs a "never
     * sleeps" episode after a USB replug (the aborts themselves run with the
     * console dead). Counts reset at boot -- a fresh boot banner on replug
     * means the phone DID sleep and this line will read all-zero. */
    power_sleep_abort_stats_t a;
    power_sleep_get_abort_stats(&a);
    uint32_t total = 0u;
    for (uint8_t i = 0u; i < (uint8_t)POWER_SLEEP_ABORT_CAUSE_COUNT; i++) {
        total += a.counts[i];
    }
    printf("[wake] entry aborts: flush=%u alarm=%u alarm_cfg=%u legacy_timer=%u "
           "tca_arm=%u lvl_read=%u lvl_vbus=%u lvl_stat2=%u lvl_charger=%u",
           (unsigned)a.counts[POWER_SLEEP_ABORT_FLUSH],
           (unsigned)a.counts[POWER_SLEEP_ABORT_ALARM_PENDING],
           (unsigned)a.counts[POWER_SLEEP_ABORT_ALARM_CONFIG],
           (unsigned)a.counts[POWER_SLEEP_ABORT_LEGACY_TIMER_STOP],
           (unsigned)a.counts[POWER_SLEEP_ABORT_TCA_ARM],
           (unsigned)a.counts[POWER_SLEEP_ABORT_LEVEL_READ_FAIL],
           (unsigned)a.counts[POWER_SLEEP_ABORT_LEVEL_VBUS],
           (unsigned)a.counts[POWER_SLEEP_ABORT_LEVEL_STAT2],
           (unsigned)a.counts[POWER_SLEEP_ABORT_LEVEL_CHARGER_INPUT]);
    if (total != 0u) {
        printf(" (last cause=%u at t=%lu ms)", (unsigned)a.last_cause,
               (unsigned long)a.last_ms);
    }
    printf("\n");
    /* Story of the boot that LAST entered dormant (AON scratch telemetry;
     * survives P1.7 + watchdog): its decoded cause, the raw LAST_SWCORE_PWRUP
     * it captured, how long it stayed awake, aborts, and whether its commit
     * was refused by POWMAN (refused -> watchdog reboot -> COLD + 12 s
     * quarantine -- the ~14 s-awake signature) or abandoned mid-WAITING
     * (wake source raced the sequencer; also watchdog-rebooted). */
    uint8_t encoded_last_abort = (uint8_t)((e.prev_entry_info >> 27) & 0x1fu);
    printf("[wake] prev dormant entry: #%u cause=%u lastpwrup=%02x awake=%lu ms "
           "aborts=%u last_abort=%d refused=%u abandoned=%u stat2dead=%u | live last=%02lx\n",
           (unsigned)(e.prev_entry_info & 0xFFFFu),
           (unsigned)(e.prev_entry_stamp >> 28),
           (unsigned)((e.prev_entry_stamp >> 20) & 0xFFu),
           (unsigned long)(e.prev_entry_stamp & 0xFFFFFu),
           (unsigned)((e.prev_entry_info >> 16) & 0xFFu),
           encoded_last_abort == 0u ? -1 : (int)encoded_last_abort - 1,
           (unsigned)((e.prev_entry_info >> 24) & 1u),
           (unsigned)((e.prev_entry_info >> 25) & 1u),
           (unsigned)((e.prev_entry_info >> 26) & 1u),
           (unsigned long)powman_hw->last_swcore_pwrup);
    /* Live probe: is the POWMAN scratch write password-gated on this silicon?
     * (The dormant marker lives in scratch[7], telemetry in [4]/[5]; this
     * test uses the unused scratch[3].) */
    uint32_t save = powman_hw->scratch[3];
    powman_hw->scratch[3] = 0xa5c3f00du;
    uint32_t readback = powman_hw->scratch[3];
    powman_hw->scratch[3] = save;
    printf("[wake] scratch rw probe: wrote a5c3f00d, read %08lx -> %s\n",
           (unsigned long)readback,
           readback == 0xa5c3f00du ? "OK" : "FAIL (write ignored/password-gated)");
}

static void command_pads(void) {
    /* Pad audit for all RP2354B bank-0 pins. SDK accessors are required here:
     * the legacy 32-bit SIO registers silently omit GP32-GP47. */
    printf("[pads] gp fn dir out in | ie od pue pde iso\n");
    for (uint32_t gp = 0u; gp < NUM_BANK0_GPIOS; gp++) {
        uint32_t pad = pads_bank0_hw->io[gp];
        bool oe = gpio_is_dir_out(gp);
        bool out = gpio_get_out_level(gp);
        printf("[pads] %2lu %2d %s %u %u  | %u  %u  %u   %u   %u\n",
               (unsigned long)gp,
               (int)gpio_get_function(gp),
               oe ? "out" : "in ",
               out ? 1u : 0u,
               gpio_get(gp) ? 1u : 0u,
               (pad & PADS_BANK0_GPIO0_IE_BITS) ? 1u : 0u,
               (pad & PADS_BANK0_GPIO0_OD_BITS) ? 1u : 0u,
               (pad & PADS_BANK0_GPIO0_PUE_BITS) ? 1u : 0u,
               (pad & PADS_BANK0_GPIO0_PDE_BITS) ? 1u : 0u,
               (pad & PADS_BANK0_GPIO0_ISO_BITS) ? 1u : 0u);
    }
}

static void command_hw(const app_t *app) {
    board_diag_snapshot_t s;
    board_diag_get_snapshot(&s);
    if (s.battery_valid) {
        printf("[hw] batt mv=%u raw=%u fresh=%u bars=%u seq=%lu\n",
               (unsigned)s.battery_mv,
               (unsigned)board_diag_battery_millivolts_raw(),
               (unsigned)board_diag_battery_millivolts_fresh(),
               (unsigned)s.battery_bars,
               (unsigned long)board_diag_battery_sample_sequence());
    } else {
        printf("[hw] batt policy unavailable (LTC voltage not authoritative)\n");
    }
    printf("[hw] charger conn=%u vin=%lumV adc=%u pin=%umV en=%c forced=%u stat=%u,%u state=%s chem=%s\n",
           s.charger_connected ? 1u : 0u,
           (unsigned long)s.charger_input_mv,
           (unsigned)s.charger_adc_raw,
           (unsigned)s.charger_pin_mv,
           s.charger_enable_valid ? (s.charger_enabled ? '1' : '0') : '?',
           s.charger_forced ? 1u : 0u,
           (unsigned)s.chr_stat1,
           (unsigned)s.chr_stat2,
           board_diag_charge_state_text(s.charge_state),
           board_diag_chemistry_text(s.chemistry));
    printf("[hw] charger req=%u owners=%02x apply=%lu fail=%lu mismatch=%lu\n",
           s.charger_enabled_requested ? 1u : 0u,
           (unsigned)s.charger_inhibit_owner_mask,
           (unsigned long)s.charger_control_attempts,
           (unsigned long)s.charger_control_failures,
           (unsigned long)s.charger_control_mismatches);
    printf("[hw] ltc present=%u cfg=%u valid=%u mv=%u ua=%ld acr=%08lx dQ=%lldnAh qlsb=%u/rev%u temp=%ldmC status=%02x session=%lu i2c=%lu ara=%lu\n",
           s.ltc_present ? 1u : 0u,
           s.ltc_configured ? 1u : 0u,
           s.ltc_sample_valid ? 1u : 0u,
           (unsigned)s.ltc_voltage_mv,
           (long)s.ltc_current_ua,
           (unsigned long)s.ltc_acr_raw,
           (long long)s.ltc_session_delta_nah,
           (unsigned)LTC2959_ACR_LSB_NAH,
           (unsigned)LTC2959_ACR_SCALE_REVISION,
           (long)s.ltc_temperature_mdegc,
           (unsigned)s.ltc_status,
           (unsigned long)s.ltc_gauge_session,
           (unsigned long)s.ltc_i2c_errors,
           (unsigned long)s.ltc_ara_errors);
    battery_charge_supervisor_service_snapshot_t charge;
    battery_charge_supervisor_service_get_snapshot(&charge);
    printf("[hw] charge phase=%u gen=%lu q=%u term=%u block=%08lx "
           "shadow=%lu/%lu\n",
           (unsigned)charge.model.phase,
           (unsigned long)charge.model.charge_generation,
           charge.model.admitted ? 1u : 0u,
           (unsigned)charge.model.terminal_reason,
           (unsigned long)charge.model.blockers,
           (unsigned long)charge.shadow_matches,
           (unsigned long)charge.shadow_mismatches);
    printf("[hw] headset ins=%u hook_mv=%u pressed=%u | vbus=%u edges=%lu tca=%u codec=%u\n",
           s.headset_inserted ? 1u : 0u, (unsigned)s.headset_hook_mv, s.headset_hook_pressed ? 1u : 0u,
           s.vbus_present ? 1u : 0u, (unsigned long)s.service_vbus_edges,
           s.tca_present ? 1u : 0u, s.codec_present ? 1u : 0u);
    printf("[hw] 3v8 en=%u pg=%u owners=%02x pwm=%u | status adc=%u pin=%umV | dtr=%u ri=%u edges=%lu on=%u reset/shdn=%u\n",
           s.rail_3v8_enabled ? 1u : 0u,
           s.rail_3v8_power_good ? 1u : 0u,
           (unsigned)shared_3v8_service_owner_mask(),
           s.tps63020_pwm_mode ? 1u : 0u,
           (unsigned)s.modem_status_adc_raw,
           (unsigned)s.modem_status_pin_mv,
           s.modem_dtr_level ? 1u : 0u,
           s.modem_ri_level ? 1u : 0u,
           (unsigned long)s.modem_ri_edges,
           s.modem_pwr_control_asserted ? 1u : 0u,
           s.modem_hw_shutdown_asserted ? 1u : 0u);
    printf("[hw] irq line=%u edges=%lu drains=%lu stuck=%lu src=%lu/%lu/%lu err=%lu/%lu/%lu last=%02x/%02x t=%02x r=%02x l=%02x\n",
           s.sys_int_asserted ? 1u : 0u,
           (unsigned long)s.shared_irq_edges,
           (unsigned long)s.shared_irq_drains,
           (unsigned long)s.shared_irq_stuck,
           (unsigned long)s.shared_irq_tca_events,
           (unsigned long)s.shared_irq_rtc_events,
           (unsigned long)s.shared_irq_ltc_events,
           (unsigned long)s.shared_irq_source_errors[0],
           (unsigned long)s.shared_irq_source_errors[1],
           (unsigned long)s.shared_irq_source_errors[2],
           (unsigned)s.shared_irq_last_serviced,
           (unsigned)s.shared_irq_last_errors,
           (unsigned)s.shared_irq_last_tca_status,
           (unsigned)s.shared_irq_last_rtc_flags,
           (unsigned)s.shared_irq_last_ltc_status);
    if (app != 0) {
        printf("[hw] route=%u poweroff=%u low_notified=%u "
               "empty_off_ms=%lu endpoint=%u sag=%u cap_v_conflict=%u\n",
               (unsigned)app->route, app->route == APP_ROUTE_POWER_OFF ? 1u : 0u,
               app->battery_low_notified ? 1u : 0u,
               (unsigned long)app->battery_empty_off_ms,
               (unsigned)app->battery_endpoint_kind,
               app->battery_endpoint_state.load_sag_latched ? 1u : 0u,
               app->battery_capacity_voltage_disagreement ? 1u : 0u);
        uint8_t stored_alarm = 0xffu;
        store_status_t alarm_status =
            store_setting_get_u8(STORE_SETTING_CLOCK_ALARM_ENABLED, &stored_alarm);
        printf("[hw] alarm ui=%u store=%s%u rtc=%u event=%u mode=%u offwake=%u chip=%u\n",
               app->clock_alarm_enabled ? 1u : 0u,
               alarm_status == STORE_STATUS_OK ? "" : "?",
               (unsigned)stored_alarm,
               rtc_alarm_hal_alarm_enabled() ? 1u : 0u,
               rtc_alarm_hal_alarm_event_pending() ? 1u : 0u,
               (unsigned)app->clock_alarm_mode,
               app->clock_alarm_power_off_wake ? 1u : 0u,
               rtc_alarm_hal_chip_available() ? 1u : 0u);
    }
    /* Silicon stepping errata filter: the datasheet's "A4 = REVISION 0x8" is
     * wrong in practice -- real A4 reads
     * 0x3 (REVISION was not bumped; RPi forum). The positive A4 check is the
     * bootrom version byte at 0x13 (A3=3, A4=4). Only REVISION 0x2 = A2 =
     * erratum E9 applies. */
    uint32_t chip_id = sysinfo_hw->chip_id;
#if PICO_SDK_VERSION_MAJOR > 2 || \
    (PICO_SDK_VERSION_MAJOR == 2 && PICO_SDK_VERSION_MINOR >= 3)
    uint8_t bootrom_ver = rp2350_rom_version();
#else
    /* SDK <=2.2 retained the RP2040 name for the equivalent RP2350 ROM byte. */
    uint8_t bootrom_ver = rp2040_rom_version();
#endif
    printf("[hw] silicon chip_id=%08lx rev=0x%x bootrom=%u -> %s\n",
           (unsigned long)chip_id, (unsigned)(chip_id >> 28),
           (unsigned)bootrom_ver,
           (chip_id >> 28) == 0x2u  ? "A2 (E9 APPLIES!)"
           : bootrom_ver == 4u      ? "A4"
           : (chip_id >> 28) == 0x3u ? "A3-class (post-A2)"
                                     : "post-A2 (unmapped rev)");
    /* USB liveness forensics for the dead-USB-after-REBOOT2 quirk: tud flags
     * say what TinyUSB thinks, SIE_STATUS says what the wire saw (VBUS_DETECT
     * bit0x1? CONNECTED/SUSPENDED/line-state fields -- raw dump; decode against
     * the datasheet when a dead boot is on the bench). */
    printf("[hw] usb active=%u tud_conn=%u mounted=%u sie=%08lx init_fail=%lu warm_boot=%u i2c_stuck=%u\n",
           usb_service_active() ? 1u : 0u,
           usb_service_connected() ? 1u : 0u,
           usb_service_mounted() ? 1u : 0u,
           (unsigned long)usb_service_sie_status(),
           (unsigned long)usb_service_attach_failures(),
           power_sleep_boot_was_warm() ? 1u : 0u,
           board_i2c_bus_was_stuck() ? 1u : 0u);
    usb_service_diag_t usb_diag;
    usb_service_get_diag(&usb_diag);
    printf("[hw] usb worker start/stop=%lu/%lu kick=%lu rearm=%lu q=%lu/%lu bus=%lu/%lu/%lu/%lu line=%02x/%lu rx=%lu\n",
           (unsigned long)usb_diag.starts,
           (unsigned long)usb_diag.stops,
           (unsigned long)usb_diag.worker_kicks,
           (unsigned long)usb_diag.worker_disabled_rearms,
           (unsigned long)usb_diag.event_pending_observations,
           (unsigned long)usb_diag.event_pending_streak_max,
           (unsigned long)usb_diag.mounts,
           (unsigned long)usb_diag.unmounts,
           (unsigned long)usb_diag.suspends,
           (unsigned long)usb_diag.resumes,
           (unsigned)usb_diag.line_state,
           (unsigned long)usb_diag.line_state_changes,
           (unsigned long)usb_diag.rx_callbacks);
}

/* Print the first snapshot and every subsequent change in the stable hardware
 * state (charger / bars / headset insert / raw STAT). This is how the console
 * catches boot-time transients (e.g. the warm-reboot charger blip) with a
 * timestamp, without polling. Keyed on coarse fields so battery-mv noise alone
 * never spams. */
static void hw_change_log(const app_t *app) {
    static bool seen;
    static bool prev_charger, prev_ins;
    static uint8_t prev_bars, prev_stat1, prev_stat2, prev_route;

    board_diag_snapshot_t s;
    board_diag_get_snapshot(&s);
    uint8_t route = (app != 0) ? (uint8_t)app->route : 0u;
    if (seen && s.charger_connected == prev_charger && s.battery_bars == prev_bars &&
        s.headset_inserted == prev_ins && s.chr_stat1 == prev_stat1 && s.chr_stat2 == prev_stat2 &&
        route == prev_route) {
        return;
    }
    seen = true;
    prev_charger = s.charger_connected;
    prev_bars = s.battery_bars;
    prev_ins = s.headset_inserted;
    prev_stat1 = s.chr_stat1;
    prev_stat2 = s.chr_stat2;
    prev_route = route;
    printf("[hw] t=%lu mv=%u bars=%u charger=%u stat=%u,%u state=%s hs=%u route=%u\n",
           (unsigned long)time_ms(), (unsigned)s.battery_mv, (unsigned)s.battery_bars,
           s.charger_connected ? 1u : 0u, (unsigned)s.chr_stat1, (unsigned)s.chr_stat2,
           board_diag_charge_state_text(s.charge_state),
           s.headset_inserted ? 1u : 0u, (unsigned)route);
}

static void command_ui(app_t *app, char *args) {
    if (app == 0) {
        printf("[debug] no app handle\n");
        return;
    }
    char *cursor = args;
    char *name = next_token(&cursor);
    if (name == 0) {
        printf("[debug] usage: ui <standby|number|keyguard|call|callopts|incoming>\n");
        return;
    }

    app->route = APP_ROUTE_STANDBY;
    app->input_len = 0u;
    app->input_text[0] = '\0';
    app->input_action = APP_STANDBY_ACTION_CALL;
    app->standby_message[0] = '\0';
    app->backlight_activity_pending = false;
    app->missed_call_pending = false;
    app->missed_call_pending_count = 0u;
    app->sms_received_pending = false;
    app->keyguard_locked = false;
    app->unlock_armed = false;
    app->call_number[0] = '\0';
    app->call_name[0] = '\0';
    app->call_volume_visible = false;

    if (strcmp(name, "standby") == 0) {
        app->route = APP_ROUTE_STANDBY;
    } else if (strcmp(name, "number") == 0) {
        copy_text(app->input_text, sizeof(app->input_text), "123456789012345678901234567890");
        app->input_len = (uint8_t)strlen(app->input_text);
        app->input_action = APP_STANDBY_ACTION_CALL;
    } else if (strcmp(name, "keyguard") == 0) {
        app->keyguard_locked = true;
    } else if (strcmp(name, "call") == 0) {
        app->route = APP_ROUTE_CALL;
        app->call_phase = 1u;
        copy_text(app->call_name, sizeof(app->call_name), "Test");
        copy_text(app->call_number, sizeof(app->call_number), "+12125550123");
        app->call_connected_ms = time_ms() - 65000u;
        app->call_last_second = -1;
    } else if (strcmp(name, "callopts") == 0) {
        app->route = APP_ROUTE_CALL_OPTIONS;
        app->call_phase = 1u;
        copy_text(app->call_name, sizeof(app->call_name), "Test");
        copy_text(app->call_number, sizeof(app->call_number), "+12125550123");
        app->call_connected_ms = time_ms() - 65000u;
        app->call_options_selected = 0u;
    } else if (strcmp(name, "incoming") == 0) {
        app->route = APP_ROUTE_INCOMING_CALL;
        app->call_incoming = true;
        copy_text(app->call_name, sizeof(app->call_name), "Test");
        copy_text(app->call_number, sizeof(app->call_number), "+12125550123");
        app->call_incoming_silenced = false;
    } else {
        printf("[debug] unknown ui checkpoint: %s\n", name);
        return;
    }
    app->dirty = true;
    printf("[debug] ui checkpoint: %s\n", name);
}

static void command_text(char *args) {
    char *cursor = args;
    char *number = next_token(&cursor);
    char *message = skip_spaces(cursor);
    if (number == 0 || message == 0 || message[0] == '\0') {
        printf("[debug] usage: txt <number> <message>\n");
        return;
    }
    uint32_t request_id = 0u;
    bool ok = modem_service_request_send_sms(number, message, &request_id);
    if (ok) {
        s_waiting_sms_result = true;
        s_waiting_sms_request_id = request_id;
    }
    printf("[debug] queued text sms ok=%u number=%s\n", ok ? 1u : 0u, number);
}

static void command_pdu7(char *args) {
    char *cursor = args;
    char *number = next_token(&cursor);
    if (number == 0) {
        printf("[debug] usage: pdu7 <number>\n");
        return;
    }
    static const uint8_t payload[] = {'P', 'D', 'U', 'T', 'E', 'S', 'T'};
    uint32_t request_id = 0u;
    bool ok = modem_service_request_send_binary_sms_mode(number,
                                                         payload,
                                                         sizeof(payload),
                                                         0u,
                                                         0u,
                                                         MODEM_BINARY_SMS_MODE_GSM7_TEXT,
                                                         &request_id);
    if (ok) {
        s_waiting_sms_result = true;
        s_waiting_sms_request_id = request_id;
    }
    printf("[debug] queued pdu7 sms ok=%u number=%s\n", ok ? 1u : 0u, number);
}

static void command_binary(char *args) {
    char *cursor = args;
    char *number = next_token(&cursor);
    if (number == 0) {
        printf("[debug] usage: bin <number>\n");
        return;
    }
    static const uint8_t payload[] = {'B', 'I', 'N', 'A', 'R', 'Y'};
    uint32_t request_id = 0u;
    bool ok = modem_service_request_send_binary_sms_mode(number,
                                                         payload,
                                                         sizeof(payload),
                                                         0u,
                                                         0u,
                                                         MODEM_BINARY_SMS_MODE_DCS04_PORT_FIRST,
                                                         &request_id);
    if (ok) {
        s_waiting_sms_result = true;
        s_waiting_sms_request_id = request_id;
    }
    printf("[debug] queued binary no-udh ok=%u number=%s\n", ok ? 1u : 0u, number);
}

static void command_binary_f5(char *args) {
    char *cursor = args;
    char *number = next_token(&cursor);
    if (number == 0) {
        printf("[debug] usage: binf5 <number>\n");
        return;
    }
    static const uint8_t payload[] = {'B', 'I', 'N', 'F', '5'};
    uint32_t request_id = 0u;
    bool ok = modem_service_request_send_binary_sms_mode(number,
                                                         payload,
                                                         sizeof(payload),
                                                         0u,
                                                         0u,
                                                         MODEM_BINARY_SMS_MODE_F5_PORT_FIRST,
                                                         &request_id);
    if (ok) {
        s_waiting_sms_result = true;
        s_waiting_sms_request_id = request_id;
    }
    printf("[debug] queued binary f5 no-udh ok=%u number=%s\n", ok ? 1u : 0u, number);
}

static void command_port(char *args) {
    char *cursor = args;
    char *number = next_token(&cursor);
    if (number == 0) {
        printf("[debug] usage: port <number>\n");
        return;
    }
    static const uint8_t payload[] = {'P', 'O', 'R', 'T'};
    uint32_t request_id = 0u;
    bool ok = modem_service_request_send_binary_sms_mode(number,
                                                         payload,
                                                         sizeof(payload),
                                                         DEBUG_PICTURE_PORT,
                                                         0u,
                                                         MODEM_BINARY_SMS_MODE_DCS04_PORT_FIRST,
                                                         &request_id);
    if (ok) {
        s_waiting_sms_result = true;
        s_waiting_sms_request_id = request_id;
    }
    printf("[debug] queued binary port-udh ok=%u number=%s\n", ok ? 1u : 0u, number);
}

static void command_picture(char *args) {
    char *cursor = args;
    char *number = next_token(&cursor);
    char *mode_text = next_token(&cursor);
    if (number == 0 || mode_text == 0) {
        printf("[debug] usage: pic <number> <mode>\n");
        return;
    }
    unsigned mode_number = (unsigned)strtoul(mode_text, 0, 10);
    modem_binary_sms_mode_t mode = MODEM_BINARY_SMS_MODE_DCS04_PORT_FIRST;
    uint16_t source_port = 0u;
    if (mode_number == 1u) {
        mode = MODEM_BINARY_SMS_MODE_DCS04_CONCAT_FIRST;
    } else if (mode_number == 2u) {
        mode = MODEM_BINARY_SMS_MODE_F5_PORT_FIRST;
    } else if (mode_number == 3u) {
        mode = MODEM_BINARY_SMS_MODE_F5_CONCAT_FIRST_VP;
        source_port = DEBUG_PICTURE_PORT;
    }

    uint8_t payload[SMS_CODEC_BINARY_MAX];
    uint16_t payload_len = 0u;
    uint8_t chunks = 0u;
    if (!build_debug_picture_payload(payload, sizeof(payload), &payload_len, &chunks)) {
        printf("[debug] picture payload build failed\n");
        return;
    }
    uint32_t request_id = 0u;
    bool ok = modem_service_request_send_binary_sms_mode(number,
                                                         payload,
                                                         payload_len,
                                                         DEBUG_PICTURE_PORT,
                                                         source_port,
                                                         mode,
                                                         &request_id);
    if (ok) {
        s_waiting_sms_result = true;
        s_waiting_sms_request_id = request_id;
    }
    printf("[debug] queued picture ok=%u number=%s mode=%u bytes=%u chunks=%u src=0x%04x\n",
           ok ? 1u : 0u,
           number,
           mode_number,
           (unsigned)payload_len,
           (unsigned)chunks,
           (unsigned)source_port);
}

static bool build_debug_picture_payload(uint8_t *payload, size_t cap, uint16_t *out_len, uint8_t *out_chunks) {
    store_picture_message_t picture;
    if (store_picture_message_get(0u, &picture) != STORE_STATUS_OK) {
        fill_fallback_picture(&picture);
    }
    if (picture.text[0] == '\0') {
        strcpy(picture.text, "Debug picture");
    }
    return sms_picture_payload_encode(&picture, picture.text, payload, cap, out_len, out_chunks);
}

static void fill_fallback_picture(store_picture_message_t *picture) {
    if (picture == 0) {
        return;
    }
    memset(picture, 0, sizeof(*picture));
    picture->used = true;
    picture->width = STORE_PICTURE_WIDTH;
    picture->height = STORE_PICTURE_HEIGHT;
    picture->bitmap_len = STORE_PICTURE_BITMAP_BYTES;
    strcpy(picture->text, "Debug picture");
    for (uint8_t y = 0u; y < STORE_PICTURE_HEIGHT; y++) {
        for (uint8_t x = 0u; x < STORE_PICTURE_WIDTH; x++) {
            if (x == 0u || y == 0u || x + 1u == STORE_PICTURE_WIDTH || y + 1u == STORE_PICTURE_HEIGHT ||
                ((x + y) % 9u) == 0u) {
                uint16_t bit = (uint16_t)y * STORE_PICTURE_WIDTH + x;
                picture->bitmap[bit / 8u] |= (uint8_t)(1u << (7u - (bit & 7u)));
            }
        }
    }
}
