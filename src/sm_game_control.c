// Pure helpers for key art and game launch/close. Keep this file free of
// console-only headers: tests/test_game_control.c compiles it on the host.
#include "sm_game_control.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static bool valid_title_id(const char *title_id) {
  if (!title_id || strlen(title_id) != 9u)
    return false;
  for (size_t i = 0; i < 4u; ++i)
    if (title_id[i] < 'A' || title_id[i] > 'Z')
      return false;
  for (size_t i = 4u; i < 9u; ++i)
    if (title_id[i] < '0' || title_id[i] > '9')
      return false;
  return true;
}

bool sm_art_parse_variant(const char *value, sm_art_variant_t *variant_out) {
  if (!variant_out)
    return false;
  if (!value || value[0] == '\0' || strcmp(value, "tv") == 0) {
    *variant_out = SM_ART_VARIANT_TV;
    return true;
  }
  if (strcmp(value, "card") == 0) {
    *variant_out = SM_ART_VARIANT_CARD;
    return true;
  }
  return false;
}

const char *sm_art_variant_name(sm_art_variant_t variant) {
  return variant == SM_ART_VARIANT_CARD ? "card" : "tv";
}

bool sm_art_variant_bounds(sm_art_variant_t variant, uint32_t *width_out,
                           uint32_t *height_out) {
  if (!width_out || !height_out)
    return false;
  if (variant == SM_ART_VARIANT_TV) {
    *width_out = SM_ART_TV_MAX_WIDTH;
    *height_out = SM_ART_TV_MAX_HEIGHT;
    return true;
  }
  if (variant == SM_ART_VARIANT_CARD) {
    *width_out = SM_ART_CARD_MAX_WIDTH;
    *height_out = SM_ART_CARD_MAX_HEIGHT;
    return true;
  }
  return false;
}

bool sm_art_source_dimensions_ok(uint32_t width, uint32_t height) {
  return width > 0 && height > 0 && width <= SM_ART_SOURCE_MAX_SIDE &&
         height <= SM_ART_SOURCE_MAX_SIDE &&
         (uint64_t)width * height <= SM_ART_SOURCE_MAX_PIXELS;
}

bool sm_art_fit(uint32_t width, uint32_t height, uint32_t max_width,
                uint32_t max_height, uint32_t *width_out, uint32_t *height_out) {
  if (!width_out || !height_out || width == 0 || height == 0 ||
      max_width == 0 || max_height == 0)
    return false;
  if (width <= max_width && height <= max_height) {
    *width_out = width;
    *height_out = height;
    return true;
  }
  // Scale by the tighter of the two limits: compare w/mw with h/mh.
  uint64_t w, h;
  if ((uint64_t)width * max_height >= (uint64_t)height * max_width) {
    w = max_width;
    h = ((uint64_t)height * max_width + width / 2u) / width;
    if (h > max_height)
      h = max_height;
  } else {
    h = max_height;
    w = ((uint64_t)width * max_height + height / 2u) / height;
    if (w > max_width)
      w = max_width;
  }
  *width_out = w ? (uint32_t)w : 1u;
  *height_out = h ? (uint32_t)h : 1u;
  return true;
}

bool sm_art_cache_name(const char *title_id, sm_art_variant_t variant,
                       uint64_t fingerprint, char *out, size_t size) {
  if (!out || size == 0 || !valid_title_id(title_id) ||
      (variant != SM_ART_VARIANT_TV && variant != SM_ART_VARIANT_CARD))
    return false;
  int written = snprintf(out, size, "%s-a%u-%s-%016llx.jpg", title_id,
                         SM_ART_CACHE_VERSION, sm_art_variant_name(variant),
                         (unsigned long long)fingerprint);
  return written > 0 && (size_t)written < size;
}

bool sm_art_cache_entry_is_stale(const char *name, const char *title_id,
                                 uint64_t fingerprint) {
  if (!name || !valid_title_id(title_id) || strncmp(name, title_id, 9u) != 0 ||
      name[9] != '-')
    return false;
  if (strstr(name, ".tmp."))
    return true;
  size_t length = strlen(name);
  if (length < 4u || strcmp(name + length - 4u, ".jpg") != 0)
    return false;
  char version[16], current[24];
  (void)snprintf(version, sizeof(version), "-a%u-", SM_ART_CACHE_VERSION);
  (void)snprintf(current, sizeof(current), "-%016llx.",
                 (unsigned long long)fingerprint);
  return !(strncmp(name + 9u, version, strlen(version)) == 0 &&
           strstr(name, current));
}

sm_game_control_decision_t sm_launch_decide(const sm_launch_state_t *state) {
  if (!state || !state->title_valid)
    return SM_GAME_CONTROL_INVALID_TITLE;
  if (!state->allow_launch)
    return SM_GAME_CONTROL_DISABLED;
  if (state->stopping)
    return SM_GAME_CONTROL_STOPPING;
  if (state->rest_mode)
    return SM_GAME_CONTROL_REST_MODE;
  if (!state->title_known)
    return SM_GAME_CONTROL_UNKNOWN_TITLE;
  if (state->running_app_id > 0)
    return state->replace ? SM_GAME_CONTROL_REPLACE : SM_GAME_CONTROL_APP_RUNNING;
  return SM_GAME_CONTROL_OK;
}

sm_game_control_decision_t sm_close_decide(const sm_close_state_t *state) {
  if (!state || (state->title_given &&
                 (!state->title_valid || !valid_title_id(state->title_id))))
    return SM_GAME_CONTROL_INVALID_TITLE;
  if (!state->allow_launch)
    return SM_GAME_CONTROL_DISABLED;
  if (state->stopping)
    return SM_GAME_CONTROL_STOPPING;
  if (state->rest_mode)
    return SM_GAME_CONTROL_REST_MODE;
  if (state->running_app_id <= 0)
    return SM_GAME_CONTROL_NO_APP_RUNNING;
  if (state->title_given) {
    if (!valid_title_id(state->running_title_id))
      return SM_GAME_CONTROL_TITLE_UNKNOWN_RUNNING;
    if (strcmp(state->running_title_id, state->title_id) != 0)
      return SM_GAME_CONTROL_TITLE_MISMATCH;
  }
  return SM_GAME_CONTROL_OK;
}

const char *sm_game_control_reason(sm_game_control_decision_t decision) {
  switch (decision) {
  case SM_GAME_CONTROL_DISABLED: return "launch_disabled";
  case SM_GAME_CONTROL_STOPPING: return "stopping";
  case SM_GAME_CONTROL_REST_MODE: return "rest_mode";
  case SM_GAME_CONTROL_INVALID_TITLE: return "invalid_title_id";
  case SM_GAME_CONTROL_UNKNOWN_TITLE: return "unknown_title";
  case SM_GAME_CONTROL_APP_RUNNING: return "app_running";
  case SM_GAME_CONTROL_NO_APP_RUNNING: return "no_app_running";
  case SM_GAME_CONTROL_TITLE_MISMATCH: return "title_mismatch";
  case SM_GAME_CONTROL_TITLE_UNKNOWN_RUNNING: return "running_title_unknown";
  default: return "";
  }
}

int sm_game_control_status(sm_game_control_decision_t decision) {
  switch (decision) {
  case SM_GAME_CONTROL_DISABLED: return EPERM;
  case SM_GAME_CONTROL_STOPPING: return ECANCELED;
  case SM_GAME_CONTROL_INVALID_TITLE: return EINVAL;
  case SM_GAME_CONTROL_UNKNOWN_TITLE: return ENOENT;
  case SM_GAME_CONTROL_REST_MODE:
  case SM_GAME_CONTROL_APP_RUNNING:
  case SM_GAME_CONTROL_NO_APP_RUNNING:
  case SM_GAME_CONTROL_TITLE_MISMATCH:
  case SM_GAME_CONTROL_TITLE_UNKNOWN_RUNNING: return EBUSY;
  default: return 0;
  }
}

int sm_game_control_http_status(sm_game_control_decision_t decision) {
  switch (decision) {
  case SM_GAME_CONTROL_OK:
  case SM_GAME_CONTROL_REPLACE: return 200;
  case SM_GAME_CONTROL_INVALID_TITLE: return 400;
  case SM_GAME_CONTROL_DISABLED: return 403;
  case SM_GAME_CONTROL_UNKNOWN_TITLE: return 404;
  default: return 409;
  }
}

const char *sm_running_title(int32_t running_app_id, int32_t tracked_pid,
                             uint32_t tracked_app_id,
                             const char *tracked_title_id) {
  if (running_app_id <= 0 || tracked_pid <= 0 ||
      tracked_app_id != (uint32_t)running_app_id ||
      !valid_title_id(tracked_title_id))
    return NULL;
  return tracked_title_id;
}
