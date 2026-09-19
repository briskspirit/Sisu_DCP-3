#ifndef STORAGE_LAYOUT_H
#define STORAGE_LAYOUT_H

/* Rev B2: system 0x101b0000..0x101bffff; user 0x101c0000..0x101fffff.
 * The stage-1 user filesystem grows over the old journal only after system
 * records have an independently committed authority marker. */
#define STORAGE_LEGACY_BYTES (128u * 1024u)
#define STORAGE_RECORD_BYTES (128u * 1024u)
#define STORAGE_SYSTEM_BYTES (64u * 1024u)
#define STORAGE_USER_BYTES (256u * 1024u)
#define STORAGE_RESERVED_BYTES (STORAGE_SYSTEM_BYTES + STORAGE_USER_BYTES)

#endif
