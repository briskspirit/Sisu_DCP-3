#ifndef SMS_TYPES_H
#define SMS_TYPES_H

#include <stdbool.h>
#include <stddef.h>
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

/* Values match the persistent, original-style Set 1 pickers. These express
 * user intent; AT parameters and network encodings belong to the adapter. */
typedef enum {
    SMS_SEND_AS_TEXT, SMS_SEND_AS_FAX, SMS_SEND_AS_PAGING, SMS_SEND_AS_EMAIL,
} sms_send_format_t;
typedef enum {
    SMS_VALIDITY_1_HOUR, SMS_VALIDITY_6_HOURS, SMS_VALIDITY_24_HOURS,
    SMS_VALIDITY_72_HOURS, SMS_VALIDITY_1_WEEK, SMS_VALIDITY_MAXIMUM,
} sms_validity_t;
typedef struct {
    uint8_t format;
    uint8_t validity;
    bool reply_same_centre;
    bool delivery_reports;
    /* Empty means use the current SIM/modem service centre without changing it. */
    char service_centre[MODEM_SMS_SENDER_MAX + 1u];
} sms_send_options_t;

static inline bool sms_service_centre_valid(const char *number, size_t capacity) {
    if (number == NULL || capacity == 0u) return false;
    size_t i = number[0] == '+' ? 1u : 0u;
    if (i != 0u && (capacity == 1u || number[i] == '\0')) return false;
    for (; i < capacity; i++) {
        if (number[i] == '\0') return true;
        if (number[i] < '0' || number[i] > '9') return false;
    }
    return false;
}

static inline bool sms_send_options_valid(const sms_send_options_t *options) {
    return options != NULL && options->format <= SMS_SEND_AS_EMAIL &&
        options->validity <= SMS_VALIDITY_MAXIMUM &&
        sms_service_centre_valid(options->service_centre, sizeof(options->service_centre));
}
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
    /* One normalized transport segment, decoded transiently. */
    union {
        char text[MODEM_SMS_DECODED_TEXT_MAX + 1u];
        uint8_t binary_data[MODEM_SMS_BINARY_MAX];
    };
} modem_sms_message_t;

/* Public request identity is intentionally independent from the AT protocol
 * phases. Text and binary sends share one result channel, but retain distinct
 * kinds so a token/kind mismatch fails closed at every boundary. */
typedef enum {
    MODEM_SMS_REQUEST_NONE = 0,
    MODEM_SMS_REQUEST_SEND_TEXT,
    MODEM_SMS_REQUEST_SEND_BINARY,
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
} modem_sms_outcome_t;

typedef struct {
    uint32_t request_id;
    modem_sms_request_kind_t kind;
    modem_sms_outcome_t outcome;
} modem_sms_send_result_t;

#endif /* SMS_TYPES_H */
