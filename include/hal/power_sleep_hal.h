#ifndef POWER_SLEEP_HAL_H
#define POWER_SLEEP_HAL_H

#include <stdbool.h>
#include <stdint.h>

void power_sleep_hal_init(uint32_t now_ms);
void power_sleep_hal_poll(uint32_t now_ms);
bool power_sleep_hal_service_vbus_raw(void);
bool power_sleep_hal_service_vbus_present(void);

typedef struct {
    uint32_t button;
    uint32_t shared_irq;
    uint32_t service_vbus;
    uint32_t unused_slot;
} power_sleep_hal_wake_evidence_t;

typedef struct {
    bool button;
    bool shared_irq;
    bool service_vbus;
} power_sleep_hal_wake_sources_t;

/* The sleep HAL owns POWMAN slots and electrical polarity for GP7/GP42 and,
 * in the CDC-enabled service image, GP28. Evidence may be read before
 * board_init during boot capture. */
void power_sleep_hal_read_wake_evidence(
    power_sleep_hal_wake_evidence_t *out);
void power_sleep_hal_decode_wake_sources(
    const power_sleep_hal_wake_evidence_t *evidence,
    uint32_t last_swcore_pwrup,
    power_sleep_hal_wake_sources_t *out);
void power_sleep_hal_arm_wake_sources(void);
void power_sleep_hal_disarm_wake_sources(void);
bool power_sleep_hal_power_button_asserted(void);
bool power_sleep_hal_shared_irq_asserted(void);

/* Apply the complete Rev B2-only dormant pad ledger after the final peripheral
 * transaction. GP7/GP42 and I2C0 are always left wake/live; GP28 remains live
 * only in the CDC-enabled service image. */
void power_sleep_hal_park_pads(void);

#endif
