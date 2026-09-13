#include <stdio.h>
#include <string.h>

static unsigned s_failures;
static const char *s_last_failure;

static void record_sync_failure(const char *reason)
{
    s_failures++;
    s_last_failure = reason;
}

#define PICO_SYNC_STUB_FAILURE(reason) record_sync_failure(reason)
#include "pico/sync.h"

void critical_section_stub_peer_enter(critical_section_t *lock);
void critical_section_stub_peer_exit(critical_section_t *lock);

static int s_checks;

#define CHECK(cond) do { \
    s_checks++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #cond); \
        return 1; \
    } \
} while (0)

static void reset_failure(void)
{
    s_failures = 0u;
    s_last_failure = NULL;
}

static int expect_failure_contains(const char *needle)
{
    CHECK(s_failures == 1u);
    CHECK(s_last_failure != NULL);
    CHECK(strstr(s_last_failure, needle) != NULL);
    return 0;
}

int main(void)
{
    critical_section_t lock = {0};
    critical_section_t uninitialized = {0};

    critical_section_init(&lock);
    critical_section_stub_peer_enter(&lock);
    CHECK(lock.depth == 1u);

    reset_failure();
    critical_section_enter_blocking(&lock);
    if (expect_failure_contains("recursive") != 0) return 1;
    CHECK(lock.depth == 1u);

    critical_section_stub_peer_exit(&lock);
    CHECK(lock.depth == 0u);

    reset_failure();
    critical_section_exit(&lock);
    if (expect_failure_contains("not entered") != 0) return 1;

    reset_failure();
    critical_section_enter_blocking(&uninitialized);
    if (expect_failure_contains("not initialized") != 0) return 1;

    critical_section_enter_blocking(&lock);
    reset_failure();
    critical_section_deinit(&lock);
    if (expect_failure_contains("still entered") != 0) return 1;
    CHECK(lock.depth == 1u);
    critical_section_exit(&lock);

    critical_section_deinit(&lock);
    CHECK(lock.magic == 0u);
    reset_failure();
    critical_section_exit(&lock);
    if (expect_failure_contains("not initialized") != 0) return 1;

    printf("critical-section stub tests passed (%d checks)\n", s_checks);
    return 0;
}
