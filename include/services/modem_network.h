#ifndef MODEM_NETWORK_H
#define MODEM_NETWORK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MODEM_NETWORK_MAX 24u
#define MODEM_NETWORK_NAME_CAP 49u
#define MODEM_NETWORK_ID_CAP 21u

typedef enum {
    MODEM_NETWORK_AUTOMATIC = 0,
    MODEM_NETWORK_MANUAL,
    MODEM_NETWORK_OTHER,
    MODEM_NETWORK_UNKNOWN,
} modem_network_mode_t;

typedef enum {
    MODEM_NETWORK_REFRESH = 0,
    MODEM_NETWORK_SCAN,
    MODEM_NETWORK_SELECT_AUTO,
    MODEM_NETWORK_SELECT_MANUAL,
} modem_network_action_t;

typedef enum {
    MODEM_NETWORK_PENDING = 0,
    MODEM_NETWORK_DONE,
    MODEM_NETWORK_NOT_DONE,
    MODEM_NETWORK_NO_ACCESS,
    MODEM_NETWORK_CANCELLED,
} modem_network_result_t;

typedef struct {
    char plmn[7];
    char name[MODEM_NETWORK_NAME_CAP];
    uint8_t availability; /* 0 unknown, 1 available, 2 current, 3 forbidden */
} modem_network_entry_t;

typedef struct {
    uint32_t request_id;
    uint32_t list_generation;
    modem_network_action_t action;
    modem_network_result_t result;
    modem_network_mode_t mode;
    uint8_t count;
    bool busy;
    bool ready;
    char plmn[7];
} modem_network_snapshot_t;

typedef enum {
    MODEM_NETWORK_STEP_NONE = 0,
    MODEM_NETWORK_STEP_IDENTITY,
    MODEM_NETWORK_STEP_READ,
    MODEM_NETWORK_STEP_SCAN,
    MODEM_NETWORK_STEP_AUTO,
    MODEM_NETWORK_STEP_MANUAL,
} modem_network_step_t;

typedef enum {
    MODEM_NETWORK_LINE_IGNORE = 0,
    MODEM_NETWORK_LINE_VALID,
    MODEM_NETWORK_LINE_INVALID,
} modem_network_line_t;

typedef bool (*modem_network_collect_fn)(const modem_network_entry_t *entry);

typedef struct {
    bool (*build)(modem_network_step_t step, const char *plmn,
                  char *out, size_t cap, uint32_t *timeout_ms);
    modem_network_line_t (*identity)(const char *line, char *out, size_t cap);
    modem_network_line_t (*read)(const char *line, modem_network_mode_t *mode,
                                char plmn[7]);
    modem_network_line_t (*scan)(const char *line, modem_network_collect_fn collect);
    bool (*denied)(const char *final);
    const char *abort_bytes;
    uint32_t abort_timeout_ms;
} modem_network_backend_t;

typedef struct {
    bool (*read_owner)(char *out, size_t cap);
    /* Must return true only after this identity is durable. */
    bool (*save_owner)(const char *identity);
} modem_network_hooks_t;

/* Core-0 state owner. No HAL, storage, app, or modem-service dependency. The
 * outer service serializes transport and calls final only after draining it. */
void modem_network_init(const modem_network_backend_t *backend,
                        const modem_network_hooks_t *hooks);
void modem_network_reset_session(void);
void modem_network_invalidate_owner(void);
bool modem_network_supported(void);
void modem_network_snapshot(modem_network_snapshot_t *out);
bool modem_network_entry(uint32_t generation, uint8_t index, modem_network_entry_t *out);
uint32_t modem_network_request(modem_network_action_t action, uint32_t generation,
                               uint8_t index);
void modem_network_cancel(void);
void modem_network_timeout(void);
bool modem_network_cancelled(void);
bool modem_network_work_pending(void);
bool modem_network_prepare(char *command, size_t cap, uint32_t *timeout_ms);
bool modem_network_command_abortable(void);
bool modem_network_line(const char *line);
void modem_network_final(bool ok, const char *final, bool transport_clean);
static inline bool modem_network_plmn_valid(const char *plmn) {
    if (plmn == NULL) return false;
    size_t n = 0u;
    while (plmn[n] >= '0' && plmn[n] <= '9' && n < 6u) n++;
    return (n == 5u || n == 6u) && plmn[n] == '\0';
}

#endif
