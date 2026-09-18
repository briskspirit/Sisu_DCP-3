#include "storage/storage_lfs.h"

#include <string.h>

#include "lfs.h"
#include "storage_bytes.h"

#define RECORD_MAGIC 0x31525353u
#define RECORD_HEADER 16u
#define CACHE_SIZE 256u
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

static record_fs_t s_fs;

static int io_result(record_fs_t *fs, nvm_status_t result) {
    if (result == NVM_STATUS_BUSY) {
        fs->io_busy = true;
    }
    return result == NVM_STATUS_OK ? 0 : LFS_ERR_IO;
}

static bool block_range(const struct lfs_config *cfg, lfs_block_t block,
                        lfs_off_t off, lfs_size_t size) {
    return block < cfg->block_count && off <= cfg->block_size &&
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
    if (!block_range(cfg, block, off, size)) {
        return LFS_ERR_IO;
    }
    return io_result(fs, fs->hal->write(fs->hal, block * cfg->block_size + off,
                                       src, size));
}

static int bd_erase(const struct lfs_config *cfg, lfs_block_t block) {
    record_fs_t *fs = cfg->context;
    if (block >= cfg->block_count) {
        return LFS_ERR_IO;
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
    return fs->io_busy ? STORAGE_RECORD_BUSY : STORAGE_RECORD_ERROR;
}

static int prepare(record_fs_t *fs) {
    fs->io_busy = false;
    if (!fs->mounted && !fs->needs_remount) {
        return LFS_ERR_IO;
    }
    if (fs->needs_remount) {
        if (fs->mounted) {
            lfs_unmount(&fs->fs);
        }
        fs->mounted = false;
        int rc = lfs_mount(&fs->fs, &fs->cfg);
        if (rc != 0) {
            /* Keep retrying a failed remount on the next operation. */
            return rc;
        }
        fs->mounted = true;
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
    if (src == NULL || len > STORAGE_RECORD_MAX_PAYLOAD) {
        return STORAGE_RECORD_ERROR;
    }
    int rc = prepare(fs);
    if (rc != 0) {
        return result_for(fs, rc);
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
    return result_for(fs, rc);
}

void storage_lfs_deinit(void) {
    if (s_fs.mounted) {
        lfs_unmount(&s_fs.fs);
    }
    memset(&s_fs, 0, sizeof(s_fs));
}

storage_record_result_t storage_lfs_init(storage_backend_t *backend, nvm_hal_t *hal) {
    if (backend == NULL || hal == NULL || hal->read == NULL || hal->write == NULL ||
        hal->erase == NULL || !hal->erase_required || hal->erase_block != 4096u ||
        hal->write_block != CACHE_SIZE || hal->capacity < 4u * hal->erase_block ||
        hal->capacity % hal->erase_block != 0u) {
        return STORAGE_RECORD_ERROR;
    }
    storage_lfs_deinit();
    record_fs_t *fs = &s_fs;
    fs->hal = hal;
    fs->cfg = (struct lfs_config){
        .context = fs, .read = bd_read, .prog = bd_prog, .erase = bd_erase, .sync = bd_sync,
        .read_size = 1u, .prog_size = CACHE_SIZE, .block_size = hal->erase_block,
        .block_count = hal->capacity / hal->erase_block, .block_cycles = 100,
        .cache_size = CACHE_SIZE, .lookahead_size = LOOKAHEAD_SIZE,
        .read_buffer = fs->read_cache, .prog_buffer = fs->prog_cache,
        .lookahead_buffer = fs->lookahead, .name_max = 16u,
        .file_max = STORAGE_RECORD_MAX_PAYLOAD + RECORD_HEADER,
    };
    int rc = lfs_mount(&fs->fs, &fs->cfg);
    if (rc == LFS_ERR_CORRUPT) {
        /* Never turn mount failure into silent data loss. First-use formatting
         * requires proof that the ENTIRE dedicated region is erased. */
        for (uint32_t off = 0; off < hal->capacity; off += CACHE_SIZE) {
            if (io_result(fs, hal->read(hal, off, fs->read_cache, CACHE_SIZE)) != 0) {
                return result_for(fs, LFS_ERR_IO);
            }
            for (size_t i = 0; i < CACHE_SIZE; i++) {
                if (fs->read_cache[i] != 0xffu) {
                    return STORAGE_RECORD_ERROR;
                }
            }
        }
        rc = lfs_format(&fs->fs, &fs->cfg);
        if (rc == 0) {
            rc = lfs_mount(&fs->fs, &fs->cfg);
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
    int rc = prepare(&s_fs);
    return rc == 0 ? lfs_fs_size(&s_fs.fs) : rc;
}
