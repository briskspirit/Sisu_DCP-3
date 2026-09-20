#include "services/business_card_codec.h"
#include "services/sms_deliver_codec.h"

#include <assert.h>
#include <string.h>

static phonebook_entry_t entry;
static message_file_t file;
static const char body[] = "BEGIN:VCARD\r\nN:Alice\r\nTEL:+123456789\r\nEND:VCARD\r\n";

static bool decode(const char *text, bool utf8) {
    return business_card_decode((const uint8_t *)text, strlen(text), utf8, &entry);
}

static void part(const char *text, uint8_t total, uint8_t seq, uint16_t port, uint8_t dcs) {
    sms_deliver_t d = {.dcs=dcs};
    strcpy(d.address, "+123456789");
    assert(sms_deliver_scts_encode(2026, 9, 20, 0, 0, 0, 0, d.scts));
    if (total > 1u || port) {
        d.udhi = true; d.ud_len = 1u;
        if (total > 1u) {
            uint8_t concat[] = {0,3,21,total,seq};
            memcpy(d.ud + d.ud_len, concat, sizeof(concat)); d.ud_len += sizeof(concat);
        }
        if (port) {
            uint8_t ports[] = {5,4,(uint8_t)(port >> 8),(uint8_t)port,0,0};
            memcpy(d.ud + d.ud_len, ports, sizeof(ports)); d.ud_len += sizeof(ports);
        }
        d.ud[0] = d.ud_len - 1u;
    }
    for (size_t i = 0u; text[i]; i++) {
        if (dcs == 8u) d.ud[d.ud_len++] = 0u;
        assert(d.ud_len < sizeof(d.ud));
        d.ud[d.ud_len++] = (uint8_t)text[i];
    }
    d.udl = d.ud_len;
    char pdu[SMS_DELIVER_HEX_MAX]; uint8_t n;
    assert(sms_deliver_build(&d, pdu, sizeof(pdu), &n));
    if (!file.count) assert(message_file_receive(&file, pdu));
    else assert(message_file_merge(&file, pdu) == MESSAGE_MERGE_ADDED);
}

int main(void) {
    phonebook_entry_t source = {.name="Alice", .number="+123456789"};
    char text[MODEM_SMS_TEXT_MAX + 1u];
    assert(business_card_encode(&source, text, sizeof(text)));
    assert(strcmp(text, "//SCKE2 BEGIN:VCARD\r\nN:Alice\r\nTEL:+123456789\r\nEND:VCARD\r\n") == 0);
    assert(business_card_text_envelope(text) && !business_card_text_envelope("//SCK"));
    assert(!business_card_encode(&source, text, 8u));
    assert(decode(body, false) && strcmp(entry.name, "Alice") == 0 && strcmp(entry.number, "+123456789") == 0);
    source.name[0] = 0;
    assert(business_card_encode(&source, text, sizeof(text)) && strstr(text, "\r\nN:") == NULL);
    assert(decode(text + 8, true) && entry.name[0] == 0);
    strcpy(source.name, "Bad\r\nTEL:911"); assert(!business_card_encode(&source, text, sizeof(text)));
    strcpy(source.name, "Alice"); strcpy(source.number, "123\r\n");
    assert(!business_card_encode(&source, text, sizeof(text)));
    assert(decode("BEGIN:VCARD\r\nVERSION:2.1\r\nN:Doe;Alice\r\nTEL;CELL:+1 (234)-56\r\nEND:VCARD\r\n", false));
    assert(strcmp(entry.name, "Doe;Alice") == 0 && strcmp(entry.number, "+123456") == 0);
    assert(decode("BEGIN:VCARD\r\nN:Doe;Alice\r\nFN:Alice Doe\r\nTEL:123\r\nEND:VCARD\r\n", true));
    assert(strcmp(entry.name, "Alice Doe") == 0);
    assert(decode("BEGIN:VCARD\r\nN:Jos\xe9\r\nTEL:123\r\nEND:VCARD\r\n", false));
    assert(strcmp(entry.name, "Jos\xc3\xa9") == 0);
    assert(decode("BEGIN:VCARD\r\nN;ENCODING=QUOTED-PRINTABLE;CHARSET=ISO-8859-1:Jos=E9\r\nTEL:123\r\nEND:VCARD\r\n", true));
    assert(strcmp(entry.name, "Jos\xc3\xa9") == 0);
    assert(decode("BEGIN:VCARD\r\nN;CHARSET=UTF-8:Jos\xc3\xa9\r\nTEL:123\r\nEND:VCARD\r\n", false));
    assert(strcmp(entry.name, "Jos\xc3\xa9") == 0);
    assert(decode("BEGIN:VCARD\r\nN:A\\;B\\,C\\\\D\r\nTEL:123\r\nEND:VCARD\r\n", true));
    assert(strcmp(entry.name, "A;B,C\\D") == 0);
    const char *bad[] = {
        "BEGIN:VCARD\nTEL:123\nEND:VCARD\n", "BEGIN:VCARD\r\nTEL:123\r\n",
        "BEGIN:VCARD\r\nN:A\r\nEND:VCARD\r\n", "BEGIN:VCARD\r\nTEL:+\r\nEND:VCARD\r\n",
        "BEGIN:VCARD\r\nTEL:123\r\nEND:VCARD\r\nx", "BEGIN:VCARD\r\nBEGIN:VCARD\r\nTEL:123\r\nEND:VCARD\r\n",
        "BEGIN:VCARD\r\nN:1234567890123456789012345\r\nTEL:123\r\nEND:VCARD\r\n",
        "BEGIN:VCARD\r\nN;ENCODING=BASE64:QQ==\r\nTEL:123\r\nEND:VCARD\r\n",
        "BEGIN:VCARD\r\nN;CHARSET=UTF-8:\xc0\xaf\r\nTEL:123\r\nEND:VCARD\r\n",
        "BEGIN:VCARD\r\nN;ENCODING=QUOTED-PRINTABLE:=00\r\nTEL:123\r\nEND:VCARD\r\n",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) assert(!decode(bad[i], true));

    strcpy(text, "//SCKE2 "); strcat(text, body);
    part(text, 1, 1, 0, 8);
    assert(business_card_file_decode(&file, &entry) == BUSINESS_CARD_VALID);
    memset(&file, 0, sizeof(file));
    part(text + 5, 2, 2, 0, 8); /* Prefix split and second part first. */
    assert(business_card_file_decode(&file, &entry) == BUSINESS_CARD_NONE);
    part("//SCK", 2, 1, 0, 8);
    assert(business_card_file_decode(&file, &entry) == BUSINESS_CARD_VALID);
    assert(strcmp(entry.name, "Alice") == 0);
    file.flags |= MESSAGE_FILE_QUARANTINED;
    /* A quarantined partial prefix is not enough to discard an ordinary SMS. */
    assert(business_card_file_decode(&file, &entry) == BUSINESS_CARD_NONE);
    const uint16_t ports[] = {0xe2, 0x23f4, 0xc004};
    for (unsigned i = 0; i < 3; i++) {
        memset(&file, 0, sizeof(file)); part(body, 1, 1, ports[i], 0xf5);
        assert(business_card_file_decode(&file, &entry) == BUSINESS_CARD_VALID);
        file.flags |= MESSAGE_FILE_QUARANTINED;
        assert(business_card_file_decode(&file, &entry) == BUSINESS_CARD_INVALID);
    }
    memset(&file, 0, sizeof(file)); part(body, 1, 1, 0x1585, 4);
    assert(business_card_file_decode(&file, &entry) == BUSINESS_CARD_NONE);
    memset(&file, 0, sizeof(file)); part(body, 1, 1, 0, 8);
    assert(business_card_file_decode(&file, &entry) == BUSINESS_CARD_NONE);
    memset(&file, 0, sizeof(file)); part("//SCKE2 invalid", 1, 1, 0, 8);
    assert(business_card_file_decode(&file, &entry) == BUSINESS_CARD_INVALID);
    return 0;
}
