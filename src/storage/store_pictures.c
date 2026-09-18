#include "storage/store_service.h"

#include "storage_bytes.h"
#include "store_service_internal.h"
#include "services/timebase.h"

#include <string.h>

#define PICTURE_MAGIC 0x50494331u
#define PICTURE_VERSION 2u
#define PICTURE_LEGACY_SLOTS 4u
#define PICTURE_RECEIVE_TTL_MS (30u * 60u * 1000u)
#define PICTURE_SLOT_BYTES (6u + STORE_PICTURE_BITMAP_BYTES + STORE_PICTURE_TEXT_MAX + 1u)
#define PICTURE_PENDING_BYTES (22u + MODEM_SMS_SENDER_MAX + 1u + MODEM_SMS_TIMESTAMP_MAX + 1u + MODEM_SMS_BINARY_MAX)
#define PICTURE_PAYLOAD_BYTES (12u + STORE_PICTURE_SLOT_COUNT * (PICTURE_SLOT_BYTES + MODEM_SMS_SENDER_MAX + 1u) + STORE_PICTURE_PENDING_COUNT * PICTURE_PENDING_BYTES)
_Static_assert(PICTURE_PAYLOAD_BYTES <= STORAGE_JOURNAL_MAX_PAYLOAD,
               "pictures and pending receptions must fit one journal payload");

typedef struct {
    uint32_t id;
    uint16_t reference;
    uint16_t source_port;
    uint8_t state; /* 0 free, 1 collecting, 2 ready, 3 consumed, 4 conflicted */
    uint8_t total;
    uint8_t ref16;
    uint8_t concat;
    uint8_t dcs;
    uint8_t seen;
    uint8_t lengths[MODEM_SMS_SEGMENT_MAX];
    char sender[MODEM_SMS_SENDER_MAX + 1u];
    char timestamp[MODEM_SMS_TIMESTAMP_MAX + 1u];
    uint8_t data[MODEM_SMS_BINARY_MAX];
} picture_pending_t;

typedef struct {
    store_picture_message_t slots[STORE_PICTURE_SLOT_COUNT];
    char senders[STORE_PICTURE_SLOT_COUNT][MODEM_SMS_SENDER_MAX + 1u];
    picture_pending_t pending[STORE_PICTURE_PENDING_COUNT];
    uint32_t next_id;
} picture_message_state_t;

static uint32_t s_pending_touched[STORE_PICTURE_PENDING_COUNT];
static store_picture_message_t s_receive_decoded;
static sms_codec_message_t s_receive_part;

static const uint8_t DEFAULT_PICTURE_SLOT_0[STORE_PICTURE_BITMAP_BYTES] = {
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x1cu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x07u, 0xe0u, 0x00u, 0x72u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x3cu, 0x10u, 0x01u,
    0xc1u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0xe0u, 0x10u, 0xfeu, 0x03u, 0xe0u, 0x00u,
    0x00u, 0x00u, 0x1fu, 0x00u, 0x0bu, 0xffu, 0x8fu, 0xf8u, 0x00u, 0x00u, 0x00u, 0xf0u,
    0x00u, 0x07u, 0xffu, 0xdfu, 0xfcu, 0x00u, 0x00u, 0x00u, 0x80u, 0x00u, 0x0fu, 0xedu,
    0xffu, 0xdcu, 0x00u, 0x00u, 0x00u, 0x60u, 0x2au, 0x0fu, 0x7fu, 0xefu, 0xceu, 0x00u,
    0x00u, 0x00u, 0x1fu, 0xd4u, 0x1eu, 0x7fu, 0xf7u, 0xeeu, 0x00u, 0x00u, 0x00u, 0x00u,
    0x60u, 0x1cu, 0xffu, 0xf7u, 0xeeu, 0x00u, 0x00u, 0x00u, 0x07u, 0x80u, 0x1cu, 0xffu,
    0xf3u, 0xfeu, 0x00u, 0x00u, 0x00u, 0x04u, 0x00u, 0x1cu, 0xffu, 0xfbu, 0xfeu, 0x00u,
    0x00u, 0x00u, 0x03u, 0x05u, 0x1cu, 0xffu, 0xfbu, 0xfeu, 0x00u, 0x07u, 0xf8u, 0x00u,
    0xfau, 0x9cu, 0xffu, 0xfbu, 0xfeu, 0x00u, 0x78u, 0x00u, 0x00u, 0x0cu, 0x1cu, 0x7fu,
    0xffu, 0xfeu, 0x00u, 0x00u, 0x00u, 0x00u, 0x30u, 0x0eu, 0x7fu, 0xffu, 0xfcu, 0x00u,
    0x00u, 0x00u, 0x00u, 0x21u, 0xceu, 0x7fu, 0xffu, 0xfcu, 0x00u, 0x00u, 0x1fu, 0xfeu,
    0x1eu, 0x37u, 0x3fu, 0xffu, 0xf8u, 0x00u, 0x07u, 0xe0u, 0x00u, 0x00u, 0x0fu, 0xbfu,
    0xffu, 0xf8u, 0x00u, 0x18u, 0x00u, 0x00u, 0x00u, 0x03u, 0xdfu, 0xffu, 0xf0u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0xefu, 0xffu, 0xe0u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0xffu, 0xffu, 0xc0u, 0x00u, 0x00u, 0x00u, 0x0fu, 0xffu, 0x00u, 0x7fu,
    0xffu, 0x80u, 0x00u, 0x00u, 0x07u, 0xf0u, 0x00u, 0x00u, 0x3fu, 0xffu, 0x00u, 0x00u,
    0x00u, 0x38u, 0x00u, 0x00u, 0x00u, 0x1fu, 0xfeu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x07u, 0xfcu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u,
    0xf8u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x60u, 0x00u, 0x00u
};

static const uint8_t DEFAULT_PICTURE_SLOT_1[STORE_PICTURE_BITMAP_BYTES] = {
    0x00u, 0x00u, 0x00u, 0x00u, 0x07u, 0xfcu, 0xe0u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x3cu, 0x03u, 0x10u, 0x00u, 0x00u, 0x00u, 0x70u, 0x00u, 0x00u, 0x42u, 0xacu,
    0x10u, 0x6cu, 0x00u, 0x01u, 0xfcu, 0x00u, 0x00u, 0x40u, 0x10u, 0x28u, 0x92u, 0x00u,
    0x07u, 0x1eu, 0x00u, 0x00u, 0x2fu, 0xa0u, 0x28u, 0x00u, 0x00u, 0x1fu, 0xe6u, 0x00u,
    0x03u, 0x20u, 0x40u, 0x28u, 0x00u, 0x00u, 0x33u, 0xb8u, 0x00u, 0x07u, 0x6eu, 0x80u,
    0x28u, 0x00u, 0x00u, 0x67u, 0x5cu, 0x00u, 0x06u, 0xa1u, 0x00u, 0x48u, 0x01u, 0x80u,
    0xeeu, 0xacu, 0x00u, 0x05u, 0x2au, 0x00u, 0x48u, 0x00u, 0x40u, 0xdcu, 0x50u, 0x00u,
    0x02u, 0x44u, 0x00u, 0x48u, 0x00u, 0x40u, 0xdcu, 0x28u, 0x00u, 0x04u, 0xa8u, 0x37u,
    0x50u, 0x00u, 0x30u, 0x1cu, 0x14u, 0x00u, 0x09u, 0x48u, 0x32u, 0x50u, 0x00u, 0x08u,
    0x08u, 0x14u, 0x00u, 0x0au, 0x90u, 0x0au, 0x90u, 0x00u, 0x08u, 0x00u, 0x0au, 0x00u,
    0x05u, 0x20u, 0x24u, 0xa0u, 0x00u, 0x00u, 0x00u, 0x0au, 0x00u, 0x0au, 0x48u, 0x38u,
    0xa0u, 0x00u, 0x00u, 0x00u, 0x0au, 0x00u, 0x14u, 0x80u, 0x21u, 0x40u, 0x00u, 0x00u,
    0x00u, 0x05u, 0x00u, 0x29u, 0x20u, 0x01u, 0x40u, 0x00u, 0x00u, 0x00u, 0x05u, 0x00u,
    0x26u, 0x00u, 0x01u, 0x80u, 0x00u, 0x00u, 0x00u, 0xfdu, 0xe0u, 0x28u, 0x80u, 0x01u,
    0x80u, 0x00u, 0x00u, 0x03u, 0x07u, 0x18u, 0x12u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u,
    0x0cu, 0x2bu, 0xaeu, 0x20u, 0x00u, 0x0eu, 0xc0u, 0x00u, 0x00u, 0x10u, 0x55u, 0xd7u,
    0x20u, 0x00u, 0x70u, 0x30u, 0x00u, 0x00u, 0x20u, 0xaau, 0xebu, 0x90u, 0x03u, 0x83u,
    0xcfu, 0x00u, 0x00u, 0x41u, 0x55u, 0x75u, 0xc8u, 0x7cu, 0x0fu, 0xf0u, 0xfcu, 0x00u,
    0x4au, 0xaau, 0xbau, 0xffu, 0x80u, 0xffu, 0xffu, 0x03u, 0xffu, 0x95u, 0x55u, 0x5du,
    0xe0u, 0x07u, 0xffu, 0xffu, 0x80u, 0x00u, 0x8au, 0xaau, 0xbcu, 0x00u, 0x00u, 0x00u,
    0x7fu, 0x80u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x03u, 0xe0u, 0x00u
};

static const uint8_t DEFAULT_PICTURE_SLOT_2[STORE_PICTURE_BITMAP_BYTES] = {
    0x00u, 0x00u, 0x00u, 0x80u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x04u, 0x00u, 0x00u, 0x10u, 0x00u, 0x00u, 0x00u, 0x38u, 0x10u, 0x00u, 0x04u, 0x40u,
    0x00u, 0x10u, 0x00u, 0x00u, 0x44u, 0x00u, 0x04u, 0x00u, 0xa1u, 0x0cu, 0x00u, 0x00u,
    0x00u, 0x42u, 0x01u, 0x0au, 0x11u, 0x12u, 0x82u, 0x00u, 0x00u, 0x00u, 0x49u, 0xc2u,
    0x51u, 0x29u, 0x54u, 0x41u, 0x00u, 0x00u, 0x00u, 0x30u, 0x22u, 0x15u, 0x44u, 0xa5u,
    0x41u, 0x20u, 0x00u, 0x00u, 0x01u, 0x01u, 0x0au, 0x54u, 0x42u, 0x82u, 0x00u, 0x10u,
    0x02u, 0x00u, 0x00u, 0x84u, 0x28u, 0xa1u, 0x01u, 0x02u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x4au, 0x10u, 0xa2u, 0x80u, 0x84u, 0x40u, 0x00u, 0x02u, 0x08u, 0x8au, 0x28u, 0xa2u,
    0x80u, 0x04u, 0x00u, 0x02u, 0x00u, 0xc1u, 0x0au, 0x28u, 0xa2u, 0x80u, 0x08u, 0x00u,
    0x00u, 0x41u, 0x01u, 0x0au, 0x28u, 0xa2u, 0x80u, 0x30u, 0x00u, 0x00u, 0x42u, 0x00u,
    0x7au, 0xf4u, 0xafu, 0xe0u, 0x40u, 0x10u, 0x00u, 0x32u, 0x80u, 0x87u, 0x03u, 0xf0u,
    0x10u, 0x50u, 0x00u, 0x00u, 0x0cu, 0x01u, 0x00u, 0x00u, 0x00u, 0x08u, 0x80u, 0x00u,
    0x00u, 0x00u, 0x21u, 0x00u, 0x00u, 0x00u, 0x0bu, 0x00u, 0x40u, 0x04u, 0x40u, 0x21u,
    0x10u, 0x61u, 0x80u, 0xc8u, 0x38u, 0x80u, 0x00u, 0x04u, 0x24u, 0xf8u, 0xffu, 0xc1u,
    0xf0u, 0x44u, 0x80u, 0x00u, 0x00u, 0x20u, 0x3fu, 0xffu, 0xffu, 0xe0u, 0x42u, 0x40u,
    0x40u, 0x18u, 0x10u, 0x2fu, 0xdfu, 0xbfu, 0x60u, 0x42u, 0x20u, 0x00u, 0x25u, 0x0cu,
    0x27u, 0x8fu, 0x1eu, 0x21u, 0x22u, 0x20u, 0x00u, 0xa4u, 0x00u, 0x2fu, 0xdfu, 0xbfu,
    0x60u, 0x1fu, 0x40u, 0x20u, 0xa4u, 0xc0u, 0x3fu, 0xffu, 0xffu, 0xe0u, 0x82u, 0x80u,
    0x00u, 0x59u, 0x23u, 0xffu, 0xffu, 0xffu, 0xfcu, 0x62u, 0x00u, 0x01u, 0x36u, 0x01u,
    0x00u, 0x00u, 0x00u, 0x08u, 0x1cu, 0x08u, 0x00u, 0x00u, 0x40u, 0xc0u, 0x00u, 0x00u,
    0x30u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x3fu, 0xffu, 0xffu, 0xc0u, 0x00u, 0x00u
};

static const uint8_t DEFAULT_PICTURE_SLOT_3[STORE_PICTURE_BITMAP_BYTES] = {
    0x00u, 0x00u, 0x02u, 0xd0u, 0x20u, 0x1bu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u,
    0x30u, 0x08u, 0x07u, 0xd0u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x20u, 0x10u, 0x03u,
    0xa0u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x40u, 0x21u, 0x83u, 0xe0u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x70u, 0x23u, 0xc2u, 0xd0u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0xf0u, 0x23u, 0xc7u, 0x98u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xe0u, 0xa1u, 0x02u,
    0xa8u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x60u, 0x28u, 0x07u, 0x20u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x35u, 0xf0u, 0x4eu, 0x30u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x1fu, 0xbfu, 0x1du, 0x10u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x0eu, 0xeeu, 0xffu,
    0x50u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x0fu, 0x0bu, 0xf4u, 0x40u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x0du, 0x26u, 0xd8u, 0x60u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x1du, 0x01u, 0x00u, 0xa0u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x31u, 0x28u, 0x21u,
    0x80u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x41u, 0x02u, 0x88u, 0xa0u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x15u, 0x24u, 0x03u, 0x30u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x03u, 0x1fu, 0x20u, 0xa8u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x4cu, 0x14u,
    0x30u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x02u, 0x00u, 0x82u, 0x68u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x04u, 0x92u, 0x25u, 0x30u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x08u, 0x20u, 0x81u, 0xb0u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x08u, 0x8au, 0x00u,
    0x90u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x12u, 0x00u, 0x64u, 0x40u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x10u, 0xe1u, 0x80u, 0x40u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x14u, 0x73u, 0x95u, 0x40u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x11u, 0x57u, 0x00u,
    0x40u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x14u, 0x3eu, 0xa4u, 0x40u, 0x00u, 0x00u
};

static const uint8_t *const DEFAULT_PICTURE_BITMAPS[STORE_PICTURE_SLOT_COUNT] = {
    DEFAULT_PICTURE_SLOT_0,
    DEFAULT_PICTURE_SLOT_1,
    DEFAULT_PICTURE_SLOT_2,
    DEFAULT_PICTURE_SLOT_3,
};

static const char *const DEFAULT_PICTURE_TEXTS[STORE_PICTURE_SLOT_COUNT] = {
    "",
    "Ich hass euer scheiss wetter.",
    "",
    "",
};

static picture_message_state_t s_picture_messages;

uint8_t store_picture_message_count(void) {
    uint8_t count = 0u;
    for (uint8_t i = 0u; i < STORE_PICTURE_SLOT_COUNT; i++) {
        if (s_picture_messages.slots[i].used) {
            count++;
        }
    }
    return count;
}

store_status_t store_picture_message_get(uint8_t slot, store_picture_message_t *out_message) {
    if (slot >= STORE_PICTURE_SLOT_COUNT || out_message == 0) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    if (!s_picture_messages.slots[slot].used) {
        return STORE_STATUS_NOT_FOUND;
    }
    *out_message = s_picture_messages.slots[slot];
    return STORE_STATUS_OK;
}

store_status_t store_picture_message_set(uint8_t slot, const store_picture_message_t *message) {
    if (slot >= STORE_PICTURE_SLOT_COUNT || message == 0 ||
        message->bitmap_len > STORE_PICTURE_BITMAP_BYTES) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    s_picture_messages.slots[slot] = *message;
    s_picture_messages.slots[slot].used = true;
    s_picture_messages.slots[slot].width = message->width == 0u ? STORE_PICTURE_WIDTH : message->width;
    s_picture_messages.slots[slot].height = message->height == 0u ? STORE_PICTURE_HEIGHT : message->height;
    s_picture_messages.slots[slot].bitmap_len = message->bitmap_len;
    s_picture_messages.slots[slot].text[STORE_PICTURE_TEXT_MAX] = '\0';
    memset(s_picture_messages.senders[slot], 0, sizeof(s_picture_messages.senders[slot]));
    return store_engine_mark_dirty(STORE_UNIT_PICTURE_MESSAGES);
}

store_status_t store_picture_message_set_text(uint8_t slot, const char *text) {
    if (slot >= STORE_PICTURE_SLOT_COUNT || text == NULL) return STORE_STATUS_INVALID_ARGUMENT;
    if (!s_picture_messages.slots[slot].used) return STORE_STATUS_NOT_FOUND;
    store_copy_text(s_picture_messages.slots[slot].text,
                    sizeof(s_picture_messages.slots[slot].text), text);
    return store_engine_mark_dirty(STORE_UNIT_PICTURE_MESSAGES);
}

store_status_t store_picture_message_clear(uint8_t slot) {
    if (slot >= STORE_PICTURE_SLOT_COUNT) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    memset(&s_picture_messages.slots[slot], 0, sizeof(s_picture_messages.slots[slot]));
    memset(s_picture_messages.senders[slot], 0, sizeof(s_picture_messages.senders[slot]));
    return store_engine_mark_dirty(STORE_UNIT_PICTURE_MESSAGES);
}

store_status_t store_picture_message_sender(uint8_t slot, char *dst, size_t cap) {
    if (slot >= STORE_PICTURE_SLOT_COUNT || dst == NULL || cap == 0u) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    store_copy_text(dst, cap, s_picture_messages.senders[slot]);
    return s_picture_messages.slots[slot].used ? STORE_STATUS_OK : STORE_STATUS_NOT_FOUND;
}

store_status_t store_picture_commit_status(void) {
    store_diag_snapshot_t diag;
    store_service_get_diag(&diag);
    if (!diag.ready) return STORE_STATUS_NOT_READY;
    if (diag.degraded_mask & (1u << STORE_UNIT_PICTURE_MESSAGES)) return STORE_STATUS_STORAGE_ERROR;
    return (diag.dirty_mask & (1u << STORE_UNIT_PICTURE_MESSAGES))
        ? STORE_STATUS_NOT_READY : STORE_STATUS_OK;
}

static uint16_t pending_size(const picture_pending_t *p) {
    uint16_t len = 0u;
    for (uint8_t i = 0u; i < MODEM_SMS_SEGMENT_MAX; i++) len += p->lengths[i];
    return len;
}

static picture_pending_t *pending_by_id(uint32_t id) {
    if (id == 0u) return NULL;
    for (uint8_t i = 0u; i < STORE_PICTURE_PENDING_COUNT; i++) {
        picture_pending_t *p = &s_picture_messages.pending[i];
        if (p->id == id && (p->state == 2u || p->state == 3u)) return p;
    }
    return NULL;
}

static bool picture_timestamp_matches(const char *saved, const char *incoming,
                                       bool concatenated) {
    if (!concatenated || strlen(saved) != 17u || strlen(incoming) != 17u ||
        saved[8] != ',' || incoming[8] != ',' ||
        saved[11] != ':' || incoming[11] != ':' ||
        saved[14] != ':' || incoming[14] != ':') {
        return strcmp(saved, incoming) == 0;
    }
    if (memcmp(saved, incoming, 8u) != 0) return false;
    uint32_t seconds[2] = {0u, 0u};
    const char *stamps[] = {saved, incoming};
    for (uint8_t stamp = 0u; stamp < 2u; stamp++) {
        for (uint8_t field = 0u; field < 3u; field++) {
            const char *digits = stamps[stamp] + 9u + field * 3u;
            if (digits[0] < '0' || digits[0] > '9' ||
                digits[1] < '0' || digits[1] > '9') return strcmp(saved, incoming) == 0;
            uint8_t value = (uint8_t)((digits[0] - '0') * 10 + digits[1] - '0');
            if (value > (field == 0u ? 23u : 59u)) return strcmp(saved, incoming) == 0;
            seconds[stamp] = seconds[stamp] * 60u + value;
        }
    }
    uint32_t gap = seconds[0] > seconds[1] ? seconds[0] - seconds[1] : seconds[1] - seconds[0];
    /* Uptime restarts, but a saved reference must not shadow a whole day's SMS. */
    return gap < PICTURE_RECEIVE_TTL_MS / 1000u;
}

store_status_t store_picture_receive(const sms_codec_message_t *part, uint32_t now_ms) {
    if (part == NULL || part->submit || !part->binary || !part->has_ports ||
        part->dest_port != SMS_CODEC_PICTURE_PORT || part->udh_unhandled ||
        part->trailing_data || part->pid != 0u ||
        (part->dcs != 4u && part->dcs != 0xf5u) ||
        part->binary_len == 0u || part->binary_len > 140u ||
        part->address[0] == '\0') return STORE_STATUS_INVALID_ARGUMENT;
    uint8_t total = part->has_concat ? part->concat_total : 1u;
    uint8_t seq = part->has_concat ? part->concat_seq : 1u;
    if (total == 0u || total > MODEM_SMS_SEGMENT_MAX || seq == 0u || seq > total) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    if (!store_service_ready()) {
        return STORE_STATUS_NOT_READY;
    }
    picture_pending_t *p = NULL;
    uint8_t selected = 0u;
    int free_slot = -1;
    for (uint8_t i = 0u; i < STORE_PICTURE_PENDING_COUNT; i++) {
        picture_pending_t *candidate = &s_picture_messages.pending[i];
        if (candidate->state != 2u && candidate->state != 0u &&
            (candidate->state != 3u || store_picture_commit_status() == STORE_STATUS_OK) &&
            (uint32_t)(now_ms - s_pending_touched[i]) >= PICTURE_RECEIVE_TTL_MS) {
            memset(candidate, 0, sizeof(*candidate));
            (void)store_engine_mark_dirty(STORE_UNIT_PICTURE_MESSAGES);
        }
        if (candidate->state != 0u && candidate->reference == part->concat_ref &&
            candidate->ref16 == part->concat_ref_16bit && candidate->concat == part->has_concat &&
            candidate->source_port == part->source_port &&
            strcmp(candidate->sender, part->address) == 0 &&
            picture_timestamp_matches(candidate->timestamp, part->timestamp, part->has_concat)) {
            p = candidate;
            selected = i;
            break;
        }
        if (candidate->state == 0u ||
            (candidate->state == 3u && store_picture_commit_status() == STORE_STATUS_OK)) free_slot = i;
    }
    if (p == NULL) {
        if (free_slot < 0) return STORE_STATUS_STORAGE_ERROR;
        selected = (uint8_t)free_slot;
        p = &s_picture_messages.pending[selected];
        memset(p, 0, sizeof(*p));
        p->state = 1u;
        p->id = ++s_picture_messages.next_id;
        if (p->id == 0u) p->id = ++s_picture_messages.next_id;
        p->reference = part->concat_ref;
        p->source_port = part->source_port;
        p->total = total;
        p->ref16 = part->concat_ref_16bit;
        p->concat = part->has_concat;
        p->dcs = part->dcs;
        store_copy_text(p->sender, sizeof(p->sender), part->address);
        store_copy_text(p->timestamp, sizeof(p->timestamp), part->timestamp);
    }
    s_pending_touched[selected] = now_ms;
    if (p->state == 4u) return STORE_STATUS_CONFLICT;
    uint16_t offset = 0u;
    for (uint8_t i = 0u; i + 1u < seq; i++) offset += p->lengths[i];
    uint16_t size = pending_size(p);
    bool duplicate = (p->seen & (1u << (seq - 1u))) != 0u;
    if (p->total != total || p->dcs != part->dcs ||
        (duplicate && (p->lengths[seq - 1u] != part->binary_len ||
                       memcmp(p->data + offset, part->binary_data, part->binary_len) != 0)) ||
        (!duplicate && size + part->binary_len > sizeof(p->data))) {
        /* Never invalidate a complete picture because of a colliding ref. */
        if (p->state == 1u) {
            p->state = 4u;
            (void)store_engine_mark_dirty(STORE_UNIT_PICTURE_MESSAGES);
        }
        return STORE_STATUS_CONFLICT;
    }
    if (duplicate) return STORE_STATUS_OK;
    (void)store_engine_mark_dirty(STORE_UNIT_PICTURE_MESSAGES);
    memmove(p->data + offset + part->binary_len, p->data + offset, size - offset);
    memcpy(p->data + offset, part->binary_data, part->binary_len);
    p->lengths[seq - 1u] = (uint8_t)part->binary_len;
    p->seen |= (uint8_t)(1u << (seq - 1u));
    if (p->seen == (uint8_t)((1u << total) - 1u)) {
        if (!sms_picture_payload_decode(p->data, pending_size(p), &s_receive_decoded)) {
            p->state = 4u;
            return STORE_STATUS_INVALID_ARGUMENT;
        }
        p->state = 2u;
    }
    return STORE_STATUS_OK;
}

store_status_t store_picture_receive_pdu(const char *pdu, uint32_t now_ms) {
    if (!sms_pdu_decode(pdu, &s_receive_part) || !s_receive_part.has_ports ||
        s_receive_part.dest_port != SMS_CODEC_PICTURE_PORT) return STORE_STATUS_NOT_FOUND;
    return store_picture_receive(&s_receive_part, now_ms);
}

uint32_t store_picture_pending_first(void) {
    if (store_picture_commit_status() != STORE_STATUS_OK) return 0u;
    uint32_t id = 0u;
    for (uint8_t i = 0u; i < STORE_PICTURE_PENDING_COUNT; i++) {
        const picture_pending_t *p = &s_picture_messages.pending[i];
        if (p->state == 2u && (id == 0u || (int32_t)(p->id - id) < 0)) id = p->id;
    }
    return id;
}

store_status_t store_picture_pending_get(uint32_t id, store_picture_message_t *out,
                                         char *sender, size_t sender_cap) {
    picture_pending_t *p = pending_by_id(id);
    if (p == NULL) return STORE_STATUS_NOT_FOUND;
    if (out == NULL || !sms_picture_payload_decode(p->data, pending_size(p), out)) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    if (sender != NULL) store_copy_text(sender, sender_cap, p->sender);
    return STORE_STATUS_OK;
}

store_status_t store_picture_pending_save(uint32_t id, uint8_t slot) {
    picture_pending_t *p = pending_by_id(id);
    if (p == NULL) return STORE_STATUS_NOT_FOUND;
    if (slot >= STORE_PICTURE_SLOT_COUNT ||
        !sms_picture_payload_decode(p->data, pending_size(p), &s_receive_decoded)) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    store_status_t status = store_engine_mark_dirty(STORE_UNIT_PICTURE_MESSAGES);
    if (status != STORE_STATUS_OK) return status;
    s_picture_messages.slots[slot] = s_receive_decoded;
    store_copy_text(s_picture_messages.senders[slot], MODEM_SMS_SENDER_MAX + 1u, p->sender);
    p->state = 3u; /* gallery + consumption share the same atomic journal record */
    return STORE_STATUS_OK;
}

store_status_t store_picture_pending_discard(uint32_t id) {
    picture_pending_t *p = pending_by_id(id);
    if (p == NULL) return STORE_STATUS_NOT_FOUND;
    store_status_t status = store_engine_mark_dirty(STORE_UNIT_PICTURE_MESSAGES);
    if (status == STORE_STATUS_OK) p->state = 3u;
    return status;
}

static void seed_default_picture_messages(void) {
    memset(&s_picture_messages, 0, sizeof(s_picture_messages));
    for (uint8_t i = 0u; i < PICTURE_LEGACY_SLOTS; i++) {
        store_picture_message_t *slot = &s_picture_messages.slots[i];
        slot->used = true;
        slot->width = STORE_PICTURE_WIDTH;
        slot->height = STORE_PICTURE_HEIGHT;
        slot->bitmap_len = STORE_PICTURE_BITMAP_BYTES;
        memcpy(slot->bitmap, DEFAULT_PICTURE_BITMAPS[i], STORE_PICTURE_BITMAP_BYTES);
        store_copy_text(slot->text, sizeof(slot->text), DEFAULT_PICTURE_TEXTS[i]);
    }
}

static void reset_pictures_unit(uint8_t instance) {
    (void)instance;
    memset(&s_picture_messages, 0, sizeof(s_picture_messages));
    memset(s_pending_touched, 0, sizeof(s_pending_touched));
}

static void fallback_pictures_unit(uint8_t instance) {
    (void)instance;
    seed_default_picture_messages();
}

static bool serialize_picture_messages(uint8_t *dst, size_t cap, size_t *out_len) {
    size_t pos = 0u;
    uint8_t count = store_picture_message_count();
    if (!write_u32_field(dst, cap, &pos, PICTURE_MAGIC) ||
        !write_u16_field(dst, cap, &pos, PICTURE_VERSION) ||
        !write_u8_field(dst, cap, &pos, count) ||
        !write_u8_field(dst, cap, &pos, STORE_PICTURE_SLOT_COUNT) ||
        !write_u32_field(dst, cap, &pos, s_picture_messages.next_id)) {
        return false;
    }
    for (uint8_t i = 0u; i < STORE_PICTURE_SLOT_COUNT; i++) {
        const store_picture_message_t *slot = &s_picture_messages.slots[i];
        uint8_t fixed_bitmap[STORE_PICTURE_BITMAP_BYTES];
        uint8_t fixed_text[STORE_PICTURE_TEXT_MAX + 1u];
        uint16_t bitmap_len = slot->bitmap_len <= STORE_PICTURE_BITMAP_BYTES
            ? slot->bitmap_len
            : STORE_PICTURE_BITMAP_BYTES;
        uint8_t text_len = (uint8_t)strnlen(slot->text, STORE_PICTURE_TEXT_MAX);
        memset(fixed_bitmap, 0, sizeof(fixed_bitmap));
        memset(fixed_text, 0, sizeof(fixed_text));
        memcpy(fixed_bitmap, slot->bitmap, bitmap_len);
        memcpy(fixed_text, slot->text, text_len);
        if (!write_u8_field(dst, cap, &pos, slot->used ? 1u : 0u) ||
            !write_u8_field(dst, cap, &pos, slot->width) ||
            !write_u8_field(dst, cap, &pos, slot->height) ||
            !write_u16_field(dst, cap, &pos, bitmap_len) ||
            !write_u8_field(dst, cap, &pos, text_len) ||
            !write_bytes(dst, cap, &pos, fixed_bitmap, sizeof(fixed_bitmap)) ||
            !write_bytes(dst, cap, &pos, fixed_text, sizeof(fixed_text)) ||
            !write_bytes(dst, cap, &pos, s_picture_messages.senders[i], MODEM_SMS_SENDER_MAX + 1u)) {
            return false;
        }
    }
    for (uint8_t i = 0u; i < STORE_PICTURE_PENDING_COUNT; i++) {
        const picture_pending_t *p = &s_picture_messages.pending[i];
        if (!write_u8_field(dst, cap, &pos, p->state) ||
            !write_u32_field(dst, cap, &pos, p->id) ||
            !write_u16_field(dst, cap, &pos, p->reference) ||
            !write_u16_field(dst, cap, &pos, p->source_port) ||
            !write_u8_field(dst, cap, &pos, p->total) ||
            !write_u8_field(dst, cap, &pos, p->ref16) ||
            !write_u8_field(dst, cap, &pos, p->concat) ||
            !write_u8_field(dst, cap, &pos, p->dcs) ||
            !write_u8_field(dst, cap, &pos, p->seen) ||
            !write_bytes(dst, cap, &pos, p->lengths, sizeof(p->lengths)) ||
            !write_bytes(dst, cap, &pos, p->sender, sizeof(p->sender)) ||
            !write_bytes(dst, cap, &pos, p->timestamp, sizeof(p->timestamp)) ||
            !write_bytes(dst, cap, &pos, p->data, sizeof(p->data))) {
            return false;
        }
    }
    *out_len = pos;
    return true;
}

static bool apply_picture_payload(const uint8_t *payload, size_t len) {
    if (len < 8u || read_u32(&payload[0]) != PICTURE_MAGIC) {
        return false;
    }
    uint16_t version = read_u16(&payload[4]);
    bool legacy = version == 1u;
    if ((!legacy && version != PICTURE_VERSION) ||
        len != (legacy ? 8u + PICTURE_LEGACY_SLOTS * PICTURE_SLOT_BYTES : PICTURE_PAYLOAD_BYTES) ||
        (!legacy && payload[7] != STORE_PICTURE_SLOT_COUNT)) {
        return false;
    }
    /* Journal load runs on the small core0 stack. Validate off-stack before
     * publishing, so a corrupt record never half-replaces the live state. */
    static picture_message_state_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    size_t pos = legacy ? 8u : 12u;
    loaded.next_id = legacy ? 0u : read_u32(&payload[8]);
    for (uint8_t i = 0u; i < (legacy ? PICTURE_LEGACY_SLOTS : STORE_PICTURE_SLOT_COUNT); i++) {
        if (pos + 6u + STORE_PICTURE_BITMAP_BYTES + STORE_PICTURE_TEXT_MAX + 1u > len) {
            return false;
        }
        store_picture_message_t *slot = &loaded.slots[i];
        slot->used = payload[pos++] != 0u;
        slot->width = payload[pos++];
        slot->height = payload[pos++];
        slot->bitmap_len = read_u16(&payload[pos]);
        pos += 2u;
        uint8_t text_len = payload[pos++];
        if (slot->bitmap_len > STORE_PICTURE_BITMAP_BYTES || text_len > STORE_PICTURE_TEXT_MAX) {
            return false;
        }
        memcpy(slot->bitmap, &payload[pos], slot->bitmap_len);
        pos += STORE_PICTURE_BITMAP_BYTES;
        memcpy(slot->text, &payload[pos], text_len);
        slot->text[text_len] = '\0';
        pos += STORE_PICTURE_TEXT_MAX + 1u;
        if (!legacy) {
            memcpy(loaded.senders[i], &payload[pos], MODEM_SMS_SENDER_MAX + 1u);
            loaded.senders[i][MODEM_SMS_SENDER_MAX] = '\0';
            pos += MODEM_SMS_SENDER_MAX + 1u;
        }
        if (slot->used && (slot->width == 0u || slot->height == 0u)) {
            slot->width = STORE_PICTURE_WIDTH;
            slot->height = STORE_PICTURE_HEIGHT;
        }
    }
    if (!legacy) {
        for (uint8_t i = 0u; i < STORE_PICTURE_PENDING_COUNT; i++) {
            picture_pending_t *p = &loaded.pending[i];
            p->state = payload[pos++];
            p->id = read_u32(&payload[pos]); pos += 4u;
            p->reference = read_u16(&payload[pos]); pos += 2u;
            p->source_port = read_u16(&payload[pos]); pos += 2u;
            p->total = payload[pos++];
            p->ref16 = payload[pos++];
            p->concat = payload[pos++];
            p->dcs = payload[pos++];
            p->seen = payload[pos++];
            memcpy(p->lengths, &payload[pos], sizeof(p->lengths)); pos += sizeof(p->lengths);
            memcpy(p->sender, &payload[pos], sizeof(p->sender)); pos += sizeof(p->sender);
            memcpy(p->timestamp, &payload[pos], sizeof(p->timestamp)); pos += sizeof(p->timestamp);
            memcpy(p->data, &payload[pos], sizeof(p->data)); pos += sizeof(p->data);
            unsigned bytes = 0u;
            for (uint8_t j = 0u; j < MODEM_SMS_SEGMENT_MAX; j++) {
                if (p->lengths[j] > 140u ||
                    (((p->seen >> j) & 1u) != (p->lengths[j] != 0u))) return false;
                bytes += p->lengths[j];
            }
            if (p->state > 4u || p->ref16 > 1u || p->concat > 1u ||
                bytes > MODEM_SMS_BINARY_MAX ||
                p->sender[MODEM_SMS_SENDER_MAX] != '\0' ||
                p->timestamp[MODEM_SMS_TIMESTAMP_MAX] != '\0') return false;
            if (p->state != 0u && (p->id == 0u || p->total == 0u ||
                p->total > MODEM_SMS_SEGMENT_MAX || (p->seen >> p->total) != 0u)) return false;
            if ((p->state == 2u || p->state == 3u) &&
                (p->seen != (uint8_t)((1u << p->total) - 1u) ||
                 !sms_picture_payload_decode(p->data, (uint16_t)bytes, &s_receive_decoded))) return false;
            s_pending_touched[i] = time_ms();
        }
    }
    s_picture_messages = loaded;
    return true;
}

static bool serialize_pictures_unit(uint8_t instance,
                                    uint8_t *dst,
                                    size_t cap,
                                    size_t *out_len) {
    (void)instance;
    return serialize_picture_messages(dst, cap, out_len);
}

static bool apply_pictures_unit(uint8_t instance,
                                const uint8_t *payload,
                                size_t len) {
    (void)instance;
    return apply_picture_payload(payload, len);
}

const store_unit_ops_t g_store_pictures_unit_ops = {
    .reset_ram = reset_pictures_unit,
    .serialize = serialize_pictures_unit,
    .apply = apply_pictures_unit,
    .fallback_missing_or_corrupt = fallback_pictures_unit,
    .name = "picture messages",
};
