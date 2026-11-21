#ifndef SM_KRAKEN_AUTO_H
#define SM_KRAKEN_AUTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sm_limits.h"

struct json_object;

// Automatic build+register of plaintext debug base PKGs found by the scanner.
// The job itself is POST /api/v1/packages/mount's storage job.
bool sm_kraken_auto_start(void);
void sm_kraken_auto_stop(void);

// Scanner hooks. note_package runs on the scan thread and only stats the file.
void sm_kraken_auto_begin_pass(void);
void sm_kraken_auto_note_package(const char *path, bool *unstable_out);
void sm_kraken_auto_end_pass(bool complete);

// Storage job tap: every Kraken PKG job reports its result here.
void sm_kraken_auto_job_finished(const char *source, int status,
                                 const char *reason);
// An explicit API mount of this file clears its remembered failure.
void sm_kraken_auto_forget_failure(const char *path);
// True while an automatic job is in flight.
bool sm_kraken_auto_busy(void);
// True while a game runs or a background title blocks: automatic jobs do not
// start and a running automatic job is cancelled (retried later).
bool sm_kraken_auto_game_active(void);

typedef struct {
  char path[MAX_PATH];
  char title_id[MAX_TITLE_ID];
  char title_name[MAX_TITLE_NAME];
  char version[32];
  uint64_t size;
  const char *status;  // Kraken status: pending, building, error, duplicate
  char error[64];
} sm_kraken_auto_entry_t;

// Copy the candidates that are not ready yet. Returns the number copied.
size_t sm_kraken_auto_snapshot(sm_kraken_auto_entry_t *out, size_t max);
// Add {enabled, queued, building, failed:[{path,error}], overflow} to an
// object.
bool sm_kraken_auto_add_status_json(struct json_object *object);

#endif
