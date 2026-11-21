// Host test for the key-art RGB box filter (resize_rgb in src/sm_icon_thumb.c,
// extracted by tools/test_game_control.py) combined with sm_art_fit.
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sm_game_control.h"

static bool g_stop;
static bool should_stop_requested(void) { return g_stop; }
static bool runtime_sleep_mode_active(void) { return false; }

#include "art_resize.inc"

static unsigned char *image(uint32_t w, uint32_t h) {
  unsigned char *p = malloc((size_t)w * h * 3u);
  assert(p);
  return p;
}

static void test_uniform_and_fit(void) {
  // A 4K-shaped source reduces to both variants with a uniform color kept.
  const uint32_t sw = 3840, sh = 2160;
  unsigned char *source = image(sw, sh);
  for (size_t i = 0; i < (size_t)sw * sh; ++i) {
    source[i * 3] = 200; source[i * 3 + 1] = 100; source[i * 3 + 2] = 7;
  }
  uint32_t tw, th, cw, ch;
  assert(sm_art_fit(sw, sh, SM_ART_TV_MAX_WIDTH, SM_ART_TV_MAX_HEIGHT, &tw, &th));
  assert(tw == 1920 && th == 1080);
  assert(sm_art_fit(sw, sh, SM_ART_CARD_MAX_WIDTH, SM_ART_CARD_MAX_HEIGHT, &cw, &ch));
  assert(cw == 640 && ch == 360);
  unsigned char *tv = image(tw, th), *card = image(cw, ch);
  assert(resize_rgb(source, sw, sh, tv, tw, th));
  assert(resize_rgb(tv, tw, th, card, cw, ch));
  for (size_t i = 0; i < (size_t)cw * ch; ++i)
    assert(card[i * 3] == 200 && card[i * 3 + 1] == 100 && card[i * 3 + 2] == 7);
  free(source); free(tv); free(card);
}

static void test_averaging(void) {
  // 2x2 checker of 0/255 averages to mid grey; non-integer ratios keep range.
  unsigned char source[4 * 4 * 3];
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x)
      memset(source + (y * 4 + x) * 3, ((x + y) & 1) ? 255 : 0, 3);
  unsigned char target[2 * 2 * 3];
  assert(resize_rgb(source, 4, 4, target, 2, 2));
  for (int i = 0; i < 12; ++i)
    assert(target[i] == 128 || target[i] == 127);
  unsigned char odd[3 * 2 * 3];
  assert(resize_rgb(source, 4, 4, odd, 3, 2));
  for (int i = 0; i < 18; ++i)
    assert(odd[i] >= 64 && odd[i] <= 192);
  // Horizontal gradient stays monotonic after a 3x reduction.
  unsigned char grad[30 * 1 * 3], out[10 * 3];
  for (int x = 0; x < 30; ++x)
    memset(grad + x * 3, x * 8, 3);
  assert(resize_rgb(grad, 30, 1, out, 10, 1));
  for (int x = 1; x < 10; ++x)
    assert(out[x * 3] > out[(x - 1) * 3]);
  assert(out[0] == 8);  // (0 + 8 + 16) / 3
}

static void test_stop(void) {
  unsigned char source[4 * 4 * 3] = {0}, target[2 * 2 * 3];
  g_stop = true;
  assert(!resize_rgb(source, 4, 4, target, 2, 2));
  g_stop = false;
}

int main(void) {
  test_uniform_and_fit();
  test_averaging();
  test_stop();
  puts("art resize tests passed");
  return 0;
}
