#ifndef SM_TAKEOVER_H
#define SM_TAKEOVER_H

#include <stdbool.h>
#include <stdint.h>

struct json_object;

#define OWNER_LEDGER_FILE "/data/shadowmount/owner.json"

// Ask a running real Kraken Mount (port 8097) to quit, wait while it has a
// game running, then release its /system_ex/app title mounts. Returns false
// only when a stop was requested while waiting.
bool sm_takeover_run(void);
// True once sm_takeover_run() finished; scan/mount work may proceed.
bool sm_takeover_ownership_ready(void);
// Retry legacy Kraken title mounts that were busy earlier (scan passes).
void sm_takeover_retry_legacy_mounts(void);
// Adoption check for a title registered by real Kraken Mount. Returns true
// when the conflict guard may be relaxed. *busy_out is set when the title
// is Kraken's but its legacy mount is still present.
bool sm_takeover_kraken_adoptable(const char *title_id, bool *busy_out);

// Ownership ledger: detect a leftover ledger and write ours.
void sm_ownership_begin(void);
// Remove the ledger on clean shutdown.
void sm_ownership_end(void);
// kern.boottime in seconds (0 when unavailable), as stored in the ledger.
int64_t sm_ownership_boot_time(void);
// Add the GET /api/v1/ownership fields to a response object.
bool sm_ownership_add_json(struct json_object *response);

#endif
