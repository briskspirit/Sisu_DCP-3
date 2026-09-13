#ifndef STORAGE_BYTES_H
#define STORAGE_BYTES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Little-endian byte-order helpers shared by the storage layer (the journal
 * and the persistent store serialize the same on-flash byte order). Internal
 * to src/storage/ -- nothing outside the storage engine reads flash bytes. */

static inline uint16_t read_u16(const uint8_t *src) {
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8);
}

static inline uint32_t read_u32(const uint8_t *src) {
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

static inline uint64_t read_u64(const uint8_t *src) {
    return (uint64_t)read_u32(src) |
           ((uint64_t)read_u32(src + 4u) << 32);
}

static inline int32_t read_i32(const uint8_t *src) {
    uint32_t raw = read_u32(src);
    int32_t value;
    memcpy(&value, &raw, sizeof(value));
    return value;
}

static inline int64_t read_i64(const uint8_t *src) {
    uint64_t raw = read_u64(src);
    int64_t value;
    memcpy(&value, &raw, sizeof(value));
    return value;
}

static inline void write_u16(uint8_t *dst, uint16_t value) {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static inline void write_u32(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static inline void write_u64(uint8_t *dst, uint64_t value) {
    write_u32(dst, (uint32_t)value);
    write_u32(dst + 4u, (uint32_t)(value >> 32));
}

static inline bool write_bytes(uint8_t *dst,
                               size_t cap,
                               size_t *pos,
                               const void *src,
                               size_t len) {
    if (*pos + len > cap) {
        return false;
    }
    memcpy(&dst[*pos], src, len);
    *pos += len;
    return true;
}

static inline bool write_u8_field(uint8_t *dst,
                                  size_t cap,
                                  size_t *pos,
                                  uint8_t value) {
    return write_bytes(dst, cap, pos, &value, 1u);
}

static inline bool write_u16_field(uint8_t *dst,
                                   size_t cap,
                                   size_t *pos,
                                   uint16_t value) {
    uint8_t tmp[2];
    write_u16(tmp, value);
    return write_bytes(dst, cap, pos, tmp, sizeof(tmp));
}

static inline bool write_u32_field(uint8_t *dst,
                                   size_t cap,
                                   size_t *pos,
                                   uint32_t value) {
    uint8_t tmp[4];
    write_u32(tmp, value);
    return write_bytes(dst, cap, pos, tmp, sizeof(tmp));
}

static inline bool write_i32_field(uint8_t *dst,
                                   size_t cap,
                                   size_t *pos,
                                   int32_t value) {
    uint32_t raw;
    memcpy(&raw, &value, sizeof(raw));
    return write_u32_field(dst, cap, pos, raw);
}

static inline bool write_i64_field(uint8_t *dst,
                                   size_t cap,
                                   size_t *pos,
                                   int64_t value) {
    uint64_t raw;
    uint8_t tmp[8];
    memcpy(&raw, &value, sizeof(raw));
    write_u64(tmp, raw);
    return write_bytes(dst, cap, pos, tmp, sizeof(tmp));
}

#endif /* STORAGE_BYTES_H */
