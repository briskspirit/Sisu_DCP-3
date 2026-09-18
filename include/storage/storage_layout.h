#ifndef STORAGE_LAYOUT_H
#define STORAGE_LAYOUT_H

/* Rev B2: leave the deployed A/B region intact until migration is qualified.
 * The build budget guard must protect both regions. */
#define STORAGE_LEGACY_BYTES (128u * 1024u)
#define STORAGE_RECORD_BYTES (128u * 1024u)
#define STORAGE_RESERVED_BYTES (STORAGE_LEGACY_BYTES + STORAGE_RECORD_BYTES)

#endif
