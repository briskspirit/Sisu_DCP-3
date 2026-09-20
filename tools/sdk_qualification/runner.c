#include <stdio.h>
#include <string.h>

#include "hal/board_safe_gpio.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "pico/multicore.h"
#include "pico/stdio/driver.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

static char output[128u * 1024u];
static size_t output_size;
static bool output_overflow;
static critical_section_t output_lock;

static void safe_init(uint8_t pin) { gpio_init(pin); }
static void safe_put(uint8_t pin, bool level) { gpio_put(pin, level); }
static void safe_dir(uint8_t pin, bool output_enabled) { gpio_set_dir(pin, output_enabled); }
static void safe_pulls(uint8_t pin) { gpio_disable_pulls(pin); }
static void safe_input(uint8_t pin, bool enabled) { gpio_set_input_enabled(pin, enabled); }

static void capture(const char *text, int length) {
    if (length <= 0) return;
    critical_section_enter_blocking(&output_lock);
    size_t available = sizeof(output) - output_size;
    size_t count = (size_t)length;
    if (count > available) {
        count = available;
        output_overflow = true;
    }
    memcpy(output + output_size, text, count);
    output_size += count;
    watchdog_update();
    critical_section_exit(&output_lock);
}

static stdio_driver_t capture_driver = {.out_chars = capture};

/* The unmodified upstream tests initialize/deinitialize stdio themselves.
 * Keep USB entirely off until their timing-sensitive sections have finished. */
bool __wrap_stdio_init_all(void) { return true; }
void __wrap_stdio_deinit_all(void) {}
bool __real_stdio_init_all(void);
int __real_main(void);

int __wrap_main(void) {
    const board_safe_gpio_ops_t gpio_ops = {
        safe_init, safe_put, safe_dir, safe_pulls, safe_input,
    };
    board_safe_gpio_apply(&gpio_ops);
    set_sys_clock_pll(1536000000u, 5u, 2u);

    int result = -1;
    uint64_t elapsed = 0;
    bool timed_out = watchdog_enable_caused_reboot();
    watchdog_disable();
    if (!timed_out) {
        critical_section_init(&output_lock);
        stdio_set_driver_enabled(&capture_driver, true);
        watchdog_enable(8000u, true);
        uint64_t start = time_us_64();
        result = __real_main();
        elapsed = time_us_64() - start;
        watchdog_disable();
        multicore_reset_core1();
        stdio_set_driver_enabled(&capture_driver, false);
        critical_section_deinit(&output_lock);
    }

    __real_stdio_init_all();
    bool reported = false;
    while (true) {
        bool connected = stdio_usb_connected();
        int ch = getchar_timeout_us(0);
        if (connected && (!reported || ch == 'r')) {
            printf("[sdk-test] %s sdk=%s sys=%lu elapsed_us=%llu timeout=%u overflow=%u\n",
                   SDK_TEST_NAME, PICO_SDK_VERSION_STRING,
                   (unsigned long)clock_get_hz(clk_sys),
                   (unsigned long long)elapsed, timed_out, output_overflow);
            for (size_t i = 0; i < output_size; i++) putchar_raw(output[i]);
            printf("[sdk-test] END result=%d\n", output_overflow ? -2 : result);
            stdio_flush();
            reported = true;
        }
        if (!connected) reported = false;
        sleep_ms(10);
    }
}
