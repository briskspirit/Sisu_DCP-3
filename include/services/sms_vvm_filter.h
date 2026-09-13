#ifndef SMS_VVM_FILTER_H
#define SMS_VVM_FILTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Legacy VVM application ports. OMTP ports are carrier/client configured, so
 * every protocol validates its payload rather than trusting a port alone. */
#define SMS_VVM_LEGACY_STATUS_PORT 0x1578u
#define SMS_VVM_LEGACY_SYNC_PORT   0x157bu

bool sms_vvm_control_payload_is_recognized(bool has_ports,
                                           uint16_t destination_port,
                                           const uint8_t *payload,
                                           size_t payload_len);

#endif
