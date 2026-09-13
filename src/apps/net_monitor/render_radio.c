#include "render_internal.h"

void netmon_render_radio(uint8_t id, uint8_t frame_index,
                         const modem_diag_snapshot_t *m,
                         netmon_frame_t *out) {
    const modem_diag_group_meta_t *serving =
        &m->group[MODEM_DIAG_GROUP_SERVING];
    const modem_diag_group_meta_t *reg =
        &m->group[MODEM_DIAG_GROUP_REGISTRATION];
    const modem_diag_group_meta_t *policy =
        &m->group[MODEM_DIAG_GROUP_RADIO_POLICY];
    const modem_diag_group_meta_t *tuner =
        &m->group[MODEM_DIAG_GROUP_TUNER];
    switch (id) {
    case 1u:
        if ((serving->present_fields &
             (MODEM_DIAG_SERVING_RAT | MODEM_DIAG_SERVING_BAND)) ==
            (MODEM_DIAG_SERVING_RAT | MODEM_DIAG_SERVING_BAND)) {
            netmon_render_linef(out, 0u, "%s B%u", netmon_render_rat_text(m->serving.rat),
                  (unsigned)m->serving.band);
        } else {
            netmon_render_linef(out, 0u, "RAT/BAND --");
        }
        if ((serving->present_fields & MODEM_DIAG_SERVING_CHANNEL) != 0u) {
            netmon_render_linef(out, 1u, "CH %lu", (unsigned long)m->serving.channel);
        } else {
            netmon_render_linef(out, 1u, "CH --");
        }
        if ((serving->present_fields & MODEM_DIAG_SERVING_PCI) != 0u) {
            netmon_render_linef(out, 2u, "PCI %u", (unsigned)m->serving.pci);
        } else {
            netmon_render_linef(out, 2u, "PCI --");
        }
        if ((serving->present_fields & MODEM_DIAG_SERVING_BAND) != 0u &&
            m->serving.inferred_rf_state >= 1u &&
            m->serving.inferred_rf_state <= 4u) {
            netmon_render_linef(out, 3u, "RF%u %s", (unsigned)m->serving.inferred_rf_state,
                  m->serving.inferred_rf_tuned ? "calc" : "weak");
        } else {
            netmon_render_linef(out, 3u, "RF --");
        }
        break;
    case 2u:
        if (frame_index == 0u) {
            if ((serving->present_fields & MODEM_DIAG_SERVING_RSSI) != 0u) {
                netmon_render_linef(out, 0u, "RSSI %ddBm", (int)m->serving.rssi_dbm);
            } else {
                netmon_render_linef(out, 0u, "RSSI --");
            }
            if ((serving->present_fields & MODEM_DIAG_SERVING_RSRP) != 0u) {
                netmon_render_linef(out, 1u, "RSRP %d", (int)m->serving.rsrp_dbm);
            } else {
                netmon_render_linef(out, 1u, "RSRP --");
            }
            if ((serving->present_fields & MODEM_DIAG_SERVING_RSRQ) != 0u) {
                netmon_render_half_db(out->lines[2], NETMON_FRAME_LINE_CAP,
                               "RSRQ", m->serving.rsrq_db_x2);
            } else {
                netmon_render_linef(out, 2u, "RSRQ --");
            }
            if ((serving->present_fields & MODEM_DIAG_SERVING_SINR) != 0u) {
                netmon_render_tenth(out->lines[3], NETMON_FRAME_LINE_CAP,
                             "SINR", m->serving.sinr_db_x10);
            } else {
                netmon_render_linef(out, 3u, "SINR --");
            }
        } else {
            if ((serving->present_fields & MODEM_DIAG_SERVING_TX_POWER) != 0u) {
                netmon_render_tenth(out->lines[0], NETMON_FRAME_LINE_CAP,
                             "TX", m->serving.tx_power_dbm_x10);
            } else {
                netmon_render_linef(out, 0u, "TX --");
            }
            if ((serving->present_fields &
                 (MODEM_DIAG_SERVING_RAT | MODEM_DIAG_SERVING_BAND)) ==
                (MODEM_DIAG_SERVING_RAT | MODEM_DIAG_SERVING_BAND)) {
                netmon_render_linef(out, 1u, "%s B%u", netmon_render_rat_text(m->serving.rat),
                      (unsigned)m->serving.band);
            } else {
                netmon_render_linef(out, 1u, "RAT/BAND --");
            }
        }
        break;
    case 3u:
        if (frame_index == 0u) {
            if ((serving->present_fields & MODEM_DIAG_SERVING_PLMN) != 0u) {
                netmon_render_linef(out, 0u, "PLMN %s%s", m->serving.mcc, m->serving.mnc);
            } else {
                netmon_render_linef(out, 0u, "PLMN --");
            }
            netmon_render_text_chunks(out,
                (serving->present_fields & MODEM_DIAG_SERVING_OPERATOR) != 0u
                    ? m->serving.operator_name : "--", 1u);
        } else {
            netmon_render_linef(out, 0u, "AREA %s",
                  (serving->present_fields & MODEM_DIAG_SERVING_AREA) != 0u
                      ? m->serving.area_code : "--");
            netmon_render_linef(out, 1u, "CELL %s",
                  (serving->present_fields & MODEM_DIAG_SERVING_CELL) != 0u
                      ? m->serving.cell_id : "--");
            if ((serving->present_fields & MODEM_DIAG_SERVING_PCI) != 0u) {
                netmon_render_linef(out, 2u, "PCI %u", (unsigned)m->serving.pci);
            } else {
                netmon_render_linef(out, 2u, "PCI --");
            }
        }
        break;
    case 4u:
        if ((reg->present_fields & MODEM_DIAG_REG_CS) != 0u) {
            if (m->registration.cs_act == UINT8_MAX) {
                netmon_render_linef(out, 0u, "CS %u A--", (unsigned)m->registration.cs_stat);
            } else {
                netmon_render_linef(out, 0u, "CS %u A%u", (unsigned)m->registration.cs_stat,
                      (unsigned)m->registration.cs_act);
            }
        } else {
            netmon_render_linef(out, 0u, "CS --");
        }
        if ((reg->present_fields & MODEM_DIAG_REG_PS) != 0u) {
            if (m->registration.ps_act == UINT8_MAX) {
                netmon_render_linef(out, 1u, "PS %u A--", (unsigned)m->registration.ps_stat);
            } else {
                netmon_render_linef(out, 1u, "PS %u A%u", (unsigned)m->registration.ps_stat,
                      (unsigned)m->registration.ps_act);
            }
        } else {
            netmon_render_linef(out, 1u, "PS --");
        }
        if ((reg->present_fields & MODEM_DIAG_REG_EPS) != 0u) {
            if (m->registration.eps_act == UINT8_MAX) {
                netmon_render_linef(out, 2u, "EPS %u A--",
                      (unsigned)m->registration.eps_stat);
            } else {
                netmon_render_linef(out, 2u, "EPS %u A%u",
                      (unsigned)m->registration.eps_stat,
                      (unsigned)m->registration.eps_act);
            }
        } else {
            netmon_render_linef(out, 2u, "EPS --");
        }
        if ((reg->present_fields & MODEM_DIAG_REG_IMS) != 0u) {
            netmon_render_linef(out, 3u, "IMS %u", (unsigned)m->registration.ims_stat);
        } else {
            netmon_render_linef(out, 3u, "IMS --");
        }
        break;
    case 5u:
        netmon_render_linef(out, 0u, "AT%s REG%u", m->at_ready ? "ok" : "wait",
              m->network_registered ? 1u : 0u);
        if ((serving->present_fields &
             (MODEM_DIAG_SERVING_MM | MODEM_DIAG_SERVING_RRC)) ==
            (MODEM_DIAG_SERVING_MM | MODEM_DIAG_SERVING_RRC)) {
            netmon_render_linef(out, 1u, "MM %u RRC%u", (unsigned)m->serving.mm_state,
                  (unsigned)m->serving.rrc_state);
        } else {
            netmon_render_linef(out, 1u, "MM/RRC --");
        }
        if ((serving->present_fields & MODEM_DIAG_SERVING_DOMAIN) != 0u) {
            netmon_render_linef(out, 2u, "DOM %u", (unsigned)m->serving.service_domain);
        } else {
            netmon_render_linef(out, 2u, "DOM --");
        }
        if ((serving->present_fields & MODEM_DIAG_SERVING_DRX) != 0u) {
            netmon_render_linef(out, 3u, "DRX %ums", (unsigned)m->serving.drx_ms);
        } else {
            netmon_render_linef(out, 3u, "DRX --");
        }
        break;
    case 6u:
        if ((policy->present_fields & MODEM_DIAG_POLICY_SCAN_CONFIG) != 0u) {
            netmon_render_linef(out, 0u, "SCAN %us",
                  (unsigned)m->radio_policy.scan_timer_s);
        } else {
            netmon_render_linef(out, 0u, "SCAN --");
        }
        if ((policy->present_fields & MODEM_DIAG_POLICY_SCAN_REMAINING) != 0u) {
            netmon_render_linef(out, 1u, "LEFT %us",
                  (unsigned)m->radio_policy.scan_remaining_s);
        } else {
            netmon_render_linef(out, 1u, "LEFT --");
        }
        if ((policy->present_fields & MODEM_DIAG_POLICY_COPS) != 0u) {
            if (m->radio_policy.cops_act == UINT8_MAX) {
                netmon_render_linef(out, 2u, "COPS %u/--",
                      (unsigned)m->radio_policy.cops_mode);
            } else {
                netmon_render_linef(out, 2u, "COPS %u/%u",
                      (unsigned)m->radio_policy.cops_mode,
                      (unsigned)m->radio_policy.cops_act);
            }
        } else {
            netmon_render_linef(out, 2u, "COPS --");
        }
        break;
    case 7u:
        if (frame_index == 0u) {
            if ((policy->present_fields & MODEM_DIAG_POLICY_ENS) != 0u) {
                netmon_render_linef(out, 0u, "ENS %u", (unsigned)m->radio_policy.ens);
            } else {
                netmon_render_linef(out, 0u, "ENS --");
            }
            if ((policy->present_fields & MODEM_DIAG_POLICY_FWSWITCH) != 0u) {
                netmon_render_linef(out, 1u, "FW %u S%u",
                      (unsigned)m->radio_policy.firmware_image,
                      (unsigned)m->radio_policy.firmware_storage);
            } else {
                netmon_render_linef(out, 1u, "FW --");
            }
            if ((policy->present_fields & MODEM_DIAG_POLICY_FWAUTOSIM) != 0u) {
                netmon_render_linef(out, 2u, "AUTO %u",
                      (unsigned)m->radio_policy.firmware_auto_sim);
            } else {
                netmon_render_linef(out, 2u, "AUTO --");
            }
            netmon_render_linef(out, 3u, "COPS NAME");
        } else {
            netmon_render_linef(out, 0u, "COPS NAME");
            netmon_render_text_chunks(out,
                (policy->present_fields & MODEM_DIAG_POLICY_COPS) != 0u
                    ? m->radio_policy.cops_operator : "--", 1u);
        }
        break;
    case 8u:
        if (frame_index == 0u) {
            if ((policy->present_fields & MODEM_DIAG_POLICY_WS46) != 0u) {
                netmon_render_linef(out, 0u, "WS46 %u", (unsigned)m->radio_policy.ws46);
            } else {
                netmon_render_linef(out, 0u, "WS46 --");
            }
            if ((policy->present_fields & MODEM_DIAG_POLICY_SELBNDMODE) != 0u) {
                netmon_render_linef(out, 1u, "MODE %u",
                      (unsigned)m->radio_policy.select_band_mode);
            } else {
                netmon_render_linef(out, 1u, "MODE --");
            }
            if ((policy->present_fields &
                 (MODEM_DIAG_POLICY_BND | MODEM_DIAG_POLICY_BNDRAM)) ==
                (MODEM_DIAG_POLICY_BND | MODEM_DIAG_POLICY_BNDRAM)) {
                netmon_render_linef(out, 2u, "GSM %u/%u",
                      (unsigned)m->radio_policy.bnd_gsm,
                      (unsigned)m->radio_policy.bndram_gsm);
                netmon_render_linef(out, 3u, "WCD %u/%u",
                      (unsigned)m->radio_policy.bnd_wcdma,
                      (unsigned)m->radio_policy.bndram_wcdma);
            } else {
                netmon_render_linef(out, 2u, "GSM --");
                netmon_render_linef(out, 3u, "WCD --");
            }
        } else if (frame_index == 1u) {
            netmon_render_linef(out, 0u, "BND LTE");
            netmon_render_text_chunks(out,
                (policy->present_fields & MODEM_DIAG_POLICY_BND) != 0u
                    ? m->radio_policy.bnd_lte : "--", 1u);
        } else {
            netmon_render_linef(out, 0u, "RAM LTE");
            netmon_render_text_chunks(out,
                (policy->present_fields & MODEM_DIAG_POLICY_BNDRAM) != 0u
                    ? m->radio_policy.bndram_lte : "--", 1u);
        }
        break;
    case 9u:
        if (frame_index == 0u) {
            netmon_render_linef(out, 0u, "STUNE %s",
                  (tuner->present_fields & MODEM_DIAG_TUNER_ENABLED) != 0u
                      ? netmon_render_on_off(m->tuner.enabled) : "--");
            if ((tuner->present_fields & MODEM_DIAG_TUNER_TABLE) != 0u) {
                netmon_render_linef(out, 1u, "ROWS %u", (unsigned)m->tuner.row_count);
                netmon_render_linef(out, 2u, "EXACT %s", netmon_render_yes_no(m->tuner.table_exact));
                netmon_render_linef(out, 3u, "FULL %s", netmon_render_yes_no(m->tuner.table_complete));
            } else {
                netmon_render_linef(out, 1u, "ROWS --");
                netmon_render_linef(out, 2u, "EXACT --");
                netmon_render_linef(out, 3u, "FULL --");
            }
        } else {
            netmon_render_linef(out, 0u, "MASK HI");
            if ((tuner->present_fields &
                 MODEM_DIAG_TUNER_SUPPORTED_MASK) != 0u) {
                netmon_render_linef(out, 1u, "%08lX",
                      (unsigned long)(m->tuner.supported_mask >> 32u));
            } else {
                netmon_render_linef(out, 1u, "--");
            }
            netmon_render_linef(out, 2u, "MASK LO");
            if ((tuner->present_fields &
                 MODEM_DIAG_TUNER_SUPPORTED_MASK) != 0u) {
                netmon_render_linef(out, 3u, "%08lX",
                      (unsigned long)m->tuner.supported_mask);
            } else {
                netmon_render_linef(out, 3u, "--");
            }
        }
        break;
    case 10u:
    case 11u:
    case 12u:
    case 13u: {
        uint8_t row = (uint8_t)(id - 10u);
        if (row >= m->tuner.row_count ||
            (tuner->present_fields & MODEM_DIAG_TUNER_TABLE) == 0u) {
            netmon_render_linef(out, 1u, "NO ROW");
            break;
        }
        const modem_diag_tuner_row_t *r = &m->tuner.rows[row];
        netmon_render_linef(out, 0u, "RF%u C%u/%u", (unsigned)(row + 1u),
              (unsigned)r->ctrl1, (unsigned)r->ctrl2);
        netmon_render_linef(out, 1u, "HI %08lX", (unsigned long)(r->band_mask >> 32u));
        netmon_render_linef(out, 2u, "LO %08lX", (unsigned long)r->band_mask);
        netmon_render_linef(out, 3u, "Exact %s", netmon_render_yes_no(m->tuner.table_exact));
        break;
    }
    case 14u:
        if (frame_index == 0u) {
            const modem_diag_group_meta_t *packet =
                &m->group[MODEM_DIAG_GROUP_PACKET];
            if ((packet->present_fields & MODEM_DIAG_PACKET_ATTACH) != 0u) {
                netmon_render_linef(out, 0u, "ATT %u", (unsigned)m->packet.attached);
            } else {
                netmon_render_linef(out, 0u, "ATT --");
            }
            if ((packet->present_fields & MODEM_DIAG_PACKET_CONTEXTS) != 0u) {
                netmon_render_linef(out, 1u, "CTX %u/%u",
                      (unsigned)m->packet.active_context_count,
                      (unsigned)m->packet.context_count);
                netmon_render_linef(out, 2u, "CID %u S%u",
                      (unsigned)m->packet.first_cid,
                      (unsigned)m->packet.first_status);
            } else {
                netmon_render_linef(out, 1u, "CTX --");
                netmon_render_linef(out, 2u, "CID --");
            }
            netmon_render_text_chunks_mark_truncated(out,
                (packet->present_fields & MODEM_DIAG_PACKET_APN) != 0u
                    ? m->packet.apn : "--", 3u,
                (packet->present_fields &
                 MODEM_DIAG_PACKET_APN_TRUNCATED) != 0u);
        } else {
            const modem_diag_group_meta_t *packet =
                &m->group[MODEM_DIAG_GROUP_PACKET];
            netmon_render_text_chunks_mark_truncated(out,
                (packet->present_fields & MODEM_DIAG_PACKET_ADDRESS) != 0u
                    ? m->packet.address : "--", 0u,
                (packet->present_fields &
                 MODEM_DIAG_PACKET_ADDRESS_TRUNCATED) != 0u);
        }
        break;
    case 15u:
        netmon_render_linef(out, 0u, "CHK %s", netmon_render_yes_no(m->sim_checked));
        netmon_render_linef(out, 1u, "SIM %s", m->sim_present ? "present" : "absent");
        if ((m->group[MODEM_DIAG_GROUP_SIM].present_fields &
             MODEM_DIAG_SIM_QSS) != 0u) {
            netmon_render_linef(out, 2u, "QSS %u/%u", (unsigned)m->sim.qss_mode,
                  (unsigned)m->sim.qss_status);
        } else {
            netmon_render_linef(out, 2u, "QSS --");
        }
        if ((m->group[MODEM_DIAG_GROUP_SIM].present_fields &
             MODEM_DIAG_SIM_CPIN) != 0u) {
            netmon_render_linef(out, 3u, "CPIN %.7s", m->sim.cpin);
        } else {
            netmon_render_linef(out, 3u, "CPIN --");
        }
        break;
    case 16u:
        netmon_render_linef(out, 0u, "VOICE %s", m->runtime.audio_init_ok ? "ready" : "degrad");
        if ((m->group[MODEM_DIAG_GROUP_VOICE].present_fields &
             MODEM_DIAG_VOICE_IMS_REG) != 0u) {
            netmon_render_linef(out, 1u, "IMS %u", (unsigned)m->voice.ims_registration);
        } else {
            netmon_render_linef(out, 1u, "IMS --");
        }
        if ((m->group[MODEM_DIAG_GROUP_VOICE].present_fields &
             MODEM_DIAG_VOICE_DOMAIN) != 0u) {
            netmon_render_linef(out, 2u, "DOMAIN %u", (unsigned)m->voice.service_domain);
        } else {
            netmon_render_linef(out, 2u, "DOMAIN --");
        }
        netmon_render_linef(out, 3u, "REG %s", netmon_render_yes_no(m->network_registered));
        break;
    case 17u:
        netmon_render_linef(out, 0u, "TEMP %dC", (int)m->temperature.celsius);
        netmon_render_linef(out, 1u, "LEVEL %d", (int)m->temperature.level);
        break;
    default:
        (void)policy;
        break;
    }
}
