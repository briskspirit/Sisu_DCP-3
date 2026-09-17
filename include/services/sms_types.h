#ifndef SMS_TYPES_H
#define SMS_TYPES_H

#include <stdbool.h>
#include <stdint.h>

/* Outbound drafts remain bounded to one 160-septet GSM message. A received
 * GSM7 body can expand to two UTF-8 bytes per septet (the extension-table euro
 * consumes two septets for three bytes), so decoded content has its own bound.
 * The binary mode describes only outgoing SUBMIT framing. */
#define MODEM_SMS_TEXT_MAX 160u
#define MODEM_SMS_DECODED_TEXT_MAX (MODEM_SMS_TEXT_MAX * 2u)
#define MODEM_SMS_BINARY_MAX 384u
#define MODEM_SMS_BINARY_CHUNK_MAX 128u
#define MODEM_SMS_SENDER_MAX 32u
#define MODEM_SMS_TIMESTAMP_MAX 24u
#define MODEM_SMS_STATUS_MAX 12u
#define MODEM_SMS_SEGMENT_MAX 8u
/* Telit ME exposes 255 message slots. Counts stay uint8_t by design: valid
 * mailbox positions are 0..254 and a count of 255 represents a full store. */
#define MODEM_SMS_RECORD_MAX 255u

/* Module memory, rather than the typically tiny SIM store, is the shared
 * receive/send policy for the service and concrete modem backend. */
#define MODEM_SMS_STORAGE "ME"

typedef enum {
    MODEM_BINARY_SMS_MODE_DCS04_PORT_FIRST = 0,
    MODEM_BINARY_SMS_MODE_DCS04_CONCAT_FIRST,
    MODEM_BINARY_SMS_MODE_F5_PORT_FIRST,
    MODEM_BINARY_SMS_MODE_F5_CONCAT_FIRST_VP,
    MODEM_BINARY_SMS_MODE_GSM7_TEXT,
} modem_binary_sms_mode_t;

typedef struct {
    uint16_t index;
    bool binary;
    bool has_ports;
    bool has_concat;
    bool concat_ref_16bit;
    uint16_t dest_port;
    uint16_t source_port;
    uint16_t binary_len;
    uint16_t concat_ref;
    uint8_t concat_total;
    uint8_t concat_seq;
    char status[MODEM_SMS_STATUS_MAX + 1u];
    char sender[MODEM_SMS_SENDER_MAX + 1u];
    char timestamp[MODEM_SMS_TIMESTAMP_MAX + 1u];
    /* A decoded SMS is either text or binary. Only transient/selected messages
     * use this full payload; mailbox rows use modem_sms_record_t below. */
    union {
        char text[MODEM_SMS_DECODED_TEXT_MAX + 1u];
        uint8_t binary_data[MODEM_SMS_BINARY_MAX];
    };
} modem_sms_message_t;

typedef struct {
    /* A complete concatenated text or picture SMS has one index per segment so
     * it can be reconstructed lazily and erased as one UI record. Ordinary
     * text/data has one index. Quarantined rows retain every observed index but
     * expose a neutral Data message instead of incomplete reconstruction. */
    uint16_t indices[MODEM_SMS_SEGMENT_MAX];
    uint32_t identity_hash;
    uint8_t index_count;
    bool picture;
    bool quarantined;
    char status[MODEM_SMS_STATUS_MAX + 1u];
    char sender[MODEM_SMS_SENDER_MAX + 1u];
    char timestamp[MODEM_SMS_TIMESTAMP_MAX + 1u];
} modem_sms_record_t;

typedef enum {
    MODEM_SMS_MAILBOX_INBOX = 0,
    MODEM_SMS_MAILBOX_OUTBOX,
} modem_sms_mailbox_t;

/* Public request identity is intentionally independent from the AT protocol
 * phases. Text and binary sends share one result channel, but retain distinct
 * kinds so a token/kind mismatch fails closed at every boundary. */
typedef enum {
    MODEM_SMS_REQUEST_NONE = 0,
    MODEM_SMS_REQUEST_SEND_TEXT,
    MODEM_SMS_REQUEST_SEND_BINARY,
    MODEM_SMS_REQUEST_SAVE,
    MODEM_SMS_REQUEST_MAILBOX,
    MODEM_SMS_REQUEST_READ,
    MODEM_SMS_REQUEST_DELETE,
    /* Internal re-store of a direct-delivered (+CMT) message. No app-facing
     * result channel; the arrival surfaces through the mailbox scan. */
    MODEM_SMS_REQUEST_DELIVERED,
} modem_sms_request_kind_t;

typedef enum {
    MODEM_SMS_OUTCOME_NONE = 0,
    MODEM_SMS_OUTCOME_OK,
    MODEM_SMS_OUTCOME_ERROR,
    MODEM_SMS_OUTCOME_TIMEOUT,
    MODEM_SMS_OUTCOME_CANCELLED,
    MODEM_SMS_OUTCOME_EVICTED,
    /* Payload or a mutating command crossed the transport boundary, but its
     * authoritative final was lost. Retrying may duplicate or partially
     * repeat the operation. */
    MODEM_SMS_OUTCOME_UNCERTAIN,
    /* The module's message store is full (+CMS ERROR: 322 / "memory full").
     * A failure for every consumer; a direct-delivery store keeps its entry
     * and waits for room instead of burning a retry attempt. */
    MODEM_SMS_OUTCOME_STORAGE_FULL,
} modem_sms_outcome_t;

typedef struct {
    uint32_t request_id;
    modem_sms_request_kind_t kind;
    modem_sms_outcome_t outcome;
    bool sim_not_ready;
    bool complete; /* false if any raw row/segment could not be represented */
    modem_sms_mailbox_t mailbox;
} modem_sms_mailbox_result_t;

typedef struct {
    uint32_t request_id;
    modem_sms_request_kind_t kind;
    modem_sms_outcome_t outcome;
    bool sim_not_ready;
    uint32_t request_identity_hash;
    uint32_t identity_hash;
    modem_sms_message_t message;
} modem_sms_read_result_t;

typedef struct {
    uint32_t request_id;
    modem_sms_request_kind_t kind;
    modem_sms_outcome_t outcome;
} modem_sms_send_result_t;

typedef struct {
    uint32_t request_id;
    modem_sms_request_kind_t kind;
    modem_sms_outcome_t outcome;
    bool sim_not_ready;
} modem_sms_save_result_t;

typedef struct {
    uint32_t request_id;
    modem_sms_request_kind_t kind;
    modem_sms_outcome_t outcome;
    bool sim_not_ready;
} modem_sms_delete_result_t;

#endif /* SMS_TYPES_H */
