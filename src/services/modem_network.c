#include "services/modem_network.h"
#include "services/modem_network_sim.h"

#include <string.h>

static const modem_network_backend_t *s_backend;
static modem_network_hooks_t s_hooks;
static modem_network_snapshot_t s_view;
static modem_network_entry_t s_entries[MODEM_NETWORK_MAX];
static modem_network_step_t s_step;
static uint32_t s_next_id;
static bool s_reconcile, s_waiting, s_cancel, s_seen, s_invalid;
static bool s_verifying, s_mutation_ok, s_reconciling;
static modem_network_result_t s_mutation_result;
static modem_network_result_t s_cancel_result;
static char s_identity[MODEM_NETWORK_ID_CAP], s_target[7];
static char s_saved_record[MODEM_NETWORK_RECORD_CAP];
static modem_network_mode_t s_saved_mode;
static modem_network_mode_t s_read_mode;
static char s_read_plmn[7];
static bool s_identity_verified;

bool modem_network_supported(void) {
    return s_backend != NULL && s_backend->build != NULL &&
        s_backend->identity != NULL && s_backend->read != NULL &&
        s_backend->scan != NULL && s_backend->drain_timeout_ms != 0u;
}

static void clear_list(void) {
    s_view.count = 0u;
    s_view.home = false;
    if (++s_view.list_generation == 0u) ++s_view.list_generation;
}

static void finish(modem_network_result_t result) {
    s_view.busy = false;
    s_view.result = result;
    s_step = MODEM_NETWORK_STEP_NONE;
    s_waiting = false;
    s_cancel = false;
    s_verifying = false;
    if (result != MODEM_NETWORK_DONE && s_view.action == MODEM_NETWORK_SCAN)
        clear_list();
}

void modem_network_reset_session(void) {
    if (s_view.busy) finish(MODEM_NETWORK_CANCELLED);
    s_view.ready = false;
    s_view.mode = MODEM_NETWORK_UNKNOWN;
    s_view.plmn[0] = '\0';
    s_identity[0] = '\0';
    s_identity_verified = false;
    s_saved_record[0] = '\0';
    s_reconcile = true;
    clear_list();
}

void modem_network_init(const modem_network_backend_t *backend,
                        const modem_network_hooks_t *hooks) {
    s_backend = backend;
    memset(&s_hooks, 0, sizeof(s_hooks));
    if (hooks != NULL) s_hooks = *hooks;
    memset(&s_view, 0, sizeof(s_view));
    s_next_id = 0u;
    s_step = MODEM_NETWORK_STEP_NONE;
    s_waiting = s_cancel = s_verifying = false;
    modem_network_reset_session();
}

void modem_network_invalidate_owner(void) {
    modem_network_cancel();
    s_view.ready = false;
    s_view.mode = MODEM_NETWORK_UNKNOWN;
    s_view.plmn[0] = '\0';
    s_identity[0] = '\0';
    s_identity_verified = false;
    clear_list();
    s_reconcile = true;
}

void modem_network_snapshot(modem_network_snapshot_t *out) {
    if (out != NULL) *out = s_view;
}

bool modem_network_identity(char *out, size_t cap) {
    if (out == NULL || cap == 0u) return false;
    out[0] = '\0';
    if (!s_identity_verified || strlen(s_identity) >= cap) return false;
    memcpy(out, s_identity, strlen(s_identity) + 1u);
    return true;
}

bool modem_network_entry(uint32_t generation, uint8_t index, modem_network_entry_t *out) {
    if (out == NULL || generation != s_view.list_generation ||
        s_view.busy || index >= s_view.count) return false;
    *out = s_entries[index];
    return true;
}

uint32_t modem_network_request(modem_network_action_t action, uint32_t generation,
                               uint8_t index) {
    if (!modem_network_supported() || s_view.busy || action > MODEM_NETWORK_SELECT_MANUAL)
        return 0u;
    if (action != MODEM_NETWORK_REFRESH && !s_view.ready) return 0u;
    if (action == MODEM_NETWORK_SELECT_MANUAL) {
        if (generation != s_view.list_generation || index >= s_view.count) return 0u;
        memcpy(s_target, s_entries[index].plmn, sizeof(s_target));
    }
    if (++s_next_id == 0u) ++s_next_id;
    s_view.request_id = s_next_id;
    s_view.action = action;
    s_view.result = MODEM_NETWORK_PENDING;
    s_view.busy = true;
    s_view.home = false;
    s_cancel = s_verifying = s_waiting = false;
    s_reconciling = action == MODEM_NETWORK_REFRESH;
    s_reconcile = false;
    if (s_reconciling) {
        /* A fresh identity read cannot retain another card's scan metadata. */
        clear_list();
        s_identity_verified = false;
        s_view.ready = false;
        s_step = MODEM_NETWORK_STEP_IDENTITY;
    }
    else if (action == MODEM_NETWORK_SCAN) {
        clear_list();
        s_step = MODEM_NETWORK_STEP_SCAN;
    } else {
        /* Prove the previous confirmed choice is durable before changing it.
         * The new choice is committed only after matching modem readback. */
        if (s_identity[0] == '\0' || s_saved_record[0] == '\0' ||
            s_hooks.save_owner == NULL || !s_hooks.save_owner(s_saved_record)) {
            finish(MODEM_NETWORK_NOT_DONE);
            return s_view.request_id;
        }
        s_step = action == MODEM_NETWORK_SELECT_AUTO
            ? MODEM_NETWORK_STEP_AUTO : MODEM_NETWORK_STEP_MANUAL;
    }
    return s_view.request_id;
}

static void stop(modem_network_result_t result) {
    if (!s_view.busy) return;
    s_cancel = true;
    s_cancel_result = result;
    if (s_verifying || s_reconciling ||
        (s_waiting && (s_step == MODEM_NETWORK_STEP_AUTO ||
                       s_step == MODEM_NETWORK_STEP_MANUAL))) {
        s_view.ready = false;
        s_view.mode = MODEM_NETWORK_UNKNOWN;
        s_reconcile = true;
    }
    if (!s_waiting) finish(result);
}

void modem_network_cancel(void) { stop(MODEM_NETWORK_CANCELLED); }
void modem_network_timeout(void) { stop(MODEM_NETWORK_NOT_DONE); }

bool modem_network_cancelled(void) { return s_cancel; }
bool modem_network_work_pending(void) {
    return modem_network_supported() && (s_view.busy || s_reconcile);
}

bool modem_network_prepare(char *command, size_t cap, uint32_t *timeout_ms) {
    if (!modem_network_supported() || s_waiting || command == NULL || cap == 0u ||
        timeout_ms == NULL) return false;
    if (!s_view.busy && s_reconcile) {
        if (modem_network_request(MODEM_NETWORK_REFRESH, 0u, 0u) == 0u) return false;
        /* Background reconciliation must not impersonate a UI request. */
        s_view.request_id = 0u;
    }
    if (!s_view.busy || s_step == MODEM_NETWORK_STEP_NONE) return false;
    bool built;
    if (s_step == MODEM_NETWORK_STEP_SIM) {
        *timeout_ms = 5000u;
        built = modem_network_sim_prepare(s_backend, command, cap);
    } else built = s_backend->build(s_step, s_target, command, cap, timeout_ms);
    if (!built) {
        finish(MODEM_NETWORK_NOT_DONE);
        return false;
    }
    s_seen = s_invalid = false;
    s_read_mode = MODEM_NETWORK_UNKNOWN;
    s_read_plmn[0] = '\0';
    s_waiting = true;
    return true;
}

bool modem_network_command_changes_registration(void) {
    return s_step == MODEM_NETWORK_STEP_AUTO || s_step == MODEM_NETWORK_STEP_MANUAL;
}

static bool collect(const modem_network_entry_t *entry) {
    if (!modem_network_plmn_valid(entry->plmn) || entry->availability > 3u) return false;
    for (uint8_t i = 0u; i < s_view.count; i++) {
        if (strcmp(s_entries[i].plmn, entry->plmn) == 0) {
            if (entry->availability == 2u ||
                (s_entries[i].availability != 2u && entry->availability == 1u))
                s_entries[i] = *entry;
            return true;
        }
    }
    if (s_view.count == MODEM_NETWORK_MAX) return false;
    s_entries[s_view.count++] = *entry;
    return true;
}

bool modem_network_line(const char *line) {
    if (!s_waiting || s_cancel) return false;
    if (s_step == MODEM_NETWORK_STEP_SIM) return modem_network_sim_line(s_backend, line);
    modem_network_line_t result = MODEM_NETWORK_LINE_IGNORE;
    switch (s_step) {
    case MODEM_NETWORK_STEP_IDENTITY:
        result = s_backend->identity(line, s_identity, sizeof(s_identity)); break;
    case MODEM_NETWORK_STEP_READ:
        result = s_backend->read(line, &s_read_mode, s_read_plmn); break;
    case MODEM_NETWORK_STEP_SCAN:
        result = s_backend->scan(line, collect); break;
    default: break;
    }
    if (result == MODEM_NETWORK_LINE_IGNORE) return false;
    if (result == MODEM_NETWORK_LINE_INVALID ||
        (s_seen && s_step != MODEM_NETWORK_STEP_SCAN)) s_invalid = true;
    s_seen = true;
    return true;
}

static bool read_saved_selection(void) {
    char record[MODEM_NETWORK_RECORD_CAP] = {0};
    if (s_hooks.read_owner == NULL || !s_hooks.read_owner(record, sizeof(record))) return false;
    size_t n = 0u;
    while (n < MODEM_NETWORK_ID_CAP - 1u && record[n] >= '0' && record[n] <= '9') n++;
    if (record[0] != '\0' && ((n != 19u && n != 20u) ||
        (record[n] != '\0' && (record[n] != ':' || !modem_network_plmn_valid(record + n + 1u)))))
        return false;
    s_saved_mode = MODEM_NETWORK_AUTOMATIC;
    s_target[0] = '\0';
    if (n == strlen(s_identity) && memcmp(record, s_identity, n) == 0 && record[n] == ':') {
        s_saved_mode = MODEM_NETWORK_MANUAL;
        memcpy(s_target, record + n + 1u, strlen(record + n + 1u) + 1u);
    }
    return true;
}

static bool remember_selection(modem_network_mode_t mode, const char *plmn) {
    char record[MODEM_NETWORK_RECORD_CAP];
    size_t n = strlen(s_identity);
    if (n == 0u || n >= MODEM_NETWORK_ID_CAP) return false;
    memcpy(record, s_identity, n + 1u);
    if (mode == MODEM_NETWORK_MANUAL) {
        if (!modem_network_plmn_valid(plmn)) return false;
        record[n++] = ':';
        memcpy(record + n, plmn, strlen(plmn) + 1u);
    } else if (mode != MODEM_NETWORK_AUTOMATIC) return false;
    if (s_hooks.save_owner == NULL || !s_hooks.save_owner(record)) return false;
    memcpy(s_saved_record, record, strlen(record) + 1u);
    return true;
}

void modem_network_final(bool ok, const char *final, bool transport_clean) {
    if (!s_waiting) return;
    s_waiting = false;
    if (s_cancel) {
        if (s_step == MODEM_NETWORK_STEP_AUTO || s_step == MODEM_NETWORK_STEP_MANUAL) {
            s_view.mode = MODEM_NETWORK_UNKNOWN;
            s_view.ready = false;
            s_reconcile = true;
        }
        finish(s_cancel_result);
        return;
    }
    if (s_step == MODEM_NETWORK_STEP_SIM) {
        if (!modem_network_sim_final(ok, transport_clean)) finish(MODEM_NETWORK_DONE);
        return;
    }
    bool valid = ok && transport_clean && s_seen && !s_invalid;
    if (s_step == MODEM_NETWORK_STEP_AUTO || s_step == MODEM_NETWORK_STEP_MANUAL) {
        s_mutation_ok = ok && transport_clean;
        s_mutation_result = s_backend->denied != NULL && s_backend->denied(final)
            ? MODEM_NETWORK_NO_ACCESS : MODEM_NETWORK_NOT_DONE;
        s_verifying = true;
        s_step = MODEM_NETWORK_STEP_READ;
        s_view.ready = false;
        s_view.mode = MODEM_NETWORK_UNKNOWN;
        return;
    }
    if (!valid) {
        if (s_step != MODEM_NETWORK_STEP_SCAN) {
            s_view.ready = false;
            s_view.mode = MODEM_NETWORK_UNKNOWN;
        }
        finish(MODEM_NETWORK_NOT_DONE);
        return;
    }
    if (s_step == MODEM_NETWORK_STEP_IDENTITY) {
        s_identity_verified = true;
        if (!read_saved_selection()) { finish(MODEM_NETWORK_NOT_DONE); return; }
        s_step = MODEM_NETWORK_STEP_READ;
    } else if (s_step == MODEM_NETWORK_STEP_SCAN) {
        if (s_view.count != 0u && s_backend->sim_build != NULL && s_backend->sim_read != NULL) {
            modem_network_sim_begin(s_entries, s_view.count);
            s_step = MODEM_NETWORK_STEP_SIM;
            return;
        }
        /* Backend without SIM metadata: retain the current-first fallback. */
        for (uint8_t i = 1u; i < s_view.count; i++) {
            if (s_entries[i].availability == 2u) {
                modem_network_entry_t current = s_entries[i];
                memmove(&s_entries[1], &s_entries[0], i * sizeof(s_entries[0]));
                s_entries[0] = current;
                break;
            }
        }
        finish(MODEM_NETWORK_DONE);
    } else if (s_step == MODEM_NETWORK_STEP_READ) {
        if (s_read_mode == MODEM_NETWORK_UNKNOWN) {
            s_view.ready = false;
            s_view.mode = MODEM_NETWORK_UNKNOWN;
            finish(MODEM_NETWORK_NOT_DONE);
            return;
        }
        s_view.mode = s_read_mode;
        memcpy(s_view.plmn, s_read_plmn, sizeof(s_view.plmn));
        if (s_reconciling) {
            bool matches = s_read_mode == s_saved_mode &&
                (s_saved_mode != MODEM_NETWORK_MANUAL || strcmp(s_target, s_read_plmn) == 0);
            if (!matches && !s_verifying) {
                s_step = s_saved_mode == MODEM_NETWORK_MANUAL
                    ? MODEM_NETWORK_STEP_MANUAL : MODEM_NETWORK_STEP_AUTO;
                return;
            }
            /* A rejected restore must not trap the user: retain the saved
             * choice and allow Automatic or a new manual selection. No loop. */
            s_view.ready = remember_selection(s_saved_mode, s_target);
            finish(s_view.ready && matches && (!s_verifying || s_mutation_ok)
                ? MODEM_NETWORK_DONE : MODEM_NETWORK_NOT_DONE);
        } else {
            s_view.ready = s_read_mode != MODEM_NETWORK_UNKNOWN;
            bool matches = s_view.action == MODEM_NETWORK_SELECT_AUTO
                ? s_read_mode == MODEM_NETWORK_AUTOMATIC
                : s_read_mode == MODEM_NETWORK_MANUAL && strcmp(s_target, s_read_plmn) == 0;
            if (s_mutation_ok && matches) {
                s_view.ready = remember_selection(s_read_mode, s_read_plmn);
                if (s_view.ready && s_view.action == MODEM_NETWORK_SELECT_MANUAL) {
                    for (uint8_t i = 0u; i < s_view.count; i++) {
                        if (strcmp(s_entries[i].plmn, s_read_plmn) == 0) s_view.home = s_entries[i].home;
                    }
                }
                finish(s_view.ready ? MODEM_NETWORK_DONE : MODEM_NETWORK_NOT_DONE);
            } else finish(s_mutation_result);
        }
    }
}
