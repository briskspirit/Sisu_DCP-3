#ifndef STATUS_CHROME_H
#define STATUS_CHROME_H

#include <stdint.h>

#include "ui/framebuffer.h"

void draw_status(framebuffer_t *fb, uint8_t bars);
void draw_battery(framebuffer_t *fb, uint8_t level);
/* Draw bitmap 31 immediately to the left of right_edge, clamped to the status
 * content area. Shared by standby and the powered-off alarm screen. */
void draw_alarm_indicator(framebuffer_t *fb, int right_edge);
void status_chrome_set_battery(uint8_t level);

uint8_t status_bars_from_rssi(uint8_t rssi);

#endif
