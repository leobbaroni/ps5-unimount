// Host regression test for src/sm_game_control.c: key-art variant sizing,
// cache naming/eviction and the launch/close/running guard decisions.
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "sm_game_control.h"

static void fit(uint32_t w, uint32_t h, uint32_t mw, uint32_t mh,
                uint32_t ew, uint32_t eh) {
  uint32_t ow = 0, oh = 0;
  assert(sm_art_fit(w, h, mw, mh, &ow, &oh));
  if (ow != ew || oh != eh) {
    fprintf(stderr, "fit %ux%u in %ux%u: got %ux%u, want %ux%u\n", w, h, mw,
            mh, ow, oh, ew, eh);
    assert(0);
  }
  assert(ow <= mw && oh <= mh && ow <= w && oh <= h);
}

static void test_variants(void) {
  sm_art_variant_t v;
  assert(sm_art_parse_variant(NULL, &v) && v == SM_ART_VARIANT_TV);
  assert(sm_art_parse_variant("", &v) && v == SM_ART_VARIANT_TV);
  assert(sm_art_parse_variant("tv", &v) && v == SM_ART_VARIANT_TV);
  assert(sm_art_parse_variant("card", &v) && v == SM_ART_VARIANT_CARD);
  assert(!sm_art_parse_variant("TV", &v));
  assert(!sm_art_parse_variant("cover", &v));
  assert(!sm_art_parse_variant("card ", &v));
  assert(!strcmp(sm_art_variant_name(SM_ART_VARIANT_TV), "tv"));
  assert(!strcmp(sm_art_variant_name(SM_ART_VARIANT_CARD), "card"));
  uint32_t w, h;
  assert(sm_art_variant_bounds(SM_ART_VARIANT_TV, &w, &h) && w == 1920 && h == 1080);
  assert(sm_art_variant_bounds(SM_ART_VARIANT_CARD, &w, &h) && w == 640 && h == 360);
  assert(!sm_art_variant_bounds(SM_ART_VARIANT_COUNT, &w, &h));
}

static void test_fit(void) {
  // 16:9 key art.
  fit(3840, 2160, 1920, 1080, 1920, 1080);
  fit(3840, 2160, 640, 360, 640, 360);
  fit(1920, 1080, 1920, 1080, 1920, 1080);
  // Never upscale.
  fit(1280, 720, 1920, 1080, 1280, 720);
  fit(320, 180, 640, 360, 320, 180);
  fit(1, 1, 640, 360, 1, 1);
  // Wider than 16:9: width-limited.
  fit(4000, 1000, 1920, 1080, 1920, 480);
  fit(2560, 1080, 640, 360, 640, 270);
  // Taller than 16:9: height-limited.
  fit(1080, 1920, 1920, 1080, 608, 1080);
  fit(2048, 2048, 640, 360, 360, 360);
  fit(1000, 3000, 640, 360, 120, 360);
  // Extreme ratios keep at least one pixel.
  fit(8192, 1, 640, 360, 640, 1);
  fit(1, 8192, 640, 360, 1, 360);
  // Aspect ratio stays within rounding of the source.
  for (uint32_t w = 700; w <= 8192; w += 997) {
    for (uint32_t h = 400; h <= 8192; h += 1231) {
      uint32_t ow, oh;
      assert(sm_art_fit(w, h, 1920, 1080, &ow, &oh));
      assert(ow <= 1920 && oh <= 1080 && ow <= w && oh <= h);
      assert(ow == 1920 || oh == 1080 || (ow == w && oh == h));
      double source = (double)w / h, out = (double)ow / oh;
      double tolerance = 1.0 / (oh < ow ? oh : ow) + 1e-9;
      assert(out / source < 1.0 + tolerance * 1.5 &&
             source / out < 1.0 + tolerance * 1.5);
    }
  }
  uint32_t ow, oh;
  assert(!sm_art_fit(0, 10, 640, 360, &ow, &oh));
  assert(!sm_art_fit(10, 0, 640, 360, &ow, &oh));
  assert(!sm_art_fit(10, 10, 0, 360, &ow, &oh));
  assert(!sm_art_fit(10, 10, 640, 360, NULL, &oh));
}

static void test_dimension_caps(void) {
  assert(sm_art_source_dimensions_ok(3840, 2160));
  assert(sm_art_source_dimensions_ok(1, 1));
  assert(sm_art_source_dimensions_ok(8192, 8192));  // exactly 64 MPx
  assert(!sm_art_source_dimensions_ok(8193, 100));
  assert(!sm_art_source_dimensions_ok(100, 8193));
  assert(!sm_art_source_dimensions_ok(0, 100));
  assert(!sm_art_source_dimensions_ok(100, 0));
  assert(!sm_art_source_dimensions_ok(UINT32_MAX, UINT32_MAX));
  assert(SM_ART_SOURCE_MAX_PIXELS == 8192u * 8192u);
}

static void test_cache_names(void) {
  char a[128], b[128], c[128], d[128];
  assert(sm_art_cache_name("PPSA12345", SM_ART_VARIANT_TV, 0x1122334455667788ull,
                           a, sizeof(a)));
  assert(!strcmp(a, "PPSA12345-a1-tv-1122334455667788.jpg"));
  assert(sm_art_cache_name("PPSA12345", SM_ART_VARIANT_CARD, 0x1122334455667788ull,
                           b, sizeof(b)));
  assert(!strcmp(b, "PPSA12345-a1-card-1122334455667788.jpg"));
  assert(sm_art_cache_name("PPSA12345", SM_ART_VARIANT_TV, 0x1122334455667789ull,
                           c, sizeof(c)));
  assert(sm_art_cache_name("CUSA00001", SM_ART_VARIANT_TV, 0x1122334455667788ull,
                           d, sizeof(d)));
  // Variant, fingerprint and title each change the key.
  assert(strcmp(a, b) && strcmp(a, c) && strcmp(a, d));
  assert(!sm_art_cache_name("PPSA1234", SM_ART_VARIANT_TV, 1, a, sizeof(a)));
  assert(!sm_art_cache_name("../../etc", SM_ART_VARIANT_TV, 1, a, sizeof(a)));
  assert(!sm_art_cache_name("ppsa12345", SM_ART_VARIANT_TV, 1, a, sizeof(a)));
  assert(!sm_art_cache_name(NULL, SM_ART_VARIANT_TV, 1, a, sizeof(a)));
  assert(!sm_art_cache_name("PPSA12345", SM_ART_VARIANT_COUNT, 1, a, sizeof(a)));
  assert(!sm_art_cache_name("PPSA12345", SM_ART_VARIANT_TV, 1, a, 20));

  const uint64_t fp = 0x1122334455667788ull;
  // Current variants survive.
  assert(!sm_art_cache_entry_is_stale("PPSA12345-a1-tv-1122334455667788.jpg", "PPSA12345", fp));
  assert(!sm_art_cache_entry_is_stale("PPSA12345-a1-card-1122334455667788.jpg", "PPSA12345", fp));
  // Another source revision, older cache version and temporaries are stale.
  assert(sm_art_cache_entry_is_stale("PPSA12345-a1-tv-1122334455667789.jpg", "PPSA12345", fp));
  assert(sm_art_cache_entry_is_stale("PPSA12345-a0-tv-1122334455667788.jpg", "PPSA12345", fp));
  assert(sm_art_cache_entry_is_stale(
      "PPSA12345-a1-tv-1122334455667788.jpg.tmp.85.3", "PPSA12345", fp));
  // Other titles and foreign files are never touched.
  assert(!sm_art_cache_entry_is_stale("PPSA12346-a1-tv-1122334455667789.jpg", "PPSA12345", fp));
  assert(!sm_art_cache_entry_is_stale("PPSA123456-a1-tv-0.jpg", "PPSA12345", fp));
  assert(!sm_art_cache_entry_is_stale("PPSA12345-a1-tv-1122334455667789.png", "PPSA12345", fp));
  assert(!sm_art_cache_entry_is_stale("notes.txt", "PPSA12345", fp));
  assert(!sm_art_cache_entry_is_stale(NULL, "PPSA12345", fp));
}

static sm_launch_state_t launch_ok(void) {
  sm_launch_state_t s = {.allow_launch = true, .title_valid = true,
                         .title_known = true, .running_app_id = -1};
  return s;
}

static void test_launch_decisions(void) {
  sm_launch_state_t s = launch_ok();
  assert(sm_launch_decide(&s) == SM_GAME_CONTROL_OK);
  s.running_app_id = 0;
  assert(sm_launch_decide(&s) == SM_GAME_CONTROL_OK);
  s.running_app_id = 0x60000012;
  assert(sm_launch_decide(&s) == SM_GAME_CONTROL_APP_RUNNING);
  s.replace = true;
  assert(sm_launch_decide(&s) == SM_GAME_CONTROL_REPLACE);

  s = launch_ok(); s.title_valid = false; s.allow_launch = false;
  assert(sm_launch_decide(&s) == SM_GAME_CONTROL_INVALID_TITLE);
  s = launch_ok(); s.allow_launch = false;
  assert(sm_launch_decide(&s) == SM_GAME_CONTROL_DISABLED);
  s = launch_ok(); s.stopping = true; s.rest_mode = true;
  assert(sm_launch_decide(&s) == SM_GAME_CONTROL_STOPPING);
  s = launch_ok(); s.rest_mode = true;
  assert(sm_launch_decide(&s) == SM_GAME_CONTROL_REST_MODE);
  s = launch_ok(); s.title_known = false; s.running_app_id = 0x60000012; s.replace = true;
  // An unknown title never closes the running app.
  assert(sm_launch_decide(&s) == SM_GAME_CONTROL_UNKNOWN_TITLE);
  s = launch_ok(); s.rest_mode = true; s.running_app_id = 0x60000012; s.replace = true;
  assert(sm_launch_decide(&s) == SM_GAME_CONTROL_REST_MODE);
  assert(sm_launch_decide(NULL) == SM_GAME_CONTROL_INVALID_TITLE);
}

static void test_close_decisions(void) {
  sm_close_state_t s = {.allow_launch = true, .running_app_id = 0x60000012};
  assert(sm_close_decide(&s) == SM_GAME_CONTROL_OK);
  s.running_app_id = -1;
  assert(sm_close_decide(&s) == SM_GAME_CONTROL_NO_APP_RUNNING);
  s.running_app_id = 0;
  assert(sm_close_decide(&s) == SM_GAME_CONTROL_NO_APP_RUNNING);

  s.running_app_id = 0x60000012;
  s.title_given = true; s.title_valid = true; s.title_id = "PPSA12345";
  s.running_title_id = NULL;
  assert(sm_close_decide(&s) == SM_GAME_CONTROL_TITLE_UNKNOWN_RUNNING);
  s.running_title_id = "";
  assert(sm_close_decide(&s) == SM_GAME_CONTROL_TITLE_UNKNOWN_RUNNING);
  s.running_title_id = "PPSA54321";
  assert(sm_close_decide(&s) == SM_GAME_CONTROL_TITLE_MISMATCH);
  s.running_title_id = "PPSA12345";
  assert(sm_close_decide(&s) == SM_GAME_CONTROL_OK);
  s.title_valid = false;
  assert(sm_close_decide(&s) == SM_GAME_CONTROL_INVALID_TITLE);
  s.title_valid = true; s.title_id = "PPSA1";
  assert(sm_close_decide(&s) == SM_GAME_CONTROL_INVALID_TITLE);
  s.title_id = "PPSA12345";
  s.allow_launch = false;
  assert(sm_close_decide(&s) == SM_GAME_CONTROL_DISABLED);
  s.allow_launch = true; s.stopping = true;
  assert(sm_close_decide(&s) == SM_GAME_CONTROL_STOPPING);
  s.stopping = false; s.rest_mode = true;
  assert(sm_close_decide(&s) == SM_GAME_CONTROL_REST_MODE);
  // Without a title the running app is closed even when its title is unknown.
  sm_close_state_t any = {.allow_launch = true, .running_app_id = 0x60000012};
  assert(sm_close_decide(&any) == SM_GAME_CONTROL_OK);
}

static void test_reasons_and_running_title(void) {
  static const struct {
    sm_game_control_decision_t decision;
    const char *reason;
    int status, http;
  } table[] = {
      {SM_GAME_CONTROL_OK, "", 0, 200},
      {SM_GAME_CONTROL_REPLACE, "", 0, 200},
      {SM_GAME_CONTROL_DISABLED, "launch_disabled", EPERM, 403},
      {SM_GAME_CONTROL_STOPPING, "stopping", ECANCELED, 409},
      {SM_GAME_CONTROL_REST_MODE, "rest_mode", EBUSY, 409},
      {SM_GAME_CONTROL_INVALID_TITLE, "invalid_title_id", EINVAL, 400},
      {SM_GAME_CONTROL_UNKNOWN_TITLE, "unknown_title", ENOENT, 404},
      {SM_GAME_CONTROL_APP_RUNNING, "app_running", EBUSY, 409},
      {SM_GAME_CONTROL_NO_APP_RUNNING, "no_app_running", EBUSY, 409},
      {SM_GAME_CONTROL_TITLE_MISMATCH, "title_mismatch", EBUSY, 409},
      {SM_GAME_CONTROL_TITLE_UNKNOWN_RUNNING, "running_title_unknown", EBUSY, 409},
  };
  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
    assert(!strcmp(sm_game_control_reason(table[i].decision), table[i].reason));
    assert(sm_game_control_status(table[i].decision) == table[i].status);
    assert(sm_game_control_http_status(table[i].decision) == table[i].http);
  }

  assert(!strcmp(sm_running_title(0x60000012, 85, 0x60000012u, "PPSA12345"),
                 "PPSA12345"));
  // The tracked game must be the big app.
  assert(!sm_running_title(0x60000013, 85, 0x60000012u, "PPSA12345"));
  assert(!sm_running_title(-1, 85, 0xFFFFFFFFu, "PPSA12345"));
  assert(!sm_running_title(0x60000012, 0, 0x60000012u, "PPSA12345"));
  assert(!sm_running_title(0x60000012, 85, 0x60000012u, ""));
  assert(!sm_running_title(0x60000012, 85, 0x60000012u, NULL));
  assert(!sm_running_title(0x60000012, 85, 0x60000012u, "NPXS20001x"));
}

int main(void) {
  test_variants();
  test_fit();
  test_dimension_caps();
  test_cache_names();
  test_launch_decisions();
  test_close_decisions();
  test_reasons_and_running_title();
  puts("game control tests passed");
  return 0;
}
