#ifndef SM_KRAKEN_COMPAT_H
#define SM_KRAKEN_COMPAT_H

#include <stdbool.h>

// Kraken Mount compatible HTTP listener ([compat] kraken_api, port 8097):
// GET /api/state, POST /api/rescan, POST /api/quit (always 409), GET / (302).
// Start only after the takeover finished; the thread retries the bind.
bool sm_kraken_compat_start(void);
void sm_kraken_compat_stop(void);
void sm_kraken_compat_status(bool *enabled_out, unsigned *port_out,
                             bool *bound_out);

#endif
