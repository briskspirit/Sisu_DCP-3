#ifndef MODEM_PHONEBOOK_STATE_H
#define MODEM_PHONEBOOK_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "services/phonebook_types.h"

/* Single-modem cache/result owner. The outer service retains its existing
 * critical section around every cross-core call into this pure state module. */
void modem_phonebook_state_init(void);
void modem_phonebook_state_clear(void);
/* Refresh rows overwrite the cache storage but remain invisible until the
 * owner publishes their staged count after a clean CPBR final. */
void modem_phonebook_state_refresh_begin(void);
bool modem_phonebook_state_refresh_finish(bool publish);
bool modem_phonebook_state_append(const modem_phonebook_entry_t *entry);
uint16_t modem_phonebook_state_count(void);
bool modem_phonebook_state_entry(uint16_t position,
                                 modem_phonebook_entry_t *out);

/* Reservation makes terminal delivery total: an admitted request owns one
 * journal slot until it is either released before admission or completed.
 * Publishing an unknown/already-completed id fails instead of duplicating a
 * terminal result. */
bool modem_phonebook_state_reserve_request(uint32_t *request_id_out);
bool modem_phonebook_state_release_request(uint32_t request_id);
bool modem_phonebook_state_publish_result(
    uint32_t request_id, modem_phonebook_op_t kind,
    modem_phonebook_outcome_t outcome, bool sim_not_ready);
bool modem_phonebook_state_pop_result(modem_phonebook_result_t *out);

#endif /* MODEM_PHONEBOOK_STATE_H */
