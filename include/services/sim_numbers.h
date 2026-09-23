#ifndef SIM_NUMBERS_H
#define SIM_NUMBERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SIM_NUMBERS_MAX 64u
#define SIM_NUMBER_NAME_CAP 97u
#define SIM_NUMBER_CAP 33u
#define SIM_NUMBER_RECORD_MAX 120u
#define SIM_NUMBERS_ROOT 255u

typedef enum { SIM_NUMBERS_SERVICE, SIM_NUMBERS_INFO } sim_numbers_kind_t;
typedef enum {
    SIM_NUMBERS_PENDING, SIM_NUMBERS_DONE, SIM_NUMBERS_UNAVAILABLE,
    SIM_NUMBERS_ERROR, SIM_NUMBERS_CANCELLED,
} sim_numbers_result_t;
typedef struct {
    char name[SIM_NUMBER_NAME_CAP];
    char number[SIM_NUMBER_CAP];
    uint8_t parent;
    bool folder, premium, home_only;
} sim_number_t;
typedef struct {
    uint32_t request_id;
    sim_numbers_result_t result;
    uint8_t count;
    bool busy;
} sim_numbers_snapshot_t;

/* SIM file identifiers and record formats stay below the app boundary. */
typedef enum { SIM_NUMBER_FILE_INFO, SIM_NUMBER_BINARY, SIM_NUMBER_RECORD } sim_number_read_t;
typedef struct {
    sim_number_read_t kind;
    uint16_t file;
    uint8_t record, length;
} sim_number_query_t;
typedef struct {
    bool missing;
    uint16_t size;
    uint8_t record_length, records, length;
    uint8_t data[SIM_NUMBER_RECORD_MAX];
} sim_number_reply_t;
typedef enum { SIM_NUMBER_IGNORE, SIM_NUMBER_VALID, SIM_NUMBER_INVALID } sim_number_line_t;
typedef struct {
    bool (*build)(const sim_number_query_t *, char *, size_t);
    sim_number_line_t (*read)(const sim_number_query_t *, const char *, sim_number_reply_t *);
    bool (*alpha)(const uint8_t *, size_t, char *, size_t);
} sim_numbers_backend_t;

void sim_numbers_init(const sim_numbers_backend_t *backend);
void sim_numbers_invalidate(void);
uint32_t sim_numbers_request(sim_numbers_kind_t kind);
uint32_t sim_numbers_probe(sim_numbers_kind_t kind);
bool sim_numbers_probe_needed(sim_numbers_kind_t kind);
bool sim_numbers_available(sim_numbers_kind_t kind);
void sim_numbers_cancel(uint32_t id);
void sim_numbers_timeout(void);
void sim_numbers_snapshot(sim_numbers_snapshot_t *out);
bool sim_numbers_entry(uint32_t id, uint8_t index, sim_number_t *out);
bool sim_numbers_pending(void);
bool sim_numbers_cancelled(void);
bool sim_numbers_prepare(char *out, size_t cap);
bool sim_numbers_line(const char *line);
void sim_numbers_final(bool ok, bool clean);

#endif
