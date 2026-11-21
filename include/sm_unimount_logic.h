#ifndef SM_UNIMOUNT_LOGIC_H
#define SM_UNIMOUNT_LOGIC_H

// Pure decision helpers for Kraken takeover, the Kraken-compatible listener,
// automatic PKG mounting and AMPR selection. Inputs are injected so the host
// tests (tests/test_unimount_logic.c) exercise the same code as the payload.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sm_limits.h"

#define SM_KRAKEN_PORT 8097u
#define SM_KRAKEN_LEGACY_CACHES "/data/krakenmount/caches"
#define SM_COMPAT_MAX_REQUEST 16384u

// --- Takeover of a running Kraken Mount ---

typedef enum {
  SM_PEER_NONE = 0,  // nothing listens on the port (connection refused)
  SM_PEER_OTHER,     // a valid answer that is clearly not Kraken Mount
  SM_PEER_KRAKEN,    // real Kraken Mount
  SM_PEER_UNIMOUNT,  // our own compatible listener (carries "unimount")
  SM_PEER_UNKNOWN,   // something listens, but its reply is not conclusive
} sm_peer_kind_t;

// Classify the bytes a GET /api/state probe read from a listener before it
// closed, reset or the probe deadline passed (head plus any body prefix).
// Real Kraken answers 200 with a JSON object holding a string "version" and
// an array "games", without "unimount"; a prefix that already holds both is
// enough. No reply, a cut-off head, a 5xx or a cut-off body without those keys
// is SM_PEER_UNKNOWN (never "absent"). *why receives a static reason. The
// caller maps "connection refused" to SM_PEER_NONE itself.
sm_peer_kind_t sm_takeover_classify_reply(const char *reply, size_t length,
                                          char version[32], const char **why);

typedef enum {
  SM_QUIT_ACCEPTED = 0,   // 2xx: Kraken exits shortly
  SM_QUIT_GAME_RUNNING,   // 409: a game runs; ask again later
  SM_QUIT_REFUSED,        // anything else (old build, other app, no answer)
} sm_quit_outcome_t;

sm_quit_outcome_t sm_takeover_quit_outcome(int http_status);

// Parse "HTTP/1.x NNN ..." up to the blank line. Returns false until the head
// is complete or when the status line is malformed.
bool sm_http_parse_response(const char *buf, size_t len, int *status_out,
                            size_t *body_offset_out);

// nullfs /data/krakenmount/caches/<TID> mounted on /system_ex/app/<TID>.
bool sm_takeover_is_legacy_mount(const char *fstype, const char *from,
                                 const char *on, char title_id[MAX_TITLE_ID]);

// True when a mount point belongs to a live sandbox of the title
// (/mnt/sandbox/<TID>_NNN or below).
bool sm_takeover_sandbox_mount_for_title(const char *mount_point,
                                         const char *title_id);

typedef struct {
  bool user_param_json;       // /user/app/<TID>/sce_sys/param.json
  bool mount_link;            // /user/app/<TID>/mount.lnk (ours)
  bool user_app_pkg;          // /user/app/<TID>/app.pkg (installed PKG)
  bool ext_installed;         // installed copy on extended storage
  bool kraken_cache_done;     // /data/krakenmount/caches/<TID>/.done
  bool legacy_mount_present;  // Kraken's /system_ex/app/<TID> still mounted
} sm_adopt_facts_t;

typedef enum {
  SM_ADOPT_NO = 0,  // not a Kraken registration: keep the conflict guard
  SM_ADOPT_YES,     // registered by Kraken Mount: build our cache and adopt
  SM_ADOPT_LATER,   // Kraken registration whose legacy mount is still busy
} sm_adopt_t;

sm_adopt_t sm_takeover_adoption(const sm_adopt_facts_t *facts);

// --- Startup, replacement and shutdown around a running game ---

typedef struct {
  bool predecessor;  // another UniMount instance is running (informational)
  bool app_running;  // sceSystemServiceGetAppIdOfRunningBigApp() > 0
  bool title_known;  // the running application's title id was determined
  bool managed;      // that title has our link files or a nullfs on
                     // /system_ex/app/<TID>
} sm_startup_facts_t;

typedef enum {
  SM_STARTUP_PROCEED = 0,  // signal the predecessor / start services now
  SM_STARTUP_WAIT,         // stay passive until the managed game is closed
} sm_startup_decision_t;

// A running managed title, or one whose title id is unknown, keeps a new
// instance passive, with or without a predecessor. Unmanaged apps do not.
sm_startup_decision_t sm_startup_decide(const sm_startup_facts_t *facts);

typedef enum {
  SM_RESTART_WAIT_FOR_APP = 0,  // predecessor deferred for a game: wait again
  SM_RESTART_GIVE_UP,           // predecessor stuck without a game: exit
} sm_restart_timeout_t;

// The predecessor did not exit within the takeover timeout. Never continue
// alongside it.
sm_restart_timeout_t sm_restart_timeout_decide(bool app_running);

typedef enum {
  SM_STOP_NONE = 0,  // no stop request
  SM_STOP_NOW,       // shut down now
  SM_STOP_DEFER,     // keep running until the active game has exited
} sm_stop_decision_t;

// requests counts deferrable stop requests (signals, STOP file). immediate is
// a stop that cannot wait (console shutting down, scanner failure). A second
// deferrable request forces the stop.
sm_stop_decision_t sm_stop_decide(unsigned requests, bool immediate,
                                  bool game_active);

typedef struct {
  bool has_layers;       // the session still holds mounts or redirects
  int32_t pid;           // game process bound to the session, 0 before exec
  bool pid_alive;        // that process still exists
  bool handoff_pending;  // the lifecycle awaits a same-app replacement for it
  bool sandbox_exists;   // a /mnt/sandbox/<TID>_NNN directory exists
} sm_layer_owner_facts_t;

// A shutdown keeps a session's sandbox layers when they may still be in use:
// a live process, a launch not yet bound to a process, or a same-app handoff.
bool sm_shutdown_keeps_game_layers(const sm_layer_owner_facts_t *facts);

// Small set of title ids (no duplicates, invalid ids rejected).
bool sm_title_list_add(char list[][MAX_TITLE_ID], size_t *count, size_t max,
                       const char *title_id);
bool sm_title_list_contains(const char list[][MAX_TITLE_ID], size_t count,
                            const char *title_id);

// --- Ownership ledger (/data/shadowmount/owner.json) ---

bool sm_owner_ledger_format(char *out, size_t out_size, const char *product,
                            const char *version, long pid, int64_t started,
                            int64_t boot_time);
// Reads pid and boot_time from a ledger; false when it is not one of ours.
bool sm_owner_ledger_parse(const char *body, size_t len, long *pid_out,
                           int64_t *boot_time_out);

// --- Kraken-compatible HTTP listener ---

typedef struct {
  char method[8];
  char path[128];
  char host[128];
  size_t head_len;        // bytes up to and including the blank line
  size_t content_length;  // declared body length
} sm_http_request_head_t;

// 1: complete request in buf; 0: need more bytes; negative: -HTTP status to
// answer (400 bad request, 413 too large with body, 431 head too large).
int sm_http_request_scan(const char *buf, size_t len, size_t cap,
                         sm_http_request_head_t *out);

typedef enum {
  SM_COMPAT_NOT_FOUND = 0,
  SM_COMPAT_STATE,     // GET /api/state
  SM_COMPAT_RESCAN,    // POST /api/rescan
  SM_COMPAT_QUIT,      // POST /api/quit
  SM_COMPAT_ROOT,      // GET /
} sm_compat_route_t;

sm_compat_route_t sm_compat_route(const char *method, const char *path);

// Location for GET /: the UniMount UI on api_port of the requested host.
// Hosts with unexpected characters fall back to 127.0.0.1.
bool sm_compat_redirect_location(const char *host, unsigned api_port,
                                 char *out, size_t out_size);

// --- Automatic mounting of plaintext debug packages ---

typedef struct {
  char path[MAX_PATH];
  uint64_t size;
  int64_t mtime;
  char error[64];
  bool used;
} sm_auto_failure_t;

// A remembered failure blocks only the identical (path, size, mtime).
bool sm_auto_failure_blocks(const sm_auto_failure_t *list, size_t count,
                            const char *path, uint64_t size, int64_t mtime);
// Remember (replacing an older record for the same path). Oldest slot is
// reused when the table is full.
void sm_auto_failure_record(sm_auto_failure_t *list, size_t count,
                            const char *path, uint64_t size, int64_t mtime,
                            const char *error);
void sm_auto_failure_forget(sm_auto_failure_t *list, size_t count,
                            const char *path);
// Reason the job tap passes for a storage-job cancel the user requested
// (not rest mode, shutdown or a game starting).
#define SM_AUTO_REASON_USER_CANCELLED "cancelled_by_user"

// Busy, cancelled or changed-while-reading results are retried, not recorded.
// A user cancel (SM_AUTO_REASON_USER_CANCELLED) is recorded like a failure.
bool sm_auto_error_is_transient(int status, const char *reason);

typedef enum {
  SM_AUTO_LIBRARY_NONE = 0,     // title not in the library
  SM_AUTO_LIBRARY_SAME_PKG,     // already mounted from this .pkg
  SM_AUTO_LIBRARY_OTHER,        // provided by another source
} sm_auto_library_t;

typedef struct {
  bool enabled;            // [kraken] auto_mount
  bool ownership_ready;    // takeover finished
  bool game_running;
  bool job_active;         // any storage job / auto job in flight
  bool stable;             // stability wait passed
  bool eligible;           // plaintext debug base PKG (header checks passed)
  bool failed_same_key;    // remembered failure for (path, size, mtime)
  bool registered_conflict;  // registered by something else, not adoptable
  sm_auto_library_t library;
} sm_auto_facts_t;

typedef enum {
  SM_AUTO_IGNORE = 0,  // not a candidate (disabled, retail, PS4, patch)
  SM_AUTO_WAIT,        // try again on a later pass
  SM_AUTO_START,       // start the build+register job now
  SM_AUTO_READY,       // already mounted from this package
  SM_AUTO_DUPLICATE,   // the title is provided by something else
  SM_AUTO_FAILED,      // failed before; waits for a change or explicit mount
} sm_auto_decision_t;

sm_auto_decision_t sm_auto_decide(const sm_auto_facts_t *facts);

// --- AMPR-emu-NG for zstd folder games ---

// Folder games packed with ampr_zstd.index only run with ampr-emu-ng.
bool sm_ampr_ng_folder_needs_injection(bool inject_enabled, bool kraken_cache,
                                       bool has_zstd_index);

typedef enum {
  SM_AMPR_SOURCE_SHARED_NG_DIR = 0,  // <emulators>/ampr-emu-ng/libSceAmpr.sprx
  SM_AMPR_SOURCE_SHARED_TOP,         // <emulators>/libSceAmpr.sprx (if ng)
  SM_AMPR_SOURCE_BUNDLED,            // the copy embedded in the payload
} sm_ampr_source_t;

sm_ampr_source_t sm_ampr_ng_select_source(bool shared_ng_dir_valid,
                                          bool shared_top_valid);

// ampr-emu-ng embeds "ampr-emu-ng <version>".
bool sm_ampr_buffer_is_ng(const unsigned char *data, size_t length);
// 1 for ampr-emu-ng, 0 otherwise, -1 on read errors.
int sm_ampr_file_is_ng(const char *path);

// Emulator updates replace libSceAmpr.sprx only with a build of the same
// runtime (ampr-emu-ng for ampr-emu-ng). Other names are not restricted.
// source_ng/emulator_ng: 1 ng, 0 upstream, -1 unknown (refused).
bool sm_ampr_emulator_replacement_allowed(const char *name, int source_ng,
                                          int emulator_ng);

#endif
