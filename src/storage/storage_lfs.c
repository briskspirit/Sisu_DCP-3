#include "storage/storage_lfs.h"
#include "storage/storage_objects.h"
#include "storage/storage_layout.h"
#include "storage/storage_user_space.h"

#include <string.h>

#include "lfs.h"
#include "storage_bytes.h"

#define RECORD_MAGIC 0x31525353u
#define RECORD_HEADER 16u
#define PROGRAM_SIZE 256u
#define CACHE_SIZE 512u
#define LOOKAHEAD_SIZE 16u

typedef struct {
    lfs_t fs;
    struct lfs_config cfg;
    nvm_hal_t *hal;
    uint8_t read_cache[CACHE_SIZE];
    uint8_t prog_cache[CACHE_SIZE];
    uint8_t file_cache[CACHE_SIZE];
    uint8_t lookahead[LOOKAHEAD_SIZE];
    bool mounted;
    bool io_busy;
    bool needs_remount;
} record_fs_t;

static record_fs_t s_volumes[STORAGE_LFS_VOLUME_COUNT];
static lfs_dir_t s_object_dir;
static bool s_object_scan_open;
static bool s_objects_ready;
static uint32_t s_next_object;
static const char *const OBJECT_DIRS[] = {"contacts", "inbox", "outbox", "pending"};
_Static_assert(sizeof(OBJECT_DIRS) / sizeof(OBJECT_DIRS[0]) ==
               STORAGE_OBJECT_COLLECTION_COUNT, "object collection paths");
_Static_assert((unsigned)STORAGE_OBJECT_CONTACT == (unsigned)STORAGE_USER_CONTACTS &&
               (unsigned)STORAGE_OBJECT_INBOX == (unsigned)STORAGE_USER_INBOX &&
               (unsigned)STORAGE_OBJECT_OUTBOX == (unsigned)STORAGE_USER_OUTBOX &&
               (unsigned)STORAGE_OBJECT_PENDING_SMS == (unsigned)STORAGE_USER_PENDING &&
               (unsigned)STORAGE_OBJECT_COLLECTION_COUNT == (unsigned)STORAGE_USER_OTHER,
               "object collections must map to their user-space pools");
#define OBJECT_MAGIC 0x314a424fu
#define OBJECT_HEADER 20u
#define OBJECT_COUNTER_ID 0xfff1u
#define OBJECT_STATE_ATTR 0x53u

#define USER_BLOCKS (STORAGE_USER_BYTES / 4096u)
#define RECOVERY_BLOCKS 8u
#define RECLAIM_BLOCKS 4u
static const uint8_t USER_LIMIT_BLOCKS[STORAGE_USER_POOL_COUNT] = {16, 36, 12, 8, 16};
_Static_assert(STORAGE_USER_BYTES == 384u * 1024u, "review user budgets with geometry");
_Static_assert(16u + 36u + 12u + 8u + 16u + RECOVERY_BLOCKS == USER_BLOCKS,
               "user budgets and recovery reserve must cover the volume");
static storage_user_usage_t s_usage;
/* Zero is free; otherwise the value is one plus a storage_user_pool_t. */
static uint8_t s_block_owner[USER_BLOCKS];
static bool s_usage_valid;
static bool s_budget_active;
static unsigned s_budget_left;
static uint8_t s_budget_seen[(USER_BLOCKS + 7u) / 8u];

static int usage_measure(record_fs_t *fs);
static int budget_begin(record_fs_t *fs, storage_user_pool_t pool, bool reclaim);
static void budget_end(void);

static int allocation_guard(const struct lfs_config *cfg, lfs_block_t block) {
    if (cfg->context != &s_volumes[STORAGE_LFS_USER]) return 0;
    s_usage_valid = false;
    if (!s_budget_active) return 0;
    if (block >= USER_BLOCKS) return LFS_ERR_CORRUPT;
    uint8_t bit = (uint8_t)(1u << (block % 8u));
    if (!(s_budget_seen[block / 8u] & bit)) {
        /* Also charge blocks freed and reused during this operation: their
         * previous owner may have been a different pool. */
        if (s_budget_left == 0u) return LFS_ERR_NOSPC;
        s_budget_left--;
        s_budget_seen[block / 8u] |= bit;
    }
    return 0;
}

static int io_result(record_fs_t *fs, nvm_status_t result) {
    if (result == NVM_STATUS_BUSY) {
        fs->io_busy = true;
    }
    return result == NVM_STATUS_OK ? 0 : LFS_ERR_IO;
}

static bool block_range(const struct lfs_config *cfg, lfs_block_t block,
                        lfs_off_t off, lfs_size_t size) {
    const record_fs_t *fs = cfg->context;
    return block < fs->hal->capacity / cfg->block_size && off <= cfg->block_size &&
           size <= cfg->block_size - off;
}

static int bd_read(const struct lfs_config *cfg, lfs_block_t block,
                    lfs_off_t off, void *dst, lfs_size_t size) {
    record_fs_t *fs = cfg->context;
    if (!block_range(cfg, block, off, size)) {
        return LFS_ERR_IO;
    }
    return io_result(fs, fs->hal->read(fs->hal, block * cfg->block_size + off,
                                      dst, size));
}

static int bd_prog(const struct lfs_config *cfg, lfs_block_t block,
                    lfs_off_t off, const void *src, lfs_size_t size) {
    record_fs_t *fs = cfg->context;
    if (fs == &s_volumes[STORAGE_LFS_USER]) s_usage_valid = false;
    if (!block_range(cfg, block, off, size)) {
        return LFS_ERR_IO;
    }
    /* Match the physical page boundary even when the filesystem combines two
     * pages in its cache. Fault tests and the HAL see each program separately. */
    if (off % PROGRAM_SIZE != 0u || size % PROGRAM_SIZE != 0u) return LFS_ERR_IO;
    for (lfs_size_t pos = 0u; pos < size; pos += PROGRAM_SIZE) {
        int rc = io_result(fs, fs->hal->write(fs->hal, block * cfg->block_size + off + pos,
                                             (const uint8_t *)src + pos, PROGRAM_SIZE));
        if (rc != 0) return rc;
    }
    return 0;
}

static int bd_erase(const struct lfs_config *cfg, lfs_block_t block) {
    record_fs_t *fs = cfg->context;
    if (!block_range(cfg, block, 0u, cfg->block_size)) {
        return LFS_ERR_IO;
    }
    if (fs == &s_volumes[STORAGE_LFS_USER]) {
        s_usage_valid = false;
    }
    return io_result(fs, fs->hal->erase(fs->hal, block * cfg->block_size,
                                       cfg->block_size));
}

static int bd_sync(const struct lfs_config *cfg) {
    (void)cfg; /* HAL writes are synchronous and reads see committed media. */
    return 0;
}

static uint32_t record_crc(const uint8_t *data, size_t len) {
    return lfs_crc(0xffffffffu, data, len) ^ 0xffffffffu;
}

static void record_path(uint16_t id, char path[6]) {
    static const char hex[] = "0123456789abcdef";
    path[0] = 'u';
    for (unsigned i = 0; i < 4u; i++) {
        path[i + 1u] = hex[(id >> (12u - i * 4u)) & 15u];
    }
    path[5] = '\0';
}

static storage_record_result_t result_for(record_fs_t *fs, int result) {
    if (result >= 0) {
        return STORAGE_RECORD_OK;
    }
    if (result == LFS_ERR_NOENT) {
        return STORAGE_RECORD_NOT_FOUND;
    }
    /* An I/O failure may leave allocator/cache state ahead of durable media. */
    fs->needs_remount = true;
    if (result == LFS_ERR_NOSPC && !fs->io_busy) {
        return STORAGE_RECORD_FULL;
    }
    return fs->io_busy ? STORAGE_RECORD_BUSY : STORAGE_RECORD_ERROR;
}

static int mount_consistent(record_fs_t *fs) {
    if (fs == &s_volumes[STORAGE_LFS_USER]) s_usage_valid = false;
    int rc = lfs_mount(&fs->fs, &fs->cfg);
    if (rc != 0) {
        return rc;
    }
    fs->mounted = true;
    if (fs->fs.block_count > fs->hal->capacity / fs->cfg.block_size ||
        fs->fs.block_count < 4u) {
        return LFS_ERR_CORRUPT;
    }
    /* Finish interrupted moves before any read can be interpreted as a missing
     * record. Mount alone defers this recovery until the first writable open. */
    return lfs_fs_mkconsistent(&fs->fs);
}

static int prepare(record_fs_t *fs) {
    fs->io_busy = false;
    if (!fs->mounted && !fs->needs_remount) {
        return LFS_ERR_IO;
    }
    if (fs->needs_remount) {
        if (fs == &s_volumes[STORAGE_LFS_USER]) {
            s_object_scan_open = false;
            s_objects_ready = false;
        }
        if (fs->mounted) {
            lfs_unmount(&fs->fs);
        }
        fs->mounted = false;
        int rc = mount_consistent(fs);
        if (rc != 0) {
            /* Keep retrying a failed remount on the next operation. */
            return rc;
        }
        fs->needs_remount = false;
    }
    return 0;
}

static storage_record_result_t record_read(storage_backend_t *backend, uint16_t id,
                                            uint8_t *dst, size_t cap, size_t *len) {
    record_fs_t *fs = backend->ctx;
    if (dst == NULL || len == NULL) {
        return STORAGE_RECORD_ERROR;
    }
    *len = 0;
    int rc = prepare(fs);
    if (rc != 0) {
        return result_for(fs, rc);
    }
    char path[6];
    record_path(id, path);
    lfs_file_t file;
    struct lfs_file_config file_cfg = {.buffer = fs->file_cache};
    rc = lfs_file_opencfg(&fs->fs, &file, path, LFS_O_RDONLY, &file_cfg);
    if (rc != 0) {
        return result_for(fs, rc);
    }
    uint8_t header[RECORD_HEADER];
    lfs_ssize_t size = lfs_file_size(&fs->fs, &file);
    lfs_ssize_t got = lfs_file_read(&fs->fs, &file, header, sizeof(header));
    if (got != sizeof(header) || read_u32(header) != RECORD_MAGIC ||
        read_u16(header + 4) != id || read_u16(header + 6) != 1u ||
        read_u32(header + 8) > STORAGE_RECORD_MAX_PAYLOAD ||
        read_u32(header + 8) > cap ||
        size != (lfs_ssize_t)(RECORD_HEADER + read_u32(header + 8))) {
        rc = LFS_ERR_CORRUPT;
    } else {
        size_t count = read_u32(header + 8);
        got = lfs_file_read(&fs->fs, &file, dst, count);
        if (got != (lfs_ssize_t)count || record_crc(dst, count) != read_u32(header + 12)) {
            rc = LFS_ERR_CORRUPT;
        } else {
            *len = count;
        }
    }
    int close_rc = lfs_file_close(&fs->fs, &file);
    return result_for(fs, rc != 0 ? rc : close_rc);
}

static storage_record_result_t record_write(storage_backend_t *backend, uint16_t id,
                                             const uint8_t *src, size_t len) {
    record_fs_t *fs = backend->ctx;
    if (src == NULL || len > STORAGE_RECORD_MAX_PAYLOAD ||
        (fs == &s_volumes[STORAGE_LFS_USER] && s_object_scan_open)) {
        return STORAGE_RECORD_ERROR;
    }
    int rc = prepare(fs);
    if (rc != 0) {
        return result_for(fs, rc);
    }
    /* The initial import runs before the object allocator exists. Once present,
     * legacy user records and the ID allocator share the OTHER budget. */
    if (fs == &s_volumes[STORAGE_LFS_USER] && fs->fs.block_count == USER_BLOCKS) {
        struct lfs_info info;
        rc = lfs_stat(&fs->fs, "ufff1", &info);
        if (rc == 0 || id == OBJECT_COUNTER_ID) {
            rc = budget_begin(fs, STORAGE_USER_OTHER, false);
        }
        else if (rc == LFS_ERR_NOENT) rc = 0;
        if (rc != 0) return result_for(fs, rc);
    }
    uint8_t header[RECORD_HEADER];
    write_u32(header, RECORD_MAGIC);
    write_u16(header + 4, id);
    write_u16(header + 6, 1u);
    write_u32(header + 8, (uint32_t)len);
    write_u32(header + 12, record_crc(src, len));
    lfs_file_t file;
    struct lfs_file_config file_cfg = {.buffer = fs->file_cache};
    rc = lfs_file_opencfg(&fs->fs, &file, "txn", LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC,
                         &file_cfg);
    if (rc != 0) {
        budget_end();
        return result_for(fs, rc);
    }
    lfs_ssize_t written = lfs_file_write(&fs->fs, &file, header, sizeof(header));
    if (written != sizeof(header)) {
        rc = written < 0 ? (int)written : LFS_ERR_IO;
    } else {
        written = lfs_file_write(&fs->fs, &file, src, len);
        if (written != (lfs_ssize_t)len) {
            rc = written < 0 ? (int)written : LFS_ERR_IO;
        }
    }
    int close_rc = lfs_file_close(&fs->fs, &file);
    if (rc == 0) {
        rc = close_rc;
    }
    if (rc == 0) {
        char path[6];
        record_path(id, path);
        /* Only publish the complete, synced replacement. A reset before rename
         * leaves the old record authoritative, including for multi-block data. */
        rc = lfs_rename(&fs->fs, "txn", path);
    }
    budget_end();
    return result_for(fs, rc);
}

void storage_lfs_deinit(void) {
    s_usage_valid = false;
    s_budget_active = false;
    s_object_scan_open = false;
    s_objects_ready = false;
    for (unsigned i = 0; i < STORAGE_LFS_VOLUME_COUNT; i++) {
        if (s_volumes[i].mounted) {
            lfs_unmount(&s_volumes[i].fs);
        }
    }
    memset(s_volumes, 0, sizeof(s_volumes));
}

storage_record_result_t storage_lfs_init(storage_backend_t *backend, nvm_hal_t *hal) {
    return storage_lfs_init_volume(backend, hal, STORAGE_LFS_USER,
                                   hal != NULL ? hal->capacity : 0u);
}

storage_record_result_t storage_lfs_init_volume(storage_backend_t *backend,
    nvm_hal_t *hal, storage_lfs_volume_t volume, uint32_t initial_bytes) {
    if (backend == NULL || hal == NULL || hal->read == NULL || hal->write == NULL ||
        hal->erase == NULL || !hal->erase_required || hal->erase_block != 4096u ||
        hal->write_block != PROGRAM_SIZE || hal->capacity < 4u * hal->erase_block ||
        hal->capacity % hal->erase_block != 0u || (unsigned)volume >= STORAGE_LFS_VOLUME_COUNT ||
        initial_bytes < 4u * hal->erase_block || initial_bytes > hal->capacity ||
        initial_bytes % hal->erase_block != 0u) {
        return STORAGE_RECORD_ERROR;
    }
    record_fs_t *fs = &s_volumes[volume];
    if (volume == STORAGE_LFS_USER) {
        s_usage_valid = false;
        s_budget_active = false;
        s_object_scan_open = false;
        s_objects_ready = false;
    }
    if (fs->mounted) {
        lfs_unmount(&fs->fs);
    }
    memset(fs, 0, sizeof(*fs));
    fs->hal = hal;
    fs->cfg = (struct lfs_config){
        .context = fs, .read = bd_read, .prog = bd_prog, .erase = bd_erase, .sync = bd_sync,
        .alloc_guard = allocation_guard,
        .read_size = 1u, .prog_size = PROGRAM_SIZE, .block_size = hal->erase_block,
        /* Mount discovers the durable size, including either side of an
         * interrupted growth. Physical I/O is always bounded by the HAL. */
        .block_count = 0u, .block_cycles = 100,
        .cache_size = volume == STORAGE_LFS_USER ? CACHE_SIZE : PROGRAM_SIZE,
        .lookahead_size = LOOKAHEAD_SIZE,
        .read_buffer = fs->read_cache, .prog_buffer = fs->prog_cache,
        .lookahead_buffer = fs->lookahead, .name_max = 16u,
        .file_max = STORAGE_RECORD_MAX_PAYLOAD + RECORD_HEADER,
    };
    int rc = mount_consistent(fs);
    if (rc == LFS_ERR_CORRUPT && !fs->mounted) {
        /* Never turn mount failure into silent data loss. First-use formatting
         * requires proof that the entire initial format extent is erased.
         * The remaining user extent may still contain the legacy journal. */
        for (uint32_t off = 0; off < initial_bytes; off += CACHE_SIZE) {
            if (io_result(fs, hal->read(hal, off, fs->read_cache, CACHE_SIZE)) != 0) {
                return result_for(fs, LFS_ERR_IO);
            }
            for (size_t i = 0; i < CACHE_SIZE; i++) {
                if (fs->read_cache[i] != 0xffu) {
                    return STORAGE_RECORD_ERROR;
                }
            }
        }
        fs->cfg.block_count = initial_bytes / hal->erase_block;
        rc = lfs_format(&fs->fs, &fs->cfg);
        fs->cfg.block_count = 0u;
        if (rc == 0) {
            rc = mount_consistent(fs);
        }
    }
    if (rc != 0) {
        return result_for(fs, rc);
    }
    fs->mounted = true;
    *backend = (storage_backend_t){.ctx = fs, .read = record_read, .write = record_write};
    return STORAGE_RECORD_OK;
}

int32_t storage_lfs_used_blocks(void) {
    record_fs_t *fs = &s_volumes[STORAGE_LFS_USER];
    int rc = prepare(fs);
    return rc == 0 ? lfs_fs_size(&fs->fs) : rc;
}

storage_record_result_t storage_lfs_grow(storage_backend_t *backend) {
    record_fs_t *fs = backend->ctx;
    int rc = prepare(fs);
    if (rc == 0) {
        rc = lfs_fs_grow(&fs->fs, fs->hal->capacity / fs->cfg.block_size);
    }
    return result_for(fs, rc);
}

storage_record_result_t storage_lfs_remove(storage_backend_t *backend, uint16_t id) {
    record_fs_t *fs = backend->ctx;
    if (fs == &s_volumes[STORAGE_LFS_USER] && s_object_scan_open) return STORAGE_RECORD_ERROR;
    int rc = prepare(fs);
    if (rc == 0 && fs == &s_volumes[STORAGE_LFS_USER] && fs->fs.block_count == USER_BLOCKS) {
        struct lfs_info info;
        rc = lfs_stat(&fs->fs, "ufff1", &info);
        if (rc == 0) rc = budget_begin(fs, STORAGE_USER_OTHER, true);
        else if (rc == LFS_ERR_NOENT) rc = 0;
    }
    if (rc == 0) {
        char path[6];
        record_path(id, path);
        rc = lfs_remove(&fs->fs, path);
    }
    budget_end();
    return result_for(fs, rc);
}

int32_t storage_lfs_volume_blocks(storage_backend_t *backend) {
    record_fs_t *fs = backend->ctx;
    return prepare(fs) == 0 ? (int32_t)fs->fs.block_count : -1;
}

int32_t storage_lfs_volume_used(storage_backend_t *backend) {
    record_fs_t *fs = backend->ctx;
    return prepare(fs) == 0 ? lfs_fs_size(&fs->fs) : -1;
}

static int usage_live_block(void *ctx, lfs_block_t block) {
    (void)ctx;
    if (block >= USER_BLOCKS) return LFS_ERR_CORRUPT;
    s_block_owner[block] = STORAGE_USER_OTHER + 1u;
    return 0;
}

static int usage_pool_block(void *ctx, lfs_block_t block) {
    unsigned pool = *(const unsigned *)ctx;
    if (block >= USER_BLOCKS ||
        (s_block_owner[block] != STORAGE_USER_OTHER + 1u &&
         s_block_owner[block] != pool + 1u)) return LFS_ERR_CORRUPT;
    s_block_owner[block] = (uint8_t)(pool + 1u);
    return 0;
}

static bool temporary_name(const char *name) {
    return strcmp(name, ".txn") == 0 || strcmp(name, "txn") == 0 ||
           strcmp(name, "object-txn") == 0;
}

static int legacy_category(const char *name) {
    if (strcmp(name, "u3216") == 0 || strcmp(name, "u3217") == 0 ||
        strcmp(name, "u3218") == 0) return STORAGE_USER_CALLS;
    if (strcmp(name, "u3219") == 0) return STORAGE_USER_DICTIONARY;
    if (strcmp(name, "u321a") == 0) return STORAGE_USER_PICTURES;
    if (strcmp(name, "u321b") == 0) return STORAGE_USER_TONES;
    if (strcmp(name, "u321c") == 0) return STORAGE_USER_DIVERT;
    return -1;
}

static int usage_files(record_fs_t *fs, const char *path, unsigned pool) {
    lfs_dir_t dir;
    int rc = lfs_dir_open(&fs->fs, &dir, path);
    if (rc != 0) return rc;
    struct lfs_info info;
    while ((rc = lfs_dir_read(&fs->fs, &dir, &info)) > 0) {
        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) continue;
        if (info.type == LFS_TYPE_DIR && pool == STORAGE_USER_OTHER) continue;
        if (info.type != LFS_TYPE_REG) {
            rc = LFS_ERR_CORRUPT;
            break;
        }
        if (temporary_name(info.name)) continue;
        s_usage.pools[pool].contents.files++;
        s_usage.pools[pool].contents.file_bytes += info.size;
        int category = pool == STORAGE_USER_OTHER ? legacy_category(info.name) : -1;
        if (category >= 0) {
            s_usage.legacy[category].files++;
            s_usage.legacy[category].file_bytes += info.size;
        }
    }
    int closed = lfs_dir_close(&fs->fs, &dir);
    return rc < 0 ? rc : closed;
}

static int usage_measure(record_fs_t *fs) {
    if (fs->fs.block_count != USER_BLOCKS) return LFS_ERR_INVAL;
    if (s_usage_valid) return 0;
    memset(&s_usage, 0, sizeof(s_usage));
    memset(s_block_owner, 0, sizeof(s_block_owner));
    s_usage.capacity_bytes = STORAGE_USER_BYTES;
    s_usage.recovery_reserve_bytes = RECOVERY_BLOCKS * fs->cfg.block_size;
    int rc = lfs_fs_traverse(&fs->fs, usage_live_block, NULL);
    if (rc != 0) return rc;
    for (unsigned i = 0; i < STORAGE_OBJECT_COLLECTION_COUNT; i++) {
        rc = lfs_dir_blocks(&fs->fs, OBJECT_DIRS[i], usage_pool_block, &i);
        if (rc == LFS_ERR_NOENT) continue;
        if (rc != 0) return rc;
        rc = usage_files(fs, OBJECT_DIRS[i], i);
        if (rc != 0) return rc;
    }
    rc = usage_files(fs, "/", STORAGE_USER_OTHER);
    if (rc != 0) return rc;
    for (unsigned b = 0; b < USER_BLOCKS; b++) {
        if (s_block_owner[b] == 0u) continue;
        s_usage.allocated_bytes += fs->cfg.block_size;
        s_usage.pools[s_block_owner[b] - 1u].allocated_bytes += fs->cfg.block_size;
    }
    for (unsigned i = 0; i < STORAGE_USER_POOL_COUNT; i++) {
        s_usage.pools[i].limit_bytes = USER_LIMIT_BLOCKS[i] * fs->cfg.block_size;
    }
    s_usage_valid = true;
    return 0;
}

static unsigned pool_headroom(unsigned pool) {
    uint32_t used = s_usage.pools[pool].allocated_bytes / 4096u;
    return used < USER_LIMIT_BLOCKS[pool] ? USER_LIMIT_BLOCKS[pool] - used : 0u;
}

static int budget_begin(record_fs_t *fs, storage_user_pool_t pool, bool reclaim) {
    if (s_budget_active) return LFS_ERR_INVAL;
    int rc = usage_measure(fs);
    if (rc != 0) return rc;
    unsigned reserved = RECOVERY_BLOCKS;
    for (unsigned i = 0; i < STORAGE_USER_POOL_COUNT; i++) {
        if (i != (unsigned)pool) reserved += pool_headroom(i);
    }
    unsigned extra = reclaim ? RECLAIM_BLOCKS : 0u;
    reserved -= extra;
    unsigned free = USER_BLOCKS - s_usage.allocated_bytes / fs->cfg.block_size;
    unsigned allowance = free > reserved ? free - reserved : 0u;
    unsigned own = pool_headroom(pool) + extra;
    if (allowance > own) allowance = own;
    /* An object mutation may also relocate/split shared root metadata. Bound
     * the entire allocation peak by both pools, not an estimated split size. */
    if (pool != STORAGE_USER_OTHER) {
        unsigned shared = pool_headroom(STORAGE_USER_OTHER) + extra;
        if (allowance > shared) allowance = shared;
    }
    s_budget_left = allowance;
    memset(s_budget_seen, 0, sizeof(s_budget_seen));
    s_budget_active = true;
    return 0;
}

static void budget_end(void) {
    s_budget_active = false;
}

storage_record_result_t storage_user_get_usage(storage_user_usage_t *out) {
    if (out == NULL || s_budget_active) return STORAGE_RECORD_ERROR;
    memset(out, 0, sizeof(*out));
    record_fs_t *fs = &s_volumes[STORAGE_LFS_USER];
    int rc = prepare(fs);
    if (rc == 0) rc = usage_measure(fs);
    if (rc == 0) *out = s_usage;
    return result_for(fs, rc);
}

static int remove_temporary(record_fs_t *fs, const char *path, storage_user_pool_t pool) {
    struct lfs_info info;
    int rc = lfs_stat(&fs->fs, path, &info);
    if (rc == LFS_ERR_NOENT) return 0;
    if (rc != 0) return rc;
    if (info.type != LFS_TYPE_REG) return LFS_ERR_CORRUPT;
    rc = budget_begin(fs, pool, true);
    if (rc != 0) return rc;
    rc = lfs_remove(&fs->fs, path);
    budget_end();
    return rc;
}

static void object_temporary(unsigned collection, char path[32]) {
    strcpy(path, OBJECT_DIRS[collection]);
    strcat(path, "/.txn");
}

static bool object_path(storage_object_collection_t collection, uint32_t id,
                        char path[32]) {
    if ((unsigned)collection >= STORAGE_OBJECT_COLLECTION_COUNT || id == 0u) {
        return false;
    }
    const char *dir = OBJECT_DIRS[collection];
    size_t n = strlen(dir);
    memcpy(path, dir, n);
    path[n++] = '/';
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 8u; i++) {
        path[n++] = digits[(id >> (28u - 4u * i)) & 15u];
    }
    path[n] = '\0';
    return true;
}

storage_record_result_t storage_objects_open(void) {
    record_fs_t *fs = &s_volumes[STORAGE_LFS_USER];
    int rc = prepare(fs);
    if (rc != 0) return result_for(fs, rc);
    if (fs->fs.block_count != USER_BLOCKS) return STORAGE_RECORD_ERROR;
    if (s_objects_ready) return STORAGE_RECORD_OK;
    storage_backend_t backend = {.ctx = fs};
    uint8_t counter[4];
    size_t len = 0;
    storage_record_result_t result = record_read(&backend, OBJECT_COUNTER_ID,
                                                 counter, sizeof(counter), &len);
    if (result == STORAGE_RECORD_NOT_FOUND) {
        /* Never reseed a lost allocator over existing objects. Directory
         * creation happens only after this counter has committed. */
        struct lfs_info info;
        for (unsigned i = 0; i < STORAGE_OBJECT_COLLECTION_COUNT; i++) {
            rc = lfs_stat(&fs->fs, OBJECT_DIRS[i], &info);
            if (rc != LFS_ERR_NOENT) {
                return rc == 0 ? STORAGE_RECORD_ERROR : result_for(fs, rc);
            }
        }
        write_u32(counter, 1u);
        result = record_write(&backend, OBJECT_COUNTER_ID, counter, sizeof(counter));
        if (result != STORAGE_RECORD_OK) return result;
        len = sizeof(counter);
    }
    if (result != STORAGE_RECORD_OK) return result;
    if (len != sizeof(counter) || read_u32(counter) == 0u) return STORAGE_RECORD_ERROR;
    s_next_object = read_u32(counter);
    for (unsigned i = 0; i < STORAGE_OBJECT_COLLECTION_COUNT; i++) {
        struct lfs_info info;
        rc = lfs_stat(&fs->fs, OBJECT_DIRS[i], &info);
        if (rc == LFS_ERR_NOENT) {
            rc = budget_begin(fs, (storage_user_pool_t)i, false);
            if (rc != 0) return result_for(fs, rc);
            rc = lfs_mkdir(&fs->fs, OBJECT_DIRS[i]);
            budget_end();
        } else if (rc == 0 && info.type != LFS_TYPE_DIR) {
            rc = LFS_ERR_CORRUPT;
        }
        if (rc != 0) return result_for(fs, rc);
        char temporary[32];
        object_temporary(i, temporary);
        rc = remove_temporary(fs, temporary, (storage_user_pool_t)i);
        if (rc != 0) return result_for(fs, rc);
    }
    rc = remove_temporary(fs, "object-txn", STORAGE_USER_OTHER);
    if (rc == 0) rc = remove_temporary(fs, "txn", STORAGE_USER_OTHER);
    if (rc != 0) return result_for(fs, rc);
    s_objects_ready = true;
    return STORAGE_RECORD_OK;
}

storage_record_result_t storage_object_allocate(uint32_t *id) {
    if (id == NULL || s_object_scan_open) return STORAGE_RECORD_ERROR;
    *id = 0u;
    storage_record_result_t result = storage_objects_open();
    if (result != STORAGE_RECORD_OK) return result;
    if (s_next_object == UINT32_MAX) return STORAGE_RECORD_FULL;
    uint32_t allocated = s_next_object;
    uint8_t counter[4];
    write_u32(counter, allocated + 1u);
    storage_backend_t backend = {.ctx = &s_volumes[STORAGE_LFS_USER]};
    result = record_write(&backend, OBJECT_COUNTER_ID, counter, sizeof(counter));
    /* Even a failed rename may have committed. Reload the durable counter
     * before another reservation instead of risking duplicate allocation. */
    s_objects_ready = false;
    if (result == STORAGE_RECORD_OK) *id = allocated;
    return result;
}

storage_record_result_t storage_object_read(storage_object_collection_t collection,
    uint32_t id, uint8_t *dst, size_t cap, size_t *len) {
    char path[32];
    if (dst == NULL || len == NULL || !object_path(collection, id, path)) {
        return STORAGE_RECORD_ERROR;
    }
    *len = 0u;
    storage_record_result_t result = storage_objects_open();
    if (result != STORAGE_RECORD_OK) return result;
    record_fs_t *fs = &s_volumes[STORAGE_LFS_USER];
    lfs_file_t file;
    struct lfs_file_config cfg = {.buffer = fs->file_cache};
    int rc = lfs_file_opencfg(&fs->fs, &file, path, LFS_O_RDONLY, &cfg);
    if (rc != 0) return result_for(fs, rc);
    uint8_t header[OBJECT_HEADER];
    lfs_ssize_t got = lfs_file_read(&fs->fs, &file, header, sizeof(header));
    if (got != sizeof(header) || read_u32(header) != OBJECT_MAGIC ||
        read_u16(header + 4u) != 1u || read_u16(header + 6u) != (unsigned)collection ||
        read_u32(header + 8u) != id || read_u32(header + 12u) > STORAGE_OBJECT_MAX_PAYLOAD ||
        read_u32(header + 12u) > cap ||
        lfs_file_size(&fs->fs, &file) != (lfs_ssize_t)(OBJECT_HEADER + read_u32(header + 12u))) {
        rc = LFS_ERR_CORRUPT;
    } else {
        size_t n = read_u32(header + 12u);
        got = lfs_file_read(&fs->fs, &file, dst, n);
        if (got != (lfs_ssize_t)n || record_crc(dst, n) != read_u32(header + 16u)) {
            rc = LFS_ERR_CORRUPT;
        } else {
            *len = n;
        }
    }
    int closed = lfs_file_close(&fs->fs, &file);
    return result_for(fs, rc != 0 ? rc : closed);
}

static int object_state_read(record_fs_t *fs, const char *path, uint8_t state[4]) {
    lfs_ssize_t size = lfs_getattr(&fs->fs, path, OBJECT_STATE_ATTR, state, 4u);
    if (size == LFS_ERR_NOATTR) {
        memset(state, 0, 4u);
        return 0;
    }
    return size == 4 ? 0 : size < 0 ? (int)size : LFS_ERR_CORRUPT;
}

storage_record_result_t storage_object_get_state(storage_object_collection_t collection,
    uint32_t id, uint32_t *state) {
    char path[32];
    if (state == NULL || !object_path(collection, id, path)) return STORAGE_RECORD_ERROR;
    storage_record_result_t result = storage_objects_open();
    if (result != STORAGE_RECORD_OK) return result;
    record_fs_t *fs = &s_volumes[STORAGE_LFS_USER];
    uint8_t bytes[4];
    int rc = object_state_read(fs, path, bytes);
    if (rc == 0) *state = read_u32(bytes);
    return result_for(fs, rc);
}

storage_record_result_t storage_object_set_state(storage_object_collection_t collection,
    uint32_t id, uint32_t state) {
    char path[32];
    if (s_object_scan_open || !object_path(collection, id, path)) return STORAGE_RECORD_ERROR;
    storage_record_result_t result = storage_objects_open();
    if (result != STORAGE_RECORD_OK) return result;
    record_fs_t *fs = &s_volumes[STORAGE_LFS_USER];
    uint8_t bytes[4];
    int rc = object_state_read(fs, path, bytes);
    if (rc != 0) return result_for(fs, rc);
    if (read_u32(bytes) == state) return STORAGE_RECORD_OK;
    write_u32(bytes, state);
    rc = budget_begin(fs, (storage_user_pool_t)collection, true);
    if (rc == 0) {
        rc = lfs_setattr(&fs->fs, path, OBJECT_STATE_ATTR, bytes, sizeof(bytes));
        budget_end();
    }
    return result_for(fs, rc);
}

storage_record_result_t storage_object_write(storage_object_collection_t collection,
    uint32_t id, const uint8_t *src, size_t len) {
    char path[32];
    if (src == NULL || len > STORAGE_OBJECT_MAX_PAYLOAD || s_object_scan_open ||
        !object_path(collection, id, path)) return STORAGE_RECORD_ERROR;
    storage_record_result_t result = storage_objects_open();
    if (result != STORAGE_RECORD_OK) return result;
    if (id >= s_next_object) return STORAGE_RECORD_ERROR;
    record_fs_t *fs = &s_volumes[STORAGE_LFS_USER];
    uint8_t state[4] = {0};
    int rc = object_state_read(fs, path, state);
    if (rc == LFS_ERR_NOENT) rc = 0;
    if (rc != 0) return result_for(fs, rc);
    rc = budget_begin(fs, (storage_user_pool_t)collection, false);
    if (rc != 0) return result_for(fs, rc);
    char temporary[32];
    object_temporary(collection, temporary);
    uint8_t header[OBJECT_HEADER];
    write_u32(header, OBJECT_MAGIC);
    write_u16(header + 4u, 1u);
    write_u16(header + 6u, (uint16_t)collection);
    write_u32(header + 8u, id);
    write_u32(header + 12u, (uint32_t)len);
    write_u32(header + 16u, record_crc(src, len));
    lfs_file_t file;
    struct lfs_attr attr = {.type = OBJECT_STATE_ATTR, .buffer = state, .size = sizeof(state)};
    struct lfs_file_config cfg = {.buffer = fs->file_cache, .attrs = &attr, .attr_count = 1u};
    rc = lfs_file_opencfg(&fs->fs, &file, temporary,
                             LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &cfg);
    if (rc != 0) {
        budget_end();
        return result_for(fs, rc);
    }
    lfs_ssize_t n = lfs_file_write(&fs->fs, &file, header, sizeof(header));
    if (n == sizeof(header)) {
        n = lfs_file_write(&fs->fs, &file, src, len);
        if (n != (lfs_ssize_t)len) rc = n < 0 ? (int)n : LFS_ERR_IO;
    } else {
        rc = n < 0 ? (int)n : LFS_ERR_IO;
    }
    int closed = lfs_file_close(&fs->fs, &file);
    if (rc == 0) rc = closed;
    if (rc == 0) rc = lfs_rename(&fs->fs, temporary, path);
    budget_end();
    return result_for(fs, rc);
}

storage_record_result_t storage_object_remove(storage_object_collection_t collection,
    uint32_t id) {
    char path[32];
    if (s_object_scan_open || !object_path(collection, id, path)) return STORAGE_RECORD_ERROR;
    storage_record_result_t result = storage_objects_open();
    if (result != STORAGE_RECORD_OK) return result;
    record_fs_t *fs = &s_volumes[STORAGE_LFS_USER];
    int rc = budget_begin(fs, (storage_user_pool_t)collection, true);
    if (rc == 0) {
        rc = lfs_remove(&fs->fs, path);
        budget_end();
    }
    return result_for(fs, rc);
}

void storage_object_scan_end(void) {
    if (s_object_scan_open) {
        lfs_dir_close(&s_volumes[STORAGE_LFS_USER].fs, &s_object_dir);
        s_object_scan_open = false;
    }
}

storage_record_result_t storage_object_scan_begin(storage_object_collection_t collection) {
    if ((unsigned)collection >= STORAGE_OBJECT_COLLECTION_COUNT) return STORAGE_RECORD_ERROR;
    storage_object_scan_end();
    storage_record_result_t result = storage_objects_open();
    if (result != STORAGE_RECORD_OK) return result;
    record_fs_t *fs = &s_volumes[STORAGE_LFS_USER];
    int rc = lfs_dir_open(&fs->fs, &s_object_dir, OBJECT_DIRS[collection]);
    s_object_scan_open = rc == 0;
    return result_for(fs, rc);
}

storage_record_result_t storage_object_scan_next(uint32_t *id) {
    record_fs_t *fs = &s_volumes[STORAGE_LFS_USER];
    if (id == NULL || !s_object_scan_open || fs->needs_remount) return STORAGE_RECORD_ERROR;
    struct lfs_info info;
    int rc;
    while ((rc = lfs_dir_read(&fs->fs, &s_object_dir, &info)) > 0) {
        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0 ||
            strcmp(info.name, ".txn") == 0) continue;
        if (info.type != LFS_TYPE_REG || strlen(info.name) != 8u) return STORAGE_RECORD_ERROR;
        uint32_t value = 0u;
        for (unsigned i = 0; i < 8u; i++) {
            char c = info.name[i];
            unsigned digit;
            if (c >= '0' && c <= '9') digit = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') digit = (unsigned)(c - 'a') + 10u;
            else return STORAGE_RECORD_ERROR;
            value = value * 16u + digit;
        }
        if (value == 0u || value >= s_next_object) return STORAGE_RECORD_ERROR;
        *id = value;
        return STORAGE_RECORD_OK;
    }
    return rc < 0 ? result_for(fs, rc) : STORAGE_RECORD_NOT_FOUND;
}
