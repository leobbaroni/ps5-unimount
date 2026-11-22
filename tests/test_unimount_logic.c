// Host regression test for the UniMount decision helpers in
// src/sm_unimount_logic.c: Kraken takeover, the compatible listener,
// automatic PKG mounting, adoption, the sandbox session record and its
// adoption checks, and AMPR-emu-NG selection.
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

static void test_startup_wait(void) {
  for (int predecessor = 0; predecessor < 2; ++predecessor) {
    sm_startup_facts_t f = {.predecessor = predecessor != 0};
    // No application: start (and replace the predecessor) at once.
    assert(sm_startup_decide(&f) == SM_STARTUP_PROCEED);
    // Unmanaged app (retail game, system app): no delay.
    f.app_running = true;
    f.title_known = true;
    assert(sm_startup_decide(&f) == SM_STARTUP_PROCEED);
    // Managed title: stay passive until it is closed.
    f.managed = true;
    assert(sm_startup_decide(&f) == SM_STARTUP_WAIT);
    // Unknown title: it may be ours.
    f.title_known = false;
    f.managed = false;
    assert(sm_startup_decide(&f) == SM_STARTUP_WAIT);
    // Stale facts of a closed app do not matter.
    f.app_running = false;
    f.managed = true;
    assert(sm_startup_decide(&f) == SM_STARTUP_PROCEED);
  }
  assert(sm_startup_decide(NULL) == SM_STARTUP_PROCEED);

  // Predecessor still alive after the takeover timeout: never run alongside.
  assert(sm_restart_timeout_decide(true) == SM_RESTART_WAIT_FOR_APP);
  assert(sm_restart_timeout_decide(false) == SM_RESTART_GIVE_UP);
}

static void test_wait_notice(void) {
  const uint64_t s = 1000000ull;
  sm_wait_notice_state_t st;

  // A live previous copy: the "starts later" notice once, no reminders.
  sm_wait_notice_init(&st);
  assert(sm_wait_notice_decide(&st, true, 5, 1 * s) == SM_WAIT_NOTICE_WAITING);
  assert(sm_wait_notice_decide(&st, true, 9, 4 * s) == SM_WAIT_NOTICE_NONE);
  // It goes away during the wait: the blocking notice once.
  assert(sm_wait_notice_decide(&st, false, 9, 7 * s) == SM_WAIT_NOTICE_BLOCKING);
  assert(sm_wait_notice_decide(&st, false, 9, 10 * s) == SM_WAIT_NOTICE_NONE);
  // A new console error: reminder, then at most one per 30 s.
  assert(sm_wait_notice_decide(&st, false, 10, 13 * s) ==
         SM_WAIT_NOTICE_REMINDER);
  assert(sm_wait_notice_decide(&st, false, 11, 16 * s) == SM_WAIT_NOTICE_NONE);
  // A throttled error does not come back later by itself.
  assert(sm_wait_notice_decide(&st, false, 11, 50 * s) == SM_WAIT_NOTICE_NONE);
  assert(sm_wait_notice_decide(&st, false, 12, 53 * s) ==
         SM_WAIT_NOTICE_REMINDER);
  // A shrinking history (rotation) only moves the baseline.
  assert(sm_wait_notice_decide(&st, false, 2, 90 * s) == SM_WAIT_NOTICE_NONE);
  assert(sm_wait_notice_decide(&st, false, 3, 93 * s) ==
         SM_WAIT_NOTICE_REMINDER);
  // Unreadable history: no reminder, the baseline is kept.
  assert(sm_wait_notice_decide(&st, false, -1, 130 * s) == SM_WAIT_NOTICE_NONE);
  assert(sm_wait_notice_decide(&st, false, 3, 133 * s) == SM_WAIT_NOTICE_NONE);

  // No previous copy from the start: the blocking notice first; a history
  // unreadable at first gets its baseline later.
  sm_wait_notice_init(&st);
  assert(sm_wait_notice_decide(&st, false, -1, 1 * s) ==
         SM_WAIT_NOTICE_BLOCKING);
  assert(sm_wait_notice_decide(&st, false, 4, 4 * s) == SM_WAIT_NOTICE_NONE);
  assert(sm_wait_notice_decide(&st, false, 5, 7 * s) ==
         SM_WAIT_NOTICE_REMINDER);
  // A previous copy appears (an adopting copy): announced once, no reminder.
  assert(sm_wait_notice_decide(&st, true, 8, 10 * s) == SM_WAIT_NOTICE_WAITING);
  assert(sm_wait_notice_decide(&st, true, 9, 13 * s) == SM_WAIT_NOTICE_NONE);
  // Gone again: blocking notice again, fresh baseline.
  assert(sm_wait_notice_decide(&st, false, 9, 16 * s) ==
         SM_WAIT_NOTICE_BLOCKING);
  assert(sm_wait_notice_decide(&st, false, 9, 19 * s) == SM_WAIT_NOTICE_NONE);
}

static void test_legacy_tile_param(void) {
  static const char shortcut[] =
      "{\"applicationCategoryType\": 66048, \"titleId\": \"FAKE10101\", "
      "\"deeplinkUri\": \"http://127.0.0.1:10101/\"}";
  size_t length = sizeof(shortcut) - 1u;
  assert(sm_legacy_tile_param_matches(shortcut, length, "FAKE10101"));
  // Another title id, or a cut-off file, does not match.
  assert(!sm_legacy_tile_param_matches(shortcut, length, "KRKN00001"));
  assert(!sm_legacy_tile_param_matches(shortcut, length - 20u, "FAKE10101"));
  // A regular title (no deeplink, or not a local address) does not match.
  static const char game[] =
      "{\"titleId\": \"PPSA01234\", \"localizedParameters\": {}}";
  assert(!sm_legacy_tile_param_matches(game, sizeof(game) - 1u, "PPSA01234"));
  static const char remote[] =
      "{\"titleId\": \"KRKN00001\", \"deeplinkUri\": \"http://10.0.0.1:1/\"}";
  assert(!sm_legacy_tile_param_matches(remote, sizeof(remote) - 1u,
                                       "KRKN00001"));
  // Embedded NUL bytes do not end the search early.
  static const char nul[] =
      "{\0\"deeplinkUri\": \"http://127.0.0.1:8080/\", \"titleId\": "
      "\"KRKN00001\"}";
  assert(sm_legacy_tile_param_matches(nul, sizeof(nul) - 1u, "KRKN00001"));
  assert(!sm_legacy_tile_param_matches(NULL, 0, "KRKN00001"));
  assert(!sm_legacy_tile_param_matches(shortcut, length, ""));
}

static void test_session_kind(void) {
  static const char app0[] = "/mnt/sandbox/PPSA01234_000/app0";
  // Plain directory aliases at the sandbox root.
  assert(sm_sandbox_layer_kind(false, "/mnt/sandbox/PPSA01234_000/data", app0) ==
         SM_LAYER_ALIAS);
  assert(sm_sandbox_layer_kind(false, "/mnt/sandbox/PPSA01234_000/mnt/usb0",
                               app0) == SM_LAYER_ALIAS);
  assert(sm_sandbox_layer_kind(
             false, "/mnt/sandbox/PPSA01234_000/preinst/common/font", app0) ==
         SM_LAYER_ALIAS);
  // A sibling whose name only starts like app0 is not inside it.
  assert(sm_sandbox_layer_kind(false, "/mnt/sandbox/PPSA01234_000/app0x",
                               app0) == SM_LAYER_ALIAS);
  // fakelib/fakelib2 unionfs on common/lib, app0 cover, app0/kraken.
  assert(sm_sandbox_layer_kind(true, "/mnt/sandbox/PPSA01234_000/x/common/lib",
                               app0) == SM_LAYER_OVERLAY);
  assert(sm_sandbox_layer_kind(false, app0, app0) == SM_LAYER_OVERLAY);
  assert(sm_sandbox_layer_kind(false, "/mnt/sandbox/PPSA01234_000/app0/kraken",
                               app0) == SM_LAYER_OVERLAY);
  // Unknown app0: only the filesystem type decides.
  assert(sm_sandbox_layer_kind(false, app0, "") == SM_LAYER_ALIAS);
  assert(sm_sandbox_layer_kind(false, app0, NULL) == SM_LAYER_ALIAS);

  const sm_layer_kind_t image[] = {SM_LAYER_ALIAS, SM_LAYER_ALIAS,
                                   SM_LAYER_ALIAS};
  const sm_layer_kind_t kraken[] = {SM_LAYER_OVERLAY, SM_LAYER_ALIAS,
                                    SM_LAYER_ALIAS, SM_LAYER_OVERLAY,
                                    SM_LAYER_OVERLAY, SM_LAYER_ALIAS};
  assert(sm_session_kind(NULL, 0, false) == SM_SESSION_NONE);
  assert(sm_session_kind(image, 3, false) == SM_SESSION_ALIASES);
  assert(sm_session_kind(kraken, 6, false) == SM_SESSION_OVERLAY);
  // NSFS backport redirects count as an overlay, with or without aliases.
  assert(sm_session_kind(image, 3, true) == SM_SESSION_OVERLAY);
  assert(sm_session_kind(NULL, 0, true) == SM_SESSION_OVERLAY);
}

static void test_stop_decide(void) {
  const sm_session_kind_t kinds[] = {SM_SESSION_NONE, SM_SESSION_ALIASES,
                                     SM_SESSION_OVERLAY};
  for (size_t k = 0; k < 3; ++k) {
    bool overlay = kinds[k] == SM_SESSION_OVERLAY;
    for (int signal = 0; signal < 2; ++signal) {
      sm_stop_facts_t f = {.by_signal = signal != 0, .session = kinds[k]};
      // No request, with or without a game.
      assert(sm_stop_decide(&f) == SM_STOP_NONE);
      f.game_active = true;
      assert(sm_stop_decide(&f) == SM_STOP_NONE);
      // First request without an active game: normal stop.
      f.requests = 1;
      f.game_active = false;
      assert(sm_stop_decide(&f) == SM_STOP_NOW);
      // First request with a game. STOP file: always defer. Signal (SIGKILL
      // may follow): defer only for an overlay session.
      f.game_active = true;
      assert(sm_stop_decide(&f) ==
             (signal && !overlay ? SM_STOP_NOW : SM_STOP_DEFER));
      // Second request: stop now; keep only an overlay session's layers.
      f.requests = 2;
      assert(sm_stop_decide(&f) == (overlay ? SM_STOP_NOW_KEEP : SM_STOP_NOW));
      f.requests = 9;
      assert(sm_stop_decide(&f) == (overlay ? SM_STOP_NOW_KEEP : SM_STOP_NOW));
      // Console shutdown / scanner failure cannot wait.
      f.requests = signal ? 1u : 0u;
      f.immediate = true;
      assert(sm_stop_decide(&f) == (overlay ? SM_STOP_NOW_KEEP : SM_STOP_NOW));
      f.game_active = false;
      assert(sm_stop_decide(&f) == SM_STOP_NOW);
    }
  }
  assert(sm_stop_decide(NULL) == SM_STOP_NONE);
}

static void test_kept_layers(void) {
  sm_layer_owner_facts_t f = {0};
  // Nothing recorded: nothing to keep.
  assert(!sm_shutdown_keeps_game_layers(&f));
  assert(!sm_shutdown_keeps_game_layers(NULL));
  // Alias-only session of a live game: normal (non-forced) teardown.
  f.has_layers = true;
  f.pid = 298;
  f.pid_alive = true;
  f.sandbox_exists = true;
  f.handoff_pending = true;
  assert(!sm_shutdown_keeps_game_layers(&f));
  f = (sm_layer_owner_facts_t){.overlay = true};
  f.has_layers = true;
  // Pre-spawn session (sandbox ready, no process yet).
  assert(!sm_shutdown_keeps_game_layers(&f));
  f.sandbox_exists = true;
  assert(sm_shutdown_keeps_game_layers(&f));
  // Live process.
  f.pid = 298;
  f.pid_alive = true;
  f.sandbox_exists = false;
  assert(sm_shutdown_keeps_game_layers(&f));
  // Exited process, no handoff: normal teardown, even with a lingering sandbox.
  f.pid_alive = false;
  assert(!sm_shutdown_keeps_game_layers(&f));
  f.sandbox_exists = true;
  assert(!sm_shutdown_keeps_game_layers(&f));
  // Exited process during a same-app handoff in that sandbox: keep.
  f.handoff_pending = true;
  assert(sm_shutdown_keeps_game_layers(&f));
  f.sandbox_exists = false;
  assert(!sm_shutdown_keeps_game_layers(&f));
  f.has_layers = false;
  f.pid_alive = true;
  assert(!sm_shutdown_keeps_game_layers(&f));

  // /system_ex/app/<TID> stacks kept for the live titles only.
  char kept[3][MAX_TITLE_ID];
  size_t count = 0;
  assert(!sm_title_list_contains((const char (*)[MAX_TITLE_ID])kept, count,
                                 "PPSA01234"));
  assert(sm_title_list_add(kept, &count, 3, "PPSA01234"));
  assert(sm_title_list_add(kept, &count, 3, "PPSA01234") && count == 1);
  assert(!sm_title_list_add(kept, &count, 3, "") && count == 1);
  assert(!sm_title_list_add(kept, &count, 3, "ppsa01234") && count == 1);
  assert(!sm_title_list_add(kept, &count, 3, NULL) && count == 1);
  assert(sm_title_list_add(kept, &count, 3, "CUSA00001") && count == 2);
  assert(sm_title_list_add(kept, &count, 3, "PPSA99999") && count == 3);
  assert(!sm_title_list_add(kept, &count, 3, "PPSA11111") && count == 3);
  const char (*view)[MAX_TITLE_ID] = (const char (*)[MAX_TITLE_ID])kept;
  assert(sm_title_list_contains(view, count, "PPSA01234"));
  assert(sm_title_list_contains(view, count, "CUSA00001"));
  assert(!sm_title_list_contains(view, count, "PPSA11111"));
  assert(!sm_title_list_contains(view, count, NULL));
}

// --- Session record and adoption ---

#define SANDBOX "/mnt/sandbox/PPSA01234_000"

static void add_layer(sm_session_record_t *record, const char *label,
                      const char *source, const char *target, int32_t id,
                      bool read_only) {
  sm_session_layer_t *layer = &record->layers[record->layer_count++];
  snprintf(layer->label, sizeof(layer->label), "%s", label);
  snprintf(layer->fstype, sizeof(layer->fstype), "%s",
           strcmp(label, "directory") == 0 ? "nullfs" : "unionfs");
  snprintf(layer->source, sizeof(layer->source), "%s", source);
  snprintf(layer->target, sizeof(layer->target), "%s", target);
  layer->fsid[0] = id;
  layer->fsid[1] = 7;
  layer->read_only = read_only;
  layer->created = strcmp(label, "directory") == 0;
}

static void add_parent(sm_session_record_t *record, const char *target) {
  sm_session_layer_t *layer = &record->layers[record->layer_count++];
  snprintf(layer->label, sizeof(layer->label), SM_SESSION_PARENT_LABEL);
  snprintf(layer->fstype, sizeof(layer->fstype), SM_SESSION_PARENT_FSTYPE);
  snprintf(layer->target, sizeof(layer->target), "%s", target);
  layer->created = true;
}

// A debug-.pkg session: fakelib2 overlay, /data alias, app0 cover,
// app0/kraken, the font alias with its two created parent directories,
// plus one NSFS redirect set.
static sm_session_record_t *sample_record(void) {
  sm_session_record_t *record = calloc(1, sizeof(*record));
  assert(record);
  record->boot_time = 1700000000;
  record->owner_pid = 500;
  record->game_pid = 600;
  record->app_id = 0x60000123u;
  record->complete = true;
  snprintf(record->title_id, sizeof(record->title_id), "PPSA01234");
  snprintf(record->sandbox_app0, sizeof(record->sandbox_app0), SANDBOX "/app0");
  snprintf(record->lib_mount, sizeof(record->lib_mount), SANDBOX "/common/lib");
  add_layer(record, "fakelib2", "/data/shadowmount/kraken/PPSA01234/fakelib",
            SANDBOX "/common/lib", 101, false);
  add_layer(record, "directory", "/data", SANDBOX "/data", 102, false);
  add_layer(record, "directory", "/system_ex/app/PPSA01234", SANDBOX "/app0",
            103, false);
  add_layer(record, "directory", "/mnt/usb0/games", SANDBOX "/app0/kraken", 104,
            true);
  add_parent(record, SANDBOX "/preinst");
  add_parent(record, SANDBOX "/preinst/common");
  add_layer(record, "directory", "/preinst/common/font",
            SANDBOX "/preinst/common/font", 106, true);
  sm_session_redirects_t *set = &record->redirects[record->redirect_count++];
  snprintf(set->source, sizeof(set->source), "/data/homebrew/backports/PPSA01234");
  snprintf(set->target, sizeof(set->target), SANDBOX "/app0");
  set->device = 77;
  set->fsid[0] = 200;
  set->fsid[1] = 3;
  assert(sm_session_redirects_add(set, SANDBOX "/app0/sce_module/libc.prx"));
  assert(sm_session_redirects_add(set, SANDBOX "/app0/sce_module/extra"));
  return record;
}

#define MOUNT(type, src, dst, id0, id1) \
  {.fstype = type, .from = src, .on = dst, .fsid = {id0, id1}, .from_size = 0}

static sm_mount_entry_t g_mounts[16];
static size_t g_mount_count;

static void sample_mounts(void) {
  const sm_mount_entry_t mounts[] = {
      MOUNT("ufs", "/dev/ssd0.user", "/user", 1, 1),
      MOUNT("nullfs", "/user/data", "/data", 2, 1),
      MOUNT("nsfs", "nsfs", SANDBOX "/app0", 200, 3),
      MOUNT("nullfs", "/system/common/lib", SANDBOX "/common/lib", 3, 1),
      MOUNT("unionfs", "<above>:/data/shadowmount/kraken/PPSA01234/fakelib",
            SANDBOX "/common/lib", 101, 7),
      MOUNT("nullfs", "/data", SANDBOX "/data", 102, 7),
      MOUNT("nullfs", "/system_ex/app/PPSA01234", SANDBOX "/app0", 103, 7),
      MOUNT("nullfs", "/mnt/usb0/games", SANDBOX "/app0/kraken", 104, 7),
      MOUNT("exfatfs", "/dev/da0s1", "/mnt/usb0", 5, 1),
      MOUNT("nullfs", "/preinst/common/font", SANDBOX "/preinst/common/font",
            106, 7),
  };
  memcpy(g_mounts, mounts, sizeof(mounts));
  g_mount_count = sizeof(mounts) / sizeof(mounts[0]);
}

static sm_adopt_live_t sample_live(void) {
  sm_adopt_live_t live = {.boot_time = 1700000000,
                          .game_alive = true,
                          .game_app_id = 0x60000123u,
                          .running_app_id = 0x60000123u,
                          .mounts = g_mounts,
                          .mount_count = g_mount_count};
  snprintf(live.game_title_id, sizeof(live.game_title_id), "PPSA01234");
  snprintf(live.running_title_id, sizeof(live.running_title_id), "PPSA01234");
  return live;
}

static const char *refusal(const sm_session_record_t *record,
                           const sm_adopt_live_t *live) {
  static char buf[MAX_PATH + 128];
  const char *reason = NULL;
  bool adopt = sm_session_adopt_decide(record, live, buf, sizeof(buf), &reason);
  assert(adopt == (reason == NULL));
  return reason;
}

static void test_session_record_format(void) {
  sm_session_record_t *record = sample_record();
  sm_session_record_t *parsed = calloc(1, sizeof(*parsed));
  assert(parsed);
  char *text = NULL;
  const char *why = NULL;
  assert(sm_session_record_format(record, "0.1.0", &text));
  assert(strstr(text, "\"format\":1") && strstr(text, "\"title_id\":\"PPSA01234\""));
  assert(sm_session_record_parse(text, strlen(text), parsed, &why) && !why);
  assert(parsed->boot_time == record->boot_time &&
         parsed->owner_pid == 500 && parsed->game_pid == 600 &&
         parsed->app_id == 0x60000123u && parsed->complete);
  assert(strcmp(parsed->lib_mount, record->lib_mount) == 0 &&
         strcmp(parsed->sandbox_app0, record->sandbox_app0) == 0);
  // Every entry, parent directories included, in the original order.
  assert(parsed->layer_count == 7 && parsed->redirect_count == 1);
  assert(sm_session_layer_is_parent(&parsed->layers[4]) &&
         sm_session_layer_is_parent(&parsed->layers[5]) &&
         !sm_session_layer_is_parent(&parsed->layers[6]) &&
         strcmp(parsed->layers[4].target, SANDBOX "/preinst") == 0 &&
         strcmp(parsed->layers[5].target, SANDBOX "/preinst/common") == 0 &&
         parsed->layers[4].source[0] == 0 && parsed->layers[4].created &&
         strcmp(parsed->layers[4].fstype, SM_SESSION_PARENT_FSTYPE) == 0);
  for (size_t i = 0; i < record->layer_count; ++i) {
    const sm_session_layer_t *a = &record->layers[i], *b = &parsed->layers[i];
    assert(strcmp(a->label, b->label) == 0 && strcmp(a->fstype, b->fstype) == 0 &&
           strcmp(a->source, b->source) == 0 && strcmp(a->target, b->target) == 0 &&
           a->fsid[0] == b->fsid[0] && a->fsid[1] == b->fsid[1] &&
           a->read_only == b->read_only && a->created == b->created);
  }
  const sm_session_redirects_t *set = &parsed->redirects[0];
  assert(set->device == 77 && set->fsid[0] == 200 && set->fsid[1] == 3 &&
         set->count == 2 && set->bytes == record->redirects[0].bytes &&
         memcmp(set->storage, record->redirects[0].storage, set->bytes) == 0);
  assert(sm_session_record_units(parsed) == 6);  // parents not counted
  assert(sm_session_record_kind(parsed) == SM_SESSION_OVERLAY);

  // Negative fsid words and a large device survive the round trip.
  record->layers[0].fsid[0] = INT32_MIN;
  record->redirects[0].device = UINT64_C(0xFFFFFFFF00000001);
  free(text);
  assert(sm_session_record_format(record, "0.1.0", &text));
  assert(sm_session_record_parse(text, strlen(text), parsed, &why));
  assert(parsed->layers[0].fsid[0] == INT32_MIN &&
         parsed->redirects[0].device == UINT64_C(0xFFFFFFFF00000001));

  // Truncated or malformed JSON.
  for (size_t cut = 0; cut < strlen(text); cut += 37)
    assert(!sm_session_record_parse(text, cut, parsed, &why) && why);
  assert(!sm_session_record_parse(NULL, 0, parsed, &why));
  assert(!sm_session_record_parse("garbage", 7, parsed, &why) &&
         strstr(why, "malformed"));
  assert(!sm_session_record_parse("[]", 2, parsed, &why));
  assert(!sm_session_record_parse("{}", 2, parsed, &why));
  size_t length = strlen(text);
  char *trailing = malloc(length + 3);
  assert(trailing);
  memcpy(trailing, text, length);
  memcpy(trailing + length, " x", 3);
  assert(!sm_session_record_parse(trailing, length + 2, parsed, &why));
  memcpy(trailing + length, " \n", 3);
  assert(sm_session_record_parse(trailing, length + 2, parsed, &why));
  free(trailing);

  // A record from another format version is not read.
  char *other = strdup(text);
  assert(other);
  char *format = strstr(other, "\"format\":1");
  format[9] = '2';
  assert(!sm_session_record_parse(other, strlen(other), parsed, &why) &&
         strstr(why, "format"));
  free(other);

  // Wrong types, unknown label, fstype that does not fit the label.
  static const char *const replacements[][2] = {
      {"\"game_pid\":600", "\"game_pid\":\"600\""},
      {"\"complete\":true", "\"complete\":1"},
      {"\"label\":\"fakelib2\"", "\"label\":\"other\""},
      {"\"fstype\":\"unionfs\"", "\"fstype\":\"nullfs\""},
      {"\"fsid\":[101,7]", "\"fsid\":[101]"},
      {"\"fsid\":[101,7]", "\"fsid\":[4294967296,7]"},
      {"\"title_id\":\"PPSA01234\"", "\"title_id\":\"PPSA0123\""},
      {"\"target\":\"" SANDBOX "/data\"", "\"target\":\"/data\""},
      {"\"target\":\"" SANDBOX "/data\"",
       "\"target\":\"/mnt/sandbox/PPSA99999_000/data\""},
      // Parent directories: inside the sandbox, no source, created.
      {"\"target\":\"" SANDBOX "/preinst\"", "\"target\":\"/preinst\""},
      {"\"source\":\"\",\"target\":\"" SANDBOX "/preinst\"",
       "\"source\":\"/preinst\",\"target\":\"" SANDBOX "/preinst\""},
      {"\"label\":\"parent\",\"fstype\":\"none\"",
       "\"label\":\"parent\",\"fstype\":\"nullfs\""},
      {"\"fstype\":\"nullfs\",\"source\":\"/data\"",
       "\"fstype\":\"nullfs\",\"source\":\"\""},
  };
  record->layers[0].fsid[0] = 101;
  record->redirects[0].device = 77;
  free(text);
  assert(sm_session_record_format(record, "0.1.0", &text));
  for (size_t i = 0; i < sizeof(replacements) / sizeof(replacements[0]); ++i) {
    const char *at = strstr(text, replacements[i][0]);
    assert(at);
    char edited[16384];
    snprintf(edited, sizeof(edited), "%.*s%s%s", (int)(at - text), text,
             replacements[i][1], at + strlen(replacements[i][0]));
    assert(!sm_session_record_parse(edited, strlen(edited), parsed, &why) && why);
  }

  // Over-long strings.
  char *long_text = malloc(strlen(text) + 2 * MAX_PATH);
  assert(long_text);
  char long_path[MAX_PATH + 8];
  memset(long_path, 'a', sizeof(long_path) - 1);
  long_path[0] = '/';
  long_path[sizeof(long_path) - 1] = '\0';
  const char *source = strstr(text, "\"source\":\"/data\"");
  assert(source);
  sprintf(long_text, "%.*s\"source\":\"%s\"%s", (int)(source - text), text,
          long_path, source + strlen("\"source\":\"/data\""));
  assert(!sm_session_record_parse(long_text, strlen(long_text), parsed, &why) &&
         strstr(why, "over-long"));
  const char *destination = strstr(text, "\"destinations\":[\"");
  assert(destination);
  sprintf(long_text, "%.*s\"destinations\":[\"%s\",%s", (int)(destination - text),
          text, long_path, destination + strlen("\"destinations\":["));
  assert(!sm_session_record_parse(long_text, strlen(long_text), parsed, &why) &&
         strstr(why, "over-long"));
  free(long_text);
  free(text);

  // Too many layers, redirect sets and redirects.
  sm_session_record_t *big = sample_record();
  big->layer_count = 0;
  for (size_t i = 0; i < SM_SESSION_MAX_LAYERS; ++i) {
    char target[64];
    snprintf(target, sizeof(target), SANDBOX "/mnt/usb%zu", i);
    add_layer(big, "directory", "/mnt/usb0", target, (int32_t)(300 + i), false);
  }
  assert(sm_session_record_format(big, "0.1.0", &text));
  assert(sm_session_record_parse(text, strlen(text), parsed, &why));
  const char *layers = strstr(text, "\"layers\":[");
  assert(layers);
  layers += strlen("\"layers\":[");
  const char *first_end = strstr(layers, "},");
  assert(first_end);
  size_t first_len = (size_t)(first_end - layers) + 2u;
  char *more = malloc(strlen(text) + first_len + 1);
  assert(more);
  sprintf(more, "%.*s%.*s%s", (int)(layers - text), text, (int)first_len, layers,
          layers);
  assert(!sm_session_record_parse(more, strlen(more), parsed, &why) &&
         strstr(why, "too many layers"));
  free(more);
  free(text);

  big->layer_count = 1;
  big->redirect_count = 2;
  big->redirects[1] = big->redirects[0];
  assert(sm_session_record_format(big, "0.1.0", &text));
  assert(sm_session_record_parse(text, strlen(text), parsed, &why));
  const char *sets = strstr(text, "\"redirects\":[");
  assert(sets);
  sets += strlen("\"redirects\":[");
  size_t set_len = strlen(sets) - 2u;  // without the closing "]}"
  more = malloc(strlen(text) + set_len + 2);
  assert(more);
  sprintf(more, "%.*s%.*s,%s", (int)(sets - text), text, (int)set_len, sets, sets);
  assert(!sm_session_record_parse(more, strlen(more), parsed, &why) &&
         strstr(why, "too many redirect sets"));
  free(more);
  free(text);

  big->redirect_count = 1;
  memset(&big->redirects[0].storage, 0, sizeof(big->redirects[0].storage));
  big->redirects[0].count = big->redirects[0].bytes = 0;
  for (size_t i = 0; i < SM_SESSION_MAX_REDIRECTS; ++i) {
    char path[64];
    snprintf(path, sizeof(path), SANDBOX "/app0/f%zu", i);
    assert(sm_session_redirects_add(&big->redirects[0], path));
  }
  assert(!sm_session_redirects_add(&big->redirects[0], SANDBOX "/app0/x"));
  assert(sm_session_record_format(big, "0.1.0", &text));
  assert(sm_session_record_parse(text, strlen(text), parsed, &why));
  const char *list = strstr(text, "\"destinations\":[");
  assert(list);
  list += strlen("\"destinations\":[");
  more = malloc(strlen(text) + 64);
  assert(more);
  sprintf(more, "%.*s\"" SANDBOX "/app0/extra\",%s", (int)(list - text), text,
          list);
  assert(!sm_session_record_parse(more, strlen(more), parsed, &why) &&
         strstr(why, "too many redirects"));
  free(more);
  free(text);

  // Storage overflow and invalid destinations in the packed set.
  sm_session_redirects_t *set_full = calloc(1, sizeof(*set_full));
  assert(set_full);
  assert(!sm_session_redirects_add(set_full, ""));
  assert(!sm_session_redirects_add(set_full, long_path));
  char half[MAX_PATH];
  memset(half, 'b', sizeof(half) - 1);
  half[sizeof(half) - 1] = '\0';
  size_t added = 0;
  while (sm_session_redirects_add(set_full, half))
    ++added;
  assert(added == SM_SESSION_REDIRECT_BYTES / MAX_PATH &&
         set_full->count == added);
  free(set_full);

  free(big);
  free(parsed);
  free(record);
}

static void test_session_adopt_decide(void) {
  sm_session_record_t *record = sample_record();
  sample_mounts();
  sm_adopt_live_t live = sample_live();

  // Valid match, with unrelated mounts around ours.
  assert(refusal(record, &live) == NULL);

  // A long source shows truncated in f_mntfromname (MNAMELEN).
  g_mounts[7].from = "/mnt/usb0/ga";
  assert(refusal(record, &live) && strstr(refusal(record, &live), "source"));
  g_mounts[7].from_size = 13;
  assert(refusal(record, &live) == NULL);
  sample_mounts();

  // One layer missing.
  g_mounts[5] = g_mounts[--g_mount_count];
  live = sample_live();
  const char *why = refusal(record, &live);
  assert(why && strstr(why, "layer 2") && strstr(why, "not mounted"));

  // Same path, different filesystem id.
  sample_mounts();
  g_mounts[5].fsid[0] = 999;
  why = refusal(record, &live);
  assert(why && strstr(why, "different filesystem id"));

  // Different source or type.
  sample_mounts();
  g_mounts[6].from = "/system_ex/app/PPSA99999";
  why = refusal(record, &live);
  assert(why && strstr(why, "layer 3") && strstr(why, "different source"));
  sample_mounts();
  g_mounts[4].fstype = "nullfs";
  why = refusal(record, &live);
  assert(why && strstr(why, "different type"));

  // A newer mount covers a layer; a duplicated id is not trusted.
  sample_mounts();
  g_mounts[g_mount_count++] =
      (sm_mount_entry_t)MOUNT("nullfs", "/data", SANDBOX "/data", 555, 1);
  live = sample_live();
  why = refusal(record, &live);
  assert(why && strstr(why, "covered"));
  sample_mounts();
  g_mounts[g_mount_count++] =
      (sm_mount_entry_t)MOUNT("nullfs", "/elsewhere", "/mnt/elsewhere", 102, 7);
  live = sample_live();
  why = refusal(record, &live);
  assert(why && strstr(why, "not unique"));

  // The NSFS namespace of the redirects.
  sample_mounts();
  live = sample_live();
  g_mounts[2].fsid[0] = 201;
  why = refusal(record, &live);
  assert(why && strstr(why, "redirect namespace"));
  sample_mounts();
  g_mounts[2].fstype = "nullfs";
  why = refusal(record, &live);
  assert(why && strstr(why, "redirect namespace") && strstr(why, "not mounted"));
  sample_mounts();

  // Different boot, dead or foreign process, another running application.
  live.boot_time = 1700000500;
  assert(strstr(refusal(record, &live), "another boot"));
  live = sample_live();
  live.boot_time = 0;
  assert(strstr(refusal(record, &live), "another boot"));
  live = sample_live();
  live.game_alive = false;
  assert(strstr(refusal(record, &live), "not running"));
  live = sample_live();
  live.game_app_id = 0x60000999u;
  assert(strstr(refusal(record, &live), "recorded application"));
  live = sample_live();
  snprintf(live.game_title_id, sizeof(live.game_title_id), "PPSA99999");
  assert(strstr(refusal(record, &live), "another title"));
  live = sample_live();
  live.running_app_id = 0x60000999u;
  assert(strstr(refusal(record, &live), "not the running application"));
  live = sample_live();
  live.running_app_id = 0;
  assert(strstr(refusal(record, &live), "not the running application"));
  live = sample_live();
  snprintf(live.running_title_id, sizeof(live.running_title_id), "PPSA99999");
  assert(strstr(refusal(record, &live), "another title"));
  live = sample_live();
  live.running_title_id[0] = '\0';  // title of the running app unknown
  assert(refusal(record, &live) == NULL);

  // Records that cannot be adopted on their own.
  live = sample_live();
  record->game_pid = 0;
  assert(strstr(refusal(record, &live), "no game process"));
  record->game_pid = 600;
  record->complete = false;
  assert(strstr(refusal(record, &live), "incomplete"));
  record->complete = true;
  record->app_id = 0;
  live.game_app_id = 0;
  live.running_app_id = 0;
  assert(strstr(refusal(record, &live), "recorded application"));
  sm_session_record_t *empty = calloc(1, sizeof(*empty));
  assert(empty);
  *empty = *record;
  empty->layer_count = empty->redirect_count = 0;
  assert(strstr(refusal(empty, &live), "empty session record"));
  assert(sm_session_record_kind(empty) == SM_SESSION_NONE);
  char *text = NULL;
  assert(sm_session_record_format(empty, "0.1.0", &text));
  assert(sm_session_record_parse(text, strlen(text), empty, &why));
  assert(sm_session_record_units(empty) == 0);
  free(text);
  assert(strstr(refusal(NULL, &live), "no session record"));

  // An image session: only directory aliases outside app0.
  sm_session_record_t *aliases = sample_record();
  aliases->layer_count = 0;
  aliases->redirect_count = 0;
  add_layer(aliases, "directory", "/data", SANDBOX "/data", 102, false);
  add_layer(aliases, "directory", "/mnt/usb0", SANDBOX "/mnt/usb0", 105, false);
  assert(sm_session_record_kind(aliases) == SM_SESSION_ALIASES);
  sample_mounts();
  g_mounts[g_mount_count++] =
      (sm_mount_entry_t)MOUNT("nullfs", "/mnt/usb0", SANDBOX "/mnt/usb0", 105, 7);
  live = sample_live();
  assert(refusal(aliases, &live) == NULL);

  // Only created parent directories left: no mount to match, still a
  // session to clean (not empty), counted as zero layers.
  sm_session_record_t *parents = sample_record();
  parents->layer_count = 0;
  parents->redirect_count = 0;
  add_parent(parents, SANDBOX "/preinst");
  add_parent(parents, SANDBOX "/preinst/common");
  assert(sm_session_record_units(parents) == 0 &&
         !sm_session_record_empty(parents) &&
         sm_session_record_kind(parents) == SM_SESSION_NONE);
  live = sample_live();
  live.mount_count = 0;
  assert(refusal(parents, &live) == NULL);
  // A parent entry must have been created by the owner.
  parents->layers[0].created = false;
  assert(sm_session_record_format(parents, "0.1.0", &text));
  assert(!sm_session_record_parse(text, strlen(text), empty, &why));
  free(text);
  free(parents);

  free(aliases);
  free(empty);
  free(record);
}

int main(void) {
  test_session_record_format();
  test_session_adopt_decide();
  test_startup_wait();
  test_wait_notice();
  test_legacy_tile_param();
  test_session_kind();
  test_stop_decide();
  test_kept_layers();
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
  puts("unimount logic: session record, adoption checks, startup wait, "
       "wait notices, legacy tiles, session kinds, stop deferral, kept layers, takeover, "
       "adoption, ledger, compat listener, auto-mount and AMPR selection passed");
  return 0;
}
