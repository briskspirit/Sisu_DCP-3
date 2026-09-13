#include "pico/sync.h"

void critical_section_stub_peer_enter(critical_section_t *lock)
{
    critical_section_enter_blocking(lock);
}

void critical_section_stub_peer_exit(critical_section_t *lock)
{
    critical_section_exit(lock);
}
