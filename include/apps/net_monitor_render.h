#ifndef APPS_NET_MONITOR_RENDER_H
#define APPS_NET_MONITOR_RENDER_H

#include <stdint.h>

#include "apps/net_monitor_logic.h"
#include "services/modem_diag.h"
#include "services/netmon_control_service.h"
#include "services/netmon_diag_service.h"

#define NETMON_FRAME_LINE_COUNT 4u
#define NETMON_FRAME_VISIBLE_CHARS 12u
#define NETMON_FRAME_LINE_CAP (NETMON_FRAME_VISIBLE_CHARS + 1u)
#define NETMON_QUERY_FAILURE_GRACE_ATTEMPTS 5u

typedef struct {
    char lines[NETMON_FRAME_LINE_COUNT][NETMON_FRAME_LINE_CAP];
    char freshness_marker;
} netmon_frame_t;

uint8_t netmon_frame_count(const netmon_page_descriptor_t *page,
                           const modem_diag_snapshot_t *modem);
void netmon_format_frame(const netmon_page_descriptor_t *page,
                         uint8_t frame_index,
                         uint32_t capabilities,
                         uint32_t now_ms,
                         const netmon_local_diag_snapshot_t *local,
                         const modem_diag_snapshot_t *modem,
                         const netmon_control_snapshot_t *control,
                         netmon_frame_t *out);

#endif
