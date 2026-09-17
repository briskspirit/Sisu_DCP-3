#include <stdio.h>
#include <string.h>

#include "services/sms_control_filter.h"
#include "services/sms_deliver_codec.h"
#include "sms_control_fixtures.h"

static unsigned failures;
static void check(bool ok, const char *message) {
    if (!ok) { printf("FAIL: %s\n", message); failures++; }
}

static sms_codec_message_t dm_message(bool wdp) {
    sms_codec_message_t message = {0};
    const char *hex = wdp ? CONTROL_DM_WDP_HEX : CONTROL_DM_WSP_HEX;
    message.binary = true;
    message.dcs = 4u;
    message.has_ports = !wdp;
    message.dest_port = 2948u;
    message.binary_len = (uint16_t)(strlen(hex) / 2u);
    for (size_t i = 0u; i < message.binary_len; i++) {
        unsigned byte = 0u;
        (void)sscanf(hex + i * 2u, "%2x", &byte);
        message.binary_data[i] = (uint8_t)byte;
    }
    return message;
}

static void test_dm(void) {
    for (unsigned wdp = 0u; wdp < 2u; wdp++) {
        sms_codec_message_t m = dm_message(wdp != 0u);
        check(sms_control_classify(&m, wdp != 0u) == SMS_CONTROL_OMA_DM,
              "complete synthetic OMA-DM notification recognized on either transport");
        uint16_t length = m.binary_len;
        for (uint16_t n = 0u; n < length; n++) {
            m.binary_len = n;
            check(sms_control_classify(&m, wdp != 0u) == SMS_CONTROL_KEEP,
                  "every truncated notification is preserved");
        }
        m.binary_len = length + 1u;
        check(sms_control_classify(&m, wdp != 0u) == SMS_CONTROL_KEEP,
              "vendor-specific trailing body is not silently discarded");
        m.binary_len = length;
        size_t wsp = wdp ? 7u : 0u;
        const size_t altered[] = {1u, 2u, 3u, 4u, 5u, 22u, 23u, 24u, 25u, 26u, 29u};
        for (size_t i = 0u; i < sizeof(altered) / sizeof(altered[0]); i++) {
            m.binary_data[wsp + altered[i]] ^= 1u;
            check(sms_control_classify(&m, wdp != 0u) == SMS_CONTROL_KEEP,
                  "unknown headers, versions, reserved bits and server lengths are kept");
            m.binary_data[wsp + altered[i]] ^= 1u;
        }
        m.binary_data[wsp + 5u] = 0x9au;
        check(sms_control_classify(&m, wdp != 0u) == SMS_CONTROL_KEEP,
              "LwM2M application ID is not legacy OMA-DM");
        m.binary_data[wsp + 5u] = 0x87u;
        m.binary_data[wsp + 30u] = 0u;
        check(sms_control_classify(&m, wdp != 0u) == SMS_CONTROL_KEEP,
              "embedded NUL in server identifier is kept");
    }
    sms_codec_message_t m = dm_message(true);
    check(sms_control_classify(&m, false) == SMS_CONTROL_KEEP,
          "binary payload alone cannot assert WDP provenance");
    for (size_t i = 0u; i < 7u; i++) {
        if (i == 3u || i == 4u) { continue; } /* source port is not an identity */
        m.binary_data[i] ^= 1u;
        check(sms_control_classify(&m, true) == SMS_CONTROL_KEEP,
              "invalid WDP type, segmentation or destination is preserved");
        m.binary_data[i] ^= 1u;
    }
    m = dm_message(false);
    m.dest_port = 5500u;
    check(sms_control_classify(&m, false) == SMS_CONTROL_KEEP, "wrong UDH port kept");
    m.dest_port = 2948u;
    m.has_concat = true;
    check(sms_control_classify(&m, false) == SMS_CONTROL_KEEP, "multipart controls kept");
    m.has_concat = false;
    m.udh_unhandled = true;
    check(sms_control_classify(&m, false) == SMS_CONTROL_KEEP, "unhandled UDH kept");
    m.udh_unhandled = false;
    m.trailing_data = true;
    check(sms_control_classify(&m, false) == SMS_CONTROL_KEEP, "unparsed trailing bytes kept");
    m.trailing_data = false;
    m.submit = true;
    check(sms_control_classify(&m, false) == SMS_CONTROL_KEEP, "outgoing records kept");
    m.submit = false;
    const uint8_t dcs[] = {0x14u, 0x16u, 0x24u, 0x84u, 0xc8u, 0xd8u, 0xe8u};
    for (size_t i = 0u; i < sizeof(dcs); i++) {
        m.dcs = dcs[i];
        check(sms_control_classify(&m, false) == SMS_CONTROL_KEEP,
              "flash, SIM class, compressed, reserved and MWI codings kept");
    }
    m.dcs = 4u;
    m.pid = 0x7fu;
    check(sms_control_classify(&m, false) == SMS_CONTROL_KEEP, "SIM download PID kept");
    m.pid = 0x40u;
    check(sms_control_classify(&m, false) == SMS_CONTROL_TYPE0, "type-0 PID consumed");
    m.pid = 0u;
    m.binary_data[3] = 0xbeu;
    m.binary_data[5] = 0x84u;
    check(sms_control_classify(&m, false) == SMS_CONTROL_KEEP, "MMS notification kept");
    check(sms_control_classify(NULL, false) == SMS_CONTROL_KEEP, "null is not a control");
}

static void test_vvm(void) {
    sms_codec_message_t m = {0};
    m.has_ports = true;
    m.dest_port = 5500u;
    strcpy(m.text, "//VVM:SYNC:ev=NM;id=123;");
    check(sms_control_classify(&m, false) == SMS_CONTROL_VVM, "known OMTP text control");
    m.has_ports = false;
    check(sms_control_classify(&m, false) == SMS_CONTROL_KEEP, "ordinary SMS quoting VVM kept");
    m.has_ports = true;
    m.has_concat = true;
    check(sms_control_classify(&m, false) == SMS_CONTROL_KEEP, "VVM fragment kept");
    m.has_concat = false;
    m.dcs = 0xd8u;
    check(sms_control_classify(&m, false) == SMS_CONTROL_KEEP, "MWI store text kept");
    m.dcs = 4u;
    m.binary = true;
    static const char control[] = "//VVM:SYNC:ev=NM;id=123;";
    memcpy(m.binary_data, control, sizeof(control) - 1u);
    m.binary_len = sizeof(control) - 1u;
    check(sms_control_classify(&m, false) == SMS_CONTROL_VVM, "known OMTP binary control");
    m.binary_data[0] = '?';
    check(sms_control_classify(&m, false) == SMS_CONTROL_KEEP, "unknown VVM payload kept");
}

static void test_udh_validation(void) {
    sms_deliver_t d = {0};
    strcpy(d.address, "15551230000");
    d.toa = 0x81u;
    d.dcs = 4u;
    d.udhi = true;
    (void)sms_deliver_scts_encode(2026u, 9u, 17u, 12u, 0u, 0u, 0, d.scts);
    sms_codec_message_t m = dm_message(false);
    static const uint8_t udh[] = {6u, 5u, 4u, 0x0bu, 0x84u, 0xc0u, 2u};
    memcpy(d.ud, udh, sizeof(udh));
    memcpy(d.ud + sizeof(udh), m.binary_data, m.binary_len);
    d.udl = d.ud_len = (uint8_t)(sizeof(udh) + m.binary_len);
    char hex[SMS_DELIVER_HEX_MAX];
    uint8_t tpdu;
    check(sms_deliver_build(&d, hex, sizeof(hex), &tpdu) && sms_pdu_decode(hex, &m) &&
              !m.udh_unhandled && m.pid == 0u && m.dcs == 4u &&
              sms_control_classify(&m, false) == SMS_CONTROL_OMA_DM,
          "3GPP port-addressed PDU is eligible after decoding");
    strcat(hex, "00");
    check(sms_pdu_decode(hex, &m) && m.trailing_data &&
              sms_control_classify(&m, false) == SMS_CONTROL_KEEP,
          "bytes beyond TP-UDL cannot be hidden by decoding before filtering");
    /* An orphan UDH octet must not disappear during decoding and accidentally
     * promote a malformed packet into a known control. */
    memmove(d.ud + 8u, d.ud + 7u, d.ud_len - 7u);
    d.ud[0] = 7u;
    d.ud[7] = 0x70u;
    d.udl = ++d.ud_len;
    check(sms_deliver_build(&d, hex, sizeof(hex), &tpdu) && sms_pdu_decode(hex, &m) &&
              m.udh_unhandled && sms_control_classify(&m, false) == SMS_CONTROL_KEEP,
          "malformed extra UDH data is not filtered");

    /* Replace the orphan by a second port IE. Even identical duplicate
     * addressing is not a qualified envelope. */
    memmove(d.ud + 13u, d.ud + 8u, d.ud_len - 8u);
    memcpy(d.ud + 7u, udh + 1u, 6u);
    d.ud[0] = 12u;
    d.ud_len += 5u;
    d.udl = d.ud_len;
    check(sms_deliver_build(&d, hex, sizeof(hex), &tpdu) && sms_pdu_decode(hex, &m) &&
              m.udh_unhandled && sms_control_classify(&m, false) == SMS_CONTROL_KEEP,
          "duplicate port IE is not filtered");
    d.ud[7] = 0x70u;
    check(sms_deliver_build(&d, hex, sizeof(hex), &tpdu) && sms_pdu_decode(hex, &m) &&
              m.udh_unhandled && sms_control_classify(&m, false) == SMS_CONTROL_KEEP,
          "unknown IE alongside valid ports is not filtered");
}

int main(void) {
    test_dm();
    test_vvm();
    test_udh_validation();
    if (failures) { printf("%u failures\n", failures); return 1; }
    puts("test_sms_control_filter: all passed");
    return 0;
}
