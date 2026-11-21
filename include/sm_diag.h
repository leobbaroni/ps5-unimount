#ifndef SM_DIAG_H
#define SM_DIAG_H

// Launch diagnostics. The sm_diag_note_* hooks only copy the event into a
// bounded queue and wake the diagnostics thread: no file I/O, no waiting, and
// they never change the launch they observe. A full queue drops the event.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct json_object;

// ShellCore LAUNCH handled: status is the response returned to ShellCore,
// native_errno/layer describe a failed launch mount (layer may be NULL).
void sm_diag_note_launch(const char *title_id, int status, int native_errno,
                         const char *layer);
void sm_diag_note_launch_failed(const char *title_id, int status);
void sm_diag_note_sandbox_ready(const char *title_id, int status);
// Lifecycle watcher: a new game process (not a same-app handoff).
void sm_diag_note_process_started(const char *title_id, pid_t pid,
                                  uint32_t app_id, uint64_t exec_time_us);
// NOTE_EXIT of the active game process (it may still be handed off).
void sm_diag_note_process_exit(pid_t pid);
void sm_diag_note_process_handoff(pid_t old_pid, pid_t new_pid);
// Final per-process exit after the handoff grace.
void sm_diag_note_process_finalized(pid_t pid, uint64_t exit_time_us);
// sm_mdbg finding. kind is sm_diag_finding_kind_t.
void sm_diag_note_game_error(pid_t pid, const char *title_id, int kind,
                             const char *text);

// Shutdown: drop new events, let the diagnostics thread process what is
// queued, write launches.json once more and join it. Call after the event
// producers (ShellCore hooks/service, lifecycle watcher) stopped and before
// mdbg, app.db and the log shut down. Idempotent.
void sm_diag_stop(void);

// API helpers. They return a new JSON object owned by the caller.
// True when url is one of the GET routes served by sm_diag_api_response().
bool sm_diag_api_route(const char *url);
// Build the full response body ({"status":0,...} or an error object) for a
// GET route. title_id and limit are the raw query values (may be NULL).
struct json_object *sm_diag_api_response(const char *url, const char *title_id,
                                         const char *limit, int *http_status);
// Add "last_launch" ({id,outcome,at,short_error,category} or null) to a
// library/game item. Returns false only when out of memory.
bool sm_diag_add_last_launch(struct json_object *item, const char *title_id);

// System report (src/sm_diag_system.c).
struct json_object *sm_diag_system_report(void);
// A mount of this filesystem type ("unionfs", "pfs") succeeded; the system
// report uses it as evidence when the kernel's filesystem list is missing.
// Lock-free; safe on launch and mount paths.
void sm_diag_note_fs_mounted(const char *fstype);

#endif
