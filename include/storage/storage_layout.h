#ifndef STORAGE_LAYOUT_H
#define STORAGE_LAYOUT_H

/* Rev B2: system 0x10190000..0x1019ffff; user 0x101a0000..0x101fffff.
 * Deployment initializes these ranges as erased media. No legacy import runs.
 * The legacy constants below belong only to the disconnected journal bench. */
#define STORAGE_LEGACY_BYTES (128u * 1024u)
#define STORAGE_RECORD_BYTES (128u * 1024u)
#define STORAGE_SYSTEM_BYTES (64u * 1024u)
#define STORAGE_USER_BYTES (384u * 1024u)
#define STORAGE_RESERVED_BYTES (STORAGE_SYSTEM_BYTES + STORAGE_USER_BYTES)

#endif
