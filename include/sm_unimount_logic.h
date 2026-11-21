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

// --- Sandbox session kinds ---

typedef enum {
  SM_LAYER_ALIAS = 0,  // plain nullfs directory alias outside app0
  SM_LAYER_OVERLAY,    // a unionfs layer, or anything mounted on/inside app0
} sm_layer_kind_t;

// app0_path is the sandbox's /mnt/sandbox/<TID>_NNN/app0.
sm_layer_kind_t sm_sandbox_layer_kind(bool unionfs, const char *mount_path,
                                      const char *app0_path);

typedef enum {
  SM_SESSION_NONE = 0,  // no layers of ours in the live game's sandbox
  SM_SESSION_ALIASES,   // only plain directory aliases (/data, storage, fonts)
  SM_SESSION_OVERLAY,   // a fakelib/fakelib2 overlay, an app0 cover, app0/kraken
                        // or NSFS backport redirects
} sm_session_kind_t;

// redirects: NSFS backport redirects are installed for the game.
sm_session_kind_t sm_session_kind(const sm_layer_kind_t *layers, size_t count,
                                  bool redirects);

// --- Stop requests ---

typedef enum {
  SM_STOP_NONE = 0,   // no stop request
  SM_STOP_NOW,        // shut down now with the normal teardown
  SM_STOP_DEFER,      // keep running until the active game has exited
  SM_STOP_NOW_KEEP,   // shut down now, keeping the live game's overlay layers
} sm_stop_decision_t;

typedef struct {
  unsigned requests;  // deferrable requests so far (signals and STOP files)
  bool by_signal;     // at least one was a signal: a SIGKILL may follow
                      // (PS5 Payload Manager kills 1.5 s after SIGTERM)
  bool immediate;     // cannot wait (console shutdown, scanner failure)
  bool game_active;   // the lifecycle tracks an active game
  sm_session_kind_t session;  // layers of ours in that game's sandbox
} sm_stop_facts_t;

// STOP file: defer for any active game. Signal: defer only for an overlay
// session (alias-only sessions take the normal teardown, verified safe on
// hardware). A second request or an immediate stop never waits; it keeps an
// overlay session's layers and tears an alias-only session down as usual.
sm_stop_decision_t sm_stop_decide(const sm_stop_facts_t *facts);

typedef struct {
  bool has_layers;       // the session still holds mounts or redirects
  bool overlay;          // the session is SM_SESSION_OVERLAY
  int32_t pid;           // game process bound to the session, 0 before exec
  bool pid_alive;        // that process still exists
  bool handoff_pending;  // the lifecycle awaits a same-app replacement for it
  bool sandbox_exists;   // a /mnt/sandbox/<TID>_NNN directory exists
} sm_layer_owner_facts_t;

// A shutdown keeps an overlay session's layers when they may still be in
// use: a live process, a launch not yet bound to a process, or a same-app
// handoff. Alias-only sessions are always torn down (non-forced).
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

// --- Sandbox session record (/data/shadowmount/sandbox-session.json) ---
//
// The owning instance records every sandbox mount and NSFS redirect set it
// removes at game exit, so that a later instance can remove them after the
// owner was killed ("adoption").

#define SM_SESSION_RECORD_FORMAT 1
#define SM_SESSION_RECORD_MAX_BYTES (512u * 1024u)
#define SM_SESSION_MAX_LAYERS 24u
#define SM_SESSION_MAX_REDIRECT_SETS 2u
#define SM_SESSION_MAX_REDIRECTS 256u
#define SM_SESSION_REDIRECT_BYTES (64u * 1024u)

// A directory the owner created only as a parent of mount points (no mount;
// cleanup removes it with rmdir after its children).
#define SM_SESSION_PARENT_LABEL "parent"
#define SM_SESSION_PARENT_FSTYPE "none"

typedef struct {
  char label[16];         // game, global, fakelib2 (unionfs), directory
                          // (nullfs) or parent (no mount, empty source)
  char fstype[16];        // unionfs, nullfs or none
  char source[MAX_PATH];
  char target[MAX_PATH];  // mount path inside the sandbox
  int32_t fsid[2];        // f_fsid of the mount, both words
  bool read_only;
  bool created;           // the owner created the mount directory
} sm_session_layer_t;

typedef struct {
  char source[MAX_PATH];  // backport root
  char target[MAX_PATH];  // the sandbox's NSFS app0
  uint64_t device;        // st_dev of that app0
  int32_t fsid[2];        // f_fsid of that NSFS mount
  size_t count;           // destinations, in creation order
  size_t bytes;           // used bytes of storage (NUL-terminated strings)
  char storage[SM_SESSION_REDIRECT_BYTES];
} sm_session_redirects_t;

typedef struct {
  int64_t boot_time;       // kern.boottime seconds, as in owner.json
  int64_t owner_pid;
  int64_t game_pid;        // 0 before the game process is known
  uint32_t app_id;         // AppInfo app_id of game_pid, 0 when unknown
  bool complete;           // false: a layer could not be identified
  char title_id[MAX_TITLE_ID];
  char sandbox_app0[MAX_PATH];
  char lib_mount[MAX_PATH];  // the fakelib common/lib mount path
  size_t layer_count;      // in creation order
  sm_session_layer_t layers[SM_SESSION_MAX_LAYERS];
  size_t redirect_count;
  sm_session_redirects_t redirects[SM_SESSION_MAX_REDIRECT_SETS];
} sm_session_record_t;

// Appends one destination; false when it does not fit (the set is then
// unchanged).
bool sm_session_redirects_add(sm_session_redirects_t *set,
                              const char *destination);

// JSON text of the record; *out is malloc'd. version is informational.
bool sm_session_record_format(const sm_session_record_t *record,
                              const char *version, char **out);
// Strict parse. *why receives a static reason when it fails.
bool sm_session_record_parse(const char *body, size_t length,
                             sm_session_record_t *out, const char **why);

bool sm_session_layer_is_parent(const sm_session_layer_t *layer);
// Mounts plus one per NSFS redirect set (the count in [ADOPT] lines);
// parent directories are not counted.
size_t sm_session_record_units(const sm_session_record_t *record);
// No layer entry (mount or parent directory) and no redirect set.
bool sm_session_record_empty(const sm_session_record_t *record);
sm_session_kind_t sm_session_record_kind(const sm_session_record_t *record);

typedef struct {
  const char *fstype;
  const char *from;
  const char *on;
  int32_t fsid[2];
  size_t from_size;  // capacity of the from field (MNAMELEN); 0: unlimited
} sm_mount_entry_t;

typedef struct {
  int64_t boot_time;         // this boot
  bool game_alive;           // the record's game pid exists
  uint32_t game_app_id;      // AppInfo of that pid (0 unknown)
  char game_title_id[MAX_TITLE_ID];
  uint32_t running_app_id;   // the running big app (0 none)
  char running_title_id[MAX_TITLE_ID];  // "" when unknown
  const sm_mount_entry_t *mounts;  // live mount table, in mount order
  size_t mount_count;
} sm_adopt_live_t;

// Every recorded mount must be mounted with the same type, source, path and
// filesystem id, and be the topmost mount on its path (parent directories
// need no mount); every redirect set's
// NSFS app0 must be mounted with the same id. NULL when all match, else a
// reason written to buf.
const char *sm_session_record_match(const sm_session_record_t *record,
                                    const sm_mount_entry_t *mounts,
                                    size_t count, char *buf, size_t size);

// All checks for adopting a record; true to adopt. On false, *reason
// (written to buf or static) says why.
bool sm_session_adopt_decide(const sm_session_record_t *record,
                             const sm_adopt_live_t *live, char *buf,
                             size_t size, const char **reason);

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
