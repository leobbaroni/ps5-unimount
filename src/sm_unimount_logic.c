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

typedef struct {
  bool closed;          // the top-level object ended with '}'
  bool syntax_error;    // not a JSON object, or malformed at the top level
  bool unimount;        // top-level key "unimount" (any value)
  bool version_string;  // top-level "version" with a string value
  bool games_array;     // top-level "games" with an array value
  char version[32];
} json_top_scan_t;

static bool json_is_space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Skips a string starting at text[*at] == '"'. False when it is cut off.
static bool json_skip_string(const char *text, size_t length, size_t *at) {
  bool escaped = false;
  for (size_t i = *at + 1u; i < length; ++i) {
    if (escaped)
      escaped = false;
    else if (text[i] == '\\')
      escaped = true;
    else if (text[i] == '"') {
      *at = i + 1u;
      return true;
    }
  }
  return false;
}

// Records the top-level keys of a JSON object, or of any prefix of one: the
// probe may only have read the start of a large body.
static void json_scan_top_level(const char *text, size_t length,
                                json_top_scan_t *out) {
  memset(out, 0, sizeof(*out));
  size_t i = 0;
#define SKIP_SPACE()                                    \
  do {                                                  \
    while (i < length && json_is_space(text[i]))        \
      ++i;                                              \
    if (i == length)                                    \
      return;                                           \
  } while (0)
  SKIP_SPACE();
  if (text[i] != '{') {
    out->syntax_error = true;
    return;
  }
  ++i;
  for (bool first = true;; first = false) {
    SKIP_SPACE();
    if (first && text[i] == '}') {
      out->closed = true;
      return;
    }
    if (text[i] != '"') {
      out->syntax_error = true;
      return;
    }
    size_t key = i + 1u;
    if (!json_skip_string(text, length, &i))
      return;
    size_t key_len = i - 1u - key;
    SKIP_SPACE();
    if (text[i] != ':') {
      out->syntax_error = true;
      return;
    }
    ++i;
    SKIP_SPACE();
    char first_char = text[i];
#define KEY_IS(name) \
  (key_len == sizeof(name) - 1u && memcmp(text + key, name, key_len) == 0)
    if (KEY_IS("unimount"))
      out->unimount = true;
    else if (KEY_IS("games") && first_char == '[')
      out->games_array = true;
    bool version = KEY_IS("version") && first_char == '"';
#undef KEY_IS
    if (first_char == '"') {
      size_t start = i + 1u;
      if (!json_skip_string(text, length, &i))
        return;
      if (version) {
        out->version_string = true;
        size_t value_len = i - 1u - start;
        if (value_len < sizeof(out->version)) {
          memcpy(out->version, text + start, value_len);
          out->version[value_len] = '\0';
        }
      }
    } else if (first_char == '{' || first_char == '[') {
      unsigned depth = 0;
      for (;;) {
        if (i == length)
          return;
        char c = text[i];
        if (c == '"') {
          if (!json_skip_string(text, length, &i))
            return;
          continue;
        }
        ++i;
        if (c == '{' || c == '[')
          ++depth;
        else if ((c == '}' || c == ']') && --depth == 0)
          break;
      }
    } else {
      size_t start = i;
      while (i < length && text[i] != ',' && text[i] != '}' &&
             !json_is_space(text[i])) {
        if (!isalnum((unsigned char)text[i]) && text[i] != '-' &&
            text[i] != '+' && text[i] != '.') {
          out->syntax_error = true;
          return;
        }
        ++i;
      }
      if (i == start) {
        out->syntax_error = true;
        return;
      }
    }
    SKIP_SPACE();
    if (text[i] == '}') {
      out->closed = true;
      return;
    }
    if (text[i] != ',') {
      out->syntax_error = true;
      return;
    }
    ++i;
  }
#undef SKIP_SPACE
}

sm_peer_kind_t sm_takeover_classify_reply(const char *reply, size_t length,
                                          char version[32], const char **why) {
  static const char http_prefix[] = "HTTP/1.";
  const char *reason = NULL;
  sm_peer_kind_t kind = SM_PEER_UNKNOWN;
  if (version)
    version[0] = '\0';
  size_t prefix_len = length < sizeof(http_prefix) - 1u
                          ? length : sizeof(http_prefix) - 1u;
  int status = 0;
  size_t body_offset = 0;
  if (!reply || length == 0) {
    reason = "listener sent no reply";
  } else if (memcmp(reply, http_prefix, prefix_len) != 0) {
    // Kraken Mount always answers HTTP/1.1; anything else is another service.
    kind = SM_PEER_OTHER;
    reason = "listener is not an HTTP server";
  } else if (!find_bytes((const unsigned char *)reply, length, "\r\n\r\n", 4u)) {
    reason = "incomplete HTTP response head";
  } else if (!sm_http_parse_response(reply, length, &status, &body_offset)) {
    kind = SM_PEER_OTHER;
    reason = "malformed HTTP status line";
  } else if (status >= 500) {
    // An overloaded or starting server may be Kraken Mount: ask again.
    reason = "HTTP server error";
  } else if (status != 200) {
    kind = SM_PEER_OTHER;
    reason = "HTTP status other than 200";
  } else {
    json_top_scan_t scan;
    json_scan_top_level(reply + body_offset, length - body_offset, &scan);
    if (scan.unimount) {
      kind = SM_PEER_UNIMOUNT;
      reason = "UniMount compatible listener";
    } else if (scan.version_string && scan.games_array) {
      kind = SM_PEER_KRAKEN;
      reason = scan.closed ? "Kraken Mount state"
                           : "Kraken Mount state (body prefix)";
      if (version)
        (void)copy_text(version, 32, scan.version);
    } else if (scan.syntax_error) {
      kind = SM_PEER_OTHER;
      reason = "body is not a JSON object";
    } else if (scan.closed) {
      kind = SM_PEER_OTHER;
      reason = "JSON without Kraken Mount's version and games";
    } else {
      reason = length - body_offset == 0
                   ? "empty or missing body"
                   : "truncated body without Kraken Mount's keys";
    }
  }
  if (why)
    *why = reason;
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

// --- Startup, replacement and shutdown around a running game ---

sm_startup_decision_t sm_startup_decide(const sm_startup_facts_t *facts) {
  if (!facts || !facts->app_running)
    return SM_STARTUP_PROCEED;
  // An unknown title may be ours: waiting is the safe side.
  if (!facts->title_known || facts->managed)
    return SM_STARTUP_WAIT;
  return SM_STARTUP_PROCEED;
}

sm_restart_timeout_t sm_restart_timeout_decide(bool app_running) {
  // A predecessor that defers its stop does so for a running game.
  return app_running ? SM_RESTART_WAIT_FOR_APP : SM_RESTART_GIVE_UP;
}

sm_layer_kind_t sm_sandbox_layer_kind(bool unionfs, const char *mount_path,
                                      const char *app0_path) {
  if (unionfs)
    return SM_LAYER_OVERLAY;
  if (mount_path && app0_path && app0_path[0] != '\0') {
    size_t length = strlen(app0_path);
    if (strncmp(mount_path, app0_path, length) == 0 &&
        (mount_path[length] == '\0' || mount_path[length] == '/'))
      return SM_LAYER_OVERLAY;
  }
  return SM_LAYER_ALIAS;
}

sm_session_kind_t sm_session_kind(const sm_layer_kind_t *layers, size_t count,
                                  bool redirects) {
  if (redirects)
    return SM_SESSION_OVERLAY;
  sm_session_kind_t kind = SM_SESSION_NONE;
  for (size_t i = 0; layers && i < count; ++i) {
    if (layers[i] == SM_LAYER_OVERLAY)
      return SM_SESSION_OVERLAY;
    kind = SM_SESSION_ALIASES;
  }
  return kind;
}

sm_stop_decision_t sm_stop_decide(const sm_stop_facts_t *facts) {
  if (!facts || (facts->requests == 0 && !facts->immediate))
    return SM_STOP_NONE;
  if (!facts->game_active)
    return SM_STOP_NOW;
  bool overlay = facts->session == SM_SESSION_OVERLAY;
  if (facts->immediate || facts->requests >= 2u)
    return overlay ? SM_STOP_NOW_KEEP : SM_STOP_NOW;
  // A signal may be followed by SIGKILL: only an overlay session, whose
  // teardown and abandonment both crash the console at game exit, waits.
  if (facts->by_signal && !overlay)
    return SM_STOP_NOW;
  return SM_STOP_DEFER;
}

bool sm_shutdown_keeps_game_layers(const sm_layer_owner_facts_t *facts) {
  if (!facts || !facts->has_layers || !facts->overlay)
    return false;
  if (facts->pid > 0 && facts->pid_alive)
    return true;
  if (!facts->sandbox_exists)
    return false;
  // No process yet: the launch is still in progress in that sandbox.
  return facts->pid <= 0 || facts->handoff_pending;
}

bool sm_title_list_contains(const char list[][MAX_TITLE_ID], size_t count,
                            const char *title_id) {
  if (!list || !title_id)
    return false;
  for (size_t i = 0; i < count; ++i)
    if (strcmp(list[i], title_id) == 0)
      return true;
  return false;
}

bool sm_title_list_add(char list[][MAX_TITLE_ID], size_t *count, size_t max,
                       const char *title_id) {
  if (!list || !count || !title_id ||
      !valid_title_id(title_id, strlen(title_id)))
    return false;
  if (sm_title_list_contains((const char (*)[MAX_TITLE_ID])list, *count,
                             title_id))
    return true;
  if (*count >= max)
    return false;
  (void)copy_text(list[(*count)++], MAX_TITLE_ID, title_id);
  return true;
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

// --- Sandbox session record ---

static const char *const k_session_labels[] = {
    "game", "global", "fakelib2", "directory", SM_SESSION_PARENT_LABEL};

static bool session_label_valid(const char *label) {
  for (size_t i = 0; i < sizeof(k_session_labels) / sizeof(k_session_labels[0]);
       ++i)
    if (strcmp(label, k_session_labels[i]) == 0)
      return true;
  return false;
}

static const char *session_label_fstype(const char *label) {
  if (strcmp(label, SM_SESSION_PARENT_LABEL) == 0)
    return SM_SESSION_PARENT_FSTYPE;
  return strcmp(label, "directory") == 0 ? "nullfs" : "unionfs";
}

bool sm_session_layer_is_parent(const sm_session_layer_t *layer) {
  return layer && strcmp(layer->label, SM_SESSION_PARENT_LABEL) == 0;
}

bool sm_session_redirects_add(sm_session_redirects_t *set,
                              const char *destination) {
  if (!set || !destination || destination[0] == '\0' ||
      set->count >= SM_SESSION_MAX_REDIRECTS)
    return false;
  size_t length = strlen(destination) + 1u;
  if (length > MAX_PATH || length > sizeof(set->storage) - set->bytes)
    return false;
  memcpy(set->storage + set->bytes, destination, length);
  set->bytes += length;
  ++set->count;
  return true;
}

static bool add_json(struct json_object *object, const char *key,
                     struct json_object *value) {
  if (!value)
    return false;
  if (json_object_object_add(object, key, value) != 0) {
    json_object_put(value);
    return false;
  }
  return true;
}

static bool append_json(struct json_object *array, struct json_object *value) {
  if (!value)
    return false;
  if (json_object_array_add(array, value) != 0) {
    json_object_put(value);
    return false;
  }
  return true;
}

static struct json_object *fsid_json(const int32_t fsid[2]) {
  struct json_object *array = json_object_new_array();
  if (array && (!append_json(array, json_object_new_int(fsid[0])) ||
                !append_json(array, json_object_new_int(fsid[1])))) {
    json_object_put(array);
    return NULL;
  }
  return array;
}

bool sm_session_record_format(const sm_session_record_t *record,
                              const char *version, char **out) {
  *out = NULL;
  if (!record || record->layer_count > SM_SESSION_MAX_LAYERS ||
      record->redirect_count > SM_SESSION_MAX_REDIRECT_SETS)
    return false;
  struct json_object *root = json_object_new_object();
  struct json_object *layers = NULL, *redirects = NULL;
  bool ok =
      root &&
      add_json(root, "format", json_object_new_int(SM_SESSION_RECORD_FORMAT)) &&
      add_json(root, "version", json_object_new_string(version ? version : "")) &&
      add_json(root, "boot_time", json_object_new_int64(record->boot_time)) &&
      add_json(root, "owner_pid", json_object_new_int64(record->owner_pid)) &&
      add_json(root, "game_pid", json_object_new_int64(record->game_pid)) &&
      add_json(root, "app_id", json_object_new_int64(record->app_id)) &&
      add_json(root, "complete", json_object_new_boolean(record->complete)) &&
      add_json(root, "title_id", json_object_new_string(record->title_id)) &&
      add_json(root, "sandbox_app0",
               json_object_new_string(record->sandbox_app0)) &&
      add_json(root, "lib_mount", json_object_new_string(record->lib_mount)) &&
      add_json(root, "layers", layers = json_object_new_array()) &&
      add_json(root, "redirects", redirects = json_object_new_array());
  for (size_t i = 0; ok && i < record->layer_count; ++i) {
    const sm_session_layer_t *layer = &record->layers[i];
    struct json_object *item = json_object_new_object();
    ok = append_json(layers, item) &&
         add_json(item, "label", json_object_new_string(layer->label)) &&
         add_json(item, "fstype", json_object_new_string(layer->fstype)) &&
         add_json(item, "source", json_object_new_string(layer->source)) &&
         add_json(item, "target", json_object_new_string(layer->target)) &&
         add_json(item, "fsid", fsid_json(layer->fsid)) &&
         add_json(item, "read_only", json_object_new_boolean(layer->read_only)) &&
         add_json(item, "created", json_object_new_boolean(layer->created));
  }
  for (size_t i = 0; ok && i < record->redirect_count; ++i) {
    const sm_session_redirects_t *set = &record->redirects[i];
    struct json_object *item = json_object_new_object();
    struct json_object *destinations = NULL;
    ok = append_json(redirects, item) &&
         add_json(item, "source", json_object_new_string(set->source)) &&
         add_json(item, "target", json_object_new_string(set->target)) &&
         add_json(item, "device", json_object_new_int64((int64_t)set->device)) &&
         add_json(item, "fsid", fsid_json(set->fsid)) &&
         add_json(item, "destinations",
                  destinations = json_object_new_array());
    size_t offset = 0;
    for (size_t j = 0; ok && j < set->count; ++j) {
      if (offset >= set->bytes || offset >= sizeof(set->storage)) {
        ok = false;
        break;
      }
      const char *destination = set->storage + offset;
      size_t length = strnlen(destination, set->bytes - offset);
      if (length == set->bytes - offset) {
        ok = false;
        break;
      }
      ok = append_json(destinations, json_object_new_string(destination));
      offset += length + 1u;
    }
  }
  if (ok) {
    const char *text = json_object_to_json_string_ext(
        root, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
    *out = text ? strdup(text) : NULL;
    ok = *out != NULL;
  }
  if (root)
    json_object_put(root);
  return ok;
}

static const char k_record_malformed[] = "malformed session record";
static const char k_record_long[] = "over-long field in session record";

// 0 when found and copied; otherwise the reason.
static const char *record_string(struct json_object *object, const char *key,
                                 char *out, size_t size, bool allow_empty) {
  struct json_object *value;
  if (!json_object_object_get_ex(object, key, &value) ||
      !json_object_is_type(value, json_type_string))
    return k_record_malformed;
  const char *text = json_object_get_string(value);
  size_t length = (size_t)json_object_get_string_len(value);
  if (length >= size)
    return k_record_long;
  if (strlen(text) != length || (!allow_empty && length == 0))
    return k_record_malformed;
  memcpy(out, text, length + 1u);
  return NULL;
}

static bool record_int(struct json_object *object, const char *key,
                       int64_t min, int64_t max, int64_t *out) {
  struct json_object *value;
  if (!json_object_object_get_ex(object, key, &value) ||
      !json_object_is_type(value, json_type_int))
    return false;
  errno = 0;
  int64_t number = json_object_get_int64(value);
  if (errno != 0 || number < min || number > max)
    return false;
  *out = number;
  return true;
}

static bool record_bool(struct json_object *object, const char *key,
                        bool *out) {
  struct json_object *value;
  if (!json_object_object_get_ex(object, key, &value) ||
      !json_object_is_type(value, json_type_boolean))
    return false;
  *out = json_object_get_boolean(value);
  return true;
}

static bool record_fsid(struct json_object *object, int32_t fsid[2]) {
  struct json_object *value;
  if (!json_object_object_get_ex(object, "fsid", &value) ||
      !json_object_is_type(value, json_type_array) ||
      json_object_array_length(value) != 2u)
    return false;
  for (size_t i = 0; i < 2u; ++i) {
    struct json_object *word = json_object_array_get_idx(value, i);
    if (!json_object_is_type(word, json_type_int))
      return false;
    errno = 0;
    int64_t number = json_object_get_int64(word);
    if (errno != 0 || number < INT32_MIN || number > INT32_MAX)
      return false;
    fsid[i] = (int32_t)number;
  }
  return true;
}

static const char *parse_record_root(struct json_object *root,
                                     sm_session_record_t *out) {
  const char *why;
  int64_t number;
  struct json_object *array;
  if (!json_object_is_type(root, json_type_object))
    return k_record_malformed;
  if (!record_int(root, "format", INT64_MIN, INT64_MAX, &number))
    return k_record_malformed;
  if (number != SM_SESSION_RECORD_FORMAT)
    return "unsupported session record format";
  if (!record_int(root, "boot_time", 0, INT64_MAX, &out->boot_time) ||
      !record_int(root, "owner_pid", 0, INT32_MAX, &out->owner_pid) ||
      !record_int(root, "game_pid", 0, INT32_MAX, &out->game_pid) ||
      !record_int(root, "app_id", 0, UINT32_MAX, &number) ||
      !record_bool(root, "complete", &out->complete))
    return k_record_malformed;
  out->app_id = (uint32_t)number;
  if ((why = record_string(root, "title_id", out->title_id,
                           sizeof(out->title_id), false)) ||
      (why = record_string(root, "sandbox_app0", out->sandbox_app0,
                           sizeof(out->sandbox_app0), true)) ||
      (why = record_string(root, "lib_mount", out->lib_mount,
                           sizeof(out->lib_mount), true)))
    return why;
  if (!valid_title_id(out->title_id, strlen(out->title_id)))
    return "invalid title id in session record";
  if ((out->sandbox_app0[0] &&
       !sm_takeover_sandbox_mount_for_title(out->sandbox_app0, out->title_id)) ||
      (out->lib_mount[0] &&
       !sm_takeover_sandbox_mount_for_title(out->lib_mount, out->title_id)))
    return "path outside the game's sandbox in session record";

  if (!json_object_object_get_ex(root, "layers", &array) ||
      !json_object_is_type(array, json_type_array))
    return k_record_malformed;
  size_t count = json_object_array_length(array);
  if (count > SM_SESSION_MAX_LAYERS)
    return "too many layers in session record";
  for (size_t i = 0; i < count; ++i) {
    struct json_object *item = json_object_array_get_idx(array, i);
    sm_session_layer_t *layer = &out->layers[i];
    if (!json_object_is_type(item, json_type_object))
      return k_record_malformed;
    if ((why = record_string(item, "label", layer->label, sizeof(layer->label),
                             false)) ||
        (why = record_string(item, "fstype", layer->fstype,
                             sizeof(layer->fstype), false)) ||
        (why = record_string(item, "source", layer->source,
                             sizeof(layer->source), true)) ||
        (why = record_string(item, "target", layer->target,
                             sizeof(layer->target), false)))
      return why;
    bool parent = sm_session_layer_is_parent(layer);
    if (!session_label_valid(layer->label) ||
        strcmp(layer->fstype, session_label_fstype(layer->label)) != 0 ||
        // A parent directory has no source and was created by the owner;
        // every mount has a source.
        (parent ? layer->source[0] != '\0' : layer->source[0] == '\0') ||
        !record_fsid(item, layer->fsid) ||
        !record_bool(item, "read_only", &layer->read_only) ||
        !record_bool(item, "created", &layer->created) ||
        (parent && (!layer->created || layer->read_only)))
      return k_record_malformed;
    if (!sm_takeover_sandbox_mount_for_title(layer->target, out->title_id))
      return "path outside the game's sandbox in session record";
  }
  out->layer_count = count;

  if (!json_object_object_get_ex(root, "redirects", &array) ||
      !json_object_is_type(array, json_type_array))
    return k_record_malformed;
  count = json_object_array_length(array);
  if (count > SM_SESSION_MAX_REDIRECT_SETS)
    return "too many redirect sets in session record";
  for (size_t i = 0; i < count; ++i) {
    struct json_object *item = json_object_array_get_idx(array, i);
    struct json_object *destinations;
    sm_session_redirects_t *set = &out->redirects[i];
    if (!json_object_is_type(item, json_type_object))
      return k_record_malformed;
    if ((why = record_string(item, "source", set->source, sizeof(set->source),
                             false)) ||
        (why = record_string(item, "target", set->target, sizeof(set->target),
                             false)))
      return why;
    if (!record_int(item, "device", INT64_MIN, INT64_MAX, &number) ||
        !record_fsid(item, set->fsid) ||
        !json_object_object_get_ex(item, "destinations", &destinations) ||
        !json_object_is_type(destinations, json_type_array))
      return k_record_malformed;
    set->device = (uint64_t)number;
    if (!sm_takeover_sandbox_mount_for_title(set->target, out->title_id))
      return "path outside the game's sandbox in session record";
    size_t total = json_object_array_length(destinations);
    if (total > SM_SESSION_MAX_REDIRECTS)
      return "too many redirects in session record";
    for (size_t j = 0; j < total; ++j) {
      struct json_object *value = json_object_array_get_idx(destinations, j);
      if (!json_object_is_type(value, json_type_string))
        return k_record_malformed;
      const char *text = json_object_get_string(value);
      size_t length = (size_t)json_object_get_string_len(value);
      if (length >= MAX_PATH)
        return k_record_long;
      if (length == 0 || strlen(text) != length)
        return k_record_malformed;
      if (!sm_session_redirects_add(set, text))
        return "too many redirects in session record";
    }
  }
  out->redirect_count = count;
  return NULL;
}

bool sm_session_record_parse(const char *body, size_t length,
                             sm_session_record_t *out, const char **why) {
  const char *reason = NULL;
  memset(out, 0, sizeof(*out));
  if (!body || length == 0) {
    reason = "empty session record";
  } else if (length > SM_SESSION_RECORD_MAX_BYTES) {
    reason = "session record too large";
  } else {
    struct json_tokener *tokener = json_tokener_new();
    struct json_object *root =
        tokener ? json_tokener_parse_ex(tokener, body, (int)length) : NULL;
    bool complete = root && json_tokener_get_error(tokener) == json_tokener_success;
    if (complete) {
      // Only whitespace may follow the object.
      for (size_t i = json_tokener_get_parse_end(tokener); i < length; ++i)
        if (!json_is_space(body[i]))
          complete = false;
    }
    reason = complete ? parse_record_root(root, out) : k_record_malformed;
    if (root)
      json_object_put(root);
    if (tokener)
      json_tokener_free(tokener);
  }
  if (reason) {
    memset(out, 0, sizeof(*out));
    if (why)
      *why = reason;
    return false;
  }
  if (why)
    *why = NULL;
  return true;
}

size_t sm_session_record_units(const sm_session_record_t *record) {
  if (!record)
    return 0;
  size_t units = record->redirect_count;
  for (size_t i = 0; i < record->layer_count && i < SM_SESSION_MAX_LAYERS; ++i)
    units += sm_session_layer_is_parent(&record->layers[i]) ? 0u : 1u;
  return units;
}

bool sm_session_record_empty(const sm_session_record_t *record) {
  return !record || (record->layer_count == 0 && record->redirect_count == 0);
}

sm_session_kind_t sm_session_record_kind(const sm_session_record_t *record) {
  sm_layer_kind_t kinds[SM_SESSION_MAX_LAYERS];
  size_t count = 0;
  for (size_t i = 0; record && i < record->layer_count &&
                     i < SM_SESSION_MAX_LAYERS; ++i)
    if (!sm_session_layer_is_parent(&record->layers[i]))
      kinds[count++] = sm_sandbox_layer_kind(
        strcmp(record->layers[i].fstype, "unionfs") == 0,
        record->layers[i].target, record->sandbox_app0);
  return sm_session_kind(kinds, count, record && record->redirect_count > 0);
}

static bool fsid_equal(const int32_t a[2], const int32_t b[2]) {
  return a[0] == b[0] && a[1] == b[1];
}

// Same rule as the owner's sandbox_mount_matches, plus MNAMELEN truncation.
static bool mount_source_matches(const sm_mount_entry_t *entry,
                                 const char *source) {
  if (!entry->from)
    return false;
  const char *from = entry->from;
  size_t raw = strlen(from);
  if (strncmp(from, "<above>:", 8) == 0)
    from += 8;
  if (strcmp(from, source) == 0 ||
      (strcmp(source, "/data") == 0 && strcmp(from, "/user/data") == 0))
    return true;
  size_t length = strlen(from);
  return entry->from_size != 0 && raw + 1u >= entry->from_size && length > 0 &&
         strncmp(source, from, length) == 0;
}

static bool entry_on(const sm_mount_entry_t *entry, const char *path) {
  return entry->on && strcmp(entry->on, path) == 0;
}

const char *sm_session_record_match(const sm_session_record_t *record,
                                    const sm_mount_entry_t *mounts,
                                    size_t count, char *buf, size_t size) {
  if (!record || (!mounts && count != 0)) {
    (void)copy_text(buf, size, "mount table unavailable");
    return buf;
  }
  for (size_t i = 0; i < record->layer_count; ++i) {
    const sm_session_layer_t *layer = &record->layers[i];
    if (sm_session_layer_is_parent(layer))
      continue;  // a directory, not a mount: removed with rmdir only
    const sm_mount_entry_t *top = NULL, *same = NULL;
    size_t same_count = 0;
    for (size_t j = 0; j < count; ++j) {
      if (entry_on(&mounts[j], layer->target))
        top = &mounts[j];
      if (fsid_equal(mounts[j].fsid, layer->fsid)) {
        same = &mounts[j];
        ++same_count;
      }
    }
    const char *problem = NULL;
    if (!same)
      problem = top ? "has a different filesystem id" : "is not mounted";
    else if (same_count != 1u)
      problem = "has a filesystem id that is not unique";
    else if (!entry_on(same, layer->target))
      problem = "has a different filesystem id";
    else if (!same->fstype || strcmp(same->fstype, layer->fstype) != 0)
      problem = "has a different type";
    else if (!mount_source_matches(same, layer->source))
      problem = "has a different source";
    else if (top != same)
      problem = "is covered by a newer mount";
    if (problem) {
      (void)snprintf(buf, size, "layer %zu (%s %s) %s", i + 1u, layer->fstype,
                     layer->target, problem);
      return buf;
    }
  }
  // Redirects live in the NSFS namespace itself, which an app0 cover may
  // stack over; the owner's delete re-checks the namespace before each pass.
  for (size_t i = 0; i < record->redirect_count; ++i) {
    const sm_session_redirects_t *set = &record->redirects[i];
    bool nsfs = false, same = false;
    for (size_t j = 0; j < count; ++j) {
      if (!entry_on(&mounts[j], set->target) || !mounts[j].fstype ||
          strcmp(mounts[j].fstype, "nsfs") != 0)
        continue;
      nsfs = true;
      same = same || fsid_equal(mounts[j].fsid, set->fsid);
    }
    const char *problem = !nsfs   ? "is not mounted"
                          : !same ? "has a different filesystem id"
                                  : NULL;
    if (problem) {
      (void)snprintf(buf, size, "redirect namespace %s %s", set->target,
                     problem);
      return buf;
    }
  }
  return NULL;
}

bool sm_session_adopt_decide(const sm_session_record_t *record,
                             const sm_adopt_live_t *live, char *buf,
                             size_t size, const char **reason) {
  const char *why = NULL;
  if (!record || !live)
    why = "no session record";
  else if (!record->complete)
    why = "incomplete session record";
  else if (sm_session_record_empty(record))
    why = "empty session record";
  else if (record->boot_time == 0 || live->boot_time == 0 ||
           record->boot_time != live->boot_time)
    why = "record from another boot";
  else if (record->game_pid <= 0)
    why = "no game process in the record";
  else if (!live->game_alive)
    why = "recorded game process is not running";
  else if (record->app_id == 0 || live->game_app_id != record->app_id)
    why = "recorded process is not the recorded application";
  else if (strcmp(live->game_title_id, record->title_id) != 0)
    why = "recorded process runs another title";
  else if (live->running_app_id != record->app_id)
    why = "recorded game is not the running application";
  else if (live->running_title_id[0] != '\0' &&
           strcmp(live->running_title_id, record->title_id) != 0)
    why = "running application has another title";
  else
    why = sm_session_record_match(record, live->mounts, live->mount_count, buf,
                                  size);
  if (reason)
    *reason = why;
  return why == NULL;
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
  if (reason && strcmp(reason, SM_AUTO_REASON_USER_CANCELLED) == 0)
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
