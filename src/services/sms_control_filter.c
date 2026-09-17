#include "services/sms_control_filter.h"

#include <string.h>

#include "services/sms_vvm_filter.h"

#define WAP_PUSH_PORT 2948u

static bool oma_dm_notification(const uint8_t *data, size_t length) {
    /* The qualified connectionless WSP form: transaction ID, Push, three
     * header octets, SyncML notification content type, SyncML DM app ID.
     * Other encodings/headers stay visible until explicitly supported. */
    static const uint8_t headers[] = {0x06u, 0x03u, 0xc4u, 0xafu, 0x87u};
    if (length < 6u + 24u + 1u ||
        memcmp(data + 1u, headers, sizeof(headers)) != 0) {
        return false;
    }
    /* Package 0: 16-byte digest, then the eight-byte notification header.
     * Version 11 is the notification format used by OMA-DM 1.2.1. Require
     * server initiation, zero reserved bits and no vendor-specific body. */
    const uint8_t *header = data + 6u + 16u;
    unsigned version = ((unsigned)header[0] << 2u) | (header[1] >> 6u);
    size_t server_length = header[7];
    if (version != 11u || (header[1] & 0x0fu) != 0x08u ||
        header[2] != 0u || header[3] != 0u || header[4] != 0u ||
        server_length == 0u || length != 6u + 24u + server_length) {
        return false;
    }
    for (size_t i = 0u; i < server_length; i++) {
        if (header[8u + i] < 0x21u || header[8u + i] > 0x7eu) {
            return false;
        }
    }
    return true;
}

sms_control_filter_t sms_control_classify(const sms_codec_message_t *message,
                                          bool wdp) {
    if (message == NULL || message->submit) {
        return SMS_CONTROL_KEEP;
    }
    if (message->pid == 0x40u) {
        return SMS_CONTROL_TYPE0;
    }
    /* Keep MWI, class-0/2, SIM download, reserved/compressed encodings and
     * all multipart/unknown UDH on their existing paths. Never filter a
     * fragment solely because its prefix resembles a complete control. */
    if (message->pid != 0u || message->has_concat || message->udh_unhandled ||
        message->trailing_data || message->binary_len > sizeof(message->binary_data) ||
        (message->dcs != 0x00u && message->dcs != 0x04u && message->dcs != 0x08u)) {
        return SMS_CONTROL_KEEP;
    }
    const uint8_t *payload = message->binary
        ? message->binary_data : (const uint8_t *)message->text;
    size_t length = message->binary ? message->binary_len : strlen(message->text);
    if (sms_vvm_control_payload_is_recognized(message->has_ports,
                                              message->dest_port, payload, length)) {
        return SMS_CONTROL_VVM;
    }
    if (!message->binary || message->dcs != 0x04u) {
        return SMS_CONTROL_KEEP;
    }
    if (wdp) {
        /* CDMA WAP teleservice: WDP type/total/segment, source/dest ports.
         * Only a complete single-segment datagram is eligible. */
        if (message->has_ports || length < 7u || payload[0] != 0u ||
            payload[1] != 1u || payload[2] != 0u ||
            (((unsigned)payload[5] << 8u) | payload[6]) != WAP_PUSH_PORT) {
            return SMS_CONTROL_KEEP;
        }
        payload += 7u;
        length -= 7u;
    } else if (!message->has_ports || message->dest_port != WAP_PUSH_PORT) {
        return SMS_CONTROL_KEEP;
    }
    return oma_dm_notification(payload, length) ? SMS_CONTROL_OMA_DM : SMS_CONTROL_KEEP;
}
