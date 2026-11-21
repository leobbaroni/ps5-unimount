#ifndef SM_GAME_CONTROL_H
#define SM_GAME_CONTROL_H

// Pure helpers for the key-art route and the launch/close/running routes.
// Inputs are injected so tests/test_game_control.c exercises the same code as
// the payload. Keep this header and src/sm_game_control.c free of console-only
// headers.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// --- Key art (GET /api/v1/games/art) ---

#define SM_ART_CACHE_VERSION 1u
#define SM_ART_JPEG_QUALITY 82
#define SM_ART_TV_MAX_WIDTH 1920u
#define SM_ART_TV_MAX_HEIGHT 1080u
#define SM_ART_CARD_MAX_WIDTH 640u
#define SM_ART_CARD_MAX_HEIGHT 360u
// Whole-image decode limits: one side and total pixels.
#define SM_ART_SOURCE_MAX_SIDE 8192u
#define SM_ART_SOURCE_MAX_PIXELS (64u * 1024u * 1024u)
#define SM_ART_SOURCE_MAX_FILE_SIZE (64u * 1024u * 1024u)

typedef enum {
  SM_ART_VARIANT_TV = 0,
  SM_ART_VARIANT_CARD,
  SM_ART_VARIANT_COUNT,
} sm_art_variant_t;

// NULL or "" selects tv; otherwise "tv" or "card". False for anything else.
bool sm_art_parse_variant(const char *value, sm_art_variant_t *variant_out);
const char *sm_art_variant_name(sm_art_variant_t variant);
// Maximum output box for a variant; false for an unknown variant.
bool sm_art_variant_bounds(sm_art_variant_t variant, uint32_t *width_out,
                           uint32_t *height_out);
// True when a decoded source of this size may be converted.
bool sm_art_source_dimensions_ok(uint32_t width, uint32_t height);
// Fit width x height inside max_width x max_height preserving the aspect
// ratio; never enlarges. Results are at least 1 pixel.
bool sm_art_fit(uint32_t width, uint32_t height, uint32_t max_width,
                uint32_t max_height, uint32_t *width_out, uint32_t *height_out);
// Cache file name "<TITLE>-a<version>-<variant>-<fingerprint>.jpg".
bool sm_art_cache_name(const char *title_id, sm_art_variant_t variant,
                       uint64_t fingerprint, char *out, size_t size);
// True when a cache directory entry belongs to title_id but is not a current
// variant for fingerprint (older revision, other source, or a temporary file).
// Old entries are removed only after the caller's age check.
bool sm_art_cache_entry_is_stale(const char *name, const char *title_id,
                                 uint64_t fingerprint);

// --- Launch / close / running ---

typedef enum {
  SM_GAME_CONTROL_OK = 0,
  SM_GAME_CONTROL_REPLACE,  // launch: close the running big app first
  SM_GAME_CONTROL_DISABLED,
  SM_GAME_CONTROL_STOPPING,
  SM_GAME_CONTROL_REST_MODE,
  SM_GAME_CONTROL_INVALID_TITLE,
  SM_GAME_CONTROL_UNKNOWN_TITLE,
  SM_GAME_CONTROL_APP_RUNNING,
  SM_GAME_CONTROL_NO_APP_RUNNING,
  SM_GAME_CONTROL_TITLE_MISMATCH,
  SM_GAME_CONTROL_TITLE_UNKNOWN_RUNNING,
} sm_game_control_decision_t;

typedef struct {
  bool allow_launch;     // [api] allow_launch
  bool stopping;         // daemon shutdown or API restart
  bool rest_mode;        // runtime sleep mode
  bool title_valid;      // request title_id passed the API validation
  bool title_known;      // title is in the library or registered in app.db
  bool replace;          // request "replace": true
  int32_t running_app_id;  // sceSystemServiceGetAppIdOfRunningBigApp()
} sm_launch_state_t;

typedef struct {
  bool allow_launch;
  bool stopping;
  bool rest_mode;
  bool title_given;      // request carried title_id
  bool title_valid;
  const char *title_id;  // requested title when title_given
  int32_t running_app_id;
  const char *running_title_id;  // confirmed title of the big app, or NULL
} sm_close_state_t;

sm_game_control_decision_t sm_launch_decide(const sm_launch_state_t *state);
sm_game_control_decision_t sm_close_decide(const sm_close_state_t *state);
// Stable error_reason string for a refusal; "" for OK/REPLACE.
const char *sm_game_control_reason(sm_game_control_decision_t decision);
// errno-style status for a refusal; 0 for OK/REPLACE.
int sm_game_control_status(sm_game_control_decision_t decision);
// HTTP status for a refusal (400, 403, 404 or 409); 200 for OK/REPLACE.
int sm_game_control_http_status(sm_game_control_decision_t decision);
// Title of the big app when the tracked game process is it: the lifecycle
// title is used only when the tracked pid's app_id equals the big app id.
// Returns NULL when it cannot tell.
const char *sm_running_title(int32_t running_app_id, int32_t tracked_pid,
                             uint32_t tracked_app_id,
                             const char *tracked_title_id);

#endif
