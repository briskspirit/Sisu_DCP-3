#include "modem_sms_protocol_internal.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "modem_sms_state_internal.h"
#include "services/log.h"
#include "services/modem_at_util.h"
#include "services/modem_line_parser.h"
#include "services/sms_picture_codec.h"
#include "services/sms_submit_codec.h"
#include "services/sms_vvm_filter.h"

#define MODEM_SMS_BINARY_RESULT_TIMEOUT_MS 120000u

typedef struct {
    /* Operation-local choreography only. Cached messages, result latches, and
     * transport state remain owned by the state module and root host. */
    modem_sms_mailbox_t mailbox;
    modem_sms_outcome_t restore_outcome;
    bool restore_sim_not_ready;
    bool delete_failed;
    uint8_t delete_pos;
    bool payload_submitted;
    bool send_confirmed;
    uint8_t binary_segments_accepted;
    bool delete_command_in_flight;
    uint8_t delete_commands_accepted;
    modem_sms_outcome_t binary_outcome;
} modem_sms_protocol_state_t;

static modem_sms_protocol_state_t s_protocol;

static bool emit(const modem_sms_protocol_hooks_t *hooks,
                 const modem_sms_protocol_action_t *action) {
    return hooks != NULL && hooks->emit != NULL && action != NULL &&
           hooks->emit(action);
}

static bool sim_ready(const modem_sms_protocol_hooks_t *hooks) {
    return hooks != NULL && hooks->sim_ready != NULL && hooks->sim_ready();
}

static bool call_preempt_pending(const modem_sms_protocol_hooks_t *hooks) {
    return hooks != NULL && hooks->call_preempt_pending != NULL &&
           hooks->call_preempt_pending();
}

static modem_sms_request_kind_t request_kind(
    const modem_sms_protocol_request_t *request) {
    if (request == NULL) {
        return MODEM_SMS_REQUEST_NONE;
    }
    switch (request->operation) {
    case MODEM_SMS_PROTOCOL_SEND_TEXT:
        return MODEM_SMS_REQUEST_SEND_TEXT;
    case MODEM_SMS_PROTOCOL_SEND_BINARY:
        return MODEM_SMS_REQUEST_SEND_BINARY;
    case MODEM_SMS_PROTOCOL_SAVE:
        return MODEM_SMS_REQUEST_SAVE;
    case MODEM_SMS_PROTOCOL_MAILBOX:
        return MODEM_SMS_REQUEST_MAILBOX;
    case MODEM_SMS_PROTOCOL_READ:
        return MODEM_SMS_REQUEST_READ;
    case MODEM_SMS_PROTOCOL_DELETE:
        return MODEM_SMS_REQUEST_DELETE;
    case MODEM_SMS_PROTOCOL_NONE:
    default:
        return MODEM_SMS_REQUEST_NONE;
    }
}

static bool request_valid(const modem_sms_protocol_request_t *request) {
    if (request == NULL || request->request_id == 0u) {
        return false;
    }
    switch (request->operation) {
    case MODEM_SMS_PROTOCOL_SEND_TEXT:
        return request->number != NULL && request->number[0] != '\0' &&
               request->text != NULL;
    case MODEM_SMS_PROTOCOL_SEND_BINARY:
        return request->number != NULL && request->number[0] != '\0' &&
               request->binary != NULL && request->binary_len != 0u;
    case MODEM_SMS_PROTOCOL_SAVE:
        return request->text != NULL && request->text[0] != '\0';
    case MODEM_SMS_PROTOCOL_MAILBOX:
        return request->mailbox == MODEM_SMS_MAILBOX_INBOX ||
               request->mailbox == MODEM_SMS_MAILBOX_OUTBOX;
    case MODEM_SMS_PROTOCOL_READ:
    case MODEM_SMS_PROTOCOL_DELETE:
        return request->indices != NULL && request->index_count != 0u &&
               request->index_count <= MODEM_SMS_SEGMENT_MAX;
    case MODEM_SMS_PROTOCOL_NONE:
    default:
        return false;
    }
}

static bool emit_command(const modem_sms_protocol_hooks_t *hooks,
                         modem_sms_command_kind_t kind, const char *command,
                         uint32_t timeout_ms, uint32_t now_ms, bool raw,
                         bool append_cr) {
    modem_sms_protocol_action_t action = {
        .type = MODEM_SMS_ACTION_COMMAND,
        .data.command = {
            .kind = kind,
            .command = command,
            .timeout_ms = timeout_ms,
            .now_ms = now_ms,
            .raw = raw,
            .append_cr = append_cr,
        },
    };
    return emit(hooks, &action);
}

static void emit_complete(const modem_sms_protocol_request_t *request,
                          const modem_sms_protocol_hooks_t *hooks,
                          modem_sms_outcome_t outcome, bool command_ok) {
    modem_sms_protocol_action_t action = {
        .type = MODEM_SMS_ACTION_COMPLETE,
        .data.complete = {
            .request_id = request != NULL ? request->request_id : 0u,
            .kind = request_kind(request),
            .outcome = outcome,
            .command_ok = command_ok,
        },
    };
    (void)emit(hooks, &action);
}

static void emit_send_result(const modem_sms_protocol_request_t *request,
                             const modem_sms_protocol_hooks_t *hooks,
                             modem_sms_outcome_t outcome) {
    modem_sms_protocol_action_t action = {
        .type = MODEM_SMS_ACTION_PUBLISH_SEND,
        .data.result = {
            .request_id = request != NULL ? request->request_id : 0u,
            .kind = request_kind(request),
            .outcome = outcome,
        },
    };
    (void)emit(hooks, &action);
}

static void emit_save_result(const modem_sms_protocol_request_t *request,
                             const modem_sms_protocol_hooks_t *hooks,
                             modem_sms_outcome_t outcome,
                             bool sim_not_ready) {
    modem_sms_protocol_action_t action = {
        .type = MODEM_SMS_ACTION_PUBLISH_SAVE,
        .data.result = {
            .request_id = request != NULL ? request->request_id : 0u,
            .kind = request_kind(request),
            .outcome = outcome,
            .sim_not_ready = sim_not_ready,
        },
    };
    (void)emit(hooks, &action);
}

static void emit_delete_result(const modem_sms_protocol_request_t *request,
                               const modem_sms_protocol_hooks_t *hooks,
                               modem_sms_outcome_t outcome,
                               bool sim_not_ready) {
    modem_sms_protocol_action_t action = {
        .type = MODEM_SMS_ACTION_PUBLISH_DELETE,
        .data.result = {
            .request_id = request != NULL ? request->request_id : 0u,
            .kind = request_kind(request),
            .outcome = outcome,
            .sim_not_ready = sim_not_ready,
        },
    };
    (void)emit(hooks, &action);
}

static void emit_read_result(const modem_sms_protocol_request_t *request,
                             const modem_sms_protocol_hooks_t *hooks,
                             modem_sms_outcome_t outcome,
                             bool sim_not_ready) {
    modem_sms_protocol_action_t action = {
        .type = MODEM_SMS_ACTION_PUBLISH_READ,
        .data.read_result = {
            .request_id = request != NULL ? request->request_id : 0u,
            .kind = request_kind(request),
            .outcome = outcome,
            .sim_not_ready = sim_not_ready,
            .expected_identity_hash = request != NULL
                ? request->expected_identity_hash : 0u,
        },
    };
    (void)emit(hooks, &action);
}

static void emit_mailbox_result(const modem_sms_protocol_request_t *request,
                                const modem_sms_protocol_hooks_t *hooks,
                                modem_sms_outcome_t outcome,
                                bool sim_not_ready) {
    bool complete = outcome == MODEM_SMS_OUTCOME_OK &&
                    !modem_sms_state_scan_incomplete();
    LOGI("sms", "mailbox raw=%u logical=%u complete=%u",
         (unsigned)modem_sms_state_scan_raw_count(),
         (unsigned)modem_sms_state_mailbox_count(),
         (unsigned)complete);
    modem_sms_protocol_action_t action = {
        .type = MODEM_SMS_ACTION_PUBLISH_MAILBOX,
        .data.mailbox_result = {
            .request_id = request != NULL ? request->request_id : 0u,
            .kind = request_kind(request),
            .outcome = outcome,
            .sim_not_ready = sim_not_ready,
            .complete = complete,
            .mailbox = s_protocol.mailbox,
        },
    };
    (void)emit(hooks, &action);
}

static void emit_status_increment(const modem_sms_protocol_hooks_t *hooks,
                                  modem_sms_action_type_t type) {
    modem_sms_protocol_action_t action = { .type = type };
    (void)emit(hooks, &action);
}

static void emit_arrival_scan_begin(
    const modem_sms_protocol_hooks_t *hooks) {
    modem_sms_protocol_action_t action = {
        .type = MODEM_SMS_ACTION_ARRIVAL_SCAN_BEGIN,
    };
    (void)emit(hooks, &action);
}

static void emit_arrival_scan_commit(
    const modem_sms_protocol_hooks_t *hooks, bool complete) {
    modem_sms_protocol_action_t action = {
        .type = MODEM_SMS_ACTION_ARRIVAL_SCAN_COMMIT,
        .data.arrival_commit = {
            .complete = complete,
            .inbox = s_protocol.mailbox == MODEM_SMS_MAILBOX_INBOX,
        },
    };
    (void)emit(hooks, &action);
}

static void start_delete(const modem_sms_protocol_hooks_t *hooks,
                         uint16_t index, uint32_t now_ms) {
    char command[24];
    snprintf(command, sizeof(command), "AT+CMGD=%u", (unsigned)index);
    (void)emit_command(hooks, MODEM_SMS_COMMAND_CMGD, command, 5000u,
                       now_ms, false, false);
}

static bool status_policy_enabled(
    const modem_sms_protocol_request_t *request) {
    return request != NULL &&
           request->read_status.preserve_unread_cmd != NULL &&
           request->read_status.consume_unread_cmd != NULL &&
           request->read_status.timeout_ms != 0u;
}

static void complete_selected_read(
    const modem_sms_protocol_request_t *request,
    const modem_sms_protocol_hooks_t *hooks, bool status_restore_ok) {
    modem_sms_outcome_t outcome = s_protocol.restore_outcome;
    if (outcome == MODEM_SMS_OUTCOME_OK && !status_restore_ok) {
        outcome = MODEM_SMS_OUTCOME_ERROR;
    }
    emit_read_result(request, hooks, outcome,
                     s_protocol.restore_sim_not_ready);
    emit_complete(request, hooks, outcome,
                  outcome == MODEM_SMS_OUTCOME_OK);
}

static void restore_selected_status(
    const modem_sms_protocol_request_t *request,
    const modem_sms_protocol_hooks_t *hooks, uint32_t now_ms,
    modem_sms_outcome_t outcome, bool sim_not_ready) {
    s_protocol.restore_outcome = outcome;
    s_protocol.restore_sim_not_ready = sim_not_ready;
    if (status_policy_enabled(request)) {
        (void)emit_command(
            hooks, MODEM_SMS_COMMAND_STATUS_PRESERVE,
            request->read_status.preserve_unread_cmd,
            request->read_status.timeout_ms, now_ms, false, false);
        return;
    }
    complete_selected_read(request, hooks, true);
}

static void restore_text_mode(const modem_sms_protocol_hooks_t *hooks,
                              uint32_t now_ms, modem_sms_outcome_t outcome,
                              bool sim_not_ready) {
    s_protocol.restore_outcome = outcome;
    s_protocol.restore_sim_not_ready = sim_not_ready;
    (void)emit_command(hooks, MODEM_SMS_COMMAND_CMGF_TEXT, "AT+CMGF=1",
                       5000u, now_ms, false, false);
}

static void start_status_safe_read(
    const modem_sms_protocol_request_t *request,
    const modem_sms_protocol_hooks_t *hooks, uint32_t now_ms) {
    if (status_policy_enabled(request)) {
        if (request->operation == MODEM_SMS_PROTOCOL_MAILBOX) {
            /* Listing is metadata discovery, not a user read. Some backends
             * otherwise persist REC READ as a side effect of CMGL/CMGR. */
            (void)emit_command(
                hooks, MODEM_SMS_COMMAND_STATUS_PRESERVE,
                request->read_status.preserve_unread_cmd,
                request->read_status.timeout_ms, now_ms, false, false);
            return;
        }
        if (request->operation == MODEM_SMS_PROTOCOL_READ) {
            /* A selected body is the sole operation that intentionally
             * consumes unread state. Preserve mode is restored before the
             * result is published, including every failure/timeout path. */
            (void)emit_command(
                hooks, MODEM_SMS_COMMAND_STATUS_CONSUME,
                request->read_status.consume_unread_cmd,
                request->read_status.timeout_ms, now_ms, false, false);
            return;
        }
    }
    if (request->read_status.preserve_unread_cmd != NULL ||
        request->read_status.consume_unread_cmd != NULL ||
        request->read_status.timeout_ms != 0u) {
        LOGE("sms", "incomplete vendor unread-status policy");
        if (request->operation == MODEM_SMS_PROTOCOL_MAILBOX) {
            emit_mailbox_result(request, hooks, MODEM_SMS_OUTCOME_ERROR,
                                false);
        } else {
            emit_read_result(request, hooks, MODEM_SMS_OUTCOME_ERROR, false);
        }
        emit_complete(request, hooks, MODEM_SMS_OUTCOME_ERROR, false);
        return;
    }
    (void)emit_command(hooks, MODEM_SMS_COMMAND_CMGF_PDU, "AT+CMGF=0",
                       5000u, now_ms, false, false);
}

static bool status_matches_mailbox(const char *status,
                                   modem_sms_mailbox_t mailbox) {
    if (status == NULL) {
        return false;
    }
    if (mailbox == MODEM_SMS_MAILBOX_OUTBOX) {
        return modem_at_starts_with(status, "STO");
    }
    return modem_at_starts_with(status, "REC");
}

static bool message_is_vvm_control(const modem_sms_message_t *message) {
    if (message == NULL) {
        return false;
    }
    const uint8_t *payload = message->binary
        ? message->binary_data : (const uint8_t *)message->text;
    size_t payload_len = message->binary
        ? message->binary_len : strlen(message->text);
    return sms_vvm_control_payload_is_recognized(
        message->has_ports, message->dest_port, payload, payload_len);
}

static bool append_mailbox_record(
    const modem_sms_record_t *record,
    const modem_sms_protocol_hooks_t *hooks) {
    modem_sms_protocol_action_t action = {
        .type = MODEM_SMS_ACTION_APPEND_MAILBOX,
        .data.append_mailbox = { .record = record },
    };
    if (emit(hooks, &action)) {
        return true;
    }
    modem_sms_state_scan_mark_incomplete();
    return false;
}

static void finish_decoded_message(
    const modem_sms_message_t *message,
    const modem_sms_protocol_hooks_t *hooks) {
    if (message == NULL) {
        return;
    }
    if (s_protocol.mailbox == MODEM_SMS_MAILBOX_INBOX &&
        message_is_vvm_control(message)) {
        modem_sms_state_arrival_scan_note(
            message->index, MODEM_SMS_ARRIVAL_FILTERED);
        if (!modem_sms_state_filtered_queue(message->index)) {
            LOGW("sms", "VVM cleanup queue full index=%u",
                 (unsigned)message->index);
        }
        return;
    }
    modem_sms_record_t record;
    if (!modem_sms_state_mailbox_prepare_record(message, &record)) {
        return;
    }
    (void)append_mailbox_record(&record, hooks);
}

static void finish_read(const modem_sms_protocol_request_t *request,
                        const modem_sms_protocol_hooks_t *hooks) {
    modem_sms_collected_view_t collected;
    modem_sms_state_collection_view(&collected);
    if (request->operation == MODEM_SMS_PROTOCOL_MAILBOX) {
        const char *listed_status = modem_line_sms_status_from_numeric(
            collected.list_status);
        if (listed_status != NULL) {
            /* CMGL owns both first-sighting read/unread truth and the streamed
             * PDU body. No per-row CMGR round trip is needed for list metadata. */
            modem_sms_state_collection_set_status(listed_status);
            modem_sms_state_collection_view(&collected);
        } else {
            modem_sms_state_scan_mark_incomplete();
        }
        bool inbox_status = modem_at_starts_with(
            collected.message->status, "REC");
        bool outbox_status = modem_at_starts_with(
            collected.message->status, "STO");
        if (!inbox_status && !outbox_status) {
            modem_sms_state_scan_mark_incomplete();
        } else if (status_matches_mailbox(collected.message->status,
                                          s_protocol.mailbox)) {
            if (collected.decoded) {
                finish_decoded_message(collected.message, hooks);
            } else {
                /* A full CMGL snapshot is authoritative even when one stored
                 * PDU is corrupt. Preserve that physical row as neutral data;
                 * retrying the same bytes immediately can never repair it. */
                if (s_protocol.mailbox == MODEM_SMS_MAILBOX_INBOX) {
                    modem_sms_state_arrival_scan_note(
                        collected.message->index,
                        MODEM_SMS_ARRIVAL_USER);
                }
                modem_sms_record_t record;
                if (modem_sms_state_mailbox_prepare_quarantine(
                        collected.message, &record)) {
                    (void)append_mailbox_record(&record, hooks);
                }
            }
        }
    } else if (request->operation == MODEM_SMS_PROTOCOL_READ) {
        if (collected.decoded) {
            (void)modem_sms_state_selected_accept_segment(
                collected.message);
        } else {
            (void)modem_sms_state_selected_accept_undecoded(
                collected.message);
        }
    }
}

static void start_read_next(const modem_sms_protocol_request_t *request,
                            const modem_sms_protocol_hooks_t *hooks,
                            uint32_t now_ms) {
    if (request->operation != MODEM_SMS_PROTOCOL_READ) {
        emit_complete(request, hooks, MODEM_SMS_OUTCOME_ERROR, false);
        return;
    }
    if (call_preempt_pending(hooks)) {
        /* A selected multipart read yields between CMGR commands when call
         * control arrives. A stable ACTIVE call alone does not trigger this. */
        restore_text_mode(hooks, now_ms, MODEM_SMS_OUTCOME_CANCELLED,
                          false);
        return;
    }
    uint16_t index = 0u;
    if (!modem_sms_state_selected_next_index(
            request->indices, request->index_count, &index)) {
        restore_text_mode(
            hooks, now_ms,
            modem_sms_state_selected_complete() ? MODEM_SMS_OUTCOME_OK
                                                : MODEM_SMS_OUTCOME_ERROR,
            false);
        return;
    }
    modem_sms_state_detail_row_begin(index);
    char command[24];
    snprintf(command, sizeof(command), "AT+CMGR=%u", (unsigned)index);
    (void)emit_command(hooks, MODEM_SMS_COMMAND_CMGR, command, 5000u,
                       now_ms, false, false);
}

static void note_filtered_delete_failure(
    const modem_sms_protocol_hooks_t *hooks) {
    modem_sms_state_filtered_note_failure();
    emit_status_increment(hooks, MODEM_SMS_ACTION_INCREMENT_COMMAND_ERRORS);
}

static void finish_filtered_cleanup(
    const modem_sms_protocol_request_t *request,
    const modem_sms_protocol_hooks_t *hooks) {
    modem_sms_outcome_t outcome = s_protocol.restore_outcome;
    bool complete = outcome == MODEM_SMS_OUTCOME_OK &&
                    !modem_sms_state_scan_incomplete();
    modem_sms_filtered_summary_t summary;
    modem_sms_state_filtered_finish(&summary);
    LOGI("sms", "VVM controls filtered=%u delete_ok=%u",
         (unsigned)summary.count, summary.delete_ok ? 1u : 0u);
    emit_arrival_scan_commit(hooks, complete);
    emit_mailbox_result(request, hooks, outcome,
                        s_protocol.restore_sim_not_ready);
    emit_complete(request, hooks, outcome,
                  outcome == MODEM_SMS_OUTCOME_OK);
}

static void start_filtered_delete(
                                  const modem_sms_protocol_request_t *request,
                                  const modem_sms_protocol_hooks_t *hooks,
                                  uint32_t now_ms) {
    uint16_t index = 0u;
    if (!modem_sms_state_filtered_current(&index)) {
        finish_filtered_cleanup(request, hooks);
        return;
    }
    start_delete(hooks, index, now_ms);
}

static void binary_start_segment(
    const modem_sms_protocol_request_t *request,
    const modem_sms_protocol_hooks_t *hooks, uint32_t now_ms);

static void binary_restore_text(const modem_sms_protocol_hooks_t *hooks,
                                uint32_t now_ms,
                                modem_sms_outcome_t outcome) {
    s_protocol.binary_outcome = outcome;
    modem_sms_state_binary_set_send_ok(outcome == MODEM_SMS_OUTCOME_OK);
    (void)emit_command(hooks, MODEM_SMS_COMMAND_CMGF_TEXT, "AT+CMGF=1",
                       5000u, now_ms, false, false);
}

static void binary_start(const modem_sms_protocol_request_t *request,
                         const modem_sms_protocol_hooks_t *hooks,
                         uint32_t now_ms) {
    uint8_t segment_total = modem_sms_state_binary_begin(request->binary_len);
    s_protocol.payload_submitted = false;
    s_protocol.send_confirmed = false;
    s_protocol.binary_segments_accepted = 0u;
    s_protocol.binary_outcome = MODEM_SMS_OUTCOME_NONE;
    LOGI("modem", "binary SMS start bytes=%u segments=%u dest_port=0x%04x src_port=0x%04x mode=%u",
         (unsigned)request->binary_len, (unsigned)segment_total,
         (unsigned)request->dest_port, (unsigned)request->source_port,
         (unsigned)request->binary_mode);
    (void)emit_command(hooks, MODEM_SMS_COMMAND_CMGF_PDU, "AT+CMGF=0",
                       5000u, now_ms, false, false);
}

static void binary_start_segment(
    const modem_sms_protocol_request_t *request,
    const modem_sms_protocol_hooks_t *hooks, uint32_t now_ms) {
    if (!modem_sms_state_binary_build_segment(
            request->number, request->binary, request->binary_len,
            request->dest_port, request->source_port,
            request->binary_mode)) {
        LOGW("modem", "binary SMS PDU build failed");
        binary_restore_text(
            hooks, now_ms,
            s_protocol.binary_segments_accepted != 0u
                ? MODEM_SMS_OUTCOME_UNCERTAIN
                : MODEM_SMS_OUTCOME_ERROR);
        return;
    }
    modem_sms_binary_segment_view_t segment;
    modem_sms_state_binary_segment_view(&segment);
    char command[24];
    snprintf(command, sizeof(command), "AT+CMGS=%u",
             (unsigned)segment.tpdu_len);
    (void)emit_command(hooks, MODEM_SMS_COMMAND_CMGS_PROMPT, command,
                       5000u, now_ms, true, true);
}

static void parse_cmgr_header(const char *line) {
    modem_sms_collected_view_t collected;
    modem_sms_state_collection_view(&collected);
    char status[MODEM_SMS_STATUS_MAX + 1u];
    char sender[MODEM_SMS_SENDER_MAX + 1u];
    char timestamp[MODEM_SMS_TIMESTAMP_MAX + 1u];
    modem_at_copy_bounded(status, sizeof(status), collected.message->status);
    modem_at_copy_bounded(sender, sizeof(sender), collected.message->sender);
    modem_at_copy_bounded(timestamp, sizeof(timestamp),
                          collected.message->timestamp);
    modem_line_parse_cmgr_header(
        line, status, sizeof(status), sender, sizeof(sender), timestamp,
        sizeof(timestamp));
    modem_sms_state_detail_header(status, sender, timestamp);
}

void modem_sms_protocol_init(void) {
    memset(&s_protocol, 0, sizeof(s_protocol));
    s_protocol.mailbox = MODEM_SMS_MAILBOX_INBOX;
    modem_sms_protocol_operation_finished();
}

void modem_sms_protocol_operation_finished(void) {
    modem_sms_state_scan_reset();
    modem_sms_state_filtered_reset();
}

void modem_sms_protocol_reset_pending_arrivals(void) {
    modem_sms_state_pending_arrivals_reset();
}

bool modem_sms_protocol_track_pending_arrival(uint16_t index) {
    return modem_sms_state_pending_arrival_track(index);
}

void modem_sms_protocol_line_dropped(void) {
    modem_sms_state_collection_abort_body();
}

bool modem_sms_protocol_begin(const modem_sms_protocol_request_t *request,
                              const modem_sms_protocol_hooks_t *hooks,
                              uint32_t now_ms) {
    if (!request_valid(request) || hooks == NULL || hooks->emit == NULL) {
        return false;
    }

    s_protocol.restore_outcome = MODEM_SMS_OUTCOME_NONE;
    s_protocol.restore_sim_not_ready = false;
    s_protocol.payload_submitted = false;
    s_protocol.send_confirmed = false;
    s_protocol.binary_segments_accepted = 0u;
    s_protocol.delete_command_in_flight = false;
    s_protocol.delete_commands_accepted = 0u;
    s_protocol.binary_outcome = MODEM_SMS_OUTCOME_NONE;

    modem_sms_protocol_action_t action;
    switch (request->operation) {
    case MODEM_SMS_PROTOCOL_SEND_TEXT:
        return emit_command(
            hooks, MODEM_SMS_COMMAND_CPMS,
            "AT+CPMS=\"" MODEM_SMS_STORAGE "\",\"" MODEM_SMS_STORAGE
            "\",\"" MODEM_SMS_STORAGE "\"",
            5000u, now_ms, false, false);
    case MODEM_SMS_PROTOCOL_SEND_BINARY:
        binary_start(request, hooks, now_ms);
        return true;
    case MODEM_SMS_PROTOCOL_SAVE:
        return emit_command(
            hooks, MODEM_SMS_COMMAND_CPMS,
            "AT+CPMS=\"" MODEM_SMS_STORAGE "\",\"" MODEM_SMS_STORAGE
            "\",\"" MODEM_SMS_STORAGE "\"",
            5000u, now_ms, false, false);
    case MODEM_SMS_PROTOCOL_MAILBOX:
        s_protocol.mailbox = request->mailbox;
        action = (modem_sms_protocol_action_t){
            .type = MODEM_SMS_ACTION_CLEAR_MAILBOX,
        };
        (void)emit(hooks, &action);
        modem_sms_state_scan_reset();
        modem_sms_state_filtered_reset();
        if (s_protocol.mailbox == MODEM_SMS_MAILBOX_INBOX) {
            emit_arrival_scan_begin(hooks);
        }
        return emit_command(
            hooks, MODEM_SMS_COMMAND_CPMS,
            "AT+CPMS=\"" MODEM_SMS_STORAGE "\",\"" MODEM_SMS_STORAGE
            "\",\"" MODEM_SMS_STORAGE "\"",
            5000u, now_ms, false, false);
    case MODEM_SMS_PROTOCOL_READ:
        action = (modem_sms_protocol_action_t){
            .type = MODEM_SMS_ACTION_SELECTED_BEGIN,
            .data.selected_begin = {
                .first_index = request->indices[0],
                .index_count = request->index_count,
                .quarantined = request->quarantined,
            },
        };
        (void)emit(hooks, &action);
        return emit_command(
            hooks, MODEM_SMS_COMMAND_CPMS,
            "AT+CPMS=\"" MODEM_SMS_STORAGE "\",\"" MODEM_SMS_STORAGE
            "\",\"" MODEM_SMS_STORAGE "\"",
            5000u, now_ms, false, false);
    case MODEM_SMS_PROTOCOL_DELETE:
        s_protocol.delete_failed = false;
        s_protocol.delete_pos = 1u;
        return emit_command(
            hooks, MODEM_SMS_COMMAND_CPMS,
            "AT+CPMS=\"" MODEM_SMS_STORAGE "\",\"" MODEM_SMS_STORAGE
            "\",\"" MODEM_SMS_STORAGE "\"",
            5000u, now_ms, false, false);
    case MODEM_SMS_PROTOCOL_NONE:
    default:
        return false;
    }
}

void modem_sms_protocol_resume_after_wake(
    const modem_sms_protocol_request_t *request,
    const modem_sms_protocol_hooks_t *hooks, uint32_t now_ms) {
    if (!request_valid(request)) {
        emit_complete(request, hooks, MODEM_SMS_OUTCOME_ERROR, false);
        return;
    }
    if (request->operation == MODEM_SMS_PROTOCOL_SEND_TEXT) {
        char command[MODEM_SMS_SENDER_MAX + 40u];
        sms_submit_format_text_command(command, sizeof(command), "AT+CMGS",
                                       request->number, false);
        (void)emit_command(hooks, MODEM_SMS_COMMAND_CMGS_PROMPT, command,
                           5000u, now_ms, true, true);
    } else if (request->operation == MODEM_SMS_PROTOCOL_SAVE) {
        char command[MODEM_SMS_SENDER_MAX + 40u];
        sms_submit_format_text_command(command, sizeof(command), "AT+CMGW",
                                       request->number, false);
        (void)emit_command(hooks, MODEM_SMS_COMMAND_CMGW_PROMPT, command,
                           5000u, now_ms, true, true);
    } else if (request->operation == MODEM_SMS_PROTOCOL_MAILBOX ||
               request->operation == MODEM_SMS_PROTOCOL_READ) {
        start_status_safe_read(request, hooks, now_ms);
    } else if (request->operation == MODEM_SMS_PROTOCOL_DELETE) {
        start_delete(hooks, request->indices[0], now_ms);
    } else {
        emit_complete(request, hooks, MODEM_SMS_OUTCOME_ERROR, false);
    }
}

void modem_sms_protocol_resume_after_prompt_abort(
    const modem_sms_protocol_request_t *request,
    const modem_sms_protocol_hooks_t *hooks, uint32_t now_ms) {
    if (request != NULL &&
        request->operation == MODEM_SMS_PROTOCOL_SEND_BINARY) {
        modem_sms_outcome_t outcome = s_protocol.binary_outcome;
        if (outcome == MODEM_SMS_OUTCOME_NONE) {
            outcome = MODEM_SMS_OUTCOME_TIMEOUT;
        }
        binary_restore_text(hooks, now_ms, outcome);
    } else {
        emit_complete(request, hooks, MODEM_SMS_OUTCOME_ERROR, false);
    }
}

bool modem_sms_protocol_on_prompt(
    modem_sms_command_kind_t kind,
    const modem_sms_protocol_request_t *request,
    const modem_sms_protocol_hooks_t *hooks, uint32_t now_ms) {
    if (!request_valid(request) ||
        (kind != MODEM_SMS_COMMAND_CMGS_PROMPT &&
         kind != MODEM_SMS_COMMAND_CMGW_PROMPT)) {
        return false;
    }

    modem_sms_protocol_action_t action = {
        .type = MODEM_SMS_ACTION_BODY,
        .data.body = {
            .final_kind = kind == MODEM_SMS_COMMAND_CMGW_PROMPT
                ? MODEM_SMS_COMMAND_CMGW_FINAL
                : MODEM_SMS_COMMAND_CMGS_FINAL,
            .timeout_ms = request->operation == MODEM_SMS_PROTOCOL_SEND_BINARY
                ? MODEM_SMS_BINARY_RESULT_TIMEOUT_MS : 30000u,
            .now_ms = now_ms,
            .binary = request->operation == MODEM_SMS_PROTOCOL_SEND_BINARY,
        },
    };
    if (request->operation == MODEM_SMS_PROTOCOL_SEND_BINARY) {
        modem_sms_binary_segment_view_t segment;
        modem_sms_state_binary_segment_view(&segment);
        action.data.body.bytes = (const uint8_t *)segment.pdu_hex;
        action.data.body.length = strlen(segment.pdu_hex);
        action.data.body.segment = segment.segment;
        action.data.body.segment_total = segment.segment_total;
        action.data.body.tpdu_len = segment.tpdu_len;
    } else {
        action.data.body.bytes = (const uint8_t *)request->text;
        action.data.body.length = strlen(request->text);
    }
    bool emitted = emit(hooks, &action);
    if (emitted) {
        s_protocol.payload_submitted = true;
    }
    return emitted;
}

void modem_sms_protocol_command_dispatched(modem_sms_command_kind_t kind) {
    if (kind == MODEM_SMS_COMMAND_CMGD) {
        s_protocol.delete_command_in_flight = true;
    }
}

bool modem_sms_protocol_parse_line(
    modem_sms_command_kind_t kind, const char *line, bool known_urc,
    bool final_text, const modem_sms_protocol_request_t *request,
    const modem_sms_protocol_hooks_t *hooks) {
    if (line == NULL || request == NULL) {
        return false;
    }
    switch (kind) {
    case MODEM_SMS_COMMAND_CPMS:
        return modem_at_starts_with(line, "+CPMS:");
    case MODEM_SMS_COMMAND_CMGL: {
        if (modem_at_starts_with(line, "+CMGL:")) {
            unsigned index = 0u;
            unsigned status = UINT_MAX;
            int fields = sscanf(line, "+CMGL: %u,%u", &index, &status);
            bool index_valid = fields >= 1 && index <= UINT16_MAX;
            bool status_valid = fields == 2 && status <= UINT8_MAX &&
                modem_line_sms_status_from_numeric(status) != NULL;
            modem_sms_state_mailbox_row_begin(
                index_valid, index_valid ? (uint16_t)index : 0u,
                status_valid,
                status_valid ? (uint8_t)status : UINT8_MAX);
            return true;
        }
        if (known_urc) {
            return false;
        }
        modem_sms_collected_view_t collected;
        modem_sms_state_collection_view(&collected);
        if (collected.collecting_body && !final_text) {
            sms_codec_message_t decoded;
            if (sms_pdu_decode(line, &decoded)) {
                modem_sms_state_collection_feed_decoded(&decoded);
            } else {
                modem_sms_state_collection_abort_body();
            }
            finish_read(request, hooks);
            return true;
        }
        if (!final_text) {
            modem_sms_state_scan_mark_incomplete();
            return true;
        }
        return false;
    }
    case MODEM_SMS_COMMAND_CMGR: {
        if (modem_at_starts_with(line, "+CMGR:")) {
            parse_cmgr_header(line);
            return true;
        }
        if (known_urc) {
            return false;
        }
        modem_sms_collected_view_t collected;
        modem_sms_state_collection_view(&collected);
        if (collected.collecting_body && !final_text) {
            sms_codec_message_t decoded;
            if (sms_pdu_decode(line, &decoded)) {
                modem_sms_state_collection_feed_decoded(&decoded);
            } else {
                modem_sms_state_collection_feed_text(line);
            }
            return true;
        }
        return false;
    }
    case MODEM_SMS_COMMAND_CMGS_FINAL:
        return modem_at_starts_with(line, "+CMGS:");
    case MODEM_SMS_COMMAND_CMGW_FINAL:
        return modem_at_starts_with(line, "+CMGW:");
    case MODEM_SMS_COMMAND_STATUS_PRESERVE:
    case MODEM_SMS_COMMAND_STATUS_CONSUME:
    case MODEM_SMS_COMMAND_CMGD:
    case MODEM_SMS_COMMAND_CMGF_PDU:
    case MODEM_SMS_COMMAND_CMGF_TEXT:
    case MODEM_SMS_COMMAND_CMGS_PROMPT:
    case MODEM_SMS_COMMAND_CMGW_PROMPT:
    default:
        return false;
    }
}

void modem_sms_protocol_on_final(
    modem_sms_command_kind_t kind, bool ok,
    const modem_sms_protocol_request_t *request,
    const modem_sms_protocol_hooks_t *hooks, uint32_t now_ms) {
    if (!request_valid(request)) {
        emit_complete(request, hooks, MODEM_SMS_OUTCOME_ERROR, false);
        return;
    }
    switch (kind) {
    case MODEM_SMS_COMMAND_CPMS:
        if (!ok) {
            bool not_ready = !sim_ready(hooks);
            if (request->operation == MODEM_SMS_PROTOCOL_SEND_TEXT) {
                emit_send_result(request, hooks, MODEM_SMS_OUTCOME_ERROR);
            } else if (request->operation == MODEM_SMS_PROTOCOL_SAVE) {
                emit_save_result(request, hooks, MODEM_SMS_OUTCOME_ERROR,
                                 not_ready);
            } else if (request->operation == MODEM_SMS_PROTOCOL_MAILBOX) {
                emit_mailbox_result(request, hooks, MODEM_SMS_OUTCOME_ERROR,
                                    not_ready);
            } else if (request->operation == MODEM_SMS_PROTOCOL_READ) {
                emit_read_result(request, hooks, MODEM_SMS_OUTCOME_ERROR,
                                 not_ready);
            } else if (request->operation == MODEM_SMS_PROTOCOL_DELETE) {
                emit_delete_result(request, hooks, MODEM_SMS_OUTCOME_ERROR,
                                   not_ready);
            }
            emit_complete(request, hooks, MODEM_SMS_OUTCOME_ERROR, false);
        } else {
            /* Selecting storage may invalidate the live SMS-to-RI latch.
             * Re-qualify it before continuing; neutral backends without that
             * contract resume synchronously in the root host. */
            modem_sms_protocol_action_t action = {
                .type = MODEM_SMS_ACTION_REQUEST_WAKE_CONTINUATION,
                .data.wake_continuation = { .now_ms = now_ms },
            };
            (void)emit(hooks, &action);
        }
        return;
    case MODEM_SMS_COMMAND_STATUS_PRESERVE:
        if (request->operation == MODEM_SMS_PROTOCOL_MAILBOX) {
            if (ok) {
                (void)emit_command(hooks, MODEM_SMS_COMMAND_CMGF_PDU,
                                   "AT+CMGF=0", 5000u, now_ms, false,
                                   false);
            } else {
                emit_mailbox_result(request, hooks, MODEM_SMS_OUTCOME_ERROR,
                                    false);
                emit_complete(request, hooks, MODEM_SMS_OUTCOME_ERROR,
                              false);
            }
        } else if (request->operation == MODEM_SMS_PROTOCOL_READ) {
            complete_selected_read(request, hooks, ok);
        } else {
            emit_complete(request, hooks, MODEM_SMS_OUTCOME_ERROR, false);
        }
        return;
    case MODEM_SMS_COMMAND_STATUS_CONSUME:
        if (request->operation == MODEM_SMS_PROTOCOL_READ && ok) {
            (void)emit_command(hooks, MODEM_SMS_COMMAND_CMGF_PDU,
                               "AT+CMGF=0", 5000u, now_ms, false, false);
        } else if (request->operation == MODEM_SMS_PROTOCOL_READ) {
            /* The consume write may have taken effect even when its final is
             * ERROR/lost. Reassert preserve mode before releasing the read. */
            restore_selected_status(request, hooks, now_ms,
                                    MODEM_SMS_OUTCOME_ERROR, false);
        } else {
            emit_complete(request, hooks, MODEM_SMS_OUTCOME_ERROR, false);
        }
        return;
    case MODEM_SMS_COMMAND_CMGL: {
        modem_sms_collected_view_t collected;
        modem_sms_state_collection_view(&collected);
        if (collected.collecting_body) {
            /* A header with no decodable body is still a real stored row. */
            finish_read(request, hooks);
            modem_sms_state_collection_abort_body();
        }
        modem_sms_record_t quarantine;
        while (modem_sms_state_multipart_group_take_quarantine(
                   &quarantine)) {
            (void)append_mailbox_record(&quarantine, hooks);
        }
        restore_text_mode(hooks, now_ms,
                          ok ? MODEM_SMS_OUTCOME_OK
                             : MODEM_SMS_OUTCOME_ERROR,
                          false);
        return;
    }
    case MODEM_SMS_COMMAND_CMGR:
        if (ok) {
            finish_read(request, hooks);
        } else if (request->operation == MODEM_SMS_PROTOCOL_READ) {
            modem_sms_state_selected_fail();
        } else {
            modem_sms_state_scan_mark_incomplete();
        }
        start_read_next(request, hooks, now_ms);
        return;
    case MODEM_SMS_COMMAND_CMGD:
        s_protocol.delete_command_in_flight = false;
        if (request->operation == MODEM_SMS_PROTOCOL_DELETE && ok &&
            s_protocol.delete_commands_accepted < UINT8_MAX) {
            s_protocol.delete_commands_accepted++;
        }
        if (request->operation == MODEM_SMS_PROTOCOL_MAILBOX &&
            modem_sms_state_filtered_active()) {
            if (!ok) {
                note_filtered_delete_failure(hooks);
            }
            if (modem_sms_state_filtered_advance(ok)) {
                start_filtered_delete(request, hooks, now_ms);
            } else {
                finish_filtered_cleanup(request, hooks);
            }
            return;
        }
        if (!ok) {
            s_protocol.delete_failed = true;
        }
        if (s_protocol.delete_pos < request->index_count) {
            start_delete(hooks, request->indices[s_protocol.delete_pos++],
                         now_ms);
        } else {
            modem_sms_outcome_t outcome = s_protocol.delete_failed
                ? MODEM_SMS_OUTCOME_ERROR : MODEM_SMS_OUTCOME_OK;
            emit_delete_result(request, hooks, outcome,
                               outcome != MODEM_SMS_OUTCOME_OK &&
                                   !sim_ready(hooks));
            emit_complete(request, hooks, outcome,
                          outcome == MODEM_SMS_OUTCOME_OK);
        }
        return;
    case MODEM_SMS_COMMAND_CMGF_PDU:
        if (ok && request->operation == MODEM_SMS_PROTOCOL_SEND_BINARY) {
            binary_start_segment(request, hooks, now_ms);
        } else if (ok &&
                   (request->operation == MODEM_SMS_PROTOCOL_MAILBOX ||
                    request->operation == MODEM_SMS_PROTOCOL_READ)) {
            if (request->operation == MODEM_SMS_PROTOCOL_MAILBOX) {
                (void)emit_command(hooks, MODEM_SMS_COMMAND_CMGL,
                                   "AT+CMGL=4", 30000u, now_ms, false,
                                   false);
            } else {
                start_read_next(request, hooks, now_ms);
            }
        } else {
            if (request->operation == MODEM_SMS_PROTOCOL_MAILBOX) {
                emit_mailbox_result(request, hooks, MODEM_SMS_OUTCOME_ERROR,
                                    false);
            } else if (request->operation == MODEM_SMS_PROTOCOL_READ) {
                emit_read_result(request, hooks, MODEM_SMS_OUTCOME_ERROR,
                                 false);
            } else {
                emit_send_result(request, hooks, MODEM_SMS_OUTCOME_ERROR);
            }
            emit_complete(request, hooks, MODEM_SMS_OUTCOME_ERROR, false);
        }
        return;
    case MODEM_SMS_COMMAND_CMGF_TEXT:
        if (request->operation == MODEM_SMS_PROTOCOL_MAILBOX ||
            request->operation == MODEM_SMS_PROTOCOL_READ) {
            modem_sms_outcome_t outcome = s_protocol.restore_outcome;
            if (outcome == MODEM_SMS_OUTCOME_NONE) {
                outcome = MODEM_SMS_OUTCOME_ERROR;
            }
            if (outcome == MODEM_SMS_OUTCOME_OK && !ok) {
                outcome = MODEM_SMS_OUTCOME_ERROR;
            }
            if (request->operation == MODEM_SMS_PROTOCOL_MAILBOX) {
                if (outcome == MODEM_SMS_OUTCOME_OK &&
                    modem_sms_state_filtered_begin()) {
                    start_filtered_delete(request, hooks, now_ms);
                    return;
                }
                emit_arrival_scan_commit(
                    hooks, outcome == MODEM_SMS_OUTCOME_OK &&
                               !modem_sms_state_scan_incomplete());
                emit_mailbox_result(request, hooks, outcome,
                                    s_protocol.restore_sim_not_ready);
                emit_complete(request, hooks, outcome,
                              outcome == MODEM_SMS_OUTCOME_OK);
            } else {
                restore_selected_status(
                    request, hooks, now_ms, outcome,
                    s_protocol.restore_sim_not_ready);
            }
            return;
        } else {
            modem_sms_outcome_t outcome = s_protocol.binary_outcome;
            if (outcome == MODEM_SMS_OUTCOME_NONE) {
                outcome = MODEM_SMS_OUTCOME_ERROR;
            }
            if (outcome == MODEM_SMS_OUTCOME_OK) {
                emit_status_increment(hooks, MODEM_SMS_ACTION_INCREMENT_SENT);
            }
            emit_send_result(request, hooks, outcome);
            emit_complete(request, hooks, outcome,
                          outcome == MODEM_SMS_OUTCOME_OK && ok);
            return;
        }
    case MODEM_SMS_COMMAND_CMGS_PROMPT:
        if (request->operation == MODEM_SMS_PROTOCOL_SEND_BINARY) {
            binary_restore_text(
                hooks, now_ms,
                s_protocol.binary_segments_accepted != 0u
                    ? MODEM_SMS_OUTCOME_UNCERTAIN
                    : MODEM_SMS_OUTCOME_ERROR);
        } else {
            emit_send_result(request, hooks, MODEM_SMS_OUTCOME_ERROR);
            emit_complete(request, hooks, MODEM_SMS_OUTCOME_ERROR, false);
        }
        return;
    case MODEM_SMS_COMMAND_CMGS_FINAL:
        s_protocol.payload_submitted = false;
        if (request->operation == MODEM_SMS_PROTOCOL_SEND_BINARY) {
            if (!ok) {
                binary_restore_text(
                    hooks, now_ms,
                    s_protocol.binary_segments_accepted != 0u
                        ? MODEM_SMS_OUTCOME_UNCERTAIN
                        : MODEM_SMS_OUTCOME_ERROR);
            } else {
                s_protocol.binary_segments_accepted++;
                if (modem_sms_state_binary_has_more(request->binary_len)) {
                    binary_start_segment(request, hooks, now_ms);
                } else {
                    s_protocol.send_confirmed = true;
                    binary_restore_text(hooks, now_ms,
                                        MODEM_SMS_OUTCOME_OK);
                }
            }
            return;
        }
        if (ok) {
            s_protocol.send_confirmed = true;
            emit_status_increment(hooks, MODEM_SMS_ACTION_INCREMENT_SENT);
            char command[MODEM_SMS_SENDER_MAX + 48u];
            sms_submit_format_text_command(command, sizeof(command),
                                           "AT+CMGW", request->number, true);
            (void)emit_command(hooks, MODEM_SMS_COMMAND_CMGW_PROMPT,
                               command, 5000u, now_ms, true, true);
        } else {
            emit_send_result(request, hooks, MODEM_SMS_OUTCOME_ERROR);
            emit_complete(request, hooks, MODEM_SMS_OUTCOME_ERROR, false);
        }
        return;
    case MODEM_SMS_COMMAND_CMGW_PROMPT:
        s_protocol.payload_submitted = false;
        if (request->operation == MODEM_SMS_PROTOCOL_SAVE) {
            emit_save_result(request, hooks, MODEM_SMS_OUTCOME_ERROR,
                             !sim_ready(hooks));
            emit_complete(request, hooks, MODEM_SMS_OUTCOME_ERROR, false);
        } else {
            emit_send_result(request, hooks, MODEM_SMS_OUTCOME_OK);
            emit_complete(request, hooks, MODEM_SMS_OUTCOME_OK, true);
        }
        return;
    case MODEM_SMS_COMMAND_CMGW_FINAL:
        s_protocol.payload_submitted = false;
        if (request->operation == MODEM_SMS_PROTOCOL_SAVE) {
            modem_sms_outcome_t outcome = ok ? MODEM_SMS_OUTCOME_OK
                                             : MODEM_SMS_OUTCOME_ERROR;
            emit_save_result(request, hooks, outcome,
                             !ok && !sim_ready(hooks));
            emit_complete(request, hooks, outcome, ok);
        } else {
            if (!ok) {
                LOGW("modem", "SMS sent, but SIM sent-copy write failed");
            }
            emit_send_result(request, hooks, MODEM_SMS_OUTCOME_OK);
            emit_complete(request, hooks, MODEM_SMS_OUTCOME_OK, true);
        }
        return;
    }
}

void modem_sms_protocol_on_timeout(
    modem_sms_command_kind_t kind,
    const modem_sms_protocol_request_t *request,
    const modem_sms_protocol_hooks_t *hooks, uint32_t now_ms) {
    if (!request_valid(request)) {
        emit_complete(request, hooks, MODEM_SMS_OUTCOME_ERROR, false);
        return;
    }
    bool prompt_timeout = kind == MODEM_SMS_COMMAND_CMGS_PROMPT ||
                          kind == MODEM_SMS_COMMAND_CMGW_PROMPT;
    if (prompt_timeout) {
        /* A lost prompt leaves command text liable to become message body.
         * Binary mode also needs CMGF restoration, but only after ESC's own
         * final has drained in the core-owned settle window. */
        modem_sms_protocol_action_t action = {
            .type = MODEM_SMS_ACTION_ABORT_PROMPT,
            .data.abort_prompt = {
                .now_ms = now_ms,
                .restore_after_settle =
                    request->operation == MODEM_SMS_PROTOCOL_SEND_BINARY &&
                    kind == MODEM_SMS_COMMAND_CMGS_PROMPT,
            },
        };
        (void)emit(hooks, &action);
        if (action.data.abort_prompt.restore_after_settle) {
            s_protocol.binary_outcome =
                s_protocol.binary_segments_accepted != 0u
                    ? MODEM_SMS_OUTCOME_UNCERTAIN
                    : MODEM_SMS_OUTCOME_TIMEOUT;
            return;
        }
    }

    if (request->operation == MODEM_SMS_PROTOCOL_SEND_BINARY &&
        kind == MODEM_SMS_COMMAND_CMGF_PDU) {
        binary_restore_text(hooks, now_ms, MODEM_SMS_OUTCOME_TIMEOUT);
        return;
    }
    if (request->operation == MODEM_SMS_PROTOCOL_SEND_BINARY &&
        kind == MODEM_SMS_COMMAND_CMGS_FINAL) {
        binary_restore_text(hooks, now_ms, MODEM_SMS_OUTCOME_UNCERTAIN);
        return;
    }
    if (request->operation == MODEM_SMS_PROTOCOL_SEND_BINARY &&
        kind == MODEM_SMS_COMMAND_CMGF_TEXT) {
        /* All segments may already be accepted. A lost cleanup final is a
         * transport failure, not a retryable user send failure that could
         * duplicate the delivered message. */
        modem_sms_outcome_t outcome = s_protocol.binary_outcome;
        if (outcome == MODEM_SMS_OUTCOME_NONE) {
            outcome = MODEM_SMS_OUTCOME_TIMEOUT;
        }
        if (outcome == MODEM_SMS_OUTCOME_OK) {
            emit_status_increment(hooks, MODEM_SMS_ACTION_INCREMENT_SENT);
        }
        emit_send_result(request, hooks, outcome);
        emit_complete(request, hooks, outcome, false);
        return;
    }
    if (kind == MODEM_SMS_COMMAND_STATUS_PRESERVE) {
        if (request->operation == MODEM_SMS_PROTOCOL_MAILBOX) {
            emit_mailbox_result(request, hooks, MODEM_SMS_OUTCOME_TIMEOUT,
                                false);
            emit_complete(request, hooks, MODEM_SMS_OUTCOME_TIMEOUT, false);
        } else if (request->operation == MODEM_SMS_PROTOCOL_READ) {
            if (s_protocol.restore_outcome == MODEM_SMS_OUTCOME_OK ||
                s_protocol.restore_outcome == MODEM_SMS_OUTCOME_NONE) {
                s_protocol.restore_outcome = MODEM_SMS_OUTCOME_TIMEOUT;
            }
            complete_selected_read(request, hooks, false);
        } else {
            emit_complete(request, hooks, MODEM_SMS_OUTCOME_TIMEOUT, false);
        }
        return;
    }
    if (kind == MODEM_SMS_COMMAND_STATUS_CONSUME &&
        request->operation == MODEM_SMS_PROTOCOL_READ) {
        /* Timeout is an uncertain write outcome; force preserve mode even
         * though no message body was requested afterward. */
        restore_selected_status(request, hooks, now_ms,
                                MODEM_SMS_OUTCOME_TIMEOUT, false);
        return;
    }
    if ((request->operation == MODEM_SMS_PROTOCOL_MAILBOX ||
         request->operation == MODEM_SMS_PROTOCOL_READ) &&
        (kind == MODEM_SMS_COMMAND_CMGF_PDU ||
         kind == MODEM_SMS_COMMAND_CMGL ||
         kind == MODEM_SMS_COMMAND_CMGR)) {
        restore_text_mode(hooks, now_ms, MODEM_SMS_OUTCOME_TIMEOUT, false);
        return;
    }
    if ((request->operation == MODEM_SMS_PROTOCOL_MAILBOX ||
         request->operation == MODEM_SMS_PROTOCOL_READ) &&
        kind == MODEM_SMS_COMMAND_CMGF_TEXT) {
        if (request->operation == MODEM_SMS_PROTOCOL_MAILBOX) {
            emit_mailbox_result(request, hooks, MODEM_SMS_OUTCOME_TIMEOUT,
                                false);
            emit_complete(request, hooks, MODEM_SMS_OUTCOME_TIMEOUT, false);
        } else {
            restore_selected_status(request, hooks, now_ms,
                                    MODEM_SMS_OUTCOME_TIMEOUT, false);
        }
        return;
    }
    if (kind == MODEM_SMS_COMMAND_CMGD &&
        request->operation == MODEM_SMS_PROTOCOL_MAILBOX &&
        modem_sms_state_filtered_active()) {
        /* The delete outcome is uncertain. Keep the recognized control hidden,
         * stop this cleanup pass, and retry it on a later scan. */
        s_protocol.delete_command_in_flight = false;
        note_filtered_delete_failure(hooks);
        finish_filtered_cleanup(request, hooks);
        return;
    }

    bool not_ready = !sim_ready(hooks);
    if (kind == MODEM_SMS_COMMAND_CPMS) {
        if (request->operation == MODEM_SMS_PROTOCOL_SEND_TEXT) {
            emit_send_result(request, hooks, MODEM_SMS_OUTCOME_TIMEOUT);
        } else if (request->operation == MODEM_SMS_PROTOCOL_SAVE) {
            emit_save_result(request, hooks, MODEM_SMS_OUTCOME_TIMEOUT,
                             not_ready);
        } else if (request->operation == MODEM_SMS_PROTOCOL_MAILBOX) {
            emit_mailbox_result(request, hooks, MODEM_SMS_OUTCOME_TIMEOUT,
                                not_ready);
        } else if (request->operation == MODEM_SMS_PROTOCOL_READ) {
            emit_read_result(request, hooks, MODEM_SMS_OUTCOME_TIMEOUT,
                             not_ready);
        } else if (request->operation == MODEM_SMS_PROTOCOL_DELETE) {
            emit_delete_result(request, hooks, MODEM_SMS_OUTCOME_TIMEOUT,
                               not_ready);
        }
        emit_complete(request, hooks, MODEM_SMS_OUTCOME_TIMEOUT, false);
        return;
    } else if (kind == MODEM_SMS_COMMAND_CMGS_PROMPT) {
        emit_send_result(request, hooks, MODEM_SMS_OUTCOME_TIMEOUT);
        emit_complete(request, hooks, MODEM_SMS_OUTCOME_TIMEOUT, false);
        return;
    } else if (kind == MODEM_SMS_COMMAND_CMGS_FINAL) {
        emit_send_result(request, hooks, MODEM_SMS_OUTCOME_UNCERTAIN);
        emit_complete(request, hooks, MODEM_SMS_OUTCOME_UNCERTAIN, false);
        return;
    } else if (kind == MODEM_SMS_COMMAND_CMGW_PROMPT ||
               kind == MODEM_SMS_COMMAND_CMGW_FINAL) {
        if (request->operation == MODEM_SMS_PROTOCOL_SAVE) {
            modem_sms_outcome_t outcome = kind == MODEM_SMS_COMMAND_CMGW_FINAL
                ? MODEM_SMS_OUTCOME_UNCERTAIN : MODEM_SMS_OUTCOME_TIMEOUT;
            emit_save_result(request, hooks, outcome, not_ready);
            emit_complete(request, hooks, outcome, false);
        } else {
            emit_send_result(request, hooks, MODEM_SMS_OUTCOME_OK);
            emit_complete(request, hooks, MODEM_SMS_OUTCOME_OK, true);
        }
        return;
    } else if (kind == MODEM_SMS_COMMAND_CMGL ||
               kind == MODEM_SMS_COMMAND_CMGR) {
        if (request->operation == MODEM_SMS_PROTOCOL_READ) {
            emit_read_result(request, hooks, MODEM_SMS_OUTCOME_TIMEOUT,
                             false);
        } else {
            emit_mailbox_result(request, hooks, MODEM_SMS_OUTCOME_TIMEOUT,
                                false);
        }
        emit_complete(request, hooks, MODEM_SMS_OUTCOME_TIMEOUT, false);
        return;
    } else if (kind == MODEM_SMS_COMMAND_CMGD) {
        s_protocol.delete_command_in_flight = false;
        emit_delete_result(request, hooks, MODEM_SMS_OUTCOME_UNCERTAIN,
                           not_ready);
        emit_complete(request, hooks, MODEM_SMS_OUTCOME_UNCERTAIN, false);
        return;
    }
    emit_complete(request, hooks, MODEM_SMS_OUTCOME_TIMEOUT, false);
}

modem_sms_outcome_t modem_sms_protocol_cancel_outcome(
    const modem_sms_protocol_request_t *request) {
    if (request == NULL) {
        return MODEM_SMS_OUTCOME_CANCELLED;
    }
    switch (request->operation) {
    case MODEM_SMS_PROTOCOL_SEND_TEXT:
        if (s_protocol.send_confirmed) {
            return MODEM_SMS_OUTCOME_OK;
        }
        return s_protocol.payload_submitted
            ? MODEM_SMS_OUTCOME_UNCERTAIN
            : MODEM_SMS_OUTCOME_CANCELLED;
    case MODEM_SMS_PROTOCOL_SEND_BINARY:
        if (s_protocol.send_confirmed ||
            s_protocol.binary_outcome == MODEM_SMS_OUTCOME_OK) {
            return MODEM_SMS_OUTCOME_OK;
        }
        return s_protocol.payload_submitted ||
                       s_protocol.binary_segments_accepted != 0u
            ? MODEM_SMS_OUTCOME_UNCERTAIN
            : MODEM_SMS_OUTCOME_CANCELLED;
    case MODEM_SMS_PROTOCOL_SAVE:
        return s_protocol.payload_submitted
            ? MODEM_SMS_OUTCOME_UNCERTAIN
            : MODEM_SMS_OUTCOME_CANCELLED;
    case MODEM_SMS_PROTOCOL_DELETE:
        return s_protocol.delete_command_in_flight ||
                       s_protocol.delete_commands_accepted != 0u
            ? MODEM_SMS_OUTCOME_UNCERTAIN
            : MODEM_SMS_OUTCOME_CANCELLED;
    case MODEM_SMS_PROTOCOL_MAILBOX:
    case MODEM_SMS_PROTOCOL_READ:
    case MODEM_SMS_PROTOCOL_NONE:
    default:
        return MODEM_SMS_OUTCOME_CANCELLED;
    }
}

void modem_sms_protocol_cancel(void) {
    modem_sms_protocol_operation_finished();
    modem_sms_state_selected_begin(0u, 0u, false);
    modem_sms_state_binary_set_send_ok(false);
    memset(&s_protocol, 0, sizeof(s_protocol));
    s_protocol.mailbox = MODEM_SMS_MAILBOX_INBOX;
}
