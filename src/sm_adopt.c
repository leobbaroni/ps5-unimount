#include "sm_platform.h"

#include <sys/event.h>
#include <sys/sysctl.h>

#include "sm_adopt.h"
#include "sm_fakelib.h"
#include "sm_filesystem.h"
#include "sm_l10n.h"
#include "sm_limits.h"
#include "sm_log.h"
#include "sm_pkg_backport.h"
#include "sm_sandbox_record.h"
#include "sm_shellcore_service.h"
#include "sm_takeover.h"
#include "sm_time.h"
#include "sm_unimount_logic.h"

#define ADOPT_KINFO_PID_OFFSET 72

static bool process_alive(pid_t pid) {
  return pid > 0 && (kill(pid, 0) == 0 || errno != ESRCH);
}

static void app_info_title(pid_t pid, uint32_t *app_id,
                           char title_id[MAX_TITLE_ID]) {
  app_info_t info;
  memset(&info, 0, sizeof(info));
  *app_id = 0;
  title_id[0] = '\0';
  if (pid <= 0 || sceKernelGetAppInfo(pid, &info) != 0)
    return;
  size_t length = strnlen(info.title_id, sizeof(info.title_id));
  if (length < MAX_TITLE_ID) {
    memcpy(title_id, info.title_id, length);
    title_id[length] = '\0';
  }
  *app_id = info.app_id;
}

// Reads and parses the record. On failure *reason says why.
static sm_session_record_t *read_record(const char **reason) {
  static char error[96];
  int fd = open(SANDBOX_SESSION_FILE, O_RDONLY | O_NOFOLLOW);
  if (fd < 0) {
    if (errno == ENOENT) {
      *reason = "no session record";
    } else {
      (void)snprintf(error, sizeof(error), "session record unreadable (%s)",
                     strerror(errno));
      *reason = error;
    }
    return NULL;
  }
  size_t capacity = SM_SESSION_RECORD_MAX_BYTES + 1u;
  char *body = malloc(capacity);
  sm_session_record_t *record = calloc(1, sizeof(*record));
  size_t length = 0;
  int read_errno = 0;
  while (body && record && length < capacity) {
    ssize_t received = read(fd, body + length, capacity - length);
    if (received < 0 && errno == EINTR)
      continue;
    if (received < 0)
      read_errno = errno;
    if (received <= 0)
      break;
    length += (size_t)received;
  }
  (void)close(fd);
  bool parsed = false;
  if (!body || !record) {
    *reason = "out of memory reading the session record";
  } else if (read_errno != 0) {
    (void)snprintf(error, sizeof(error), "session record unreadable (%s)",
                   strerror(read_errno));
    *reason = error;
  } else {
    parsed = sm_session_record_parse(body, length, record, reason);
  }
  free(body);
  if (!parsed) {
    free(record);
    return NULL;
  }
  return record;
}

// The record, when it passes every check; otherwise NULL and *reason.
static sm_session_record_t *evaluate(const char *running_title_id,
                                     char *buf, size_t size,
                                     const char **reason) {
  sm_session_record_t *record = read_record(reason);
  if (!record)
    return NULL;
  sm_adopt_live_t live;
  memset(&live, 0, sizeof(live));
  live.boot_time = sm_ownership_boot_time();
  pid_t pid = (pid_t)record->game_pid;
  live.game_alive = process_alive(pid);
  if (live.game_alive)
    app_info_title(pid, &live.game_app_id, live.game_title_id);
  int32_t running = sceSystemServiceGetAppIdOfRunningBigApp();
  live.running_app_id = running > 0 ? (uint32_t)running : 0;
  if (running_title_id && strlen(running_title_id) == 9u)
    (void)strlcpy(live.running_title_id, running_title_id,
                  sizeof(live.running_title_id));

  struct statfs *mounts = NULL;
  int count = sm_mount_table_snapshot(&mounts);
  sm_mount_entry_t *entries =
      count > 0 ? calloc((size_t)count, sizeof(*entries)) : NULL;
  bool adopt = false;
  if (count < 0 || (count > 0 && !entries)) {
    (void)snprintf(buf, size, "mount table unavailable (%s)",
                   strerror(count < 0 ? errno : ENOMEM));
    *reason = buf;
  } else {
    for (int i = 0; i < count; ++i) {
      entries[i].fstype = mounts[i].f_fstypename;
      entries[i].from = mounts[i].f_mntfromname;
      entries[i].on = mounts[i].f_mntonname;
      entries[i].from_size = sizeof(mounts[i].f_mntfromname);
      memcpy(entries[i].fsid, &mounts[i].f_fsid, sizeof(entries[i].fsid));
    }
    live.mounts = entries;
    live.mount_count = (size_t)count;
    adopt = sm_session_adopt_decide(record, &live, buf, size, reason);
  }
  free(entries);
  free(mounts);
  if (!adopt) {
    free(record);
    return NULL;
  }
  return record;
}

void sm_adopt_dry_run(const char *running_title_id) {
  char buf[MAX_PATH + 128];
  const char *reason = NULL;
  sm_session_record_t *record = evaluate(running_title_id, buf, sizeof(buf),
                                         &reason);
  if (!record) {
    log_debug("[ADOPT] refused: %s", reason ? reason : "unknown");
    return;
  }
  log_debug("[ADOPT] would adopt %zu layer(s) of %s pid=%ld",
            sm_session_record_units(record), record->title_id,
            (long)record->game_pid);
  free(record);
}

// A process of the same application other than the exited one (ExitSpawn).
static pid_t find_replacement(uint32_t app_id, const char *title_id,
                              pid_t exited_pid) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t size = 0;
  if (app_id == 0 || sysctl(mib, 4, NULL, &size, NULL, 0) != 0 || size == 0)
    return 0;
  uint8_t *buf = malloc(size);
  if (!buf)
    return 0;
  pid_t found = 0;
  if (sysctl(mib, 4, buf, &size, NULL, 0) == 0) {
    for (uint8_t *ptr = buf, *end = buf + size;
         (size_t)(end - ptr) >= sizeof(int);) {
      int structsize = 0;
      memcpy(&structsize, ptr, sizeof(structsize));
      if (structsize <= ADOPT_KINFO_PID_OFFSET + (int)sizeof(pid_t) ||
          (size_t)structsize > (size_t)(end - ptr))
        break;
      pid_t pid = 0;
      memcpy(&pid, ptr + ADOPT_KINFO_PID_OFFSET, sizeof(pid));
      ptr += structsize;
      uint32_t other_app_id = 0;
      char other_title[MAX_TITLE_ID];
      if (pid <= 0 || pid == exited_pid)
        continue;
      app_info_title(pid, &other_app_id, other_title);
      if (other_app_id == app_id && strcmp(other_title, title_id) == 0) {
        found = pid;
        break;
      }
    }
  }
  free(buf);
  return found;
}

typedef enum {
  WATCH_CONTINUE = 0,
  WATCH_EXITED,
  WATCH_STOP_NOW,   // alias-only session: normal teardown, then exit
  WATCH_STOP_KEEP,  // overlay session, second request: keep layers, exit
} watch_result_t;

static const char *session_kind_text(sm_session_kind_t kind) {
  return kind == SM_SESSION_OVERLAY   ? "overlay layers"
         : kind == SM_SESSION_ALIASES ? "only directory aliases"
                                      : "no layers";
}

// Stop requests follow sm_stop_decide with the adopted game active: an
// overlay session is never abandoned on a first signal.
static watch_result_t check_stop(const sm_adopt_hooks_t *hooks,
                                 const char *title_id, sm_session_kind_t kind,
                                 unsigned *seen, bool *deferred) {
  bool by_signal = false;
  unsigned requests = hooks->stop_requests ? hooks->stop_requests(&by_signal)
                                           : 0;
  if (requests == 0)
    return WATCH_CONTINUE;
  if (requests > *seen) {
    *seen = requests;
    log_debug("[ADOPT] stop request %u received (%s)", requests,
              by_signal ? "signal" : "STOP file");
  }
  sm_stop_facts_t facts = {.requests = requests,
                           .by_signal = by_signal,
                           .game_active = true,
                           .session = kind};
  sm_stop_decision_t decision = sm_stop_decide(&facts);
  if (decision == SM_STOP_DEFER) {
    if (!*deferred)
      log_debug("[ADOPT] stop deferred: %s is running (%s, %s)", title_id,
                by_signal ? "signal" : "STOP file", session_kind_text(kind));
    *deferred = true;
    return WATCH_CONTINUE;
  }
  if (decision == SM_STOP_NOW_KEEP) {
    log_debug("[ADOPT] stopping now while %s is running (%s); its overlay "
              "layers are kept in the session record",
              title_id, requests >= 2u ? "second stop request" : "stop request");
    return WATCH_STOP_KEEP;
  }
  if (decision == SM_STOP_NOW) {
    log_debug("[ADOPT] stopping now while %s is running (%s): %s in its "
              "sandbox, normal teardown",
              title_id, requests >= 2u ? "second stop request" : "signal",
              session_kind_text(kind));
    return WATCH_STOP_NOW;
  }
  return WATCH_CONTINUE;
}

// Returns WATCH_EXITED once the game process is gone, or a stop result.
static watch_result_t watch_game(pid_t pid, const char *title_id,
                                 sm_session_kind_t kind,
                                 const sm_adopt_hooks_t *hooks,
                                 bool *deferred) {
  unsigned seen = 0;
  int kq = kqueue();
  if (kq >= 0) {
    struct kevent kev;
    EV_SET(&kev, (uintptr_t)pid, EVFILT_PROC, EV_ADD | EV_ENABLE | EV_CLEAR,
           NOTE_EXIT, 0, NULL);
    if (kevent(kq, &kev, 1, NULL, 0, NULL) != 0) {
      int watch_errno = errno;
      (void)close(kq);
      kq = -1;
      if (watch_errno == ESRCH)
        return WATCH_EXITED;
      log_debug("[ADOPT] exit watch for pid=%ld unavailable (%s); polling",
                (long)pid, strerror(watch_errno));
    }
  }
  watch_result_t result = WATCH_EXITED;
  for (;;) {
    if (kq >= 0) {
      struct kevent event;
      struct timespec timeout = {
          .tv_sec = 0,
          .tv_nsec = (long)GAME_LIFECYCLE_POLL_INTERVAL_US * 1000L};
      int nev = kevent(kq, NULL, 0, &event, 1, &timeout);
      if (nev > 0 && event.filter == EVFILT_PROC &&
          (event.fflags & NOTE_EXIT) != 0 && (pid_t)event.ident == pid)
        break;
    } else {
      (void)sceKernelUsleep(GAME_LIFECYCLE_POLL_INTERVAL_US);
    }
    if (!process_alive(pid))
      break;
    result = check_stop(hooks, title_id, kind, &seen, deferred);
    if (result != WATCH_CONTINUE)
      break;
    result = WATCH_EXITED;
  }
  if (kq >= 0)
    (void)close(kq);
  return result;
}

static uint64_t elapsed_us(uint64_t since_us) {
  uint64_t now_us = monotonic_time_us();
  return now_us > since_us ? now_us - since_us : 0;
}

// The owner's retry rule for busy layers: every
// GAME_LIFECYCLE_POLL_INTERVAL_US until GAME_SANDBOX_CLEANUP_TIMEOUT_US after
// the first busy pass; any other error ends the retries.
static size_t release_fakelib(const char *title_id) {
  uint64_t deadline_us = 0;
  size_t remaining = 0;
  while (!sm_fakelib_adopted_release(deadline_us, &remaining)) {
    int error = errno != 0 ? errno : EIO;
    uint64_t now_us = monotonic_time_us();
    if (error != EBUSY) {
      log_debug("[ADOPT] cleanup retained %zu layer(s) of %s: %s", remaining,
                title_id, strerror(error));
      break;
    }
    if (deadline_us == 0) {
      deadline_us = now_us + GAME_SANDBOX_CLEANUP_TIMEOUT_US;
      log_debug("[ADOPT] busy sandbox cleanup scheduled: %s", title_id);
    }
    if (now_us == 0 || now_us >= deadline_us) {
      log_debug("[ADOPT] sandbox cleanup wait ended; retaining %zu layer(s) "
                "of %s",
                remaining, title_id);
      break;
    }
    (void)sceKernelUsleep(GAME_LIFECYCLE_POLL_INTERVAL_US);
  }
  return remaining;
}

static size_t release_redirects(const char *title_id) {
  uint64_t deadline_us = 0;
  size_t remaining = 0;
  while (!sm_pkg_backport_adopted_release(deadline_us, &remaining)) {
    int error = errno != 0 ? errno : EIO;
    uint64_t now_us = monotonic_time_us();
    if (error != EBUSY) {
      log_debug("[ADOPT] redirect cleanup retained for %s: %s", title_id,
                strerror(error));
      break;
    }
    if (deadline_us == 0) {
      deadline_us = now_us + GAME_SANDBOX_CLEANUP_TIMEOUT_US;
      log_debug("[ADOPT] busy redirect cleanup scheduled: %s", title_id);
    }
    if (now_us == 0 || now_us >= deadline_us) {
      log_debug("[ADOPT] redirect cleanup wait ended; retaining redirects: %s",
                title_id);
      break;
    }
    (void)sceKernelUsleep(GAME_LIFECYCLE_POLL_INTERVAL_US);
  }
  return remaining;
}

// The owner deletes redirects when it finalizes the exit: after the
// same-app handoff grace, and once the sandbox is gone or has lingered for
// GAME_EXIT_SANDBOX_LINGER_US. A replacement process keeps them.
static pid_t wait_for_finalize(const sm_session_record_t *record, pid_t pid,
                               uint64_t exit_us) {
  for (;;) {
    pid_t replacement =
        find_replacement(record->app_id, record->title_id, pid);
    if (replacement > 0)
      return replacement;
    uint64_t waited_us = elapsed_us(exit_us);
    if (waited_us >= GAME_PROCESS_HANDOFF_GRACE_US &&
        (record->redirect_count == 0 || waited_us >= GAME_EXIT_SANDBOX_LINGER_US ||
         !sm_shellcore_service_title_sandbox_exists(record->title_id)))
      return 0;
    (void)sceKernelUsleep(GAME_LIFECYCLE_POLL_INTERVAL_US);
  }
}

sm_adopt_result_t sm_adopt_session(const char *running_title_id,
                                   const sm_adopt_hooks_t *hooks) {
  char buf[MAX_PATH + 128];
  const char *reason = NULL;
  sm_session_record_t *record = evaluate(running_title_id, buf, sizeof(buf),
                                         &reason);
  if (!record) {
    log_debug("[ADOPT] refused: %s", reason ? reason : "unknown");
    return SM_ADOPT_NOT_TAKEN;
  }
  if (!sm_pkg_backport_adopt_redirects(record)) {
    log_debug("[ADOPT] refused: NSFS redirects could not be taken over (%s)",
              strerror(errno));
    free(record);
    return SM_ADOPT_NOT_TAKEN;
  }
  if (!sm_fakelib_adopt_session(record)) {
    int error = errno;
    sm_pkg_backport_adopted_drop();
    log_debug("[ADOPT] refused: sandbox layers could not be taken over (%s)",
              strerror(error));
    free(record);
    return SM_ADOPT_NOT_TAKEN;
  }

  pid_t pid = (pid_t)record->game_pid;
  size_t units = sm_session_record_units(record);
  sm_session_kind_t kind = sm_session_record_kind(record);
  if (hooks && hooks->process_name)
    syscall(SYS_thr_set_name, -1, hooks->process_name);
  // Rewrite the record under this pid; it now tracks what is left.
  (void)sm_session_record_start();
  log_debug("[ADOPT] adopted %zu layer(s) of %s pid=%ld", units,
            record->title_id, (long)pid);
  notify_system_l10n(SM_L10N_ADOPT_SESSION, record->title_id);

  bool deferred = false;
  sm_adopt_hooks_t no_hooks = {0};
  watch_result_t watched =
      watch_game(pid, record->title_id, kind, hooks ? hooks : &no_hooks,
                 &deferred);
  if (watched == WATCH_STOP_KEEP) {
    sm_session_record_stop();
    free(record);
    return SM_ADOPT_STOPPED;
  }
  uint64_t exit_us = monotonic_time_us();
  if (watched == WATCH_STOP_NOW) {
    // Alias-only: the owner's normal teardown under a live game, one pass.
    size_t remaining = 0;
    if (!sm_fakelib_adopted_release(0, &remaining))
      log_debug("[ADOPT] teardown retained %zu layer(s) of %s: %s", remaining,
                record->title_id, strerror(errno));
    sm_session_record_stop();
    free(record);
    return SM_ADOPT_STOPPED;
  }

  log_debug("[ADOPT] %s pid=%ld exited; removing the adopted layers",
            record->title_id, (long)pid);
  size_t remaining = release_fakelib(record->title_id);
  pid_t replacement = wait_for_finalize(record, pid, exit_us);
  if (replacement > 0) {
    log_debug("[ADOPT] %s restarted as pid=%ld (same-application "
              "replacement); it runs without the adopted layers",
              record->title_id, (long)replacement);
    if (record->redirect_count > 0) {
      log_debug("[ADOPT] NSFS redirects of %s kept for pid=%ld",
                record->title_id, (long)replacement);
      sm_pkg_backport_adopted_drop();
      remaining += record->redirect_count;
    }
  } else if (record->redirect_count > 0) {
    remaining += release_redirects(record->title_id);
  }
  uint64_t took_us = elapsed_us(exit_us);
  log_debug("[ADOPT] released %s: %zu of %zu layer(s) removed in %llu ms",
            record->title_id, units - (remaining < units ? remaining : units),
            units, (unsigned long long)(took_us / 1000ull));
  // An empty session removes the record; retained layers stay recorded.
  sm_session_record_stop();
  free(record);
  return deferred ? SM_ADOPT_STOPPED : SM_ADOPT_RELEASED;
}
