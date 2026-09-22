#ifndef MODEM_NETWORK_SIM_H
#define MODEM_NETWORK_SIM_H

#include "services/modem_network.h"

/* Bounded enrichment of an unpublished scan. No persistent SIM cache: every
 * explicit scan rereads the card, including operator/OTA preference changes. */
void modem_network_sim_begin(modem_network_entry_t *entries, uint8_t count);
bool modem_network_sim_prepare(const modem_network_backend_t *backend,
                               char *command, size_t cap);
bool modem_network_sim_line(const modem_network_backend_t *backend, const char *line);
/* True means another metadata read remains. False publishes the final order. */
bool modem_network_sim_final(bool ok, bool transport_clean);

#endif
