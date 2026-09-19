#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "lfs.h"
#include "storage/storage_layout.h"
#include "storage/storage_lfs.h"

static uint8_t media[STORAGE_USER_BYTES], saved[sizeof(media)];
static uint8_t rcache[512], pcache[512], fcache[512], lookahead[16];
static uint8_t data[200], readback[sizeof(data)];
static lfs_t fs;
static uint32_t physical_blocks;
static unsigned operations, cut_at, tear, cases;
static bool fail_io;
static jmp_buf power_cut;

static bool in_range(lfs_block_t block, lfs_off_t off, lfs_size_t len) {
    return block < physical_blocks && off <= 4096u && len <= 4096u - off;
}

static int read_block(const struct lfs_config *cfg, lfs_block_t block,
                      lfs_off_t off, void *dst, lfs_size_t len) {
    (void)cfg;
    if (!in_range(block, off, len)) return LFS_ERR_IO;
    memcpy(dst, media + block * 4096u + off, len);
    return 0;
}

static int change(uint32_t off, const uint8_t *src, size_t len) {
    assert(off + len <= physical_blocks * 4096u);
    bool interrupted = ++operations == cut_at;
    size_t done = interrupted ? (tear == 0 ? 0 : tear == 1 ? len / 2 : len) : len;
    for (size_t i = 0; i < done; i++) {
        if (src == NULL) media[off + i] = 0xff;
        else {
            assert((media[off + i] & src[i]) == src[i]);
            media[off + i] &= src[i];
        }
    }
    if (interrupted) {
        if (fail_io) return LFS_ERR_IO;
        longjmp(power_cut, 1);
    }
    return 0;
}

static int prog_block(const struct lfs_config *cfg, lfs_block_t block,
                      lfs_off_t off, const void *src, lfs_size_t len) {
    (void)cfg;
    assert(in_range(block, off, len) && off % 256u == 0 && len % 256u == 0);
    for (lfs_size_t i = 0; i < len; i += 256u) {
        int rc = change(block * 4096u + off + i, (const uint8_t *)src + i, 256u);
        if (rc != 0) return rc;
    }
    return 0;
}

static int erase_block(const struct lfs_config *cfg, lfs_block_t block) {
    (void)cfg;
    assert(in_range(block, 0, 4096u));
    return change(block * 4096u, NULL, 4096u);
}

static int sync_blocks(const struct lfs_config *cfg) { (void)cfg; return 0; }

static struct lfs_config cfg = {
    .read = read_block, .prog = prog_block, .erase = erase_block, .sync = sync_blocks,
    .read_size = 1, .prog_size = 256, .block_size = 4096, .block_cycles = 1,
    .cache_size = 512, .lookahead_size = sizeof(lookahead), .name_max = 16,
    .file_max = STORAGE_RECORD_MAX_PAYLOAD + 16u,
    .read_buffer = rcache, .prog_buffer = pcache, .lookahead_buffer = lookahead,
};
static const struct lfs_file_config file_cfg = {.buffer = fcache};

static int write_file(const char *name, uint8_t value) {
    lfs_file_t file;
    int rc = lfs_file_opencfg(&fs, &file, name,
                             LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &file_cfg);
    if (rc != 0) return rc;
    memset(data, value, sizeof(data));
    lfs_ssize_t n = lfs_file_write(&fs, &file, data, sizeof(data));
    int closed = lfs_file_close(&fs, &file);
    return n == sizeof(data) ? closed : (n < 0 ? (int)n : LFS_ERR_IO);
}

static uint8_t read_file(const char *name) {
    lfs_file_t file;
    assert(lfs_file_opencfg(&fs, &file, name, LFS_O_RDONLY, &file_cfg) == 0);
    assert(lfs_file_read(&fs, &file, readback, sizeof(readback)) == sizeof(readback));
    assert(lfs_file_close(&fs, &file) == 0);
    for (size_t i = 1; i < sizeof(readback); i++) assert(readback[i] == readback[0]);
    return readback[0];
}

static void mount_ok(void) {
    int rc = lfs_mount(&fs, &cfg);
    if (rc != 0) fprintf(stderr, "growth remount failed: rc=%d physical=%u\n", rc, physical_blocks);
    assert(rc == 0);
    assert(lfs_fs_mkconsistent(&fs) == 0);
}

static void seed(unsigned blocks) {
    memset(media, 0xff, sizeof(media));
    physical_blocks = sizeof(media) / 4096u;
    cfg.block_count = blocks;
    assert(lfs_format(&fs, &cfg) == 0);
    cfg.block_count = 0;
    mount_ok();
    assert(write_file("kept", 0x37) == 0);
    /* Frequent root compaction creates a stale superblock in the tail chain. */
    for (unsigned i = 0; i < 100; i++) assert(write_file("state", 0x42) == 0);
    assert(fs.root[0] > 1u || fs.root[1] > 1u);
    assert(lfs_unmount(&fs) == 0);
    memcpy(saved, media, sizeof(media));
}

static void test_growth_cuts(unsigned old_blocks, unsigned new_blocks) {
    seed(old_blocks);
    mount_ok();
    operations = 0;
    assert(lfs_fs_grow(&fs, new_blocks) == 0);
    unsigned boundaries = operations;
    assert(boundaries > 0);
    assert(lfs_unmount(&fs) == 0);
    for (unsigned error = 0; error < 2; error++) {
        for (unsigned point = 1; point <= boundaries; point++) {
            for (unsigned mode = 0; mode < 3; mode++) {
                memcpy(media, saved, sizeof(media));
                mount_ok();
                operations = 0;
                cut_at = point;
                tear = mode;
                fail_io = error != 0;
                if (setjmp(power_cut) == 0) {
                    int rc = lfs_fs_grow(&fs, new_blocks);
                    assert(fail_io && rc == LFS_ERR_IO);
                }
                cut_at = 0;
                fail_io = false;
                assert(lfs_unmount(&fs) == 0);
                mount_ok();
                assert(fs.block_count == old_blocks || fs.block_count == new_blocks);
                assert(read_file("kept") == 0x37 && read_file("state") == 0x42);
                assert(lfs_fs_grow(&fs, new_blocks) == 0);
                assert(lfs_unmount(&fs) == 0);
                mount_ok();
                assert(fs.block_count == new_blocks);
                assert(read_file("kept") == 0x37 && read_file("state") == 0x42);
                assert(lfs_unmount(&fs) == 0);
                cases++;
            }
        }
    }
    printf("growth %u -> %u blocks: %u boundaries x 3 tears x 2 failure modes passed\n",
           old_blocks, new_blocks, boundaries);
}

static void test_root_update_cuts(unsigned boundaries, uint8_t old_value,
                                  uint8_t new_value) {
    assert(lfs_unmount(&fs) == 0);
    for (unsigned error = 0; error < 2; error++) {
        for (unsigned point = 1; point <= boundaries; point++) {
            for (unsigned mode = 0; mode < 3; mode++) {
                memcpy(media, saved, sizeof(media));
                mount_ok();
                operations = 0;
                cut_at = point;
                tear = mode;
                fail_io = error != 0;
                if (setjmp(power_cut) == 0) {
                    int rc = write_file("state", new_value);
                    assert(fail_io && rc == LFS_ERR_IO);
                }
                cut_at = 0;
                fail_io = false;
                assert(lfs_unmount(&fs) == 0);
                mount_ok();
                uint8_t value = read_file("state");
                assert(value == old_value || value == new_value);
                assert(read_file("kept") == 0x37);
                assert(write_file("state", new_value) == 0);
                assert(lfs_unmount(&fs) == 0);
                cases++;
            }
        }
    }
    mount_ok();
    assert(read_file("state") == new_value);
    printf("root relocation: %u boundaries x 3 tears x 2 failure modes passed\n", boundaries);
}

static void test_relocated_root(unsigned old_blocks, unsigned new_blocks) {
    seed(old_blocks);
    mount_ok();
    assert(lfs_fs_grow(&fs, new_blocks) == 0);
    bool reached_new_extent = false;
    for (unsigned i = 0; i < 600; i++) {
        if (!reached_new_extent) memcpy(saved, media, sizeof(media));
        unsigned before = operations;
        assert(write_file("state", (uint8_t)i) == 0);
        if (!reached_new_extent && (fs.root[0] >= old_blocks || fs.root[1] >= old_blocks)) {
            test_root_update_cuts(operations - before,
                                  i == 0 ? 0x42 : (uint8_t)(i - 1u), (uint8_t)i);
            reached_new_extent = true;
        }
        reached_new_extent |= fs.root[0] >= old_blocks || fs.root[1] >= old_blocks;
        assert(lfs_unmount(&fs) == 0);
        mount_ok();
        assert(fs.block_count == new_blocks);
        assert(read_file("kept") == 0x37 && read_file("state") == (uint8_t)i);
    }
    assert(reached_new_extent);
    assert(lfs_unmount(&fs) == 0);
    /* An explicit final geometry must also ignore stale superblock sizes. */
    cfg.block_count = new_blocks;
    mount_ok();
    assert(lfs_unmount(&fs) == 0);
    cfg.block_count = new_blocks + 1u;
    unsigned before = operations;
    assert(lfs_mount(&fs, &cfg) == LFS_ERR_INVAL);
    assert(before == operations);
    cfg.block_count = 0;
    printf("grown-root relocation %u -> %u blocks: 600 write/remount/readback cycles passed\n",
           old_blocks, new_blocks);
}

static nvm_status_t adapter_read(nvm_hal_t *hal, uint32_t off, void *dst, size_t len) {
    assert(off <= hal->capacity && len <= hal->capacity - off);
    memcpy(dst, media + off, len);
    return NVM_STATUS_OK;
}
static nvm_status_t forbidden_write(nvm_hal_t *h, uint32_t off, const void *src, size_t len) {
    (void)h; (void)off; (void)src; (void)len;
    assert(!"invalid mount must not write or format");
    return NVM_STATUS_IO_ERROR;
}
static nvm_status_t forbidden_erase(nvm_hal_t *h, uint32_t off, size_t len) {
    return forbidden_write(h, off, NULL, len);
}

static void test_adapter_rejects_oversized_image(void) {
    nvm_hal_t hal = {.capacity = 32u * 4096u, .erase_block = 4096, .write_block = 256,
        .erase_required = true, .read = adapter_read,
        .write = forbidden_write, .erase = forbidden_erase};
    storage_backend_t backend;
    assert(storage_lfs_init(&backend, &hal) == STORAGE_RECORD_ERROR);
    storage_lfs_deinit();
    /* Also reject a physically readable root whose advertised size is too
     * large, before consistency recovery can write to the smaller partition. */
    memset(media, 0xff, sizeof(media));
    cfg.block_count = physical_blocks;
    assert(lfs_format(&fs, &cfg) == 0);
    cfg.block_count = 0;
    assert(storage_lfs_init(&backend, &hal) == STORAGE_RECORD_ERROR);
    storage_lfs_deinit();
    puts("oversized durable image rejected without writes or out-of-partition reads");
}

int main(void) {
    test_growth_cuts(32, 64);
    test_growth_cuts(32, 96);
    test_growth_cuts(64, 96);
    test_relocated_root(32, 64);
    test_relocated_root(32, 96);
    test_relocated_root(64, 96);
    test_adapter_rejects_oversized_image();
    printf("filesystem growth recovery: %u injected failures passed\n", cases);
    return 0;
}
