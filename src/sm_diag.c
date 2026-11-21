// Launch diagnostics runtime: event queue, observer thread, console probes and
// persistence. The launch-path hooks (sm_diag_note_*) only enqueue; all file
// and kernel-log reads happen on the diagnostics thread.

#include "sm_platform.h"

#include <pthread.h>
#include <sys/mount.h>

#include <json-c/json.h>

#include "sm_appdb.h"
#include "sm_config_mount.h"
#include "sm_diag.h"
#include "sm_diag_core.h"
#include "sm_filesystem.h"
#include "sm_gameinfo.h"
#include "sm_kstuff.h"
#include "sm_log.h"
#include "sm_mdbg.h"
#include "sm_paths.h"
#include "sm_shellcore_hooks.h"
#include "sm_time.h"
#include "sm_types.h"

#define DIAG_QUEUE_SIZE 64u
#define DIAG_LAUNCHES_FILE LOG_DIR "/launches.json"
#define DIAG_LAUNCHES_MAX_BYTES (4u * 1024u * 1024u)
#define DIAG_ERROR_HISTORY_DIR "/system_data/priv/error/history"
#define DIAG_ERROR_FILE_MAX_BYTES (64u * 1024u)
#define DIAG_ERROR_HISTORY_MAX_FILES 512u
#define DIAG_ERROR_POLL_US 2000000u
#define DIAG_PERSIST_DELAY_US 2000000u
#define DIAG_ACTIVE_TICK_US 1000000u
#define DIAG_IDLE_TICK_US 30000000u
#define DIAG_KLOG_SCAN_DELAY_US 1500000u
#define DIAG_KLOG_TAIL_BYTES (1024u * 1024u)
#define DIAG_LOAD_WAIT_US 1000000u
#define DIAG_DEFAULT_LIST_LIMIT 20u
#define DIAG_MAX_ERROR_LIST 200u

typedef sm_diag_history_entry_t history_entry_t;

typedef struct {
  int kstuff_loaded;
  int hooks_installed;
  uint32_t console_fw_raw;
  int registered; // 1, 0, -1
  bool has_required_fw;
  uint32_t required_fw_bcd;
  char required_fw_raw[24];
  char anchor[SM_DIAG_KLOG_ANCHOR_SIZE];
  size_t anchor_len;
  bool anchor_valid;
} launch_environment_t;

static struct {
  pthread_once_t once;
  pthread_mutex_t queue_mutex;
  pthread_cond_t queue_cond;
  sm_diag_input_t queue[DIAG_QUEUE_SIZE];
  size_t queue_head;
  size_t queue_count;
  uint64_t dropped;
  bool thread_running;
  bool stop;  // sm_diag_stop(): no new events, thread exits after a flush
  pthread_t thread;

  pthread_mutex_t store_mutex;
  pthread_cond_t loaded_cond;
  bool loaded;
  bool store_ready;
  sm_diag_store_t store;
  bool dirty;
  uint64_t dirty_since_us;

  history_entry_t *baseline;
  size_t baseline_count;
  uint64_t next_error_poll_us;
} g_diag = {
    .once = PTHREAD_ONCE_INIT,
    .queue_mutex = PTHREAD_MUTEX_INITIALIZER,
    .queue_cond = PTHREAD_COND_INITIALIZER,
    .store_mutex = PTHREAD_MUTEX_INITIALIZER,
    .loaded_cond = PTHREAD_COND_INITIALIZER,
};

static int64_t wall_time_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
    return 0;
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *diag_thread_main(void *arg);

// Is the recorded PID still the same application? Mirrors the lifecycle
// watcher: kill(pid, 0) for existence (EPERM still means alive) and
// sceKernelGetAppInfo() for the title and app id, so a recycled PID that now
// runs something else does not match.
static sm_diag_process_state_t probe_process(pid_t pid, const char *title_id,
                                             uint32_t app_id) {
  if (pid <= 0)
    return SM_DIAG_PROC_GONE;
  if (kill(pid, 0) != 0 && errno == ESRCH)
    return SM_DIAG_PROC_GONE;
  app_info_t info;
  memset(&info, 0, sizeof(info));
  // The PID exists but its application identity cannot be read: neither
  // proof of the same game nor of a different one.
  if (sceKernelGetAppInfo(pid, &info) != 0)
    return SM_DIAG_PROC_UNKNOWN;
  size_t length = strnlen(info.title_id, sizeof(info.title_id));
  if (length == 0 || length >= sizeof(info.title_id) ||
      strncmp(info.title_id, title_id, sizeof(info.title_id)) != 0 ||
      title_id[length] != '\0')
    return SM_DIAG_PROC_OTHER;
  if (app_id == 0 || info.app_id == 0)
    return SM_DIAG_PROC_UNVERIFIED;
  return info.app_id == app_id ? SM_DIAG_PROC_SAME : SM_DIAG_PROC_OTHER;
}

// Without the thread the store never loads: report it unavailable at once
// instead of letting every reader wait DIAG_LOAD_WAIT_US.
static void mark_store_unavailable(void) {
  pthread_mutex_lock(&g_diag.store_mutex);
  g_diag.store_ready = false;
  g_diag.loaded = true;
  pthread_cond_broadcast(&g_diag.loaded_cond);
  pthread_mutex_unlock(&g_diag.store_mutex);
}

static void start_thread_once(void) {
  pthread_mutex_lock(&g_diag.queue_mutex);
  bool stopping = g_diag.stop;
  int rc = stopping ? ECANCELED
                    : pthread_create(&g_diag.thread, NULL, diag_thread_main, NULL);
  g_diag.thread_running = rc == 0;
  pthread_mutex_unlock(&g_diag.queue_mutex);
  if (rc != 0) {
    if (!stopping)
      log_debug("  [DIAG] observer thread unavailable: %s", strerror(rc));
    mark_store_unavailable();
  }
}

static void ensure_started(void) { (void)pthread_once(&g_diag.once, start_thread_once); }

// --- Hooks (any thread, launch path) ---

static void enqueue(const sm_diag_input_t *input) {
  ensure_started();
  pthread_mutex_lock(&g_diag.queue_mutex);
  if (g_diag.stop || !g_diag.thread_running) {
    // Shut down (or never started): nothing would consume the event.
  } else if (g_diag.queue_count >= DIAG_QUEUE_SIZE) {
    g_diag.dropped++;
  } else {
    size_t slot = (g_diag.queue_head + g_diag.queue_count) % DIAG_QUEUE_SIZE;
    g_diag.queue[slot] = *input;
    g_diag.queue_count++;
    pthread_cond_signal(&g_diag.queue_cond);
  }
  pthread_mutex_unlock(&g_diag.queue_mutex);
}

static void init_input(sm_diag_input_t *input, sm_diag_event_type_t type,
                       const char *title_id) {
  memset(input, 0, sizeof(*input));
  input->type = type;
  if (title_id)
    (void)strlcpy(input->title_id, title_id, sizeof(input->title_id));
  input->wall_ms = wall_time_ms();
  input->mono_us = monotonic_time_us();
}

void sm_diag_note_launch(const char *title_id, int status, int native_errno,
                         const char *layer) {
  if (!is_supported_game_title_id(title_id))
    return;
  sm_diag_input_t input;
  init_input(&input, SM_DIAG_EV_LAUNCH, title_id);
  input.status = status;
  input.native_errno = native_errno;
  if (layer)
    (void)strlcpy(input.layer, layer, sizeof(input.layer));
  enqueue(&input);
}

void sm_diag_note_launch_failed(const char *title_id, int status) {
  if (!is_supported_game_title_id(title_id))
    return;
  sm_diag_input_t input;
  init_input(&input, SM_DIAG_EV_LAUNCH_FAILED, title_id);
  input.status = status;
  enqueue(&input);
}

void sm_diag_note_sandbox_ready(const char *title_id, int status) {
  if (!is_supported_game_title_id(title_id))
    return;
  sm_diag_input_t input;
  init_input(&input, SM_DIAG_EV_SANDBOX_READY, title_id);
  input.status = status;
  enqueue(&input);
}

void sm_diag_note_process_started(const char *title_id, pid_t pid,
                                  uint32_t app_id, uint64_t exec_time_us) {
  if (!is_supported_game_title_id(title_id) || pid <= 0)
    return;
  sm_diag_input_t input;
  init_input(&input, SM_DIAG_EV_PROCESS_STARTED, title_id);
  input.pid = pid;
  input.app_id = app_id;
  input.event_mono_us = exec_time_us;
  enqueue(&input);
}

void sm_diag_note_process_exit(pid_t pid) {
  if (pid <= 0)
    return;
  sm_diag_input_t input;
  init_input(&input, SM_DIAG_EV_PROCESS_EXIT, NULL);
  input.pid = pid;
  enqueue(&input);
}

void sm_diag_note_process_handoff(pid_t old_pid, pid_t new_pid) {
  if (old_pid <= 0 || new_pid <= 0)
    return;
  sm_diag_input_t input;
  init_input(&input, SM_DIAG_EV_PROCESS_HANDOFF, NULL);
  input.pid = old_pid;
  input.new_pid = new_pid;
  enqueue(&input);
}

void sm_diag_note_process_finalized(pid_t pid, uint64_t exit_time_us) {
  if (pid <= 0)
    return;
  sm_diag_input_t input;
  init_input(&input, SM_DIAG_EV_PROCESS_FINAL, NULL);
  input.pid = pid;
  input.event_mono_us = exit_time_us;
  enqueue(&input);
}

void sm_diag_note_game_error(pid_t pid, const char *title_id, int kind,
                             const char *text) {
  if (pid <= 0)
    return;
  sm_diag_input_t input;
  init_input(&input, SM_DIAG_EV_FINDING, title_id);
  input.pid = pid;
  input.finding_kind = kind;
  if (text)
    (void)strlcpy(input.text, text, sizeof(input.text));
  enqueue(&input);
}

// --- Files ---

static char *read_small_file(const char *path, size_t max_bytes,
                             size_t *size_out) {
  *size_out = 0;
  int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
  if (fd < 0)
    return NULL;
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
      (uint64_t)st.st_size > max_bytes) {
    close(fd);
    errno = EFBIG;
    return NULL;
  }
  size_t size = (size_t)st.st_size;
  char *data = malloc(size + 1u);
  if (!data) {
    close(fd);
    return NULL;
  }
  size_t done = 0;
  while (done < size) {
    ssize_t count = read(fd, data + done, size - done);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      break;
    done += (size_t)count;
  }
  close(fd);
  data[done] = '\0';
  *size_out = done;
  return data;
}

static bool write_launches_file(const char *text, size_t size) {
  // Unique temporary name created exclusively (never through a planted
  // symlink), then renamed over the target, like owner.json.
  char temporary[] = DIAG_LAUNCHES_FILE ".XXXXXX";
  int fd = mkstemp(temporary);
  if (fd < 0)
    return false;
  (void)fchmod(fd, 0644);
  size_t done = 0;
  bool ok = true;
  while (done < size) {
    ssize_t count = write(fd, text + done, size - done);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      ok = false;
      break;
    }
    done += (size_t)count;
  }
  if (ok && fsync(fd) != 0)
    ok = false;
  if (close(fd) != 0)
    ok = false;
  if (ok && rename(temporary, DIAG_LAUNCHES_FILE) != 0)
    ok = false;
  if (!ok) {
    int saved_errno = errno;
    (void)unlink(temporary);
    errno = saved_errno;
  }
  return ok;
}

static void mark_dirty_locked(uint64_t now_us);

static void load_persisted_locked(void) {
  size_t size = 0;
  char *text = read_small_file(DIAG_LAUNCHES_FILE, DIAG_LAUNCHES_MAX_BYTES, &size);
  if (!text)
    return;
  struct json_object *root = json_tokener_parse(text);
  free(text);
  if (!root || !json_object_is_type(root, json_type_object)) {
    log_debug("  [DIAG] ignoring unreadable %s", DIAG_LAUNCHES_FILE);
    if (root)
      json_object_put(root);
    return;
  }
  struct json_object *records = NULL;
  struct json_object *next_id = NULL;
  if (json_object_object_get_ex(root, "next_id", &next_id) &&
      json_object_is_type(next_id, json_type_int) &&
      json_object_get_int64(next_id) > 0)
    g_diag.store.next_id = (uint64_t)json_object_get_int64(next_id);
  size_t loaded = 0;
  if (json_object_object_get_ex(root, "records", &records) &&
      json_object_is_type(records, json_type_array)) {
    size_t count = json_object_array_length(records);
    size_t first = count > SM_DIAG_MAX_RECORDS ? count - SM_DIAG_MAX_RECORDS : 0;
    for (size_t i = first; i < count; ++i) {
      sm_diag_record_t record;
      if (!sm_diag_record_from_json(json_object_array_get_idx(records, i), &record))
        continue;
      sm_diag_record_t *slot =
          sm_diag_store_append(&g_diag.store, record.title_id, 0, 0);
      g_diag.store.next_id--; // append consumed an id; keep the stored one.
      *slot = record;
      if (sm_diag_record_unfinished(slot)) {
        // Never leave a launch "in progress" across a restart: re-attach to
        // the same live application, or finalise it as interrupted.
        sm_diag_process_state_t state =
            probe_process(slot->pid, slot->title_id, slot->app_id);
        uint64_t now_us = monotonic_time_us();
        bool resumed = sm_diag_record_resolve_restored(slot, state, wall_time_ms(),
                                                       now_us);
        log_debug("  [DIAG] launch %" PRIu64 " (%s pid=%ld) %s after restart",
                  slot->id, slot->title_id, (long)slot->pid,
                  resumed ? "re-attached" : "finalised as observation_interrupted");
        mark_dirty_locked(now_us);
      }
      if (record.id >= g_diag.store.next_id)
        g_diag.store.next_id = record.id + 1u;
      loaded++;
    }
  }
  json_object_put(root);
  log_debug("  [DIAG] restored %zu launch record(s)", loaded);
}

static void persist_if_due(uint64_t now_us, bool force) {
  pthread_mutex_lock(&g_diag.store_mutex);
  if (!g_diag.dirty ||
      (!force && now_us - g_diag.dirty_since_us < DIAG_PERSIST_DELAY_US)) {
    pthread_mutex_unlock(&g_diag.store_mutex);
    return;
  }
  struct json_object *root = json_object_new_object();
  struct json_object *records = json_object_new_array();
  bool ok = root && records;
  for (size_t i = 0; ok && i < sm_diag_store_count(&g_diag.store); ++i) {
    struct json_object *record =
        sm_diag_record_to_json(sm_diag_store_at(&g_diag.store, i));
    ok = record && json_object_array_add(records, record) == 0;
  }
  if (ok) {
    json_object_object_add(root, "version", json_object_new_int(1));
    json_object_object_add(root, "next_id",
                           json_object_new_int64((int64_t)g_diag.store.next_id));
    json_object_object_add(root, "records", records);
    records = NULL;
  }
  g_diag.dirty = false;
  pthread_mutex_unlock(&g_diag.store_mutex);

  if (records)
    json_object_put(records);
  if (!ok) {
    if (root)
      json_object_put(root);
    return;
  }
  size_t size = 0;
  const char *text =
      json_object_to_json_string_length(root, JSON_C_TO_STRING_PLAIN, &size);
  if (!text || !write_launches_file(text, size))
    log_debug("  [DIAG] failed to save %s: %s", DIAG_LAUNCHES_FILE,
              strerror(errno));
  json_object_put(root);
}

static void mark_dirty_locked(uint64_t now_us) {
  if (!g_diag.dirty)
    g_diag.dirty_since_us = now_us;
  g_diag.dirty = true;
}

// --- Probes (diagnostics thread) ---

static void capture_klog_anchor(launch_environment_t *env) {
  char *text = NULL;
  size_t length = 0, total = 0;
  if (sm_mdbg_get_log_tail(SM_DIAG_KLOG_ANCHOR_SIZE, &text, &length, &total) != 0) {
    env->anchor_valid = false;
    return;
  }
  // An empty log is still a valid starting point: everything later is new.
  memcpy(env->anchor, text, length);
  env->anchor_len = length;
  env->anchor_valid = true;
  free(text);
}

static void read_required_firmware(const char *title_id,
                                   launch_environment_t *env) {
  // Read a managed title's param.json from its source, never through the
  // live /system_ex/app/<TID> mount: an open file there during the launch
  // window can make a non-forced unmount fail with EBUSY.
  char source[MAX_PATH];
  char path[MAX_PATH];
  if (read_mount_link(title_id, source, sizeof(source)))
    snprintf(path, sizeof(path), "%s/sce_sys/param.json", source);
  else
    snprintf(path, sizeof(path), "/system_ex/app/%s/sce_sys/param.json",
             title_id);
  size_t size = 0;
  char *text = read_small_file(path, 1024u * 1024u, &size);
  if (!text)
    return;
  env->has_required_fw = sm_diag_parse_required_fw(
      text, size, &env->required_fw_bcd, env->required_fw_raw);
  free(text);
}

static void capture_environment(const char *title_id, bool read_param,
                                launch_environment_t *env) {
  memset(env, 0, sizeof(*env));
  env->kstuff_loaded = sm_kstuff_is_loaded() ? 1 : 0;
  sm_shellcore_hooks_info_t hooks;
  env->hooks_installed =
      sm_shellcore_hooks_get_info(&hooks) ? (hooks.installed ? 1 : 0) : -1;
  env->console_fw_raw = kernel_get_fw_version();
  struct AppDbTitleList titles = {0};
  if (get_app_db_title_list_cached(&titles)) {
    env->registered = app_db_title_list_contains(&titles, title_id) ? 1 : 0;
    free_app_db_title_list(&titles);
  } else {
    env->registered = -1;
  }
  if (read_param)
    read_required_firmware(title_id, env);
}

static void apply_environment(sm_diag_record_t *rec,
                              const launch_environment_t *env, bool anchor,
                              int64_t now_ms, uint64_t now_us) {
  rec->kstuff_loaded = env->kstuff_loaded;
  rec->hooks_installed = env->hooks_installed;
  rec->console_fw_raw = env->console_fw_raw;
  if (env->has_required_fw) {
    rec->has_required_fw = true;
    rec->required_fw_bcd = env->required_fw_bcd;
    (void)strlcpy(rec->required_fw_raw, env->required_fw_raw,
                  sizeof(rec->required_fw_raw));
  }
  if (env->registered == 1) {
    sm_diag_record_set_stage(rec, SM_DIAG_STAGE_REGISTERED, SM_DIAG_OK, now_ms,
                             now_us, "title present in app.db at launch time");
  } else if (env->registered == 0) {
    sm_diag_record_set_stage(rec, SM_DIAG_STAGE_REGISTERED, SM_DIAG_FAILED,
                             now_ms, now_us,
                             "title not present in app.db at launch time");
  } else if (rec->stages[SM_DIAG_STAGE_REGISTERED].state == SM_DIAG_NOT_OBSERVED) {
    sm_diag_record_set_stage(rec, SM_DIAG_STAGE_REGISTERED, SM_DIAG_NOT_OBSERVED,
                             0, 0, "app.db could not be read");
  }
  if (anchor && !rec->klog_anchor_valid && env->anchor_valid) {
    memcpy(rec->klog_anchor, env->anchor, env->anchor_len);
    rec->klog_anchor_len = env->anchor_len;
    rec->klog_anchor_valid = true;
  }
}

static void list_fakelib_files(const char *source, sm_diag_record_t *tmp) {
  DIR *dir = opendir(source);
  if (!dir)
    return;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL &&
         tmp->fakelib_file_count < SM_DIAG_MAX_FAKELIB_FILES) {
    if (entry->d_name[0] == '.' || strlen(entry->d_name) >= SM_DIAG_FILE_NAME_SIZE)
      continue;
    char path[MAX_PATH];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s", source, entry->d_name);
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
      continue;
    (void)strlcpy(tmp->fakelib_files[tmp->fakelib_file_count++], entry->d_name,
                  SM_DIAG_FILE_NAME_SIZE);
  }
  closedir(dir);
}

// Observe the mounts that exist inside this title's sandbox right now.
static void capture_layers(const char *title_id, sm_diag_record_t *tmp) {
  struct statfs *mounts = NULL;
  int count = sm_mount_table_snapshot(&mounts);
  if (count < 0)
    return;
  tmp->layers_captured = true;
  for (int i = 0; i < count; ++i) {
    char label[16];
    if (!sm_diag_sandbox_layer_label(title_id, mounts[i].f_mntonname, label))
      continue;
    const char *from = mounts[i].f_mntfromname;
    // Source: sm_fakelib.c sandbox_mount_matches(): unionfs reports
    // "<above>:<source>".
    if (strncmp(from, "<above>:", 8) == 0)
      from += 8;
    if (tmp->layer_count < SM_DIAG_MAX_LAYERS) {
      sm_diag_layer_t *layer = &tmp->layers[tmp->layer_count++];
      (void)strlcpy(layer->label, label, sizeof(layer->label));
      (void)strlcpy(layer->fstype, mounts[i].f_fstypename, sizeof(layer->fstype));
      (void)strlcpy(layer->from, from, sizeof(layer->from));
      (void)strlcpy(layer->on, mounts[i].f_mntonname, sizeof(layer->on));
    }
    if (strcmp(label, "fakelib") == 0 && !tmp->fakelib_source[0]) {
      (void)strlcpy(tmp->fakelib_source, from, sizeof(tmp->fakelib_source));
      list_fakelib_files(from, tmp);
    }
  }
  free(mounts);
}

static void copy_layers(sm_diag_record_t *rec, const sm_diag_record_t *tmp) {
  rec->layers_captured = tmp->layers_captured;
  rec->layer_count = tmp->layer_count;
  memcpy(rec->layers, tmp->layers, sizeof(rec->layers));
  memcpy(rec->fakelib_source, tmp->fakelib_source, sizeof(rec->fakelib_source));
  rec->fakelib_file_count = tmp->fakelib_file_count;
  memcpy(rec->fakelib_files, tmp->fakelib_files, sizeof(rec->fakelib_files));
}

// --- Native error history ---

static bool has_json_suffix(const char *name) {
  size_t length = strlen(name);
  return length > 5u && strcasecmp(name + length - 5u, ".json") == 0;
}

// Newest first, at most DIAG_ERROR_HISTORY_MAX_FILES (the newest ones).
// Returns -1 with errno when the directory cannot be read.
static ssize_t list_error_history(history_entry_t **out) {
  *out = NULL;
  DIR *dir = opendir(DIAG_ERROR_HISTORY_DIR);
  if (!dir)
    return -1;
  history_entry_t *entries =
      calloc(DIAG_ERROR_HISTORY_MAX_FILES, sizeof(*entries));
  if (!entries) {
    closedir(dir);
    errno = ENOMEM;
    return -1;
  }
  size_t count = 0;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] == '.' || !has_json_suffix(entry->d_name) ||
        strlen(entry->d_name) >= SM_DIAG_FILE_NAME_SIZE)
      continue;
    char path[MAX_PATH];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s", DIAG_ERROR_HISTORY_DIR, entry->d_name);
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
      continue;
    history_entry_t candidate;
    (void)strlcpy(candidate.name, entry->d_name, sizeof(candidate.name));
    candidate.mtime = (int64_t)st.st_mtime;
    candidate.size = (int64_t)st.st_size;
    sm_diag_history_keep_newest(entries, &count, DIAG_ERROR_HISTORY_MAX_FILES,
                                &candidate);
  }
  closedir(dir);
  qsort(entries, count, sizeof(*entries), sm_diag_history_compare);
  *out = entries;
  return (ssize_t)count;
}

static bool read_native_error(const history_entry_t *entry,
                              sm_diag_native_error_t *error) {
  memset(error, 0, sizeof(*error));
  (void)strlcpy(error->file, entry->name, sizeof(error->file));
  error->mtime = entry->mtime;
  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s/%s", DIAG_ERROR_HISTORY_DIR, entry->name);
  size_t size = 0;
  char *text = read_small_file(path, DIAG_ERROR_FILE_MAX_BYTES, &size);
  if (!text)
    return false;
  bool parsed = sm_diag_parse_native_error(text, size, error);
  free(text);
  return parsed;
}

static bool history_contains(const history_entry_t *list, size_t count,
                             const history_entry_t *entry) {
  for (size_t i = 0; i < count; ++i) {
    if (list[i].mtime == entry->mtime && list[i].size == entry->size &&
        strcmp(list[i].name, entry->name) == 0)
      return true;
  }
  return false;
}

static bool any_error_window_open_locked(uint64_t now_us) {
  for (size_t i = 0; i < sm_diag_store_count(&g_diag.store); ++i) {
    const sm_diag_record_t *rec = sm_diag_store_at(&g_diag.store, i);
    if (!rec->restored && rec->error_window_end_mono_us > now_us)
      return true;
  }
  return false;
}

// Called before a new error window opens. Files written at or after the launch
// second stay "new" so an error filed just before this snapshot still counts.
static void refresh_error_baseline(int64_t launch_ms) {
  history_entry_t *entries = NULL;
  ssize_t count = list_error_history(&entries);
  free(g_diag.baseline);
  g_diag.baseline = NULL;
  g_diag.baseline_count = 0;
  if (count < 0)
    return;
  size_t kept = 0;
  for (ssize_t i = 0; i < count; ++i) {
    if (entries[i].mtime >= launch_ms / 1000 - 1)
      continue;
    entries[kept++] = entries[i];
  }
  g_diag.baseline = entries;
  g_diag.baseline_count = kept;
}

static void attach_native_error_locked(const sm_diag_native_error_t *error,
                                       int64_t now_ms, uint64_t now_us) {
  if (!error->parsed)
    return;
  sm_diag_record_t *target = NULL;
  const char *attribution = NULL;
  bool system_title = strncmp(error->title_id, "NPXS", 4) == 0;
  for (size_t i = sm_diag_store_count(&g_diag.store); i > 0 && !target; --i) {
    sm_diag_record_t *rec = sm_diag_store_at(&g_diag.store, i - 1u);
    if (rec->restored || rec->error_window_end_mono_us <= now_us ||
        error->mtime < rec->created_ms / 1000 - 2)
      continue;
    if (strcmp(error->title_id, rec->title_id) == 0) {
      target = rec;
      attribution = "title_id";
    }
  }
  if (!target && system_title) {
    for (size_t i = sm_diag_store_count(&g_diag.store); i > 0; --i) {
      sm_diag_record_t *rec = sm_diag_store_at(&g_diag.store, i - 1u);
      if (rec->restored || rec->error_window_end_mono_us <= now_us ||
          error->mtime < rec->created_ms / 1000 - 2)
        continue;
      target = rec;
      attribution = "time_window";
      break;
    }
  }
  if (!target)
    return;
  for (size_t i = 0; i < target->native_error_count; ++i) {
    if (strcmp(target->native_errors[i].file, error->file) == 0 &&
        target->native_errors[i].mtime == error->mtime)
      return;
  }
  if (target->native_error_count >= SM_DIAG_MAX_NATIVE_ERRORS)
    return;
  sm_diag_native_error_t *slot = &target->native_errors[target->native_error_count++];
  *slot = *error;
  (void)strlcpy(slot->attribution, attribution, sizeof(slot->attribution));
  char hex[11] = "none";
  if (error->has_internal_code)
    sm_diag_format_code_hex(error->internal_code, hex);
  sm_diag_record_add_event(target, now_ms, now_us, "native_error",
                           "%s %s %s (%s, %s)", error->file, error->short_error,
                           hex, error->title_id, attribution);
  log_debug("  [DIAG] native error %s %s %s attached to launch %" PRIu64
            " (%s, %s)",
            error->file, error->short_error, hex, target->id, target->title_id,
            attribution);
  mark_dirty_locked(now_us);
}

static void poll_error_history(int64_t now_ms, uint64_t now_us) {
  history_entry_t *entries = NULL;
  ssize_t count = list_error_history(&entries);
  if (count < 0)
    return;
  for (ssize_t i = 0; i < count; ++i) {
    if (history_contains(g_diag.baseline, g_diag.baseline_count, &entries[i]))
      continue;
    sm_diag_native_error_t error;
    (void)read_native_error(&entries[i], &error);
    pthread_mutex_lock(&g_diag.store_mutex);
    attach_native_error_locked(&error, now_ms, now_us);
    pthread_mutex_unlock(&g_diag.store_mutex);
  }
  free(g_diag.baseline);
  g_diag.baseline = entries;
  g_diag.baseline_count = (size_t)count;
}

// --- Event processing ---

static sm_diag_policy_t current_policy(const runtime_config_t *cfg) {
  sm_diag_policy_t policy = sm_diag_default_policy();
  // Running is confirmed only by a positive PID probe (reconcile_liveness()).
  policy.require_alive_probe = true;
  if (cfg->diagnostics_running_confirm_seconds)
    policy.running_confirm_seconds = cfg->diagnostics_running_confirm_seconds;
  return policy;
}

static void schedule_klog_scan_locked(sm_diag_record_t *rec, uint64_t now_us) {
  if (!rec->klog_anchor_valid)
    return;
  uint64_t due = now_us + DIAG_KLOG_SCAN_DELAY_US;
  if (rec->klog_scan_due_mono_us == 0 || rec->klog_scan_due_mono_us > due)
    rec->klog_scan_due_mono_us = due;
}

static void process_event(const sm_diag_input_t *input,
                          const runtime_config_t *cfg) {
  const sm_diag_policy_t policy = current_policy(cfg);
  launch_environment_t env;
  bool have_env = false;
  sm_diag_record_t *layers = NULL;

  if (input->type == SM_DIAG_EV_LAUNCH) {
    // param.json only matters for a launch whose mount succeeded.
    capture_environment(input->title_id, input->status == 0, &env);
    capture_klog_anchor(&env);
    have_env = true;
    pthread_mutex_lock(&g_diag.store_mutex);
    bool window_open = any_error_window_open_locked(input->mono_us);
    pthread_mutex_unlock(&g_diag.store_mutex);
    if (!window_open)
      refresh_error_baseline(input->wall_ms);
  } else if (input->type == SM_DIAG_EV_SANDBOX_READY) {
    layers = calloc(1, sizeof(*layers));
    if (layers)
      capture_layers(input->title_id, layers);
  }

  pthread_mutex_lock(&g_diag.store_mutex);
  sm_diag_record_t *rec = sm_diag_store_apply(&g_diag.store, &policy, input);
  uint64_t record_id = rec ? rec->id : 0;
  bool needs_env = rec && !have_env && rec->kstuff_loaded < 0 &&
                   input->type == SM_DIAG_EV_PROCESS_STARTED;
  if (rec) {
    uint64_t now_us = monotonic_time_us();
    if (have_env)
      apply_environment(rec, &env, true, input->wall_ms, input->mono_us);
    if (input->type == SM_DIAG_EV_LAUNCH && rec->error_window_end_mono_us == 0)
      rec->error_window_end_mono_us =
          input->mono_us + (uint64_t)cfg->diagnostics_error_window_seconds * 1000000u;
    if (layers)
      copy_layers(rec, layers);
    if (input->type == SM_DIAG_EV_LAUNCH_FAILED ||
        input->type == SM_DIAG_EV_PROCESS_EXIT ||
        input->type == SM_DIAG_EV_PROCESS_FINAL ||
        input->type == SM_DIAG_EV_FINDING)
      schedule_klog_scan_locked(rec, now_us);
    (void)sm_diag_record_evaluate(rec, &policy, wall_time_ms(), now_us);
    mark_dirty_locked(now_us);
  }
  pthread_mutex_unlock(&g_diag.store_mutex);
  free(layers);

  if (needs_env) {
    // The launch was not seen through ShellCore (hooks missing or another
    // launcher). Record the environment when the process is observed.
    capture_environment(input->title_id, true, &env);
    pthread_mutex_lock(&g_diag.store_mutex);
    rec = sm_diag_store_find_id(&g_diag.store, record_id);
    if (rec) {
      apply_environment(rec, &env, false, input->wall_ms, input->mono_us);
      if (rec->error_window_end_mono_us == 0)
        rec->error_window_end_mono_us =
            input->mono_us + (uint64_t)cfg->diagnostics_error_window_seconds * 1000000u;
      (void)sm_diag_record_evaluate(rec, &policy, wall_time_ms(), monotonic_time_us());
    }
    pthread_mutex_unlock(&g_diag.store_mutex);
  }
}

typedef struct {
  uint64_t id;
  char title_id[SM_DIAG_TITLE_ID_SIZE];
  pid_t pid;
  char anchor[SM_DIAG_KLOG_ANCHOR_SIZE];
  size_t anchor_len;
} klog_job_t;

static void run_klog_scans(klog_job_t *jobs, size_t count,
                           const sm_diag_policy_t *policy) {
  char *text = NULL;
  size_t length = 0, total = 0;
  if (count == 0 ||
      sm_mdbg_get_log_tail(DIAG_KLOG_TAIL_BYTES, &text, &length, &total) != 0)
    return;
  for (size_t i = 0; i < count; ++i) {
    size_t offset = sm_diag_log_offset_after_anchor(text, length, jobs[i].anchor,
                                                    jobs[i].anchor_len);
    sm_diag_kline_t lines[SM_DIAG_MAX_KERNEL_LINES];
    size_t found = sm_diag_scan_kernel_log(text + offset, length - offset,
                                           jobs[i].title_id, jobs[i].pid, lines,
                                           SM_DIAG_MAX_KERNEL_LINES);
    pthread_mutex_lock(&g_diag.store_mutex);
    sm_diag_record_t *rec = sm_diag_store_find_id(&g_diag.store, jobs[i].id);
    if (rec) {
      // Keep earlier lines that rolled out of the log since the last scan.
      bool added = false;
      for (size_t j = 0; j < found; ++j) {
        bool known = false;
        for (size_t k = 0; k < rec->kernel_line_count && !known; ++k)
          known = strcmp(rec->kernel_lines[k].text, lines[j].text) == 0;
        if (!known && rec->kernel_line_count < SM_DIAG_MAX_KERNEL_LINES) {
          rec->kernel_lines[rec->kernel_line_count++] = lines[j];
          added = true;
        }
      }
      uint64_t now_us = monotonic_time_us();
      if (sm_diag_record_evaluate(rec, policy, wall_time_ms(), now_us) || added)
        mark_dirty_locked(now_us);
    }
    pthread_mutex_unlock(&g_diag.store_mutex);
  }
  free(text);
}

typedef struct {
  uint64_t id;
  pid_t pid;
  uint32_t app_id;
  char title_id[SM_DIAG_TITLE_ID_SIZE];
} liveness_job_t;

// Bounded reconciliation: probe recorded game PIDs (about once a second until
// running is confirmed, every 5 s after) so a missed exit cannot leave a
// launch pending forever. Diagnostics thread only; the probes are two cheap
// syscalls per PID and take no launch-path lock.
static bool reconcile_liveness(const sm_diag_policy_t *policy) {
  liveness_job_t jobs[8];
  size_t count = 0;
  uint64_t now_us = monotonic_time_us();
  pthread_mutex_lock(&g_diag.store_mutex);
  for (size_t i = 0; i < sm_diag_store_count(&g_diag.store) && count < 8u; ++i) {
    sm_diag_record_t *rec = sm_diag_store_at(&g_diag.store, i);
    if (!sm_diag_record_liveness_due(rec, now_us))
      continue;
    jobs[count].id = rec->id;
    jobs[count].pid = rec->pid;
    jobs[count].app_id = rec->app_id;
    (void)strlcpy(jobs[count].title_id, rec->title_id, sizeof(jobs[count].title_id));
    count++;
  }
  pthread_mutex_unlock(&g_diag.store_mutex);
  for (size_t i = 0; i < count; ++i) {
    sm_diag_process_state_t state =
        probe_process(jobs[i].pid, jobs[i].title_id, jobs[i].app_id);
    pthread_mutex_lock(&g_diag.store_mutex);
    sm_diag_record_t *rec = sm_diag_store_find_id(&g_diag.store, jobs[i].id);
    uint64_t probe_us = monotonic_time_us();
    if (rec && rec->pid == jobs[i].pid) {
      sm_diag_outcome_t before = rec->outcome;
      if (sm_diag_record_note_liveness(rec, policy, state, wall_time_ms(), probe_us)) {
        mark_dirty_locked(probe_us);
        if (rec->outcome != before)
          schedule_klog_scan_locked(rec, probe_us);
      }
    }
    pthread_mutex_unlock(&g_diag.store_mutex);
  }
  return count > 0;
}

// At shutdown: launches whose process is already gone cannot be observed by
// the next run either, so finalise them now. A game still running is saved in
// progress and resolved by the next start (re-attach or interrupt).
static void finalise_for_shutdown(const sm_diag_policy_t *policy) {
  liveness_job_t jobs[SM_DIAG_MAX_RECORDS];
  size_t count = 0;
  pthread_mutex_lock(&g_diag.store_mutex);
  for (size_t i = 0; i < sm_diag_store_count(&g_diag.store); ++i) {
    sm_diag_record_t *rec = sm_diag_store_at(&g_diag.store, i);
    if (rec->restored || rec->pid <= 0 || !sm_diag_record_unfinished(rec))
      continue;
    jobs[count].id = rec->id;
    jobs[count].pid = rec->pid;
    jobs[count].app_id = rec->app_id;
    (void)strlcpy(jobs[count].title_id, rec->title_id, sizeof(jobs[count].title_id));
    count++;
  }
  pthread_mutex_unlock(&g_diag.store_mutex);
  for (size_t i = 0; i < count; ++i) {
    sm_diag_process_state_t state =
        probe_process(jobs[i].pid, jobs[i].title_id, jobs[i].app_id);
    pthread_mutex_lock(&g_diag.store_mutex);
    sm_diag_record_t *rec = sm_diag_store_find_id(&g_diag.store, jobs[i].id);
    uint64_t now_us = monotonic_time_us();
    if (rec && rec->pid == jobs[i].pid &&
        sm_diag_record_finalise_for_shutdown(rec, policy, state, wall_time_ms(),
                                             now_us))
      mark_dirty_locked(now_us);
    pthread_mutex_unlock(&g_diag.store_mutex);
  }
}

// Re-evaluate time-based stages, then run due kernel-log scans and the
// error-history poll. Returns the next wake-up delay.
static uint64_t periodic(const runtime_config_t *cfg) {
  const sm_diag_policy_t policy = current_policy(cfg);
  uint64_t now_us = monotonic_time_us();
  int64_t now_ms = wall_time_ms();
  bool busy = false;
  bool window_open = false;
  klog_job_t *jobs = NULL;
  size_t job_count = 0;

  pthread_mutex_lock(&g_diag.store_mutex);
  size_t count = sm_diag_store_count(&g_diag.store);
  for (size_t i = 0; i < count; ++i) {
    sm_diag_record_t *rec = sm_diag_store_at(&g_diag.store, i);
    if (rec->restored)
      continue;
    sm_diag_outcome_t before = rec->outcome;
    if (sm_diag_record_evaluate(rec, &policy, now_ms, now_us)) {
      mark_dirty_locked(now_us);
      if (rec->outcome != before)
        schedule_klog_scan_locked(rec, now_us);
    }
    if (sm_diag_record_is_active(rec))
      busy = true;
    if (rec->error_window_end_mono_us > now_us) {
      window_open = true;
      busy = true;
    } else if (rec->error_window_end_mono_us != 0 && rec->klog_anchor_valid &&
               !rec->klog_final_scan_done && !rec->klog_scan_due_mono_us &&
               sm_diag_outcome_is_failure(rec->outcome)) {
      // One last look once the window closes.
      rec->klog_final_scan_done = true;
      rec->klog_scan_due_mono_us = now_us;
    }
    if (rec->klog_scan_due_mono_us != 0) {
      busy = true;
      if (rec->klog_scan_due_mono_us <= now_us) {
        if (!jobs)
          jobs = calloc(SM_DIAG_MAX_RECORDS, sizeof(*jobs));
        if (jobs) {
          klog_job_t *job = &jobs[job_count++];
          job->id = rec->id;
          (void)strlcpy(job->title_id, rec->title_id, sizeof(job->title_id));
          job->pid = rec->pid;
          memcpy(job->anchor, rec->klog_anchor, rec->klog_anchor_len);
          job->anchor_len = rec->klog_anchor_len;
        }
        rec->klog_scan_due_mono_us = 0;
      }
    }
  }
  pthread_mutex_unlock(&g_diag.store_mutex);

  run_klog_scans(jobs, job_count, &policy);
  free(jobs);
  if (reconcile_liveness(&policy))
    busy = true;

  if (window_open && now_us >= g_diag.next_error_poll_us) {
    poll_error_history(now_ms, now_us);
    g_diag.next_error_poll_us = now_us + DIAG_ERROR_POLL_US;
  }
  persist_if_due(now_us, false);
  pthread_mutex_lock(&g_diag.store_mutex);
  bool dirty = g_diag.dirty;
  pthread_mutex_unlock(&g_diag.store_mutex);
  return busy || dirty ? DIAG_ACTIVE_TICK_US : DIAG_IDLE_TICK_US;
}

static void *diag_thread_main(void *arg) {
  (void)arg;
  pthread_mutex_lock(&g_diag.store_mutex);
  g_diag.store_ready = sm_diag_store_init(&g_diag.store);
  if (g_diag.store_ready)
    load_persisted_locked();
  g_diag.loaded = true;
  pthread_cond_broadcast(&g_diag.loaded_cond);
  pthread_mutex_unlock(&g_diag.store_mutex);
  if (!g_diag.store_ready) {
    log_debug("  [DIAG] launch record store unavailable");
    return NULL;
  }

  uint64_t wait_us = DIAG_ACTIVE_TICK_US;
  static sm_diag_input_t batch[DIAG_QUEUE_SIZE]; // Only this thread uses it.
  bool stop = false;
  while (!stop) {
    pthread_mutex_lock(&g_diag.queue_mutex);
    if (g_diag.queue_count == 0 && !g_diag.stop) {
      struct timespec deadline;
      clock_gettime(CLOCK_REALTIME, &deadline);
      deadline.tv_sec += (time_t)(wait_us / 1000000u);
      deadline.tv_nsec += (long)(wait_us % 1000000u) * 1000L;
      if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
      }
      (void)pthread_cond_timedwait(&g_diag.queue_cond, &g_diag.queue_mutex,
                                   &deadline);
    }
    size_t count = g_diag.queue_count;
    for (size_t i = 0; i < count; ++i)
      batch[i] = g_diag.queue[(g_diag.queue_head + i) % DIAG_QUEUE_SIZE];
    g_diag.queue_head = (g_diag.queue_head + count) % DIAG_QUEUE_SIZE;
    g_diag.queue_count = 0;
    uint64_t dropped = g_diag.dropped;
    g_diag.dropped = 0;
    stop = g_diag.stop;
    pthread_mutex_unlock(&g_diag.queue_mutex);

    if (dropped)
      log_debug("  [DIAG] dropped %" PRIu64 " launch event(s): queue full", dropped);
    const runtime_config_t cfg = runtime_config();
    if (cfg.diagnostics_enabled) {
      for (size_t i = 0; i < count; ++i)
        process_event(&batch[i], &cfg);
    }
    wait_us = periodic(&cfg);
  }
  {
    const runtime_config_t cfg = runtime_config();
    const sm_diag_policy_t policy = current_policy(&cfg);
    finalise_for_shutdown(&policy);
  }
  // Last record written before app.db, mdbg and the log are torn down.
  persist_if_due(monotonic_time_us(), true);
  return NULL;
}

void sm_diag_stop(void) {
  pthread_mutex_lock(&g_diag.queue_mutex);
  g_diag.stop = true;
  bool running = g_diag.thread_running;
  pthread_cond_broadcast(&g_diag.queue_cond);
  pthread_mutex_unlock(&g_diag.queue_mutex);
  // A later ensure_started() sees the stop flag and creates no thread.
  if (running)
    (void)pthread_join(g_diag.thread, NULL);
  pthread_mutex_lock(&g_diag.queue_mutex);
  g_diag.thread_running = false;
  pthread_mutex_unlock(&g_diag.queue_mutex);
}

// --- API ---

static bool wait_until_loaded(void) {
  ensure_started();
  pthread_mutex_lock(&g_diag.store_mutex);
  if (!g_diag.loaded) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += (long)DIAG_LOAD_WAIT_US * 1000L;
    while (deadline.tv_nsec >= 1000000000L) {
      deadline.tv_sec++;
      deadline.tv_nsec -= 1000000000L;
    }
    while (!g_diag.loaded &&
           pthread_cond_timedwait(&g_diag.loaded_cond, &g_diag.store_mutex,
                                  &deadline) == 0) {
    }
  }
  bool ready = g_diag.loaded && g_diag.store_ready;
  pthread_mutex_unlock(&g_diag.store_mutex);
  return ready;
}

bool sm_diag_add_last_launch(struct json_object *item, const char *title_id) {
  struct json_object *summary = NULL;
  if (title_id && wait_until_loaded()) {
    pthread_mutex_lock(&g_diag.store_mutex);
    sm_diag_record_t *rec = sm_diag_store_latest_for_title(&g_diag.store, title_id);
    if (rec) {
      summary = sm_diag_record_summary_json(rec);
      if (!summary) {
        pthread_mutex_unlock(&g_diag.store_mutex);
        return false;
      }
    }
    pthread_mutex_unlock(&g_diag.store_mutex);
  }
  // A NULL value is stored as JSON null.
  return json_object_object_add(item, "last_launch", summary) == 0;
}

static struct json_object *error_response(int http, int status,
                                          const char *message, int *http_status) {
  *http_status = http;
  struct json_object *response = json_object_new_object();
  if (!response)
    return NULL;
  json_object_object_add(response, "status", json_object_new_int(status));
  json_object_object_add(response, "error", json_object_new_string(message));
  return response;
}

static bool parse_limit(const char *text, size_t fallback, size_t max,
                        size_t *out) {
  *out = fallback;
  if (!text || !text[0])
    return true;
  char *end = NULL;
  errno = 0;
  unsigned long value = strtoul(text, &end, 10);
  if (errno != 0 || !end || *end != '\0' || value == 0 || value > max)
    return false;
  *out = (size_t)value;
  return true;
}

static struct json_object *launches_response(const char *title_id,
                                             const char *limit_text,
                                             int *http_status) {
  size_t limit = 0;
  if (!parse_limit(limit_text, DIAG_DEFAULT_LIST_LIMIT, SM_DIAG_MAX_RECORDS, &limit))
    return error_response(400, EINVAL, "limit must be 1..64", http_status);
  if (title_id && title_id[0] && !is_supported_game_title_id(title_id))
    return error_response(400, EINVAL,
                          "title_id must be a valid PS4 or PS5 title ID",
                          http_status);
  struct json_object *response = json_object_new_object();
  struct json_object *items = json_object_new_array();
  if (!response || !items) {
    if (response)
      json_object_put(response);
    if (items)
      json_object_put(items);
    return NULL;
  }
  bool ready = wait_until_loaded();
  runtime_config_t cfg = runtime_config();
  json_object_object_add(response, "status", json_object_new_int(0));
  json_object_object_add(response, "enabled",
                         json_object_new_boolean(cfg.diagnostics_enabled));
  json_object_object_add(response, "running_confirm_seconds",
                         json_object_new_int64(cfg.diagnostics_running_confirm_seconds));
  json_object_object_add(response, "error_window_seconds",
                         json_object_new_int64(cfg.diagnostics_error_window_seconds));
  if (ready) {
    pthread_mutex_lock(&g_diag.store_mutex);
    size_t count = sm_diag_store_count(&g_diag.store);
    for (size_t i = count; i > 0 && json_object_array_length(items) < limit; --i) {
      sm_diag_record_t *rec = sm_diag_store_at(&g_diag.store, i - 1u);
      if (title_id && title_id[0] && strcmp(rec->title_id, title_id) != 0)
        continue;
      struct json_object *item = sm_diag_record_to_json(rec);
      if (item)
        json_object_array_add(items, item);
    }
    pthread_mutex_unlock(&g_diag.store_mutex);
  }
  json_object_object_add(response, "launches", items);
  *http_status = 200;
  return response;
}

static struct json_object *launch_response(const char *id_text,
                                           int *http_status) {
  char *end = NULL;
  errno = 0;
  unsigned long long id = strtoull(id_text, &end, 10);
  if (errno != 0 || !end || *end != '\0' || id == 0 || id_text[0] == '\0')
    return error_response(400, EINVAL, "launch id must be a positive integer",
                          http_status);
  struct json_object *item = NULL;
  if (wait_until_loaded()) {
    pthread_mutex_lock(&g_diag.store_mutex);
    sm_diag_record_t *rec = sm_diag_store_find_id(&g_diag.store, (uint64_t)id);
    if (rec)
      item = sm_diag_record_to_json(rec);
    pthread_mutex_unlock(&g_diag.store_mutex);
  }
  if (!item)
    return error_response(404, ENOENT, "launch record not found", http_status);
  struct json_object *response = json_object_new_object();
  if (!response) {
    json_object_put(item);
    return NULL;
  }
  json_object_object_add(response, "status", json_object_new_int(0));
  json_object_object_add(response, "launch", item);
  *http_status = 200;
  return response;
}

static struct json_object *errors_response(const char *limit_text,
                                           int *http_status) {
  size_t limit = 0;
  if (!parse_limit(limit_text, DIAG_DEFAULT_LIST_LIMIT, DIAG_MAX_ERROR_LIST, &limit))
    return error_response(400, EINVAL, "limit must be 1..200", http_status);
  struct json_object *response = json_object_new_object();
  struct json_object *items = json_object_new_array();
  if (!response || !items) {
    if (response)
      json_object_put(response);
    if (items)
      json_object_put(items);
    return NULL;
  }
  history_entry_t *entries = NULL;
  ssize_t count = list_error_history(&entries);
  int list_errno = count < 0 ? errno : 0;
  json_object_object_add(response, "status", json_object_new_int(0));
  json_object_object_add(response, "directory",
                         json_object_new_string(DIAG_ERROR_HISTORY_DIR));
  json_object_object_add(response, "readable", json_object_new_boolean(count >= 0));
  json_object_object_add(response, "read_error",
                         count < 0 ? json_object_new_string(strerror(list_errno)) : NULL);
  json_object_object_add(response, "total_files",
                         json_object_new_int64(count < 0 ? 0 : (int64_t)count));
  for (ssize_t i = 0; i < count && (size_t)i < limit; ++i) {
    sm_diag_native_error_t error;
    (void)read_native_error(&entries[i], &error);
    struct json_object *item = sm_diag_native_error_to_json(&error);
    if (item)
      json_object_array_add(items, item);
  }
  free(entries);
  json_object_object_add(response, "errors", items);
  *http_status = 200;
  return response;
}

#define DIAG_ROUTE_SYSTEM "/api/v1/system"
#define DIAG_ROUTE_LAUNCHES "/api/v1/diagnostics/launches"
#define DIAG_ROUTE_ERRORS "/api/v1/diagnostics/errors"

bool sm_diag_api_route(const char *url) {
  if (!url)
    return false;
  size_t launches_len = strlen(DIAG_ROUTE_LAUNCHES);
  return strcmp(url, DIAG_ROUTE_SYSTEM) == 0 ||
         strcmp(url, DIAG_ROUTE_ERRORS) == 0 ||
         strcmp(url, DIAG_ROUTE_LAUNCHES) == 0 ||
         (strncmp(url, DIAG_ROUTE_LAUNCHES, launches_len) == 0 &&
          url[launches_len] == '/');
}

struct json_object *sm_diag_api_response(const char *url, const char *title_id,
                                         const char *limit, int *http_status) {
  *http_status = 200;
  if (strcmp(url, DIAG_ROUTE_SYSTEM) == 0) {
    struct json_object *report = sm_diag_system_report();
    if (report)
      json_object_object_add(report, "status", json_object_new_int(0));
    return report;
  }
  if (strcmp(url, DIAG_ROUTE_ERRORS) == 0)
    return errors_response(limit, http_status);
  if (strcmp(url, DIAG_ROUTE_LAUNCHES) == 0)
    return launches_response(title_id, limit, http_status);
  size_t launches_len = strlen(DIAG_ROUTE_LAUNCHES);
  if (strncmp(url, DIAG_ROUTE_LAUNCHES, launches_len) == 0 &&
      url[launches_len] == '/')
    return launch_response(url + launches_len + 1u, http_status);
  return error_response(404, ENOENT, "unknown API route", http_status);
}
