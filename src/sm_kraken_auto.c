#include "sm_platform.h"

#include <json-c/json.h>
#include <pthread.h>

#include "sm_api_service.h"
#include "sm_appdb.h"
#include "sm_config_mount.h"
#include "sm_filesystem.h"
#include "sm_game_cache.h"
#include "sm_game_lifecycle.h"
#include "sm_kraken.h"
#include "sm_kraken_auto.h"
#include "sm_log.h"
#include "sm_pkg.h"
#include "sm_runtime.h"
#include "sm_stability.h"
#include "sm_takeover.h"
#include "sm_types.h"
#include "sm_unimount_logic.h"

#define AUTO_MAX_CANDIDATES 64u
#define AUTO_MAX_FAILURES 64u
#define AUTO_IDLE_WAIT_SECONDS 10

typedef struct {
  bool used;
  char path[MAX_PATH];
  char real_path[MAX_PATH];
  uint64_t size;
  int64_t mtime;
  uint64_t generation;
  bool stable;
  bool parsed;
  bool eligible;
  bool building;
  char title_id[MAX_TITLE_ID];
  char title_name[MAX_TITLE_NAME];
  char version[32];
  sm_auto_decision_t decision;
  char error[64];
} auto_candidate_t;

static struct {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  pthread_t thread;
  bool started;
  bool stop;
  bool wake;
  uint64_t generation;
  bool job_active;
  char job_path[MAX_PATH];
  auto_candidate_t candidates[AUTO_MAX_CANDIDATES];
  sm_auto_failure_t failures[AUTO_MAX_FAILURES];
} g_auto = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

static bool auto_enabled(void) {
  return runtime_config().kraken_auto_mount_enabled;
}

static auto_candidate_t *find_candidate_locked(const char *path) {
  for (size_t i = 0; i < AUTO_MAX_CANDIDATES; ++i) {
    auto_candidate_t *c = &g_auto.candidates[i];
    if (c->used && (strcmp(c->path, path) == 0 ||
                    (c->real_path[0] && strcmp(c->real_path, path) == 0)))
      return c;
  }
  return NULL;
}

void sm_kraken_auto_begin_pass(void) {
  pthread_mutex_lock(&g_auto.mutex);
  ++g_auto.generation;
  pthread_mutex_unlock(&g_auto.mutex);
}

void sm_kraken_auto_note_package(const char *path, bool *unstable_out) {
  if (!path || !auto_enabled() || !sm_kraken_is_package(path))
    return;
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
    return;
  // Same rule as other sources: a file still being copied waits.
  bool stable = is_path_stable_now(path, NULL, NULL);
  if (!stable && unstable_out)
    *unstable_out = true;

  pthread_mutex_lock(&g_auto.mutex);
  auto_candidate_t *c = find_candidate_locked(path);
  if (!c) {
    for (size_t i = 0; i < AUTO_MAX_CANDIDATES && !c; ++i)
      if (!g_auto.candidates[i].used)
        c = &g_auto.candidates[i];
    if (!c) {
      pthread_mutex_unlock(&g_auto.mutex);
      return;
    }
    memset(c, 0, sizeof(*c));
    c->used = true;
    (void)strlcpy(c->path, path, sizeof(c->path));
    c->decision = SM_AUTO_WAIT;
  }
  if (c->size != (uint64_t)st.st_size || c->mtime != (int64_t)st.st_mtime) {
    // A changed file is a new candidate: parse it again.
    c->size = (uint64_t)st.st_size;
    c->mtime = (int64_t)st.st_mtime;
    c->parsed = false;
    c->eligible = false;
    c->error[0] = '\0';
    if (!c->building)
      c->decision = SM_AUTO_WAIT;
  }
  c->stable = stable;
  c->generation = g_auto.generation;
  pthread_mutex_unlock(&g_auto.mutex);
}

void sm_kraken_auto_end_pass(bool complete) {
  pthread_mutex_lock(&g_auto.mutex);
  if (complete) {
    for (size_t i = 0; i < AUTO_MAX_CANDIDATES; ++i) {
      auto_candidate_t *c = &g_auto.candidates[i];
      if (c->used && !c->building && c->generation != g_auto.generation)
        memset(c, 0, sizeof(*c));
    }
  }
  g_auto.wake = true;
  pthread_cond_signal(&g_auto.cond);
  pthread_mutex_unlock(&g_auto.mutex);
}

static const char *decision_status(const auto_candidate_t *c) {
  if (c->building)
    return "building";
  switch (c->decision) {
  case SM_AUTO_READY:
    return "ready";
  case SM_AUTO_DUPLICATE:
    return "duplicate";
  case SM_AUTO_FAILED:
    return "error";
  case SM_AUTO_WAIT:
  case SM_AUTO_START:
    return "pending";
  case SM_AUTO_IGNORE:
  default:
    return NULL;
  }
}

static void record_failure_locked(auto_candidate_t *c, const char *reason) {
  sm_auto_failure_record(g_auto.failures, AUTO_MAX_FAILURES, c->path, c->size,
                         c->mtime, reason);
  (void)strlcpy(c->error, reason && reason[0] ? reason : "failed",
                sizeof(c->error));
  c->decision = SM_AUTO_FAILED;
}

void sm_kraken_auto_job_finished(const char *source, int status,
                                 const char *reason) {
  if (!source)
    return;
  pthread_mutex_lock(&g_auto.mutex);
  auto_candidate_t *c = find_candidate_locked(source);
  bool ours = g_auto.job_active && (strcmp(g_auto.job_path, source) == 0 ||
                                    (c && strcmp(g_auto.job_path, c->path) == 0));
  if (c) {
    c->building = false;
    if (status == 0) {
      sm_auto_failure_forget(g_auto.failures, AUTO_MAX_FAILURES, c->path);
      c->decision = SM_AUTO_READY;
      c->error[0] = '\0';
    } else if (sm_auto_error_is_transient(status, reason)) {
      c->decision = SM_AUTO_WAIT;
    } else {
      record_failure_locked(c, reason && reason[0] ? reason : strerror(status));
    }
  }
  if (ours) {
    g_auto.job_active = false;
    g_auto.job_path[0] = '\0';
  }
  g_auto.wake = true;
  pthread_cond_signal(&g_auto.cond);
  pthread_mutex_unlock(&g_auto.mutex);
  if (c && ours)
    log_debug("  [AUTO] %s: %s%s%s", source,
              status == 0 ? "mounted" : "job stopped: ",
              status == 0 ? "" : (reason && reason[0] ? reason : strerror(status)),
              status != 0 && sm_auto_error_is_transient(status, reason)
                  ? " (retried later)" : "");
}

void sm_kraken_auto_forget_failure(const char *path) {
  if (!path)
    return;
  pthread_mutex_lock(&g_auto.mutex);
  sm_auto_failure_forget(g_auto.failures, AUTO_MAX_FAILURES, path);
  auto_candidate_t *c = find_candidate_locked(path);
  if (c) {
    sm_auto_failure_forget(g_auto.failures, AUTO_MAX_FAILURES, c->path);
    if (c->decision == SM_AUTO_FAILED)
      c->decision = SM_AUTO_WAIT;
  }
  pthread_mutex_unlock(&g_auto.mutex);
}

bool sm_kraken_auto_busy(void) {
  pthread_mutex_lock(&g_auto.mutex);
  bool busy = g_auto.job_active;
  pthread_mutex_unlock(&g_auto.mutex);
  return busy;
}

// Header checks only; the storage job runs the full parser.
static void parse_candidate(auto_candidate_t *c) {
  sm_pkg_info_t info;
  c->parsed = true;
  c->eligible = false;
  char resolved[PATH_MAX];
  if (realpath(c->path, resolved))
    (void)strlcpy(c->real_path, resolved, sizeof(c->real_path));
  // Retail, encrypted, PS4 and patch packages are not candidates.
  if (!sm_pkg_parse_file(c->path, NULL, &info) ||
      info.pkg_kind != PKG_KIND_BASE ||
      strncmp(info.title_id, "PPSA", 4u) != 0 ||
      !sm_kraken_can_mount(&info)) {
    if (errno == EAGAIN)
      c->parsed = false;  // changed while reading; try next pass
    return;
  }
  c->eligible = true;
  (void)strlcpy(c->title_id, info.title_id, sizeof(c->title_id));
  (void)strlcpy(c->title_name, info.title_name[0] ? info.title_name : info.title_id,
                sizeof(c->title_name));
  (void)strlcpy(c->version, info.app_ver, sizeof(c->version));
}

static sm_auto_library_t library_state(const auto_candidate_t *c,
                                       const sm_game_cache_snapshot_entry_t *games,
                                       size_t game_count) {
  for (size_t i = 0; i < game_count; ++i) {
    if (strcmp(games[i].title_id, c->title_id) != 0)
      continue;
    char package[MAX_PATH];
    if (sm_kraken_is_cache(games[i].path) && sm_kraken_source(games[i].path, package) &&
        (strcmp(package, c->path) == 0 ||
         (c->real_path[0] && strcmp(package, c->real_path) == 0)))
      return SM_AUTO_LIBRARY_SAME_PKG;
    return SM_AUTO_LIBRARY_OTHER;
  }
  // Registered by us but not in the library yet (offline source, restart).
  char link[MAX_PATH], package[MAX_PATH];
  if (read_mount_link(c->title_id, link, sizeof(link)) &&
      !(sm_kraken_is_cache(link) && sm_kraken_source(link, package) &&
        (strcmp(package, c->path) == 0 ||
         (c->real_path[0] && strcmp(package, c->real_path) == 0))))
    return SM_AUTO_LIBRARY_OTHER;
  return SM_AUTO_LIBRARY_NONE;
}

static void process_candidates(void) {
  auto_candidate_t *work = calloc(AUTO_MAX_CANDIDATES, sizeof(*work));
  if (!work)
    return;
  pthread_mutex_lock(&g_auto.mutex);
  bool job_active = g_auto.job_active;
  memcpy(work, g_auto.candidates, AUTO_MAX_CANDIDATES * sizeof(*work));
  pthread_mutex_unlock(&g_auto.mutex);

  bool game_running = sm_game_lifecycle_has_active_game() ||
                      !sm_game_lifecycle_background_title(NULL);
  sm_game_cache_snapshot_entry_t *games = NULL;
  size_t game_count = 0;
  (void)sm_game_cache_snapshot(&games, &game_count);
  struct AppDbTitleList titles = {0};
  bool titles_ready = get_app_db_title_list_cached(&titles);
  bool started = false;

  for (size_t i = 0; i < AUTO_MAX_CANDIDATES; ++i) {
    auto_candidate_t *c = &work[i];
    if (!c->used || c->building)
      continue;
    if (should_stop_requested() || runtime_sleep_mode_active())
      break;
    if (!c->parsed && c->stable && !game_running)
      parse_candidate(c);
    if (!c->parsed)
      continue;

    sm_auto_facts_t facts = {
        .enabled = true,
        .ownership_ready = sm_takeover_ownership_ready(),
        .game_running = game_running,
        .job_active = job_active || started,
        .stable = c->stable,
        .eligible = c->eligible,
    };
    bool legacy_busy = false;
    if (c->eligible) {
      facts.library = library_state(c, games, game_count);
      pthread_mutex_lock(&g_auto.mutex);
      facts.failed_same_key = sm_auto_failure_blocks(
          g_auto.failures, AUTO_MAX_FAILURES, c->path, c->size, c->mtime);
      pthread_mutex_unlock(&g_auto.mutex);
      char link[MAX_PATH];
      if (facts.library == SM_AUTO_LIBRARY_NONE && titles_ready &&
          app_db_title_list_contains(&titles, c->title_id) &&
          !read_mount_link(c->title_id, link, sizeof(link)) &&
          !sm_takeover_kraken_adoptable(c->title_id, &legacy_busy) && !legacy_busy)
        facts.registered_conflict = true;
      if (legacy_busy)
        facts.stable = false;  // retried after the legacy mount is released
    }
    sm_auto_decision_t decision = sm_auto_decide(&facts);
    const char *error = NULL;
    bool started_now = false;
    if (decision == SM_AUTO_START) {
      // Claim before starting: the job's result tap may arrive at any time.
      pthread_mutex_lock(&g_auto.mutex);
      auto_candidate_t *live = find_candidate_locked(c->path);
      bool claimed = !g_auto.job_active && live && live->size == c->size &&
                     live->mtime == c->mtime;
      if (claimed) {
        g_auto.job_active = true;
        (void)strlcpy(g_auto.job_path, c->path, sizeof(g_auto.job_path));
        (void)strlcpy(live->real_path, c->real_path, sizeof(live->real_path));
        live->building = true;
      }
      pthread_mutex_unlock(&g_auto.mutex);
      if (!claimed) {
        decision = SM_AUTO_WAIT;
      } else {
        int status = sm_api_service_start_kraken_mount(c->path, &error);
        if (status == 0) {
          started = started_now = true;
          log_debug("  [AUTO] building %s (%s) from %s", c->title_id,
                    c->title_name, c->path);
        } else {
          pthread_mutex_lock(&g_auto.mutex);
          g_auto.job_active = false;
          g_auto.job_path[0] = '\0';
          live = find_candidate_locked(c->path);
          if (live)
            live->building = false;
          pthread_mutex_unlock(&g_auto.mutex);
          if (sm_auto_error_is_transient(status, error) || status == EBUSY) {
            decision = SM_AUTO_WAIT;
          } else {
            decision = SM_AUTO_FAILED;
            if (!error)
              error = strerror(status);
            log_debug("  [AUTO] %s not mounted: %s", c->path, error);
          }
        }
      }
    }

    pthread_mutex_lock(&g_auto.mutex);
    auto_candidate_t *live = find_candidate_locked(c->path);
    if (live && live->size == c->size && live->mtime == c->mtime) {
      sm_auto_decision_t previous = live->decision;
      live->parsed = c->parsed;
      live->eligible = c->eligible;
      (void)strlcpy(live->real_path, c->real_path, sizeof(live->real_path));
      (void)strlcpy(live->title_id, c->title_id, sizeof(live->title_id));
      (void)strlcpy(live->title_name, c->title_name, sizeof(live->title_name));
      (void)strlcpy(live->version, c->version, sizeof(live->version));
      if (!live->building && !started_now) {
        if (decision == SM_AUTO_FAILED && error)
          record_failure_locked(live, error);
        else
          live->decision = decision;
        if (decision == SM_AUTO_FAILED && !error) {
          for (size_t f = 0; f < AUTO_MAX_FAILURES; ++f)
            if (g_auto.failures[f].used &&
                strcmp(g_auto.failures[f].path, live->path) == 0)
              (void)strlcpy(live->error, g_auto.failures[f].error,
                            sizeof(live->error));
        }
      }
      if (previous != live->decision && live->eligible &&
          (live->decision == SM_AUTO_DUPLICATE || live->decision == SM_AUTO_READY))
        log_debug("  [AUTO] %s: %s (%s)", live->path,
                  live->decision == SM_AUTO_READY ? "already mounted"
                                                  : "title provided by another source",
                  live->title_id);
    }
    pthread_mutex_unlock(&g_auto.mutex);
  }
  free_app_db_title_list(&titles);
  free(games);
  free(work);
}

static void *auto_thread_main(void *unused) {
  (void)unused;
  for (;;) {
    pthread_mutex_lock(&g_auto.mutex);
    if (!g_auto.stop && !g_auto.wake) {
      struct timespec deadline;
      if (clock_gettime(CLOCK_REALTIME, &deadline) == 0) {
        deadline.tv_sec += AUTO_IDLE_WAIT_SECONDS;
        (void)pthread_cond_timedwait(&g_auto.cond, &g_auto.mutex, &deadline);
      }
    }
    bool stop = g_auto.stop;
    g_auto.wake = false;
    pthread_mutex_unlock(&g_auto.mutex);
    if (stop || should_stop_requested())
      break;
    if (!sm_takeover_ownership_ready() || runtime_sleep_mode_active())
      continue;
    sm_takeover_retry_legacy_mounts();
    if (auto_enabled())
      process_candidates();
  }
  return NULL;
}

bool sm_kraken_auto_start(void) {
  pthread_mutex_lock(&g_auto.mutex);
  if (g_auto.started) {
    pthread_mutex_unlock(&g_auto.mutex);
    return true;
  }
  g_auto.stop = false;
  int rc = pthread_create(&g_auto.thread, NULL, auto_thread_main, NULL);
  g_auto.started = rc == 0;
  pthread_mutex_unlock(&g_auto.mutex);
  if (rc != 0) {
    errno = rc;
    return false;
  }
  return true;
}

void sm_kraken_auto_stop(void) {
  pthread_mutex_lock(&g_auto.mutex);
  bool started = g_auto.started;
  g_auto.stop = true;
  g_auto.started = false;
  pthread_cond_signal(&g_auto.cond);
  pthread_mutex_unlock(&g_auto.mutex);
  if (started)
    (void)pthread_join(g_auto.thread, NULL);
}

size_t sm_kraken_auto_snapshot(sm_kraken_auto_entry_t *out, size_t max) {
  size_t count = 0;
  pthread_mutex_lock(&g_auto.mutex);
  for (size_t i = 0; i < AUTO_MAX_CANDIDATES && count < max; ++i) {
    const auto_candidate_t *c = &g_auto.candidates[i];
    const char *status = c->used && c->eligible ? decision_status(c) : NULL;
    if (!status || strcmp(status, "ready") == 0)
      continue;
    sm_kraken_auto_entry_t *entry = &out[count++];
    memset(entry, 0, sizeof(*entry));
    (void)strlcpy(entry->path, c->path, sizeof(entry->path));
    (void)strlcpy(entry->title_id, c->title_id, sizeof(entry->title_id));
    (void)strlcpy(entry->title_name, c->title_name, sizeof(entry->title_name));
    (void)strlcpy(entry->version, c->version, sizeof(entry->version));
    (void)strlcpy(entry->error, c->error, sizeof(entry->error));
    entry->size = c->size;
    entry->status = status;
  }
  pthread_mutex_unlock(&g_auto.mutex);
  return count;
}

static bool add_value(struct json_object *object, const char *key,
                      struct json_object *value) {
  if (!value)
    return false;
  if (json_object_object_add(object, key, value) != 0) {
    json_object_put(value);
    return false;
  }
  return true;
}

bool sm_kraken_auto_add_status_json(struct json_object *object) {
  struct json_object *failed = json_object_new_array();
  if (!failed)
    return false;
  size_t queued = 0;
  char building[MAX_PATH] = "";
  bool ok = true;
  pthread_mutex_lock(&g_auto.mutex);
  for (size_t i = 0; i < AUTO_MAX_CANDIDATES; ++i) {
    const auto_candidate_t *c = &g_auto.candidates[i];
    if (c->used && c->eligible && !c->building &&
        (c->decision == SM_AUTO_WAIT || c->decision == SM_AUTO_START))
      ++queued;
  }
  if (g_auto.job_active)
    (void)strlcpy(building, g_auto.job_path, sizeof(building));
  for (size_t i = 0; ok && i < AUTO_MAX_FAILURES; ++i) {
    const sm_auto_failure_t *f = &g_auto.failures[i];
    if (!f->used)
      continue;
    struct json_object *item = json_object_new_object();
    ok = item && add_value(item, "path", json_object_new_string(f->path)) &&
         add_value(item, "error", json_object_new_string(f->error)) &&
         json_object_array_add(failed, item) == 0;
    if (!ok && item)
      json_object_put(item);
  }
  pthread_mutex_unlock(&g_auto.mutex);
  if (!ok) {
    json_object_put(failed);
    return false;
  }
  return add_value(object, "failed", failed) &&
         add_value(object, "enabled", json_object_new_boolean(auto_enabled())) &&
         add_value(object, "queued", json_object_new_int64((int64_t)queued)) &&
         (building[0] ? add_value(object, "building", json_object_new_string(building))
                      : json_object_object_add(object, "building", NULL) == 0);
}
