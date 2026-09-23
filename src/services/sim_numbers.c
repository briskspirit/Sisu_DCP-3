#include "services/sim_numbers.h"

#include <string.h>

enum { EF_SDN = 0x6f49, EF_CPHS = 0x6f16, EF_CSP = 0x6f15,
       EF_INFO = 0x6f19, EF_INFO_OLD = 0xea01, EF_EXT1 = 0x6f4a, EF_EXT3 = 0x6f4c };
static const sim_numbers_backend_t *backend;
static sim_numbers_snapshot_t state;
static sim_number_query_t query;
static sim_number_reply_t reply;
static sim_number_t entries[SIM_NUMBERS_MAX], pending_entry;
static sim_numbers_kind_t kind;
static uint32_t serial;
static uint16_t records_file;
static uint8_t record, record_length, record_count, count, level;
static uint8_t ancestors[15], extension, extension_seen[32];
static bool in_flight, seen, invalid, cancelled, probing;
static uint8_t checked, available, probe_failures[2];
static sim_numbers_result_t abort_result;

static void finish(sim_numbers_result_t result) {
    uint8_t bit = (uint8_t)(1u << kind);
    if (probing && result != SIM_NUMBERS_CANCELLED) {
        if (result != SIM_NUMBERS_ERROR || ++probe_failures[kind] >= 3u) checked |= bit;
        if (result == SIM_NUMBERS_DONE) available |= bit;
        else available &= (uint8_t)~bit;
    } else if (!probing && (result == SIM_NUMBERS_UNAVAILABLE ||
               (result == SIM_NUMBERS_DONE && count == 0u))) available &= (uint8_t)~bit;
    state.result = result;
    state.busy = false;
    state.count = result == SIM_NUMBERS_DONE && !probing ? count : 0u;
    in_flight = false;
}

void sim_numbers_init(const sim_numbers_backend_t *value) {
    backend = value;
    memset(&state, 0, sizeof(state));
    state.result = SIM_NUMBERS_UNAVAILABLE;
    in_flight = cancelled = false;
    checked = available = 0u;
    memset(probe_failures, 0, sizeof(probe_failures));
}

void sim_numbers_invalidate(void) {
    probing = false;
    checked = available = 0u;
    memset(probe_failures, 0, sizeof(probe_failures));
    state.count = 0u;
    if (state.busy) {
        cancelled = true;
        if (!in_flight) finish(abort_result);
    } else state.result = SIM_NUMBERS_UNAVAILABLE;
}

uint32_t sim_numbers_request(sim_numbers_kind_t requested) {
    if (state.busy || backend == NULL || backend->build == NULL ||
        backend->read == NULL || backend->alpha == NULL ||
        (requested != SIM_NUMBERS_SERVICE && requested != SIM_NUMBERS_INFO)) return 0u;
    if (++serial == 0u) ++serial;
    state = (sim_numbers_snapshot_t){.request_id = serial, .busy = true};
    kind = requested;
    count = level = 0u;
    cancelled = in_flight = probing = false;
    abort_result = SIM_NUMBERS_CANCELLED;
    memset(ancestors, SIM_NUMBERS_ROOT, sizeof(ancestors));
    records_file = kind == SIM_NUMBERS_SERVICE ? EF_SDN : EF_INFO;
    query = (sim_number_query_t){.kind = SIM_NUMBER_FILE_INFO,
        .file = kind == SIM_NUMBERS_SERVICE ? EF_SDN : EF_CPHS};
    return serial;
}

bool sim_numbers_probe_needed(sim_numbers_kind_t value) {
    return (value == SIM_NUMBERS_SERVICE || value == SIM_NUMBERS_INFO) && backend != NULL &&
        (checked & (1u << value)) == 0u;
}
uint32_t sim_numbers_probe(sim_numbers_kind_t value) {
    if (!sim_numbers_probe_needed(value)) return 0u;
    uint32_t id = sim_numbers_request(value);
    if (id != 0u) probing = true;
    return id;
}
bool sim_numbers_available(sim_numbers_kind_t value) {
    /* Publish both gates together; opening a row must not race the other probe. */
    return (value == SIM_NUMBERS_SERVICE || value == SIM_NUMBERS_INFO) && checked == 3u &&
        (available & (1u << value)) != 0u;
}

void sim_numbers_cancel(uint32_t id) {
    if (id != 0u && id == state.request_id && state.busy) {
        cancelled = true;
        if (!in_flight) finish(abort_result);
    }
}
void sim_numbers_timeout(void) {
    if (state.busy) {
        abort_result = SIM_NUMBERS_ERROR;
        sim_numbers_cancel(state.request_id);
    }
}
void sim_numbers_snapshot(sim_numbers_snapshot_t *out) { *out = state; }
bool sim_numbers_pending(void) { return state.busy && !in_flight && !cancelled; }
bool sim_numbers_cancelled(void) { return cancelled; }
bool sim_numbers_entry(uint32_t id, uint8_t index, sim_number_t *out) {
    if (id != state.request_id || state.busy || state.result != SIM_NUMBERS_DONE ||
        index >= state.count || out == NULL) return false;
    *out = entries[index];
    return true;
}
bool sim_numbers_prepare(char *out, size_t cap) {
    if (!sim_numbers_pending()) return false;
    if (!backend->build(&query, out, cap)) { finish(SIM_NUMBERS_ERROR); return false; }
    seen = invalid = false;
    in_flight = true;
    return true;
}
bool sim_numbers_line(const char *line) {
    if (!in_flight) return false;
    sim_number_line_t result = backend->read(&query, line, &reply);
    if (result == SIM_NUMBER_IGNORE) return false;
    invalid |= seen || result == SIM_NUMBER_INVALID;
    seen = true;
    return true;
}

static bool padding(const uint8_t *data, size_t length) {
    for (size_t i = 0u; i < length; i++) if (data[i] != 0xffu) return false;
    return true;
}

static bool digits(const uint8_t *data, size_t length, char *out) {
    size_t used = strlen(out);
    for (size_t i = 0u; i < length * 2u; i++) {
        uint8_t digit = (uint8_t)((data[i / 2u] >> (4u * (i % 2u))) & 15u);
        if (digit == 15u) return i == length * 2u - 1u && (i & 1u) != 0u;
        /* Pause/wildcard/subaddress control digits need a separate dial policy.
         * Never silently drop them and call a different number. */
        if (digit > 11u || used + 1u >= SIM_NUMBER_CAP) return false;
        out[used++] = "0123456789*#"[digit];
        out[used] = '\0';
    }
    return true;
}

static bool decode_record(void) {
    const uint8_t *d = reply.data;
    size_t n = reply.length, alpha, number_bytes, offset;
    memset(&pending_entry, 0, sizeof(pending_entry));
    if (kind == SIM_NUMBERS_SERVICE) {
        if (n < 14u) return false;
        alpha = n - 14u; offset = alpha; number_bytes = 10u;
        level = 1u;
        /* This route is voice-only. Don't reinterpret bearer-specific SDNs. */
        if (d[n - 2u] != 0xffu) return false;
    } else {
        if (n < 5u || d[0] > n - 5u || (d[1] & 0xc0u) != 0xc0u) return false;
        alpha = d[0]; offset = alpha + 2u; number_bytes = n - alpha - 5u;
        level = d[1] & 15u;
        if (level == 0u) return false;
        pending_entry.premium = (d[1] & 0x10u) != 0u;
        pending_entry.home_only = (d[1] & 0x20u) != 0u;
        pending_entry.folder = d[offset] == 0xffu && d[offset + 1u] == 0xffu;
    }
    if (!backend->alpha(d + (kind == SIM_NUMBERS_INFO ? 2u : 0u), alpha,
                         pending_entry.name, sizeof(pending_entry.name))) return false;
    extension = d[n - 1u];
    if (pending_entry.folder) {
        if (extension != 0xffu || pending_entry.name[0] == '\0') return false;
    } else if (d[offset] == 0xffu || d[offset] == 0u) {
        if (kind == SIM_NUMBERS_INFO || extension != 0xffu) return false;
    } else {
        size_t length = d[offset];
        uint8_t ton = d[offset + 1u];
        if (length < 2u || length > number_bytes + 1u ||
            (ton != 0x81u && ton != 0x91u && ton != 0xa1u)) return false;
        if (ton == 0x91u) strcpy(pending_entry.number, "+");
        if (!digits(d + offset + 2u, length - 1u, pending_entry.number)) return false;
    }
    if (level > 1u && ancestors[level - 2u] == SIM_NUMBERS_ROOT) return false;
    pending_entry.parent = level == 1u ? SIM_NUMBERS_ROOT : ancestors[level - 2u];
    if (pending_entry.parent != SIM_NUMBERS_ROOT) {
        pending_entry.home_only |= entries[pending_entry.parent].home_only;
        pending_entry.premium |= entries[pending_entry.parent].premium;
    }
    memset(extension_seen, 0, sizeof(extension_seen));
    return true;
}

static void next_record(void) {
    if (record == record_count) { finish(SIM_NUMBERS_DONE); return; }
    query = (sim_number_query_t){.kind = SIM_NUMBER_RECORD, .file = records_file,
        .record = ++record, .length = record_length};
}

static void save_or_extend(void) {
    if (extension != 0xffu) {
        if (extension == 0u || (extension_seen[extension / 8u] & (1u << (extension % 8u))) != 0u) {
            finish(SIM_NUMBERS_ERROR); return;
        }
        extension_seen[extension / 8u] |= (uint8_t)(1u << (extension % 8u));
        query = (sim_number_query_t){.kind = SIM_NUMBER_RECORD,
            .file = kind == SIM_NUMBERS_SERVICE ? EF_EXT3 : EF_EXT1,
            .record = extension, .length = 13u};
        return;
    }
    if (count == SIM_NUMBERS_MAX) { finish(SIM_NUMBERS_ERROR); return; }
    for (uint8_t i = level - 1u; i < 15u; i++) ancestors[i] = SIM_NUMBERS_ROOT;
    if (pending_entry.folder) ancestors[level - 1u] = count;
    entries[count++] = pending_entry;
    next_record();
}

void sim_numbers_final(bool ok, bool clean) {
    if (!state.busy || !in_flight) return;
    in_flight = false;
    if (cancelled) { finish(abort_result); return; }
    if (!ok || !clean || !seen || invalid) { finish(SIM_NUMBERS_ERROR); return; }
    if (reply.missing) {
        if (query.file == EF_INFO && query.kind == SIM_NUMBER_FILE_INFO) {
            records_file = EF_INFO_OLD;
            query.file = EF_INFO_OLD;
        } else finish(query.kind == SIM_NUMBER_FILE_INFO ? SIM_NUMBERS_UNAVAILABLE : SIM_NUMBERS_ERROR);
        return;
    }
    if (query.kind == SIM_NUMBER_FILE_INFO) {
        if (query.file == EF_CPHS || query.file == EF_CSP) {
            if (reply.record_length != 0u || reply.size < (query.file == EF_CPHS ? 3u : 2u) ||
                reply.size > SIM_NUMBER_RECORD_MAX) { finish(SIM_NUMBERS_ERROR); return; }
            query.kind = SIM_NUMBER_BINARY;
            query.length = (uint8_t)reply.size;
        } else {
            if (reply.record_length < (kind == SIM_NUMBERS_SERVICE ? 14u : 5u)) {
                finish(SIM_NUMBERS_ERROR); return;
            }
            if (probing) {
                finish(reply.records != 0u ? SIM_NUMBERS_DONE : SIM_NUMBERS_UNAVAILABLE);
                return;
            }
            record_length = reply.record_length; record_count = reply.records; record = 0u;
            next_record();
        }
    } else if (query.kind == SIM_NUMBER_BINARY) {
        bool allowed = false;
        if (query.file == EF_CPHS) allowed = reply.data[0] >= 2u && reply.data[0] != 0xffu && (reply.data[2] & 3u) == 3u &&
            (reply.data[1] & 3u) == 3u;
        else {
            if ((reply.length & 1u) != 0u) { finish(SIM_NUMBERS_ERROR); return; }
            bool found = false;
            for (uint8_t i = 0u; i < reply.length; i += 2u) {
                if (reply.data[i] != 0xd5u) continue;
                if (found) { finish(SIM_NUMBERS_ERROR); return; }
                found = true;
                allowed = reply.data[i + 1u] == 0xffu;
            }
        }
        if (!allowed) { finish(SIM_NUMBERS_UNAVAILABLE); return; }
        query = (sim_number_query_t){.kind = SIM_NUMBER_FILE_INFO,
            .file = query.file == EF_CPHS ? EF_CSP : records_file};
    } else if (query.file == records_file) {
        if (padding(reply.data, reply.length)) { next_record(); return; }
        if (!decode_record()) { finish(SIM_NUMBERS_ERROR); return; }
        save_or_extend();
    } else {
        if (reply.length != 13u || reply.data[0] != 2u || reply.data[1] > 10u ||
            !digits(reply.data + 2u, reply.data[1], pending_entry.number)) {
            finish(SIM_NUMBERS_ERROR); return;
        }
        extension = reply.data[12];
        save_or_extend();
    }
}
