#ifndef CALL_TYPES_H
#define CALL_TYPES_H

/* Vendor-neutral call-model boundary types (§16.12).
 *
 * The neutral leg/direction/mode enums, the normalized call-event kinds, and the
 * neutral call-model timing profile live here so BOTH the vendor adapter
 * (modem_vendor.h) and the call model (modem_call_model.h) can share them WITHOUT
 * a vendor->model dependency. This header depends on nothing but <stdint.h> /
 * <stdbool.h> — it must stay neutral (no vendor conditionals or timing values). A concrete
 * per-backend timing profile is supplied by the
 * test/integration and passed into call_model_init by value; the model bakes no
 * vendor constants.
 */

#include <stdbool.h>
#include <stdint.h>

/* Shared flat call projection contract. These values are consumed by the pure
 * call model, modem service, and UI; none belongs to a concrete modem vendor. */
#define MODEM_PHONE_MAX 32u

typedef enum {
    MODEM_CALL_IDLE = 0,
    MODEM_CALL_RINGING,
    MODEM_CALL_DIALING,
    MODEM_CALL_ANSWERING,
    MODEM_CALL_ACTIVE,
    MODEM_CALL_ENDING,
} modem_call_state_t;

typedef enum {
    MODEM_CALL_RESULT_NONE = 0,
    MODEM_CALL_RESULT_CONNECTED,
    MODEM_CALL_RESULT_NO_CARRIER,
    MODEM_CALL_RESULT_BUSY,
    MODEM_CALL_RESULT_NO_ANSWER,
    MODEM_CALL_RESULT_NO_DIALTONE,
} modem_call_result_t;

/* Normalized per-leg state — never store raw stat/dir numbers (§4.1). */
typedef enum { CALL_LEG_UNKNOWN=0, CALL_LEG_ACTIVE, CALL_LEG_HELD, CALL_LEG_DIALING,
               CALL_LEG_ALERTING, CALL_LEG_INCOMING, CALL_LEG_WAITING, CALL_LEG_RELEASING
} call_leg_state_t;
typedef enum { CALL_DIR_UNKNOWN=0, CALL_DIR_MO, CALL_DIR_MT } call_direction_t;
typedef enum { CALL_MODE_UNKNOWN=0, CALL_MODE_VOICE, CALL_MODE_DATA } call_mode_t;

/* Neutral call-control operation identity. The pure model uses it as the
 * transaction kind and each vendor adapter uses the same value to build its AT
 * command, capability, and timeout. Keeping it here prevents either side from
 * depending on the other's implementation header. */
typedef enum {
    CALL_TXN_NONE = 0,
    CALL_TXN_DIAL,
    CALL_TXN_ANSWER,
    CALL_TXN_HOLD,
    CALL_TXN_SWAP,
    CALL_TXN_WAIT_ANSWER,
    CALL_TXN_WAIT_REJECT,
    CALL_TXN_RELEASE_ACTIVE,
    CALL_TXN_RELEASE_LEG,
    CALL_TXN_HANGUP,
} call_txn_kind_t;

/* The 3GPP call-id domain represented by the fixed call table. */
#define MODEM_CALL_ID_MAX 7u

/* Normalized call-event kinds a vendor adapter emits from its call URC
 * (§4.1). SETUP_DONE (stat 7) and BUSY are table no-ops for the model. */
typedef enum {
    MODEM_CALL_EV_ACTIVE, MODEM_CALL_EV_HELD, MODEM_CALL_EV_DIALING,
    MODEM_CALL_EV_ALERTING_MO, MODEM_CALL_EV_RINGING_MT,
    MODEM_CALL_EV_WAITING_MT, MODEM_CALL_EV_RELEASED,
    MODEM_CALL_EV_SETUP_DONE, MODEM_CALL_EV_BUSY,
} modem_call_event_kind_t;

typedef struct {
    uint8_t call_id;
    bool id_valid;                   /* false = coarse/untrusted id; recover via CLCC */
    modem_call_event_kind_t event;
} modem_call_event_t;

/* One normalized 3GPP +CLCC row. Raw numeric direction/state/mode values stop
 * at the AT adapter boundary; the service and call model consume these neutral
 * enums regardless of modem vendor. */
typedef struct {
    uint8_t id;
    call_direction_t dir;
    call_mode_t mode;
    bool mpty;
    call_leg_state_t state;
    char number[MODEM_PHONE_MAX + 1u];
} modem_clcc_row_t;

/* Neutral call-model timing profile (§16.12) — passed to call_model_init and
 * copied BY VALUE into the model. The selected backend supplies these values;
 * the model has no vendor timing constants. NOTE: command_timeout is
 * deliberately ABSENT — the AT transport owns per-command deadlines; the model
 * only consumes a reported CMD_TIMEOUT (§16.2/§16.12). */
typedef struct {
    uint32_t clcc_keepalive_ms;   /* §6 keepalive cadence */
    uint32_t clcc_confirm_ms;     /* §5.3/§6 fast confirm / uncertain poll interval [BP] */
    uint32_t leg_limbo_ms;        /* §5.3 pending_removal force-evict horizon [BP] */
    uint32_t txn_policy_ms;       /* §7.1 fast-CLCC window before an unresolved op backs off [BP] */
    uint32_t txn_abandon_ms;      /* §7.1 hard abandonment backstop (~2x max GSM setup) [BP] */
} call_timing_t;

#endif /* CALL_TYPES_H */
