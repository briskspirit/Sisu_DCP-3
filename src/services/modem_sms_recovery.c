#include "services/modem_sms_recovery.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECOVERY_RETRY_MS 60000u
#define RECOVERY_SLOT_LIMIT 512u

static bool selection_counts(const char *line, uint16_t *used, uint16_t *total) {
    const char *p = line + 6u;
    unsigned long values[6];
    for (unsigned i = 0u; i < 6u; i++) {
        while (*p == ' ') p++;
        if (*p < '0' || *p > '9') return false;
        char *end;
        values[i] = strtoul(p, &end, 10);
        if (values[i] > UINT16_MAX) return false;
        p = end;
        while (*p == ' ') p++;
        if (i < 5u) { if (*p++ != ',') return false; }
        else if (*p != '\0') return false;
    }
    if (values[0] > values[1] || values[1] > RECOVERY_SLOT_LIMIT) return false;
    *used = (uint16_t)values[0]; *total = (uint16_t)values[1];
    return true;
}

void modem_sms_recovery_reset(modem_sms_recovery_t *r) {
    uint32_t recovered = r->recovered, failures = r->failures;
    memset(r, 0, sizeof(*r));
    r->recovered = recovered;
    r->failures = failures;
}

void modem_sms_recovery_request(modem_sms_recovery_t *r) {
    if (r->step != MODEM_SMS_RECOVERY_NONE || r->pending) r->again = true;
    else r->pending = true;
}

static void done(modem_sms_recovery_t *r, uint32_t now) {
    r->step = MODEM_SMS_RECOVERY_NONE;
    r->issued = false;
    r->pending = r->again || r->retry;
    r->retry_at = r->retry ? now + RECOVERY_RETRY_MS : now;
    r->again = false;
}

void modem_sms_recovery_defer(modem_sms_recovery_t *r, uint32_t now) {
    r->failures++;
    r->retry = true;
    done(r, now);
}

static void advance(modem_sms_recovery_t *r, uint32_t now) {
    if (r->header_seen && r->remaining > 0u) r->remaining--;
    r->index++;
    if (r->index > r->capacity || r->remaining == 0u) done(r, now);
    else r->step = MODEM_SMS_RECOVERY_READ;
}

bool modem_sms_recovery_command(modem_sms_recovery_t *r, uint32_t now,
                                char *command, size_t cap) {
    if (r->issued || r->step == MODEM_SMS_RECOVERY_STORE) return false;
    if (r->step == MODEM_SMS_RECOVERY_NONE) {
        if (!r->pending || (r->retry && (int32_t)(now - r->retry_at) < 0)) return false;
        r->pending = r->retry = false;
        r->selected = false;
        r->step = MODEM_SMS_RECOVERY_SELECT;
    }
    int n;
    if (r->step == MODEM_SMS_RECOVERY_SELECT) n = snprintf(command, cap, "AT+CPMS=\"ME\"");
    else if (r->step == MODEM_SMS_RECOVERY_DELETE) n = snprintf(command, cap, "AT+CMGD=%u,0", r->index);
    else n = snprintf(command, cap, "AT+CMGR=%u", r->index);
    if (n < 0 || (size_t)n >= cap) return false;
    r->issued = true;
    r->header_seen = r->payload_seen = r->readable = r->verify_match = false;
    return true;
}

bool modem_sms_recovery_line(modem_sms_recovery_t *r, const char *line,
                             char *delivery_header, size_t cap) {
    if (cap == 0u) return false;
    delivery_header[0] = '\0';
    if (r->step == MODEM_SMS_RECOVERY_SELECT && strncmp(line, "+CPMS:", 6u) == 0) {
        uint16_t used, total;
        if (!r->header_seen && selection_counts(line, &used, &total) &&
            used <= total && total <= RECOVERY_SLOT_LIMIT) {
            r->remaining = used; r->capacity = total; r->selected = true;
        } else r->selected = false;
        r->header_seen = true;
        return true;
    }
    if ((r->step != MODEM_SMS_RECOVERY_READ && r->step != MODEM_SMS_RECOVERY_VERIFY) ||
        strncmp(line, "+CMGR:", 6u) != 0) return false;
    bool first_header = !r->header_seen;
    r->header_seen = true;
    const char *status = line + 6u;
    while (*status == ' ') status++;
    const char *body = strchr(status, ',');
    r->readable = first_header && body != NULL &&
        ((body - status == 12 && strncmp(status, "\"REC UNREAD\"", 12u) == 0) ||
         (body - status == 10 && strncmp(status, "\"REC READ\"", 10u) == 0));
    /* CMGR's DELIVER fields are exactly CMT's after removing <stat>.
     * Still frame unsupported records' bodies; body text is never an AT URC. */
    int n = snprintf(delivery_header, cap, "+CMT:%s", body != NULL ? body + 1u : "");
    if (n < 0 || (size_t)n >= cap) {
        /* Preserve only framing for an overlong header. The invalid address
         * prevents decoding, and readable remains false regardless of body. */
        const char *length = strrchr(line, ',');
        n = snprintf(delivery_header, cap, "+CMT:?,%s", length != NULL ? length + 1u : "");
        if (n < 0 || (size_t)n >= cap) delivery_header[0] = '\0';
        r->readable = false;
    }
    return true;
}

void modem_sms_recovery_payload(modem_sms_recovery_t *r,
                                modem_sms_direct_step_t result, const char *pdu) {
    if (r->step != MODEM_SMS_RECOVERY_READ && r->step != MODEM_SMS_RECOVERY_VERIFY) return;
    if (r->payload_seen) { r->readable = false; return; }
    r->payload_seen = true;
    if (!r->readable || (result != MODEM_SMS_DIRECT_STEP_READY &&
                        result != MODEM_SMS_DIRECT_STEP_FILTERED)) { r->readable = false; return; }
    if (r->step == MODEM_SMS_RECOVERY_VERIFY) {
        r->verify_match = strcmp(r->pdu, pdu) == 0 &&
            r->filtered == (result == MODEM_SMS_DIRECT_STEP_FILTERED);
    } else {
        size_t n = strlen(pdu);
        if (n >= sizeof(r->pdu)) { r->readable = false; return; }
        memcpy(r->pdu, pdu, n + 1u);
        r->filtered = result == MODEM_SMS_DIRECT_STEP_FILTERED;
    }
}

void modem_sms_recovery_final(modem_sms_recovery_t *r, bool ok,
                              bool empty_slot, uint32_t now) {
    if (!r->issued) return; /* A session change invalidated this command. */
    r->issued = false;
    switch (r->step) {
    case MODEM_SMS_RECOVERY_SELECT:
        if (!ok || !r->selected || (r->remaining != 0u && r->capacity == 0u)) {
            modem_sms_recovery_defer(r, now); return;
        }
        r->index = 1u;
        if (r->remaining == 0u) done(r, now);
        else r->step = MODEM_SMS_RECOVERY_READ;
        break;
    case MODEM_SMS_RECOVERY_READ:
        if (ok && r->readable && r->payload_seen) r->step = MODEM_SMS_RECOVERY_STORE;
        else {
            if (!empty_slot) { r->failures++; r->retry = true; }
            advance(r, now);
        }
        break;
    case MODEM_SMS_RECOVERY_VERIFY:
        if (ok && r->readable && r->payload_seen && r->verify_match)
            r->step = MODEM_SMS_RECOVERY_DELETE;
        else modem_sms_recovery_defer(r, now);
        break;
    case MODEM_SMS_RECOVERY_DELETE:
        if (ok) r->recovered++;
        else { r->failures++; r->retry = true; }
        /* DELETE has no header; this occupied slot was proven by VERIFY. */
        r->header_seen = true;
        advance(r, now);
        break;
    default: break;
    }
}

void modem_sms_recovery_committed(modem_sms_recovery_t *r) {
    if (r->step == MODEM_SMS_RECOVERY_STORE) r->step = MODEM_SMS_RECOVERY_VERIFY;
}
