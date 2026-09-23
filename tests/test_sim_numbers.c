#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "services/sim_numbers.h"
#include "../src/services/modem_vendor_telit_internal.h"

static uint32_t id;
static sim_numbers_snapshot_t view(void) {
    sim_numbers_snapshot_t s; sim_numbers_snapshot(&s); return s;
}
static void command(const char *expected) {
    char s[96]; assert(sim_numbers_prepare(s, sizeof(s)));
    if (strcmp(s, expected)) fprintf(stderr, "expected %s, got %s\n", expected, s);
    assert(strcmp(s, expected) == 0);
}
static void response(const char *line) {
    assert(!sim_numbers_line("+CMTI: \"ME\",1"));
    assert(sim_numbers_line(line)); sim_numbers_final(true, true);
}
static void data(const uint8_t *bytes, size_t n) {
    char line[300] = "+CRSM: 144,0,\""; size_t used = strlen(line);
    for (size_t i = 0; i < n; i++) used += (size_t)snprintf(line + used, sizeof(line) - used, "%02X", bytes[i]);
    strcpy(line + used, "\""); response(line);
}
static void meta(unsigned file, unsigned length, unsigned records) {
    char s[96]; snprintf(s, sizeof(s), "AT+CRSM=192,%u", file); command(s);
    snprintf(s, sizeof(s), "+CRSM: 144,0,621282054221%04X%02X8302%04X8002%04X8A0105",
        length, records, file, length * records); response(s);
}
static void transparent(unsigned file, unsigned length) {
    char s[96]; snprintf(s, sizeof(s), "AT+CRSM=192,%u", file); command(s);
    snprintf(s, sizeof(s), "+CRSM: 144,0,620F820241218302%04X8002%04X8A0105", file, length); response(s);
}
static void read_record(unsigned file, unsigned record, const uint8_t *d, unsigned n) {
    char s[96]; snprintf(s, sizeof(s), "AT+CRSM=178,%u,%u,4,%u", file, record, n); command(s); data(d, n);
}
static void begin(sim_numbers_kind_t kind) {
    sim_numbers_init(&TELIT_SIM_NUMBERS_BACKEND); id = sim_numbers_request(kind); assert(id);
    assert(!sim_numbers_request(kind));
}
static void cphs(void) {
    transparent(0x6f16, 3); command("AT+CRSM=176,28438,0,0,3"); response("+CRSM: 144,0,020303");
    transparent(0x6f15, 4); command("AT+CRSM=176,28437,0,0,4"); response("+CRSM: 144,0,01FFD5FF");
}
static void service_record(uint8_t d[22]) {
    memset(d, 255, 22); memcpy(d, "Care", 4);
    d[8] = 3; d[9] = 0x91; d[10] = 0x16; d[11] = 0xf1;
}
static void info_record(uint8_t d[19], uint8_t flags, bool folder) {
    memset(d, 255, 19); d[0] = 8; d[1] = flags; memcpy(d + 2, "Weather", 7);
    if (!folder) { d[10] = 3; d[11] = 0x81; d[12] = 0x23; d[13] = 0xf3; }
}

static void test_boundaries(void) {
    uint8_t d[22], ext[13];
    sim_number_t entry;
    begin(SIM_NUMBERS_SERVICE); meta(0x6f49, 22, 254);
    for (unsigned i = 1; i <= 254; i++) {
        if (i <= SIM_NUMBERS_MAX) service_record(d);
        else memset(d, 255, sizeof(d));
        read_record(0x6f49, i, d, sizeof(d));
    }
    assert(view().result == SIM_NUMBERS_DONE && view().count == SIM_NUMBERS_MAX);
    assert(sim_numbers_entry(id, SIM_NUMBERS_MAX - 1u, &entry));

    for (unsigned overflow = 0; overflow < 2; overflow++) {
        begin(SIM_NUMBERS_SERVICE); meta(0x6f49, 22, 1);
        service_record(d); d[8] = 11; d[9] = 0x81; d[21] = 1;
        memset(d + 10, 0x11, 10);
        read_record(0x6f49, 1, d, sizeof(d));
        memset(ext, 255, sizeof(ext)); ext[0] = 2; ext[1] = 6 + overflow;
        memset(ext + 2, 0x22, 6); if (overflow) ext[8] = 0xf2;
        read_record(0x6f4c, 1, ext, sizeof(ext));
        if (overflow) assert(view().result == SIM_NUMBERS_ERROR && view().count == 0);
        else assert(sim_numbers_entry(id, 0, &entry) && strlen(entry.number) == SIM_NUMBER_CAP - 1u);
    }

    begin(SIM_NUMBERS_INFO); cphs(); meta(0x6f19, 19, 16);
    for (unsigned i = 1; i <= 15; i++) {
        info_record(d, (uint8_t)((i == 1 ? 0xf0 : 0xc0) | i), i != 15);
        read_record(0x6f19, i, d, 19);
    }
    info_record(d, 0xc1, false); read_record(0x6f19, 16, d, 19);
    assert(sim_numbers_entry(id, 14, &entry) && entry.parent == 13 && entry.home_only && entry.premium);
    assert(sim_numbers_entry(id, 15, &entry) && entry.parent == SIM_NUMBERS_ROOT && !entry.home_only && !entry.premium);

    /* A cancelled partial read must not leave even its completed first row visible. */
    begin(SIM_NUMBERS_SERVICE); meta(0x6f49, 22, 2);
    service_record(d); read_record(0x6f49, 1, d, sizeof(d));
    uint32_t old_id = id;
    sim_numbers_cancel(id);
    assert(!view().busy && !sim_numbers_entry(old_id, 0, &entry));
    id = sim_numbers_request(SIM_NUMBERS_SERVICE); assert(id && id != old_id);
    sim_numbers_cancel(old_id); assert(view().busy);
    meta(0x6f49, 22, 1); read_record(0x6f49, 1, d, sizeof(d));
    assert(sim_numbers_entry(id, 0, &entry) && !sim_numbers_entry(old_id, 0, &entry));

    uint8_t alpha[SIM_NUMBER_NAME_CAP]; char decoded[SIM_NUMBER_NAME_CAP];
    memset(alpha, 'A', sizeof(alpha));
    assert(telit_sim_alpha(alpha, sizeof(alpha) - 1u, decoded, sizeof(decoded)));
    assert(strlen(decoded) == sizeof(decoded) - 1u);
    assert(!telit_sim_alpha(alpha, sizeof(alpha), decoded, sizeof(decoded)) && !decoded[0]);
    const uint8_t compressed[] = {0x82, 1, 0x03, 0x80, 0xa9, 0xff};
    assert(telit_sim_alpha(compressed, sizeof(compressed), decoded, sizeof(decoded)) && !strcmp(decoded, "\xce\xa9"));

    char command_text[64];
    sim_number_query_t query = {.kind = SIM_NUMBER_FILE_INFO, .file = 0x6f07};
    assert(!TELIT_SIM_NUMBERS_BACKEND.build(&query, command_text, sizeof(command_text)));
    query.file = 0x6f49; query.kind = (sim_number_read_t)3; query.length = 22; query.record = 1;
    assert(!TELIT_SIM_NUMBERS_BACKEND.build(&query, command_text, sizeof(command_text)));
}

int main(void) {
    test_boundaries();
    uint8_t d[22]; sim_number_t entry;
    sim_numbers_init(&TELIT_SIM_NUMBERS_BACKEND);
    assert(!sim_numbers_available(SIM_NUMBERS_SERVICE) && !sim_numbers_available(SIM_NUMBERS_INFO));
    assert(sim_numbers_probe(SIM_NUMBERS_SERVICE)); meta(0x6f49, 22, 3);
    assert(!view().busy && !view().count && !sim_numbers_probe_needed(SIM_NUMBERS_SERVICE));
    assert(!sim_numbers_available(SIM_NUMBERS_SERVICE)); /* Gates publish together. */
    assert(sim_numbers_probe(SIM_NUMBERS_INFO)); cphs(); meta(0x6f19, 19, 2);
    assert(sim_numbers_available(SIM_NUMBERS_SERVICE) && sim_numbers_available(SIM_NUMBERS_INFO));
    assert(!sim_numbers_entry(view().request_id, 0, &entry)); /* Probing never invents records. */
    sim_numbers_invalidate();
    assert(!sim_numbers_available(SIM_NUMBERS_SERVICE) && sim_numbers_probe_needed(SIM_NUMBERS_SERVICE));
    assert(sim_numbers_probe(SIM_NUMBERS_SERVICE)); meta(0x6f49, 22, 0);
    assert(sim_numbers_probe(SIM_NUMBERS_INFO)); command("AT+CRSM=192,28438"); response("+CRSM: 106,130");
    assert(!sim_numbers_available(SIM_NUMBERS_SERVICE) && !sim_numbers_available(SIM_NUMBERS_INFO));
    assert(!sim_numbers_probe_needed(SIM_NUMBERS_INFO));
    sim_numbers_invalidate();
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        assert(sim_numbers_probe(SIM_NUMBERS_SERVICE)); command("AT+CRSM=192,28489");
        sim_numbers_timeout(); sim_numbers_final(false, false);
        assert(view().result == SIM_NUMBERS_ERROR);
    }
    assert(!sim_numbers_probe_needed(SIM_NUMBERS_SERVICE)); /* Failed optional reads are bounded. */
    sim_numbers_invalidate();
    id = sim_numbers_probe(SIM_NUMBERS_SERVICE); command("AT+CRSM=192,28489");
    sim_numbers_cancel(id); response("+CRSM: 106,130");
    assert(sim_numbers_probe_needed(SIM_NUMBERS_SERVICE)); /* A call cancellation can retry later. */
    begin(SIM_NUMBERS_SERVICE); meta(0x6f49, 22, 3);
    service_record(d); read_record(0x6f49, 1, d, 22);
    assert(!sim_numbers_entry(id, 0, &entry)); /* Never publish a partial list. */
    memset(d, 255, sizeof(d)); read_record(0x6f49, 2, d, 22);
    service_record(d); d[9] = 0x81; read_record(0x6f49, 3, d, 22);
    assert(view().result == SIM_NUMBERS_DONE && view().count == 2);
    assert(sim_numbers_entry(id, 0, &entry) && strcmp(entry.number, "+611") == 0 &&
        strcmp(entry.name, "Care") == 0 && entry.parent == SIM_NUMBERS_ROOT);
    assert(!sim_numbers_entry(id + 1, 0, &entry));
    sim_numbers_invalidate(); assert(!sim_numbers_entry(id, 0, &entry));

    begin(SIM_NUMBERS_SERVICE); meta(0x6f49, 22, 0);
    assert(view().result == SIM_NUMBERS_DONE && !view().count);
    begin(SIM_NUMBERS_SERVICE); command("AT+CRSM=192,28489"); response("+CRSM: 106,130");
    assert(view().result == SIM_NUMBERS_UNAVAILABLE);

    begin(SIM_NUMBERS_INFO); transparent(0x6f16, 3);
    command("AT+CRSM=176,28438,0,0,3"); response("+CRSM: 144,0,FFFFFF");
    assert(view().result == SIM_NUMBERS_UNAVAILABLE);
    begin(SIM_NUMBERS_INFO); transparent(0x6f16, 3);
    command("AT+CRSM=176,28438,0,0,3"); response("+CRSM: 144,0,020303");
    transparent(0x6f15, 4); command("AT+CRSM=176,28437,0,0,4"); response("+CRSM: 144,0,D5FFD500");
    assert(view().result == SIM_NUMBERS_ERROR);

    begin(SIM_NUMBERS_SERVICE); meta(0x6f49, 22, 1);
    service_record(d); d[21] = 2; read_record(0x6f49, 1, d, 22);
    uint8_t ext[13]; memset(ext, 255, sizeof(ext)); ext[0] = 2; ext[1] = 1; ext[2] = 0x54;
    read_record(0x6f4c, 2, ext, 13);
    assert(sim_numbers_entry(id, 0, &entry) && !strcmp(entry.number, "+61145"));
    begin(SIM_NUMBERS_SERVICE); meta(0x6f49, 22, 1);
    read_record(0x6f49, 1, d, 22); ext[12] = 2; read_record(0x6f4c, 2, ext, 13);
    assert(view().result == SIM_NUMBERS_ERROR); /* Extension cycles cannot hang or truncate. */

    begin(SIM_NUMBERS_INFO); cphs();
    command("AT+CRSM=192,28441"); response("+CRSM: 148,4");
    meta(0xea01, 19, 4);
    info_record(d, 0xf1, true); read_record(0xea01, 1, d, 19);
    memset(d, 255, sizeof(d)); read_record(0xea01, 2, d, 19);
    info_record(d, 0xc2, false); read_record(0xea01, 3, d, 19);
    info_record(d, 0xc1, false); read_record(0xea01, 4, d, 19);
    assert(view().result == SIM_NUMBERS_DONE && view().count == 3);
    assert(sim_numbers_entry(id, 1, &entry) && entry.parent == 0 && entry.home_only && entry.premium &&
        !entry.folder && !strcmp(entry.number, "323"));
    assert(sim_numbers_entry(id, 2, &entry) && entry.parent == SIM_NUMBERS_ROOT && !entry.home_only && !entry.premium);

    begin(SIM_NUMBERS_INFO); cphs(); meta(0x6f19, 19, 1);
    info_record(d, 0xc2, false); read_record(0x6f19, 1, d, 19);
    assert(view().result == SIM_NUMBERS_ERROR); /* Orphan child. */
    begin(SIM_NUMBERS_INFO); transparent(0x6f16, 3);
    command("AT+CRSM=176,28438,0,0,3"); response("+CRSM: 144,0,020301");
    assert(view().result == SIM_NUMBERS_UNAVAILABLE);
    begin(SIM_NUMBERS_INFO); transparent(0x6f16, 3);
    command("AT+CRSM=176,28438,0,0,3"); response("+CRSM: 144,0,020303");
    transparent(0x6f15, 2); command("AT+CRSM=176,28437,0,0,2"); response("+CRSM: 144,0,D500");
    assert(view().result == SIM_NUMBERS_UNAVAILABLE);

    for (unsigned scenario = 0; scenario < 8; scenario++) {
        begin(SIM_NUMBERS_SERVICE); meta(0x6f49, 22, 1); service_record(d);
        switch (scenario) {
        case 0: d[8] = 12; break; /* Oversized BCD. */
        case 1: d[10] = 0x1f; break; /* Filler before a digit. */
        case 2: d[10] = 0xc1; break; /* A dial control cannot be silently discarded. */
        case 3: d[9] = 0xd0; break; /* Alphanumeric TON. */
        case 4: d[0] = 0x80; d[1] = 0xd8; d[2] = 0x00; break;
        case 5: d[20] = 1; break; /* Unsupported bearer. */
        case 6: d[21] = 0; break;
        default: d[0] = 0x1b; d[1] = 0xff; break;
        }
        read_record(0x6f49, 1, d, 22);
        assert(view().result == SIM_NUMBERS_ERROR && !view().count);
    }
    begin(SIM_NUMBERS_SERVICE); meta(0x6f49, 22, 65); service_record(d);
    for (unsigned i = 1; i <= 65; i++) read_record(0x6f49, i, d, 22);
    assert(view().result == SIM_NUMBERS_ERROR && !view().count);

    const char *bad_meta[] = {
        "+CRSM: 144,0,62128205422100160283026F49800200168A0105", /* Count/size mismatch. */
        "+CRSM: 144,0,62128205422100160183026F19800200168A0105", /* Wrong EF. */
        "+CRSM: 144,0,62128205422100160183026F49800200168A0104", /* Deactivated. */
        "+CRSM: 144,0,620F8205422100160183026F4980020016", /* Missing lifecycle. */
        "+CRSM: 144,0,62128205422100160183026F49800200168A0105,1",
        "+CRSM: 144,0,\"62128205422100160183026F49800200168A0105",
        "+CRSM: 152,4", "+CRSM: 144,0", "+CRSM: 106,130,00",
    };
    for (size_t i = 0; i < sizeof(bad_meta) / sizeof(bad_meta[0]); i++) {
        begin(SIM_NUMBERS_SERVICE); command("AT+CRSM=192,28489"); response(bad_meta[i]);
        assert(view().result == SIM_NUMBERS_ERROR);
    }
    begin(SIM_NUMBERS_SERVICE); command("AT+CRSM=192,28489");
    response("+CRSM: 144,0,000000166F49040000000001020116");
    service_record(d); read_record(0x6f49, 1, d, 22); assert(view().result == SIM_NUMBERS_DONE);
    begin(SIM_NUMBERS_SERVICE); command("AT+CRSM=192,28489");
    response("+CRSM: 144,0,62128205422100160183026F49800200168A0107");
    service_record(d); read_record(0x6f49, 1, d, 22); assert(view().result == SIM_NUMBERS_DONE);

    for (unsigned scenario = 0; scenario < 5; scenario++) {
        begin(SIM_NUMBERS_SERVICE); command("AT+CRSM=192,28489");
        if (scenario < 2) {
            if (!scenario) sim_numbers_cancel(id); else sim_numbers_invalidate();
            assert(view().busy && !sim_numbers_request(SIM_NUMBERS_INFO));
        }
        if (scenario != 4) assert(sim_numbers_line("+CRSM: 106,130"));
        if (scenario == 2) assert(sim_numbers_line("+CRSM: 106,130"));
        sim_numbers_final(true, scenario != 3);
        assert(!view().busy && view().result == (scenario < 2 ? SIM_NUMBERS_CANCELLED : SIM_NUMBERS_ERROR));
    }
    char alpha[97];
    const uint8_t ucs[] = {0x80, 0x03, 0xa9, 0xff, 0xff};
    const uint8_t compressed[] = {0x81, 1, 0x08, 0x91, 0xff};
    assert(telit_sim_alpha(ucs, sizeof(ucs), alpha, sizeof(alpha)) && !strcmp(alpha, "\xce\xa9"));
    assert(telit_sim_alpha(compressed, sizeof(compressed), alpha, sizeof(alpha)));
    assert(!telit_sim_alpha(compressed, 1, alpha, sizeof(alpha)));
    assert(!telit_sim_alpha(ucs, sizeof(ucs), alpha, 2));
    puts("SIM number lists: OK");
    return 0;
}
