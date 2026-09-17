#ifndef OPERATOR_NAME_DB_H
#define OPERATOR_NAME_DB_H

#define OPERATOR_NAME_DB_NAME_MAX 16u

/* Exact serving-PLMN lookup: MCC is exactly three ASCII digits; MNC is
 * exactly two or three ASCII digits. Both arguments must be NUL-terminated.
 * Leading zeroes and MNC width are significant; do not pad or trim them.
 * Returns an immutable, NUL-terminated ASCII name of at most NAME_MAX bytes,
 * with static lifetime, or NULL for unknown/malformed input (including NULL).
 * No allocation, mutable state, SIM-name policy, or numeric fallback. */
const char *operator_name_db_lookup(const char *mcc, const char *mnc);

#endif
