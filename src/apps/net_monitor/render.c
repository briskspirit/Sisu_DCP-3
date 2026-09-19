#include "render_internal.h"

#include <string.h>

static const char *diag_error_text(modem_diag_error_t error) {
    switch (error) {
    case MODEM_DIAG_ERROR_REJECTED: return "AT REJECT";
    case MODEM_DIAG_ERROR_TIMEOUT: return "TIMEOUT";
    case MODEM_DIAG_ERROR_MALFORMED: return "BAD DATA";
    case MODEM_DIAG_ERROR_CANCELLED: return "CANCELLED";
    case MODEM_DIAG_ERROR_UNAVAILABLE: return "UNSUP";
    case MODEM_DIAG_ERROR_NONE:
    default: return "ERROR";
    }
}
static bool diag_error_uses_retry_grace(modem_diag_error_t error) {
    return error == MODEM_DIAG_ERROR_REJECTED ||
           error == MODEM_DIAG_ERROR_TIMEOUT ||
           error == MODEM_DIAG_ERROR_MALFORMED;
}

static uint32_t query_age_limit_ms(const netmon_page_descriptor_t *page) {
    uint32_t limit_ms = page->stale_after_ms;
    if (page->refresh_period_ms == 0u) {
        return limit_ms;
    }

    uint32_t retry_grace_ms = UINT32_MAX;
    if (page->refresh_period_ms <=
        UINT32_MAX / NETMON_QUERY_FAILURE_GRACE_ATTEMPTS) {
        retry_grace_ms = page->refresh_period_ms *
                         NETMON_QUERY_FAILURE_GRACE_ATTEMPTS;
    }
    return retry_grace_ms > limit_ms ? retry_grace_ms : limit_ms;
}

static void format_query_failure(const modem_diag_group_meta_t *meta,
                                 netmon_frame_t *frame) {
    netmon_render_linef(frame, 1u, "%s",
          meta->last_error != MODEM_DIAG_ERROR_NONE
              ? diag_error_text(meta->last_error) : "STALE DATA");
}

static bool query_gate(const netmon_page_descriptor_t *page,
                       uint32_t capabilities, uint32_t now_ms,
                       const modem_diag_snapshot_t *modem,
                       netmon_frame_t *frame) {
    if (!netmon_page_supported(page, capabilities)) {
        netmon_render_linef(frame, 1u, "UNSUP");
        return false;
    }
    if (page->provider != NETMON_PROVIDER_MODEM_QUERY) {
        return true;
    }
    if (modem == NULL || !modem->backend_available) {
        netmon_render_linef(frame, 1u, "NO MODEM");
        return false;
    }
    modem_diag_group_t group = netmon_page_modem_group(page);
    if (group <= MODEM_DIAG_GROUP_NONE || group >= MODEM_DIAG_GROUP_COUNT) {
        return true;
    }
    const modem_diag_group_meta_t *meta = &modem->group[group];
    if (meta->state == MODEM_DIAG_STATE_UNSUPPORTED) {
        netmon_render_linef(frame, 1u, "UNSUP");
        return false;
    }
    if (!modem->at_ready) {
        netmon_render_linef(frame, 1u, "AT STARTUP");
        return false;
    }
    if (modem->calls.projected_state != MODEM_CALL_IDLE) {
        netmon_render_linef(frame, 1u, "QUERY PAUSED");
        return false;
    }
    bool has_sample = meta->sequence != 0u;
    bool retries_automatically = page->refresh_period_ms != 0u;
    bool has_failed_refresh = meta->consecutive_failures != 0u &&
        diag_error_uses_retry_grace(meta->last_error);
    bool failure_limit_reached =
        meta->consecutive_failures >= NETMON_QUERY_FAILURE_GRACE_ATTEMPTS;
    uint32_t age_limit_ms = query_age_limit_ms(page);
    bool age_limit_reached = has_sample && age_limit_ms != 0u &&
        (uint32_t)(now_ms - meta->last_success_ms) > age_limit_ms;

    if (meta->state == MODEM_DIAG_STATE_PENDING) {
        if (has_sample && !failure_limit_reached && !age_limit_reached) {
            frame->freshness_marker = has_failed_refresh ? '!' : '?';
            return true;
        }
        if ((failure_limit_reached || age_limit_reached) &&
            meta->last_error != MODEM_DIAG_ERROR_NONE) {
            format_query_failure(meta, frame);
        } else {
            netmon_render_linef(frame, 1u, "READING...");
        }
        return false;
    }
    if (meta->state == MODEM_DIAG_STATE_STALE ||
        meta->state == MODEM_DIAG_STATE_ERROR) {
        bool grace_active = retries_automatically && has_failed_refresh &&
            !failure_limit_reached;
        if (has_sample && grace_active && !age_limit_reached) {
            frame->freshness_marker = '!';
            return true;
        }
        if (!has_sample && grace_active) {
            netmon_render_linef(frame, 1u, "READING...");
            return false;
        }
        format_query_failure(meta, frame);
        return false;
    }
    if (meta->state != MODEM_DIAG_STATE_FRESH || meta->sequence == 0u) {
        netmon_render_linef(frame, 1u, "NO DATA");
        return false;
    }
    if (age_limit_reached) {
        netmon_render_linef(frame, 1u, "STALE DATA");
        return false;
    }
    return true;
}

uint8_t netmon_frame_count(const netmon_page_descriptor_t *page,
                           const modem_diag_snapshot_t *modem) {
    if (page == NULL) {
        return 1u;
    }
    switch (page->id) {
    case 2u:
    case 3u:
    case 7u:
    case 9u:
    case 14u:
    case 21u:
    case 23u:
    case 24u:
    case 25u:
    case 27u:
    case 35u:
    case 64u:
    case 66u:
    case 72u:
    case 82u:
    case 83u:
    case 88u:
    case 89u:
    case 92u:
    case 95u:
    case 97u:
    case 98u:
    case 99u:
        return 2u;
    case 28u:
        return 3u;
    case 47u:
    case 48u:
        return 5u;
    case 8u:
    case 26u:
    case 29u:
        return 3u;
    case 75u:
        return 4u;
    case 76u:
        return 8u;
    case 31u:
        return modem != NULL && modem->calls.leg_count != 0u
            ? modem->calls.leg_count : 1u;
    case 32u:
        return modem != NULL && modem->calls.txn_count != 0u
            ? modem->calls.txn_count : 1u;
    case 73u:
        return (uint8_t)STORE_UNIT_COUNT;
    case 85u:
        return 4u;
    default:
        return 1u;
    }
}

static netmon_render_owner_t read_only_owner_for_id(uint8_t id) {
    if (id >= 1u && id <= 17u) {
        return NETMON_RENDER_OWNER_RADIO;
    }
    if (id >= 20u && id <= 29u) {
        return NETMON_RENDER_OWNER_MODEM;
    }
    if (id >= 30u && id <= 38u) {
        return NETMON_RENDER_OWNER_TELEPHONY;
    }
    if (id >= 40u && id <= 89u) {
        return NETMON_RENDER_OWNER_LOCAL;
    }
    return NETMON_RENDER_OWNER_NONE;
}

netmon_render_owner_t netmon_render_owner_for_page(
    const netmon_page_descriptor_t *page) {
    if (page == NULL) {
        return NETMON_RENDER_OWNER_NONE;
    }
    if ((page->behavior_flags & NETMON_PAGE_EDITABLE) != 0u) {
        return NETMON_RENDER_OWNER_CONTROL;
    }
    return read_only_owner_for_id(page->id);
}

void netmon_format_frame(const netmon_page_descriptor_t *page,
                         uint8_t frame_index,
                         uint32_t capabilities,
                         uint32_t now_ms,
                         const netmon_local_diag_snapshot_t *local,
                         const modem_diag_snapshot_t *modem,
                         const netmon_control_snapshot_t *control,
                         netmon_frame_t *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->freshness_marker = ' ';
    if (page == NULL) {
        netmon_render_linef(out, 1u, "NO TEST");
        return;
    }
    if ((page->behavior_flags & NETMON_PAGE_EDITABLE) != 0u) {
        if (!netmon_page_supported(page, capabilities)) {
            netmon_render_linef(out, 1u, "UNSUP");
            return;
        }
        if (page->provider != NETMON_PROVIDER_LOCAL &&
            (modem == NULL || !modem->backend_available)) {
            netmon_render_linef(out, 1u, "NO MODEM");
            return;
        }
        if (page->provider != NETMON_PROVIDER_LOCAL &&
            !modem->at_ready) {
            netmon_render_linef(out, 1u, "AT STARTUP");
            return;
        }
    } else if (!query_gate(page, capabilities, now_ms, modem, out)) {
        return;
    }
    uint8_t count_value = netmon_frame_count(page, modem);
    if (count_value == 0u) {
        count_value = 1u;
    }
    frame_index = (uint8_t)(frame_index % count_value);

    netmon_render_owner_t owner = netmon_render_owner_for_page(page);
    if (owner == NETMON_RENDER_OWNER_CONTROL && control == NULL) {
        /*
         * Preserve the old dispatcher's fall-through for a missing control
         * snapshot. Registered page 89 reaches the local formatter (which
         * intentionally has no page-89 body); pages 90-99 report UNSUP.
         */
        owner = read_only_owner_for_id(page->id);
    }

    switch (owner) {
    case NETMON_RENDER_OWNER_RADIO:
        netmon_render_radio(page->id, frame_index, modem, out);
        break;
    case NETMON_RENDER_OWNER_MODEM:
        netmon_render_modem(page->id, frame_index, local, modem, out);
        break;
    case NETMON_RENDER_OWNER_TELEPHONY:
        netmon_render_telephony(page->id, frame_index, modem, out);
        break;
    case NETMON_RENDER_OWNER_LOCAL:
        if (local != NULL) {
            netmon_render_local(page->id, frame_index, local, out);
            break;
        }
        netmon_render_linef(out, 1u, "UNSUP");
        break;
    case NETMON_RENDER_OWNER_CONTROL:
        netmon_render_control(page->id, frame_index, control, out);
        break;
    case NETMON_RENDER_OWNER_NONE:
    default:
        netmon_render_linef(out, 1u, "UNSUP");
        break;
    }
}
