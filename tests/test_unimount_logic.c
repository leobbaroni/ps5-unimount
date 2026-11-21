// Host regression test for the UniMount decision helpers in
// src/sm_unimount_logic.c: Kraken takeover, the compatible listener,
// automatic PKG mounting, adoption and AMPR-emu-NG selection.
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sm_unimount_logic.h"

static void test_classify_state(void) {
  // Shape of real Kraken Mount v0.3.x GET /api/state (DESIGN.md "Web API").
  static const char kraken[] =
      "{\"version\":\"0.3.1\",\"busy\":false,\"paused\":false,\"waiting\":null,"
      "\"running\":[],\"games\":[{\"title_id\":\"PPSA01234\",\"title_name\":\"Game\","
      "\"version\":\"01.000.000\",\"path\":\"/mnt/usb0/game.pkg\",\"kind\":\"pkg\","
      "\"packing\":null,\"drive\":\"/mnt/usb0\",\"size\":1,\"status\":\"ready\","
      "\"error\":null,\"cache_size\":0,\"cache_built\":null,\"backport\":null,"
      "\"playgo\":null,\"playgo_off\":false}],\"drives\":[],\"cache\":{},\"log\":[]}";
  static const char ours[] =
      "{\"version\":\"0.3.0\",\"games\":[],\"unimount\":\"0.1.0\"}";
  static const char ok_head[] =
      "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/json\r\n"
      "Cache-Control: no-store\r\n\r\n";
  char reply[4096];
  char version[32];
  const char *why = NULL;
#define CLASSIFY(head, body, body_len)                                       \
  (snprintf(reply, sizeof(reply), "%s%.*s", head, (int)(body_len), body),    \
   sm_takeover_classify_reply(reply, strlen(reply), version, &why))

  // Complete replies.
  assert(CLASSIFY(ok_head, kraken, strlen(kraken)) == SM_PEER_KRAKEN);
  assert(strcmp(version, "0.3.1") == 0 && why);
  assert(CLASSIFY(ok_head, ours, strlen(ours)) == SM_PEER_UNIMOUNT);
  assert(version[0] == '\0');
  // Valid HTTP from another server: not Kraken.
  assert(CLASSIFY("HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\n",
                  "not found", 9) == SM_PEER_OTHER);
  assert(CLASSIFY("HTTP/1.0 302 Found\r\nLocation: /\r\n\r\n", "", 0) ==
         SM_PEER_OTHER);
  assert(CLASSIFY(ok_head, "<html>", 6) == SM_PEER_OTHER);
  assert(CLASSIFY(ok_head, "[]", 2) == SM_PEER_OTHER);
  static const char no_games[] = "{\"version\":\"1\",\"games\":{}}";
  assert(CLASSIFY(ok_head, no_games, strlen(no_games)) == SM_PEER_OTHER);
  static const char numeric_version[] = "{\"version\":3,\"games\":[]}";
  assert(CLASSIFY(ok_head, numeric_version, strlen(numeric_version)) ==
         SM_PEER_OTHER);
  static const char other_json[] = " {\"status\":\"ok\",\"items\":[1,2,{\"a\":\"}\"}]}";
  assert(CLASSIFY(ok_head, other_json, strlen(other_json)) == SM_PEER_OTHER);
  assert(CLASSIFY(ok_head, "{}", 2) == SM_PEER_OTHER);
  assert(CLASSIFY(ok_head, "{\"a\" 1}", 7) == SM_PEER_OTHER);
  // Not HTTP at all (another protocol on the port).
  assert(sm_takeover_classify_reply("SSH-2.0-OpenSSH\r\n", 17, version, &why) ==
         SM_PEER_OTHER);
  assert(sm_takeover_classify_reply("HTTX", 4, version, &why) == SM_PEER_OTHER);
  assert(CLASSIFY("HTTP/1.1 2x4 X\r\n\r\n", "", 0) == SM_PEER_OTHER);

  // Inconclusive: no bytes, a cut-off head, a server error, an empty body,
  // or a body cut off before Kraken's keys. Never "absent".
  assert(sm_takeover_classify_reply(NULL, 0, version, &why) == SM_PEER_UNKNOWN);
  assert(sm_takeover_classify_reply("", 0, version, &why) == SM_PEER_UNKNOWN);
  assert(sm_takeover_classify_reply("HTTP/1", 6, version, &why) == SM_PEER_UNKNOWN);
  assert(sm_takeover_classify_reply(ok_head, 20, version, &why) == SM_PEER_UNKNOWN);
  assert(CLASSIFY("HTTP/1.1 503 Service Unavailable\r\n\r\n", "", 0) ==
         SM_PEER_UNKNOWN);
  assert(CLASSIFY(ok_head, "", 0) == SM_PEER_UNKNOWN);
  assert(CLASSIFY(ok_head, "  ", 2) == SM_PEER_UNKNOWN);
  assert(CLASSIFY(ok_head, kraken, 10) == SM_PEER_UNKNOWN);
  // "version" seen, "games" not yet.
  assert(CLASSIFY(ok_head, kraken, 40) == SM_PEER_UNKNOWN);
  assert(why && strstr(why, "truncated"));
  // A truncated version string is not trusted either.
  assert(CLASSIFY(ok_head, "{\"games\":[],\"version\":\"0.3", 27) ==
         SM_PEER_UNKNOWN);

  // A body prefix that already holds both keys is Kraken Mount (a large
  // library or log can exceed the probe's read cap or deadline).
  const char *games = strstr(kraken, "\"games\":[");
  size_t cut = (size_t)(games - kraken) + strlen("\"games\":[{\"title");
  assert(CLASSIFY(ok_head, kraken, cut) == SM_PEER_KRAKEN);
  assert(strcmp(version, "0.3.1") == 0 && strstr(why, "prefix"));
  // Escaped quotes and nested braces in skipped values do not confuse it.
  static const char nested[] =
      "{\"note\":\"a \\\"}\\\" b\",\"busy\":{\"x\":[\"]\"]},\"waiting\":null,"
      "\"version\":\"0.4.0\",\"games\":[{\"title_name\":\"{[\"";
  assert(CLASSIFY(ok_head, nested, strlen(nested)) == SM_PEER_KRAKEN);
  assert(strcmp(version, "0.4.0") == 0);
  // "games" seen first, "version" later.
  static const char reordered[] = "{\"games\":[],\"version\":\"1.0\",\"lo";
  assert(CLASSIFY(ok_head, reordered, strlen(reordered)) == SM_PEER_KRAKEN);
  // A "games"/"version" inside a nested value is not top level.
  static const char nested_keys[] =
      "{\"data\":{\"version\":\"1\",\"games\":[]},\"x\":";
  assert(CLASSIFY(ok_head, nested_keys, strlen(nested_keys)) == SM_PEER_UNKNOWN);
  static const char nested_keys_closed[] =
      "{\"data\":{\"version\":\"1\",\"games\":[]}}";
  assert(CLASSIFY(ok_head, nested_keys_closed, strlen(nested_keys_closed)) ==
         SM_PEER_OTHER);
  // An oversized version string is still Kraken; it is just not copied.
  static const char long_version[] =
      "{\"version\":\"0123456789012345678901234567890123456789\",\"games\":[]}";
  assert(CLASSIFY(ok_head, long_version, strlen(long_version)) == SM_PEER_KRAKEN);
  assert(version[0] == '\0');
#undef CLASSIFY
}

static void test_quit_and_http(void) {
  assert(sm_takeover_quit_outcome(204) == SM_QUIT_ACCEPTED);
  assert(sm_takeover_quit_outcome(200) == SM_QUIT_ACCEPTED);
  assert(sm_takeover_quit_outcome(409) == SM_QUIT_GAME_RUNNING);
  assert(sm_takeover_quit_outcome(404) == SM_QUIT_REFUSED);
  assert(sm_takeover_quit_outcome(0) == SM_QUIT_REFUSED);
  assert(sm_takeover_quit_outcome(-1) == SM_QUIT_REFUSED);

  int status;
  size_t body;
  static const char reply[] =
      "HTTP/1.1 409 Conflict\r\nConnection: close\r\nContent-Type: text/plain\r\n"
      "Content-Length: 17\r\nCache-Control: no-store\r\n\r\na game is running";
  assert(sm_http_parse_response(reply, strlen(reply), &status, &body));
  assert(status == 409 && strcmp(reply + body, "a game is running") == 0);
  static const char no_content[] =
      "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
  assert(sm_http_parse_response(no_content, strlen(no_content), &status, &body));
  assert(status == 204 && body == strlen(no_content));
  assert(!sm_http_parse_response(reply, 30, &status, &body));
  assert(!sm_http_parse_response("SSH-2.0\r\n\r\n", 11, &status, &body));
  assert(!sm_http_parse_response("HTTP/1.1 2x4 X\r\n\r\n", 18, &status, &body));
}

static void test_legacy_mounts(void) {
  char tid[MAX_TITLE_ID];
  assert(sm_takeover_is_legacy_mount("nullfs", "/data/krakenmount/caches/PPSA01234",
                                     "/system_ex/app/PPSA01234", tid));
  assert(strcmp(tid, "PPSA01234") == 0);
  // Different title, other fs type, our cache, or a sandbox target: untouched.
  assert(!sm_takeover_is_legacy_mount("nullfs", "/data/krakenmount/caches/PPSA01234",
                                      "/system_ex/app/PPSA09999", tid));
  assert(!sm_takeover_is_legacy_mount("exfatfs", "/data/krakenmount/caches/PPSA01234",
                                      "/system_ex/app/PPSA01234", tid));
  assert(!sm_takeover_is_legacy_mount("nullfs", "/data/shadowmount/kraken/PPSA01234",
                                      "/system_ex/app/PPSA01234", tid));
  assert(!sm_takeover_is_legacy_mount("nullfs", "/data/krakenmount/caches/PPSA01234",
                                      "/mnt/sandbox/PPSA01234_000/app0", tid));
  assert(!sm_takeover_is_legacy_mount("nullfs", "/data/krakenmount/caches/PPSA01234/x",
                                      "/system_ex/app/PPSA01234/x", tid));
  assert(!sm_takeover_is_legacy_mount("nullfs", "/data/krakenmount/caches/NPXS40000",
                                      "/system_ex/app/NPXS40000", tid));
  assert(!sm_takeover_is_legacy_mount(NULL, "/a", "/b", tid));

  assert(sm_takeover_sandbox_mount_for_title("/mnt/sandbox/PPSA01234_000/app0",
                                             "PPSA01234"));
  assert(sm_takeover_sandbox_mount_for_title("/mnt/sandbox/PPSA01234_001", "PPSA01234"));
  assert(!sm_takeover_sandbox_mount_for_title("/mnt/sandbox/PPSA01235_000/app0",
                                              "PPSA01234"));
  assert(!sm_takeover_sandbox_mount_for_title("/mnt/sandbox/PPSA01234/app0",
                                              "PPSA01234"));
  assert(!sm_takeover_sandbox_mount_for_title("/system_ex/app/PPSA01234", "PPSA01234"));
}

static void test_adoption(void) {
  sm_adopt_facts_t facts = {
      .user_param_json = true, .kraken_cache_done = true,
  };
  assert(sm_takeover_adoption(&facts) == SM_ADOPT_YES);
  facts.legacy_mount_present = true;
  assert(sm_takeover_adoption(&facts) == SM_ADOPT_LATER);
  facts.legacy_mount_present = false;
  // A genuinely installed copy, or one of ours, is never adopted.
  sm_adopt_facts_t installed = facts;
  installed.user_app_pkg = true;
  assert(sm_takeover_adoption(&installed) == SM_ADOPT_NO);
  installed = facts;
  installed.ext_installed = true;
  assert(sm_takeover_adoption(&installed) == SM_ADOPT_NO);
  installed = facts;
  installed.mount_link = true;
  assert(sm_takeover_adoption(&installed) == SM_ADOPT_NO);
  installed = facts;
  installed.kraken_cache_done = false;
  assert(sm_takeover_adoption(&installed) == SM_ADOPT_NO);
  installed = facts;
  installed.user_param_json = false;
  assert(sm_takeover_adoption(&installed) == SM_ADOPT_NO);
  assert(sm_takeover_adoption(NULL) == SM_ADOPT_NO);
}

static void test_ledger(void) {
  char ledger[512];
  assert(sm_owner_ledger_format(ledger, sizeof(ledger), "UniMount", "0.1.0", 1234,
                                1700000000, 1699990000));
  assert(strstr(ledger, "\"product\":\"UniMount\""));
  long pid = 0;
  int64_t boot = 0;
  assert(sm_owner_ledger_parse(ledger, strlen(ledger), &pid, &boot));
  assert(pid == 1234 && boot == 1699990000);
  assert(!sm_owner_ledger_parse("{}", 2, &pid, &boot));
  assert(!sm_owner_ledger_parse("garbage", 7, &pid, &boot));
  char tiny[8];
  assert(!sm_owner_ledger_format(tiny, sizeof(tiny), "UniMount", "0.1.0", 1, 2, 3));
}

static void test_request_scan(void) {
  sm_http_request_head_t head;
  static const char get_state[] =
      "GET /api/state?x=1 HTTP/1.1\r\nHost: 192.168.1.5:8097\r\n\r\n";
  assert(sm_http_request_scan(get_state, strlen(get_state), SM_COMPAT_MAX_REQUEST,
                              &head) == 1);
  assert(strcmp(head.method, "GET") == 0 && strcmp(head.path, "/api/state") == 0);
  assert(strcmp(head.host, "192.168.1.5:8097") == 0);
  assert(sm_compat_route(head.method, head.path) == SM_COMPAT_STATE);

  static const char quit[] =
      "POST /api/quit HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n";
  assert(sm_http_request_scan(quit, strlen(quit), SM_COMPAT_MAX_REQUEST, &head) == 1);
  assert(sm_compat_route(head.method, head.path) == SM_COMPAT_QUIT);

  static const char body[] =
      "POST /api/rescan HTTP/1.1\r\ncontent-length: 8\r\n\r\nshutdown";
  assert(sm_http_request_scan(body, strlen(body) - 3, SM_COMPAT_MAX_REQUEST, &head) == 0);
  assert(sm_http_request_scan(body, strlen(body), SM_COMPAT_MAX_REQUEST, &head) == 1);
  assert(head.content_length == 8 && sm_compat_route(head.method, head.path) ==
                                         SM_COMPAT_RESCAN);

  // Incomplete head, oversized head and body, bad framing.
  assert(sm_http_request_scan("GET / HTTP/1.1\r\n", 16, SM_COMPAT_MAX_REQUEST, &head) == 0);
  char big[64];
  memset(big, 'a', sizeof(big));
  assert(sm_http_request_scan(big, sizeof(big), sizeof(big), &head) == -431);
  static const char huge[] =
      "POST /api/rescan HTTP/1.1\r\nContent-Length: 99999999999999999999\r\n\r\n";
  assert(sm_http_request_scan(huge, strlen(huge), SM_COMPAT_MAX_REQUEST, &head) == -400 ||
         sm_http_request_scan(huge, strlen(huge), SM_COMPAT_MAX_REQUEST, &head) == -413);
  static const char too_long[] =
      "POST /api/rescan HTTP/1.1\r\nContent-Length: 20000\r\n\r\n";
  assert(sm_http_request_scan(too_long, strlen(too_long), SM_COMPAT_MAX_REQUEST,
                              &head) == -413);
  static const char nonnumeric[] =
      "POST /api/rescan HTTP/1.1\r\nContent-Length: abc\r\n\r\n";
  assert(sm_http_request_scan(nonnumeric, strlen(nonnumeric), SM_COMPAT_MAX_REQUEST,
                              &head) == -400);
  static const char chunked[] =
      "POST /api/rescan HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
  assert(sm_http_request_scan(chunked, strlen(chunked), SM_COMPAT_MAX_REQUEST,
                              &head) == -400);
  static const char no_version[] = "GET /api/state\r\n\r\n";
  assert(sm_http_request_scan(no_version, strlen(no_version), SM_COMPAT_MAX_REQUEST,
                              &head) == -400);

  assert(sm_compat_route("GET", "/") == SM_COMPAT_ROOT);
  assert(sm_compat_route("POST", "/api/state") == SM_COMPAT_NOT_FOUND);
  assert(sm_compat_route("GET", "/api/quit") == SM_COMPAT_NOT_FOUND);
  assert(sm_compat_route("GET", "/api/settings") == SM_COMPAT_NOT_FOUND);
  assert(sm_compat_route("POST", "/api/rebuild-all") == SM_COMPAT_NOT_FOUND);
}

static void test_redirect(void) {
  char location[192];
  assert(sm_compat_redirect_location("192.168.1.5:8097", 10101, location,
                                     sizeof(location)));
  assert(strcmp(location, "http://192.168.1.5:10101/") == 0);
  assert(sm_compat_redirect_location("ps5.local", 10101, location, sizeof(location)));
  assert(strcmp(location, "http://ps5.local:10101/") == 0);
  assert(sm_compat_redirect_location("[::1]:8097", 10101, location, sizeof(location)));
  assert(strcmp(location, "http://[::1]:10101/") == 0);
  assert(sm_compat_redirect_location("", 10101, location, sizeof(location)));
  assert(strcmp(location, "http://127.0.0.1:10101/") == 0);
  assert(sm_compat_redirect_location("evil\r\nSet-Cookie: x", 10101, location,
                                     sizeof(location)));
  assert(strcmp(location, "http://127.0.0.1:10101/") == 0);
  assert(sm_compat_redirect_location("a.b:80x", 10101, location, sizeof(location)));
  assert(strcmp(location, "http://127.0.0.1:10101/") == 0);
}

static void test_failures(void) {
  sm_auto_failure_t list[3];
  memset(list, 0, sizeof(list));
  sm_auto_failure_record(list, 3, "/mnt/usb0/a.pkg", 100, 5, "kraken_base_invalid");
  assert(sm_auto_failure_blocks(list, 3, "/mnt/usb0/a.pkg", 100, 5));
  // A changed file (size or mtime) is retried.
  assert(!sm_auto_failure_blocks(list, 3, "/mnt/usb0/a.pkg", 101, 5));
  assert(!sm_auto_failure_blocks(list, 3, "/mnt/usb0/a.pkg", 100, 6));
  assert(!sm_auto_failure_blocks(list, 3, "/mnt/usb0/b.pkg", 100, 5));
  // Re-recording the same path replaces the key.
  sm_auto_failure_record(list, 3, "/mnt/usb0/a.pkg", 101, 5, NULL);
  assert(sm_auto_failure_blocks(list, 3, "/mnt/usb0/a.pkg", 101, 5));
  assert(!sm_auto_failure_blocks(list, 3, "/mnt/usb0/a.pkg", 100, 5));
  assert(strcmp(list[0].error, "failed") == 0);
  sm_auto_failure_record(list, 3, "/b.pkg", 1, 1, "x");
  sm_auto_failure_record(list, 3, "/c.pkg", 1, 1, "x");
  sm_auto_failure_record(list, 3, "/d.pkg", 1, 1, "x");  // evicts the oldest
  assert(!sm_auto_failure_blocks(list, 3, "/mnt/usb0/a.pkg", 101, 5));
  assert(sm_auto_failure_blocks(list, 3, "/d.pkg", 1, 1));
  sm_auto_failure_forget(list, 3, "/d.pkg");
  assert(!sm_auto_failure_blocks(list, 3, "/d.pkg", 1, 1));

  assert(sm_auto_error_is_transient(EBUSY, "scanner_busy"));
  assert(sm_auto_error_is_transient(ECANCELED, NULL));
  // Rest mode, shutdown or a game starting cancel a job: retried later.
  assert(sm_auto_error_is_transient(ECANCELED, "game_active"));
  // A cancel the user asked for is remembered like a failure for this
  // (path, size, mtime), so the job does not restart 10 s later.
  assert(!sm_auto_error_is_transient(ECANCELED, SM_AUTO_REASON_USER_CANCELLED));
  {
    sm_auto_failure_t cancelled[2];
    memset(cancelled, 0, sizeof(cancelled));
    sm_auto_failure_record(cancelled, 2, "/mnt/usb0/a.pkg", 10, 20,
                           SM_AUTO_REASON_USER_CANCELLED);
    assert(sm_auto_failure_blocks(cancelled, 2, "/mnt/usb0/a.pkg", 10, 20));
    assert(!sm_auto_failure_blocks(cancelled, 2, "/mnt/usb0/a.pkg", 10, 21));
  }
  assert(sm_auto_error_is_transient(EAGAIN, "kraken_sources_changed"));
  assert(sm_auto_error_is_transient(EEXIST, "storage_busy"));
  assert(!sm_auto_error_is_transient(EINVAL, "kraken_base_invalid"));
  assert(!sm_auto_error_is_transient(EEXIST, "kraken_title_conflict"));
  assert(!sm_auto_error_is_transient(EILSEQ, "kraken_decode_failed"));
  assert(!sm_auto_error_is_transient(0, NULL));
}

static void test_auto_decide(void) {
  sm_auto_facts_t base = {
      .enabled = true, .ownership_ready = true, .stable = true, .eligible = true,
  };
  sm_auto_facts_t f = base;
  assert(sm_auto_decide(&f) == SM_AUTO_START);
  f.enabled = false;
  assert(sm_auto_decide(&f) == SM_AUTO_IGNORE);
  f = base;
  f.eligible = false;  // retail, encrypted, PS4 or patch PKG
  assert(sm_auto_decide(&f) == SM_AUTO_IGNORE);
  f = base;
  f.game_running = true;
  assert(sm_auto_decide(&f) == SM_AUTO_WAIT);
  f = base;
  f.job_active = true;
  assert(sm_auto_decide(&f) == SM_AUTO_WAIT);
  f = base;
  f.stable = false;
  assert(sm_auto_decide(&f) == SM_AUTO_WAIT);
  f = base;
  f.ownership_ready = false;
  assert(sm_auto_decide(&f) == SM_AUTO_WAIT);
  f = base;
  f.failed_same_key = true;
  assert(sm_auto_decide(&f) == SM_AUTO_FAILED);
  f = base;
  f.library = SM_AUTO_LIBRARY_SAME_PKG;
  assert(sm_auto_decide(&f) == SM_AUTO_READY);
  f = base;
  f.library = SM_AUTO_LIBRARY_OTHER;
  assert(sm_auto_decide(&f) == SM_AUTO_DUPLICATE);
  f = base;
  f.registered_conflict = true;
  assert(sm_auto_decide(&f) == SM_AUTO_DUPLICATE);
  // Another source wins over a remembered failure and a running game.
  f.failed_same_key = true;
  f.game_running = true;
  assert(sm_auto_decide(&f) == SM_AUTO_DUPLICATE);
}

static void write_file(const char *path, const void *data, size_t size) {
  FILE *file = fopen(path, "wb");
  assert(file && fwrite(data, 1, size, file) == size && fclose(file) == 0);
}

static void test_ampr(void) {
  // Only zstd-packed folders get the injection; Kraken caches carry their own.
  assert(sm_ampr_ng_folder_needs_injection(true, false, true));
  assert(!sm_ampr_ng_folder_needs_injection(false, false, true));
  assert(!sm_ampr_ng_folder_needs_injection(true, true, true));
  assert(!sm_ampr_ng_folder_needs_injection(true, false, false));  // LZ4 only

  assert(sm_ampr_ng_select_source(true, true) == SM_AMPR_SOURCE_SHARED_NG_DIR);
  assert(sm_ampr_ng_select_source(true, false) == SM_AMPR_SOURCE_SHARED_NG_DIR);
  assert(sm_ampr_ng_select_source(false, true) == SM_AMPR_SOURCE_SHARED_TOP);
  assert(sm_ampr_ng_select_source(false, false) == SM_AMPR_SOURCE_BUNDLED);

  static const unsigned char ng[] = "\177ELF....ampr-emu-ng 0.5.0\0/app0/kraken";
  static const unsigned char upstream[] = "\177ELF....ampr_emu v1.2\0";
  assert(sm_ampr_buffer_is_ng(ng, sizeof(ng)));
  assert(!sm_ampr_buffer_is_ng(upstream, sizeof(upstream)));
  assert(!sm_ampr_buffer_is_ng(NULL, 0));

  char directory[] = "/tmp/unimount-ampr-XXXXXX";
  assert(mkdtemp(directory));
  char ng_path[256], upstream_path[256], split_path[256], missing[256];
  snprintf(ng_path, sizeof(ng_path), "%s/ng.sprx", directory);
  snprintf(upstream_path, sizeof(upstream_path), "%s/upstream.sprx", directory);
  snprintf(split_path, sizeof(split_path), "%s/split.sprx", directory);
  snprintf(missing, sizeof(missing), "%s/missing.sprx", directory);
  write_file(ng_path, ng, sizeof(ng));
  write_file(upstream_path, upstream, sizeof(upstream));
  // The marker straddles the 64 KiB read boundary.
  size_t split_size = 64u * 1024u + 32u;
  unsigned char *split = calloc(1, split_size);
  assert(split);
  memcpy(split + 64u * 1024u - 5u, "ampr-emu-ng 9", 13);
  write_file(split_path, split, split_size);
  free(split);
  assert(sm_ampr_file_is_ng(ng_path) == 1);
  assert(sm_ampr_file_is_ng(upstream_path) == 0);
  assert(sm_ampr_file_is_ng(split_path) == 1);
  assert(sm_ampr_file_is_ng(missing) == -1);

  // PR #200: never replace ampr-emu-ng with upstream ampr_emu (or vice versa).
  assert(!sm_ampr_emulator_replacement_allowed("libSceAmpr.sprx", 1, 0));
  assert(!sm_ampr_emulator_replacement_allowed("libsceampr.sprx", 0, 1));
  assert(sm_ampr_emulator_replacement_allowed("libSceAmpr.sprx", 1, 1));
  assert(sm_ampr_emulator_replacement_allowed("libSceAmpr.sprx", 0, 0));
  assert(!sm_ampr_emulator_replacement_allowed("libSceAmpr.sprx", -1, 1));
  assert(!sm_ampr_emulator_replacement_allowed("libSceAmpr.sprx", 1, -1));
  assert(sm_ampr_emulator_replacement_allowed("libScePlayGo.sprx", 1, 0));
  assert(sm_ampr_emulator_replacement_allowed(
      "libSceAppContent.sprx", sm_ampr_file_is_ng(ng_path),
      sm_ampr_file_is_ng(upstream_path)));
  assert(!sm_ampr_emulator_replacement_allowed(
      "libSceAmpr.sprx", sm_ampr_file_is_ng(ng_path),
      sm_ampr_file_is_ng(upstream_path)));

  assert(unlink(ng_path) == 0 && unlink(upstream_path) == 0 &&
         unlink(split_path) == 0 && rmdir(directory) == 0);
}

int main(void) {
  test_classify_state();
  test_quit_and_http();
  test_legacy_mounts();
  test_adoption();
  test_ledger();
  test_request_scan();
  test_redirect();
  test_failures();
  test_auto_decide();
  test_ampr();
  puts("unimount logic: takeover, adoption, ledger, compat listener, auto-mount "
       "and AMPR selection passed");
  return 0;
}
