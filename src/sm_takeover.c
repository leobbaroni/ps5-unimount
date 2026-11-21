#include "sm_platform.h"

#include <arpa/inet.h>
#include <json-c/json.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/time.h>

#include "sm_filesystem.h"
#include "sm_game_lifecycle.h"
#include "sm_kraken_auto.h"
#include "sm_kraken_compat.h"
#include "sm_log.h"
#include "sm_paths.h"
#include "sm_runtime.h"
#include "sm_takeover.h"
#include "sm_time.h"
#include "sm_unimount.h"
#include "sm_unimount_logic.h"

#define TAKEOVER_IO_TIMEOUT_MS 2000
#define TAKEOVER_PROBE_DEADLINE_US 4000000ull
#define TAKEOVER_MAX_REPLY (512u * 1024u)
#define TAKEOVER_EXIT_POLL_US 250000u
#define TAKEOVER_EXIT_SLOW_US 5000000ull
#define TAKEOVER_EXIT_MAX_US 15000000ull
#define TAKEOVER_RETRY_US 3000000u
#define TAKEOVER_UNKNOWN_MAX_RETRY_US 30000000u

typedef enum {
  TAKEOVER_PENDING = 0,
  TAKEOVER_ABSENT,
  TAKEOVER_TAKEN_OVER,
  TAKEOVER_WAITING_FOR_GAME,
  TAKEOVER_WAITING_FOR_QUIT,
  TAKEOVER_WAITING_FOR_REPLY,
} takeover_state_t;

static struct {
  pthread_mutex_t mutex;
  takeover_state_t state;
  char kraken_version[32];
  char detail[128];
  unsigned legacy_released;
  unsigned legacy_left;
  bool previous_unclean;
  long previous_pid;
  int64_t started;
  bool ledger_written;
} g_takeover = {.mutex = PTHREAD_MUTEX_INITIALIZER};

static atomic_bool g_ownership_ready = false;

static const char *takeover_state_name(takeover_state_t state) {
  switch (state) {
  case TAKEOVER_ABSENT:
    return "absent";
  case TAKEOVER_TAKEN_OVER:
    return "taken_over";
  case TAKEOVER_WAITING_FOR_GAME:
    return "waiting_for_game";
  case TAKEOVER_WAITING_FOR_QUIT:
    return "waiting_for_quit";
  case TAKEOVER_WAITING_FOR_REPLY:
    return "waiting_for_reply";
  case TAKEOVER_PENDING:
  default:
    return "pending";
  }
}

static void set_state(takeover_state_t state) {
  pthread_mutex_lock(&g_takeover.mutex);
  g_takeover.state = state;
  pthread_mutex_unlock(&g_takeover.mutex);
}

// Non-blocking connect bounded by TAKEOVER_IO_TIMEOUT_MS: a full listen
// backlog must not stall startup. errno is ECONNREFUSED only when nothing
// listens.
static int connect_loopback(uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return -1;
  }
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int error = 0;
  if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
    error = errno;
    if (error == EINPROGRESS || error == EINTR) {
      struct pollfd pfd = {.fd = fd, .events = POLLOUT};
      int ready;
      do
        ready = poll(&pfd, 1, TAKEOVER_IO_TIMEOUT_MS);
      while (ready < 0 && errno == EINTR);
      socklen_t error_len = sizeof(error);
      if (ready == 0)
        error = ETIMEDOUT;
      else if (ready < 0)
        error = errno;
      else if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_len) != 0)
        error = errno;
    }
  }
  if (error != 0) {
    close(fd);
    errno = error;
    return -1;
  }
  return fd;
}

static bool port_has_listener(uint16_t port) {
  int fd = connect_loopback(port);
  if (fd < 0)
    return errno != ECONNREFUSED;
  close(fd);
  return true;
}

typedef enum {
  LOOPBACK_REFUSED = 0,  // nothing listens
  LOOPBACK_FAILED,       // something may listen; connect or send failed
  LOOPBACK_REPLIED,      // *reply_out holds what arrived (possibly nothing)
} loopback_result_t;

// Sends request and keeps what the peer sends until it closes or resets, the
// TAKEOVER_MAX_REPLY cap or TAKEOVER_PROBE_DEADLINE_US. *reply_out (caller
// frees) is NUL-terminated.
static loopback_result_t http_loopback(uint16_t port, const char *request,
                                       char **reply_out, size_t *length_out,
                                       int *error_out) {
  *reply_out = NULL;
  *length_out = 0;
  *error_out = 0;
  int fd = connect_loopback(port);
  if (fd < 0) {
    *error_out = errno;
    return errno == ECONNREFUSED ? LOOPBACK_REFUSED : LOOPBACK_FAILED;
  }
  uint64_t deadline = monotonic_time_us() + TAKEOVER_PROBE_DEADLINE_US;
  size_t request_len = strlen(request);
  for (size_t sent = 0; sent < request_len;) {
    ssize_t count = send(fd, request + sent, request_len - sent, 0);
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
        monotonic_time_us() < deadline) {
      struct pollfd pfd = {.fd = fd, .events = POLLOUT};
      (void)poll(&pfd, 1, 100);
      continue;
    }
    if (count <= 0) {
      *error_out = count < 0 ? errno : EPIPE;
      close(fd);
      return LOOPBACK_FAILED;
    }
    sent += (size_t)count;
  }
  char *reply = malloc(TAKEOVER_MAX_REPLY + 1u);
  if (!reply) {
    *error_out = ENOMEM;
    close(fd);
    return LOOPBACK_FAILED;
  }
  size_t length = 0;
  while (length < TAKEOVER_MAX_REPLY && !should_stop_requested()) {
    uint64_t now = monotonic_time_us();
    if (now >= deadline) {
      *error_out = ETIMEDOUT;
      break;
    }
    uint64_t left_ms = (deadline - now + 999u) / 1000u;
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int ready = poll(&pfd, 1, left_ms > 250u ? 250 : (int)left_ms);
    if (ready < 0 && errno != EINTR) {
      *error_out = errno;
      break;
    }
    if (ready <= 0)
      continue;
    ssize_t count = recv(fd, reply + length, TAKEOVER_MAX_REPLY - length, 0);
    if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
      continue;
    if (count < 0)
      *error_out = errno;
    if (count <= 0)
      break;
    length += (size_t)count;
  }
  close(fd);
  reply[length] = '\0';
  *reply_out = reply;
  *length_out = length;
  return LOOPBACK_REPLIED;
}

static sm_peer_kind_t probe_kraken(char version[32], char *why,
                                   size_t why_size) {
  char *reply = NULL;
  size_t length = 0;
  int error = 0;
  loopback_result_t result =
      http_loopback((uint16_t)SM_KRAKEN_PORT,
                    "GET /api/state HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                    "Connection: close\r\n\r\n",
                    &reply, &length, &error);
  version[0] = '\0';
  sm_peer_kind_t kind;
  if (result == LOOPBACK_REFUSED) {
    kind = SM_PEER_NONE;
    (void)snprintf(why, why_size, "nothing listens");
  } else if (result == LOOPBACK_FAILED) {
    kind = SM_PEER_UNKNOWN;
    (void)snprintf(why, why_size, "request failed: %s", strerror(error));
  } else {
    const char *reason = NULL;
    kind = sm_takeover_classify_reply(reply, length, version, &reason);
    (void)snprintf(why, why_size, "%s; %zu bytes%s%s", reason ? reason : "?",
                   length, error ? ", " : "", error ? strerror(error) : "");
  }
  free(reply);
  return kind;
}

static sm_quit_outcome_t request_kraken_quit(int *status_out) {
  // A newer Kraken copy takes over exactly this way (web.rs take_port).
  char *reply = NULL;
  size_t length = 0;
  int error = 0;
  loopback_result_t result =
      http_loopback((uint16_t)SM_KRAKEN_PORT,
                    "POST /api/quit HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                    "Content-Length: 0\r\nConnection: close\r\n\r\n",
                    &reply, &length, &error);
  int status = -1;
  size_t body_offset = 0;
  if (result == LOOPBACK_REFUSED)
    status = 0;
  else if (result == LOOPBACK_REPLIED &&
           !sm_http_parse_response(reply, length, &status, &body_offset))
    status = -1;
  free(reply);
  *status_out = status;
  return sm_takeover_quit_outcome(status);
}

typedef enum {
  PORT_RELEASED = 0,
  PORT_STILL_LISTENING,
  PORT_WAIT_STOPPED,
} port_release_t;

static port_release_t wait_for_port_release(void) {
  uint64_t started = monotonic_time_us();
  bool slow_logged = false;
  for (;;) {
    if (!port_has_listener((uint16_t)SM_KRAKEN_PORT)) {
      log_debug("[TAKEOVER] Kraken Mount stopped; port %u released",
                SM_KRAKEN_PORT);
      return PORT_RELEASED;
    }
    uint64_t waited = monotonic_time_us() - started;
    if (!slow_logged && waited >= TAKEOVER_EXIT_SLOW_US) {
      log_debug("[TAKEOVER] Kraken Mount still listening after %llu s",
                (unsigned long long)(waited / 1000000ull));
      slow_logged = true;
    }
    if (waited >= TAKEOVER_EXIT_MAX_US) {
      log_debug("[TAKEOVER] port %u still has a listener after %llu s;"
                " probing again before any mount work",
                SM_KRAKEN_PORT,
                (unsigned long long)(TAKEOVER_EXIT_MAX_US / 1000000ull));
      return PORT_STILL_LISTENING;
    }
    if (sleep_with_stop_check(TAKEOVER_EXIT_POLL_US))
      return PORT_WAIT_STOPPED;
  }
}

// Collect the live sandbox state once per release pass.
static bool title_sandbox_live(const struct statfs *mounts, int count,
                               const char *title_id) {
  for (int i = 0; i < count; ++i)
    if (sm_takeover_sandbox_mount_for_title(mounts[i].f_mntonname, title_id))
      return true;
  return false;
}

static void release_legacy_mounts(bool startup) {
  struct statfs *mounts = NULL;
  int count = sm_mount_table_snapshot(&mounts);
  if (count < 0) {
    log_debug("[TAKEOVER] mount table unavailable: %s", strerror(errno));
    return;
  }
  unsigned released = 0, left = 0;
  bool game_active = sm_game_lifecycle_has_active_game();
  // Newest first: a layer stacked later must go before the one below it.
  for (int i = count - 1; i >= 0; --i) {
    char title_id[MAX_TITLE_ID];
    if (!sm_takeover_is_legacy_mount(mounts[i].f_fstypename,
                                     mounts[i].f_mntfromname,
                                     mounts[i].f_mntonname, title_id))
      continue;
    if (game_active || title_sandbox_live(mounts, count, title_id)) {
      log_debug("[TAKEOVER] legacy Kraken mount kept while a game is live: %s",
                mounts[i].f_mntonname);
      ++left;
      continue;
    }
    // Never MNT_FORCE: a busy mount stays and is retried on a later pass.
    if (unmount(mounts[i].f_mntonname, 0) == 0) {
      log_debug("[TAKEOVER] released legacy Kraken mount: %s (from %s)",
                mounts[i].f_mntonname, mounts[i].f_mntfromname);
      ++released;
    } else {
      log_debug("[TAKEOVER] legacy Kraken mount kept: %s (%s)",
                mounts[i].f_mntonname, strerror(errno));
      ++left;
    }
  }
  free(mounts);
  pthread_mutex_lock(&g_takeover.mutex);
  g_takeover.legacy_released += released;
  g_takeover.legacy_left = left;
  pthread_mutex_unlock(&g_takeover.mutex);
  if (startup || released || left)
    log_debug("[TAKEOVER] legacy Kraken mounts: released=%u left=%u", released,
              left);
}

static void set_detail(const char *detail) {
  pthread_mutex_lock(&g_takeover.mutex);
  (void)strlcpy(g_takeover.detail, detail ? detail : "",
                sizeof(g_takeover.detail));
  pthread_mutex_unlock(&g_takeover.mutex);
}

bool sm_takeover_run(void) {
  bool notified = false;
  bool contacted = false;  // Kraken Mount answered at least once
  unsigned unknown_retry_us = TAKEOVER_RETRY_US;
  char last_why[128] = "";
  for (;;) {
    if (should_stop_requested())
      return false;
    char version[32];
    char why[128];
    sm_peer_kind_t peer = probe_kraken(version, why, sizeof(why));
    if (peer == SM_PEER_UNKNOWN) {
      // Something listens but did not say what it is: Kraken Mount may be
      // slow (cache build, large library). Never treat that as absent.
      pthread_mutex_lock(&g_takeover.mutex);
      bool changed = g_takeover.state != TAKEOVER_WAITING_FOR_REPLY;
      g_takeover.state = TAKEOVER_WAITING_FOR_REPLY;
      (void)strlcpy(g_takeover.detail, why, sizeof(g_takeover.detail));
      pthread_mutex_unlock(&g_takeover.mutex);
      if (changed || strcmp(last_why, why) != 0)
        log_debug("[TAKEOVER] port %u has a listener but its reply is not "
                  "conclusive (%s); no scan or mount work until it is known, "
                  "asking again in %u s",
                  SM_KRAKEN_PORT, why, unknown_retry_us / 1000000u);
      (void)strlcpy(last_why, why, sizeof(last_why));
      if (!notified) {
        notify_system_l10n(SM_L10N_KRAKEN_TAKEOVER_WAITING);
        notified = true;
      }
      if (sleep_with_stop_check(unknown_retry_us))
        return false;
      unknown_retry_us = unknown_retry_us >= TAKEOVER_UNKNOWN_MAX_RETRY_US / 2u
                             ? TAKEOVER_UNKNOWN_MAX_RETRY_US
                             : unknown_retry_us * 2u;
      continue;
    }
    unknown_retry_us = TAKEOVER_RETRY_US;
    last_why[0] = '\0';
    if (peer != SM_PEER_KRAKEN) {
      pthread_mutex_lock(&g_takeover.mutex);
      g_takeover.state = contacted ? TAKEOVER_TAKEN_OVER : TAKEOVER_ABSENT;
      (void)strlcpy(g_takeover.detail, peer == SM_PEER_OTHER ? why : "",
                    sizeof(g_takeover.detail));
      pthread_mutex_unlock(&g_takeover.mutex);
      if (peer == SM_PEER_OTHER)
        log_debug("[TAKEOVER] port %u answers but is not Kraken Mount (%s); "
                  "nothing to take over", SM_KRAKEN_PORT, why);
      else if (peer == SM_PEER_UNIMOUNT)
        log_debug("[TAKEOVER] port %u is a UniMount listener", SM_KRAKEN_PORT);
      else if (contacted)
        log_debug("[TAKEOVER] Kraken Mount is gone");
      else
        log_debug("[TAKEOVER] Kraken Mount not running");
      break;
    }

    contacted = true;
    pthread_mutex_lock(&g_takeover.mutex);
    (void)strlcpy(g_takeover.kraken_version, version,
                  sizeof(g_takeover.kraken_version));
    pthread_mutex_unlock(&g_takeover.mutex);
    int status = 0;
    sm_quit_outcome_t outcome = request_kraken_quit(&status);
    if (outcome == SM_QUIT_ACCEPTED) {
      log_debug("[TAKEOVER] Kraken Mount %s accepted quit (HTTP %d)", version,
                status);
      port_release_t released = wait_for_port_release();
      if (released == PORT_WAIT_STOPPED)
        return false;
      if (released == PORT_RELEASED) {
        set_state(TAKEOVER_TAKEN_OVER);
        set_detail(NULL);
        break;
      }
      // Still listening: find out again what it is before mounting anything.
      set_state(TAKEOVER_WAITING_FOR_QUIT);
      set_detail("accepted quit but still listening");
      continue;
    }
    takeover_state_t waiting = outcome == SM_QUIT_GAME_RUNNING
                                   ? TAKEOVER_WAITING_FOR_GAME
                                   : TAKEOVER_WAITING_FOR_QUIT;
    pthread_mutex_lock(&g_takeover.mutex);
    bool changed = g_takeover.state != waiting;
    g_takeover.state = waiting;
    g_takeover.detail[0] = '\0';
    pthread_mutex_unlock(&g_takeover.mutex);
    if (changed)
      log_debug("[TAKEOVER] Kraken Mount %s refused quit (HTTP %d): %s; "
                "no scan or mount work until it stops",
                version, status,
                waiting == TAKEOVER_WAITING_FOR_GAME ? "a game is running"
                                                     : "unexpected answer");
    if (!notified) {
      notify_system_l10n(SM_L10N_KRAKEN_TAKEOVER_WAITING);
      notified = true;
    }
    if (sleep_with_stop_check(TAKEOVER_RETRY_US))
      return false;
  }
  release_legacy_mounts(true);
  atomic_store(&g_ownership_ready, true);
  return true;
}

bool sm_takeover_ownership_ready(void) {
  return atomic_load(&g_ownership_ready);
}

void sm_takeover_retry_legacy_mounts(void) {
  if (!sm_takeover_ownership_ready())
    return;
  pthread_mutex_lock(&g_takeover.mutex);
  unsigned left = g_takeover.legacy_left;
  pthread_mutex_unlock(&g_takeover.mutex);
  if (left > 0)
    release_legacy_mounts(false);
}

static bool title_path_exists(const char *root, const char *title_id,
                              const char *suffix) {
  char path[MAX_PATH];
  int written = snprintf(path, sizeof(path), "%s/%s%s", root, title_id, suffix);
  struct stat st;
  return written > 0 && (size_t)written < sizeof(path) && stat(path, &st) == 0;
}

static bool legacy_mount_present(const char *title_id) {
  struct statfs *mounts = NULL;
  int count = sm_mount_table_snapshot(&mounts);
  if (count < 0)
    return true;  // unknown: do not stack over it
  bool present = false;
  for (int i = 0; i < count && !present; ++i) {
    char mounted_tid[MAX_TITLE_ID];
    present = sm_takeover_is_legacy_mount(mounts[i].f_fstypename,
                                          mounts[i].f_mntfromname,
                                          mounts[i].f_mntonname, mounted_tid) &&
              strcmp(mounted_tid, title_id) == 0;
  }
  free(mounts);
  return present;
}

bool sm_takeover_kraken_adoptable(const char *title_id, bool *busy_out) {
  if (busy_out)
    *busy_out = false;
  if (!title_id || strlen(title_id) != 9u)
    return false;
  static const char *const ext_roots[] = {
      "/mnt/ext0/user/app", "/mnt/ext0/ps5/user/app", "/mnt/ext1/user/app",
      "/mnt/ext1/ps5/user/app"};
  sm_adopt_facts_t facts = {
      .user_param_json = title_path_exists(APP_BASE, title_id,
                                           "/sce_sys/param.json"),
      .mount_link = title_path_exists(APP_BASE, title_id, "/mount.lnk"),
      .user_app_pkg = title_path_exists(APP_BASE, title_id, "/app.pkg"),
      .kraken_cache_done = title_path_exists(SM_KRAKEN_LEGACY_CACHES, title_id,
                                             "/.done"),
  };
  for (size_t i = 0; i < sizeof(ext_roots) / sizeof(ext_roots[0]); ++i)
    facts.ext_installed = facts.ext_installed ||
                          title_path_exists(ext_roots[i], title_id, "/app.pkg");
  if (sm_takeover_adoption(&facts) == SM_ADOPT_NO)
    return false;
  facts.legacy_mount_present = legacy_mount_present(title_id);
  // Callers log: the automatic mount asks on every pass.
  sm_adopt_t adopt = sm_takeover_adoption(&facts);
  if (adopt == SM_ADOPT_LATER && busy_out)
    *busy_out = true;
  return adopt == SM_ADOPT_YES;
}

static int64_t boot_time_seconds(void) {
  int mib[2] = {CTL_KERN, KERN_BOOTTIME};
  struct timeval boot;
  size_t size = sizeof(boot);
  if (sysctl(mib, 2, &boot, &size, NULL, 0) != 0 || size != sizeof(boot))
    return 0;
  return (int64_t)boot.tv_sec;
}

int64_t sm_ownership_boot_time(void) {
  return boot_time_seconds();
}

void sm_ownership_begin(void) {
  int64_t boot = boot_time_seconds();
  int64_t now = (int64_t)time(NULL);
  int fd = open(OWNER_LEDGER_FILE, O_RDONLY | O_NOFOLLOW);
  if (fd >= 0) {
    char body[1024];
    ssize_t length = read(fd, body, sizeof(body) - 1u);
    close(fd);
    long pid = 0;
    int64_t previous_boot = 0;
    bool parsed = length > 0 &&
                  sm_owner_ledger_parse(body, (size_t)length, &pid, &previous_boot);
    pthread_mutex_lock(&g_takeover.mutex);
    g_takeover.previous_unclean = true;
    g_takeover.previous_pid = parsed ? pid : 0;
    pthread_mutex_unlock(&g_takeover.mutex);
    log_debug("[OWNER] previous instance did not exit cleanly: pid=%ld %s; "
              "startup recovery handles its mounts",
              parsed ? pid : 0L,
              !parsed ? "(unreadable ledger)"
              : previous_boot != 0 && previous_boot != boot ? "(previous boot)"
                                                            : "(this boot)");
  }

  char ledger[512];
  pthread_mutex_lock(&g_takeover.mutex);
  g_takeover.started = now;
  pthread_mutex_unlock(&g_takeover.mutex);
  if (!sm_owner_ledger_format(ledger, sizeof(ledger), UNIMOUNT_PRODUCT,
                              UNIMOUNT_VERSION, (long)getpid(), now, boot)) {
    log_debug("[OWNER] ledger format failed");
    return;
  }
  char temporary[] = OWNER_LEDGER_FILE ".XXXXXX";
  fd = mkstemp(temporary);
  if (fd < 0) {
    log_debug("[OWNER] cannot write %s: %s", OWNER_LEDGER_FILE, strerror(errno));
    return;
  }
  size_t length = strlen(ledger);
  bool written = write(fd, ledger, length) == (ssize_t)length && fsync(fd) == 0;
  written = close(fd) == 0 && written;
  if (!written || rename(temporary, OWNER_LEDGER_FILE) != 0) {
    log_debug("[OWNER] cannot write %s: %s", OWNER_LEDGER_FILE, strerror(errno));
    (void)unlink(temporary);
    return;
  }
  pthread_mutex_lock(&g_takeover.mutex);
  g_takeover.ledger_written = true;
  pthread_mutex_unlock(&g_takeover.mutex);
  log_debug("[OWNER] %s %s owns mounting (pid=%ld)", UNIMOUNT_PRODUCT,
            UNIMOUNT_VERSION, (long)getpid());
}

void sm_ownership_end(void) {
  pthread_mutex_lock(&g_takeover.mutex);
  bool written = g_takeover.ledger_written;
  g_takeover.ledger_written = false;
  pthread_mutex_unlock(&g_takeover.mutex);
  if (written && unlink(OWNER_LEDGER_FILE) != 0 && errno != ENOENT)
    log_debug("[OWNER] cannot remove %s: %s", OWNER_LEDGER_FILE, strerror(errno));
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

bool sm_ownership_add_json(struct json_object *response) {
  pthread_mutex_lock(&g_takeover.mutex);
  takeover_state_t state = g_takeover.state;
  char kraken_version[32];
  (void)strlcpy(kraken_version, g_takeover.kraken_version, sizeof(kraken_version));
  char detail[sizeof(g_takeover.detail)];
  (void)strlcpy(detail, g_takeover.detail, sizeof(detail));
  unsigned released = g_takeover.legacy_released;
  unsigned left = g_takeover.legacy_left;
  bool previous_unclean = g_takeover.previous_unclean;
  long previous_pid = g_takeover.previous_pid;
  int64_t started = g_takeover.started;
  pthread_mutex_unlock(&g_takeover.mutex);

  bool compat_enabled = false, compat_bound = false;
  unsigned compat_port = 0;
  sm_kraken_compat_status(&compat_enabled, &compat_port, &compat_bound);

  // Each child object is owned by the response as soon as it is attached.
  struct json_object *kraken = NULL, *legacy = NULL, *compat = NULL,
                     *automount = NULL;
  return add_value(response, "product", json_object_new_string(UNIMOUNT_PRODUCT)) &&
         add_value(response, "version", json_object_new_string(UNIMOUNT_VERSION)) &&
         add_value(response, "pid", json_object_new_int64((int64_t)getpid())) &&
         add_value(response, "started", json_object_new_int64(started)) &&
         add_value(response, "ownership_ready",
                   json_object_new_boolean(sm_takeover_ownership_ready())) &&
         add_value(response, "previous_unclean",
                   json_object_new_boolean(previous_unclean)) &&
         add_value(response, "previous_pid", json_object_new_int64(previous_pid)) &&
         add_value(response, "kraken_takeover", kraken = json_object_new_object()) &&
         add_value(kraken, "state",
                   json_object_new_string(takeover_state_name(state))) &&
         (kraken_version[0]
              ? add_value(kraken, "version", json_object_new_string(kraken_version))
              : json_object_object_add(kraken, "version", NULL) == 0) &&
         (detail[0]
              ? add_value(kraken, "detail", json_object_new_string(detail))
              : json_object_object_add(kraken, "detail", NULL) == 0) &&
         add_value(response, "legacy_mounts", legacy = json_object_new_object()) &&
         add_value(legacy, "released", json_object_new_int64(released)) &&
         add_value(legacy, "left", json_object_new_int64(left)) &&
         add_value(response, "compat_listener", compat = json_object_new_object()) &&
         add_value(compat, "enabled", json_object_new_boolean(compat_enabled)) &&
         add_value(compat, "port", json_object_new_int64(compat_port)) &&
         add_value(compat, "bound", json_object_new_boolean(compat_bound)) &&
         add_value(response, "auto_mount", automount = json_object_new_object()) &&
         sm_kraken_auto_add_status_json(automount);
}
