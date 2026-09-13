#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hal/service_vbus_logic.h"

static int s_failures;

static void assert_true(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static void test_attach_and_detach_debounce(void) {
    service_vbus_filter_t f;
    bool changed = false;
    service_vbus_filter_init(&f, false);

    assert_true(!service_vbus_filter_update(&f, true, 100u, &changed) &&
                    !changed,
                "attach candidate is not published immediately");
    assert_true(!service_vbus_filter_update(
                    &f, true, 100u + SERVICE_VBUS_ATTACH_DEBOUNCE_MS - 1u,
                    &changed),
                "attach waits through full debounce");
    assert_true(service_vbus_filter_update(
                    &f, true, 100u + SERVICE_VBUS_ATTACH_DEBOUNCE_MS,
                    &changed) &&
                    changed,
                "stable attach publishes one edge");

    assert_true(service_vbus_filter_update(&f, false, 200u, &changed) &&
                    !changed,
                "detach candidate is not published immediately");
    assert_true(!service_vbus_filter_update(
                    &f, false, 200u + SERVICE_VBUS_DETACH_DEBOUNCE_MS,
                    &changed) &&
                    changed,
                "stable detach publishes one edge");
}

static void test_chatter_restarts_candidate(void) {
    service_vbus_filter_t f;
    bool changed = false;
    service_vbus_filter_init(&f, false);

    (void)service_vbus_filter_update(&f, true, 10u, &changed);
    (void)service_vbus_filter_update(&f, false, 15u, &changed);
    assert_true(!service_vbus_filter_update(&f, true, 20u, &changed),
                "second attach starts a fresh candidate");
    assert_true(!service_vbus_filter_update(
                    &f, true, 20u + SERVICE_VBUS_ATTACH_DEBOUNCE_MS - 1u,
                    &changed),
                "chatter cannot borrow time from first candidate");
}

static void test_wrap_safe_deadline(void) {
    service_vbus_filter_t f;
    bool changed = false;
    service_vbus_filter_init(&f, false);
    uint32_t start = UINT32_MAX - 7u;
    (void)service_vbus_filter_update(&f, true, start, &changed);
    assert_true(service_vbus_filter_update(
                    &f, true, start + SERVICE_VBUS_ATTACH_DEBOUNCE_MS,
                    &changed) &&
                    changed,
                "attach debounce is safe across uint32 wrap");
}

int main(void) {
    test_attach_and_detach_debounce();
    test_chatter_restarts_candidate();
    test_wrap_safe_deadline();
    if (s_failures != 0) {
        fprintf(stderr, "%d failures\n", s_failures);
        return 1;
    }
    printf("service_vbus_logic tests passed\n");
    return 0;
}
