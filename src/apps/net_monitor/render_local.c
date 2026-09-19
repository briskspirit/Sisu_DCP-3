#include "render_internal.h"

static const char *soc_provenance_short(uint8_t provenance) {
    static const char *const TEXT[] = {
        "UNKNOWN", "BOOT", "TRACK", "FULL", "EMPTY",
    };
    return provenance < sizeof(TEXT) / sizeof(TEXT[0])
        ? TEXT[provenance] : "?";
}

static const char *soc_confidence_short(uint8_t confidence) {
    static const char *const TEXT[] = {
        "NONE", "PROV", "ANCH",
    };
    return confidence < sizeof(TEXT) / sizeof(TEXT[0])
        ? TEXT[confidence] : "?";
}

static const char *charge_policy_short(uint8_t policy) {
    static const char *const TEXT[] = {
        "OBS", "ANCH", "BOOT",
    };
    return policy < sizeof(TEXT) / sizeof(TEXT[0]) ? TEXT[policy] : "?";
}

void netmon_render_local(uint8_t id, uint8_t frame_index,
                         const netmon_local_diag_snapshot_t *l,
                         netmon_frame_t *out) {
    const board_diag_snapshot_t *b = &l->board;
    static const char *const STORE_UNIT_NAMES[STORE_UNIT_COUNT] = {
        "SET PHONE", "SET SMS", "SET CALL", "SET PROF", "SET CLOCK",
        "SET SYS", "MISSED", "RECEIVED", "DIALLED", "T9 WORDS",
        "PICT SMS", "OWN TONES", "DIVERT", "WARRANTY", "BAT LRN",
        "CHG SUP",
    };
    switch (id) {
    case 40u:
        if (b->battery_valid) {
            char warning = b->battery_empty ? 'E'
                         : b->battery_low ? 'L' : '-';
            netmon_render_linef(out, 0u, "T%u R%u",
                                (unsigned)b->ltc_voltage_mv,
                                (unsigned)b->battery_mv);
            netmon_render_linef(out, 1u, "C%d B%u W%c",
                                (int)b->battery_correction_mv,
                                (unsigned)b->battery_bars, warning);
            if (b->battery_window_current_valid) {
                netmon_render_current(out->lines[2], NETMON_FRAME_LINE_CAP,
                                      b->battery_window_current_ua);
            } else {
                netmon_render_linef(out, 2u, "I5 --");
            }
            if (b->battery_forced) {
                netmon_render_linef(out, 3u, "FORCED");
            } else {
                netmon_render_linef(out, 3u, "BOOT %u/%u",
                                    (unsigned)b->battery_power_on_samples,
                                    (unsigned)b->battery_power_on_attempts);
            }
        } else {
            netmon_render_linef(out, 1u, "NO SAMPLE");
        }
        break;
    case 41u:
        if (b->ltc_sample_valid) {
            netmon_render_linef(out, 0u, "ACR %08lX", (unsigned long)b->ltc_acr_raw);
            netmon_render_charge_delta(out->lines[1], NETMON_FRAME_LINE_CAP,
                                l->measurement_delta_nah);
            netmon_render_current(out->lines[2], NETMON_FRAME_LINE_CAP,
                           l->measurement_average_ua);
            netmon_render_linef(out, 3u, "T %lus",
                  (unsigned long)(l->measurement_elapsed_ms / 1000u));
        } else {
            netmon_render_linef(out, 1u, "NO LTC SAMPLE");
        }
        break;
    case 42u:
        netmon_render_linef(out, 0u, "LTC P%u C%u", b->ltc_present ? 1u : 0u,
              b->ltc_configured ? 1u : 0u);
        if (b->ltc_sample_valid) {
            netmon_render_linef(out, 1u, "V%u S%02X", (unsigned)b->ltc_voltage_mv,
                  (unsigned)b->ltc_status);
        } else {
            netmon_render_linef(out, 1u, "SAMPLE bad");
        }
        netmon_render_linef(out, 2u, "I2C %lu", (unsigned long)b->ltc_i2c_errors);
        netmon_render_linef(out, 3u, "ARA %lu", (unsigned long)b->ltc_ara_errors);
        break;
    case 43u:
        netmon_render_linef(out, 0u, "VIN %lumV", (unsigned long)b->charger_input_mv);
        netmon_render_linef(out, 1u, "ADC %u", (unsigned)b->charger_adc_raw);
        netmon_render_linef(out, 2u, "IN %u S%u/%u", b->charger_connected ? 1u : 0u,
              (unsigned)b->chr_stat1, (unsigned)b->chr_stat2);
        netmon_render_linef(out, 3u, "STATE %u", (unsigned)b->charge_state);
        break;
    case 44u:
        if (b->charger_enable_valid) {
            netmon_render_linef(out, 0u, "ENABLE %s", netmon_render_yes_no(b->charger_enabled));
        } else {
            netmon_render_linef(out, 0u, "ENABLE ?");
        }
        netmon_render_linef(out, 1u, "ACTIVE %s", netmon_render_yes_no(b->charge_state == BOARD_DIAG_CHARGE_ACTIVE));
        netmon_render_linef(out, 2u, "FORCE %s", netmon_render_yes_no(b->charger_forced));
        netmon_render_linef(out, 3u, "NiMH");
        break;
    case 45u:
        netmon_render_linef(out, 0u, "OWN %02X", (unsigned)l->rail_owner_mask);
        netmon_render_linef(out, 1u, "EN %u PG%u", b->rail_3v8_enabled ? 1u : 0u,
              b->rail_3v8_power_good ? 1u : 0u);
        netmon_render_linef(out, 2u, "PWM %s", b->tps63020_pwm_mode ? "force" : "save");
        netmon_render_linef(out, 3u, "TR %lu", (unsigned long)l->rail_transitions);
        break;
    case 46u:
        netmon_render_linef(out, 0u, "VBUS %s", netmon_render_on_off(b->vbus_present));
        netmon_render_linef(out, 1u, "EDGES %lu", (unsigned long)b->service_vbus_edges);
        netmon_render_linef(out, 2u, "CHARG %s", netmon_render_on_off(b->charger_connected));
        netmon_render_linef(out, 3u, "debug only");
        break;
    case 47u: {
        const netmon_battery_learning_diag_t *model = &l->battery_learning;
        if (frame_index == 0u) {
            netmon_render_linef(out, 0u, "NOM %umAh",
                                (unsigned)model->nominal_capacity_mah);
            if (model->learned_capacity_valid) {
                netmon_render_linef(out, 1u, "CAP %umAh",
                                    (unsigned)model->learned_capacity_mah);
            } else {
                netmon_render_linef(out, 1u, "CAP --");
            }
            if (model->remaining_capacity_valid) {
                netmon_render_linef(out, 2u, "REM %umAh",
                                    (unsigned)model->remaining_capacity_mah);
                if (model->learned_capacity_valid) {
                    netmon_render_linef(
                        out, 3u, "S%u B%c H%u",
                        (unsigned)model->state_of_charge_percent,
                        model->soc_bars_valid
                            ? (char)('0' + model->soc_bars)
                            : '-',
                        (unsigned)model->state_of_health_percent);
                } else {
                    netmon_render_linef(
                        out, 3u, "S%u B%c H--",
                        (unsigned)model->state_of_charge_percent,
                        model->soc_bars_valid
                            ? (char)('0' + model->soc_bars)
                            : '-');
                }
            } else {
                netmon_render_linef(out, 2u, "REM --");
                netmon_render_linef(out, 3u, "SOC --");
            }
        } else if (frame_index == 1u) {
            static const char *const CONFIDENCE[] = {
                "PRIOR", "OBS", "LEARN", "CONFL",
            };
            uint8_t confidence = (uint8_t)model->capacity_confidence;
            const char *text = confidence <
                    sizeof(CONFIDENCE) / sizeof(CONFIDENCE[0])
                ? CONFIDENCE[confidence] : "?";
            netmon_render_linef(out, 0u, "CONF %s", text);
            netmon_render_linef(out, 1u, "ANCH %c P%u V%u",
                                model->full_anchor_valid
                                    ? (model->capacity_cycle_qualified
                                           ? 'Q' : 'X')
                                    : '-',
                                model->capacity_prediction_exhausted ? 1u : 0u,
                                model->capacity_voltage_disagreement ? 1u : 0u);
            netmon_render_linef(
                out, 2u, "OK %u O%lu",
                (unsigned)model->accepted_capacity_cycles,
                (unsigned long)(model->capacity_overrun_nah /
                                UINT64_C(1000000)));
            netmon_render_linef(out, 3u, "BAD %u",
                                (unsigned)model->rejected_capacity_cycles);
        } else if (frame_index == 2u) {
            netmon_render_linef(
                out, 0u, "SOC %s",
                soc_confidence_short((uint8_t)model->soc_confidence));
            netmon_render_linef(
                out, 1u, "SRC %s",
                soc_provenance_short((uint8_t)model->soc_provenance));
            netmon_render_linef(out, 2u, "SEG %s",
                                model->soc_charge_segment ? "CHG" : "DIS");
            if (model->soc_bootstrap_reference_mv != 0u) {
                netmon_render_linef(
                    out, 3u, "BOOT %umV",
                    (unsigned)model->soc_bootstrap_reference_mv);
            } else {
                netmon_render_linef(out, 3u, "BOOT --");
            }
        } else if (frame_index == 3u) {
            if (model->learned_capacity_valid) {
                netmon_render_linef(out, 0u, "LAST %u",
                                    (unsigned)model->last_capacity_mah);
                netmon_render_linef(out, 1u, "SPREAD %u",
                                    (unsigned)model->capacity_spread_mah);
            } else {
                netmon_render_linef(out, 0u, "LAST --");
                netmon_render_linef(out, 1u, "SPREAD --");
            }
            netmon_render_linef(out, 2u, "GEN %08lX",
                                (unsigned long)model->pack_generation);
            netmon_render_linef(out, 3u, "PF%08lXP%u",
                                (unsigned long)model->persistence_failures,
                                model->persistence_pending ? 1u : 0u);
        } else {
            static const char BIN_TEXT[] = {'H', 'M', 'L'};
            for (uint8_t bin_index = 0u;
                 bin_index < BATTERY_LEARNING_RESISTANCE_BIN_COUNT;
                 bin_index++) {
                uint16_t resistance = model->resistance_mohm[bin_index];
                if (resistance != 0u) {
                    netmon_render_linef(
                        out, bin_index, "%c%u N%04X", BIN_TEXT[bin_index],
                        (unsigned)resistance,
                        (unsigned)model->resistance_sample_count[bin_index]);
                } else {
                    netmon_render_linef(
                        out, bin_index, "%c-- N%04X", BIN_TEXT[bin_index],
                        (unsigned)model->resistance_sample_count[bin_index]);
                }
            }
            char bin = '-';
            if (model->current_resistance_bin_valid) {
                uint8_t index = (uint8_t)model->current_resistance_bin;
                bin = index < sizeof(BIN_TEXT) ? BIN_TEXT[index] : '?';
            }
            netmon_render_linef(out, 3u, "BIN %c", bin);
        }
        break;
    }
    case 48u: {
        const netmon_charge_supervisor_diag_t *model =
            &l->battery_charge_supervisor;
        if (frame_index == 0u) {
            netmon_render_linef(out, 0u, "%c P%u G%04lX",
                                charge_policy_short(model->policy)[0],
                                (unsigned)model->phase,
                                (unsigned long)(model->charge_generation &
                                                0xffffu));
            netmon_render_linef(out, 1u, "T %lus",
                                (unsigned long)(model->elapsed_ms / 1000u));
            netmon_render_linef(out, 2u, "TERM %u C%u",
                                (unsigned)model->terminal_reason,
                                (unsigned)model->candidate);
            netmon_render_linef(
                out, 3u, "SH %04lX/%04lX",
                (unsigned long)(model->shadow_matches & 0xffffu),
                (unsigned long)(model->shadow_mismatches & 0xffffu));
        } else if (frame_index == 1u) {
            netmon_render_charge_delta(out->lines[0], NETMON_FRAME_LINE_CAP,
                                       model->net_input_nah);
            if (model->deficit_valid) {
                netmon_render_linef(out, 1u, "DEF %umAh",
                                    (unsigned)model->deficit_mah);
            } else {
                netmon_render_linef(out, 1u, "DEF --");
            }
            if (model->target_valid) {
                netmon_render_linef(
                    out, 2u, "TGT%llumAh %c",
                    (unsigned long long)(model->target_input_nah /
                                         UINT64_C(1000000)),
                    model->target_full_capacity ? 'F' : 'D');
            } else {
                netmon_render_linef(out, 2u, "TGT --");
            }
            netmon_render_linef(out, 3u, "BLK %08lX",
                                (unsigned long)model->blockers);
        } else if (frame_index == 2u) {
            netmon_render_linef(out, 0u, "V%u C%u",
                                (unsigned)model->terminal_mv,
                                (unsigned)model->compensated_mv);
            netmon_render_linef(out, 1u, "PK%u D%u",
                                (unsigned)model->curve_peak_mv,
                                (unsigned)model->curve_drop_mv);
            netmon_render_linef(out, 2u, "SL %d",
                                (int)model->curve_slope_mv_per_min);
            netmon_render_current(out->lines[3], NETMON_FRAME_LINE_CAP,
                                  model->current_ua);
        } else if (frame_index == 3u) {
            if (model->frozen_capacity_valid) {
                netmon_render_linef(out, 0u, "CAP %u C%u",
                                    (unsigned)model->frozen_capacity_mah,
                                    (unsigned)model->frozen_capacity_confidence);
            } else {
                netmon_render_linef(out, 0u, "CAP --");
            }
            if (model->frozen_remaining_valid) {
                netmon_render_linef(out, 1u, "REM %umAh",
                                    (unsigned)model->frozen_remaining_mah);
            } else {
                netmon_render_linef(out, 1u, "REM --");
            }
            netmon_render_linef(out, 2u, "A%u Q%u M%u",
                                model->attached ? 1u : 0u,
                                model->admitted ? 1u : 0u,
                                (unsigned)model->minute_count);
            netmon_render_linef(
                out, 3u, "N%c R%c F%04lX",
                model->persistence_pending ? '!' : '+',
                model->restore_pending ? '?' :
                    (model->restored_after_reset ? '1' : '0'),
                (unsigned long)(model->persistence_failures & 0xffffu));
        } else {
            netmon_render_linef(out, 0u, "POL %s",
                                charge_policy_short(model->policy));
            netmon_render_linef(
                out, 1u, "FAC %u %c",
                (unsigned)model->charge_factor_permille,
                model->charge_factor_confident ? 'T' : 'C');
            netmon_render_linef(
                out, 2u, "STOP %u L%u",
                model->stop_requested ? 1u : 0u,
                model->supervisor_inhibit_latched ? 1u : 0u);
            netmon_render_linef(out, 3u, "REL %u",
                                model->release_inhibit_pending ? 1u : 0u);
        }
        break;
    }
    case 50u:
        netmon_render_linef(out, 0u, "RevB2 %s", l->modem_backend);
        netmon_render_text_chunks(out, l->build_hash, 1u);
        netmon_render_linef(out, 3u, "UP %lus", (unsigned long)(l->updated_ms / 1000u));
        break;
    case 51u:
        netmon_render_linef(out, 0u, "TCA %s", b->tca_present ? "ok" : "bad");
        netmon_render_linef(out, 1u, "CODEC %s", b->codec_present ? "ok" : "bad");
        netmon_render_linef(out, 2u, "RTC %s", b->rtc_present ? "ok" : "bad");
        netmon_render_linef(out, 3u, "LTC %s", b->ltc_present ? "ok" : "bad");
        break;
    case 52u:
        netmon_render_linef(out, 0u, "EDGE %lu", (unsigned long)b->shared_irq_edges);
        netmon_render_linef(out, 1u, "DRAIN %lu", (unsigned long)b->shared_irq_drains);
        netmon_render_linef(out, 2u, "T%lu R%lu L%lu",
              (unsigned long)b->shared_irq_tca_events,
              (unsigned long)b->shared_irq_rtc_events,
              (unsigned long)b->shared_irq_ltc_events);
        netmon_render_linef(out, 3u, "STUCK %lu", (unsigned long)b->shared_irq_stuck);
        break;
    case 53u:
        netmon_render_linef(out, 0u, "MASK %02X/%02X", (unsigned)b->shared_irq_last_serviced,
              (unsigned)b->shared_irq_last_errors);
        netmon_render_linef(out, 1u, "TCA %02X", (unsigned)b->shared_irq_last_tca_status);
        netmon_render_linef(out, 2u, "RTC %02X", (unsigned)b->shared_irq_last_rtc_flags);
        netmon_render_linef(out, 3u, "LTC %02X", (unsigned)b->shared_irq_last_ltc_status);
        break;
    case 54u:
        netmon_render_linef(out, 0u, "Q %u HI%u", (unsigned)l->event_queue_depth,
              (unsigned)l->event_queue_high_water);
        netmon_render_linef(out, 1u, "DROP %lu", (unsigned long)l->event_queue_drops);
        break;
    case 55u:
        netmon_render_linef(out, 0u, "HEAD %s", b->headset_inserted ? "in" : "out");
        netmon_render_linef(out, 1u, "FORCE %s", netmon_render_yes_no(b->headset_forced));
        netmon_render_linef(out, 2u, "HOOK %umV", (unsigned)b->headset_hook_mv);
        netmon_render_linef(out, 3u, "BTN %s", b->headset_hook_pressed ? "down" : "open");
        break;
    case 56u:
        netmon_render_linef(out, 0u, "VBUS %u SYS%u", b->vbus_present ? 1u : 0u,
              b->sys_int_asserted ? 1u : 0u);
        netmon_render_linef(out, 1u, "CHG %u RI%u", b->charger_connected ? 1u : 0u,
              b->modem_ri_level ? 1u : 0u);
        netmon_render_linef(out, 2u, "PG %u", b->rail_3v8_power_good ? 1u : 0u);
        break;
    case 57u:
        netmon_render_linef(out, 0u, "%02u:%02u:%02u", (unsigned)l->rtc.hour,
              (unsigned)l->rtc.minute, (unsigned)l->rtc.second);
        netmon_render_linef(out, 1u, "%04u-%02u-%02u", (unsigned)l->rtc.year,
              (unsigned)l->rtc.month, (unsigned)l->rtc.day);
        netmon_render_linef(out, 2u, "ALM %u C%u", l->rtc.alarm_enabled ? 1u : 0u,
              l->rtc.alarm_config_committed ? 1u : 0u);
        netmon_render_linef(out, 3u, "SNZ %u EV%u", l->rtc.snooze_active ? 1u : 0u,
              l->rtc.alarm_event_pending ? 1u : 0u);
        break;
    case 58u:
        netmon_render_linef(out, 0u, "LIGHT %s", netmon_render_on_off(b->backlight_on));
        netmon_render_linef(out, 1u, "VIB %s", netmon_render_on_off(b->vibra_test_enabled));
        netmon_render_linef(out, 2u, "BUZZ %s", b->buzzer_pin_configured ? "ready" : "off");
        netmon_render_linef(out, 3u, "CODEC %s", l->audio.codec_ready ? "ready" : "bad");
        break;
    case 60u:
        netmon_render_linef(out, 0u, "AUDIO %s", netmon_render_on_off(l->audio.service_active));
        netmon_render_linef(out, 1u, "ROUTE %u", (unsigned)l->audio.codec_route);
        netmon_render_linef(out, 2u, "BRIDGE %u", l->audio.bridge_active ? 1u : 0u);
        netmon_render_linef(out, 3u, "MIC %u", (unsigned)l->audio.uplink_mic_channel);
        break;
    case 61u:
        netmon_render_linef(out, 0u, "CODEC %s", l->audio.codec_ready ? "ready" : "bad");
        netmon_render_linef(out, 1u, "SPK %u M%u", (unsigned)l->audio.speaker_gain,
              l->audio.speaker_muted ? 1u : 0u);
        netmon_render_linef(out, 2u, "HP %u M%u", (unsigned)l->audio.headphone_gain,
              l->audio.headphone_muted ? 1u : 0u);
        netmon_render_linef(out, 3u, "BIAS %u", l->audio.mic_bias_hold ? 1u : 0u);
        break;
    case 62u:
        netmon_render_linef(out, 0u, "TX %lu/s", (unsigned long)l->audio.codec_tx_rate);
        netmon_render_linef(out, 1u, "RX %lu/s", (unsigned long)l->audio.codec_rx_rate);
        netmon_render_linef(out, 2u, "PK %d/%d", (int)l->audio.codec_peak_left,
              (int)l->audio.codec_peak_right);
        netmon_render_linef(out, 3u, "ABORT %lu", (unsigned long)l->audio.codec_abort_timeouts);
        break;
    case 63u:
        netmon_render_linef(out, 0u, "RX %lu/s", (unsigned long)l->audio.modem_rx_rate);
        netmon_render_linef(out, 1u, "WA %d/%d", (int)l->audio.modem_last_wa_low,
              (int)l->audio.modem_last_wa_high);
        netmon_render_linef(out, 2u, "SLOT %lu", (unsigned long)l->audio.modem_slot_mismatch);
        netmon_render_linef(out, 3u, "LOCK %lu/%lu",
              (unsigned long)l->audio.modem_relock_attempts,
              (unsigned long)l->audio.modem_relock_total);
        break;
    case 64u:
        if (frame_index == 0u) {
            netmon_render_linef(out, 0u, "TX %lu/s", (unsigned long)l->audio.modem_tx_rate);
            netmon_render_linef(out, 1u, "FIFO %lu", (unsigned long)l->audio.modem_tx_fifo_level);
            netmon_render_linef(out, 2u, "SM %u D%u%u", l->audio.modem_tx_sm_enabled ? 1u : 0u,
                  l->audio.modem_dma0_busy ? 1u : 0u,
                  l->audio.modem_dma1_busy ? 1u : 0u);
            netmon_render_linef(out, 3u, "PC %lu-%lu", (unsigned long)l->audio.modem_tx_pc_min,
                  (unsigned long)l->audio.modem_tx_pc_max);
        } else {
            netmon_render_linef(out, 0u, "DMA %08lX", (unsigned long)l->audio.modem_dma0_ctrl);
            netmon_render_linef(out, 1u, "PIO %08lX", (unsigned long)l->audio.modem_pio_fdebug);
            netmon_render_linef(out, 2u, "ABORT %lu", (unsigned long)l->audio.modem_abort_timeouts);
        }
        break;
    case 65u:
        netmon_render_linef(out, 0u, "DL %u U%lu", (unsigned)l->audio.downlink_depth,
              (unsigned long)l->audio.downlink_underflow);
        netmon_render_linef(out, 1u, "UL %u U%lu", (unsigned)l->audio.uplink_depth,
              (unsigned long)l->audio.uplink_underflow);
        netmon_render_linef(out, 2u, "OV %lu/%lu",
              (unsigned long)l->audio.downlink_overflow,
              (unsigned long)l->audio.uplink_overflow);
        netmon_render_linef(out, 3u, "FMT R%u I%u", (unsigned)l->audio.bridge_route,
              l->audio.right_invert ? 1u : 0u);
        break;
    case 66u:
        if (frame_index == 0u) {
            netmon_render_linef(out, 0u, "ACT %u P%u Q%u", l->audio.bridge_active ? 1u : 0u,
                  l->audio.bridge_start_pending ? 1u : 0u,
                  l->audio.bclk_qualifying ? 1u : 0u);
            netmon_render_linef(out, 1u, "REQ %lu ON%lu",
                  (unsigned long)l->audio.bridge_start_requests,
                  (unsigned long)l->audio.bridge_activations);
            netmon_render_linef(out, 2u, "ACQ %lums",
                  (unsigned long)l->audio.bridge_last_acquire_ms);
            netmon_render_linef(out, 3u, "MAX %lums",
                  (unsigned long)l->audio.bridge_max_acquire_ms);
        } else {
            netmon_render_linef(out, 0u, "POST %lu/%lu",
                  (unsigned long)l->core1.bridge_start_posts,
                  (unsigned long)l->core1.bridge_stop_posts);
            netmon_render_linef(out, 1u, "DSP %lums",
                  (unsigned long)l->core1.bridge_last_dispatch_ms);
            netmon_render_linef(out, 2u, "LOSS %lu",
                  (unsigned long)l->audio.bridge_bclk_losses);
            netmon_render_linef(out, 3u, "RSYNC %lu",
                  (unsigned long)l->audio.bridge_resync_failures);
        }
        break;
    case 67u:
        netmon_render_linef(out, 0u, "MIC CH%u", (unsigned)l->audio.uplink_mic_channel);
        netmon_render_linef(out, 1u, "L %d", (int)l->audio.codec_peak_left);
        netmon_render_linef(out, 2u, "R %d", (int)l->audio.codec_peak_right);
        netmon_render_linef(out, 3u, "CALL %u", l->audio.mic_in_call ? 1u : 0u);
        break;
    case 68u:
        netmon_render_linef(out, 0u, "RELOCK %lu", (unsigned long)l->audio.modem_relock_total);
        netmon_render_linef(out, 1u, "SLOT %lu", (unsigned long)l->audio.modem_slot_mismatch);
        netmon_render_linef(out, 2u, "CABRT %lu", (unsigned long)l->audio.codec_abort_timeouts);
        netmon_render_linef(out, 3u, "MABRT %lu", (unsigned long)l->audio.modem_abort_timeouts);
        break;
    case 70u:
        netmon_render_linef(out, 0u, "LCD %s", l->lcd_powered_down ? "sleep" : "awake");
        netmon_render_linef(out, 1u, "V%u T%u B%u", (unsigned)l->lcd_vop,
              (unsigned)l->lcd_temperature_coefficient,
              (unsigned)l->lcd_bias_system);
        if (l->lcd_stored_vop_valid) {
            netmon_render_linef(out, 2u, "S%u T%u B%u", (unsigned)l->lcd_stored_vop,
                  (unsigned)l->lcd_stored_temperature_coefficient,
                  (unsigned)l->lcd_stored_bias_system);
        } else {
            netmon_render_linef(out, 2u, "SAVED --");
        }
        break;
    case 71u:
        netmon_render_linef(out, 0u, "LIGHT %s", netmon_render_on_off(b->backlight_on));
        netmon_render_linef(out, 1u, "OVR %s", netmon_render_yes_no(b->backlight_override));
        netmon_render_linef(out, 2u, "LEVEL %s", netmon_render_on_off(b->backlight_override_on));
        break;
    case 72u:
        if (frame_index == 0u) {
            netmon_render_linef(out, 0u, "READY %s", netmon_render_yes_no(l->storage.ready));
            netmon_render_linef(out, 1u, "DIR %04X", (unsigned)l->storage.dirty_mask);
            netmon_render_linef(out, 2u, "DEG %04X", (unsigned)l->storage.degraded_mask);
            if (l->storage.current_unit == STORE_DIAG_NO_UNIT) {
                netmon_render_linef(out, 3u, "COMMIT idle");
            } else {
                netmon_render_linef(out, 3u, "COMMIT U%u", (unsigned)l->storage.current_unit);
            }
        } else {
            netmon_render_linef(out, 0u, "OK %lu F%lu",
                  (unsigned long)l->storage.commit_successes,
                  (unsigned long)l->storage.commit_failures);
            netmon_render_linef(out, 1u, "BUSY %lu", (unsigned long)l->storage.busy_deferrals);
            netmon_render_linef(out, 2u, "FLUSH %lu/%lu",
                  (unsigned long)l->storage.flush_incomplete,
                  (unsigned long)l->storage.flush_attempts);
            if (l->storage.last_unit == STORE_DIAG_NO_UNIT) {
                netmon_render_linef(out, 3u, "LAST --");
            } else {
                netmon_render_linef(out, 3u, "U%u S%u", (unsigned)l->storage.last_unit,
                      (unsigned)l->storage.last_status[l->storage.last_unit]);
            }
        }
        break;
    case 73u: {
        uint8_t unit = (uint8_t)(frame_index % (uint8_t)STORE_UNIT_COUNT);
        uint16_t bit = (uint16_t)(1u << unit);
        netmon_render_linef(out, 0u, "U%02u %s", (unsigned)unit, STORE_UNIT_NAMES[unit]);
        netmon_render_linef(out, 1u, "DIR %u DEG%u",
              (l->storage.dirty_mask & bit) != 0u ? 1u : 0u,
              (l->storage.degraded_mask & bit) != 0u ? 1u : 0u);
        netmon_render_linef(out, 2u, "FAIL %u",
              (unsigned)l->storage.consecutive_failures[unit]);
        netmon_render_linef(out, 3u, "STATUS %u", (unsigned)l->storage.last_status[unit]);
        break;
    }
    case 74u:
        netmon_render_linef(out, 0u, "LOOP %luus",
              (unsigned long)l->main_loop_last_us);
        netmon_render_linef(out, 1u, "MAX %luus",
              (unsigned long)l->main_loop_max_us);
        netmon_render_linef(out, 2u, "BUD %luus",
              (unsigned long)l->main_loop_budget_us);
        netmon_render_linef(out, 3u, "OVER %lu",
              (unsigned long)l->main_loop_over_budget);
        break;
    case 75u:
        if (frame_index == 0u) {
            int32_t heartbeat_delta =
                (int32_t)(l->updated_ms - l->core1.heartbeat_ms);
            uint32_t heartbeat_age = heartbeat_delta > 0
                ? (uint32_t)heartbeat_delta : 0u;
            netmon_render_linef(out, 0u, "RUN %u IDLE%u", l->core1.started ? 1u : 0u,
                  l->core1.idle_waiting ? 1u : 0u);
            netmon_render_linef(out, 1u, "HB %lums", (unsigned long)heartbeat_age);
            netmon_render_linef(out, 2u, "Q %u HI%u", (unsigned)l->core1.command_queue_depth,
                  (unsigned)l->core1.command_queue_high_water);
            netmon_render_linef(out, 3u, "DROP %lu", (unsigned long)l->core1.command_queue_drops);
        } else if (frame_index == 1u) {
            netmon_render_linef(out, 0u, "FLASH %lu/%lu",
                  (unsigned long)l->core1.flash_pause_successes,
                  (unsigned long)l->core1.flash_pause_attempts);
            netmon_render_linef(out, 1u, "PTO %lu",
                  (unsigned long)l->core1.flash_pause_timeouts);
            netmon_render_linef(out, 2u, "RTO %lu",
                  (unsigned long)l->core1.flash_resume_timeouts);
            netmon_render_linef(out, 3u, "PEND %u PARK%u",
                  l->core1.flash_pause_requested ? 1u : 0u,
                  l->core1.flash_parked ? 1u : 0u);
        } else if (frame_index == 2u) {
            netmon_render_linef(out, 0u, "GATE %lu/%lu",
                  (unsigned long)l->core1.audio_gate_successes,
                  (unsigned long)l->core1.audio_gate_requests);
            netmon_render_linef(out, 1u, "RACE %lu",
                  (unsigned long)l->core1.audio_gate_races);
            netmon_render_linef(out, 2u, "REC %lu/%lu",
                  (unsigned long)l->core1.codec_recovery_successes,
                  (unsigned long)l->core1.codec_recovery_attempts);
            netmon_render_linef(out, 3u, "FAIL %lu",
                  (unsigned long)l->core1.codec_recovery_failures);
        } else {
            netmon_render_linef(out, 0u, "C0 %lu/%lu",
                  (unsigned long)l->stack.core0.peak_used_bytes,
                  (unsigned long)l->stack.core0.minimum_margin_bytes);
            netmon_render_linef(out, 1u, "C1 %lu/%lu",
                  (unsigned long)l->stack.core1.peak_used_bytes,
                  (unsigned long)l->stack.core1.minimum_margin_bytes);
            netmon_render_linef(out, 2u, "G0 %s G1 %s",
                  l->stack.core0.canary_intact ? "OK" : "BAD",
                  l->stack.core1.canary_intact ? "OK" : "BAD");
            netmon_render_linef(out, 3u, "used/margin");
        }
        break;
    case 76u: {
        const netmon_storage_diag_t *s = &l->partitions;
        int32_t age_ms = (int32_t)(l->updated_ms - s->sampled_ms);
        uint32_t age = age_ms > 0 ? (uint32_t)age_ms / 1000u : 0u;
        if (age > 15u) {
            netmon_render_linef(out, 0u, "FS STALE");
            netmon_render_linef(out, 1u, "AGE %lus", (unsigned long)age);
            break;
        }
        if (frame_index < 6u && !s->valid) {
            netmon_render_linef(out, 0u, "FS NOT READY");
            break;
        }
        if (frame_index == 0u) {
            netmon_render_linef(out, 0u, "SYS %u/%uK", (unsigned)s->system_used_kib,
                                (unsigned)s->system_total_kib);
            netmon_render_linef(out, 1u, "USR %u/%uK", (unsigned)s->user_used_kib,
                                (unsigned)s->user_total_kib);
            unsigned free_kib = s->user_total_kib >= s->user_used_kib
                ? (unsigned)(s->user_total_kib - s->user_used_kib) : 0u;
            netmon_render_linef(out, 2u, "FREE %uK", free_kib);
            netmon_render_linef(out, 3u, "RSV %uK", (unsigned)s->reserve_kib);
        } else if (frame_index < 6u) {
            static const char *const names[] = {"CONTACTS", "INBOX", "OUTBOX", "PART SMS", "OTHER"};
            unsigned pool = frame_index - 1u;
            netmon_render_linef(out, 0u, "%s KiB", names[pool]);
            netmon_render_linef(out, 1u, "USE %u/%u", (unsigned)s->pools[pool].used_kib,
                                (unsigned)s->pools[pool].limit_kib);
            netmon_render_linef(out, 2u, "FILES %u", (unsigned)s->pools[pool].files);
            netmon_render_linef(out, 3u, "DATA %uK", (unsigned)s->pools[pool].data_kib);
        } else if (frame_index == 6u) {
            netmon_render_linef(out, 0u, "SMS %s", !s->messages_ready ? "UNREADY" :
                                s->messages_full ? "FULL" : "OK");
            netmon_render_linef(out, 1u, "IN%u OUT%u", (unsigned)s->inbox, (unsigned)s->outbox);
            netmon_render_linef(out, 2u, "PART %u Q%u", (unsigned)s->pending, (unsigned)s->queued);
            netmon_render_linef(out, 3u, "CLOCK %s", s->retention_clock_valid ? "OK" : "UNSET");
        } else {
            netmon_render_linef(out, 0u, "SMS CLEANUP");
            netmon_render_linef(out, 1u, "E %lu", (unsigned long)s->expired);
            netmon_render_linef(out, 2u, "F %lu", (unsigned long)s->filtered);
            netmon_render_linef(out, 3u, "L %lu", (unsigned long)s->lost);
        }
        break;
    }
    case 80u:
        netmon_render_linef(out, 0u, "LCD %s", l->lcd_powered_down ? "sleep" : "awake");
        netmon_render_linef(out, 1u, "LIGHT %s", netmon_render_on_off(b->backlight_on));
        netmon_render_linef(out, 2u, "3V8 %u PG%u", b->rail_3v8_enabled ? 1u : 0u,
              b->rail_3v8_power_good ? 1u : 0u);
        netmon_render_linef(out, 3u, "VBUS %u", b->vbus_present ? 1u : 0u);
        break;
    case 81u:
        netmon_render_linef(out, 0u, "CAUSE %u", (unsigned)l->sleep.wake_cause);
        netmon_render_linef(out, 1u, "RESET %08lX", (unsigned long)l->sleep.chip_reset);
        netmon_render_linef(out, 2u, "MARK %08lX", (unsigned long)l->sleep.scratch_marker);
        netmon_render_linef(out, 3u, "REQ %08lX", (unsigned long)l->sleep.current_pwrup_req);
        break;
    case 82u:
        if (frame_index == 0u) {
            netmon_render_linef(out, 0u, "CAP %lums",
                  (unsigned long)l->sleep.runtime.boot_capture_ms);
            netmon_render_linef(out, 1u, "PROBE %lums",
                  (unsigned long)l->sleep.runtime.boot_probe_ms);
            netmon_render_linef(out, 2u, "FIN %lums",
                  (unsigned long)l->sleep.runtime.boot_finish_ms);
            netmon_render_linef(out, 3u, "TOTAL %lums",
                  (unsigned long)(l->sleep.runtime.boot_finish_ms -
                                  l->sleep.runtime.boot_capture_ms));
        } else {
            netmon_render_linef(out, 0u, "C-P %lums",
                  (unsigned long)l->sleep.runtime.boot_capture_to_probe_ms);
            netmon_render_linef(out, 1u, "P-F %lums",
                  (unsigned long)l->sleep.runtime.boot_probe_to_finish_ms);
            netmon_render_linef(out, 2u, "CAUSE %u", (unsigned)l->sleep.wake_cause);
            netmon_render_linef(out, 3u, "PREV %lums",
                  (unsigned long)l->sleep.runtime.previous_awake_ms);
        }
        break;
    case 83u:
        if (frame_index == 0u) {
            netmon_render_linef(out, 0u, "ELIG %u SET%u",
                  l->sleep.runtime.eligible ? 1u : 0u,
                  l->sleep.runtime.settling ? 1u : 0u);
            netmon_render_linef(out, 1u, "WAIT %lums",
                  (unsigned long)l->sleep.runtime.settle_remaining_ms);
            netmon_render_linef(out, 2u, "TRY %lu",
                  (unsigned long)l->sleep.runtime.entry_attempts);
            netmon_render_linef(out, 3u, "ABORT %lu",
                  (unsigned long)l->sleep.runtime.returned_aborts);
        } else {
            netmon_render_linef(out, 0u, "PREV #%u",
                  (unsigned)l->sleep.runtime.previous_entry_count);
            netmon_render_linef(out, 1u, "AWAKE %lums",
                  (unsigned long)l->sleep.runtime.previous_awake_ms);
            netmon_render_linef(out, 2u, "REF %u ABN%u",
                  l->sleep.runtime.previous_refused ? 1u : 0u,
                  l->sleep.runtime.previous_abandoned ? 1u : 0u);
            netmon_render_linef(out, 3u, "LAT %lums",
                  (unsigned long)l->sleep.runtime.last_entry_latency_ms);
        }
        break;
    case 84u:
        netmon_render_linef(out, 0u, "ABORT %u", (unsigned)l->sleep.last_abort_cause);
        netmon_render_linef(out, 1u, "AT %lums", (unsigned long)l->sleep.last_abort_ms);
        netmon_render_linef(out, 2u, "COUNT %u", (unsigned)l->sleep.abort_count);
        break;
    case 85u: {
        uint8_t first = (uint8_t)((frame_index % 4u) * 4u);
        for (uint8_t row = 0u; row < NETMON_FRAME_LINE_COUNT; row++) {
            uint8_t index = (uint8_t)(first + row);
            if (index < l->sleep.abort_count) {
                netmon_render_linef(out, row, "A%u %u", (unsigned)index,
                      (unsigned)l->sleep.abort_counts[index]);
            }
        }
        break;
    }
    case 86u:
        netmon_render_linef(out, 0u, "STAMP %08lX", (unsigned long)l->sleep.prev_entry_stamp);
        netmon_render_linef(out, 1u, "INFO %08lX", (unsigned long)l->sleep.prev_entry_info);
        netmon_render_linef(out, 2u, "PWR0 %08lX", (unsigned long)l->sleep.pwrup[0]);
        netmon_render_linef(out, 3u, "SW %08lX", (unsigned long)l->sleep.last_swcore_pwrup);
        break;
    case 87u:
        netmon_render_linef(out, 0u, "SYS %luM", (unsigned long)(l->clk_sys_hz / 1000000u));
        netmon_render_linef(out, 1u, "PERI %luM", (unsigned long)(l->clk_peri_hz / 1000000u));
        netmon_render_linef(out, 2u, "USB %luM", (unsigned long)(l->clk_usb_hz / 1000000u));
        netmon_render_linef(out, 3u, "ADC %luM", (unsigned long)(l->clk_adc_hz / 1000000u));
        break;
    case 88u:
        if (frame_index == 0u) {
            uint8_t mask = l->sleep.runtime.blocker_mask;
            netmon_render_linef(out, 0u, "MDM %s", (mask & POWER_SLEEP_BLOCKER_MODEM) ? "BLOCK" : "ok");
            netmon_render_linef(out, 1u, "3V8 %s", (mask & POWER_SLEEP_BLOCKER_RAIL) ? "BLOCK" : "ok");
            netmon_render_linef(out, 2u, "CHG %s", (mask & POWER_SLEEP_BLOCKER_CHARGER) ? "BLOCK" : "ok");
            netmon_render_linef(out, 3u, "USB %s", (mask & (POWER_SLEEP_BLOCKER_VBUS |
                                               POWER_SLEEP_BLOCKER_USB)) ? "BLOCK" : "ok");
        } else {
            uint8_t mask = l->sleep.runtime.blocker_mask;
            netmon_render_linef(out, 0u, "PWR %s", (mask & POWER_SLEEP_BLOCKER_BUTTON) ? "BLOCK" : "ok");
            netmon_render_linef(out, 1u, "IRQ %s", (mask & POWER_SLEEP_BLOCKER_SHARED_IRQ) ? "BLOCK" : "ok");
            netmon_render_linef(out, 2u, "STORE %s", l->storage.dirty_mask != 0u ? "DIRTY" : "ok");
            netmon_render_linef(out, 3u, "ALM %s", l->rtc.alarm_config_committed ? "ok" : "BLOCK");
        }
        break;
    default:
        break;
    }
}
