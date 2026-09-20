#ifndef BUSINESS_CARD_SERVICE_H
#define BUSINESS_CARD_SERVICE_H

#include "services/business_card_codec.h"
#include "storage/storage_objects.h"

#define BUSINESS_CARD_LIMIT 16u

typedef struct {
    uint32_t id;
    bool save;
    phonebook_outcome_t outcome;
} business_card_action_result_t;

/* Sole core0 storage owner. Files retain original segments for recovery and
 * exact retransmission detection, including after Save/Discard. */
void business_card_service_init(void);
void business_card_service_tick(void);
bool business_card_service_idle(void);
bool business_card_service_ready(void);
uint32_t business_card_service_pending_first(void);
bool business_card_service_get(uint32_t id, phonebook_entry_t *out);
bool business_card_service_request(uint32_t id, bool save);
bool business_card_service_pop_result(business_card_action_result_t *out);
/* Called by the message owner's publication pass, in its safe write window. */
storage_record_result_t business_card_service_publish(uint32_t id, const message_file_t *file);
storage_record_result_t business_card_service_duplicate(const char *pdu, bool *duplicate);

#endif
