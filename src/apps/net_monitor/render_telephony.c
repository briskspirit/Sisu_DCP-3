#include "render_internal.h"

void netmon_render_telephony(uint8_t id, uint8_t frame_index,
                             const modem_diag_snapshot_t *m,
                             netmon_frame_t *out) {
    const modem_diag_group_meta_t *sms =
        &m->group[MODEM_DIAG_GROUP_SMS_CONFIG];
    const modem_diag_group_meta_t *storage =
        &m->group[MODEM_DIAG_GROUP_STORAGE_CAPS];
    switch (id) {
    case 30u:
        netmon_render_linef(out, 0u, "STATE %u ID%u", (unsigned)m->calls.projected_state,
              (unsigned)m->calls.active_call_id);
        netmon_render_linef(out, 1u, "R%u W%u H%u", m->calls.ringing ? 1u : 0u,
              m->calls.waiting ? 1u : 0u, m->calls.on_hold ? 1u : 0u);
        netmon_render_linef(out, 2u, "2H %u", m->calls.second_held ? 1u : 0u);
        netmon_render_linef(out, 3u, "RES %u/%u", (unsigned)m->calls.last_result,
              (unsigned)m->calls.second_result);
        break;
    case 31u:
        if (m->calls.leg_count == 0u) {
            netmon_render_linef(out, 1u, "NO LEGS");
        } else {
            uint8_t index = (uint8_t)(frame_index % m->calls.leg_count);
            const modem_diag_call_leg_t *leg = &m->calls.legs[index];
            netmon_render_linef(out, 0u, "LEG %u/%u", (unsigned)(index + 1u),
                  (unsigned)m->calls.leg_count);
            netmon_render_linef(out, 1u, "ID%u G%u", (unsigned)leg->id,
                  (unsigned)leg->generation);
            netmon_render_linef(out, 2u, "D%u S%u", (unsigned)leg->direction,
                  (unsigned)leg->state);
            netmon_render_linef(out, 3u, "ROLE %u", (unsigned)leg->role);
        }
        break;
    case 32u:
        if (m->calls.txn_count == 0u) {
            netmon_render_linef(out, 0u, "NO TXNS");
            netmon_render_linef(out, 1u, "CLCC %s", m->calls.wants_clcc ? "want" : "idle");
            netmon_render_linef(out, 2u, "TERM %u", (unsigned)m->calls.terminal_count);
        } else {
            uint8_t index = (uint8_t)(frame_index % m->calls.txn_count);
            const modem_diag_call_txn_t *txn = &m->calls.txns[index];
            netmon_render_linef(out, 0u, "TXN %u/%u", (unsigned)(index + 1u),
                  (unsigned)m->calls.txn_count);
            netmon_render_linef(out, 1u, "TOK %lu", (unsigned long)txn->token);
            netmon_render_linef(out, 2u, "K%u S%u", (unsigned)txn->kind,
                  (unsigned)txn->state);
            netmon_render_linef(out, 3u, "ID%u G%u", (unsigned)txn->target_id,
                  (unsigned)txn->target_generation);
        }
        break;
    case 33u:
        netmon_render_linef(out, 0u, "RES %u/%u", (unsigned)m->calls.last_result,
              (unsigned)m->calls.second_result);
        netmon_render_text_chunks(out, m->call_cause.ceer, 1u);
        break;
    case 34u:
        netmon_render_linef(out, 0u, "TX %lu", (unsigned long)m->sms_sent_count);
        netmon_render_linef(out, 1u, "RX %lu", (unsigned long)m->sms_received_count);
        netmon_render_linef(out, 2u, "FULL %lu", (unsigned long)m->sms_storage_full_events);
        break;
    case 35u:
        if (frame_index == 0u) {
            if ((sms->present_fields & MODEM_DIAG_SMS_CSMS) != 0u) {
                netmon_render_linef(out, 0u, "CSMS %u", (unsigned)m->sms.csms_service);
                netmon_render_linef(out, 1u, "MTMO %u/%u", (unsigned)m->sms.csms_mt,
                      (unsigned)m->sms.csms_mo);
            } else {
                netmon_render_linef(out, 0u, "CSMS --");
                netmon_render_linef(out, 1u, "MTMO --");
            }
            if ((sms->present_fields & MODEM_DIAG_SMS_CNMI) != 0u) {
                netmon_render_linef(out, 2u, "CNMI %u/%u", (unsigned)m->sms.cnmi_mode,
                      (unsigned)m->sms.cnmi_mt);
                netmon_render_linef(out, 3u, "DS %u BF%u", (unsigned)m->sms.cnmi_ds,
                      (unsigned)m->sms.cnmi_bfr);
            } else {
                netmon_render_linef(out, 2u, "CNMI --");
                netmon_render_linef(out, 3u, "DS/BF --");
            }
        } else {
            if ((sms->present_fields & MODEM_DIAG_SMS_CSMP) != 0u) {
                netmon_render_linef(out, 0u, "CSMP %u/%u", (unsigned)m->sms.csmp_fo,
                      (unsigned)m->sms.csmp_vp);
                netmon_render_linef(out, 1u, "PID %u D%u", (unsigned)m->sms.csmp_pid,
                      (unsigned)m->sms.csmp_dcs);
            } else {
                netmon_render_linef(out, 0u, "CSMP --");
                netmon_render_linef(out, 1u, "PID/DCS --");
            }
            if ((sms->present_fields & MODEM_DIAG_SMS_CSDH) != 0u) {
                netmon_render_linef(out, 2u, "CSDH %u", (unsigned)m->sms.csdh);
            } else {
                netmon_render_linef(out, 2u, "CSDH --");
            }
            if ((sms->present_fields & MODEM_DIAG_SMS_IMS) != 0u) {
                netmon_render_linef(out, 3u, "IMS %u", (unsigned)m->sms.ims_mode);
            } else {
                netmon_render_linef(out, 3u, "IMS --");
            }
        }
        break;
    case 36u:
        if ((sms->present_fields & MODEM_DIAG_SMS_MWI) != 0u) {
            const modem_message_waiting_state_t *voice_1 =
                &m->sms.message_waiting
                     .category[MODEM_MESSAGE_WAITING_VOICE_LINE_1];
            const modem_message_waiting_state_t *voice_2 =
                &m->sms.message_waiting
                     .category[MODEM_MESSAGE_WAITING_VOICE_LINE_2];
            const modem_message_waiting_state_t *fax =
                &m->sms.message_waiting
                     .category[MODEM_MESSAGE_WAITING_FAX];
            const modem_message_waiting_state_t *email =
                &m->sms.message_waiting
                     .category[MODEM_MESSAGE_WAITING_EMAIL];
            uint32_t voice_count =
                (uint32_t)voice_1->count + (uint32_t)voice_2->count;
            if (voice_count > UINT16_MAX) {
                voice_count = UINT16_MAX;
            }
            netmon_render_linef(out, 0u, "MWI EN%u", (unsigned)m->sms.mwi_enabled);
            netmon_render_linef(out, 1u, "V %u/%u",
                  voice_1->active || voice_2->active ? 1u : 0u,
                  (unsigned)voice_count);
            netmon_render_linef(out, 2u, "FAX %u/%u", fax->active ? 1u : 0u,
                  (unsigned)fax->count);
            netmon_render_linef(out, 3u, "MAIL %u/%u", email->active ? 1u : 0u,
                  (unsigned)email->count);
        } else {
            netmon_render_linef(out, 0u, "MWI --");
            netmon_render_linef(out, 1u, "V --");
            netmon_render_linef(out, 2u, "FAX --");
            netmon_render_linef(out, 3u, "MAIL --");
        }
        break;
    case 37u:
        if ((storage->present_fields & MODEM_DIAG_STORAGE_SMS) != 0u) {
            netmon_render_linef(out, 0u, "SMS %.2s", m->storage.sms_read_store);
            netmon_render_linef(out, 1u, "USE %u/%u", (unsigned)m->storage.sms_used,
                  (unsigned)m->storage.sms_total);
        } else {
            netmon_render_linef(out, 0u, "SMS --");
            netmon_render_linef(out, 1u, "USE --");
        }
        if ((storage->present_fields & MODEM_DIAG_STORAGE_PHONEBOOK) != 0u) {
            netmon_render_linef(out, 2u, "PB %.2s", m->storage.phonebook_store);
            netmon_render_linef(out, 3u, "USE %u/%u", (unsigned)m->storage.phonebook_used,
                  (unsigned)m->storage.phonebook_total);
        } else {
            netmon_render_linef(out, 2u, "PB --");
            netmon_render_linef(out, 3u, "USE --");
        }
        break;
    default:
        break;
    }
}
