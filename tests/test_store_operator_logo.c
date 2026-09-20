#include "storage/store_operator_logo.h"
#include "store_service_internal.h"
#include "services/sms_deliver_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static store_diag_snapshot_t diag;
static uint32_t now;
static bool fail_admission;
static uint8_t disk[4096];
static size_t disk_len;
uint32_t time_ms(void) { return now; }
bool store_service_ready(void) { return diag.ready; }
bool store_service_unit_ready(store_unit_t unit) {
    return diag.ready && !(diag.unavailable_mask & (1u << unit));
}
void store_service_get_diag(store_diag_snapshot_t *out) { *out = diag; }
store_status_t store_engine_mark_dirty(store_unit_t unit) {
    assert(unit == STORE_UNIT_OPERATOR_LOGO);
    if (!diag.ready) return STORE_STATUS_NOT_READY;
    if (fail_admission) return STORE_STATUS_STORAGE_ERROR;
    diag.dirty_mask |= 1u << unit;
    return STORE_STATUS_OK;
}
static void commit(void) {
    assert(g_store_operator_logo_unit_ops.serialize(0u, disk, sizeof(disk), &disk_len));
    diag.dirty_mask = 0u;
}
static void reboot(void) {
    g_store_operator_logo_unit_ops.reset_ram(0u);
    diag.dirty_mask = 0u;
    assert(g_store_operator_logo_unit_ops.apply(0u, disk, disk_len));
}
static sms_codec_message_t part;
static void make_part(const uint8_t *data, size_t len, unsigned seq, unsigned total, unsigned generation) {
    memset(&part, 0, sizeof(part));
    part.binary = part.has_ports = true;
    part.dest_port = OPERATOR_LOGO_PORT;
    part.dcs = 0xf5u;
    part.has_concat = total > 1u;
    part.concat_ref = 11u;
    part.concat_total = total;
    part.concat_seq = seq;
    strcpy(part.address, "+15551234567");
    snprintf(part.timestamp, sizeof(part.timestamp), "26/09/20,12:%02u:00", generation);
    part.binary_len = len;
    memcpy(part.binary_data, data, len);
}

int main(void) {
    diag.ready = true;
    g_store_operator_logo_unit_ops.reset_ram(0u);
    commit();
    operator_logo_t logo;
    assert(store_operator_logo_get(&logo) == STORE_STATUS_NOT_FOUND);
    uint8_t payload[135] = {0x30,0x13,0x01,0x84,0x0a,0,72,14,1};
    payload[9] = 0xa5u;
    for (unsigned reverse = 0u; reverse < 2u; reverse++) {
        for (unsigned i = 0u; i < 2u; i++) {
            unsigned seq = reverse ? 2u - i : i + 1u;
            make_part(payload + (seq == 2u ? 128u : 0u), seq == 2u ? 7u : 128u,
                      seq, 2u, reverse);
            assert(store_operator_logo_receive(&part, ++now) == STORE_STATUS_OK);
            assert(store_operator_logo_pending_first() == 0u);
            commit(); reboot();
        }
        uint32_t id = store_operator_logo_pending_first();
        assert(id != 0u);
        assert(store_operator_logo_pending_get(id, &logo) == STORE_STATUS_OK);
        assert(logo.pixels[0] == 0xa5u && operator_logo_matches(&logo, "311", "480"));
        assert(store_operator_logo_receive(&part, ++now) == STORE_STATUS_OK && diag.dirty_mask == 0u);
        part.binary_data[0] ^= 1u;
        assert(store_operator_logo_receive(&part, ++now) == STORE_STATUS_CONFLICT);
        assert(store_operator_logo_pending_first() == id);
        fail_admission = true;
        assert(store_operator_logo_pending_save(id) == STORE_STATUS_STORAGE_ERROR);
        fail_admission = false;
        assert(store_operator_logo_pending_save(id) == STORE_STATUS_OK);
        if (!reverse) assert(store_operator_logo_get(&logo) == STORE_STATUS_NOT_FOUND);
        commit(); reboot();
        assert(store_operator_logo_pending_first() == 0u);
        assert(store_operator_logo_get(&logo) == STORE_STATUS_OK && logo.pixels[0] == 0xa5u);
    }
    /* The old saved image survives a failing replacement and reboot. */
    uint8_t compact[133] = {0x13,0x00,0x62,0,72,14,1};
    compact[7] = 0x5au;
    make_part(compact, sizeof(compact), 1u, 1u, 3u);
    assert(store_operator_logo_receive(&part, ++now) == STORE_STATUS_OK);
    commit();
    uint32_t id = store_operator_logo_pending_first();
    assert(store_operator_logo_pending_save(id) == STORE_STATUS_OK);
    diag.degraded_mask = 1u << STORE_UNIT_OPERATOR_LOGO;
    assert(store_operator_logo_get(&logo) == STORE_STATUS_OK && logo.pixels[0] == 0xa5u);
    diag.degraded_mask = 0u;
    reboot();
    assert(store_operator_logo_pending_first() == id);
    assert(store_operator_logo_get(&logo) == STORE_STATUS_OK && logo.pixels[0] == 0xa5u);
    assert(store_operator_logo_pending_discard(id) == STORE_STATUS_OK);
    commit(); reboot();
    assert(store_operator_logo_pending_first() == 0u);
    assert(store_operator_logo_get(&logo) == STORE_STATUS_OK && logo.pixels[0] == 0xa5u);

    /* Ready records cannot be overwritten by saturation. Partial data expires. */
    make_part(payload, 128u, 1u, 2u, 4u);
    assert(store_operator_logo_receive(&part, ++now) == STORE_STATUS_OK); commit();
    now += 1800001u;
    store_operator_logo_expire(now); commit(); reboot();
    make_part(payload + 128u, 7u, 2u, 2u, 4u);
    assert(store_operator_logo_receive(&part, ++now) == STORE_STATUS_OK); commit();
    assert(store_operator_logo_pending_first() == 0u);
    make_part(payload, 128u, 1u, 2u, 4u);
    assert(store_operator_logo_receive(&part, ++now) == STORE_STATUS_OK); commit();
    id = store_operator_logo_pending_first(); assert(id != 0u);
    make_part(compact, sizeof(compact), 1u, 1u, 5u);
    assert(store_operator_logo_receive(&part, ++now) == STORE_STATUS_OK); commit();
    make_part(compact, sizeof(compact), 1u, 1u, 6u);
    assert(store_operator_logo_receive(&part, ++now) == STORE_STATUS_STORAGE_ERROR);
    assert(store_operator_logo_pending_first() == id);
    assert(!g_store_operator_logo_unit_ops.apply(0u, disk, disk_len - 1u));
    assert(!g_store_operator_logo_unit_ops.serialize(0u, disk, 1u, &disk_len));

    /* A corrupt record is rejected without changing the current RAM state. */
    commit();
    disk[7] = 0xaau;
    assert(!g_store_operator_logo_unit_ops.apply(0u, disk, disk_len));
    assert(store_operator_logo_get(&logo) == STORE_STATUS_OK && logo.pixels[0] == 0xa5u);

    /* Recovery authorizes deletion only for the exact, committed PDU. */
    g_store_operator_logo_unit_ops.reset_ram(0u); commit();
    sms_deliver_t deliver = {.dcs = 0xf5u, .udhi = true, .udl = 140u, .ud_len = 140u};
    strcpy(deliver.address, "+15551234567");
    assert(sms_deliver_scts_encode(2026u, 9u, 20u, 12u, 0u, 0u, 0, deliver.scts));
    const uint8_t ports[] = {6u,5u,4u,0x15u,0x82u,0u,0u};
    memcpy(deliver.ud, ports, sizeof(ports));
    memcpy(deliver.ud + sizeof(ports), compact, sizeof(compact));
    char pdu[SMS_DELIVER_HEX_MAX], different[SMS_DELIVER_HEX_MAX];
    uint8_t tpdu_len;
    assert(sms_deliver_build(&deliver, pdu, sizeof(pdu), &tpdu_len));
    assert(store_operator_logo_received_pdu_status(pdu) == STORE_STATUS_NOT_FOUND);
    assert(store_operator_logo_receive_pdu(pdu, ++now) == STORE_STATUS_OK);
    assert(store_operator_logo_received_pdu_status(pdu) == STORE_STATUS_NOT_READY);
    commit(); reboot();
    assert(store_operator_logo_received_pdu_status(pdu) == STORE_STATUS_OK);
    id = store_operator_logo_pending_first();
    deliver.ud[14] ^= 1u;
    assert(sms_deliver_build(&deliver, different, sizeof(different), &tpdu_len));
    assert(store_operator_logo_received_pdu_status(different) == STORE_STATUS_NOT_FOUND);
    assert(store_operator_logo_receive_pdu(different, ++now) == STORE_STATUS_CONFLICT);
    assert(store_operator_logo_pending_first() == id);
    assert(store_operator_logo_pending_discard(id) == STORE_STATUS_OK); commit(); reboot();
    assert(store_operator_logo_received_pdu_status(pdu) == STORE_STATUS_OK);
    assert(store_operator_logo_receive_pdu(pdu, ++now) == STORE_STATUS_OK);
    assert(store_operator_logo_pending_first() == 0u);
    assert(store_operator_logo_receive_pdu("bad", ++now) == STORE_STATUS_NOT_FOUND);
    puts("operator logo store passed");
    return 0;
}
