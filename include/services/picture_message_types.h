#ifndef PICTURE_MESSAGE_TYPES_H
#define PICTURE_MESSAGE_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#define STORE_PICTURE_SLOT_COUNT 7u
#define STORE_PICTURE_PENDING_COUNT 2u
#define STORE_PICTURE_WIDTH 72u
#define STORE_PICTURE_HEIGHT 28u
#define STORE_PICTURE_BITMAP_BYTES ((STORE_PICTURE_WIDTH * STORE_PICTURE_HEIGHT) / 8u)
#define STORE_PICTURE_TEXT_MAX 120u

/* Shared picture-message value contract. The historical store_ prefix is kept
 * because this exact DTO is persisted as well as encoded for SMS transport. */
typedef struct {
    bool used;
    uint8_t width;
    uint8_t height;
    uint16_t bitmap_len;
    uint8_t bitmap[STORE_PICTURE_BITMAP_BYTES];
    char text[STORE_PICTURE_TEXT_MAX + 1u];
} store_picture_message_t;

#endif /* PICTURE_MESSAGE_TYPES_H */
