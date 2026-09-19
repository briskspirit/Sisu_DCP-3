#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "storage/nvm_hal.h"
#include "storage/storage_layout.h"

uint8_t test_xip_flash[2u * 1024u * 1024u];
static bool parked, irq_off, watchdog, refuse_park;
static unsigned programs, erases, begins, ends;

bool core1_flash_pause_begin(void) {
    assert(!parked && !irq_off && !watchdog);
    if (refuse_park) {
        return false;
    }
    parked = true;
    begins++;
    return true;
}
void core1_flash_pause_end(void) {
    assert(parked && !irq_off && watchdog);
    parked = false;
    ends++;
}
void runtime_watchdog_flash_begin(void) {
    assert(parked && !irq_off && !watchdog);
    watchdog = true;
}
void runtime_watchdog_flash_checkpoint(void) { assert(parked && !irq_off && watchdog); }
void runtime_watchdog_flash_end(void) { assert(!parked && !irq_off && watchdog); watchdog = false; }
uint32_t save_and_disable_interrupts(void) {
    assert(parked && watchdog && !irq_off);
    irq_off = true;
    return 42;
}
void restore_interrupts(uint32_t state) {
    assert(parked && watchdog && irq_off && state == 42);
    irq_off = false;
}
void watchdog_reboot(uint32_t pc, uint32_t sp, uint32_t delay) {
    (void)pc; (void)sp; (void)delay;
    assert(!"unexpected reboot");
}
void flash_range_erase(uint32_t off, size_t len) {
    assert(parked && irq_off && watchdog);
    assert(off >= sizeof(test_xip_flash) - STORAGE_RESERVED_BYTES);
    assert(off % 4096 == 0 && len == 4096 && off + len <= sizeof(test_xip_flash));
    memset(test_xip_flash + off, 0xff, len);
    erases++;
}
void flash_range_program(uint32_t off, const uint8_t *src, size_t len) {
    assert(parked && irq_off && watchdog);
    assert(off >= sizeof(test_xip_flash) - STORAGE_RESERVED_BYTES);
    assert(off % 256 == 0 && len == 256 && off + len <= sizeof(test_xip_flash));
    assert((uintptr_t)src < (uintptr_t)test_xip_flash ||
           (uintptr_t)src >= (uintptr_t)test_xip_flash + sizeof(test_xip_flash));
    for (size_t i = 0; i < len; i++) {
        test_xip_flash[off + i] &= src[i];
    }
    programs++;
}

int main(void) {
    nvm_hal_t legacy, records, system, user;
    assert(nvm_flash_hal_init(&legacy) == NVM_STATUS_OK);
    assert(nvm_record_flash_hal_init(&records) == NVM_STATUS_OK);
    assert(nvm_system_flash_hal_init(&system) == NVM_STATUS_OK);
    assert(nvm_user_flash_hal_init(&user) == NVM_STATUS_OK);
    assert(legacy.capacity == 128u * 1024u && records.capacity == legacy.capacity);
    assert(system.capacity == 64u * 1024u && user.capacity == 256u * 1024u);
    memset(test_xip_flash, 0x5a, sizeof(test_xip_flash));
    assert(records.erase(&records, 0, 8192) == NVM_STATUS_OK);
    assert(records.write(&records, 0, test_xip_flash, 512) == NVM_STATUS_OK);
    uint8_t out[512];
    assert(records.read(&records, 0, out, sizeof(out)) == NVM_STATUS_OK);
    assert(memcmp(out, test_xip_flash, sizeof(out)) == 0);
    assert(legacy.read(&legacy, 0, out, sizeof(out)) == NVM_STATUS_OK);
    assert(out[0] == 0x5a); /* adjacent legacy partition was not erased */
    assert(erases == 2 && programs == 2 && begins == 4 && ends == 4);
    assert(records.erase(&records, records.capacity, 4096) == NVM_STATUS_OUT_OF_RANGE);
    assert(records.write(&records, UINT32_MAX, out, 256) == NVM_STATUS_OUT_OF_RANGE);
    assert(records.erase(&records, 1, 4096) == NVM_STATUS_ALIGNMENT);
    assert(records.write(&records, 0, out, 255) == NVM_STATUS_ALIGNMENT);
    refuse_park = true;
    assert(records.erase(&records, 0, 4096) == NVM_STATUS_BUSY);
    assert(records.write(&records, 0, out, 256) == NVM_STATUS_BUSY);
    assert(erases == 2 && programs == 2 && begins == ends);
    refuse_park = false;
    const size_t system_start = sizeof(test_xip_flash) - STORAGE_RESERVED_BYTES;
    const size_t user_start = sizeof(test_xip_flash) - STORAGE_USER_BYTES;
    assert(system.erase(&system, 0, 4096) == NVM_STATUS_OK);
    assert(test_xip_flash[system_start - 1u] == 0x5a);
    assert(test_xip_flash[system_start] == 0xff);
    assert(system.erase(&system, system.capacity - 4096u, 4096) == NVM_STATUS_OK);
    assert(test_xip_flash[user_start - 1u] == 0xff);
    assert(test_xip_flash[user_start] == 0x5a);
    memset(out, 0x31, sizeof(out));
    assert(system.write(&system, system.capacity - 256u, out, 256) == NVM_STATUS_OK);
    assert(user.erase(&user, 0, 4096) == NVM_STATUS_OK);
    assert(test_xip_flash[user_start - 1u] == 0x31);
    assert(user.erase(&user, user.capacity - 4096u, 4096) == NVM_STATUS_OK);
    assert(user.write(&user, user.capacity - 256u, out, 256) == NVM_STATUS_OK);
    assert(test_xip_flash[sizeof(test_xip_flash) - 1u] == 0x31);
    assert(system.erase(&system, system.capacity, 4096) == NVM_STATUS_OUT_OF_RANGE);
    assert(system.read(&system, system.capacity - 1u, out, 2) == NVM_STATUS_OUT_OF_RANGE);
    assert(user.write(&user, user.capacity, out, 256) == NVM_STATUS_OUT_OF_RANGE);
    assert(user.read(&user, UINT32_MAX, out, 2) == NVM_STATUS_OUT_OF_RANGE);
    assert(begins == ends && !parked && !irq_off && !watchdog);
    puts("nvm flash bounds, RAM staging, park/IRQ/watchdog ordering passed");
    return 0;
}
