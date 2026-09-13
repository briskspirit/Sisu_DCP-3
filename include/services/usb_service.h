#ifndef USB_SERVICE_H
#define USB_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t starts;
    uint32_t stops;
    uint32_t worker_kicks;
    uint32_t worker_disabled_rearms;
    uint32_t event_pending_observations;
    uint32_t event_pending_streak_max;
    uint32_t mounts;
    uint32_t unmounts;
    uint32_t suspends;
    uint32_t resumes;
    uint32_t line_state_changes;
    uint32_t rx_callbacks;
    uint8_t line_state;
} usb_service_diag_t;

/* Runtime owner of the RP USB device block. GP28 VBUS policy stays in
 * power_sleep_hal; this service owns TinyUSB, CDC, clk_usb, and PHY state. */
void usb_service_init(bool vbus_present, uint32_t now_ms);
void usb_service_poll(bool vbus_present, uint32_t now_ms);
void usb_service_shutdown(void);

bool usb_service_active(void);
bool usb_service_connected(void);
bool usb_service_mounted(void);
bool usb_service_host_live(void);
uint32_t usb_service_sie_status(void);
uint32_t usb_service_attach_failures(void);
void usb_service_get_diag(usb_service_diag_t *out);

#endif
