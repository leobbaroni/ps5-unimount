#include "sm_platform.h"

#include <arpa/inet.h>
#include <json-c/json.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "sm_config_mount.h"
#include "sm_filesystem.h"
#include "sm_game_cache.h"
#include "sm_game_lifecycle.h"
#include "sm_kraken.h"
#include "sm_kraken_auto.h"
#include "sm_kraken_compat.h"
#include "sm_log.h"
#include "sm_path_utils.h"
#include "sm_pkg.h"
#include "sm_runtime.h"
#include "sm_time.h"
#include "sm_types.h"
#include "sm_unimount.h"
#include "sm_unimount_logic.h"

#define COMPAT_BIND_RETRY_US 10000000ull
#define COMPAT_IDLE_POLL_MS 500
#define COMPAT_POLL_SLICE_MS 100
#define COMPAT_REQUEST_DEADLINE_US 2000000ull
#define COMPAT_REPLY_DEADLINE_US 2000000ull
#define COMPAT_MAX_AUTO_ENTRIES 64u
#define COMPAT_CACHE_PATH "/data/shadowmount/kraken"

static struct {
  pthread_mutex_t mutex;
  pthread_t thread;
  bool started;
  bool enabled;
  unsigned port;
  bool bound;
} g_compat = {.mutex = PTHREAD_MUTEX_INITIALIZER};

static atomic_bool g_compat_stop = false;

static void set_status(bool enabled, unsigned port, bool bound) {
  pthread_mutex_lock(&g_compat.mutex);
  g_compat.enabled = enabled;
  g_compat.port = port;
  g_compat.bound = bound;
  pthread_mutex_unlock(&g_compat.mutex);
}

void sm_kraken_compat_status(bool *enabled_out, unsigned *port_out,
                             bool *bound_out) {
  pthread_mutex_lock(&g_compat.mutex);
  if (!g_compat.started) {
    // Report the configuration before the listener thread exists.
    runtime_config_t cfg = runtime_config();
    g_compat.enabled = cfg.compat_kraken_api_enabled;
    g_compat.port = cfg.compat_kraken_api_port;
  }
  *enabled_out = g_compat.enabled;
  *port_out = g_compat.port;
  *bound_out = g_compat.bound;
  pthread_mutex_unlock(&g_compat.mutex);
}

static int open_listener(unsigned port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  int one = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons((uint16_t)port);
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
      listen(fd, 8) != 0 || !sm_set_fd_nonblocking(fd)) {
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return -1;
  }
  return fd;
}

static bool compat_stopping(void) {
  return atomic_load(&g_compat_stop) || should_stop_requested();
}

// The socket is non-blocking: a slow reader cannot hold the single listener
// thread past the deadline or delay shutdown.
static bool send_all(int fd, const char *data, size_t length,
                     uint64_t deadline) {
  while (length > 0) {
    if (compat_stopping() || monotonic_time_us() >= deadline)
      return false;
    ssize_t sent = send(fd, data, length, 0);
    if (sent < 0 && errno == EINTR)
      continue;
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      struct pollfd pfd = {.fd = fd, .events = POLLOUT};
      (void)poll(&pfd, 1, COMPAT_POLL_SLICE_MS);
      continue;
    }
    if (sent <= 0)
      return false;
    data += sent;
    length -= (size_t)sent;
  }
  return true;
}

static const char *reason_phrase(int code) {
  switch (code) {
  case 200: return "OK";
  case 204: return "No Content";
  case 302: return "Found";
  case 400: return "Bad Request";
  case 404: return "Not Found";
  case 409: return "Conflict";
  case 413: return "Content Too Large";
  case 431: return "Request Header Fields Too Large";
  default: return "Error";
  }
}

// Same framing as Kraken Mount's web.rs: Connection: close, no-store, no
// Content-Type/Length for 204.
static void send_reply(int fd, int code, const char *content_type,
                       const char *body, size_t body_len,
                       const char *location) {
  char head[512];
  int written;
  if (code == 204) {
    written = snprintf(head, sizeof(head),
                       "HTTP/1.1 204 No Content\r\nConnection: close\r\n"
                       "Cache-Control: no-store\r\n\r\n");
    body_len = 0;
  } else {
    written = snprintf(head, sizeof(head),
                       "HTTP/1.1 %d %s\r\nConnection: close\r\n"
                       "Content-Type: %s\r\nContent-Length: %zu\r\n"
                       "%s%s%sCache-Control: no-store\r\n\r\n",
                       code, reason_phrase(code), content_type, body_len,
                       location ? "Location: " : "", location ? location : "",
                       location ? "\r\n" : "");
  }
  if (written <= 0 || (size_t)written >= sizeof(head))
    return;
  uint64_t deadline = monotonic_time_us() + COMPAT_REPLY_DEADLINE_US;
  if (send_all(fd, head, (size_t)written, deadline) && body_len > 0)
    (void)send_all(fd, body, body_len, deadline);
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

static bool add_null(struct json_object *object, const char *key) {
  return json_object_object_add(object, key, NULL) == 0;
}

static bool mounted_at(const char *path, struct statfs *fs) {
  return statfs(path, fs) == 0 && strcmp(fs->f_mntonname, path) == 0;
}

static void drive_of(const char *path, char drive[32]) {
  (void)strlcpy(drive, "/data", 32);
  size_t best = 0;
  for (int i = 0; i < 10; ++i) {
    char mount[16];
    snprintf(mount, sizeof(mount), i < 8 ? "/mnt/usb%d" : "/mnt/ext%d",
             i < 8 ? i : i - 8);
    size_t length = strlen(mount);
    if (length > best && path_matches_root_or_child(path, mount)) {
      (void)strlcpy(drive, mount, 32);
      best = length;
    }
  }
}

static bool add_drive(struct json_object *drives, const char *mount,
                      const char *label, const char *kind,
                      const struct statfs *fs) {
  struct json_object *drive = json_object_new_object();
  uint64_t total = fs ? (uint64_t)fs->f_blocks * (uint64_t)fs->f_bsize : 0;
  uint64_t free_bytes =
      fs && fs->f_bavail > 0 ? (uint64_t)fs->f_bavail * (uint64_t)fs->f_bsize : 0;
  bool ejectable = strcmp(mount, "/mnt/ext0") == 0 ||
                   strncmp(mount, "/mnt/usb", 8) == 0;
  bool ok = drive && add_value(drive, "mount", json_object_new_string(mount)) &&
            add_value(drive, "label", json_object_new_string(label)) &&
            add_value(drive, "kind", json_object_new_string(kind)) &&
            add_value(drive, "total", json_object_new_int64((int64_t)total)) &&
            add_value(drive, "free", json_object_new_int64((int64_t)free_bytes)) &&
            add_value(drive, "ejectable", json_object_new_boolean(ejectable)) &&
            json_object_array_add(drives, drive) == 0;
  if (!ok && drive)
    json_object_put(drive);
  return ok;
}

static bool add_drives(struct json_object *root, uint64_t *data_free_out) {
  struct json_object *drives = json_object_new_array();
  if (!add_value(root, "drives", drives))
    return false;
  struct statfs fs;
  bool have_data = statfs("/data", &fs) == 0;
  *data_free_out = have_data && fs.f_bavail > 0
                       ? (uint64_t)fs.f_bavail * (uint64_t)fs.f_bsize : 0;
  if (!add_drive(drives, "/data", "Internal", "internal", have_data ? &fs : NULL))
    return false;
  for (int i = 0; i < 8; ++i) {
    char mount[16], label[16];
    snprintf(mount, sizeof(mount), "/mnt/usb%d", i);
    snprintf(label, sizeof(label), "USB %d", i + 1);
    if (mounted_at(mount, &fs) && !add_drive(drives, mount, label, "usb", &fs))
      return false;
  }
  for (int i = 0; i < 2; ++i) {
    char mount[16], label[16];
    snprintf(mount, sizeof(mount), "/mnt/ext%d", i);
    snprintf(label, sizeof(label), "Extended %d", i + 1);
    if (mounted_at(mount, &fs) && !add_drive(drives, mount, label, "ext", &fs))
      return false;
  }
  return true;
}

// One entry of Kraken's games[] with identical value types.
static bool add_game(struct json_object *games, const char *title_id,
                     const char *title_name, const char *version,
                     const char *path, uint64_t size, const char *status,
                     const char *error, bool playgo_off) {
  char drive[32];
  drive_of(path, drive);
  struct json_object *game = json_object_new_object();
  bool ok = game &&
      add_value(game, "title_id", json_object_new_string(title_id)) &&
      add_value(game, "title_name", json_object_new_string(title_name)) &&
      add_value(game, "version", json_object_new_string(version)) &&
      add_value(game, "path", json_object_new_string(path)) &&
      add_value(game, "kind", json_object_new_string("pkg")) &&
      add_null(game, "packing") &&
      add_value(game, "drive", json_object_new_string(drive)) &&
      add_value(game, "size", json_object_new_int64((int64_t)size)) &&
      add_value(game, "status", json_object_new_string(status)) &&
      (error && error[0] ? add_value(game, "error", json_object_new_string(error))
                         : add_null(game, "error")) &&
      add_value(game, "cache_size", json_object_new_int64(0)) &&
      add_null(game, "cache_built") && add_null(game, "backport") &&
      add_null(game, "playgo") &&
      add_value(game, "playgo_off", json_object_new_boolean(playgo_off)) &&
      json_object_array_add(games, game) == 0;
  if (!ok && game)
    json_object_put(game);
  return ok;
}

static bool add_games(struct json_object *root) {
  struct json_object *games = json_object_new_array();
  if (!add_value(root, "games", games))
    return false;
  const runtime_config_t cfg = runtime_config();
  sm_game_cache_snapshot_entry_t *entries = NULL;
  size_t count = 0;
  if (!sm_game_cache_snapshot(&entries, &count))
    return false;
  // Library titles backed by a Kraken PKG cache are "ready".
  char (*listed)[MAX_PATH] = count ? calloc(count, MAX_PATH) : NULL;
  size_t listed_count = 0;
  bool ok = count == 0 || listed != NULL;
  for (size_t i = 0; ok && i < count; ++i) {
    char package[MAX_PATH];
    if (!sm_kraken_is_cache(entries[i].path) ||
        !sm_kraken_source(entries[i].path, package))
      continue;
    sm_pkg_info_t info;
    memset(&info, 0, sizeof(info));
    (void)sm_pkg_read_directory_metadata(entries[i].path, &info);
    struct stat st;
    uint64_t size = stat(package, &st) == 0 ? (uint64_t)st.st_size : 0;
    ok = add_game(games, entries[i].title_id,
                  entries[i].title_name[0] ? entries[i].title_name
                                           : entries[i].title_id,
                  info.app_ver, package, size, "ready", NULL,
                  sm_config_title_playgo_stub_disabled(&cfg, entries[i].title_id));
    (void)strlcpy(listed[listed_count++], package, MAX_PATH);
  }
  free(entries);

  // Then .pkg files the automatic mount has queued, is building or failed.
  sm_kraken_auto_entry_t *pending =
      ok ? calloc(COMPAT_MAX_AUTO_ENTRIES, sizeof(*pending)) : NULL;
  size_t pending_count =
      pending ? sm_kraken_auto_snapshot(pending, COMPAT_MAX_AUTO_ENTRIES) : 0;
  ok = ok && pending != NULL;
  for (size_t i = 0; ok && i < pending_count; ++i) {
    bool duplicate_path = false;
    for (size_t j = 0; j < listed_count && !duplicate_path; ++j)
      duplicate_path = strcmp(listed[j], pending[i].path) == 0;
    if (duplicate_path)
      continue;
    ok = add_game(games, pending[i].title_id, pending[i].title_name,
                  pending[i].version, pending[i].path, pending[i].size,
                  pending[i].status, pending[i].error,
                  sm_config_title_playgo_stub_disabled(&cfg, pending[i].title_id));
  }
  free(pending);
  free(listed);
  return ok;
}

static char *build_state_json(size_t *length_out) {
  struct json_object *root = json_object_new_object();
  if (!root)
    return NULL;
  char running_title[MAX_TITLE_ID] = "";
  bool game_active = sm_game_lifecycle_has_active_game();
  (void)sm_game_lifecycle_background_title(running_title);
  struct json_object *running = json_object_new_array();
  struct json_object *cache = NULL;
  uint64_t data_free = 0;
  bool ok =
      add_value(root, "version", json_object_new_string(KRAKEN_MOUNT_RELEASE)) &&
      add_value(root, "busy", json_object_new_boolean(sm_kraken_auto_busy())) &&
      add_value(root, "paused",
                json_object_new_boolean(sm_kraken_auto_game_active())) &&
      add_null(root, "waiting") &&
      add_value(root, "running", running) &&
      (!game_active || !running_title[0] ||
       json_object_array_add(running, json_object_new_string(running_title)) == 0) &&
      add_games(root) && add_drives(root, &data_free) &&
      add_value(root, "cache", cache = json_object_new_object()) &&
      add_value(cache, "path", json_object_new_string(COMPAT_CACHE_PATH)) &&
      add_value(cache, "size", json_object_new_int64(0)) &&
      add_value(cache, "free", json_object_new_int64((int64_t)data_free)) &&
      add_value(cache, "low_space", json_object_new_boolean(false)) &&
      add_value(cache, "ampr_present", json_object_new_boolean(true)) &&
      add_value(root, "log", json_object_new_array()) &&
      add_value(root, "unimount", json_object_new_string(UNIMOUNT_VERSION));
  char *text = NULL;
  if (ok) {
    size_t length = 0;
    const char *json = json_object_to_json_string_length(
        root, JSON_C_TO_STRING_PLAIN, &length);
    text = json ? malloc(length + 1u) : NULL;
    if (text) {
      memcpy(text, json, length + 1u);
      *length_out = length;
    }
  }
  json_object_put(root);
  return text;
}

static void handle_connection(int fd, const struct sockaddr_in *peer) {
  if (!sm_set_fd_nonblocking(fd))
    return;

  char *buffer = malloc(SM_COMPAT_MAX_REQUEST + 1u);
  if (!buffer)
    return;
  size_t length = 0;
  sm_http_request_head_t head;
  int scan = 0;
  uint64_t deadline = monotonic_time_us() + COMPAT_REQUEST_DEADLINE_US;
  while (scan == 0) {
    if (length >= SM_COMPAT_MAX_REQUEST) {
      scan = -431;
      break;
    }
    if (compat_stopping() || monotonic_time_us() >= deadline)
      break;
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int ready = poll(&pfd, 1, COMPAT_POLL_SLICE_MS);
    if (ready < 0 && errno != EINTR)
      break;
    if (ready <= 0)
      continue;
    ssize_t count = recv(fd, buffer + length, SM_COMPAT_MAX_REQUEST - length, 0);
    if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
      continue;
    if (count <= 0)
      break;
    length += (size_t)count;
    scan = sm_http_request_scan(buffer, length, SM_COMPAT_MAX_REQUEST, &head);
  }
  free(buffer);
  if (scan == 0)
    return;  // client went away or timed out
  if (scan < 0) {
    static const char text[] = "bad or too large request";
    send_reply(fd, -scan, "text/plain", text, sizeof(text) - 1u, NULL);
    return;
  }

  switch (sm_compat_route(head.method, head.path)) {
  case SM_COMPAT_STATE: {
    size_t body_len = 0;
    char *body = build_state_json(&body_len);
    if (body) {
      send_reply(fd, 200, "application/json", body, body_len, NULL);
      free(body);
    } else {
      static const char text[] = "state unavailable";
      send_reply(fd, 500, "text/plain", text, sizeof(text) - 1u, NULL);
    }
    break;
  }
  case SM_COMPAT_RESCAN:
    (void)request_scan_now("Kraken-compatible API rescan");
    send_reply(fd, 204, NULL, NULL, 0, NULL);
    break;
  case SM_COMPAT_QUIT: {
    // A Kraken Mount started later asks the port owner to quit; refusing
    // makes it log "port 8097 busy" and stay off.
    static const char text[] =
        "UniMount owns game mounting on this console; Kraken Mount must not "
        "run alongside it";
    char peer_text[INET_ADDRSTRLEN] = "?";
    (void)inet_ntop(AF_INET, &peer->sin_addr, peer_text, sizeof(peer_text));
    log_debug("  [COMPAT] refused POST /api/quit from %s", peer_text);
    send_reply(fd, 409, "text/plain", text, sizeof(text) - 1u, NULL);
    break;
  }
  case SM_COMPAT_ROOT: {
    char location[192];
    runtime_config_t cfg = runtime_config();
    if (!sm_compat_redirect_location(head.host, cfg.api_port, location,
                                     sizeof(location)))
      (void)strlcpy(location, "/", sizeof(location));
    send_reply(fd, 302, "text/plain", "", 0, location);
    break;
  }
  case SM_COMPAT_NOT_FOUND:
  default: {
    static const char text[] = "not found";
    send_reply(fd, 404, "text/plain", text, sizeof(text) - 1u, NULL);
    break;
  }
  }
}

static void *compat_thread_main(void *unused) {
  (void)unused;
  int listen_fd = -1;
  unsigned bound_port = 0;
  int last_error = 0;
  uint64_t next_bind_us = 0;
  while (!atomic_load(&g_compat_stop) && !should_stop_requested()) {
    runtime_config_t cfg = runtime_config();
    unsigned port = cfg.compat_kraken_api_port;
    bool want = cfg.compat_kraken_api_enabled && !runtime_sleep_mode_active();
    if (listen_fd >= 0 && (!want || port != bound_port)) {
      close(listen_fd);
      listen_fd = -1;
      log_debug("  [COMPAT] Kraken-compatible API on port %u closed", bound_port);
    }
    uint64_t now = monotonic_time_us();
    if (want && listen_fd < 0 && now >= next_bind_us) {
      listen_fd = open_listener(port);
      if (listen_fd < 0) {
        if (errno != last_error)
          log_debug("  [COMPAT] cannot bind port %u: %s; retrying every %llu s",
                    port, strerror(errno),
                    (unsigned long long)(COMPAT_BIND_RETRY_US / 1000000ull));
        last_error = errno;
        next_bind_us = now + COMPAT_BIND_RETRY_US;
      } else {
        bound_port = port;
        last_error = 0;
        log_debug("  [COMPAT] Kraken-compatible API listening on port %u", port);
      }
    }
    set_status(cfg.compat_kraken_api_enabled, port, listen_fd >= 0);
    if (listen_fd < 0) {
      usleep(COMPAT_IDLE_POLL_MS * 1000);
      continue;
    }
    struct pollfd pfd = {.fd = listen_fd, .events = POLLIN};
    int ready = poll(&pfd, 1, COMPAT_IDLE_POLL_MS);
    if (ready <= 0 || !(pfd.revents & POLLIN))
      continue;
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    memset(&peer, 0, sizeof(peer));
    int client = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
    if (client < 0)
      continue;
    handle_connection(client, &peer);
    close(client);
  }
  if (listen_fd >= 0)
    close(listen_fd);
  set_status(runtime_config().compat_kraken_api_enabled,
             runtime_config().compat_kraken_api_port, false);
  return NULL;
}

bool sm_kraken_compat_start(void) {
  pthread_mutex_lock(&g_compat.mutex);
  if (g_compat.started) {
    pthread_mutex_unlock(&g_compat.mutex);
    return true;
  }
  atomic_store(&g_compat_stop, false);
  int rc = pthread_create(&g_compat.thread, NULL, compat_thread_main, NULL);
  g_compat.started = rc == 0;
  pthread_mutex_unlock(&g_compat.mutex);
  if (rc != 0) {
    errno = rc;
    return false;
  }
  return true;
}

void sm_kraken_compat_stop(void) {
  atomic_store(&g_compat_stop, true);
  pthread_mutex_lock(&g_compat.mutex);
  bool started = g_compat.started;
  pthread_mutex_unlock(&g_compat.mutex);
  if (started)
    (void)pthread_join(g_compat.thread, NULL);
  pthread_mutex_lock(&g_compat.mutex);
  g_compat.started = false;
  pthread_mutex_unlock(&g_compat.mutex);
}
