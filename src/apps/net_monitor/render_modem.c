#include "render_internal.h"

void netmon_render_modem(uint8_t id, uint8_t frame_index,
                         const netmon_local_diag_snapshot_t *l,
                         const modem_diag_snapshot_t *m,
                         netmon_frame_t *out) {
    switch (id) {
    case 20u:
        netmon_render_linef(out, 0u, "STATE %u", (unsigned)m->runtime.state);
        netmon_render_linef(out, 1u, "AT %s", m->at_ready ? "ready" : "wait");
        netmon_render_linef(out, 2u, "OP %u K%u", (unsigned)m->runtime.operation,
              (unsigned)m->runtime.active_kind);
        netmon_render_linef(out, 3u, "REG %s", netmon_render_yes_no(m->network_registered));
        break;
    case 21u:
        if (frame_index == 0u) {
            netmon_render_linef(out, 0u, "Q %u HI%u", (unsigned)m->scheduler.normal_queue_depth,
                  (unsigned)m->scheduler.normal_queue_high_water);
            netmon_render_linef(out, 1u, "DG %u %u/%u", (unsigned)m->scheduler.active_group,
                  (unsigned)m->scheduler.active_query,
                  (unsigned)m->scheduler.active_query_count);
            netmon_render_linef(out, 2u, "ADM %lu", (unsigned long)m->scheduler.admissions);
            netmon_render_linef(out, 3u, "DONE %lu", (unsigned long)m->scheduler.completed);
        } else {
            netmon_render_linef(out, 0u, "COAL %lu", (unsigned long)m->scheduler.coalesced);
            netmon_render_linef(out, 1u, "CANCEL %lu", (unsigned long)m->scheduler.cancelled);
            netmon_render_linef(out, 2u, "FAIL %lu/%lu",
                  (unsigned long)m->scheduler.command_failures,
                  (unsigned long)m->scheduler.command_timeouts);
            netmon_render_linef(out, 3u, "MAX %lums",
                  (unsigned long)m->scheduler.max_command_latency_ms);
        }
        break;
    case 22u:
        netmon_render_linef(out, 0u, "RX %lu", (unsigned long)m->transport.rx_bytes);
        netmon_render_linef(out, 1u, "OVR %lu D%lu",
              (unsigned long)m->transport.rx_overruns,
              (unsigned long)m->transport.rx_dropped);
        netmon_render_linef(out, 2u, "LINE %lu",
              (unsigned long)m->transport.rx_line_errors);
        netmon_render_linef(out, 3u, "CTSdrop %lu",
              (unsigned long)m->transport.tx_stall_drops);
        break;
    case 23u:
        if (frame_index == 0u) {
            netmon_render_linef(out, 0u, "DTR %s", m->transport.dtr_sleep_permitted ? "sleep" : "awake");
            netmon_render_linef(out, 1u, "CTS %u RI%u", m->transport.cts_asserted ? 1u : 0u,
                  m->transport.ri_asserted ? 1u : 0u);
            netmon_render_linef(out, 2u, "SLEEP %lu", (unsigned long)m->transport.sleep_entries);
            netmon_render_linef(out, 3u, "WAKE %lu", (unsigned long)m->transport.wake_attempts);
        } else {
            netmon_render_linef(out, 0u, "LAST %lums",
                  (unsigned long)m->transport.last_wake_latency_ms);
            netmon_render_linef(out, 1u, "MAX %lums",
                  (unsigned long)m->transport.max_wake_latency_ms);
            netmon_render_linef(out, 2u, "TO %lu/%lu",
                  (unsigned long)m->transport.wake_timeouts,
                  (unsigned long)m->transport.ri_release_timeouts);
            netmon_render_linef(out, 3u, "PEND D%u R%u",
                  m->transport.dtr_wake_pending ? 1u : 0u,
                  m->transport.ri_release_pending ? 1u : 0u);
        }
        break;
    case 24u:
        if (frame_index == 0u) {
            netmon_render_linef(out, 0u, "INIT %u R%u", (unsigned)m->runtime.init_index,
                  (unsigned)m->runtime.init_retries);
            netmon_render_linef(out, 1u, "PROV %u/%u", (unsigned)m->runtime.provision_phase,
                  (unsigned)m->runtime.provision_index);
            netmon_render_linef(out, 2u, "RETRY %u", (unsigned)m->runtime.provision_retries);
            netmon_render_linef(out, 3u, "VER %s", netmon_render_yes_no(m->runtime.provisioning_verified));
        } else {
            netmon_render_linef(out, 0u, "SCHEMA %u", (unsigned)m->runtime.provisioning_schema);
            netmon_render_linef(out, 1u, "AUDIO %s", m->runtime.audio_init_ok ? "ok" : "bad");
            netmon_render_linef(out, 2u, "SMS %s", m->runtime.sms_init_ok ? "ok" : "bad");
        }
        break;
    case 25u:
        if (frame_index == 0u) {
            netmon_render_linef(out, 0u, "3V8 %u PG%u", l->board.rail_3v8_enabled ? 1u : 0u,
                  l->board.rail_3v8_power_good ? 1u : 0u);
            netmon_render_linef(out, 1u, "OWN %02X", (unsigned)l->rail_owner_mask);
            netmon_render_linef(out, 2u, "TRANS %lu", (unsigned long)l->rail_transitions);
            netmon_render_linef(out, 3u, "STAT %umV", (unsigned)l->board.modem_status_pin_mv);
        } else {
            netmon_render_linef(out, 0u, "ONOFF %u", l->board.modem_pwr_control_asserted ? 1u : 0u);
            netmon_render_linef(out, 1u, "SHDN %u", l->board.modem_hw_shutdown_asserted ? 1u : 0u);
            netmon_render_linef(out, 2u, "DTR %u RI%u", l->board.modem_dtr_level ? 1u : 0u,
                  l->board.modem_ri_level ? 1u : 0u);
            netmon_render_linef(out, 3u, "RIEDGE %lu", (unsigned long)l->board.modem_ri_edges);
        }
        break;
    case 26u:
        if (frame_index == 0u) {
            netmon_render_linef(out, 0u, "MODEL");
            netmon_render_text_chunks(out, m->identity.model, 1u);
        } else if (frame_index == 1u) {
            netmon_render_linef(out, 0u, "FIRMWARE");
            netmon_render_text_chunks(out, m->identity.firmware, 1u);
        } else {
            netmon_render_linef(out, 0u, "IMEI");
            netmon_render_text_chunks(out, m->identity.imei, 1u);
        }
        break;
    case 27u:
        if (frame_index == 0u) {
            netmon_render_linef(out, 0u, "DVI EN%u M%u", (unsigned)m->dvi.enabled,
                  (unsigned)m->dvi.mode);
            netmon_render_linef(out, 1u, "CLK %u CFG%u", (unsigned)m->dvi.clock,
                  (unsigned)m->dvi.config);
            netmon_render_linef(out, 2u, "RATE %u", (unsigned)m->dvi.sample_rate);
            netmon_render_linef(out, 3u, "WIDTH %u", (unsigned)m->dvi.sample_width);
        } else {
            netmon_render_linef(out, 0u, "AUDIO %u", (unsigned)m->dvi.audio_mode);
            netmon_render_linef(out, 1u, "EDGE %u", (unsigned)m->dvi.edge);
            netmon_render_linef(out, 2u, "INIT %s", m->runtime.audio_init_ok ? "ok" : "bad");
        }
        break;
    case 28u:
        if (frame_index == 0u) {
            netmon_render_linef(out, 0u, "ON %lu/%lu", (unsigned long)m->runtime.power_on_starts,
                  (unsigned long)m->runtime.power_on_requests);
            netmon_render_linef(out, 1u, "READY %lu", (unsigned long)m->runtime.ready_entries);
            netmon_render_linef(out, 2u, "OFF %lu/%lu",
                  (unsigned long)m->runtime.shutdown_completions,
                  (unsigned long)m->runtime.power_off_requests);
            netmon_render_linef(out, 3u, "STATE %u", (unsigned)m->runtime.state);
        } else if (frame_index == 1u) {
            netmon_render_linef(out, 0u, "REC %lu", (unsigned long)m->runtime.automatic_recoveries);
            netmon_render_linef(out, 1u, "REBOOT %lu", (unsigned long)m->runtime.controlled_restarts);
            netmon_render_linef(out, 2u, "FAIL %lu", (unsigned long)m->runtime.power_failures);
            netmon_render_linef(out, 3u, "WHY %u", (unsigned)m->runtime.last_recovery_reason);
        } else {
            netmon_render_linef(out, 0u, "SHDN %u F%u",
                  (unsigned)m->runtime.shutdown_stage,
                  (unsigned)m->runtime.shutdown_terminal_fault);
            netmon_render_linef(out, 1u, "HW %lu",
                  (unsigned long)m->runtime.graceful_shutdown_pulses);
            netmon_render_linef(out, 2u, "EMERG %lu",
                  (unsigned long)m->runtime.emergency_shutdown_pulses);
            netmon_render_linef(out, 3u, "TERM %lu",
                  (unsigned long)m->runtime.terminal_shutdown_failures);
        }
        break;
    case 29u:
        if (frame_index == 0u) {
            netmon_render_linef(out, 0u, "KIND %u OP%u", (unsigned)m->runtime.active_kind,
                  (unsigned)m->runtime.operation);
            netmon_render_linef(out, 1u, "AT ERR %lu", (unsigned long)m->command_errors);
            netmon_render_linef(out, 2u, "URC %lu", (unsigned long)m->urc_count);
        } else if (frame_index == 1u) {
            netmon_render_linef(out, 0u, "LAST CMD");
            netmon_render_text_chunks(out, m->runtime.last_command, 1u);
        } else {
            netmon_render_linef(out, 0u, "LAST LINE");
            netmon_render_text_chunks(out, m->runtime.last_line, 1u);
        }
        break;
    default:
        break;
    }
}
