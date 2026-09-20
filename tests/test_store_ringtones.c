#include "audio/composer_codec.h"
#include "audio/ringtone_codec.h"
#include "storage/store_service.h"
#include "store_service_internal.h"
#include "services/sms_deliver_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static store_diag_snapshot_t diag;
static bool fail_writes;
static uint32_t now;
uint32_t time_ms(void) { return now; }
bool store_service_ready(void) { return diag.ready; }
void store_service_get_diag(store_diag_snapshot_t *out) { *out = diag; }
store_status_t store_engine_mark_dirty(store_unit_t unit) {
    assert(unit == STORE_UNIT_OWN_TONES);
    if (!diag.ready) return STORE_STATUS_NOT_READY;
    if (fail_writes) return STORE_STATUS_STORAGE_ERROR;
    diag.dirty_mask |= 1u << unit;
    return STORE_STATUS_OK;
}
static uint8_t disk[4096];
static size_t disk_len;
static void commit(void) {
    assert(g_store_tones_unit_ops.serialize(0u, disk, sizeof(disk), &disk_len));
    diag.dirty_mask = 0u;
}
static void reboot(void) {
    g_store_tones_unit_ops.reset_ram(0u);
    diag.dirty_mask = 0u;
    assert(g_store_tones_unit_ops.apply(0u, disk, disk_len));
}
static sms_codec_message_t part;
static void make_part(const uint8_t *data, uint16_t len, uint8_t seq, uint8_t total) {
    memset(&part, 0, sizeof(part));
    part.binary = part.has_ports = true;
    part.dest_port = RINGTONE_SMS_PORT;
    part.dcs = 0xf5u;
    part.has_concat = total > 1u;
    part.concat_ref = 11u;
    part.concat_total = total; part.concat_seq = seq;
    strcpy(part.address, "+15551234567");
    strcpy(part.timestamp, "26/09/20,12:00:00");
    part.binary_len = len;
    memcpy(part.binary_data, data, len);
}

static void make_generation_part(const uint8_t *data, uint16_t len, unsigned seq, bool newer) {
    make_part(data + (seq == 2u ? 128u : 0u), seq == 2u ? len - 128u : 128u, seq, 2u);
    snprintf(part.timestamp, sizeof(part.timestamp), "26/09/20,12:%02u:%02u",
             newer ? 2u : 0u, seq == 2u ? 5u : 0u);
}

static void part_pdu(char *pdu, size_t cap) {
    sms_deliver_t deliver = {.dcs=0xf5u, .udhi=true};
    strcpy(deliver.address, part.address);
    unsigned year, month, day, hour, minute, second;
    assert(sscanf(part.timestamp, "%u/%u/%u,%u:%u:%u",
                  &year, &month, &day, &hour, &minute, &second) == 6);
    assert(sms_deliver_scts_encode(2000u + year, month, day, hour, minute, second, 0, deliver.scts));
    uint8_t udh[] = {11u, 5u, 4u, 0x15u, 0x81u, 0u, 0u, 0u, 3u,
        (uint8_t)part.concat_ref, part.concat_total, part.concat_seq};
    memcpy(deliver.ud, udh, sizeof(udh));
    memcpy(deliver.ud + sizeof(udh), part.binary_data, part.binary_len);
    deliver.udl = deliver.ud_len = (uint8_t)(sizeof(udh) + part.binary_len);
    uint8_t tpdu_len;
    assert(sms_deliver_build(&deliver, pdu, cap, &tpdu_len));
}

static void test_reference_reuse(void) {
    /* Exercise an unread, saved, or discarded old receipt, both part orders,
     * and a new melody with identical bytes, changed prefix, or changed tail. */
    for (unsigned old_state = 0u; old_state < 3u; old_state++) {
        for (unsigned change = 0u; change < 3u; change++) {
            for (unsigned first = 1u; first <= 2u; first++) {
                g_store_tones_unit_ops.reset_ram(0u);
                diag.dirty_mask = 0u; now = 100u;
                composer_note_event_t notes[96];
                for (unsigned i = 0u; i < 96u; i++)
                    notes[i] = (composer_note_event_t){1u, 1u, 2u, false};
                uint8_t old_data[256], new_data[256]; uint16_t old_len, new_len;
                assert(composer_codec_encode("Reference", notes, 96u, 12u,
                                             old_data, sizeof(old_data), &old_len));
                if (change != 0u) notes[change == 1u ? 0u : 95u].pitch_code = 2u;
                assert(composer_codec_encode("Reference", notes, 96u, 12u,
                                             new_data, sizeof(new_data), &new_len));
                for (unsigned seq = 2u; seq != 0u; seq--) {
                    make_generation_part(old_data, old_len, seq, false);
                    assert(store_ringtone_receive(&part, now++) == STORE_STATUS_OK);
                    commit(); reboot();
                }
                uint32_t old_id = store_ringtone_pending_first();
                assert(old_id != 0u);
                if (old_state == 1u) assert(store_ringtone_pending_save(old_id) == STORE_STATUS_OK);
                if (old_state == 2u) assert(store_ringtone_pending_discard(old_id) == STORE_STATUS_OK);
                commit(); reboot();
                now = 120000u;
                make_generation_part(new_data, new_len, first, true);
                char new_pdu[SMS_DELIVER_HEX_MAX];
                part_pdu(new_pdu, sizeof(new_pdu));
                assert(store_ringtone_received_pdu_status(new_pdu) == STORE_STATUS_NOT_FOUND);
                fail_writes = true;
                assert(store_ringtone_receive_pdu(new_pdu, now) == STORE_STATUS_STORAGE_ERROR);
                fail_writes = false;
                assert(store_ringtone_receive_pdu(new_pdu, now) == STORE_STATUS_OK);
                assert(store_ringtone_received_pdu_status(new_pdu) == STORE_STATUS_NOT_READY);
                commit(); reboot();
                assert(store_ringtone_received_pdu_status(new_pdu) == STORE_STATUS_OK);

                for (unsigned seq = 1u; seq <= 2u; seq++) {
                    make_generation_part(old_data, old_len, seq, false);
                    char old_pdu[SMS_DELIVER_HEX_MAX];
                    part_pdu(old_pdu, sizeof(old_pdu));
                    assert(store_ringtone_receive_pdu(old_pdu, now) == STORE_STATUS_OK);
                    assert(diag.dirty_mask == 0u);
                    assert(store_ringtone_received_pdu_status(old_pdu) == STORE_STATUS_OK);
                }
                make_generation_part(new_data, new_len, 3u - first, true);
                assert(store_ringtone_receive(&part, now) == STORE_STATUS_OK);
                commit(); reboot();
                if (old_state == 0u) {
                    assert(store_ringtone_pending_first() == old_id);
                    assert(store_ringtone_pending_discard(old_id) == STORE_STATUS_OK);
                    commit();
                }
                uint32_t new_id = store_ringtone_pending_first();
                assert(new_id != 0u && new_id != old_id);
                store_own_tone_t tone;
                assert(store_ringtone_pending_get(new_id, &tone) == STORE_STATUS_OK);
                assert(tone.packed_len == new_len && !memcmp(tone.packed, new_data, new_len));
                part.binary_data[0] ^= 1u;
                assert(store_ringtone_receive(&part, now) == STORE_STATUS_CONFLICT);
                assert(store_ringtone_pending_first() == new_id && diag.dirty_mask == 0u);
                if (old_state == 1u) {
                    assert(store_own_tone_get(1u, &tone) == STORE_STATUS_OK);
                    assert(tone.packed_len == old_len && !memcmp(tone.packed, old_data, old_len));
                }
            }
        }
    }
}

static void test_legacy_receipts(void) {
    const size_t slots_end = 8u + STORE_OWN_TONE_SLOT_COUNT *
        (7u + STORE_OWN_TONE_NAME_MAX + 1u + STORE_OWN_TONE_NOTES_MAX + 1u + STORE_OWN_TONE_PACKED_MAX);
    const size_t metadata = 13u + MODEM_SMS_SEGMENT_MAX;
    const size_t timestamps_size = 4u * MODEM_SMS_SEGMENT_MAX;
    const size_t old_record_size = metadata + MODEM_SMS_SENDER_MAX + 1u +
        MODEM_SMS_TIMESTAMP_MAX + 1u + STORE_OWN_TONE_PACKED_MAX;
    for (unsigned state = 0u; state < 3u; state++) {
        g_store_tones_unit_ops.reset_ram(0u);
        diag.dirty_mask = 0u; now = 100u;
        composer_note_event_t notes[96];
        for (unsigned i = 0u; i < 96u; i++) notes[i] = (composer_note_event_t){1u, 1u, 2u, false};
        uint8_t data[256]; uint16_t len;
        assert(composer_codec_encode("Legacy", notes, 96u, 12u, data, sizeof(data), &len));
        make_generation_part(data, len, 2u, false);
        assert(store_ringtone_receive(&part, now) == STORE_STATUS_OK);
        if (state != 0u) {
            make_generation_part(data, len, 1u, false);
            assert(store_ringtone_receive(&part, now) == STORE_STATUS_OK);
        }
        commit();
        uint32_t id = store_ringtone_pending_first();
        if (state == 2u) { assert(store_ringtone_pending_save(id) == STORE_STATUS_OK); commit(); }

        /* Recreate the previous branch's RNT1 record, which lacks part timestamps. */
        uint8_t legacy[4096];
        assert(memcmp(disk + slots_end, "RNT2", 4u) == 0);
        memcpy(legacy, disk, slots_end + 8u);
        legacy[slots_end + 3u] = '1';
        for (unsigned i = 0u; i < 2u; i++) {
            size_t src = slots_end + 8u + i * (old_record_size + timestamps_size);
            size_t dst = slots_end + 8u + i * old_record_size;
            memcpy(legacy + dst, disk + src, metadata);
            memcpy(legacy + dst + metadata, disk + src + metadata + timestamps_size,
                   old_record_size - metadata);
        }
        disk_len = slots_end + 8u + 2u * old_record_size;
        memcpy(disk, legacy, disk_len);
        reboot();
        if (state == 1u) assert(store_ringtone_pending_first() == id);
        if (state == 2u) assert(store_ringtone_pending_first() == 0u && store_own_tone_used(1u));
        make_generation_part(data, len, 2u, false);
        assert(store_ringtone_receive(&part, now) == STORE_STATUS_OK && diag.dirty_mask == 0u);
        if (state == 0u) {
            make_generation_part(data, len, 1u, false);
            assert(store_ringtone_receive(&part, now) == STORE_STATUS_OK);
        }
        commit(); reboot();
        store_own_tone_t tone;
        if (state == 2u) assert(store_own_tone_get(1u, &tone) == STORE_STATUS_OK);
        else assert(store_ringtone_pending_get(store_ringtone_pending_first(), &tone) == STORE_STATUS_OK);
        assert(tone.packed_len == len && memcmp(tone.packed, data, len) == 0);
    }
}

static void test_reference_reuse_changes_part_count(void) {
    g_store_tones_unit_ops.reset_ram(0u);
    diag.dirty_mask = 0u; now = 100u;
    composer_note_event_t notes[96];
    for (unsigned i = 0u; i < 96u; i++) notes[i] = (composer_note_event_t){1u, 1u, 2u, false};
    uint8_t data[256]; uint16_t len;
    assert(composer_codec_encode("New parts", notes, 96u, 12u, data, sizeof(data), &len));
    for (unsigned seq = 1u; seq <= 2u; seq++) {
        make_generation_part(data, len, seq, false);
        assert(store_ringtone_receive(&part, now) == STORE_STATUS_OK);
    }
    commit();
    uint32_t old_id = store_ringtone_pending_first();
    assert(old_id != 0u);
    /* The new generation starts with a part absent from the old receipt. */
    for (unsigned seq = 3u; seq != 0u; seq--) {
        unsigned offset = (seq - 1u) * 90u;
        make_part(data + offset, seq == 3u ? len - offset : 90u, seq, 3u);
        strcpy(part.timestamp, "26/09/20,12:02:00");
        assert(store_ringtone_receive(&part, now) == STORE_STATUS_OK);
        commit(); reboot();
    }
    assert(store_ringtone_pending_first() == old_id);
    assert(store_ringtone_pending_discard(old_id) == STORE_STATUS_OK);
    commit();
    uint32_t new_id = store_ringtone_pending_first();
    store_own_tone_t tone;
    assert(new_id != 0u && new_id != old_id);
    assert(store_ringtone_pending_get(new_id, &tone) == STORE_STATUS_OK);
    assert(tone.packed_len == len && memcmp(tone.packed, data, len) == 0);
}
int main(void) {
    diag.ready = true;
    g_store_tones_unit_ops.reset_ram(0u);
    composer_note_event_t notes[96];
    for (unsigned i = 0u; i < 96u; i++) notes[i] = (composer_note_event_t){1u, 1u, 2u, false};
    uint8_t data[256]; uint16_t len;
    assert(composer_codec_encode("Received", notes, 96u, 12u, data, sizeof(data), &len));
    assert(len > 133u && len < 256u);
    make_part(data + 128u, len - 128u, 2u, 2u);
    assert(store_ringtone_receive(&part, 10u) == STORE_STATUS_OK);
    assert(store_ringtone_pending_first() == 0u);
    commit(); reboot();
    assert(store_ringtone_receive(&part, 20u) == STORE_STATUS_OK && diag.dirty_mask == 0u);
    make_part(data, 128u, 1u, 2u);
    assert(store_ringtone_receive(&part, 30u) == STORE_STATUS_OK);
    assert(store_ringtone_pending_first() == 0u);
    commit(); reboot();
    uint32_t id = store_ringtone_pending_first();
    assert(id != 0u);
    store_own_tone_t tone;
    assert(store_ringtone_pending_get(id, &tone) == STORE_STATUS_OK);
    assert(tone.packed_len == len && !memcmp(tone.packed, data, len));
    fail_writes = true;
    assert(store_ringtone_pending_save(id) == STORE_STATUS_STORAGE_ERROR);
    assert(!store_own_tone_used(1u) && store_ringtone_pending_first() == id);
    fail_writes = false;
    assert(store_ringtone_pending_save(id) == STORE_STATUS_OK);
    /* Power failure before commit keeps the unread melody, not a false save. */
    reboot(); assert(store_ringtone_pending_first() == id && !store_own_tone_used(1u));
    assert(store_ringtone_pending_save(id) == STORE_STATUS_OK);
    commit(); reboot();
    assert(store_ringtone_pending_first() == 0u && store_own_tone_used(1u) && !store_own_tone_used(0u));
    assert(store_ringtone_receive(&part, 40u) == STORE_STATUS_OK && diag.dirty_mask == 0u);
    assert(store_own_tone_get(1u, &tone) == STORE_STATUS_OK && tone.packed_len == len);

    /* Malformed tones are recognized but never offered as ordinary SMS/tones. */
    uint8_t invalid[2] = {2u, 0u};
    make_part(invalid, sizeof(invalid), 1u, 1u);
    part.concat_ref = 12u;
    assert(store_ringtone_receive(&part, 50u) == STORE_STATUS_INVALID_ARGUMENT);
    commit(); reboot();
    assert(store_ringtone_pending_first() == 0u);
    now = 1800100u;
    store_ringtone_expire(now); commit();
    store_ringtone_expire(now); commit();
    make_part(data, 128u, 1u, 2u);
    assert(store_ringtone_receive(&part, now) == STORE_STATUS_OK);
    commit();
    now += 1800001u;
    store_ringtone_expire(now); commit(); reboot();
    make_part(data + 128u, len - 128u, 2u, 2u);
    assert(store_ringtone_receive(&part, now) == STORE_STATUS_OK);
    commit();
    assert(store_ringtone_pending_first() == 0u); /* old half really expired */
    make_part(data, 128u, 1u, 2u);
    assert(store_ringtone_receive(&part, now) == STORE_STATUS_OK);
    commit();
    id = store_ringtone_pending_first();
    part.binary_data[0] ^= 1u;
    assert(store_ringtone_receive(&part, now) == STORE_STATUS_CONFLICT);
    assert(store_ringtone_pending_first() == id); /* collision cannot destroy complete tone */
    assert(store_ringtone_pending_discard(id) == STORE_STATUS_OK);
    commit(); reboot(); assert(store_ringtone_pending_first() == 0u);
    assert(store_own_tone_used(1u)); /* Discard does not erase saved ringtone. */

    /* Every truncated extension is rejected without partially applying slots. */
    for (size_t n = 1121u; n < disk_len; n++) assert(!g_store_tones_unit_ops.apply(0u, disk, n));

    g_store_tones_unit_ops.reset_ram(0u);
    make_part(data, 128u, 1u, 2u);
    strcpy(part.timestamp, "26/09/30,23:59:50");
    assert(store_ringtone_receive(&part, now) == STORE_STATUS_OK);
    commit(); reboot();
    make_part(data + 128u, len - 128u, 2u, 2u);
    strcpy(part.timestamp, "26/10/01,00:00:05");
    assert(store_ringtone_receive(&part, now) == STORE_STATUS_OK);
    commit();
    assert(store_ringtone_pending_first() != 0u); /* crosses month/day boundary */

    sms_deliver_t deliver = {.dcs=0xf5u, .udhi=true};
    strcpy(deliver.address, "+15550000002");
    assert(sms_deliver_scts_encode(2026u, 10u, 1u, 1u, 0u, 0u, 0, deliver.scts));
    assert(composer_codec_encode("PDU", notes, 3u, 12u, data, sizeof(data), &len));
    const uint8_t udh[] = {6u, 5u, 4u, 0x15u, 0x81u, 0u, 0u};
    memcpy(deliver.ud, udh, sizeof(udh));
    memcpy(deliver.ud + sizeof(udh), data, len);
    deliver.udl = deliver.ud_len = (uint8_t)(sizeof(udh) + len);
    char pdu[SMS_DELIVER_HEX_MAX];
    uint8_t tpdu_len;
    assert(sms_deliver_build(&deliver, pdu, sizeof(pdu), &tpdu_len));
    assert(store_ringtone_receive_pdu(pdu, now) == STORE_STATUS_OK);
    assert(store_ringtone_received_pdu_status(pdu) == STORE_STATUS_NOT_READY);
    commit(); reboot();
    assert(store_ringtone_received_pdu_status(pdu) == STORE_STATUS_OK);
    assert(store_ringtone_receive_pdu(pdu, now) == STORE_STATUS_OK && diag.dirty_mask == 0u);
    strcpy(deliver.address, "+15550000003");
    assert(sms_deliver_build(&deliver, pdu, sizeof(pdu), &tpdu_len));
    assert(store_ringtone_receive_pdu(pdu, now) == STORE_STATUS_STORAGE_ERROR);
    assert(store_ringtone_pending_first() != 0u); /* saturation preserves unread tones */
    test_reference_reuse();
    test_reference_reuse_changes_part_count();
    test_legacy_receipts();
    puts("ringtone store passed");
    return 0;
}
