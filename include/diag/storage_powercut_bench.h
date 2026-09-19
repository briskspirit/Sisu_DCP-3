#ifndef STORAGE_POWERCUT_BENCH_H
#define STORAGE_POWERCUT_BENCH_H

#include "hal/lcd_pcd8544.h"
#include "ui/framebuffer.h"

_Noreturn void storage_powercut_bench_run(lcd_pcd8544_t *lcd, framebuffer_t *fb);

#endif
