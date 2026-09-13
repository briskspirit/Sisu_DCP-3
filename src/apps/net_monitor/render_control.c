#include "render_internal.h"

static const char *maintenance_state_text(modem_maintenance_state_t state) {
    switch (state) {
    case MODEM_MAINTENANCE_PENDING: return "PENDING";
    case MODEM_MAINTENANCE_RUNNING: return "WORKING";
    case MODEM_MAINTENANCE_ACTIVE: return "ACTIVE";
    case MODEM_MAINTENANCE_RESTORING: return "RESTORE";
    case MODEM_MAINTENANCE_DONE: return "DONE";
    case MODEM_MAINTENANCE_ERROR: return "ERROR";
    case MODEM_MAINTENANCE_LOCKED: return "RF LOCKED";
    case MODEM_MAINTENANCE_IDLE:
    default: return "IDLE";
    }
}

static modem_maintenance_action_t maintenance_action_for_page(uint8_t id) {
    switch (id) {
    case 97u: return MODEM_MAINTENANCE_SCAN_TIMER;
    case 98u: return MODEM_MAINTENANCE_BAND_TEST;
    case 99u: return MODEM_MAINTENANCE_ANTENNA;
    default: return MODEM_MAINTENANCE_NONE;
    }
}

static void format_maintenance_state(uint8_t id,
                                     const netmon_control_snapshot_t *c,
                                     netmon_frame_t *out, uint8_t row) {
    modem_maintenance_action_t expected = maintenance_action_for_page(id);
    if (c->maintenance.action != expected) {
        netmon_render_linef(out, row, "STATE IDLE");
        return;
    }
    if ((c->maintenance.state == MODEM_MAINTENANCE_ERROR ||
         c->maintenance.state == MODEM_MAINTENANCE_LOCKED) &&
        c->maintenance.error != MODEM_MAINTENANCE_ERROR_NONE) {
        netmon_render_linef(out, row, "ERR %u", (unsigned)c->maintenance.error);
    } else {
        netmon_render_linef(out, row, "%s",
              maintenance_state_text(c->maintenance.state));
    }
}
void netmon_render_control(uint8_t id, uint8_t frame_index,
                           const netmon_control_snapshot_t *c,
                           netmon_frame_t *out) {
    switch (id) {
    case 89u:
        if (frame_index == 0u) {
            netmon_render_linef(out, 0u, "LIGHT %u%%", (unsigned)c->backlight_percent);
            if (c->backlight_stored_valid) {
                netmon_render_linef(out, 1u, "SAVED %u%%",
                      (unsigned)c->backlight_stored_percent);
            } else {
                netmon_render_linef(out, 1u, "SAVED --");
            }
            netmon_render_linef(out, 2u, "1/3 5%%");
            netmon_render_linef(out, 3u, "4/6 1%%");
        } else {
            netmon_render_linef(out, 0u, "0 REVERT");
            netmon_render_linef(out, 1u, "* SAVE");
            netmon_render_linef(out, 2u, "# DEFAULT");
        }
        break;
    case 90u:
        netmon_render_linef(out, 0u, "1 LIGHT");
        netmon_render_linef(out, 1u, "2 VIBRA");
        netmon_render_linef(out, 2u, "3 BUZZER");
        netmon_render_linef(out, 3u, "0 STOP");
        break;
    case 91u:
        netmon_render_linef(out, 0u, "BATT %umV", (unsigned)c->battery_mv);
        netmon_render_linef(out, 1u, "FORCE %s", netmon_render_yes_no(c->battery_forced));
        netmon_render_linef(out, 2u, "1..5 preset");
        netmon_render_linef(out, 3u, "# AUTO");
        break;
    case 92u:
        if (frame_index == 0u) {
            netmon_render_linef(out, 0u, "IN %u F%u", c->charger_connected ? 1u : 0u,
                  c->charger_forced ? 1u : 0u);
            if (c->charger_enable_valid) {
                netmon_render_linef(out, 1u, "ENABLE %u", c->charger_enabled ? 1u : 0u);
            } else {
                netmon_render_linef(out, 1u, "ENABLE ?");
            }
            netmon_render_linef(out, 2u, "1 IN 2 OUT");
            netmon_render_linef(out, 3u, "# AUTO");
        } else {
            netmon_render_linef(out, 0u, "REQ %u OWN %02X",
                  c->charger_enabled_requested ? 1u : 0u,
                  (unsigned)c->charger_inhibit_owner_mask);
            netmon_render_linef(out, 1u, "3 ENABLE");
            netmon_render_linef(out, 2u, "4 DISABLE");
            netmon_render_linef(out, 3u, "# AUTO");
        }
        break;
    case 93u:
        netmon_render_linef(out, 0u, "MODE %u", (unsigned)c->headset_override);
        netmon_render_linef(out, 1u, "0 AUTO");
        netmon_render_linef(out, 2u, "1 IN 2 OUT");
        break;
    case 94u:
        netmon_render_linef(out, 0u, "TPS %s", c->converter_pwm ? "PWM" : "save");
        netmon_render_linef(out, 1u, "0 SAVE");
        netmon_render_linef(out, 2u, "1 PWM");
        break;
    case 95u:
        if (frame_index == 0u) {
            netmon_render_linef(out, 0u, "V%u T%u B%u", (unsigned)c->lcd_vop,
                  (unsigned)c->lcd_temperature_coefficient,
                  (unsigned)c->lcd_bias_system);
            if (c->lcd_stored_valid) {
                netmon_render_linef(out, 1u, "S%u T%u B%u", (unsigned)c->lcd_stored_vop,
                      (unsigned)c->lcd_stored_temperature_coefficient,
                      (unsigned)c->lcd_stored_bias_system);
            } else {
                netmon_render_linef(out, 1u, "SAVED --");
            }
            netmon_render_linef(out, 2u, "1/3 VOP");
            netmon_render_linef(out, 3u, "4/6 TC");
        } else {
            netmon_render_linef(out, 0u, "7/9 BIAS");
            netmon_render_linef(out, 1u, "0 REVERT");
            netmon_render_linef(out, 2u, "%s",
                  c->lcd_save_armed ? "* CONFIRM" : "* SAVE");
            netmon_render_linef(out, 3u, "# STOCK");
        }
        break;
    case 96u:
        netmon_render_linef(out, 0u, "WINDOW %lu", (unsigned long)c->measurement_generation);
        netmon_render_linef(out, 1u, "0 RESET");
        break;
    case 97u:
        if (frame_index == 0u) {
            if (c->maintenance.action == MODEM_MAINTENANCE_SCAN_TIMER &&
                c->maintenance.scan_timer_s != 0u) {
                netmon_render_linef(out, 0u, "SCAN %us",
                      (unsigned)c->maintenance.scan_timer_s);
            } else {
                netmon_render_linef(out, 0u, "SCAN --");
            }
            format_maintenance_state(id, c, out, 1u);
            netmon_render_linef(out, 2u, "%s",
                  c->radio_armed ? "ARMED" : "* ARM # READ");
            netmon_render_linef(out, 3u, "1=5 2=30 sec");
        } else {
            netmon_render_linef(out, 0u, "3=60 4=300");
            netmon_render_linef(out, 1u, "5=900 sec");
            netmon_render_linef(out, 2u, "6=1800 sec");
            netmon_render_linef(out, 3u, "7=3600 sec");
        }
        break;
    case 98u:
        if (frame_index == 0u) {
            bool band_action =
                c->maintenance.action == MODEM_MAINTENANCE_BAND_TEST;
            if (band_action && c->maintenance.band_preset != 0u) {
                static const uint8_t BANDS[] = {2u, 4u, 5u, 12u, 14u};
                uint8_t preset = c->maintenance.band_preset;
                const char *label =
                    c->maintenance.state == MODEM_MAINTENANCE_ACTIVE
                        ? "BAND" :
                    c->maintenance.state == MODEM_MAINTENANCE_RESTORING
                        ? "RESTORE" : "TEST";
                netmon_render_linef(out, 0u, preset <= 5u ? "%s B%u" : "BAND --",
                      label,
                      preset <= 5u ? (unsigned)BANDS[preset - 1u] : 0u);
            } else if (band_action &&
                       (c->maintenance.state == MODEM_MAINTENANCE_DONE ||
                        c->maintenance.state == MODEM_MAINTENANCE_ERROR)) {
                netmon_render_linef(out, 0u, "BAND BASE");
            } else if (band_action &&
                       c->maintenance.state == MODEM_MAINTENANCE_LOCKED) {
                netmon_render_linef(out, 0u, "BAND UNKNOWN");
            } else {
                netmon_render_linef(out, 0u, "BAND --");
            }
            format_maintenance_state(id, c, out, 1u);
            netmon_render_linef(out, 2u, "%s", c->radio_armed ? "ARMED" : "* ARM");
            netmon_render_linef(out, 3u, "0/# RESTORE");
        } else {
            netmon_render_linef(out, 0u, "1 B2  2 B4");
            netmon_render_linef(out, 1u, "3 B5");
            netmon_render_linef(out, 2u, "4 B12 5 B14");
            netmon_render_linef(out, 3u, "RAM ONLY");
        }
        break;
    case 99u:
        if (frame_index == 0u) {
            if (c->maintenance.action == MODEM_MAINTENANCE_ANTENNA &&
                c->maintenance.antenna_active) {
                netmon_render_linef(out, 0u, "ANT RF%u",
                      (unsigned)c->maintenance.antenna_rf);
            } else {
                netmon_render_linef(out, 0u, "ANT AUTO");
            }
            format_maintenance_state(id, c, out, 1u);
            netmon_render_linef(out, 2u, "%s",
                  c->radio_armed ? "ARMED # ENTER" : "* ARM # ENTER");
            netmon_render_linef(out, 3u, "0 EXIT");
        } else {
            netmon_render_linef(out, 0u, "1 RF1 2 RF2");
            netmon_render_linef(out, 1u, "3 RF3 4 RF4");
            netmon_render_linef(out, 2u, "CFUN4 MANUAL");
            netmon_render_linef(out, 3u, "AUTO ON EXIT");
        }
        break;
    default:
        netmon_render_linef(out, 1u, "UNSUP");
        break;
    }
    if (c->status[0] != '\0') {
        netmon_render_linef(out, 3u, "%s", c->status);
    }
}
