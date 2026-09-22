#include "services/modem_network_sim.h"

#include <string.h>

#define SIM_FILE_LIMIT 4095u
#define RANK_NONE UINT16_MAX
#define RANK_USER 0x10000u
#define RANK_OPERATOR 0x20000u
#define RANK_OTHER 0x30000u

static modem_network_entry_t *s_entries;
static uint8_t s_count;
static modem_network_sim_query_t s_query;
static modem_network_sim_reply_t s_reply;
static bool s_seen, s_invalid, s_home_nonempty;
static uint16_t s_size, s_rank[MODEM_NETWORK_MAX], s_home_rank[MODEM_NETWORK_MAX];
static uint32_t s_preferred_rank[MODEM_NETWORK_MAX];
static char s_imsi[7], s_home[7];

void modem_network_sim_begin(modem_network_entry_t *entries, uint8_t count) {
    s_entries = entries;
    s_count = count;
    s_query = (modem_network_sim_query_t){.kind = MODEM_NETWORK_SIM_IMSI};
    s_imsi[0] = s_home[0] = '\0';
    s_home_nonempty = false;
    for (uint8_t i = 0u; i < s_count; i++) {
        s_entries[i].home = false;
        s_home_rank[i] = RANK_NONE;
        s_preferred_rank[i] = RANK_OTHER;
    }
}

bool modem_network_sim_prepare(const modem_network_backend_t *backend,
                               char *command, size_t cap) {
    s_seen = s_invalid = false;
    memset(&s_reply, 0, sizeof(s_reply));
    return backend->sim_build(&s_query, command, cap);
}

bool modem_network_sim_line(const modem_network_backend_t *backend, const char *line) {
    modem_network_line_t result = backend->sim_read(&s_query, line, &s_reply);
    if (result == MODEM_NETWORK_LINE_IGNORE) return false;
    if (s_seen || result == MODEM_NETWORK_LINE_INVALID) s_invalid = true;
    s_seen = true;
    return true;
}

static void file_info(modem_network_sim_file_t file) {
    s_query = (modem_network_sim_query_t){.kind = MODEM_NETWORK_SIM_INFO, .file = file};
    s_size = 0u;
    for (uint8_t i = 0u; i < s_count; i++) s_rank[i] = RANK_NONE;
}

static void imsi_home(void) {
    if (s_home[0] == '\0') return;
    for (uint8_t i = 0u; i < s_count; i++) {
        if (strcmp(s_entries[i].plmn, s_home) == 0) s_home_rank[i] = 0u;
    }
}

static uint32_t rank(uint8_t index) {
    if (s_home_rank[index] != RANK_NONE) return s_home_rank[index];
    if (s_preferred_rank[index] != RANK_OTHER) return s_preferred_rank[index];
    /* Preserve the useful current-first fallback when no SIM priority applies. */
    return RANK_OTHER + (s_entries[index].availability == 2u ? 0u : 1u);
}

static void sort_entries(void) {
    uint32_t ranks[MODEM_NETWORK_MAX];
    for (uint8_t i = 0u; i < s_count; i++) {
        ranks[i] = rank(i);
        s_entries[i].home = s_home_rank[i] != RANK_NONE;
    }
    /* Stable order for unlisted networks and duplicate SIM preferences. */
    for (uint8_t i = 1u; i < s_count; i++) {
        modem_network_entry_t entry = s_entries[i];
        uint32_t priority = ranks[i];
        uint8_t j = i;
        while (j > 0u && ranks[j - 1u] > priority) {
            s_entries[j] = s_entries[j - 1u];
            ranks[j] = ranks[j - 1u];
            j--;
        }
        s_entries[j] = entry;
        ranks[j] = priority;
    }
}

static bool next_file(bool valid) {
    switch (s_query.file) {
    case MODEM_NETWORK_SIM_HOME:
        file_info(MODEM_NETWORK_SIM_USER); return true;
    case MODEM_NETWORK_SIM_USER:
        file_info(valid ? MODEM_NETWORK_SIM_OPERATOR : MODEM_NETWORK_SIM_LEGACY); return true;
    case MODEM_NETWORK_SIM_LEGACY:
        file_info(MODEM_NETWORK_SIM_OPERATOR); return true;
    default:
        sort_entries(); return false;
    }
}

static uint8_t stride(void) {
    return s_query.file == MODEM_NETWORK_SIM_USER || s_query.file == MODEM_NETWORK_SIM_OPERATOR
        ? 5u : 3u;
}

static bool read_plmns(void) {
    uint8_t width = stride();
    for (uint8_t pos = 0u; pos < s_reply.length; pos += width) {
        const uint8_t *p = s_reply.data + pos;
        if (p[0] == 0xffu && p[1] == 0xffu && p[2] == 0xffu) continue;
        uint8_t digits[] = {p[0] & 15u, p[0] >> 4u, p[1] & 15u,
                            p[2] & 15u, p[2] >> 4u, p[1] >> 4u};
        char plmn[7] = {0};
        for (uint8_t d = 0u; d < 6u; d++) {
            if (d == 5u && digits[d] == 15u) break;
            if (digits[d] > 9u) return false;
            plmn[d] = (char)('0' + digits[d]);
        }
        if (s_query.file == MODEM_NETWORK_SIM_HOME) s_home_nonempty = true;
        for (uint8_t i = 0u; i < s_count; i++) {
            if (s_rank[i] == RANK_NONE && strcmp(plmn, s_entries[i].plmn) == 0)
                s_rank[i] = (uint16_t)((s_query.offset + pos) / width);
        }
    }
    return true;
}

bool modem_network_sim_final(bool ok, bool transport_clean) {
    bool valid = ok && transport_clean && s_seen && !s_invalid;
    if (s_query.kind == MODEM_NETWORK_SIM_IMSI) {
        if (valid) { memcpy(s_imsi, s_reply.imsi, 6u); s_imsi[6] = '\0'; }
        s_query = (modem_network_sim_query_t){.kind = MODEM_NETWORK_SIM_DATA,
            .file = MODEM_NETWORK_SIM_AD, .length = 4u};
        return true;
    }
    if (s_query.file == MODEM_NETWORK_SIM_AD) {
        uint8_t mnc = s_reply.data[3] & 15u;
        if (valid && !s_reply.missing && s_reply.length == 4u &&
            (mnc == 2u || mnc == 3u) && strlen(s_imsi) >= 3u + mnc) {
            memcpy(s_home, s_imsi, 3u + mnc); s_home[3u + mnc] = '\0';
        }
        file_info(MODEM_NETWORK_SIM_HOME);
        return true;
    }
    if (!valid || s_reply.missing) {
        /* Only a verified absent/empty EHPLMN file permits IMSI fallback.
         * A damaged or unreadable list must not label a roaming PLMN home. */
        if (valid && s_reply.missing && s_query.file == MODEM_NETWORK_SIM_HOME &&
            s_query.kind == MODEM_NETWORK_SIM_INFO) imsi_home();
        return next_file(false);
    }
    if (s_query.kind == MODEM_NETWORK_SIM_INFO) {
        s_size = s_reply.file_size;
        if (s_size > SIM_FILE_LIMIT || s_size % stride() != 0u) return next_file(false);
        s_query.kind = MODEM_NETWORK_SIM_DATA;
        s_query.offset = 0u;
    } else {
        if (s_reply.length != s_query.length || !read_plmns()) return next_file(false);
        s_query.offset = (uint16_t)(s_query.offset + s_reply.length);
    }
    if (s_query.offset == s_size) {
        if (s_query.file == MODEM_NETWORK_SIM_HOME) {
            if (!s_home_nonempty) imsi_home();
            else memcpy(s_home_rank, s_rank, sizeof(s_home_rank));
        } else {
            uint32_t base = s_query.file == MODEM_NETWORK_SIM_OPERATOR ? RANK_OPERATOR : RANK_USER;
            for (uint8_t i = 0u; i < s_count; i++) {
                if (s_rank[i] != RANK_NONE && base + s_rank[i] < s_preferred_rank[i])
                    s_preferred_rank[i] = base + s_rank[i];
            }
        }
        return next_file(true);
    }
    uint16_t remaining = (uint16_t)(s_size - s_query.offset);
    s_query.length = (uint8_t)(remaining < MODEM_NETWORK_SIM_CHUNK ? remaining : MODEM_NETWORK_SIM_CHUNK);
    return true;
}
