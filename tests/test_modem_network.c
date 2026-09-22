#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "services/modem_network.h"

extern const modem_network_backend_t TELIT_NETWORK_BACKEND;
static char owner[MODEM_NETWORK_RECORD_CAP];
static bool readable = true, writable = true;
static unsigned writes;
static const char *const CARD = "89014103211118510720";

static bool read_owner(char *out, size_t cap) {
    snprintf(out, cap, "%s", owner);
    return readable;
}
static bool save_owner(const char *id) {
    writes++;
    if (!writable) return false;
    snprintf(owner, sizeof(owner), "%s", id);
    return true;
}
static modem_network_snapshot_t view(void) {
    modem_network_snapshot_t v;
    modem_network_snapshot(&v);
    return v;
}
static void command(const char *expected) {
    char cmd[96]; uint32_t timeout;
    bool prepared = modem_network_prepare(cmd, sizeof(cmd), &timeout);
    if (!prepared) fprintf(stderr, "No command, expected %s\n", expected);
    assert(prepared);
    if (strcmp(cmd, expected) != 0) fprintf(stderr, "Command %s, expected %s\n", cmd, expected);
    assert(strcmp(cmd, expected) == 0);
    assert(timeout == (strstr(cmd, "COPS=1") || strcmp(cmd, "AT+COPS=0") == 0 ||
                       strcmp(cmd, "AT+COPS=?") == 0 ? 185000u : 5000u));
    assert(!modem_network_prepare(cmd, sizeof(cmd), &timeout));
}
static void response(const char *line) {
    assert(modem_network_line(line));
    modem_network_final(true, "OK", true);
}
static void reset(void) {
    modem_network_hooks_t hooks = {.read_owner = read_owner, .save_owner = save_owner};
    modem_network_init(&TELIT_NETWORK_BACKEND, &hooks);
    readable = writable = true;
    writes = 0u;
    owner[0] = '\0';
}
static void identify(void) {
    char id[MODEM_NETWORK_ID_CAP];
    command("AT#CCID");
    assert(!modem_network_identity(id, sizeof(id)) && id[0] == '\0');
    assert(modem_network_line("#CCID: 89014103211118510720"));
    assert(!modem_network_identity(id, sizeof(id)));
    modem_network_final(true, "OK", true);
    assert(modem_network_identity(id, sizeof(id)) && strcmp(id, CARD) == 0);
    assert(!modem_network_identity(id, 5u) && id[0] == '\0');
    command("AT+COPS=3,2;+COPS?");
}
static void ready(void) {
    reset(); identify(); response("+COPS: 0,2,\"310410\",7");
    assert(view().ready && !view().busy && strcmp(owner, CARD) == 0);
}
static void no_sim_metadata(void) {
    command("AT#CIMI"); modem_network_final(false, "ERROR", true);
    command("AT+CRSM=176,28589,0,0,4"); response("+CRSM: 106,130");
    command("AT+CRSM=192,28633"); response("+CRSM: 106,130");
    command("AT+CRSM=192,28512"); response("+CRSM: 106,130");
    command("AT+CRSM=192,28464"); response("+CRSM: 106,130");
    command("AT+CRSM=192,28513"); response("+CRSM: 106,130");
}
static void scan(const char *line) {
    assert(modem_network_request(MODEM_NETWORK_SCAN, 0, 0));
    command("AT+COPS=?"); response(line);
    if (view().busy) no_sim_metadata();
}
static void test_owner(void) {
    reset(); identify(); response("+COPS: 1,2,\"310410\",7");
    assert(view().busy && !view().ready && writes == 0u);
    command("AT+COPS=0"); modem_network_final(true, "OK", true);
    command("AT+COPS=3,2;+COPS?"); response("+COPS: 0");
    assert(view().ready && view().mode == MODEM_NETWORK_AUTOMATIC);
    assert(strcmp(owner, CARD) == 0);
    snprintf(owner, sizeof(owner), "%s:310410", CARD);
    modem_network_reset_session(); identify(); response("+COPS: 1,2,\"310410\",7");
    assert(view().ready && view().mode == MODEM_NETWORK_MANUAL);
    assert(!modem_network_work_pending());

    reset(); readable = false; command("AT#CCID"); response("#CCID: 89014103211118510720");
    assert(!view().ready && view().result == MODEM_NETWORK_NOT_DONE && writes == 0u);
    reset(); identify(); writable = false; response("+COPS: 0");
    assert(!view().ready && view().result == MODEM_NETWORK_NOT_DONE);
    reset(); identify(); response("+COPS: 5");
    assert(!view().ready && view().result == MODEM_NETWORK_NOT_DONE);
}

static void test_restore(void) {
    ready(); scan("+COPS: (2,\"AT&T\",\"\",\"310410\",7)");
    assert(modem_network_request(MODEM_NETWORK_SELECT_MANUAL, view().list_generation, 0));
    command("AT+COPS=1,2,\"310410\""); modem_network_final(true, "OK", true);
    assert(strcmp(owner, CARD) == 0); /* Not committed on OK alone. */
    command("AT+COPS=3,2;+COPS?"); response("+COPS: 1,2,\"310410\",7");
    assert(strcmp(owner, "89014103211118510720:310410") == 0);

    for (unsigned failure = 0u; failure < 2u; failure++) {
        modem_network_reset_session(); identify(); response("+COPS: 0,2,\"310410\",7");
        command("AT+COPS=1,2,\"310410\""); modem_network_final(!failure, failure ? "ERROR" : "OK", true);
        command("AT+COPS=3,2;+COPS?"); response(failure ? "+COPS: 1" : "+COPS: 1,2,\"310410\",7");
        assert(view().ready && !view().busy && !modem_network_work_pending());
        assert(view().result == (failure ? MODEM_NETWORK_NOT_DONE : MODEM_NETWORK_DONE));
        assert(strcmp(owner, "89014103211118510720:310410") == 0);
    }
    assert(modem_network_request(MODEM_NETWORK_SELECT_AUTO, 0, 0));
    command("AT+COPS=0"); modem_network_final(true, "OK", true);
    command("AT+COPS=3,2;+COPS?"); response("+COPS: 0");
    assert(strcmp(owner, CARD) == 0);

    snprintf(owner, sizeof(owner), "89014103211118510721:001001");
    modem_network_reset_session(); identify(); response("+COPS: 1,2,\"001001\",7");
    command("AT+COPS=0"); modem_network_final(true, "OK", true);
    command("AT+COPS=3,2;+COPS?"); response("+COPS: 0");
    assert(view().ready && strcmp(owner, CARD) == 0);

    static const char *const bad[] = {"garbage", "89014103211118510720:",
        "89014103211118510720:0010", "89014103211118510720:00100x"};
    for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); i++) {
        reset(); snprintf(owner, sizeof(owner), "%s", bad[i]);
        command("AT#CCID"); response("#CCID: 89014103211118510720");
        assert(!view().ready && view().result == MODEM_NETWORK_NOT_DONE && !modem_network_work_pending());
    }
    ready(); scan("+COPS: (2,\"AT&T\",\"\",\"310410\",7)");
    assert(modem_network_request(MODEM_NETWORK_SELECT_MANUAL, view().list_generation, 0));
    command("AT+COPS=1,2,\"310410\""); modem_network_final(true, "OK", true);
    writable = false;
    command("AT+COPS=3,2;+COPS?"); response("+COPS: 1,2,\"310410\",7");
    assert(!view().ready && view().result == MODEM_NETWORK_NOT_DONE && strcmp(owner, CARD) == 0);
}
static void test_scan(void) {
    ready();
    scan("+COPS: (1,\"other, network\",\"short\",\"00101\",7),(2,\"AT&T\",\"ATT\",\"310410\",7),(1,\"duplicate\",\"\",\"310410\",2),,(0-4),(0-2)");
    assert(view().count == 2 && view().result == MODEM_NETWORK_DONE);
    modem_network_entry_t entry;
    uint32_t gen = view().list_generation;
    assert(modem_network_entry(gen, 0, &entry) && strcmp(entry.plmn, "310410") == 0);
    assert(modem_network_entry(gen, 1, &entry) && strcmp(entry.plmn, "00101") == 0);
    assert(!modem_network_entry(gen + 1u, 0, &entry));
    assert(!modem_network_request(MODEM_NETWORK_SELECT_MANUAL, gen + 1u, 0));
    scan("+COPS: ,,(0-4),(0-2)");
    assert(view().result == MODEM_NETWORK_DONE && view().count == 0u);
    scan("+COPS:");
    assert(view().result == MODEM_NETWORK_DONE && view().count == 0u);
    static const char *bad[] = {
        "+COPS: (4,\"x\",\"x\",\"310410\",7)",
        "+COPS: (1,\"x\",\"x\",\"31041A\",7)",
        "+COPS: (1,\"x\",\"x\",\"3104\",7)",
        "+COPS: (1,\"x\",\"x\",\"3104100\",7)",
        "+COPS: (1,\"x\",\"x\",\"310410\",7),",
        "+COPS: (1,\"x\",\"x\",\"310410\",7),,(0-4)",
        "+COPS: (1,\"unterminated,\"x\",\"310410\",7)",
        "+COPS: (1,\"x\",\"x\",\"310410\",7) garbage",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        scan(bad[i]); assert(view().result == MODEM_NETWORK_NOT_DONE && view().count == 0u);
    }
    assert(modem_network_request(MODEM_NETWORK_SCAN, 0, 0)); command("AT+COPS=?");
    for (unsigned i = 0; i <= MODEM_NETWORK_MAX; i++) {
        char row[100]; snprintf(row, sizeof(row), "+COPS: (1,\"x\",\"\",\"%06u\",7)", i);
        assert(modem_network_line(row));
    }
    modem_network_final(true, "OK", true);
    assert(view().result == MODEM_NETWORK_NOT_DONE && view().count == 0u);
}
static void test_mutations(void) {
    ready(); scan("+COPS: (1,\"test\",\"\",\"001001\",7)");
    assert(modem_network_request(MODEM_NETWORK_SELECT_MANUAL, view().list_generation, 0));
    command("AT+COPS=1,2,\"001001\""); modem_network_final(true, "OK", true);
    assert(view().busy && !view().ready);
    command("AT+COPS=3,2;+COPS?"); response("+COPS: 1,2,\"001001\",7");
    assert(view().result == MODEM_NETWORK_DONE && view().ready);
    assert(modem_network_request(MODEM_NETWORK_SELECT_AUTO, 0, 0));
    command("AT+COPS=0"); modem_network_final(false, "+CME ERROR: 32", true);
    command("AT+COPS=3,2;+COPS?"); response("+COPS: 1,2,\"001001\",7");
    assert(view().result == MODEM_NETWORK_NO_ACCESS && view().ready);
    assert(modem_network_request(MODEM_NETWORK_SELECT_AUTO, 0, 0));
    command("AT+COPS=0"); modem_network_final(true, "OK", true);
    command("AT+COPS=3,2;+COPS?"); response("+COPS: 1,2,\"001001\",7");
    assert(view().result == MODEM_NETWORK_NOT_DONE);
    writable = false;
    assert(modem_network_request(MODEM_NETWORK_SELECT_AUTO, 0, 0));
    assert(!view().busy && view().result == MODEM_NETWORK_NOT_DONE);
}
static void test_cancellation(void) {
    ready(); assert(modem_network_request(MODEM_NETWORK_SCAN, 0, 0)); command("AT+COPS=?");
    modem_network_cancel(); assert(view().busy && modem_network_cancelled());
    assert(!modem_network_request(MODEM_NETWORK_SCAN, 0, 0));
    modem_network_final(true, "OK", true);
    assert(view().result == MODEM_NETWORK_CANCELLED && !view().busy);
    assert(!modem_network_work_pending());
    assert(modem_network_request(MODEM_NETWORK_SCAN, 0, 0)); command("AT+COPS=?");
    modem_network_timeout(); modem_network_final(false, "ERROR", false);
    assert(view().result == MODEM_NETWORK_NOT_DONE);
    assert(modem_network_request(MODEM_NETWORK_SELECT_AUTO, 0, 0)); command("AT+COPS=0");
    modem_network_final(true, "OK", true); modem_network_cancel();
    assert(!view().busy && !view().ready && modem_network_work_pending());
    identify(); response("+COPS: 0"); assert(view().ready);
    reset(); command("AT#CCID"); modem_network_cancel(); modem_network_final(false, "ERROR", false);
    assert(modem_network_work_pending()); identify(); response("+COPS: 0"); assert(view().ready);
    modem_network_invalidate_owner(); assert(!view().ready && view().mode == MODEM_NETWORK_UNKNOWN);
}

static void sim_scan(void) {
    ready();
    assert(modem_network_request(MODEM_NETWORK_SCAN, 0, 0));
    command("AT+COPS=?");
    response("+COPS: (2,\"roaming\",\"\",\"00101\",7),(1,\"user\",\"\",\"310260\",7),"
             "(1,\"home\",\"\",\"310410\",7),(1,\"operator\",\"\",\"001001\",7)");
    assert(view().busy);
}
static void sim_identity(const char *imsi, const char *ad) {
    command("AT#CIMI"); response(imsi);
    command("AT+CRSM=176,28589,0,0,4"); response(ad);
}
static void sim_info(unsigned file, unsigned size) {
    char cmd[80], line[100];
    snprintf(cmd, sizeof(cmd), "AT+CRSM=192,%u", file); command(cmd);
    snprintf(line, sizeof(line), "+CRSM: 144,0,620C820241218302%04X8002%04X", file, size);
    response(line);
}
static void sim_data(unsigned file, unsigned offset, const char *hex) {
    char cmd[80], line[300];
    snprintf(cmd, sizeof(cmd), "AT+CRSM=176,%u,%u,%u,%u", file, offset >> 8u,
             offset & 255u, (unsigned)strlen(hex) / 2u); command(cmd);
    snprintf(line, sizeof(line), "+CRSM: 144,0,\"%s\"", hex); response(line);
}
static void no_preferences(void) {
    command("AT+CRSM=192,28512"); response("+CRSM: 106,130");
    command("AT+CRSM=192,28464"); response("+CRSM: 148,4");
    command("AT+CRSM=192,28513"); response("+CRSM: 106,130");
}
static void expect_entry(uint8_t index, const char *plmn, bool home) {
    modem_network_entry_t entry;
    assert(modem_network_entry(view().list_generation, index, &entry));
    assert(strcmp(entry.plmn, plmn) == 0 && entry.home == home);
}
static void test_sim_priority(void) {
    sim_scan(); sim_identity("#CIMI: 310280123456789", "+CRSM: 144,0,00000003");
    sim_info(28633, 6); sim_data(28633, 0, "130014FFFFFF");
    sim_info(28512, 10); sim_data(28512, 0, "13006240001300144000");
    sim_info(28513, 10); sim_data(28513, 0, "00110040001300624000");
    assert(view().result == MODEM_NETWORK_DONE && view().ready);
    expect_entry(0, "310410", true); expect_entry(1, "310260", false);
    expect_entry(2, "001001", false); expect_entry(3, "00101", false);
    assert(modem_network_request(MODEM_NETWORK_SELECT_MANUAL, view().list_generation, 0));
    command("AT+COPS=1,2,\"310410\""); modem_network_final(true, "OK", true);
    assert(!view().home);
    command("AT+COPS=3,2;+COPS?"); response("+COPS: 1,2,\"310410\",7");
    assert(view().home && view().result == MODEM_NETWORK_DONE);
    assert(modem_network_request(MODEM_NETWORK_SELECT_AUTO, 0, 0));
    assert(!view().home);
    command("AT+COPS=0"); modem_network_final(true, "OK", true);
    command("AT+COPS=3,2;+COPS?"); response("+COPS: 0");
    uint32_t old_generation = view().list_generation;
    assert(modem_network_request(MODEM_NETWORK_REFRESH, 0, 0));
    assert(!view().count && !view().home && view().list_generation != old_generation);
    identify(); response("+COPS: 0");
    assert(!modem_network_request(MODEM_NETWORK_SELECT_MANUAL, old_generation, 0));
    modem_network_invalidate_owner(); assert(!view().home && !view().ready && !view().count);
}
static void test_sim_home(void) {
    for (unsigned scenario = 0u; scenario < 7u; scenario++) {
        sim_scan(); sim_identity("#CIMI: 001011234567890",
            scenario == 6u ? "+CRSM: 144,0,000000FF" : "+CRSM: 144,0,00000002");
        if (scenario == 0u || scenario == 6u) {
            command("AT+CRSM=192,28633"); response("+CRSM: 106,130");
        } else if (scenario == 1u) sim_info(28633, 0);
        else if (scenario == 2u) { sim_info(28633, 3); sim_data(28633, 0, "FFFFFF"); }
        else if (scenario == 3u) { sim_info(28633, 3); sim_data(28633, 0, "001100"); }
        else if (scenario == 4u) {
            sim_info(28633, 3); command("AT+CRSM=176,28633,0,0,3");
            modem_network_final(false, "ERROR", true);
        } else { sim_info(28633, 6); sim_data(28633, 0, "00F110AABBCC"); }
        no_preferences();
        assert(view().result == MODEM_NETWORK_DONE);
        expect_entry(0, scenario == 3u ? "001001" : "00101", scenario <= 3u);
        if (scenario == 3u) expect_entry(1, "00101", false); /* EHPLMN replaces IMSI home. */
    }
    sim_scan(); sim_identity("#CIMI: 001001123456789", "+CRSM: 144,0,00000003");
    command("AT+CRSM=192,28633"); response("+CRSM: 106,130"); no_preferences();
    expect_entry(0, "001001", true); expect_entry(1, "00101", false);
}
static void test_sim_chunks(void) {
    sim_scan(); sim_identity("#CIMI: 310280123456789", "+CRSM: 144,0,00000003");
    sim_info(28633, 3); sim_data(28633, 0, "130014");
    command("AT+CRSM=192,28512"); response("+CRSM: 106,130");
    command("AT+CRSM=192,28464");
    response("+CRSM: 144,0,000000036F30040000000000020000"); /* Classic SIM response. */
    sim_data(28464, 0, "130062");
    sim_info(28513, 365);
    char padding[241]; memset(padding, 'F', 240u); padding[240] = '\0';
    for (unsigned offset = 0u; offset < 360u; offset += 120u) sim_data(28513, offset, padding);
    sim_data(28513, 360, "0011004000");
    expect_entry(0, "310410", true); expect_entry(1, "310260", false);
    expect_entry(2, "001001", false); expect_entry(3, "00101", false);

    sim_scan(); sim_identity("#CIMI: 310280123456789", "+CRSM: 144,0,00000003");
    sim_info(28633, 123);
    memcpy(padding, "130014", 6u); sim_data(28633, 0, padding);
    command("AT+CRSM=176,28633,0,120,3");
    assert(modem_network_line("+CRSM: 144,0,FFFFFF"));
    modem_network_final(true, "OK", false); /* Entire file, not just last chunk, discarded. */
    no_preferences(); expect_entry(0, "00101", false); expect_entry(2, "310410", false);

    sim_scan(); command("AT#CIMI"); modem_network_cancel();
    modem_network_final(true, "OK", true);
    assert(view().result == MODEM_NETWORK_CANCELLED && view().count == 0u && !modem_network_work_pending());
}

static void test_sim_parsing(void) {
    modem_network_sim_query_t query = {.kind = MODEM_NETWORK_SIM_INFO, .file = MODEM_NETWORK_SIM_HOME};
    modem_network_sim_reply_t reply;
    static const char *const bad[] = {
        "+CRSM: 144,0,620C8202412183026FD98002000300", /* Extra byte. */
        "+CRSM: 144,0,620C8202422183026FD980020003", /* Record EF. */
        "+CRSM: 144,0,620C8202412183026F6080020003", /* Wrong file. */
        "+CRSM: 144,0,620C8202412183026FD981020003", /* No file-data size. */
        "+CRSM: 144,0,620C8202412183026FD980020004X",
        "+CRSM: 144,0,620C8202412183026FD980020003,1",
        "+CRSM: 144,0,\"620C8202412183026FD980020003",
        "+CRSM: 144,0,620C8202412183026FD980820003",
        "+CRSM: 144,0", "+CRSM: 106,130,FFFF", "+CRSM: 152,4",
        "+CRSM: 144,0,000000036FD9040000000000020100", /* Not transparent. */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        assert(TELIT_NETWORK_BACKEND.sim_read(&query, bad[i], &reply) == MODEM_NETWORK_LINE_INVALID);
    assert(TELIT_NETWORK_BACKEND.sim_read(&query,
        "+CRSM: 144,0,62238202412183026FD9A503C001008A01058B066F06010900008002000F810200238801E8",
        &reply) == MODEM_NETWORK_LINE_VALID && reply.file_size == 15u);
    assert(TELIT_NETWORK_BACKEND.sim_read(&query,
        "+CRSM: 145,18,620C8202412183026FD980020003", &reply) == MODEM_NETWORK_LINE_VALID &&
        reply.file_size == 3u);
}

static void test_sim_failures(void) {
    for (unsigned scenario = 0u; scenario < 5u; scenario++) {
        sim_scan(); sim_identity("#CIMI: 001011234567890", "+CRSM: 144,0,00000002");
        if (scenario == 0u) sim_info(28633, 4098); /* Bounded, no partial home list. */
        else if (scenario == 1u) sim_info(28633, 4); /* Misaligned PLMN records. */
        else if (scenario == 2u) {
            command("AT+CRSM=192,28633"); modem_network_final(true, "OK", true);
        } else if (scenario == 3u) {
            command("AT+CRSM=192,28633");
            assert(modem_network_line("+CRSM: 106,130"));
            response("+CRSM: 106,130"); /* Duplicate response is not verified absence. */
        } else {
            sim_info(28633, 3); command("AT+CRSM=176,28633,0,0,3");
            response("+CRSM: 144,0,00F1"); /* Truncated data. */
        }
        sim_info(28512, 10); sim_data(28512, 0, "1300624000AABBCC4000");
        command("AT+CRSM=192,28464"); response("+CRSM: 106,130");
        command("AT+CRSM=192,28513"); modem_network_final(false, "ERROR", true);
        assert(view().result == MODEM_NETWORK_DONE && view().ready && !view().busy);
        expect_entry(0, "00101", false); expect_entry(1, "310260", false);
    }
    sim_scan(); sim_identity("#CIMI: 310280123456789", "+CRSM: 144,0,00000003");
    sim_info(28633, 3); sim_data(28633, 0, "130014");
    command("AT+CRSM=192,28512"); modem_network_invalidate_owner();
    assert(!modem_network_line("+CRSM: 106,130"));
    modem_network_final(true, "OK", true);
    assert(!view().home && !view().count && !view().ready);
    identify(); response("+COPS: 0");
    scan("+COPS: (2,\"AT&T\",\"\",\"310410\",7)");
    expect_entry(0, "310410", false); /* Earlier SIM's home flag is not reused. */
}
int main(void) {
    test_owner(); test_restore(); test_scan(); test_mutations(); test_cancellation();
    test_sim_priority(); test_sim_home(); test_sim_chunks(); test_sim_parsing();
    test_sim_failures();
    puts("network state/adapter tests passed"); return 0;
}
