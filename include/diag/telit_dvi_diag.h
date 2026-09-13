#ifndef TELIT_DVI_DIAG_H
#define TELIT_DVI_DIAG_H

#include <stdbool.h>
#include <stdint.h>

#include "audio/audio_bridge.h"
#include "audio/audio_i2s_hal.h"
#include "audio/modem_i2s_hal.h"

typedef enum {
    TELIT_DVI_ROUTE_HANDSET = 0,
    TELIT_DVI_ROUTE_HEADSET,
} telit_dvi_route_t;

typedef struct {
    bool bclk_valid;
    bool wa_valid;
    uint32_t bclk_hz;
    uint32_t wa_hz;
    uint32_t bclk_per_frame_x1000;
} telit_dvi_clock_measurement_t;

typedef struct {
    bool initialized;
    bool codec_ready;
    bool codec_i2s_ready;
    bool modem_i2s_ready;
    bool clock_probe_ready;
    bool start_pending;
    bool bclk_qualifying;
    bool bclk_live;
    bool bclk_fresh;
    bool saw_bclk;
    telit_dvi_route_t route;
    audio_bridge_stats_t bridge;
    audio_i2s_stats_t codec_i2s;
    modem_i2s_stats_t modem_i2s;
    modem_i2s_tx_probe_t modem_tx;
} telit_dvi_diag_status_t;

bool telit_dvi_diag_init(telit_dvi_route_t route);
bool telit_dvi_diag_set_route(telit_dvi_route_t route);
bool telit_dvi_diag_start(void);
void telit_dvi_diag_stop(void);
void telit_dvi_diag_tick(uint32_t now_ms);
bool telit_dvi_diag_measure_clocks(telit_dvi_clock_measurement_t *out);
void telit_dvi_diag_get_status(telit_dvi_diag_status_t *out);
const char *telit_dvi_diag_route_text(telit_dvi_route_t route);

#endif
