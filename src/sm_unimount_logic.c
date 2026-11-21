// Pure UniMount decision helpers. Keep this file free of console-only
// headers: tests/test_unimount_logic.c compiles it on the host.
#include "sm_unimount_logic.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <json-c/json.h>

static bool copy_text(char *out, size_t size, const char *text) {
  int written = snprintf(out, size, "%s", text ? text : "");
  return written >= 0 && (size_t)written < size;
}

static bool valid_title_id(const char *text, size_t length) {
  if (length != 9u)
    return false;
  for (size_t i = 0; i < 4u; ++i)
    if (text[i] < 'A' || text[i] > 'Z')
      return false;
  for (size_t i = 4u; i < 9u; ++i)
    if (text[i] < '0' || text[i] > '9')
      return false;
  return strncmp(text, "NPXS", 4u) != 0;
}

static const unsigned char *find_bytes(const unsigned char *data, size_t length,
                                       const char *needle, size_t needle_len) {
  if (needle_len == 0 || length < needle_len)
    return NULL;
  for (size_t i = 0; i + needle_len <= length; ++i) {
    if (data[i] == (unsigned char)needle[0] &&
        memcmp(data + i, needle, needle_len) == 0)
      return data + i;
  }
  return NULL;
}

// --- Takeover ---

sm_peer_kind_t sm_takeover_classify_state(int http_status, const char *body,
                                          size_t body_len) {
  if (http_status <= 0)
    return SM_PEER_NONE;
  if (http_status != 200 || !body || body_len == 0)
    return SM_PEER_OTHER;
  struct json_tokener *tokener = json_tokener_new();
  if (!tokener)
    return SM_PEER_OTHER;
  struct json_object *root =
      json_tokener_parse_ex(tokener, body, (int)(body_len > INT32_MAX
                                                     ? INT32_MAX
                                                     : body_len));
  json_tokener_free(tokener);
  sm_peer_kind_t kind = SM_PEER_OTHER;
  struct json_object *value;
  if (root && json_object_is_type(root, json_type_object)) {
    if (json_object_object_get_ex(root, "unimount", &value))
      kind = SM_PEER_UNIMOUNT;
    else if (json_object_object_get_ex(root, "version", &value) &&
             json_object_is_type(value, json_type_string) &&
             json_object_object_get_ex(root, "games", &value) &&
             json_object_is_type(value, json_type_array))
      kind = SM_PEER_KRAKEN;
  }
  if (root)
    json_object_put(root);
  return kind;
}

sm_quit_outcome_t sm_takeover_quit_outcome(int http_status) {
  if (http_status >= 200 && http_status < 300)
    return SM_QUIT_ACCEPTED;
  if (http_status == 409)
    return SM_QUIT_GAME_RUNNING;
  return SM_QUIT_REFUSED;
}

bool sm_http_parse_response(const char *buf, size_t len, int *status_out,
                            size_t *body_offset_out) {
  *status_out = 0;
  *body_offset_out = 0;
  if (!buf)
    return false;
  size_t head_end = 0;
  for (size_t i = 0; i + 3u < len; ++i) {
    if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
        buf[i + 3] == '\n') {
      head_end = i + 4u;
      break;
    }
  }
  if (head_end == 0 || len < 12u || strncmp(buf, "HTTP/1.", 7) != 0 ||
      (buf[7] != '0' && buf[7] != '1') || buf[8] != ' ')
    return false;
  int status = 0;
  for (size_t i = 9; i < 12u; ++i) {
    if (!isdigit((unsigned char)buf[i]))
      return false;
    status = status * 10 + (buf[i] - '0');
  }
  if (buf[12] != ' ' && buf[12] != '\r')
    return false;
  *status_out = status;
  *body_offset_out = head_end;
  return true;
}

bool sm_takeover_is_legacy_mount(const char *fstype, const char *from,
                                 const char *on, char title_id[MAX_TITLE_ID]) {
  static const char source_prefix[] = SM_KRAKEN_LEGACY_CACHES "/";
  static const char target_prefix[] = "/system_ex/app/";
  if (title_id)
    title_id[0] = '\0';
  if (!fstype || !from || !on || strcmp(fstype, "nullfs") != 0 ||
      strncmp(from, source_prefix, sizeof(source_prefix) - 1u) != 0 ||
      strncmp(on, target_prefix, sizeof(target_prefix) - 1u) != 0)
    return false;
  const char *source_tid = from + sizeof(source_prefix) - 1u;
  const char *target_tid = on + sizeof(target_prefix) - 1u;
  size_t length = strlen(source_tid);
  if (!valid_title_id(source_tid, length) || strcmp(source_tid, target_tid) != 0)
    return false;
  if (title_id)
    (void)copy_text(title_id, MAX_TITLE_ID, source_tid);
  return true;
}

bool sm_takeover_sandbox_mount_for_title(const char *mount_point,
                                         const char *title_id) {
  static const char prefix[] = "/mnt/sandbox/";
  if (!mount_point || !title_id || !valid_title_id(title_id, strlen(title_id)) ||
      strncmp(mount_point, prefix, sizeof(prefix) - 1u) != 0)
    return false;
  const char *name = mount_point + sizeof(prefix) - 1u;
  // Sandbox directories are <TID>_<NNN>; the bare <TID> is not a sandbox.
  return strncmp(name, title_id, 9u) == 0 && name[9] == '_' &&
         isdigit((unsigned char)name[10]);
}

sm_adopt_t sm_takeover_adoption(const sm_adopt_facts_t *facts) {
  if (!facts || !facts->user_param_json || facts->mount_link ||
      facts->user_app_pkg || facts->ext_installed || !facts->kraken_cache_done)
    return SM_ADOPT_NO;
  return facts->legacy_mount_present ? SM_ADOPT_LATER : SM_ADOPT_YES;
}

// --- Ownership ledger ---

bool sm_owner_ledger_format(char *out, size_t out_size, const char *product,
                            const char *version, long pid, int64_t started,
                            int64_t boot_time) {
  struct json_object *root = json_object_new_object();
  if (!root)
    return false;
  bool ok =
      json_object_object_add(root, "product",
                             json_object_new_string(product ? product : "")) == 0 &&
      json_object_object_add(root, "version",
                             json_object_new_string(version ? version : "")) == 0 &&
      json_object_object_add(root, "pid", json_object_new_int64(pid)) == 0 &&
      json_object_object_add(root, "started", json_object_new_int64(started)) == 0 &&
      json_object_object_add(root, "boot_time", json_object_new_int64(boot_time)) == 0;
  ok = ok && copy_text(out, out_size,
                       json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN));
  json_object_put(root);
  return ok;
}

bool sm_owner_ledger_parse(const char *body, size_t len, long *pid_out,
                           int64_t *boot_time_out) {
  *pid_out = 0;
  *boot_time_out = 0;
  if (!body || len == 0)
    return false;
  struct json_tokener *tokener = json_tokener_new();
  if (!tokener)
    return false;
  struct json_object *root = json_tokener_parse_ex(
      tokener, body, (int)(len > INT32_MAX ? INT32_MAX : len));
  json_tokener_free(tokener);
  struct json_object *product, *pid, *boot;
  bool ok = root && json_object_is_type(root, json_type_object) &&
            json_object_object_get_ex(root, "product", &product) &&
            json_object_is_type(product, json_type_string) &&
            json_object_object_get_ex(root, "pid", &pid) &&
            json_object_is_type(pid, json_type_int);
  if (ok) {
    *pid_out = (long)json_object_get_int64(pid);
    if (json_object_object_get_ex(root, "boot_time", &boot) &&
        json_object_is_type(boot, json_type_int))
      *boot_time_out = json_object_get_int64(boot);
  }
  if (root)
    json_object_put(root);
  return ok;
}

// --- Kraken-compatible listener ---

static bool header_name_is(const char *line, size_t length, const char *name) {
  size_t name_len = strlen(name);
  return length > name_len && line[name_len] == ':' &&
         strncasecmp(line, name, name_len) == 0;
}

int sm_http_request_scan(const char *buf, size_t len, size_t cap,
                         sm_http_request_head_t *out) {
  memset(out, 0, sizeof(*out));
  size_t head_end = 0;
  for (size_t i = 0; i + 3u < len; ++i) {
    if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
        buf[i + 3] == '\n') {
      head_end = i + 4u;
      break;
    }
  }
  if (head_end == 0)
    return len >= cap ? -431 : 0;

  // Request line: METHOD SP TARGET SP HTTP/1.x
  const char *line_end = memchr(buf, '\r', head_end);
  const char *space = memchr(buf, ' ', (size_t)(line_end - buf));
  if (!space || space == buf || (size_t)(space - buf) >= sizeof(out->method))
    return -400;
  memcpy(out->method, buf, (size_t)(space - buf));
  const char *target = space + 1;
  const char *target_end = memchr(target, ' ', (size_t)(line_end - target));
  if (!target_end || target_end == target ||
      strncmp(target_end + 1, "HTTP/1.", 7) != 0)
    return -400;
  const char *query = memchr(target, '?', (size_t)(target_end - target));
  const char *path_end = query ? query : target_end;
  if ((size_t)(path_end - target) >= sizeof(out->path))
    return -400;
  memcpy(out->path, target, (size_t)(path_end - target));

  bool have_length = false;
  const char *line = line_end + 2;
  while (line < buf + head_end - 2) {
    const char *end = memchr(line, '\r', (size_t)(buf + head_end - line));
    if (!end)
      return -400;
    size_t length = (size_t)(end - line);
    if (header_name_is(line, length, "Content-Length")) {
      const char *value = line + strlen("Content-Length") + 1;
      while (value < end && (*value == ' ' || *value == '\t'))
        ++value;
      if (value == end || have_length)
        return -400;
      uint64_t parsed = 0;
      for (; value < end && *value != ' ' && *value != '\t'; ++value) {
        if (!isdigit((unsigned char)*value) || parsed > (UINT64_MAX - 9u) / 10u)
          return -400;
        parsed = parsed * 10u + (uint64_t)(*value - '0');
      }
      if (parsed > cap)
        return -413;
      out->content_length = (size_t)parsed;
      have_length = true;
    } else if (header_name_is(line, length, "Transfer-Encoding")) {
      return -400;
    } else if (header_name_is(line, length, "Host")) {
      const char *value = line + strlen("Host") + 1;
      while (value < end && (*value == ' ' || *value == '\t'))
        ++value;
      const char *value_end = end;
      while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t'))
        --value_end;
      size_t value_len = (size_t)(value_end - value);
      if (value_len < sizeof(out->host)) {
        memcpy(out->host, value, value_len);
        out->host[value_len] = '\0';
      }
    }
    line = end + 2;
  }
  if (head_end + out->content_length > cap)
    return -413;
  out->head_len = head_end;
  return len >= head_end + out->content_length ? 1 : 0;
}

sm_compat_route_t sm_compat_route(const char *method, const char *path) {
  if (!method || !path)
    return SM_COMPAT_NOT_FOUND;
  bool get = strcmp(method, "GET") == 0;
  bool post = strcmp(method, "POST") == 0;
  if (get && strcmp(path, "/api/state") == 0)
    return SM_COMPAT_STATE;
  if (post && strcmp(path, "/api/rescan") == 0)
    return SM_COMPAT_RESCAN;
  if (post && strcmp(path, "/api/quit") == 0)
    return SM_COMPAT_QUIT;
  if (get && strcmp(path, "/") == 0)
    return SM_COMPAT_ROOT;
  return SM_COMPAT_NOT_FOUND;
}

bool sm_compat_redirect_location(const char *host, unsigned api_port,
                                 char *out, size_t out_size) {
  char name[128];
  size_t length = 0;
  bool valid = host && host[0] != '\0';
  if (valid && host[0] == '[') {
    const char *close = strchr(host, ']');
    valid = close != NULL && (size_t)(close - host + 1) < sizeof(name) &&
            (close[1] == '\0' || close[1] == ':');
    if (valid) {
      length = (size_t)(close - host + 1);
      for (size_t i = 1; i + 1 < length && valid; ++i)
        valid = isxdigit((unsigned char)host[i]) || host[i] == ':' ||
                host[i] == '.';
    }
  } else if (valid) {
    const char *colon = strchr(host, ':');
    length = colon ? (size_t)(colon - host) : strlen(host);
    valid = length > 0 && length < sizeof(name);
    for (size_t i = 0; i < length && valid; ++i)
      valid = isalnum((unsigned char)host[i]) || host[i] == '.' ||
              host[i] == '-';
    if (valid && colon)
      for (const char *p = colon + 1; *p && valid; ++p)
        valid = isdigit((unsigned char)*p);
  }
  if (valid) {
    memcpy(name, host, length);
    name[length] = '\0';
  } else {
    (void)copy_text(name, sizeof(name), "127.0.0.1");
  }
  int written = snprintf(out, out_size, "http://%s:%u/", name, api_port);
  return written > 0 && (size_t)written < out_size;
}

// --- Automatic mounting ---

bool sm_auto_failure_blocks(const sm_auto_failure_t *list, size_t count,
                            const char *path, uint64_t size, int64_t mtime) {
  for (size_t i = 0; i < count; ++i) {
    if (list[i].used && strcmp(list[i].path, path) == 0)
      return list[i].size == size && list[i].mtime == mtime;
  }
  return false;
}

void sm_auto_failure_record(sm_auto_failure_t *list, size_t count,
                            const char *path, uint64_t size, int64_t mtime,
                            const char *error) {
  if (count == 0)
    return;
  size_t slot = count;
  for (size_t i = 0; i < count && slot == count; ++i)
    if (list[i].used && strcmp(list[i].path, path) == 0)
      slot = i;
  for (size_t i = 0; i < count && slot == count; ++i)
    if (!list[i].used)
      slot = i;
  if (slot == count) {
    // Full: drop the oldest record (slot 0) and append.
    memmove(&list[0], &list[1], (count - 1u) * sizeof(list[0]));
    slot = count - 1u;
  }
  memset(&list[slot], 0, sizeof(list[slot]));
  if (!copy_text(list[slot].path, sizeof(list[slot].path), path))
    return;
  (void)copy_text(list[slot].error, sizeof(list[slot].error),
                  error && error[0] ? error : "failed");
  list[slot].size = size;
  list[slot].mtime = mtime;
  list[slot].used = true;
}

void sm_auto_failure_forget(sm_auto_failure_t *list, size_t count,
                            const char *path) {
  for (size_t i = 0; i < count; ++i)
    if (list[i].used && strcmp(list[i].path, path) == 0)
      memset(&list[i], 0, sizeof(list[i]));
}

bool sm_auto_error_is_transient(int status, const char *reason) {
  if (status == 0)
    return false;
  if (status == EBUSY || status == ECANCELED || status == EAGAIN ||
      status == EINTR)
    return true;
  return reason && (strcmp(reason, "scanner_busy") == 0 ||
                    strcmp(reason, "runtime_busy") == 0 ||
                    strcmp(reason, "storage_busy") == 0 ||
                    strcmp(reason, "game_active") == 0 ||
                    strcmp(reason, "kraken_sources_changed") == 0 ||
                    strcmp(reason, "kraken_legacy_mount_busy") == 0);
}

sm_auto_decision_t sm_auto_decide(const sm_auto_facts_t *f) {
  if (!f->enabled || !f->eligible)
    return SM_AUTO_IGNORE;
  if (f->library == SM_AUTO_LIBRARY_SAME_PKG)
    return SM_AUTO_READY;
  if (f->library == SM_AUTO_LIBRARY_OTHER || f->registered_conflict)
    return SM_AUTO_DUPLICATE;
  if (f->failed_same_key)
    return SM_AUTO_FAILED;
  if (!f->ownership_ready || f->game_running || f->job_active || !f->stable)
    return SM_AUTO_WAIT;
  return SM_AUTO_START;
}

// --- AMPR-emu-NG ---

bool sm_ampr_ng_folder_needs_injection(bool inject_enabled, bool kraken_cache,
                                       bool has_zstd_index) {
  // Kraken caches carry their own runtime; LZ4-only folders keep the game's.
  return inject_enabled && !kraken_cache && has_zstd_index;
}

sm_ampr_source_t sm_ampr_ng_select_source(bool shared_ng_dir_valid,
                                          bool shared_top_valid) {
  if (shared_ng_dir_valid)
    return SM_AMPR_SOURCE_SHARED_NG_DIR;
  if (shared_top_valid)
    return SM_AMPR_SOURCE_SHARED_TOP;
  return SM_AMPR_SOURCE_BUNDLED;
}

bool sm_ampr_buffer_is_ng(const unsigned char *data, size_t length) {
  static const char marker[] = "ampr-emu-ng ";
  return data && find_bytes(data, length, marker, sizeof(marker) - 1u) != NULL;
}

int sm_ampr_file_is_ng(const char *path) {
  enum { MARKER_LEN = sizeof("ampr-emu-ng ") - 1, CHUNK = 64 * 1024 };
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  unsigned char *buffer = malloc(CHUNK + MARKER_LEN);
  if (!buffer) {
    (void)close(fd);
    errno = ENOMEM;
    return -1;
  }
  size_t kept = 0;
  int found = 0;
  for (;;) {
    ssize_t received = read(fd, buffer + kept, CHUNK);
    if (received < 0 && errno == EINTR)
      continue;
    if (received <= 0) {
      found = received < 0 ? -1 : 0;
      break;
    }
    size_t length = kept + (size_t)received;
    if (sm_ampr_buffer_is_ng(buffer, length)) {
      found = 1;
      break;
    }
    // Keep a tail so a marker split across reads is still found.
    kept = length < MARKER_LEN - 1 ? length : MARKER_LEN - 1;
    memmove(buffer, buffer + length - kept, kept);
  }
  int saved_errno = errno;
  free(buffer);
  (void)close(fd);
  errno = saved_errno;
  return found;
}

bool sm_ampr_emulator_replacement_allowed(const char *name, int source_ng,
                                          int emulator_ng) {
  if (!name || strcasecmp(name, "libSceAmpr.sprx") != 0)
    return true;
  if (source_ng < 0 || emulator_ng < 0)
    return false;
  return source_ng == emulator_ng;
}
