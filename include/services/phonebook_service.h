#ifndef PHONEBOOK_SERVICE_H
#define PHONEBOOK_SERVICE_H

#include "services/phonebook_types.h"

/* Core0 owns this local store and its RAM cache. Completion is published only
 * after the atomic file operation; no SIM or modem readiness is required. */
void phonebook_service_init(void);
void phonebook_service_tick(void);
bool phonebook_service_idle(void);
bool phonebook_service_space(uint32_t *used_bytes, uint32_t *limit_bytes);
bool phonebook_service_request_list(uint32_t *request_id);
bool phonebook_service_request_add(const char *name, const char *number,
                                   uint32_t *request_id);
bool phonebook_service_request_update(uint32_t id, const char *name,
                                      const char *number, uint32_t *request_id);
bool phonebook_service_request_delete(uint32_t id, uint32_t *request_id);
bool phonebook_service_pop_result(phonebook_result_t *out);
bool phonebook_service_cache_valid(void);
uint16_t phonebook_service_count(void);
bool phonebook_service_entry(uint16_t position, phonebook_entry_t *out);

#endif
