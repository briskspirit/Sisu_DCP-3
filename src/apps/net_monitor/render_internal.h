#ifndef APPS_NET_MONITOR_RENDER_INTERNAL_H
#define APPS_NET_MONITOR_RENDER_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "apps/net_monitor_render.h"

typedef enum {
    NETMON_RENDER_OWNER_NONE = 0,
    NETMON_RENDER_OWNER_RADIO,
    NETMON_RENDER_OWNER_MODEM,
    NETMON_RENDER_OWNER_TELEPHONY,
    NETMON_RENDER_OWNER_LOCAL,
    NETMON_RENDER_OWNER_CONTROL,
} netmon_render_owner_t;

netmon_render_owner_t netmon_render_owner_for_page(
    const netmon_page_descriptor_t *page);

void netmon_render_linef(netmon_frame_t *frame, uint8_t row,
                         const char *format, ...);
void netmon_render_copy_bounded(char *dst, size_t cap, const char *src);
void netmon_render_text_chunks(netmon_frame_t *frame, const char *text,
                               uint8_t first_row);
void netmon_render_text_chunks_mark_truncated(netmon_frame_t *frame,
                                              const char *text,
                                              uint8_t first_row,
                                              bool source_truncated);
const char *netmon_render_yes_no(bool value);
const char *netmon_render_on_off(bool value);
const char *netmon_render_rat_text(modem_diag_rat_t rat);
void netmon_render_half_db(char *dst, size_t cap, const char *label,
                           int16_t value_x2);
void netmon_render_charge_delta(char *dst, size_t cap, int64_t delta_nah);
void netmon_render_tenth(char *dst, size_t cap, const char *label,
                         int32_t value_x10);
void netmon_render_current(char *dst, size_t cap, int32_t current_ua);

void netmon_render_radio(uint8_t id, uint8_t frame_index,
                         const modem_diag_snapshot_t *modem,
                         netmon_frame_t *out);
void netmon_render_modem(uint8_t id, uint8_t frame_index,
                         const netmon_local_diag_snapshot_t *local,
                         const modem_diag_snapshot_t *modem,
                         netmon_frame_t *out);
void netmon_render_telephony(uint8_t id, uint8_t frame_index,
                             const modem_diag_snapshot_t *modem,
                             netmon_frame_t *out);
void netmon_render_local(uint8_t id, uint8_t frame_index,
                         const netmon_local_diag_snapshot_t *local,
                         netmon_frame_t *out);
void netmon_render_control(uint8_t id, uint8_t frame_index,
                           const netmon_control_snapshot_t *control,
                           netmon_frame_t *out);

#endif
