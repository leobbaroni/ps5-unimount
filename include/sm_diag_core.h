#ifndef SM_DIAG_CORE_H
#define SM_DIAG_CORE_H

// Pure launch-diagnostics logic: launch timeline state machine, record store,
// native error-history parsing, kernel-log line selection, classification and
// small parsers used by the system report. No platform calls, no file I/O:
// sm_diag.c and sm_diag_system.c own the threads, files and console probes.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct json_object;

#define SM_DIAG_TITLE_ID_SIZE 32u
#define SM_DIAG_TEXT_SIZE 256u
#define SM_DIAG_DETAIL_SIZE 160u
#define SM_DIAG_PATH_SIZE 192u
#define SM_DIAG_MAX_RECORDS 64u
#define SM_DIAG_MAX_TIMELINE 24u
#define SM_DIAG_MAX_FINDINGS 6u
#define SM_DIAG_MAX_KERNEL_LINES 12u
#define SM_DIAG_MAX_LAYERS 16u
#define SM_DIAG_MAX_FAKELIB_FILES 32u
#define SM_DIAG_FILE_NAME_SIZE 64u
#define SM_DIAG_MAX_NATIVE_ERRORS 4u
#define SM_DIAG_MAX_EVIDENCE 8u
#define SM_DIAG_KLOG_ANCHOR_SIZE 512u

typedef enum {
  SM_DIAG_NOT_OBSERVED = 0,
  SM_DIAG_PENDING,
  SM_DIAG_OK,
  SM_DIAG_FAILED,
} sm_diag_state_t;

typedef enum {
  SM_DIAG_STAGE_MOUNTED = 0,
  SM_DIAG_STAGE_REGISTERED,
  SM_DIAG_STAGE_LAUNCH_REQUESTED,
  SM_DIAG_STAGE_SANDBOX_READY,
  SM_DIAG_STAGE_PROCESS_STARTED,
  SM_DIAG_STAGE_RUNNING_CONFIRMED,
  SM_DIAG_STAGE_EXITED,
  SM_DIAG_STAGE_COUNT
} sm_diag_stage_id_t;

typedef enum {
  SM_DIAG_OUTCOME_LAUNCH_IN_PROGRESS = 0,
  SM_DIAG_OUTCOME_RUNNING,
  SM_DIAG_OUTCOME_EXITED_NORMALLY,
  SM_DIAG_OUTCOME_MOUNT_FAILED,
  SM_DIAG_OUTCOME_SPAWN_FAILED,
  SM_DIAG_OUTCOME_EXITED_EARLY,
  SM_DIAG_OUTCOME_CRASHED,
  SM_DIAG_OUTCOME_LAUNCH_NOT_OBSERVED,
} sm_diag_outcome_t;

// Finding kinds reported by sm_mdbg (see sm_mdbg.c handle_game_failure()).
typedef enum {
  SM_DIAG_FINDING_RTLD = 1,     // "[rtld] <pid> ERROR ..." line
  SM_DIAG_FINDING_FATAL = 2,    // fatal signal / exception report
  SM_DIAG_FINDING_CRASH_FLAG = 3, // mdbg exception-stop process flag
} sm_diag_finding_kind_t;

typedef enum {
  SM_DIAG_KLINE_SPAWN_ERROR = 1,
  SM_DIAG_KLINE_RTLD,
  SM_DIAG_KLINE_AUTH,
  SM_DIAG_KLINE_EXCEPTION,
} sm_diag_kline_kind_t;

typedef struct {
  sm_diag_state_t state;
  int64_t at_ms;      // Unix wall clock, milliseconds; 0 when not observed.
  uint64_t mono_us;   // Monotonic clock of this daemon run.
  char detail[SM_DIAG_DETAIL_SIZE];
} sm_diag_stage_t;

typedef struct {
  int64_t at_ms;
  uint64_t mono_us;
  char event[24];
  char detail[SM_DIAG_DETAIL_SIZE];
} sm_diag_event_t;

typedef struct {
  int kind; // sm_diag_finding_kind_t
  int64_t at_ms;
  char text[SM_DIAG_TEXT_SIZE];
} sm_diag_finding_t;

typedef struct {
  int kind; // sm_diag_kline_kind_t
  bool attributed; // The line itself names this launch's title or PID.
  char text[SM_DIAG_TEXT_SIZE];
} sm_diag_kline_t;

typedef struct {
  char label[16];
  char fstype[16];
  char from[SM_DIAG_PATH_SIZE];
  char on[SM_DIAG_PATH_SIZE];
} sm_diag_layer_t;

typedef struct {
  char file[SM_DIAG_FILE_NAME_SIZE];
  int64_t mtime; // Unix seconds of the history file.
  bool parsed;
  char short_error[32];
  bool has_internal_code;
  int32_t internal_code;
  char message[SM_DIAG_TEXT_SIZE];
  char title_id[SM_DIAG_TITLE_ID_SIZE];
  char display_version[64];
  bool has_tick;
  uint64_t tick;
  bool has_app_id;
  int64_t app_id;
  char attribution[16]; // "title_id", "time_window" or "" (history listing)
} sm_diag_native_error_t;

typedef struct {
  char category[24];
  char confidence[12]; // "observed" or "inferred"
  char rule[48];
  char evidence[SM_DIAG_MAX_EVIDENCE][SM_DIAG_TEXT_SIZE];
  size_t evidence_count;
  char advice[512];
} sm_diag_diagnosis_t;

typedef struct {
  uint64_t id;
  char title_id[SM_DIAG_TITLE_ID_SIZE];
  int managed; // 1, 0, or -1 when unknown
  int64_t created_ms;
  uint64_t created_mono_us;
  sm_diag_stage_t stages[SM_DIAG_STAGE_COUNT];

  bool mount_reported;
  int mount_status;
  int mount_errno;
  char mount_layer[24];

  unsigned launch_attempts;
  bool launch_failed;
  uint64_t launch_failed_mono_us;

  pid_t pid;
  uint32_t app_id;
  uint64_t process_start_mono_us;
  bool exit_pending;
  uint64_t exit_mono_us;
  int64_t exit_ms;

  sm_diag_finding_t findings[SM_DIAG_MAX_FINDINGS];
  size_t finding_count;
  sm_diag_kline_t kernel_lines[SM_DIAG_MAX_KERNEL_LINES];
  size_t kernel_line_count;
  sm_diag_layer_t layers[SM_DIAG_MAX_LAYERS];
  size_t layer_count;
  bool layers_captured;
  char fakelib_source[SM_DIAG_PATH_SIZE];
  char fakelib_files[SM_DIAG_MAX_FAKELIB_FILES][SM_DIAG_FILE_NAME_SIZE];
  size_t fakelib_file_count;
  sm_diag_native_error_t native_errors[SM_DIAG_MAX_NATIVE_ERRORS];
  size_t native_error_count;

  int kstuff_loaded;   // 1, 0, or -1 when not measured
  int hooks_installed; // 1, 0, or -1 when not measured
  uint32_t console_fw_raw;
  bool has_required_fw;
  uint32_t required_fw_bcd; // BCD major.minor, e.g. 0x1360
  char required_fw_raw[24];

  sm_diag_outcome_t outcome;
  bool has_diagnosis;
  sm_diag_diagnosis_t diagnosis;
  sm_diag_event_t timeline[SM_DIAG_MAX_TIMELINE];
  size_t timeline_count;
  bool interrupted; // Restored from disk while it was still being observed.

  // Runtime-only observation state (not persisted).
  bool restored; // Loaded from launches.json; never re-evaluated.
  bool klog_final_scan_done;
  uint64_t error_window_end_mono_us;
  uint64_t klog_scan_due_mono_us;
  char klog_anchor[SM_DIAG_KLOG_ANCHOR_SIZE];
  size_t klog_anchor_len;
  bool klog_anchor_valid;
} sm_diag_record_t;

typedef struct {
  uint32_t running_confirm_seconds;
  uint32_t spawn_timeout_seconds;
  uint32_t launch_retry_grace_seconds;
  uint32_t launch_retry_window_seconds;
} sm_diag_policy_t;

#define SM_DIAG_DEFAULT_RUNNING_CONFIRM_SECONDS 20u
#define SM_DIAG_DEFAULT_ERROR_WINDOW_SECONDS 120u
#define SM_DIAG_SPAWN_TIMEOUT_SECONDS 30u
#define SM_DIAG_LAUNCH_RETRY_GRACE_SECONDS 10u
#define SM_DIAG_LAUNCH_RETRY_WINDOW_SECONDS 60u

typedef enum {
  SM_DIAG_EV_LAUNCH = 1,
  SM_DIAG_EV_LAUNCH_FAILED,
  SM_DIAG_EV_SANDBOX_READY,
  SM_DIAG_EV_PROCESS_STARTED,
  SM_DIAG_EV_PROCESS_EXIT,
  SM_DIAG_EV_PROCESS_HANDOFF,
  SM_DIAG_EV_PROCESS_FINAL,
  SM_DIAG_EV_FINDING,
} sm_diag_event_type_t;

typedef struct {
  sm_diag_event_type_t type;
  char title_id[SM_DIAG_TITLE_ID_SIZE];
  int status;        // ShellCore response status (0 or errno)
  int native_errno;  // Native errno of the failed mount layer
  char layer[24];    // Failed mount layer
  pid_t pid;
  pid_t new_pid;
  uint32_t app_id;
  uint64_t event_mono_us; // Original event time (exec/exit); 0 = mono_us
  int64_t wall_ms;        // Wall clock when the event was queued
  uint64_t mono_us;       // Monotonic clock when the event was queued
  int finding_kind;
  char text[SM_DIAG_TEXT_SIZE];
} sm_diag_input_t;

typedef struct {
  sm_diag_record_t *records; // SM_DIAG_MAX_RECORDS ring entries
  size_t start;
  size_t count;
  uint64_t next_id;
} sm_diag_store_t;

// --- Names ---
const char *sm_diag_state_name(sm_diag_state_t state);
const char *sm_diag_stage_name(sm_diag_stage_id_t stage);
const char *sm_diag_outcome_name(sm_diag_outcome_t outcome);
bool sm_diag_outcome_from_name(const char *name, sm_diag_outcome_t *out);
sm_diag_policy_t sm_diag_default_policy(void);

// --- Record state machine ---
void sm_diag_record_init(sm_diag_record_t *rec, uint64_t id,
                         const char *title_id, int64_t now_ms,
                         uint64_t now_mono_us);
void sm_diag_record_add_event(sm_diag_record_t *rec, int64_t at_ms,
                              uint64_t mono_us, const char *event,
                              const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));
void sm_diag_record_set_stage(sm_diag_record_t *rec, sm_diag_stage_id_t stage,
                              sm_diag_state_t state, int64_t at_ms,
                              uint64_t mono_us, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));
// Re-derive running confirmation, spawn failure and the outcome for now.
// Returns true when anything visible changed.
bool sm_diag_record_evaluate(sm_diag_record_t *rec,
                             const sm_diag_policy_t *policy, int64_t now_ms,
                             uint64_t now_mono_us);
// True while the launch can still change without new console evidence
// (pending stages or a live process).
bool sm_diag_record_is_active(const sm_diag_record_t *rec);
bool sm_diag_outcome_is_failure(sm_diag_outcome_t outcome);

// --- Store ---
bool sm_diag_store_init(sm_diag_store_t *store);
void sm_diag_store_free(sm_diag_store_t *store);
size_t sm_diag_store_count(const sm_diag_store_t *store);
// Index 0 is the oldest record.
sm_diag_record_t *sm_diag_store_at(sm_diag_store_t *store, size_t index);
sm_diag_record_t *sm_diag_store_find_id(sm_diag_store_t *store, uint64_t id);
sm_diag_record_t *sm_diag_store_latest_for_title(sm_diag_store_t *store,
                                                 const char *title_id);
// Append a record (evicting the oldest when full) and return it.
sm_diag_record_t *sm_diag_store_append(sm_diag_store_t *store,
                                       const char *title_id, int64_t now_ms,
                                       uint64_t now_mono_us);
// Apply one observed event. Returns the record it changed, or NULL when the
// event does not belong to any launch (for example an exit of an unknown PID).
sm_diag_record_t *sm_diag_store_apply(sm_diag_store_t *store,
                                      const sm_diag_policy_t *policy,
                                      const sm_diag_input_t *input);

// --- Classification ---
// Fill diagnosis for a failed launch. Returns false for running/successful
// or still-pending launches.
bool sm_diag_classify(const sm_diag_record_t *rec, sm_diag_diagnosis_t *out);
// Category of a native internal error code, or NULL when it is not one of the
// documented codes.
const char *sm_diag_native_code_category(uint32_t code);

// --- Native error history (/system_data/priv/error/history/*.json) ---
void sm_diag_format_code_hex(int32_t code, char out[11]);
// Tolerant parser: BOM, missing fields and garbage are accepted; returns
// false only when the text is not a JSON object.
// One file of the console error history directory.
typedef struct {
  char name[SM_DIAG_FILE_NAME_SIZE];
  int64_t mtime;
  int64_t size;
} sm_diag_history_entry_t;

// qsort comparator: newest mtime first, then name descending.
int sm_diag_history_compare(const void *a, const void *b);
// Adds candidate to entries (at most capacity, any order) so that the list
// always holds the newest entries seen, whatever the readdir order.
void sm_diag_history_keep_newest(sm_diag_history_entry_t *entries,
                                 size_t *count, size_t capacity,
                                 const sm_diag_history_entry_t *candidate);

bool sm_diag_parse_native_error(const char *data, size_t size,
                                sm_diag_native_error_t *out);
struct json_object *sm_diag_native_error_to_json(
    const sm_diag_native_error_t *error);

// --- param.json firmware requirement ---
// Parse requiredSystemSoftwareVersion. The value is a 64-bit hex number whose
// top 16 bits are the BCD firmware major.minor (0x1360000000000000 = 13.60).
bool sm_diag_parse_required_fw(const char *data, size_t size,
                               uint32_t *bcd_out, char raw_out[24]);
// Compare a BCD major.minor requirement with kernel_get_fw_version() output,
// whose bits 31..16 use the same BCD encoding. Returns <0, 0 or >0 like strcmp
// (required relative to console).
int sm_diag_compare_fw(uint32_t required_bcd, uint32_t console_raw);
void sm_diag_format_fw_bcd(uint32_t bcd, char out[8]);

// --- Kernel log ---
// Offset of the first byte after anchor, or 0 when the anchor is no longer in
// the log (the ring has wrapped, so all current text is newer) or empty.
size_t sm_diag_log_offset_after_anchor(const char *text, size_t size,
                                       const char *anchor, size_t anchor_size);
// Select launch-relevant lines. pid <= 0 means no process was observed.
size_t sm_diag_scan_kernel_log(const char *text, size_t size,
                               const char *title_id, pid_t pid,
                               sm_diag_kline_t *out, size_t max);
// Extract the module named by an rtld line: "(path=/a/b/libc.prx)" or
// "(libSceAmpr.sprx)" -> basename.
bool sm_diag_rtld_module(const char *line, char out[SM_DIAG_FILE_NAME_SIZE]);

// --- Sandbox mounts ---
// Label a mount point inside /mnt/sandbox/<title_id>_NNN, or return false.
bool sm_diag_sandbox_layer_label(const char *title_id, const char *mount_on,
                                 char label[16]);

// --- PS5 Payload Manager (PLDMGR) ---
typedef struct {
  char entry[256];          // autoload.txt entry naming kstuff
  char slot_key[160];       // "repo/<id>@<hash>" or "local/<key>", or ""
  char implementation[16];  // "kstuff-lite", "kstuff" or "unknown"
  char version[64];         // meta.json "version", or ""
  char filename[128];
  char name[128];
} sm_diag_pldmgr_kstuff_t;
// Find the first payload entry of autoload.txt that names kstuff.
bool sm_diag_pldmgr_find_kstuff(const char *text, size_t size,
                                sm_diag_pldmgr_kstuff_t *out);
// Validate a slot key before it becomes a path component.
bool sm_diag_pldmgr_slot_key_valid(const char *key);
// Read version/name/filename/id from a slot meta.json into out.
bool sm_diag_pldmgr_parse_meta(const char *data, size_t size,
                               sm_diag_pldmgr_kstuff_t *out);
const char *sm_diag_kstuff_implementation(const char *text);

// --- AMPR runtime ---
// Find the "ampr-emu-ng <version>" identification string embedded in an AMPR
// runtime binary.
// Filesystem availability for /api/v1/system. registered: 1/0 from the
// kernel's vfs.conflist, -1 when that list is unavailable (it does not exist
// on the PS5). mounted: mounts of that type present now. seen_ok: UniMount
// mounted one successfully earlier in this process. Returns 1/0/-1 (never 0
// without the kernel list saying so) and a short factual reason.
int sm_diag_fs_availability(const char *name, int registered, size_t mounted,
                            bool seen_ok, char *reason, size_t reason_size);

bool sm_diag_ampr_version(const unsigned char *data, size_t size,
                          char out[32]);

// --- JSON ---
void sm_diag_format_time(int64_t at_ms, char out[32]);
struct json_object *sm_diag_record_to_json(const sm_diag_record_t *rec);
struct json_object *sm_diag_record_summary_json(const sm_diag_record_t *rec);
// Restore a persisted record. Records that were still active are marked
// interrupted, since this run cannot observe what happened to them.
bool sm_diag_record_from_json(struct json_object *object,
                              sm_diag_record_t *rec);

#endif
