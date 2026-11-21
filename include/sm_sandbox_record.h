#ifndef SM_SANDBOX_RECORD_H
#define SM_SANDBOX_RECORD_H

// Writer of /data/shadowmount/sandbox-session.json: the sandbox mounts and
// NSFS redirect sets the owning instance removes when the game exits.
// sm_fakelib.c and sm_pkg_backport.c publish snapshots on every unlock of
// their session state; this module only copies them under its own short
// lock, and a writer thread does all file I/O, so neither the launch path
// nor should_stop_requested() ever waits on the file.

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "sm_limits.h"
#include "sm_unimount_logic.h"

#define SANDBOX_SESSION_FILE "/data/shadowmount/sandbox-session.json"

typedef struct {
  pid_t pid;
  bool complete;  // false: an owned layer has no filesystem id
  char title_id[MAX_TITLE_ID];
  char sandbox_app0[MAX_PATH];
  char lib_mount[MAX_PATH];
  size_t layer_count;  // owned mounts and created parent directories, in
                       // session order
  sm_session_layer_t layers[SM_SESSION_MAX_LAYERS];
} sm_record_fakelib_t;

typedef struct {
  pid_t pid;
  bool complete;  // false: the destinations did not fit
  char title_id[MAX_TITLE_ID];
  sm_session_redirects_t redirects;
} sm_record_redirect_set_t;

// Called with the publishing module's lock held: copies only.
void sm_session_record_publish_fakelib(const sm_record_fakelib_t *snapshot);
void sm_session_record_publish_redirects(const sm_record_redirect_set_t *sets,
                                         size_t count);

// Starts the writer; the current state is written (or the file removed when
// it is empty) right away.
bool sm_session_record_start(void);
// Writes the last published state and stops the writer.
void sm_session_record_stop(void);

#endif
