# SMS Direct Delivery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Receive every incoming SMS (3GPP on AT&T, 3GPP2 on Verizon) through one route: `+CMT` direct delivery, rebuilt as a standard SMS-DELIVER PDU and written into the ME store with `+CMGW`, so the existing mailbox pipeline and UI keep working unchanged.

**Architecture:** A pure SMS-DELIVER builder (`sms_deliver_codec`) and a pure two-line `+CMT` collector with generic 3GPP parsers (`modem_sms_direct`) live in services; the Telit-only 3GPP2 forms live in a vendor file behind one optional `modem_vendor_t` hook. `modem_service` routes `+CMT` into the collector, keeps built PDUs in a 2-entry retry ring, and runs a new `STORE_DELIVERED` protocol operation (`AT+CMGF=0`, `AT+CMGW=<len>,0`, body, `AT+CMGF=1`) whose success is booked exactly like a `+CMTI`.

**Tech Stack:** C11 firmware (Pico SDK, RP2354), host unit tests via `tests/run_tests.sh` (ASan/UBSan, `run`/`run_telit` registration), Telit LE910C1-WWX AT commands.

**Spec:** `docs/sms_direct_delivery_design.md`

## Global Constraints

- Resting SMS mode stays `AT+CMGF=1`; no change to DTR/RI/CFUN sleep logic, `sms_wake`, or transport timing.
- Generic services code (`modem_service.c`, `modem_sms_direct.c`, `sms_deliver_codec.c`) never contains "3GPP2", teleservice ids, CDMA encodings or `$QCMTI` parsing; those live only in `modem_vendor_telit_sms.c`.
- Every new pure module gets a host test registered in `tests/run_tests.sh`; `tests/run_tests.sh` must pass in full before each commit.
- `tests/layering_scan.py` classification: new files under `src/services/` and `include/services/` only.
- Commit messages end with `Claude-Session: https://claude.ai/code/session_01UeSArQWYHateziEWYPtoQn`.
- The bench build already flashed (`AT+CNMI=2,2,0,0,0` only) is NOT the deliverable; Task 1 replaces that uncommitted one-line edit.

---

### Task 1: Telit init applies direct delivery and the extended header

**Files:**
- Modify: `src/services/modem_vendor_telit_provision.c:108-116`
- Test: `tests/test_modem_vendor_telit.c`

**Interfaces:**
- Produces: init table rows `AT+CSDH=1` and `AT+CNMI=2,2,0,0,0` (both `MODEM_SETTING_PROFILE`, `MODEM_INIT_PREREQ_SIM_READY`).

- [ ] **Step 1: Write the failing test**

Add to `tests/test_modem_vendor_telit.c` (next to the other init-table checks; find the test list at the bottom of `main`):

```c
static void test_init_table_direct_sms_delivery(void) {
    bool saw_csdh = false;
    bool saw_cnmi_direct = false;
    bool saw_cnmi_store = false;
    for (size_t i = 0u; i < TELIT_INIT_STEP_COUNT; i++) {
        const char *cmd = TELIT_INIT_STEPS[i].cmd;
        if (strcmp(cmd, "AT+CSDH=1") == 0) {
            saw_csdh = TELIT_INIT_STEPS[i].persistence == MODEM_SETTING_PROFILE;
        } else if (strcmp(cmd, "AT+CNMI=2,2,0,0,0") == 0) {
            saw_cnmi_direct = true;
        } else if (strncmp(cmd, "AT+CNMI=1,", 10u) == 0) {
            saw_cnmi_store = true;
        }
    }
    check(saw_csdh, "init sets the extended text header (+CSDH=1)");
    check(saw_cnmi_direct && !saw_cnmi_store,
          "init routes SMS-DELIVER directly (+CNMI=2,2) instead of storing");
}
```

Register it in `main` next to the other `test_init_*` calls.

- [ ] **Step 2: Run test to verify it fails**

Run: `tests/run_tests.sh 2>&1 | grep -A3 "test_modem_vendor_telit"`
Expected: `FAIL: test_modem_vendor_telit` with the "+CSDH=1" check message.

- [ ] **Step 3: Edit the init table**

In `src/services/modem_vendor_telit_provision.c` replace the `AT+CNMI` row and add `AT+CSDH=1` directly after `AT+CMGF=1`:

```c
    TELIT_INIT("AT+CMGF=1",            2500u, 3u, false, MODEM_DEGRADE_NONE,
               MODEM_INIT_PREREQ_SIM_READY, MODEM_SETTING_PROFILE, NULL),
    /* Text-mode +CMT/+CMGR/+CMGL carry <tooa>,<fo>,<pid>,<dcs>,<length>
     * (3GPP) or <tooa>,<tele_id>,<priority>,<enc>,<length> (Telit 3GPP2)
     * only with +CSDH=1. Direct delivery needs them to rebuild a PDU. */
    TELIT_INIT("AT+CSDH=1",            2500u, 3u, false, MODEM_DEGRADE_NONE,
               MODEM_INIT_PREREQ_SIM_READY, MODEM_SETTING_PROFILE, NULL),
```

and

```c
    /* Direct delivery (<mt>=2): Verizon 3GPP2 messages cannot be read back
     * from the CDMA store on this image ($QCMTI rows fail in every mode), so
     * every carrier's SMS-DELIVER is routed to the host as +CMT and re-stored
     * as a 23.040 PDU by modem_service. Mode 2 buffers the URC while the
     * TA-TE link is reserved and flushes it afterwards; DTR sleep unchanged. */
    TELIT_INIT("AT+CNMI=2,2,0,0,0",    2500u, 3u, false, MODEM_DEGRADE_NONE,
               MODEM_INIT_PREREQ_SIM_READY, MODEM_SETTING_PROFILE, NULL),
```

Check whether `tests/test_modem_telit_service.c` asserts the init sequence by exact command text (grep `"AT+CNMI=1,1,0,0,0"` and `init=22` / step counts); update those expectations (the step count grows by one).

- [ ] **Step 4: Run the full test suite**

Run: `tests/run_tests.sh 2>&1 | tail -5`
Expected: all PASS, `fail=0`.

- [ ] **Step 5: Commit**

```bash
git add src/services/modem_vendor_telit_provision.c tests/test_modem_vendor_telit.c tests/test_modem_telit_service.c
git commit -m "Route Telit SMS-DELIVER directly with the extended text header

Claude-Session: https://claude.ai/code/session_01UeSArQWYHateziEWYPtoQn"
```

---

### Task 2: SMS-DELIVER TPDU builder (`sms_deliver_codec`)

**Files:**
- Create: `include/services/sms_deliver_codec.h`
- Create: `src/services/sms_deliver_codec.c`
- Create: `tests/test_sms_deliver_codec.c`
- Modify: `tests/run_tests.sh:783` (register after `test_sms_picture_codec`)
- Modify: `CMakeLists.txt:218` (add source next to `sms_submit_codec.c`)

**Interfaces:**
- Produces:

```c
#define SMS_DELIVER_UD_MAX 140u
#define SMS_DELIVER_HEX_MAX 384u   /* "00" + up to 176 TPDU bytes as hex + NUL */

typedef struct {
    char address[SMS_CODEC_ADDRESS_MAX + 1u]; /* digits; leading '+' allowed */
    uint8_t toa;        /* 0x91 international, 0x81 unknown/national */
    uint8_t scts[7];    /* TP-SCTS semi-octets, copied verbatim */
    uint8_t pid;
    uint8_t dcs;
    bool udhi;
    uint8_t udl;        /* septets for GSM-7 DCS, octets otherwise */
    uint8_t ud[SMS_DELIVER_UD_MAX];
    uint8_t ud_len;     /* octets used in ud[] */
} sms_deliver_t;

bool sms_deliver_scts_encode(uint16_t year, uint8_t month, uint8_t day,
                             uint8_t hour, uint8_t minute, uint8_t second,
                             int8_t tz_quarters, uint8_t out[7]);
bool sms_deliver_pack_gsm7(const uint8_t *codes, uint8_t septets,
                           uint8_t *dst, size_t cap, uint8_t *out_len);
/* Latin-1 byte -> GSM 03.38 code(s). Returns 0 when no mapping exists. */
uint8_t sms_deliver_gsm7_from_latin1(uint8_t ch, uint8_t out[2]);
bool sms_deliver_build(const sms_deliver_t *deliver, char *hex,
                       size_t hex_cap, uint8_t *out_tpdu_len);
```

- [ ] **Step 1: Write the failing tests**

`tests/test_sms_deliver_codec.c`:

```c
#include <stdio.h>
#include <string.h>

#include "services/sms_deliver_codec.h"
#include "services/sms_picture_codec.h"

static int s_failures;
static void check(bool ok, const char *msg) {
    if (!ok) { printf("FAIL: %s\n", msg); s_failures++; }
}

static void test_scts_encode(void) {
    uint8_t scts[7];
    check(sms_deliver_scts_encode(2026u, 9u, 16u, 10u, 59u, 28u, 0, scts),
          "scts encodes");
    static const uint8_t expected[7] = {0x62, 0x90, 0x61, 0x01, 0x95, 0x82, 0x00};
    check(memcmp(scts, expected, 7u) == 0, "scts semi-octets 26/09/16 10:59:28 tz 0");
    check(sms_deliver_scts_encode(2026u, 9u, 16u, 10u, 59u, 4u, -16, scts),
          "scts encodes negative zone");
    check(scts[6] == 0x69, "tz -16 quarters -> 0x69 (sign in tens nibble)");
    check(!sms_deliver_scts_encode(2026u, 13u, 16u, 10u, 59u, 4u, 0, scts),
          "month 13 rejected");
}

static void test_pack_gsm7(void) {
    uint8_t packed[8];
    uint8_t len = 0u;
    check(sms_deliver_pack_gsm7((const uint8_t *)"Djssjjs", 7u, packed,
                                sizeof(packed), &len) && len == 7u,
          "packs 7 septets into 7 octets");
    static const uint8_t expected[7] = {0x44, 0xF5, 0x7C, 0xAE, 0x56, 0xCF, 0x01};
    check(memcmp(packed, expected, 7u) == 0, "packed bytes match the modem sample");
    check(!sms_deliver_pack_gsm7((const uint8_t *)"Djssjjs", 7u, packed, 6u, &len),
          "capacity is enforced");
}

static void test_latin1_map(void) {
    uint8_t out[2];
    check(sms_deliver_gsm7_from_latin1('A', out) == 1u && out[0] == 'A', "ASCII letter identity");
    check(sms_deliver_gsm7_from_latin1('@', out) == 1u && out[0] == 0x00u, "@ -> 0x00");
    check(sms_deliver_gsm7_from_latin1(0xE9u, out) == 1u && out[0] == 0x05u, "e-acute -> 0x05");
    check(sms_deliver_gsm7_from_latin1('[', out) == 2u && out[0] == 0x1Bu && out[1] == 0x3Cu,
          "[ -> ESC 0x3C");
    check(sms_deliver_gsm7_from_latin1(0xB5u, out) == 0u, "micro sign has no GSM mapping");
}

static void test_build_roundtrip_gsm7(void) {
    sms_deliver_t d;
    memset(&d, 0, sizeof(d));
    strcpy(d.address, "7866910488");
    d.toa = 0x81u;
    (void)sms_deliver_scts_encode(2026u, 9u, 16u, 10u, 59u, 28u, 0, d.scts);
    d.dcs = 0x00u;
    d.udl = 7u;
    (void)sms_deliver_pack_gsm7((const uint8_t *)"Djssjjs", 7u, d.ud, sizeof(d.ud), &d.ud_len);
    char hex[SMS_DELIVER_HEX_MAX];
    uint8_t tpdu_len = 0u;
    check(sms_deliver_build(&d, hex, sizeof(hex), &tpdu_len), "build ok");
    check(strcmp(hex, "00040A8187661940880000629061019582000744F57CAE56CF01") == 0,
          "hex matches the fixed vector");
    check(tpdu_len == 25u, "tpdu length excludes the SMSC byte");
    sms_codec_message_t decoded;
    check(sms_pdu_decode(hex, &decoded) && !decoded.submit &&
              strcmp(decoded.address, "7866910488") == 0 &&
              strcmp(decoded.text, "Djssjjs") == 0 &&
              strcmp(decoded.timestamp, "26/09/16,10:59:28") == 0,
          "phone decoder reads the built DELIVER back");
}

static void test_build_ucs2_and_udh(void) {
    sms_deliver_t d;
    memset(&d, 0, sizeof(d));
    strcpy(d.address, "+18132936877");
    d.toa = 0x91u;
    (void)sms_deliver_scts_encode(2026u, 9u, 16u, 11u, 27u, 49u, 0, d.scts);
    d.dcs = 0x08u;
    static const uint8_t emoji[4] = {0xD8, 0x3D, 0xDE, 0x1C};
    memcpy(d.ud, emoji, 4u); d.ud_len = 4u; d.udl = 4u;
    char hex[SMS_DELIVER_HEX_MAX];
    uint8_t tpdu_len = 0u;
    check(sms_deliver_build(&d, hex, sizeof(hex), &tpdu_len), "ucs2 build ok");
    sms_codec_message_t decoded;
    check(sms_pdu_decode(hex, &decoded) && strcmp(decoded.address, "+18132936877") == 0 &&
              strcmp(decoded.text, "\xF0\x9F\x98\x9C") == 0,
          "emoji round-trips as UTF-8");

    /* UDH concat part 2 of 2, ref 0x62, GSM-7 body "252ut9u9u952525" packed after the header */
    memset(&d, 0, sizeof(d));
    strcpy(d.address, "7866910488"); d.toa = 0x81u;
    (void)sms_deliver_scts_encode(2026u, 9u, 16u, 10u, 55u, 53u, 0, d.scts);
    d.dcs = 0x00u; d.udhi = true; d.udl = 23u;
    static const uint8_t ud[21] = {0x05,0x00,0x03,0x62,0x02,0x02,0xD4,0x64,0x35,0x59,0x9D,
                                   0x9E,0xAB,0xE7,0xEA,0xB9,0x9A,0xAC,0x26,0xAB,0xC9};
    memcpy(d.ud, ud, sizeof(ud)); d.ud_len = sizeof(ud);
    check(sms_deliver_build(&d, hex, sizeof(hex), &tpdu_len), "udh build ok");
    check(sms_pdu_decode(hex, &decoded) && decoded.has_concat &&
              decoded.concat_ref == 0x62u && decoded.concat_total == 2u &&
              decoded.concat_seq == 2u,
          "UDH concat fields survive the rebuild");
    check(hex[2] == '4' && hex[3] == '4', "first octet has UDHI (0x44)");
}

static void test_build_rejects(void) {
    sms_deliver_t d;
    memset(&d, 0, sizeof(d));
    char hex[SMS_DELIVER_HEX_MAX];
    uint8_t tpdu_len = 0u;
    check(!sms_deliver_build(&d, hex, sizeof(hex), &tpdu_len), "empty address rejected");
    strcpy(d.address, "12ab");
    check(!sms_deliver_build(&d, hex, sizeof(hex), &tpdu_len), "non-digit address rejected");
    strcpy(d.address, "123");
    d.ud_len = 141u;
    check(!sms_deliver_build(&d, hex, 8u, &tpdu_len), "small hex buffer rejected");
}

int main(void) {
    test_scts_encode();
    test_pack_gsm7();
    test_latin1_map();
    test_build_roundtrip_gsm7();
    test_build_ucs2_and_udh();
    test_build_rejects();
    if (s_failures != 0) { printf("%d failures\n", s_failures); return 1; }
    printf("test_sms_deliver_codec: all passed\n");
    return 0;
}
```

Register in `tests/run_tests.sh` right after the `test_sms_picture_codec` line:

```sh
run test_sms_deliver_codec  src/services/sms_deliver_codec.c src/services/sms_picture_codec.c
```

- [ ] **Step 2: Run test to verify it fails**

Run: `tests/run_tests.sh 2>&1 | grep -A6 test_sms_deliver_codec`
Expected: `BUILD FAIL: test_sms_deliver_codec` (header missing).

- [ ] **Step 3: Implement the codec**

`include/services/sms_deliver_codec.h`:

```c
#ifndef SMS_DELIVER_CODEC_H
#define SMS_DELIVER_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/sms_picture_codec.h"

#define SMS_DELIVER_UD_MAX 140u
#define SMS_DELIVER_HEX_MAX 384u

typedef struct {
    char address[SMS_CODEC_ADDRESS_MAX + 1u];
    uint8_t toa;
    uint8_t scts[7];
    uint8_t pid;
    uint8_t dcs;
    bool udhi;
    uint8_t udl;
    uint8_t ud[SMS_DELIVER_UD_MAX];
    uint8_t ud_len;
} sms_deliver_t;

bool sms_deliver_scts_encode(uint16_t year, uint8_t month, uint8_t day,
                             uint8_t hour, uint8_t minute, uint8_t second,
                             int8_t tz_quarters, uint8_t out[7]);
bool sms_deliver_pack_gsm7(const uint8_t *codes, uint8_t septets,
                           uint8_t *dst, size_t cap, uint8_t *out_len);
uint8_t sms_deliver_gsm7_from_latin1(uint8_t ch, uint8_t out[2]);
bool sms_deliver_build(const sms_deliver_t *deliver, char *hex,
                       size_t hex_cap, uint8_t *out_tpdu_len);

#endif
```

`src/services/sms_deliver_codec.c`:

```c
/* SMS-DELIVER (3GPP TS 23.040 9.2.2.1) TPDU builder. Pure: no modem or
 * vendor knowledge. Consumers rebuild a received message that the module
 * handed over out-of-band (+CMT) so it can be stored as an ordinary PDU. */

#include "services/sms_deliver_codec.h"

#include <string.h>

static uint8_t semi_octet(uint8_t value) {
    return (uint8_t)(((value % 10u) << 4) | (value / 10u));
}

bool sms_deliver_scts_encode(uint16_t year, uint8_t month, uint8_t day,
                             uint8_t hour, uint8_t minute, uint8_t second,
                             int8_t tz_quarters, uint8_t out[7]) {
    if (out == NULL || month < 1u || month > 12u || day < 1u || day > 31u ||
        hour > 23u || minute > 59u || second > 59u ||
        tz_quarters < -79 || tz_quarters > 79) {
        return false;
    }
    out[0] = semi_octet((uint8_t)(year % 100u));
    out[1] = semi_octet(month);
    out[2] = semi_octet(day);
    out[3] = semi_octet(hour);
    out[4] = semi_octet(minute);
    out[5] = semi_octet(second);
    uint8_t magnitude = (uint8_t)(tz_quarters < 0 ? -tz_quarters : tz_quarters);
    uint8_t tens = (uint8_t)(magnitude / 10u);
    if (tz_quarters < 0) {
        tens |= 0x08u; /* sign bit lives in the tens digit (23.040 9.2.3.11) */
    }
    out[6] = (uint8_t)(((magnitude % 10u) << 4) | tens);
    return true;
}

bool sms_deliver_pack_gsm7(const uint8_t *codes, uint8_t septets,
                           uint8_t *dst, size_t cap, uint8_t *out_len) {
    if (codes == NULL || dst == NULL || out_len == NULL || septets == 0u) {
        return false;
    }
    size_t bytes = ((size_t)septets * 7u + 7u) / 8u;
    if (bytes > cap) {
        return false;
    }
    memset(dst, 0, bytes);
    for (uint8_t i = 0u; i < septets; i++) {
        uint8_t value = (uint8_t)(codes[i] & 0x7fu);
        uint16_t bit = (uint16_t)i * 7u;
        size_t byte = bit / 8u;
        uint8_t shift = (uint8_t)(bit & 7u);
        dst[byte] |= (uint8_t)(value << shift);
        if (shift > 1u && byte + 1u < bytes) {
            dst[byte + 1u] |= (uint8_t)(value >> (8u - shift));
        }
    }
    *out_len = (uint8_t)bytes;
    return true;
}

/* GSM 03.38 default alphabet, Latin-1 code points that differ from ASCII. */
typedef struct { uint8_t latin1; uint8_t gsm; } gsm7_map_t;
static const gsm7_map_t GSM7_BASIC[] = {
    {0x40u, 0x00u}, {0xA3u, 0x01u}, {0x24u, 0x02u}, {0xA5u, 0x03u},
    {0xE8u, 0x04u}, {0xE9u, 0x05u}, {0xF9u, 0x06u}, {0xECu, 0x07u},
    {0xF2u, 0x08u}, {0xC7u, 0x09u}, {0xD8u, 0x0Bu}, {0xF8u, 0x0Cu},
    {0xC5u, 0x0Eu}, {0xE5u, 0x0Fu}, {0x5Fu, 0x11u}, {0xC6u, 0x1Cu},
    {0xE6u, 0x1Du}, {0xDFu, 0x1Eu}, {0xC9u, 0x1Fu}, {0xA4u, 0x24u},
    {0xA1u, 0x40u}, {0xC4u, 0x5Bu}, {0xD6u, 0x5Cu}, {0xD1u, 0x5Du},
    {0xDCu, 0x5Eu}, {0xA7u, 0x5Fu}, {0xBFu, 0x60u}, {0xE4u, 0x7Bu},
    {0xF6u, 0x7Cu}, {0xF1u, 0x7Du}, {0xFCu, 0x7Eu}, {0xE0u, 0x7Fu},
};
static const gsm7_map_t GSM7_EXT[] = {
    {0x5Eu, 0x14u}, {0x7Bu, 0x28u}, {0x7Du, 0x29u}, {0x5Cu, 0x2Fu},
    {0x5Bu, 0x3Cu}, {0x7Eu, 0x3Du}, {0x5Du, 0x3Eu}, {0x7Cu, 0x40u},
};

uint8_t sms_deliver_gsm7_from_latin1(uint8_t ch, uint8_t out[2]) {
    if (out == NULL) {
        return 0u;
    }
    for (size_t i = 0u; i < sizeof(GSM7_EXT) / sizeof(GSM7_EXT[0]); i++) {
        if (GSM7_EXT[i].latin1 == ch) {
            out[0] = 0x1Bu;
            out[1] = GSM7_EXT[i].gsm;
            return 2u;
        }
    }
    for (size_t i = 0u; i < sizeof(GSM7_BASIC) / sizeof(GSM7_BASIC[0]); i++) {
        if (GSM7_BASIC[i].latin1 == ch) {
            out[0] = GSM7_BASIC[i].gsm;
            return 1u;
        }
    }
    if (ch == 0x0Au || ch == 0x0Du || (ch >= 0x20u && ch < 0x7Fu)) {
        out[0] = ch; /* identical in ASCII and GSM 03.38 */
        return 1u;
    }
    return 0u;
}

static bool append_address(uint8_t *pdu, size_t cap, size_t *pos,
                           const char *address, uint8_t toa) {
    const char *digits = address;
    if (*digits == '+') {
        digits++;
    }
    size_t count = strlen(digits);
    if (count == 0u || count > 20u) {
        return false;
    }
    for (size_t i = 0u; i < count; i++) {
        if (digits[i] < '0' || digits[i] > '9') {
            return false;
        }
    }
    size_t bytes = (count + 1u) / 2u;
    if (*pos + 2u + bytes > cap) {
        return false;
    }
    pdu[(*pos)++] = (uint8_t)count;
    pdu[(*pos)++] = toa;
    for (size_t i = 0u; i < count; i += 2u) {
        uint8_t low = (uint8_t)(digits[i] - '0');
        uint8_t high = (i + 1u < count) ? (uint8_t)(digits[i + 1u] - '0') : 0x0Fu;
        pdu[(*pos)++] = (uint8_t)((high << 4) | low);
    }
    return true;
}

bool sms_deliver_build(const sms_deliver_t *deliver, char *hex,
                       size_t hex_cap, uint8_t *out_tpdu_len) {
    if (deliver == NULL || hex == NULL || out_tpdu_len == NULL ||
        deliver->ud_len > SMS_DELIVER_UD_MAX) {
        return false;
    }
    uint8_t pdu[1u + 176u];
    size_t pos = 0u;
    pdu[pos++] = 0x00u; /* SMSC: use the module/SIM default. */
    pdu[pos++] = (uint8_t)(0x04u | (deliver->udhi ? 0x40u : 0x00u)); /* SMS-DELIVER, TP-MMS */
    uint8_t toa = deliver->toa;
    if (toa == 0u) {
        toa = deliver->address[0] == '+' ? 0x91u : 0x81u;
    }
    if (!append_address(pdu, sizeof(pdu), &pos, deliver->address, toa)) {
        return false;
    }
    if (pos + 2u + 7u + 1u + deliver->ud_len > sizeof(pdu)) {
        return false;
    }
    pdu[pos++] = deliver->pid;
    pdu[pos++] = deliver->dcs;
    memcpy(&pdu[pos], deliver->scts, 7u);
    pos += 7u;
    pdu[pos++] = deliver->udl;
    memcpy(&pdu[pos], deliver->ud, deliver->ud_len);
    pos += deliver->ud_len;

    static const char HEX[] = "0123456789ABCDEF";
    if (pos * 2u + 1u > hex_cap) {
        return false;
    }
    for (size_t i = 0u; i < pos; i++) {
        hex[i * 2u] = HEX[pdu[i] >> 4];
        hex[i * 2u + 1u] = HEX[pdu[i] & 0x0fu];
    }
    hex[pos * 2u] = '\0';
    *out_tpdu_len = (uint8_t)(pos - 1u);
    return true;
}
```

Add `src/services/sms_deliver_codec.c` to the firmware source list in `CMakeLists.txt` next to `src/services/sms_submit_codec.c`.

- [ ] **Step 4: Run tests**

Run: `tests/run_tests.sh 2>&1 | grep "test_sms_deliver_codec"`
Expected: `PASS: test_sms_deliver_codec`. Then `tests/run_tests.sh 2>&1 | tail -3` shows `fail=0`.

- [ ] **Step 5: Commit**

```bash
git add include/services/sms_deliver_codec.h src/services/sms_deliver_codec.c tests/test_sms_deliver_codec.c tests/run_tests.sh CMakeLists.txt
git commit -m "Add SMS-DELIVER TPDU builder

Claude-Session: https://claude.ai/code/session_01UeSArQWYHateziEWYPtoQn"
```

---

### Task 3: Generic `+CMT` collector and 3GPP parsers (`modem_sms_direct`)

**Files:**
- Create: `include/services/modem_sms_direct.h`
- Create: `src/services/modem_sms_direct.c`
- Create: `tests/test_modem_sms_direct.c`
- Modify: `tests/run_tests.sh` (register after `test_sms_deliver_codec`; add the new source to `MODEM_SERVICE_SOURCES`)
- Modify: `CMakeLists.txt` (add source)

**Interfaces:**
- Consumes: `sms_deliver_t`, `sms_deliver_build`, `sms_deliver_scts_encode`, `sms_deliver_pack_gsm7` (Task 2); `sms_pdu_decode` (existing).
- Produces:

```c
typedef enum {
    MODEM_SMS_DIRECT_NOT_MINE = 0,
    MODEM_SMS_DIRECT_ACCEPTED,
    MODEM_SMS_DIRECT_REJECTED,
} modem_sms_direct_translate_result_t;

/* Vendor hook: header is the +CMT line, payload the following line. */
typedef modem_sms_direct_translate_result_t (*modem_sms_direct_translate_fn)(
    const char *header, const char *payload, sms_deliver_t *out);

typedef enum {
    MODEM_SMS_DIRECT_STEP_IGNORED = 0, /* line is not part of a direct delivery */
    MODEM_SMS_DIRECT_STEP_HEADER,      /* header captured, payload expected next */
    MODEM_SMS_DIRECT_STEP_READY,       /* pdu_hex/tpdu_len filled */
    MODEM_SMS_DIRECT_STEP_REJECTED,    /* header+payload seen, no parser accepted */
} modem_sms_direct_step_t;

void modem_sms_direct_reset(void);
bool modem_sms_direct_pending(void);
modem_sms_direct_step_t modem_sms_direct_feed(
    const char *line, modem_sms_direct_translate_fn vendor,
    char *pdu_hex, size_t pdu_hex_cap, uint8_t *tpdu_len_out);

/* Generic 3GPP forms, exposed for tests. */
bool modem_sms_direct_parse_3gpp_pdu(const char *header, const char *payload,
                                     char *pdu_hex, size_t pdu_hex_cap,
                                     uint8_t *tpdu_len_out);
bool modem_sms_direct_parse_3gpp_text(const char *header, const char *payload,
                                      sms_deliver_t *out);
bool modem_sms_direct_hex_to_bytes(const char *hex, uint8_t *dst,
                                   size_t cap, size_t *out_len);
```

- [ ] **Step 1: Write the failing tests**

`tests/test_modem_sms_direct.c`:

```c
#include <stdio.h>
#include <string.h>

#include "services/modem_sms_direct.h"
#include "services/sms_picture_codec.h"

static int s_failures;
static void check(bool ok, const char *msg) {
    if (!ok) { printf("FAIL: %s\n", msg); s_failures++; }
}

static const char PDU_3GPP[] =
    "07919130079229F0040B918131926378F70000629061019569000744F57CAE56CF01";

static void test_pdu_form_passthrough(void) {
    char hex[SMS_DELIVER_HEX_MAX];
    uint8_t tpdu = 0u;
    check(modem_sms_direct_parse_3gpp_pdu("+CMT: ,26", PDU_3GPP, hex, sizeof(hex), &tpdu) &&
              tpdu == 26u && strcmp(hex, PDU_3GPP) == 0,
          "standard +CMT PDU form is stored verbatim");
    check(modem_sms_direct_parse_3gpp_pdu("+CMT: \"\",26", PDU_3GPP, hex, sizeof(hex), &tpdu),
          "quoted empty alpha accepted");
    check(!modem_sms_direct_parse_3gpp_pdu("+CMT: ,25", PDU_3GPP, hex, sizeof(hex), &tpdu),
          "length must match the TPDU length");
    check(!modem_sms_direct_parse_3gpp_pdu("+CMT: ,26", "ZZ", hex, sizeof(hex), &tpdu),
          "non-hex payload rejected");
    check(!modem_sms_direct_parse_3gpp_pdu("+CMT: \"7866910488\",\"\",25", PDU_3GPP,
                                           hex, sizeof(hex), &tpdu),
          "three-field header is not the 3GPP PDU form");
}

static void test_text_form_gsm7(void) {
    sms_deliver_t d;
    check(modem_sms_direct_parse_3gpp_text(
              "+CMT: \"+18132936877\",,\"26/09/16,10:59:04-16\",145,4,0,0,\"+19037029920\",145,7",
              "Djssjjs", &d),
          "3GPP text form parses");
    check(strcmp(d.address, "+18132936877") == 0 && d.toa == 0x91u, "address and TOA");
    check(d.scts[6] == 0x69u && d.scts[0] == 0x62u, "scts keeps the -16 zone");
    check(d.dcs == 0x00u && !d.udhi && d.udl == 7u && d.ud_len == 7u && d.ud[0] == 0x44u,
          "GSM text repacked into septets");
    check(!modem_sms_direct_parse_3gpp_text(
              "+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,8,9", "Dhdjdjdjs", &d),
          "eight-field 3GPP2 header is not the 3GPP text form");
}

static void test_text_form_udh_and_ucs2(void) {
    sms_deliver_t d;
    check(modem_sms_direct_parse_3gpp_text(
              "+CMT: \"7866910488\",,\"26/09/16,10:55:53+00\",129,68,0,0,\"+19037029920\",145,23",
              "050003620202D46435599D9EABE7EAB99AAC26ABC9", &d),
          "UDHI text form parses");
    check(d.udhi && d.udl == 23u && d.ud_len == 21u && d.ud[0] == 0x05u,
          "UDH hex body copied, UDL in septets");
    check(modem_sms_direct_parse_3gpp_text(
              "+CMT: \"7866910488\",,\"26/09/16,11:27:49+00\",129,4,0,8,\"+19037029920\",145,4",
              "D83DDE1C", &d),
          "UCS2 text form parses");
    check(d.dcs == 0x08u && d.udl == 4u && d.ud_len == 4u && d.ud[0] == 0xD8u,
          "UCS2 hex body copied");
}

static modem_sms_direct_translate_result_t fake_vendor(
    const char *header, const char *payload, sms_deliver_t *out) {
    if (strncmp(header, "+CMT: \"V\"", 9u) != 0) {
        return MODEM_SMS_DIRECT_NOT_MINE;
    }
    if (strcmp(payload, "bad") == 0) {
        return MODEM_SMS_DIRECT_REJECTED;
    }
    memset(out, 0, sizeof(*out));
    strcpy(out->address, "123");
    out->toa = 0x81u;
    out->dcs = 0x04u;
    out->ud[0] = 0x41u; out->ud_len = 1u; out->udl = 1u;
    return MODEM_SMS_DIRECT_ACCEPTED;
}

static void test_collector(void) {
    char hex[SMS_DELIVER_HEX_MAX];
    uint8_t tpdu = 0u;
    modem_sms_direct_reset();
    check(modem_sms_direct_feed("+CEREG: 1,1", fake_vendor, hex, sizeof(hex), &tpdu) ==
              MODEM_SMS_DIRECT_STEP_IGNORED && !modem_sms_direct_pending(),
          "unrelated URC ignored");
    check(modem_sms_direct_feed("+CMT: ,26", fake_vendor, hex, sizeof(hex), &tpdu) ==
              MODEM_SMS_DIRECT_STEP_HEADER && modem_sms_direct_pending(),
          "header captured");
    check(modem_sms_direct_feed(PDU_3GPP, fake_vendor, hex, sizeof(hex), &tpdu) ==
              MODEM_SMS_DIRECT_STEP_READY && tpdu == 26u && !modem_sms_direct_pending(),
          "payload completes the generic PDU form");
    check(modem_sms_direct_feed("+CMT: \"V\",1", fake_vendor, hex, sizeof(hex), &tpdu) ==
              MODEM_SMS_DIRECT_STEP_HEADER, "vendor header captured");
    check(modem_sms_direct_feed("OK", fake_vendor, hex, sizeof(hex), &tpdu) ==
              MODEM_SMS_DIRECT_STEP_READY && strncmp(hex, "0004", 4u) == 0,
          "vendor translation wins even when the payload looks like a final");
    check(modem_sms_direct_feed("+CMT: \"V\",1", fake_vendor, hex, sizeof(hex), &tpdu) ==
              MODEM_SMS_DIRECT_STEP_HEADER, "second vendor header");
    check(modem_sms_direct_feed("bad", fake_vendor, hex, sizeof(hex), &tpdu) ==
              MODEM_SMS_DIRECT_STEP_REJECTED && !modem_sms_direct_pending(),
          "vendor rejection surfaces as REJECTED and clears the pending state");
    check(modem_sms_direct_feed("+CMT: ,26", NULL, hex, sizeof(hex), &tpdu) ==
              MODEM_SMS_DIRECT_STEP_HEADER &&
              modem_sms_direct_feed("+CMT: ,26", NULL, hex, sizeof(hex), &tpdu) ==
              MODEM_SMS_DIRECT_STEP_HEADER,
          "a header while pending replaces the stale header (payload lost, not misparsed)");
    modem_sms_direct_reset();
    check(!modem_sms_direct_pending(), "reset clears pending");
}

static void test_hex_helper(void) {
    uint8_t buf[4];
    size_t len = 0u;
    check(modem_sms_direct_hex_to_bytes("d83dDE1C", buf, sizeof(buf), &len) && len == 4u &&
              buf[0] == 0xD8u && buf[3] == 0x1Cu, "mixed-case hex decodes");
    check(!modem_sms_direct_hex_to_bytes("D83", buf, sizeof(buf), &len), "odd length rejected");
    check(!modem_sms_direct_hex_to_bytes("D83DDE1C00", buf, sizeof(buf), &len), "capacity enforced");
}

int main(void) {
    test_pdu_form_passthrough();
    test_text_form_gsm7();
    test_text_form_udh_and_ucs2();
    test_collector();
    test_hex_helper();
    if (s_failures != 0) { printf("%d failures\n", s_failures); return 1; }
    printf("test_modem_sms_direct: all passed\n");
    return 0;
}
```

Register in `tests/run_tests.sh` after `test_sms_deliver_codec`:

```sh
run test_modem_sms_direct src/services/modem_sms_direct.c src/services/sms_deliver_codec.c \
    src/services/sms_picture_codec.c
```

and add `src/services/modem_sms_direct.c` and `src/services/sms_deliver_codec.c` to `MODEM_SERVICE_SOURCES` in the same file.

- [ ] **Step 2: Run test to verify it fails**

Run: `tests/run_tests.sh 2>&1 | grep -A4 test_modem_sms_direct`
Expected: `BUILD FAIL: test_modem_sms_direct`.

- [ ] **Step 3: Implement the collector**

`include/services/modem_sms_direct.h`:

```c
#ifndef MODEM_SMS_DIRECT_H
#define MODEM_SMS_DIRECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/sms_deliver_codec.h"

#define MODEM_SMS_DIRECT_LINE_MAX 400u

typedef enum {
    MODEM_SMS_DIRECT_NOT_MINE = 0,
    MODEM_SMS_DIRECT_ACCEPTED,
    MODEM_SMS_DIRECT_REJECTED,
} modem_sms_direct_translate_result_t;

typedef modem_sms_direct_translate_result_t (*modem_sms_direct_translate_fn)(
    const char *header, const char *payload, sms_deliver_t *out);

typedef enum {
    MODEM_SMS_DIRECT_STEP_IGNORED = 0,
    MODEM_SMS_DIRECT_STEP_HEADER,
    MODEM_SMS_DIRECT_STEP_READY,
    MODEM_SMS_DIRECT_STEP_REJECTED,
} modem_sms_direct_step_t;

void modem_sms_direct_reset(void);
bool modem_sms_direct_pending(void);
modem_sms_direct_step_t modem_sms_direct_feed(
    const char *line, modem_sms_direct_translate_fn vendor,
    char *pdu_hex, size_t pdu_hex_cap, uint8_t *tpdu_len_out);

bool modem_sms_direct_parse_3gpp_pdu(const char *header, const char *payload,
                                     char *pdu_hex, size_t pdu_hex_cap,
                                     uint8_t *tpdu_len_out);
bool modem_sms_direct_parse_3gpp_text(const char *header, const char *payload,
                                      sms_deliver_t *out);
bool modem_sms_direct_hex_to_bytes(const char *hex, uint8_t *dst,
                                   size_t cap, size_t *out_len);

#endif
```

`src/services/modem_sms_direct.c`:

```c
/* Direct SMS delivery collector (3GPP 27.005 +CMT, <mt>=2).
 *
 * A +CMT is two lines: the header and the payload. The collector remembers
 * the header, hands header+payload to the vendor hook first (module-specific
 * forms), then to the generic 3GPP text/PDU parsers, and returns a
 * SMS-DELIVER PDU ready for +CMGW. Pure: no I/O, no vendor names. */

#include "services/modem_sms_direct.h"

#include <stdlib.h>
#include <string.h>

#include "services/modem_at_text.h"   /* modem_at_starts_with */
#include "services/sms_picture_codec.h"

static char s_header[MODEM_SMS_DIRECT_LINE_MAX];
static bool s_pending;

void modem_sms_direct_reset(void) {
    s_pending = false;
    s_header[0] = '\0';
}

bool modem_sms_direct_pending(void) {
    return s_pending;
}

bool modem_sms_direct_hex_to_bytes(const char *hex, uint8_t *dst,
                                   size_t cap, size_t *out_len) {
    if (hex == NULL || dst == NULL || out_len == NULL) {
        return false;
    }
    size_t len = strlen(hex);
    if (len == 0u || (len & 1u) != 0u || len / 2u > cap) {
        return false;
    }
    for (size_t i = 0u; i < len; i += 2u) {
        uint8_t value = 0u;
        for (size_t k = 0u; k < 2u; k++) {
            char c = hex[i + k];
            uint8_t nibble;
            if (c >= '0' && c <= '9') nibble = (uint8_t)(c - '0');
            else if (c >= 'A' && c <= 'F') nibble = (uint8_t)(c - 'A' + 10);
            else if (c >= 'a' && c <= 'f') nibble = (uint8_t)(c - 'a' + 10);
            else return false;
            value = (uint8_t)((value << 4) | nibble);
        }
        dst[i / 2u] = value;
    }
    *out_len = len / 2u;
    return true;
}

/* Split "+CMT: a,b,c" into at most max fields; quotes are stripped and a
 * quoted field may contain commas. Returns the field count. */
static size_t split_header(const char *header, char fields[][48], size_t max) {
    const char *p = header + 5; /* "+CMT:" */
    while (*p == ' ') p++;
    size_t count = 0u;
    while (count < max) {
        size_t n = 0u;
        if (*p == '"') {
            p++;
            while (*p != '\0' && *p != '"') {
                if (n + 1u < 48u) fields[count][n++] = *p;
                p++;
            }
            if (*p == '"') p++;
        } else {
            while (*p != '\0' && *p != ',') {
                if (n + 1u < 48u) fields[count][n++] = *p;
                p++;
            }
        }
        fields[count][n] = '\0';
        count++;
        if (*p != ',') break;
        p++;
    }
    return count;
}

static bool parse_u32(const char *text, uint32_t max, uint32_t *out) {
    if (text == NULL || *text == '\0') return false;
    uint32_t value = 0u;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p < '0' || *p > '9' || value > max / 10u) return false;
        value = value * 10u + (uint32_t)(*p - '0');
    }
    if (value > max) return false;
    *out = value;
    return true;
}

bool modem_sms_direct_parse_3gpp_pdu(const char *header, const char *payload,
                                     char *pdu_hex, size_t pdu_hex_cap,
                                     uint8_t *tpdu_len_out) {
    if (header == NULL || payload == NULL || pdu_hex == NULL || tpdu_len_out == NULL ||
        !modem_at_starts_with(header, "+CMT:")) {
        return false;
    }
    char fields[10][48];
    size_t count = split_header(header, fields, 10u);
    uint32_t length = 0u;
    if (count != 2u || !parse_u32(fields[1], 176u, &length) || length == 0u) {
        return false;
    }
    uint8_t bytes[1u + 176u];
    size_t byte_len = 0u;
    if (!modem_sms_direct_hex_to_bytes(payload, bytes, sizeof(bytes), &byte_len) ||
        byte_len < 2u || (size_t)bytes[0] + 1u >= byte_len ||
        byte_len - 1u - bytes[0] != length) {
        return false;
    }
    sms_codec_message_t decoded;
    if (!sms_pdu_decode(payload, &decoded) || decoded.submit) {
        return false;
    }
    if (strlen(payload) + 1u > pdu_hex_cap) {
        return false;
    }
    strcpy(pdu_hex, payload);
    *tpdu_len_out = (uint8_t)length;
    return true;
}

/* "yy/MM/dd,hh:mm:ss+zz" -> scts semi-octets */
static bool parse_scts(const char *text, uint8_t out[7]) {
    unsigned yy, mo, dd, hh, mi, ss, zz;
    char sign;
    if (sscanf(text, "%2u/%2u/%2u,%2u:%2u:%2u%c%2u",
               &yy, &mo, &dd, &hh, &mi, &ss, &sign, &zz) != 8 ||
        (sign != '+' && sign != '-') || zz > 79u) {
        return false;
    }
    int8_t quarters = (int8_t)(sign == '-' ? -(int)zz : (int)zz);
    return sms_deliver_scts_encode((uint16_t)(2000u + yy), (uint8_t)mo, (uint8_t)dd,
                                   (uint8_t)hh, (uint8_t)mi, (uint8_t)ss, quarters, out);
}

bool modem_sms_direct_parse_3gpp_text(const char *header, const char *payload,
                                      sms_deliver_t *out) {
    if (header == NULL || payload == NULL || out == NULL ||
        !modem_at_starts_with(header, "+CMT:")) {
        return false;
    }
    char fields[10][48];
    size_t count = split_header(header, fields, 10u);
    /* <oa>,<alpha>,<scts>,<tooa>,<fo>,<pid>,<dcs>,<sca>,<tosca>,<length> */
    uint32_t tooa, fo, pid, dcs, length;
    if (count != 10u || strchr(fields[2], '/') == NULL ||
        !parse_u32(fields[3], 255u, &tooa) || !parse_u32(fields[4], 255u, &fo) ||
        !parse_u32(fields[5], 255u, &pid) || !parse_u32(fields[6], 255u, &dcs) ||
        !parse_u32(fields[9], 255u, &length)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (strlen(fields[0]) > SMS_CODEC_ADDRESS_MAX) {
        return false;
    }
    strcpy(out->address, fields[0]);
    out->toa = (uint8_t)tooa;
    if (!parse_scts(fields[2], out->scts)) {
        return false;
    }
    out->pid = (uint8_t)pid;
    out->dcs = (uint8_t)dcs;
    out->udhi = (fo & 0x40u) != 0u;
    out->udl = (uint8_t)length;
    bool seven_bit = (dcs & 0x0Cu) == 0x00u && (dcs & 0xF0u) != 0xF0u
                     ? true : ((dcs & 0xF4u) == 0xF0u);
    bool hex_body = out->udhi || !seven_bit;
    if (hex_body) {
        size_t n = 0u;
        if (!modem_sms_direct_hex_to_bytes(payload, out->ud, sizeof(out->ud), &n)) {
            return false;
        }
        out->ud_len = (uint8_t)n;
        return true;
    }
    size_t chars = strlen(payload);
    if (chars != length || chars > 160u) {
        return false;
    }
    if (chars == 0u) {
        out->ud_len = 0u;
        return true;
    }
    return sms_deliver_pack_gsm7((const uint8_t *)payload, (uint8_t)chars,
                                 out->ud, sizeof(out->ud), &out->ud_len);
}

modem_sms_direct_step_t modem_sms_direct_feed(
    const char *line, modem_sms_direct_translate_fn vendor,
    char *pdu_hex, size_t pdu_hex_cap, uint8_t *tpdu_len_out) {
    if (line == NULL || pdu_hex == NULL || tpdu_len_out == NULL) {
        return MODEM_SMS_DIRECT_STEP_IGNORED;
    }
    if (modem_at_starts_with(line, "+CMT:")) {
        strncpy(s_header, line, sizeof(s_header) - 1u);
        s_header[sizeof(s_header) - 1u] = '\0';
        s_pending = true;
        return MODEM_SMS_DIRECT_STEP_HEADER;
    }
    if (!s_pending) {
        return MODEM_SMS_DIRECT_STEP_IGNORED;
    }
    s_pending = false;

    sms_deliver_t deliver;
    if (vendor != NULL) {
        modem_sms_direct_translate_result_t result = vendor(s_header, line, &deliver);
        if (result == MODEM_SMS_DIRECT_ACCEPTED) {
            return sms_deliver_build(&deliver, pdu_hex, pdu_hex_cap, tpdu_len_out)
                       ? MODEM_SMS_DIRECT_STEP_READY : MODEM_SMS_DIRECT_STEP_REJECTED;
        }
        if (result == MODEM_SMS_DIRECT_REJECTED) {
            return MODEM_SMS_DIRECT_STEP_REJECTED;
        }
    }
    if (modem_sms_direct_parse_3gpp_pdu(s_header, line, pdu_hex, pdu_hex_cap, tpdu_len_out)) {
        return MODEM_SMS_DIRECT_STEP_READY;
    }
    if (modem_sms_direct_parse_3gpp_text(s_header, line, &deliver) &&
        sms_deliver_build(&deliver, pdu_hex, pdu_hex_cap, tpdu_len_out)) {
        return MODEM_SMS_DIRECT_STEP_READY;
    }
    return MODEM_SMS_DIRECT_STEP_REJECTED;
}
```

If `services/modem_at_text.h` does not exist, use the header that declares `modem_at_starts_with` (grep `modem_at_starts_with` in `include/services/`). `sscanf` needs `#include <stdio.h>`.

Add `src/services/modem_sms_direct.c` to `CMakeLists.txt` next to `modem_sms_protocol.c`.

- [ ] **Step 4: Run tests**

Run: `tests/run_tests.sh 2>&1 | grep -E "test_modem_sms_direct|fail="`
Expected: `PASS: test_modem_sms_direct`, `fail=0`.

- [ ] **Step 5: Commit**

```bash
git add include/services/modem_sms_direct.h src/services/modem_sms_direct.c tests/test_modem_sms_direct.c tests/run_tests.sh CMakeLists.txt
git commit -m "Add generic +CMT direct-delivery collector

Claude-Session: https://claude.ai/code/session_01UeSArQWYHateziEWYPtoQn"
```

---

### Task 4: Telit 3GPP2 translation hook

**Files:**
- Create: `src/services/modem_vendor_telit_sms.c`
- Modify: `include/services/modem_vendor.h:282-331` (new member) — or `src/services/modem_vendor.h` if that is where `modem_vendor_t` lives; use the path `grep -rn "typedef struct modem_vendor" include src` prints
- Modify: `src/services/modem_vendor_telit_internal.h` (declare `telit_translate_direct_sms`)
- Modify: `src/services/modem_vendor_telit.c:239-243` (register hook)
- Modify: `src/services/modem_vendor_none.c:76-78` (`.translate_direct_sms = NULL`)
- Modify: `tests/run_tests.sh:37-45` (`MODEM_TELIT_VENDOR_SOURCES`), `CMakeLists.txt` (source)
- Test: `tests/test_modem_vendor_telit.c`

**Interfaces:**
- Consumes: `modem_sms_direct_translate_fn`, `sms_deliver_t`, `sms_deliver_scts_encode`, `sms_deliver_pack_gsm7`, `sms_deliver_gsm7_from_latin1`, `modem_sms_direct_hex_to_bytes`.
- Produces: `modem_sms_direct_translate_result_t telit_translate_direct_sms(const char *header, const char *payload, sms_deliver_t *out);` and `g_modem_vendor.translate_direct_sms`.

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_modem_vendor_telit.c`:

```c
#include "services/modem_sms_direct.h"
#include "services/sms_picture_codec.h"

static bool build_and_decode(const sms_deliver_t *d, sms_codec_message_t *decoded) {
    char hex[SMS_DELIVER_HEX_MAX];
    uint8_t tpdu = 0u;
    return sms_deliver_build(d, hex, sizeof(hex), &tpdu) && sms_pdu_decode(hex, decoded);
}

static void test_direct_3gpp2_text_forms(void) {
    sms_deliver_t d;
    sms_codec_message_t m;
    check(telit_translate_direct_sms(
              "+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,8,9",
              "Dhdjdjdjs", &d) == MODEM_SMS_DIRECT_ACCEPTED,
          "3GPP2 text, Latin-1 body accepted");
    check(build_and_decode(&d, &m) && strcmp(m.address, "7866910488") == 0 &&
              strcmp(m.text, "Dhdjdjdjs") == 0 &&
              strcmp(m.timestamp, "26/09/16,10:55:24") == 0,
          "Latin-1 body re-encoded as GSM-7 with the delivery time");

    check(telit_translate_direct_sms(
              "+CMT: \"7866910488\",\"\",\"20260916105553\",129,4101,0,9,23",
              "050003620202D46435599D9EABE7EAB99AAC26ABC9", &d) == MODEM_SMS_DIRECT_ACCEPTED,
          "WEMT GSM-7 hex body accepted");
    check(d.udhi && d.udl == 23u && d.ud_len == 21u, "UDH kept, UDL in septets");
    check(build_and_decode(&d, &m) && m.has_concat && m.concat_ref == 0x62u &&
              m.concat_total == 2u && m.concat_seq == 2u,
          "concatenation survives");

    check(telit_translate_direct_sms(
              "+CMT: \"7866910488\",\"\",\"20260916105559\",129,4098,0,4,2",
              "D83EDD2A", &d) == MODEM_SMS_DIRECT_ACCEPTED &&
              d.dcs == 0x08u && d.udl == 4u && d.ud_len == 4u,
          "Unicode body -> UCS2 DCS");
    check(build_and_decode(&d, &m) && strcmp(m.text, "\xF0\x9F\xA4\xAA") == 0,
          "emoji decodes as UTF-8");

    check(telit_translate_direct_sms(
              "+CMT: \"7866910488\",\"\",\"20260916105559\",129,4099,0,8,1",
              "3", &d) == MODEM_SMS_DIRECT_REJECTED,
          "voice mail notification teleservice rejected");
    check(telit_translate_direct_sms(
              "+CMT: \"+18132936877\",,\"26/09/16,10:59:04-16\",145,4,0,0,\"+19037029920\",145,7",
              "Djssjjs", &d) == MODEM_SMS_DIRECT_NOT_MINE,
          "3GPP text form is not claimed");
    check(telit_translate_direct_sms("+CMT: ,26", "07", &d) == MODEM_SMS_DIRECT_NOT_MINE,
          "3GPP PDU form is not claimed");
}

static void test_direct_3gpp2_pdu_forms(void) {
    sms_deliver_t d;
    sms_codec_message_t m;
    check(telit_translate_direct_sms("+CMT: \"7866910488\",\"\",25",
              "068187661940882609161059281002000807446A73736A6A73", &d) ==
              MODEM_SMS_DIRECT_ACCEPTED,
          "3GPP2 PDU form accepted");
    check(build_and_decode(&d, &m) && strcmp(m.address, "7866910488") == 0 &&
              strcmp(m.text, "Djssjjs") == 0 &&
              strcmp(m.timestamp, "26/09/16,10:59:28") == 0,
          "PDU-form fields map to a DELIVER");
    check(telit_translate_direct_sms("+CMT: \"7866910488\",\"\",22",
              "068187661940882609161127491002000404D83DDE1C", &d) ==
              MODEM_SMS_DIRECT_ACCEPTED && d.dcs == 0x08u && d.ud_len == 4u,
          "PDU-form Unicode");
    check(telit_translate_direct_sms("+CMT: \"7866910488\",\"\",32",
              "06818766194088260916112747100500090F6A721A4D4693D56435594D469301", &d) ==
              MODEM_SMS_DIRECT_ACCEPTED && !d.udhi && d.udl == 15u && d.ud_len == 14u,
          "PDU-form GSM-7 (UDH stripped by the module) accepted as a plain part");
    check(build_and_decode(&d, &m) && strcmp(m.text, "jdihdhdjdjdjdhd") == 0,
          "PDU-form GSM-7 decodes");
    check(telit_translate_direct_sms("+CMT: \"7866910488\",\"\",25",
              "068187661940882609161059281002000807446A73736A6A", &d) ==
              MODEM_SMS_DIRECT_REJECTED,
          "short payload rejected");
}
```

Register both in `main`.

- [ ] **Step 2: Run test to verify it fails**

Run: `tests/run_tests.sh 2>&1 | grep -A4 "test_modem_vendor_telit$"`
Expected: `BUILD FAIL` (undeclared `telit_translate_direct_sms`).

- [ ] **Step 3: Implement the hook**

Add to `modem_vendor_t` (after `parse_aux_urc`):

```c
    /* Optional: translate a module-specific +CMT header/payload pair into a
     * neutral SMS-DELIVER. Return NOT_MINE for standard 3GPP forms. Pure. */
    modem_sms_direct_translate_fn translate_direct_sms;
```

with `#include "services/modem_sms_direct.h"` at the top of the vendor header. In `modem_vendor_none.c` add `.translate_direct_sms = NULL,`; in `modem_vendor_telit.c` add `.translate_direct_sms = telit_translate_direct_sms,`. Declare in `modem_vendor_telit_internal.h`:

```c
#include "services/modem_sms_direct.h"
modem_sms_direct_translate_result_t telit_translate_direct_sms(
    const char *header, const char *payload, sms_deliver_t *out);
```

`src/services/modem_vendor_telit_sms.c`:

```c
/* Telit LE910Cx direct-delivery forms for 3GPP2 (CDMA-style) SMS.
 *
 * The Verizon image delivers SMS over IMS in 3GPP2 form. With +CNMI=2,2 the
 * module hands them over as +CMT in a Telit-specific layout (LE910 V2 AT
 * guide, "Message Sending And Writing (3GPP2 mode)"), bench-verified on the
 * WWX 2026-09-16. Both forms are translated into a neutral SMS-DELIVER; the
 * generic service never sees teleservice ids or CDMA encodings.
 *
 * Text form (+CSDH=1):
 *   +CMT: "<orig>","<callback>","<YYYYMMDDHHMMSS>",<tooa>,<tele_id>,<priority>,<enc>,<length>
 * PDU form (+CMGF=0):
 *   +CMT: "<orig>","<callback>",<len>
 *   <addr_len><toa><bcd...><YYMMDDhhmmss><tele_id:2><priority><enc><data_len><data>
 */

#include "modem_vendor_telit_internal.h"

#include <string.h>

#include "services/modem_sms_direct.h"

#define TELIT_TELE_WMT 4098u            /* CDMA messaging teleservice */
#define TELIT_TELE_WEMT 4101u           /* enhanced messaging: UDH present */
#define TELIT_TELE_VMN 4099u            /* voice mail notification */
#define TELIT_TELE_VMN_ALT 262144u

enum { ENC_OCTET = 0u, ENC_ASCII = 2u, ENC_IA5 = 3u, ENC_UNICODE = 4u,
       ENC_LATIN1 = 8u, ENC_GSM7 = 9u };

static bool digits_to_scts(const char *yyyymmddhhmmss, uint8_t out[7]) {
    if (strlen(yyyymmddhhmmss) != 14u) {
        return false;
    }
    unsigned v[7];
    const char *p = yyyymmddhhmmss;
    v[0] = (unsigned)((p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0'));
    for (size_t i = 1u; i < 7u; i++) {
        v[i] = (unsigned)((p[2u + i * 2u] - '0') * 10 + (p[3u + i * 2u] - '0'));
    }
    return sms_deliver_scts_encode((uint16_t)v[0], (uint8_t)v[1], (uint8_t)v[2],
                                   (uint8_t)v[3], (uint8_t)v[4], (uint8_t)v[5], 0, out);
}

/* Latin-1/ASCII text -> GSM-7 packed; falls back to UCS2 when unmappable. */
static bool text_to_user_data(const uint8_t *bytes, size_t len, bool already_gsm,
                              sms_deliver_t *out) {
    uint8_t codes[160];
    size_t n = 0u;
    bool mappable = true;
    for (size_t i = 0u; i < len && mappable; i++) {
        uint8_t pair[2];
        uint8_t got = already_gsm ? 1u : sms_deliver_gsm7_from_latin1(bytes[i], pair);
        if (already_gsm) {
            pair[0] = bytes[i];
        }
        if (got == 0u || n + got > sizeof(codes)) {
            mappable = false;
            break;
        }
        memcpy(&codes[n], pair, got);
        n += got;
    }
    if (mappable) {
        out->dcs = 0x00u;
        out->udl = (uint8_t)n;
        if (n == 0u) {
            out->ud_len = 0u;
            return true;
        }
        return sms_deliver_pack_gsm7(codes, (uint8_t)n, out->ud, sizeof(out->ud),
                                     &out->ud_len);
    }
    if (len * 2u > SMS_DELIVER_UD_MAX) {
        return false;
    }
    out->dcs = 0x08u;
    for (size_t i = 0u; i < len; i++) {
        out->ud[i * 2u] = 0x00u;
        out->ud[i * 2u + 1u] = bytes[i];
    }
    out->ud_len = (uint8_t)(len * 2u);
    out->udl = out->ud_len;
    return true;
}

static bool apply_encoding(uint32_t tele_id, uint32_t enc, uint32_t length,
                           const char *payload, bool text_form, sms_deliver_t *out) {
    size_t n = 0u;
    switch (enc) {
    case ENC_GSM7: {
        size_t packed = ((size_t)length * 7u + 7u) / 8u;
        if (text_form && strlen(payload) == length && strlen(payload) != packed * 2u) {
            /* plain GSM-charset text without UDH */
            return text_to_user_data((const uint8_t *)payload, length, true, out);
        }
        if (!modem_sms_direct_hex_to_bytes(payload, out->ud, sizeof(out->ud), &n)) {
            return false;
        }
        out->udhi = text_form && tele_id == TELIT_TELE_WEMT;
        out->dcs = 0x00u;
        out->udl = (uint8_t)length;
        out->ud_len = (uint8_t)n;
        return n == packed;
    }
    case ENC_UNICODE:
        if (!modem_sms_direct_hex_to_bytes(payload, out->ud, sizeof(out->ud), &n) ||
            n != (size_t)length * 2u) {
            return false;
        }
        out->dcs = 0x08u;
        out->udl = (uint8_t)n;
        out->ud_len = (uint8_t)n;
        return true;
    case ENC_OCTET:
        if (!modem_sms_direct_hex_to_bytes(payload, out->ud, sizeof(out->ud), &n) ||
            n != length) {
            return false;
        }
        out->dcs = 0x04u;
        out->udl = (uint8_t)n;
        out->ud_len = (uint8_t)n;
        return true;
    case ENC_LATIN1:
    case ENC_ASCII:
    case ENC_IA5:
        if (text_form) {
            /* The module already mapped the text through +CSCS="GSM". */
            return strlen(payload) <= 160u &&
                   text_to_user_data((const uint8_t *)payload, strlen(payload), true, out);
        }
        return false; /* PDU form handles bytes in the caller */
    default:
        return false;
    }
}

static modem_sms_direct_translate_result_t translate_text_form(
    telit_csv_view_t fields[8], const char *payload, sms_deliver_t *out) {
    uint32_t tooa, tele_id, priority, enc, length;
    if (!telit_view_parse_u32(fields[3], 255u, &tooa) ||
        !telit_view_parse_u32(fields[4], 262144u, &tele_id) ||
        !telit_view_parse_u32(fields[5], 3u, &priority) ||
        !telit_view_parse_u32(fields[6], 9u, &enc) ||
        !telit_view_parse_u32(fields[7], 255u, &length)) {
        return MODEM_SMS_DIRECT_REJECTED;
    }
    (void)priority;
    if (tele_id == TELIT_TELE_VMN || tele_id == TELIT_TELE_VMN_ALT) {
        return MODEM_SMS_DIRECT_REJECTED;
    }
    memset(out, 0, sizeof(*out));
    char date[15];
    if (!telit_view_copy_exact(out->address, sizeof(out->address), fields[0]) ||
        !telit_view_copy_exact(date, sizeof(date), fields[2]) ||
        !digits_to_scts(date, out->scts)) {
        return MODEM_SMS_DIRECT_REJECTED;
    }
    out->toa = (uint8_t)tooa;
    return apply_encoding(tele_id, enc, length, payload, true, out)
               ? MODEM_SMS_DIRECT_ACCEPTED : MODEM_SMS_DIRECT_REJECTED;
}

static modem_sms_direct_translate_result_t translate_pdu_form(
    telit_csv_view_t fields[3], const char *payload, sms_deliver_t *out) {
    uint32_t length;
    if (!telit_view_parse_u32(fields[2], 255u, &length)) {
        return MODEM_SMS_DIRECT_REJECTED;
    }
    uint8_t bytes[256];
    size_t n = 0u;
    if (!modem_sms_direct_hex_to_bytes(payload, bytes, sizeof(bytes), &n) || n != length ||
        n < 13u) {
        return MODEM_SMS_DIRECT_REJECTED;
    }
    size_t pos = 0u;
    uint8_t addr_bytes = bytes[pos++];
    uint8_t toa = bytes[pos++];
    if (addr_bytes == 0u || addr_bytes > 10u || pos + addr_bytes + 11u > n) {
        return MODEM_SMS_DIRECT_REJECTED;
    }
    memset(out, 0, sizeof(*out));
    size_t d = 0u;
    for (uint8_t i = 0u; i < addr_bytes; i++) {
        uint8_t low = (uint8_t)(bytes[pos + i] & 0x0fu);
        uint8_t high = (uint8_t)(bytes[pos + i] >> 4);
        out->address[d++] = (char)('0' + low);
        if (high != 0x0fu) {
            out->address[d++] = (char)('0' + high);
        }
    }
    out->address[d] = '\0';
    pos += addr_bytes;
    out->toa = toa;
    char date[15];
    for (size_t i = 0u; i < 6u; i++) {
        uint8_t b = bytes[pos + i];
        char *at = &date[i == 0u ? 2u : 2u + i * 2u];
        at[0] = (char)('0' + (b >> 4));
        at[1] = (char)('0' + (b & 0x0fu));
    }
    date[0] = '2'; date[1] = '0'; date[14] = '\0';
    pos += 6u;
    uint32_t tele_id = (uint32_t)((bytes[pos] << 8) | bytes[pos + 1u]);
    pos += 2u;
    pos++; /* priority */
    uint8_t enc = bytes[pos++];
    uint8_t data_len = bytes[pos++];
    if (tele_id == TELIT_TELE_VMN || tele_id == TELIT_TELE_VMN_ALT ||
        !digits_to_scts(date, out->scts)) {
        return MODEM_SMS_DIRECT_REJECTED;
    }
    const uint8_t *data = &bytes[pos];
    size_t avail = n - pos;
    if (enc == ENC_LATIN1 || enc == ENC_ASCII || enc == ENC_IA5) {
        return avail == data_len && text_to_user_data(data, data_len, false, out)
                   ? MODEM_SMS_DIRECT_ACCEPTED : MODEM_SMS_DIRECT_REJECTED;
    }
    /* GSM-7 / Unicode / octet: re-hex the remaining bytes for the shared path. */
    char hex[2u * SMS_DELIVER_UD_MAX + 1u];
    if (avail > SMS_DELIVER_UD_MAX) {
        return MODEM_SMS_DIRECT_REJECTED;
    }
    static const char HEX[] = "0123456789ABCDEF";
    for (size_t i = 0u; i < avail; i++) {
        hex[i * 2u] = HEX[data[i] >> 4];
        hex[i * 2u + 1u] = HEX[data[i] & 0x0fu];
    }
    hex[avail * 2u] = '\0';
    /* PDU-form <data_len> is a byte count for Unicode; the text-form <length>
     * (and apply_encoding) count UTF-16 code units. GSM-7 counts septets in
     * both forms. */
    uint32_t length = enc == ENC_UNICODE ? (uint32_t)data_len / 2u : data_len;
    return apply_encoding(tele_id, enc, length, hex, false, out)
               ? MODEM_SMS_DIRECT_ACCEPTED : MODEM_SMS_DIRECT_REJECTED;
}

modem_sms_direct_translate_result_t telit_translate_direct_sms(
    const char *header, const char *payload, sms_deliver_t *out) {
    if (header == NULL || payload == NULL || out == NULL) {
        return MODEM_SMS_DIRECT_NOT_MINE;
    }
    telit_csv_view_t fields[10];
    size_t count = 0u;
    if (!telit_view_split_prefixed(header, "+CMT:", fields, 10u, &count)) {
        return MODEM_SMS_DIRECT_NOT_MINE;
    }
    /* 3GPP2 text: 8 fields with a 14-digit date; 3GPP2 PDU: 3 fields. The
     * standard 3GPP forms have 2 (PDU) or 10 (text, +CSDH=1) fields. */
    if (count == 8u && telit_view_digits_only(fields[2], 14u, 14u)) {
        return translate_text_form(fields, payload, out);
    }
    if (count == 3u) {
        return translate_pdu_form(fields, payload, out);
    }
    return MODEM_SMS_DIRECT_NOT_MINE;
}
```

Check the exact signatures of `telit_view_split_prefixed`, `telit_view_copy_exact`, `telit_view_digits_only` and `telit_view_parse_u32` in `modem_vendor_telit_internal.h` (they strip quotes for quoted fields; if `telit_view_split_prefixed` keeps quotes, strip them with `telit_view_copy_exact` semantics as the existing `#RFSTS` parser in `modem_vendor_telit_parse.c` does). Add `src/services/modem_vendor_telit_sms.c` to `MODEM_TELIT_VENDOR_SOURCES` in `tests/run_tests.sh` and to `CMakeLists.txt` next to `modem_vendor_telit_tune.c`.

- [ ] **Step 4: Run tests**

Run: `tests/run_tests.sh 2>&1 | grep -E "test_modem_vendor_telit|test_modem_none_service|fail="`
Expected: all PASS, `fail=0`.

- [ ] **Step 5: Commit**

```bash
git add src/services/modem_vendor_telit_sms.c src/services/modem_vendor_telit_internal.h src/services/modem_vendor_telit.c src/services/modem_vendor_none.c include/services/modem_vendor.h tests/test_modem_vendor_telit.c tests/run_tests.sh CMakeLists.txt
git commit -m "Translate Telit 3GPP2 direct-delivery forms into SMS-DELIVER

Claude-Session: https://claude.ai/code/session_01UeSArQWYHateziEWYPtoQn"
```

---

### Task 5: `STORE_DELIVERED` protocol operation

**Files:**
- Modify: `src/services/modem_sms_protocol_internal.h:11-18` (operation enum), `:44-60` (request fields), action enum + union
- Modify: `src/services/modem_sms_protocol.c` (`begin`, `resume_after_wake`, `parse_line`, `on_prompt`, `on_final`, `on_timeout`)
- Test: `tests/test_modem_sms_protocol.c`

**Interfaces:**
- Consumes: nothing new.
- Produces:
  - `MODEM_SMS_PROTOCOL_STORE_DELIVERED` operation; request fields `const char *pdu_hex; uint8_t tpdu_len;`
  - `MODEM_SMS_ACTION_PUBLISH_DELIVERED` with `data.delivered = { uint16_t index; modem_sms_outcome_t outcome; }`
  - Command sequence: `AT+CPMS=...` → wake continuation → `AT+CMGF=0` → `AT+CMGW=<tpdu_len>,0` (prompt) → body `<pdu_hex>` + SUB → `+CMGW: <idx>` → `AT+CMGF=1` → PUBLISH_DELIVERED → COMPLETE.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_modem_sms_protocol.c` (mirror the style of `test_text_send_and_sent_copy_failure`; use the existing `capture_emit`, `clear_actions`, `s_actions`, `mh`-free helpers in that file):

```c
static void test_store_delivered_sequence(void) {
    clear_actions();
    s_sim_ready = true;
    modem_sms_protocol_init();
    modem_sms_protocol_request_t request;
    memset(&request, 0, sizeof(request));
    request.request_id = 77u;
    request.operation = MODEM_SMS_PROTOCOL_STORE_DELIVERED;
    request.pdu_hex = "00040A8187661940880000629061019582000744F57CAE56CF01";
    request.tpdu_len = 25u;

    check(modem_sms_protocol_begin(&request, &s_hooks, 1000u), "begin queues CPMS");
    check(last_command_is(MODEM_SMS_COMMAND_CPMS, "AT+CPMS=\"ME\",\"ME\",\"ME\""), "CPMS first");
    modem_sms_protocol_on_final(MODEM_SMS_COMMAND_CPMS, true, &request, &s_hooks, 1010u);
    check(last_action_type_is(MODEM_SMS_ACTION_REQUEST_WAKE_CONTINUATION), "wake continuation");
    modem_sms_protocol_resume_after_wake(&request, &s_hooks, 1020u);
    check(last_command_is(MODEM_SMS_COMMAND_CMGF_PDU, "AT+CMGF=0"), "PDU mode before CMGW");
    modem_sms_protocol_on_final(MODEM_SMS_COMMAND_CMGF_PDU, true, &request, &s_hooks, 1030u);
    check(last_command_is(MODEM_SMS_COMMAND_CMGW_PROMPT, "AT+CMGW=25,0"),
          "CMGW with the TPDU length and REC UNREAD status");
    check(modem_sms_protocol_on_prompt(MODEM_SMS_COMMAND_CMGW_PROMPT, &request, &s_hooks, 1040u),
          "prompt accepted");
    check(last_body_is(request.pdu_hex), "body is the PDU hex");
    check(modem_sms_protocol_parse_line(MODEM_SMS_COMMAND_CMGW_FINAL, "+CMGW: 7", false, false,
                                        &request, &s_hooks),
          "+CMGW index line consumed");
    modem_sms_protocol_on_final(MODEM_SMS_COMMAND_CMGW_FINAL, true, &request, &s_hooks, 1050u);
    check(last_command_is(MODEM_SMS_COMMAND_CMGF_TEXT, "AT+CMGF=1"), "text mode restored");
    modem_sms_protocol_on_final(MODEM_SMS_COMMAND_CMGF_TEXT, true, &request, &s_hooks, 1060u);
    check(action_seen(MODEM_SMS_ACTION_PUBLISH_DELIVERED) &&
              delivered_action_index() == 7u &&
              delivered_action_outcome() == MODEM_SMS_OUTCOME_OK,
          "delivered index published");
    check(last_action_type_is(MODEM_SMS_ACTION_COMPLETE) && last_complete_ok(),
          "operation completes ok");
}

static void test_store_delivered_failures(void) {
    modem_sms_protocol_request_t request;
    memset(&request, 0, sizeof(request));
    request.request_id = 78u;
    request.operation = MODEM_SMS_PROTOCOL_STORE_DELIVERED;
    request.pdu_hex = "00040A8187661940880000629061019582000744F57CAE56CF01";
    request.tpdu_len = 25u;

    /* CMGW rejected: restore text mode, publish error, complete. */
    clear_actions();
    modem_sms_protocol_init();
    (void)modem_sms_protocol_begin(&request, &s_hooks, 1000u);
    modem_sms_protocol_on_final(MODEM_SMS_COMMAND_CPMS, true, &request, &s_hooks, 1010u);
    modem_sms_protocol_resume_after_wake(&request, &s_hooks, 1020u);
    modem_sms_protocol_on_final(MODEM_SMS_COMMAND_CMGF_PDU, true, &request, &s_hooks, 1030u);
    modem_sms_protocol_on_final(MODEM_SMS_COMMAND_CMGW_PROMPT, false, &request, &s_hooks, 1040u);
    check(last_command_is(MODEM_SMS_COMMAND_CMGF_TEXT, "AT+CMGF=1"), "text restored after CMGW error");
    modem_sms_protocol_on_final(MODEM_SMS_COMMAND_CMGF_TEXT, true, &request, &s_hooks, 1050u);
    check(delivered_action_outcome() == MODEM_SMS_OUTCOME_ERROR, "error published");
    check(last_action_type_is(MODEM_SMS_ACTION_COMPLETE), "completes");

    /* Prompt timeout: ESC abort, then restore text mode. */
    clear_actions();
    modem_sms_protocol_init();
    (void)modem_sms_protocol_begin(&request, &s_hooks, 2000u);
    modem_sms_protocol_on_final(MODEM_SMS_COMMAND_CPMS, true, &request, &s_hooks, 2010u);
    modem_sms_protocol_resume_after_wake(&request, &s_hooks, 2020u);
    modem_sms_protocol_on_final(MODEM_SMS_COMMAND_CMGF_PDU, true, &request, &s_hooks, 2030u);
    modem_sms_protocol_on_timeout(MODEM_SMS_COMMAND_CMGW_PROMPT, &request, &s_hooks, 7040u);
    check(action_seen(MODEM_SMS_ACTION_ABORT_PROMPT), "prompt timeout aborts the prompt");
    modem_sms_protocol_resume_after_prompt_abort(&request, &s_hooks, 7100u);
    check(last_command_is(MODEM_SMS_COMMAND_CMGF_TEXT, "AT+CMGF=1"), "text restored after abort");
    modem_sms_protocol_on_final(MODEM_SMS_COMMAND_CMGF_TEXT, true, &request, &s_hooks, 7110u);
    check(delivered_action_outcome() == MODEM_SMS_OUTCOME_TIMEOUT, "timeout published");

    /* CPMS failure: error without touching CMGF. */
    clear_actions();
    modem_sms_protocol_init();
    (void)modem_sms_protocol_begin(&request, &s_hooks, 3000u);
    modem_sms_protocol_on_final(MODEM_SMS_COMMAND_CPMS, false, &request, &s_hooks, 3010u);
    check(delivered_action_outcome() == MODEM_SMS_OUTCOME_ERROR &&
              last_action_type_is(MODEM_SMS_ACTION_COMPLETE),
          "CPMS failure publishes error and completes");
}
```

Add the small helpers the test uses if they do not already exist in that file (`last_command_is(kind, text)`, `last_action_type_is(type)`, `action_seen(type)`, `last_body_is(text)`, `last_complete_ok()`, `delivered_action_index()`, `delivered_action_outcome()`), each scanning `s_actions[0..s_action_count)`; `delivered_action_*` read the last `MODEM_SMS_ACTION_PUBLISH_DELIVERED` entry. Register both tests in `main`.

- [ ] **Step 2: Run test to verify it fails**

Run: `tests/run_tests.sh 2>&1 | grep -A4 test_modem_sms_protocol`
Expected: `BUILD FAIL` (unknown operation/fields).

- [ ] **Step 3: Implement the operation**

`modem_sms_protocol_internal.h`:

```c
    MODEM_SMS_PROTOCOL_DELETE,
    MODEM_SMS_PROTOCOL_STORE_DELIVERED, /* re-store a +CMT as a REC UNREAD PDU */
} modem_sms_protocol_operation_t;
```

request struct, after `read_status`:

```c
    const char *pdu_hex;  /* STORE_DELIVERED: SMSC-prefixed SMS-DELIVER hex */
    uint8_t tpdu_len;     /* STORE_DELIVERED: +CMGW length argument */
```

action enum: add `MODEM_SMS_ACTION_PUBLISH_DELIVERED,` before `MODEM_SMS_ACTION_COMPLETE`; action union: add

```c
        struct {
            uint16_t index;
            modem_sms_outcome_t outcome;
        } delivered;
```

protocol state (`s_protocol`): add `uint16_t stored_index; modem_sms_outcome_t delivered_outcome;`.

`modem_sms_protocol.c` changes:

1. `request_valid`: accept `STORE_DELIVERED` only when `request->pdu_hex != NULL && request->pdu_hex[0] != '\0' && request->tpdu_len != 0u`.
2. `modem_sms_protocol_begin`: add `case MODEM_SMS_PROTOCOL_STORE_DELIVERED:` sharing the `SAVE` branch (CPMS command). Reset `s_protocol.stored_index = 0u; s_protocol.delivered_outcome = MODEM_SMS_OUTCOME_NONE;` in the common reset block.
3. New helpers:

```c
static void emit_delivered(const modem_sms_protocol_hooks_t *hooks,
                           modem_sms_outcome_t outcome) {
    modem_sms_protocol_action_t action = {
        .type = MODEM_SMS_ACTION_PUBLISH_DELIVERED,
        .data.delivered = { .index = s_protocol.stored_index, .outcome = outcome },
    };
    (void)emit(hooks, &action);
}

static void delivered_restore_text(const modem_sms_protocol_hooks_t *hooks,
                                   uint32_t now_ms, modem_sms_outcome_t outcome) {
    s_protocol.delivered_outcome = outcome;
    (void)emit_command(hooks, MODEM_SMS_COMMAND_CMGF_TEXT, "AT+CMGF=1",
                       5000u, now_ms, false, false);
}
```

4. `modem_sms_protocol_resume_after_wake`: add

```c
    } else if (request->operation == MODEM_SMS_PROTOCOL_STORE_DELIVERED) {
        (void)emit_command(hooks, MODEM_SMS_COMMAND_CMGF_PDU, "AT+CMGF=0",
                           5000u, now_ms, false, false);
```

5. `on_final` `MODEM_SMS_COMMAND_CPMS` `!ok` chain: add `else if (request->operation == MODEM_SMS_PROTOCOL_STORE_DELIVERED) { emit_delivered(hooks, MODEM_SMS_OUTCOME_ERROR); }` before `emit_complete`.
6. `on_final` `MODEM_SMS_COMMAND_CMGF_PDU`: in the `ok && (MAILBOX || READ)` `if` chain add a branch:

```c
        } else if (ok && request->operation == MODEM_SMS_PROTOCOL_STORE_DELIVERED) {
            char command[24];
            snprintf(command, sizeof(command), "AT+CMGW=%u,0",
                     (unsigned)request->tpdu_len);
            (void)emit_command(hooks, MODEM_SMS_COMMAND_CMGW_PROMPT, command,
                               5000u, now_ms, true, true);
        } else if (request->operation == MODEM_SMS_PROTOCOL_STORE_DELIVERED) {
            emit_delivered(hooks, MODEM_SMS_OUTCOME_ERROR);
            emit_complete(request, hooks, MODEM_SMS_OUTCOME_ERROR, false);
```

   (place the failing branch inside the existing `else` that handles `!ok`, before the MAILBOX/READ/send fallbacks).
7. `on_prompt`: body bytes for `STORE_DELIVERED` are `request->pdu_hex` / `strlen(request->pdu_hex)`, `binary = true`, `segment = 1`, `segment_total = 1`, `tpdu_len = request->tpdu_len`, timeout 30000u.
8. `parse_line` `case MODEM_SMS_COMMAND_CMGW_FINAL:`:

```c
    case MODEM_SMS_COMMAND_CMGW_FINAL: {
        if (!modem_at_starts_with(line, "+CMGW:")) {
            return false;
        }
        unsigned index = 0u;
        if (sscanf(line, "+CMGW: %u", &index) == 1 && index <= UINT16_MAX) {
            s_protocol.stored_index = (uint16_t)index;
        }
        return true;
    }
```

9. `on_final` `MODEM_SMS_COMMAND_CMGW_PROMPT` and `MODEM_SMS_COMMAND_CMGW_FINAL`: add first

```c
        if (request->operation == MODEM_SMS_PROTOCOL_STORE_DELIVERED) {
            delivered_restore_text(hooks, now_ms,
                                   ok ? MODEM_SMS_OUTCOME_OK : MODEM_SMS_OUTCOME_ERROR);
            return;
        }
```

   (for `CMGW_PROMPT` `ok` is always false there: it is the pre-prompt failure path).
10. `on_final` `MODEM_SMS_COMMAND_CMGF_TEXT`: add before the MAILBOX/READ handling

```c
        if (request->operation == MODEM_SMS_PROTOCOL_STORE_DELIVERED) {
            modem_sms_outcome_t outcome = s_protocol.delivered_outcome;
            if (outcome == MODEM_SMS_OUTCOME_NONE) {
                outcome = MODEM_SMS_OUTCOME_ERROR;
            }
            emit_delivered(hooks, outcome);
            emit_complete(request, hooks, outcome, outcome == MODEM_SMS_OUTCOME_OK);
            return;
        }
```

11. `on_timeout`: the existing `prompt_timeout` branch already emits `ABORT_PROMPT` with `restore_after_settle`; make sure `STORE_DELIVERED` takes the same branch as `SEND_BINARY` for the prompt case (grep `request->operation == MODEM_SMS_PROTOCOL_SEND_BINARY` in `on_timeout` and extend the condition). In `modem_sms_protocol_resume_after_prompt_abort` add:

```c
    } else if (request != NULL &&
               request->operation == MODEM_SMS_PROTOCOL_STORE_DELIVERED) {
        delivered_restore_text(hooks, now_ms, MODEM_SMS_OUTCOME_TIMEOUT);
```

   For `CMGF_PDU`/`CMGF_TEXT`/`CMGW_FINAL` timeouts with `STORE_DELIVERED`: `CMGF_PDU` timeout → `emit_delivered(TIMEOUT)` + complete; `CMGW_FINAL` timeout → `delivered_restore_text(hooks, now_ms, MODEM_SMS_OUTCOME_UNCERTAIN)`; `CMGF_TEXT` timeout → `emit_delivered(s_protocol.delivered_outcome)` + complete with `command_ok=false`.

- [ ] **Step 4: Run tests**

Run: `tests/run_tests.sh 2>&1 | grep -E "test_modem_sms_protocol|fail="`
Expected: PASS, `fail=0`.

- [ ] **Step 5: Commit**

```bash
git add src/services/modem_sms_protocol.c src/services/modem_sms_protocol_internal.h tests/test_modem_sms_protocol.c
git commit -m "Add STORE_DELIVERED SMS protocol operation

Claude-Session: https://claude.ai/code/session_01UeSArQWYHateziEWYPtoQn"
```

---

### Task 6: Service wiring: `+CMT` routing, retry ring, `$QCMTI`, arrival bookkeeping

**Files:**
- Modify: `include/services/sms_types.h:82-89` (`MODEM_SMS_REQUEST_DELIVERED`)
- Modify: `src/services/modem_service.c`: request enum (`MODEM_REQ_STORE_DELIVERED_SMS`), op enum (`MODEM_OP_STORE_DELIVERED_SMS`), `sms_request_kind_from_request_type`, `sms_protocol_request_view`, dispatch switch (`:3814`), op mapping (`:6476`), `process_line` (`:2800`), `route_urc` (`:2899`), `is_known_urc_line` (`:5396`), `sms_protocol_emit` (PUBLISH_DELIVERED), idle scheduler (`:3628`), `sms_publish_terminal` (DELIVERED no-op), `sms_cancel_protocol`
- Test: `tests/test_modem_telit_service.c`

**Interfaces:**
- Consumes: `modem_sms_direct_*` (Task 3), `g_modem_vendor.translate_direct_sms` (Task 4), `STORE_DELIVERED` op + `PUBLISH_DELIVERED` action (Task 5).
- Produces: internal behaviour only; `modem_status_t.sms_received_count` increments on a stored delivery exactly as on `+CMTI`.

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_modem_telit_service.c` (use `mh_feed`, `s_mh_tx_last`, `s_mh_tx_history`, `mh_tick` helpers from `tests/harness/modem_service_harness.h`; follow the pattern of the existing `+CMTI` test at `:1714`):

```c
static const char DIRECT_3GPP2_HEADER[] =
    "+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,8,9";

static void test_direct_delivery_is_restored_as_a_pdu(void) {
    mh_boot_to_ready(); /* whatever the file's ready-state fixture is called */
    uint32_t before = mh_status().sms_received_count;
    mh_feed(DIRECT_3GPP2_HEADER);
    mh_feed("Dhdjdjdjs");
    mh_run_ms(50u);
    check(mh_tx_seen("AT+CPMS=\"ME\",\"ME\",\"ME\""), "store starts with CPMS");
    mh_feed("+CPMS: 3,100,3,100,3,100"); mh_feed("OK"); mh_run_ms(50u);
    check(mh_tx_seen("AT+CMGF=0"), "PDU mode selected");
    mh_feed("OK"); mh_run_ms(50u);
    check(mh_tx_seen("AT+CMGW=25,0"), "CMGW length matches the rebuilt TPDU");
    mh_feed_prompt(); mh_run_ms(50u);
    check(mh_tx_seen("00040A8187661940880000629061055482000744"), "PDU body written");
    mh_feed("+CMGW: 7"); mh_feed("OK"); mh_run_ms(50u);
    check(mh_tx_seen("AT+CMGF=1"), "text mode restored");
    mh_feed("OK"); mh_run_ms(50u);
    check(mh_status().sms_received_count == before + 1u,
          "a stored delivery counts like a +CMTI");
    check(modem_sms_protocol_track_pending_arrival(7u) == false,
          "index 7 is already tracked as a pending arrival");
}

static void test_direct_delivery_mid_command_and_qcmti(void) {
    mh_boot_to_ready();
    (void)modem_service_request_debug_at("AT+COPS?");
    mh_run_ms(20u);
    mh_feed(DIRECT_3GPP2_HEADER);
    mh_feed("OK"); /* payload that looks like a final must not finish AT+COPS? */
    mh_run_ms(20u);
    check(mh_status().busy, "debug command still active after a +CMT payload");
    mh_feed("+COPS: 0,0,\"US  Mobile\",7"); mh_feed("OK"); mh_run_ms(50u);
    check(mh_tx_seen("AT+CPMS=\"ME\",\"ME\",\"ME\""), "store queued after the command");

    uint32_t errors = mh_status().command_errors;
    mh_feed("$QCMTI: \"ME\",24");
    mh_run_ms(20u);
    check(mh_status().command_errors == errors + 1u,
          "$QCMTI is recognised, counted as an error, and never triggers a CMGR");
    check(!mh_tx_seen("AT+CMGR=24"), "no read attempt for the unreadable CDMA store");
}

static void test_direct_delivery_retries_after_store_failure(void) {
    mh_boot_to_ready();
    mh_feed(DIRECT_3GPP2_HEADER);
    mh_feed("Dhdjdjdjs");
    mh_run_ms(50u);
    mh_feed("+CMS ERROR: memory failure"); /* CPMS fails */
    mh_run_ms(50u);
    check(mh_tx_count("AT+CPMS=\"ME\",\"ME\",\"ME\"") == 2u,
          "a failed store is retried from the ring");
    for (int i = 0; i < 2; i++) { mh_feed("+CMS ERROR: memory failure"); mh_run_ms(50u); }
    check(mh_tx_count("AT+CPMS=\"ME\",\"ME\",\"ME\"") == 3u,
          "retries are bounded to three attempts");
}
```

Use the harness's real helper names (`grep -n "static.*mh_" tests/harness/modem_service_harness.h`): if `mh_tx_seen`/`mh_tx_count`/`mh_feed_prompt`/`mh_run_ms`/`mh_status`/`mh_boot_to_ready` do not exist under those names, use the equivalents that the existing `+CMTI` and `binary SMS` tests in this file already use, or add them to the harness header.

- [ ] **Step 2: Run test to verify it fails**

Run: `tests/run_tests.sh 2>&1 | grep -A6 "test_modem_telit_service"`
Expected: FAIL on "store starts with CPMS".

- [ ] **Step 3: Implement the service wiring**

`include/services/sms_types.h`: add `MODEM_SMS_REQUEST_DELIVERED,` after `MODEM_SMS_REQUEST_DELETE`.

`modem_service.c`:

```c
/* --- direct delivery ring ------------------------------------------------ */
#define MODEM_DIRECT_RING_DEPTH 2u
#define MODEM_DIRECT_STORE_ATTEMPTS 3u
#define MODEM_DIRECT_INTERNAL_REQUEST_ID 0xFFFFFFF0u

typedef struct {
    char pdu_hex[SMS_DELIVER_HEX_MAX];
    uint8_t tpdu_len;
    uint8_t attempts;
    bool used;
} modem_direct_entry_t;

static modem_direct_entry_t s_direct_ring[MODEM_DIRECT_RING_DEPTH];
static uint8_t s_direct_active; /* index of the entry being stored, or UINT8_MAX */
```

Request/op enums: `MODEM_REQ_STORE_DELIVERED_SMS` (after `MODEM_REQ_DELETE_SMS`), `MODEM_OP_STORE_DELIVERED_SMS` (after `MODEM_OP_DELETE_SMS`). Map in `sms_request_kind_from_request_type` (`MODEM_REQ_STORE_DELIVERED_SMS` → `MODEM_SMS_REQUEST_DELIVERED`), in the op mapping switch (`:6476`) and the dispatch switch (`:3814`, same `sms_start_current_request`). In `sms_protocol_request_view` add `case MODEM_OP_STORE_DELIVERED_SMS: out->operation = MODEM_SMS_PROTOCOL_STORE_DELIVERED; break;` and after the field copies:

```c
    if (s_operation == MODEM_OP_STORE_DELIVERED_SMS &&
        s_direct_active < MODEM_DIRECT_RING_DEPTH) {
        out->pdu_hex = s_direct_ring[s_direct_active].pdu_hex;
        out->tpdu_len = s_direct_ring[s_direct_active].tpdu_len;
    }
```

`sms_publish_terminal`: add `case MODEM_SMS_REQUEST_DELIVERED: published = true; break;` (no app-facing result).

Collector feed (called from `process_line` and `route_urc`):

```c
static void direct_feed_line(const char *line) {
    char pdu_hex[SMS_DELIVER_HEX_MAX];
    uint8_t tpdu_len = 0u;
    modem_sms_direct_step_t step = modem_sms_direct_feed(
        line, g_modem_vendor.translate_direct_sms, pdu_hex, sizeof(pdu_hex), &tpdu_len);
    switch (step) {
    case MODEM_SMS_DIRECT_STEP_HEADER:
        LOGI("modem", "%s", line);
        break;
    case MODEM_SMS_DIRECT_STEP_READY: {
        uint8_t slot = UINT8_MAX;
        for (uint8_t i = 0u; i < MODEM_DIRECT_RING_DEPTH; i++) {
            if (!s_direct_ring[i].used) { slot = i; break; }
        }
        if (slot == UINT8_MAX) {
            LOGW("modem", "direct SMS dropped: ring full");
            critical_section_enter_blocking(&s_status_lock);
            s_status.command_errors++;
            critical_section_exit(&s_status_lock);
            break;
        }
        memcpy(s_direct_ring[slot].pdu_hex, pdu_hex, sizeof(pdu_hex));
        s_direct_ring[slot].tpdu_len = tpdu_len;
        s_direct_ring[slot].attempts = 0u;
        s_direct_ring[slot].used = true;
        LOGI("modem", "direct SMS rebuilt tpdu=%u slot=%u", (unsigned)tpdu_len, (unsigned)slot);
        break;
    }
    case MODEM_SMS_DIRECT_STEP_REJECTED:
        LOGW("modem", "direct SMS not understood; message lost");
        critical_section_enter_blocking(&s_status_lock);
        s_status.command_errors++;
        critical_section_exit(&s_status_lock);
        break;
    case MODEM_SMS_DIRECT_STEP_IGNORED:
    default:
        break;
    }
}
```

`process_line`: right after the `if (len == 0) return;` / `copy_bounded(s_debug_last_line...)` lines, before `parse_sim_observation`:

```c
    if (modem_sms_direct_pending()) {
        /* The line after a +CMT header is its payload by protocol, even when
         * it happens to read like a final or a URC. */
        direct_feed_line(line);
        return;
    }
```

`route_urc`: add branches

```c
    } else if (starts_with(line, "+CMT:")) {
        critical_section_enter_blocking(&s_status_lock);
        s_status.urc_count++;
        critical_section_exit(&s_status_lock);
        direct_feed_line(line);
    } else if (starts_with(line, "$QCMTI:")) {
        /* Module stored a message in a store this backend cannot read
         * (only happens if direct delivery was not applied). */
        critical_section_enter_blocking(&s_status_lock);
        s_status.urc_count++;
        s_status.command_errors++;
        critical_section_exit(&s_status_lock);
        LOGW("modem", "%s (unreadable store; direct delivery not active)", line);
```

`is_known_urc_line`: add `starts_with(line, "$QCMTI:") ||`.

Idle scheduler (the `else if` chain at `:3615-3633`), add before the `s_cpms_check_needed` branch:

```c
        } else if (direct_ring_start(now_ms)) {
            /* started a STORE_DELIVERED operation */
```

with

```c
static bool direct_ring_start(uint32_t now_ms) {
    (void)now_ms;
    if (s_operation != MODEM_OP_NONE || s_active || s_dtr_wake_pending ||
        !status_sim_ready_snapshot()) {
        return false;
    }
    for (uint8_t i = 0u; i < MODEM_DIRECT_RING_DEPTH; i++) {
        if (!s_direct_ring[i].used) continue;
        if (s_direct_ring[i].attempts >= MODEM_DIRECT_STORE_ATTEMPTS) {
            LOGW("modem", "direct SMS store gave up after %u attempts", (unsigned)s_direct_ring[i].attempts);
            s_direct_ring[i].used = false;
            critical_section_enter_blocking(&s_status_lock);
            s_status.command_errors++;
            critical_section_exit(&s_status_lock);
            continue;
        }
        modem_request_t request;
        memset(&request, 0, sizeof(request));
        request.type = MODEM_REQ_STORE_DELIVERED_SMS;
        request.request_id = MODEM_DIRECT_INTERNAL_REQUEST_ID;
        if (!queue_request(&request)) {
            return false;
        }
        s_direct_ring[i].attempts++;
        s_direct_active = i;
        return true;
    }
    return false;
}
```

`sms_protocol_emit`: add

```c
    case MODEM_SMS_ACTION_PUBLISH_DELIVERED: {
        bool ok = action->data.delivered.outcome == MODEM_SMS_OUTCOME_OK &&
                  action->data.delivered.index != 0u;
        if (ok) {
            uint16_t index = action->data.delivered.index;
            bool tracked = modem_sms_protocol_track_pending_arrival(index);
            critical_section_enter_blocking(&s_status_lock);
            s_status.sms_received_count++;
            if (!tracked) {
                s_status.sms_user_received_count++;
            }
            critical_section_exit(&s_status_lock);
            s_cpms_check_needed = true;
            if (s_direct_active < MODEM_DIRECT_RING_DEPTH) {
                s_direct_ring[s_direct_active].used = false;
            }
            LOGI("modem", "direct SMS stored at index %u", (unsigned)index);
        } else {
            LOGW("modem", "direct SMS store failed outcome=%u",
                 (unsigned)action->data.delivered.outcome);
        }
        s_direct_active = UINT8_MAX;
        s_sms_terminal_published = true; /* internal op: nothing for the app */
        return true;
    }
```

`sms_cancel_protocol` (call preemption): after `modem_sms_protocol_cancel()`, add `s_direct_active = UINT8_MAX;` (the ring entry keeps `used = true`, so it is retried). Initialise `s_direct_active = UINT8_MAX` in the service init and call `modem_sms_direct_reset()` wherever the SMS protocol is reset on module power-off/re-init.

Also keep the `+CMTI` branch intact: AT&T in store mode via an old profile still works, and the bench test in Task 7 confirms the new init.

- [ ] **Step 4: Run the full suite and the firmware build**

Run: `tests/run_tests.sh 2>&1 | tail -3` → `fail=0`.
Run: `cmake --build build -j8 2>&1 | tail -3` → `[100%] Built target sisu_dcp3_revb2_telit`, no new warnings (the build uses `-Werror`-class flags; fix any).

- [ ] **Step 5: Commit**

```bash
git add include/services/sms_types.h src/services/modem_service.c tests/test_modem_telit_service.c tests/harness/modem_service_harness.h
git commit -m "Store direct-delivered SMS into ME as REC UNREAD PDUs

Claude-Session: https://claude.ai/code/session_01UeSArQWYHateziEWYPtoQn"
```

---

### Task 7: Bench validation (Verizon, then AT&T) and docs

**Files:**
- Modify: `docs/sms_direct_delivery_design.md` (add "Bench results" section)
- Modify: `docs/revb2_hardware_contract.md` (SMS paragraph: direct delivery, `+CSDH=1`, `$QCMTI` note)
- Modify: `docs/public_release_audit.md` or the audit log file named in memory `public-release-audit` (one line: deferred-setup retry storm in `#SMSFORMAT=1`, CCFC/MWI "network rejected" on Verizon)

- [ ] **Step 1: Flash and confirm init**

```bash
cmake --build build -j8 && picotool load -f -x build/sisu_dcp3_revb2_telit.uf2
```

Then over the console (`/dev/cu.usbmodem*`, 115200): `phoneon`, wait 60 s, `status` must show `prov=1/v11 reg=1`, `at AT+CNMI?` → `+CNMI: 2,2,0,0,0`, `at AT+CSDH?` → `+CSDH: 1`, `at AT+CMGF?` → `+CMGF: 1`.

- [ ] **Step 2: Verizon inbound matrix**

With `modemrx on`, have a message sent from another line for each row and record the console log lines `direct SMS rebuilt`, `AT+CMGW=`, `direct SMS stored at index`, then open Messages on the phone:

| # | message | expected on phone |
|---|---|---|
| 1 | short plain text | shows as new message, sender number, delivery time |
| 2 | > 160 characters | one message, grouped, complete text |
| 3 | emoji | rendered / placeholder, not garbage |
| 4 | text with `@`, `[`, `]`, `~` | characters preserved |
| 5 | message while a mailbox scan is running (send while opening Inbox) | still stored (may log the PDU-form path) |
| 6 | message while a call is active | stored after the call (ring retry) |

Also send one message from the phone (`txt <number> <text>`) and confirm outgoing still works and the sent copy appears in Sent.

- [ ] **Step 3: Sleep and power gate (Verizon)**

Leave the phone in standby (screen off, no console traffic) for 3 minutes, then send one message. Record: time from send to the phone's new-message alert (must be ≤ the AT&T store-mode latency measured earlier, a few seconds after the RI wake), and standby current on the bench meter before/after the change (must match `docs/standby_power.md` figures within noise). If the URC is lost during sleep (no `+CMT` after wake), stop and report; do not change wake settings.

- [ ] **Step 4: AT&T regression**

Swap to the AT&T SIM (image auto-switches; wait for `prov=1` and `reg=1`), repeat rows 1-3 of the matrix plus one picture/port SMS between two phones if available. Expect `+CMT: ,<len>` PDU form or the 10-field text form in the raw capture and identical inbox behaviour to before.

- [ ] **Step 5: Record results and commit docs**

Append the measured latencies, power figures and any deviations to `docs/sms_direct_delivery_design.md` under "Bench results (date)". Update the hardware contract SMS paragraph and the audit log line.

```bash
git add docs/sms_direct_delivery_design.md docs/revb2_hardware_contract.md docs/public_release_audit.md
git commit -m "Document unified SMS direct-delivery route and bench results

Claude-Session: https://claude.ai/code/session_01UeSArQWYHateziEWYPtoQn"
```

---

## Self-review notes

- Spec coverage: init (T1), builder (T2), collector + 3GPP forms (T3), vendor 3GPP2 forms + hook + none-vendor (T4), protocol op (T5), routing/ring/`$QCMTI`/bookkeeping/idle scheduler (T6), sleep/power and AT&T gates + docs (T7). Acknowledgement policy is a documented non-change.
- Type consistency: `sms_deliver_t`, `SMS_DELIVER_HEX_MAX`, `modem_sms_direct_translate_fn`, `modem_sms_direct_feed`, `MODEM_SMS_PROTOCOL_STORE_DELIVERED`, `MODEM_SMS_ACTION_PUBLISH_DELIVERED`, `MODEM_SMS_REQUEST_DELIVERED`, `MODEM_REQ_STORE_DELIVERED_SMS`, `MODEM_OP_STORE_DELIVERED_SMS`, `telit_translate_direct_sms` are used with the same spelling in every task.
- Known judgement calls for the executor: harness helper names in T6 must be matched to `tests/harness/modem_service_harness.h`; the Telit CSV view helpers' quote handling in T4 must be checked against `modem_vendor_telit_parse.c` before relying on them.
