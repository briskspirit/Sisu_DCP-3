#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "services/modem_network.h"

extern const modem_network_backend_t TELIT_NETWORK_BACKEND;
static char owner[MODEM_NETWORK_ID_CAP];
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
    assert(modem_network_prepare(cmd, sizeof(cmd), &timeout));
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
    command("AT#CCID");
    response("#CCID: 89014103211118510720");
    command("AT+COPS=3,2;+COPS?");
}
static void ready(void) {
    reset(); identify(); response("+COPS: 0,2,\"310410\",7");
    assert(view().ready && !view().busy && strcmp(owner, CARD) == 0);
}
static void scan(const char *line) {
    assert(modem_network_request(MODEM_NETWORK_SCAN, 0, 0));
    command("AT+COPS=?"); response(line);
}
static void test_owner(void) {
    reset(); identify(); response("+COPS: 1,2,\"310410\",7");
    assert(view().busy && !view().ready && writes == 0u);
    command("AT+COPS=0"); modem_network_final(true, "OK", true);
    command("AT+COPS=3,2;+COPS?"); response("+COPS: 0");
    assert(view().ready && view().mode == MODEM_NETWORK_AUTOMATIC);
    assert(strcmp(owner, CARD) == 0);
    modem_network_reset_session(); identify(); response("+COPS: 1,2,\"310410\",7");
    assert(view().ready && view().mode == MODEM_NETWORK_MANUAL);
    assert(!modem_network_work_pending());

    reset(); identify(); readable = false; response("+COPS: 1,2,\"310410\",7");
    assert(!view().ready && view().result == MODEM_NETWORK_NOT_DONE && writes == 0u);
    reset(); identify(); writable = false; response("+COPS: 0");
    assert(!view().ready && view().result == MODEM_NETWORK_NOT_DONE);
    reset(); identify(); response("+COPS: 5");
    assert(!view().ready && view().result == MODEM_NETWORK_NOT_DONE);
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
int main(void) {
    test_owner(); test_scan(); test_mutations(); test_cancellation();
    puts("network state/adapter tests passed"); return 0;
}
