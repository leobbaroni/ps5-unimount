// Host regression test: compile the production config implementation with
// console-only dependencies stubbed below. No console files are accessed.
#define _GNU_SOURCE
#define SM_PLATFORM_H
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sm_paths.h"
#undef CONFIG_FILE
#undef PROFILES_FILE
#undef LEGACY_AUTOTUNE_FILE
#undef LEGACY_SMB_FILE
#undef CONFIG_BACKUP_DIR
#define CONFIG_FILE "./config.ini"
#define PROFILES_FILE "./profiles.ini"
#define LEGACY_AUTOTUNE_FILE "./autotune.ini"
#define LEGACY_SMB_FILE "./smb.json"
#define CONFIG_BACKUP_DIR "./backup"

static size_t test_strlcpy(char *dst, const char *src, size_t size) {
  size_t length = strlen(src);
  if (size > 0) {
    size_t copied = length < size - 1u ? length : size - 1u;
    memcpy(dst, src, copied);
    dst[copied] = '\0';
  }
  return length;
}
#define strlcpy test_strlcpy

#include "title_id_helpers.inc"
#include "sm_smb.h"
#include "smb_validation.inc"
static bool fail_main_rename;
static int test_rename(const char *source, const char *target) {
  if (fail_main_rename && !strcmp(target, CONFIG_FILE)) { errno = EIO; return -1; }
  return rename(source, target);
}
#define rename test_rename
#include "../src/sm_config_file.c"
#include "../src/sm_config_mount.c"
bool is_under_image_mount_base(const char *path) { (void)path; return false; }

void log_debug(const char *fmt, ...) {
  (void)fmt;
  // Production logging reads config, including while parsing a reload.
  (void)runtime_config();
}

bool sm_l10n_parse_language_id(const char *value, int32_t *out) {
  (void)value;
  *out = SM_LANGUAGE_AUTO;
  return true;
}

const char *sm_l10n_language_name(int32_t language_id) {
  (void)language_id;
  return "auto";
}

const char *attach_backend_name(attach_backend_t backend) {
  (void)backend;
  return "test";
}

const char *get_filename_component(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static atomic_bool reader_stop;
static atomic_uint read_count;
static pthread_barrier_t reader_barrier;

static void write_config(unsigned version) {
  char next_path[MAX_PATH];
  assert(snprintf(next_path, sizeof(next_path), "%s.next", CONFIG_FILE) > 0);
  FILE *file = fopen(next_path, "w");
  assert(file);
  fprintf(file,
          "[config]\nversion=2\n[api]\nport=%u\n"
          "[fakelib]\nglobal_path=/data/version%u\n"
          "[emulators]\npath=/data/version%u\n"
          "[scan]\npath=/data/games%u\n",
          9100u + version, version, version, version);
  assert(fclose(file) == 0);
  assert(rename(next_path, CONFIG_FILE) == 0);
  file = fopen(PROFILES_FILE, "w");
  assert(file);
  fprintf(file, "[config]\nversion=2\n[game:PPSA12345]\nignore_global_fakelib=on\n"
                "[image:game.exfat]\nmount_mode=ro\nsector_size=%u\n", version == 1 ? 512u : 4096u);
  assert(!fclose(file));
}

static void *init_reader(void *unused) {
  (void)unused;
  (void)pthread_barrier_wait(&reader_barrier);
  runtime_config_t cfg = runtime_config();
  assert(cfg.api_port == SM_API_DEFAULT_PORT);
  char path[MAX_PATH];
  assert(get_scan_path(0, path));
  assert(path[0] == '/');
  return NULL;
}

static void *reload_reader(void *unused) {
  (void)unused;
  (void)pthread_barrier_wait(&reader_barrier);
  do {
    runtime_config_t cfg = runtime_config();
    unsigned version = cfg.api_port - 9100u;
    assert(version == 1u || version == 2u);
    char expected[MAX_PATH];
    snprintf(expected, sizeof(expected), "/data/version%u", version);
    assert(strcmp(cfg.global_fakelib_path, expected) == 0);
    assert(strcmp(cfg.emulators_path, expected) == 0);
    assert(cfg.global_fakelib_exclude_title_count == 1u);
    assert(strcmp(cfg.global_fakelib_exclude_title_ids[0], "PPSA12345") == 0);

    char path[MAX_PATH];
    assert(get_scan_path_count() == 3);
    assert(get_custom_scan_path_count() == 1);
    assert(get_scan_path(0, path));
    assert(strcmp(path, "/data/games1") == 0 ||
           strcmp(path, "/data/games2") == 0);
    assert(get_custom_scan_path(0, path));
    assert(strcmp(path, "/data/games1") == 0 ||
           strcmp(path, "/data/games2") == 0);
    bool read_only = false;
    uint32_t sector = 0;
    assert(get_image_mode_override("game.exfat", &read_only) && read_only);
    assert(get_image_sector_size_override("game.exfat", &sector));
    assert(sector == 512u || sector == 4096u);
    assert(is_global_fakelib_excluded_for_title("PPSA12345"));
    atomic_fetch_add(&read_count, 1u);
  } while (!atomic_load(&reader_stop));
  return NULL;
}

static void test_logging_flags(void) {
  for (unsigned mask = 0; mask < 8; ++mask) {
    FILE *file = fopen(CONFIG_FILE, "w");
    assert(file);
    fprintf(file, "[config]\nversion=2\n[general]\ndebug=%u\nquiet_mode=%u\nlibrary_sync_notification=%u\n",
            mask & 1u, (mask >> 1) & 1u, (mask >> 2) & 1u);
    assert(fclose(file) == 0 && load_runtime_config());
    assert(sm_config_debug_enabled() == !!(mask & 1u));
    assert(sm_config_quiet_mode() == !!(mask & 2u));
    assert(sm_config_library_sync_notification() == !!(mask & 4u));
  }
  FILE *file = fopen(CONFIG_FILE, "w");
  assert(file && fputs("[config]\nversion=2\n[general]\nlibrary_sync_notification=off\n", file) >= 0);
  assert(!fclose(file) && load_runtime_config());
  file = fopen(CONFIG_FILE, "w");
  assert(file && fputs("[config]\nversion=2\n[general]\nlibrary_sync_notification=invalid\n", file) >= 0);
  assert(!fclose(file) && !load_runtime_config());
  assert(!sm_config_library_sync_notification());
  file = fopen(CONFIG_FILE, "w");
  assert(file && fputs("[config]\nversion=2\n", file) >= 0);
  assert(!fclose(file) && load_runtime_config());
  assert(sm_config_library_sync_notification());
}

static void test_diagnostics_config(void) {
  FILE *file = fopen(CONFIG_FILE, "w");
  assert(file && fputs("[config]\nversion=2\n", file) >= 0);
  assert(!fclose(file) && load_runtime_config());
  runtime_config_t cfg = runtime_config();
  assert(cfg.diagnostics_enabled);
  assert(cfg.diagnostics_running_confirm_seconds == 20u);
  assert(cfg.diagnostics_error_window_seconds == 120u);
  file = fopen(CONFIG_FILE, "w");
  assert(file && fputs("[config]\nversion=2\n[diagnostics]\nenabled=off\n"
                       "running_confirm_seconds=45\nerror_window_seconds=300\n",
                       file) >= 0);
  assert(!fclose(file) && load_runtime_config());
  cfg = runtime_config();
  assert(!cfg.diagnostics_enabled);
  assert(cfg.diagnostics_running_confirm_seconds == 45u);
  assert(cfg.diagnostics_error_window_seconds == 300u);
  file = fopen(CONFIG_FILE, "w");
  assert(file && fputs("[config]\nversion=2\n[diagnostics]\nrunning_confirm_seconds=1\n",
                       file) >= 0);
  assert(!fclose(file) && !load_runtime_config());
}

static void test_api_allow_launch_config(void) {
  FILE *file = fopen(CONFIG_FILE, "w");
  assert(file && fputs("[config]\nversion=2\n", file) >= 0);
  assert(!fclose(file) && load_runtime_config());
  assert(runtime_config().api_allow_launch);
  file = fopen(CONFIG_FILE, "w");
  assert(file && fputs("[config]\nversion=2\n[api]\nallow_launch=off\n", file) >= 0);
  assert(!fclose(file) && load_runtime_config());
  assert(!runtime_config().api_allow_launch);
  file = fopen(CONFIG_FILE, "w");
  assert(file && fputs("[config]\nversion=2\n[api]\nallow_launch=on\nport=10101\n",
                       file) >= 0);
  assert(!fclose(file) && load_runtime_config());
  assert(runtime_config().api_allow_launch);
  file = fopen(CONFIG_FILE, "w");
  assert(file && fputs("[config]\nversion=2\n[api]\nallow_launch=0\n", file) >= 0);
  assert(!fclose(file) && load_runtime_config());
  assert(!runtime_config().api_allow_launch);
  file = fopen(CONFIG_FILE, "w");
  assert(file && fputs("[config]\nversion=2\n[api]\nallow_launch=maybe\n", file) >= 0);
  assert(!fclose(file) && !load_runtime_config());
  file = fopen(CONFIG_FILE, "w");
  assert(file && fputs("[config]\nversion=2\n", file) >= 0);
  assert(!fclose(file) && load_runtime_config());
  assert(runtime_config().api_allow_launch);
}

static void put_file(const char *path, const char *text) {
  FILE *f = fopen(path, "w");
  assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
}

static char *get_file(const char *path) {
  char *data = NULL;
  size_t size = 0;
  assert(read_bytes(path, &data, &size));
  assert(data);
  return data;
}

static void reset_files(void) {
  const char *paths[] = {CONFIG_FILE, PROFILES_FILE, LEGACY_AUTOTUNE_FILE, LEGACY_SMB_FILE,
    CONFIG_BACKUP_DIR "/config.ini", CONFIG_BACKUP_DIR "/autotune.ini", CONFIG_BACKUP_DIR "/smb.json",
    CONFIG_BACKUP_DIR "/ready.ini", CONFIG_BACKUP_DIR "/profiles-v2.ini"};
  for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i)
    assert(!unlink(paths[i]) || errno == ENOENT);
  assert(!rmdir(CONFIG_BACKUP_DIR) || errno == ENOENT);
}

static void test_migration(void) {
  const char *legacy = "# preserved original\napi_port=11111\nrecursive_scan=1\nscan_depth=1\n"
      "scanpath=/data/first\nscanpath=/data/second\nimage_ro=/mnt/usb0/Game [v1].exfat\n"
      "image_sector=Game [v1].exfat:4096\nfakelib_exclude=PPSA13197\n"
      "fakelib_mode=PPSA13197:emulators\nglobal_fakelib_exclude=PPSA13197\n";
  put_file(CONFIG_FILE, legacy);
  put_file(LEGACY_AUTOTUNE_FILE, "image_sector=Game [v1].exfat:512\n");
  put_file(LEGACY_SMB_FILE, "[{\"name\":\"NAS\",\"path\":\"smb://server/games\","
      "\"username\":\"DOMAIN\\\\user\",\"password\":\" #;\\\"secret\\\\ \",\"active\":true,\"read_only\":true}]");
  // Simulate a crash/error after profiles publication but before the main commit.
  fail_main_rename = true;
  assert(!sm_config_prepare_files());
  char *data = get_file(CONFIG_FILE);
  assert(!strcmp(data, legacy)); free(data);
  assert(access(PROFILES_FILE, F_OK) == 0 && access(LEGACY_SMB_FILE, F_OK) == 0);
  fail_main_rename = false;
  assert(sm_config_prepare_files() && load_runtime_config());
  assert(runtime_config().api_port == 11111 && runtime_config().scan_depth == 2);
  assert(get_custom_scan_path_count() == 2);
  assert(get_fakelib_mode_for_title("PPSA13197") == SM_FAKELIB_EMULATORS);
  assert(is_global_fakelib_excluded_for_title("PPSA13197"));
  uint32_t sector;
  bool ro;
  assert(get_image_sector_size_override("Game [v1].exfat", &sector) && sector == 512);
  assert(get_image_mode_override("game [v1].exfat", &ro) && ro);
  sm_smb_connection_t c;
  assert(!sm_config_smb_get("nas", &c));
  assert(!strcmp(c.password, " #;\"secret\\ ") && !strcmp(c.username, "DOMAIN\\user"));
  assert(c.read_only && c.active);
  data = get_file(CONFIG_BACKUP_DIR "/config.ini");
  assert(!strcmp(data, legacy)); free(data);
  assert(access(LEGACY_AUTOTUNE_FILE, F_OK) < 0 && errno == ENOENT);
  assert(access(LEGACY_SMB_FILE, F_OK) < 0 && errno == ENOENT);
  struct stat before, after;
  assert(!stat(CONFIG_FILE, &before) && !(before.st_mode & 077));
  assert(sm_config_prepare_files() && !stat(CONFIG_FILE, &after));
  assert(before.st_ino == after.st_ino); // No rewrite on subsequent startups.
  // Even after the commit, matching legacy leftovers can be retired safely.
  assert(copy_backup(CONFIG_BACKUP_DIR "/smb.json", LEGACY_SMB_FILE));
  assert(sm_config_prepare_files() && access(LEGACY_SMB_FILE, F_OK) < 0);
  reset_files();

  // Never overwrite user-created profiles or accept a newer schema.
  put_file(CONFIG_FILE, legacy);
  put_file(PROFILES_FILE, "[config]\nversion=2\n");
  assert(!sm_config_prepare_files() && errno == EEXIST);
  reset_files();
  put_file(CONFIG_FILE, "[config]\nversion=3\n");
  assert(!sm_config_prepare_files() && errno == EPROTONOSUPPORT);
  reset_files();
  put_file(CONFIG_FILE, "api_port=99999\n");
  assert(!sm_config_prepare_files() && access(PROFILES_FILE, F_OK) < 0);
  data = get_file(CONFIG_FILE); assert(!strcmp(data, "api_port=99999\n")); free(data);
  put_file(CONFIG_FILE, "api_port=12345\n");
  assert(sm_config_prepare_files() && load_runtime_config());
  assert(runtime_config().api_port == 12345);
  reset_files();
  // UniMount: the flat api_allow_launch key migrates to [api] allow_launch.
  put_file(CONFIG_FILE, "api_allow_launch=0\n");
  assert(sm_config_prepare_files() && load_runtime_config());
  assert(!runtime_config().api_allow_launch);
  data = get_file(CONFIG_FILE);
  assert(strstr(data, "[api]") && strstr(data, "allow_launch")); free(data);
  reset_files();
  // Truncated strings and malformed SMB must leave every source intact.
  put_file(CONFIG_FILE, "api_port=10101\n");
  put_file(LEGACY_SMB_FILE, "[{\"password\":");
  assert(!sm_config_prepare_files() && access(LEGACY_SMB_FILE, F_OK) == 0);
  reset_files();
  // A fresh minimal install and a v2 main file without profiles both work.
  assert(sm_config_prepare_files() && load_runtime_config());
  assert(runtime_config().api_port == SM_API_DEFAULT_PORT);
  reset_files();
  put_file(CONFIG_FILE, "[config]\nversion=2\n");
  assert(sm_config_prepare_files() && load_runtime_config());
  reset_files();
}

static void test_editing(void) {
  put_file(CONFIG_FILE, "# keep main comment\n[config]\nversion=2\n[general]\ndebug=on # debug comment\n"
      "[custom] # keep [header comment]\nuntouched = value # custom comment\n");
  put_file(PROFILES_FILE, "[config]\nversion=2\n[game:PPSA13197]\nfakelib=disabled # game comment\n"
      "ignore_global_fakelib=on\n[image:Game [v1].exfat]\nmount_mode=rw # mount comment\n");
  assert(load_runtime_config());
  assert(sm_config_set_title_fakelib_mode("PPSA13197", SM_FAKELIB_EMULATORS));
  assert(upsert_image_sector_size_autotune("Game [v1].exfat", 512, NULL));
  char *data = get_file(PROFILES_FILE);
  assert(strstr(data, "# game comment") && strstr(data, "# mount comment")); free(data);
  assert(sm_config_set_title_fakelib_mode("PPSA13197", SM_FAKELIB_DEFAULT));
  assert(sm_config_set_title_global_fakelib_excluded("PPSA13197", false));
  assert(get_fakelib_mode_for_title("PPSA13197") == SM_FAKELIB_FULL);
  assert(!is_global_fakelib_excluded_for_title("PPSA13197"));
  assert(sm_config_set_title_playgo_stub_enabled("PPSA13197", false));
  runtime_config_t cfg = runtime_config();
  assert(sm_config_title_playgo_stub_disabled(&cfg, "ppsa13197"));
  assert(!sm_config_title_playgo_stub_disabled(&cfg, "PPSA12345"));
  assert(load_runtime_config());
  cfg = runtime_config();
  assert(sm_config_title_playgo_stub_disabled(&cfg, "PPSA13197"));
  assert(sm_config_set_title_playgo_stub_enabled("PPSA13197", true));
  assert(load_runtime_config());
  cfg = runtime_config();
  assert(!sm_config_title_playgo_stub_disabled(&cfg, "PPSA13197"));
  assert(sm_config_write_web_settings(false, true, true, false, false, 300,
      true, 0, NULL, 0, SM_FAKELIB_FULL));
  data = get_file(CONFIG_FILE);
  assert(strstr(data, "# keep main comment") && strstr(data, "# debug comment") &&
      strstr(data, "untouched = value # custom comment") && strstr(data, "# keep [header comment]")); free(data);
  sm_smb_connection_t c = {.name="NAS", .path="smb://server/games", .active=true};
  strlcpy(c.password, "  #;a\\b\"  ", sizeof(c.password));
  assert(!sm_config_smb_update(NULL, &c));
  assert(sm_config_smb_update(NULL, &c) == EEXIST);
  assert(!sm_config_smb_update("NAS", &(sm_smb_connection_t){.name="nas", .path="smb://server/games"}));
  assert(!sm_config_smb_get("nas", &c) && !strcmp(c.name, "nas"));
  assert(!sm_config_smb_update("nas", NULL));
  assert(sm_config_smb_get("nas", &c) == ENOENT);
  assert(sm_config_smb_update("missing", NULL) == ENOENT);

  bool changed;
  assert(reload_runtime_config_if_changed(&changed));
  put_file(PROFILES_FILE, "[config]\nversion=2\n[game:PPSA13197]\nfakelib=disabled\n"
      "[game:ppsa13197]\nfakelib=default\nignore_global_fakelib=on\nignore_global_fakelib=off\n"
      "[image:demo.exfat]\nmount_mode=rw\nmount_mode=default\nsector_size=4096\nsector_size=default\n");
  assert(reload_runtime_config_if_changed(&changed) && changed);
  assert(get_fakelib_mode_for_title("PPSA13197") == SM_FAKELIB_FULL);
  assert(!is_global_fakelib_excluded_for_title("PPSA13197"));
  bool ro; uint32_t sector;
  assert(!get_image_mode_override("demo.exfat", &ro) && !get_image_sector_size_override("demo.exfat", &sector));
  put_file(PROFILES_FILE, "[config]\nversion=2\n[game:PPSA13197]\nfakelib=emulators\n");
  assert(reload_runtime_config_if_changed(&changed) && changed);
  assert(get_fakelib_mode_for_title("PPSA13197") == SM_FAKELIB_EMULATORS);
  put_file(PROFILES_FILE, "[config]\nversion=2\n[game:PPSA13197]\nfakelib=invalid\n");
  assert(!reload_runtime_config_if_changed(&changed));
  assert(get_fakelib_mode_for_title("PPSA13197") == SM_FAKELIB_EMULATORS);
  assert(!sm_config_set_title_fakelib_mode("PPSA12345", SM_FAKELIB_FULL));
  // Embedded NUL, overlong input, and newline injection are rejected.
  FILE *f = fopen(PROFILES_FILE, "wb");
  const char nul[] = "[config]\nversion=2\0bad=1\n";
  assert(f && fwrite(nul, 1, sizeof(nul)-1, f) == sizeof(nul)-1 && !fclose(f));
  assert(!reload_runtime_config_if_changed(&changed));
  sm_ini_t doc = {0};
  assert(sm_ini_set(&doc, "section", "key", "value"));
  assert(!sm_ini_set(&doc, "section", "key", "value\nnew=1"));
  sm_ini_free(&doc);
  reset_files();
}

static void *settings_writer(void *arg) {
  uintptr_t kind = (uintptr_t)arg;
  (void)pthread_barrier_wait(&reader_barrier);
  sm_smb_connection_t connection = {.name="NAS", .path="smb://server/games", .active=true};
  for (unsigned i = 0; i < 8; ++i) {
    if (kind == 0)
      assert(sm_config_write_web_settings(i & 1, false, true, false, false, 300,
          true, 0, NULL, 0, SM_FAKELIB_FULL));
    else if (kind == 1)
      assert(sm_config_set_title_fakelib_mode("PPSA13197", SM_FAKELIB_EMULATORS));
    else if (kind == 2)
      assert(upsert_image_sector_size_autotune("Game.exfat", 512, NULL));
    else {
      snprintf(connection.password, sizeof(connection.password), " #secret%u;\\\" ", i);
      assert(!sm_config_smb_update(i ? "NAS" : NULL, &connection));
    }
  }
  return NULL;
}

static void test_concurrent_writers(void) {
  put_file(CONFIG_FILE, "[config]\nversion=2\n");
  put_file(PROFILES_FILE, "[config]\nversion=2\n");
  assert(load_runtime_config());
  pthread_t writers[4];
  for (uintptr_t i = 0; i < 4; ++i)
    assert(!pthread_create(&writers[i], NULL, settings_writer, (void *)i));
  (void)pthread_barrier_wait(&reader_barrier);
  for (size_t i = 0; i < 4; ++i) assert(!pthread_join(writers[i], NULL));
  assert(load_runtime_config() && sm_config_debug_enabled());
  assert(get_fakelib_mode_for_title("PPSA13197") == SM_FAKELIB_EMULATORS);
  uint32_t sector;
  assert(get_image_sector_size_override("Game.exfat", &sector) && sector == 512);
  sm_smb_connection_t c;
  assert(!sm_config_smb_get("NAS", &c) && !strcmp(c.password, " #secret7;\\\" "));
  reset_files();
  puts("config migration: recovery, credentials, validation, manual edits and concurrent writers passed");
}

static void test_unimount_settings(void) {
  put_file(CONFIG_FILE, "[config]\nversion=2\n");
  assert(load_runtime_config());
  runtime_config_t cfg = runtime_config();
  assert(cfg.compat_kraken_api_enabled && cfg.compat_kraken_api_port == 8097u);
  assert(cfg.kraken_auto_mount_enabled && cfg.kraken_inject_ampr_enabled);
  put_file(CONFIG_FILE, "[config]\nversion=2\n[compat]\nkraken_api=off\n"
                        "kraken_api_port=18097\n[kraken]\nauto_mount=off\ninject_ampr=no\n");
  assert(load_runtime_config());
  cfg = runtime_config();
  assert(!cfg.compat_kraken_api_enabled && cfg.compat_kraken_api_port == 18097u);
  assert(!cfg.kraken_auto_mount_enabled && !cfg.kraken_inject_ampr_enabled);
  put_file(CONFIG_FILE, "[config]\nversion=2\n[compat]\nkraken_api_port=0\n");
  assert(!load_runtime_config());
  put_file(CONFIG_FILE, "[config]\nversion=2\n[kraken]\nauto_mount=maybe\n");
  assert(!load_runtime_config());
  put_file(CONFIG_FILE, "[config]\nversion=2\n");
  assert(load_runtime_config());
  cfg = runtime_config();
  assert(cfg.compat_kraken_api_enabled && cfg.kraken_auto_mount_enabled);
  puts("unimount settings: [compat] and [kraken] defaults, values and validation passed");
}

static bool write_unimount(const sm_config_unimount_settings_t *u) {
  return sm_config_write_web_settings_ex(false, false, true, false, false, 300,
      true, 0, NULL, 0, SM_FAKELIB_FULL, u);
}

static void assert_ini(const sm_ini_t *doc, const char *section, const char *key,
                       const char *expected) {
  const char *value = sm_ini_get(doc, section, key);
  assert(value && !strcmp(value, expected));
}

static void test_unimount_settings_write(void) {
  const char *initial = "# top comment\n[config]\nversion=2\n"
      "[compat] # compat header comment\nkraken_api_port = 18097 # port comment\n"
      "[custom]\nuntouched = value # custom comment\n";
  put_file(CONFIG_FILE, initial);
  put_file(PROFILES_FILE, "[config]\nversion=2\n");
  assert(load_runtime_config());

  // A subset leaves the other UniMount keys, comments and unrelated keys alone.
  sm_config_unimount_settings_t u = {.has_api_allow_launch = true, .api_allow_launch = false};
  assert(write_unimount(&u));
  char *data = get_file(CONFIG_FILE);
  assert(strstr(data, "# top comment") && strstr(data, "# compat header comment") &&
         strstr(data, "kraken_api_port = 18097 # port comment") &&
         strstr(data, "untouched = value # custom comment"));
  assert(!strstr(data, "[diagnostics]") && !strstr(data, "[kraken]"));
  free(data);
  sm_ini_t doc = {0};
  assert(sm_ini_read(CONFIG_FILE, &doc, false));
  assert_ini(&doc, "api", "allow_launch", "off");
  assert(!sm_ini_get(&doc, "compat", "kraken_api"));
  sm_ini_free(&doc);
  // Read on every use: visible before the scanner reload.
  assert(!runtime_config().api_allow_launch);

  // Every key, at non-default values, lands in its own section.
  u = (sm_config_unimount_settings_t){
      .has_compat_kraken_api = true, .compat_kraken_api = false,
      .has_compat_kraken_api_port = true, .compat_kraken_api_port = 65535u,
      .has_kraken_auto_mount = true, .kraken_auto_mount = false,
      .has_kraken_inject_ampr = true, .kraken_inject_ampr = false,
      .has_diagnostics_enabled = true, .diagnostics_enabled = false,
      .has_diagnostics_running_confirm_seconds = true,
      .diagnostics_running_confirm_seconds = 600u,
      .has_diagnostics_error_window_seconds = true,
      .diagnostics_error_window_seconds = 10u,
      .has_api_allow_launch = true, .api_allow_launch = true};
  assert(write_unimount(&u));
  assert(sm_ini_read(CONFIG_FILE, &doc, false));
  assert_ini(&doc, "compat", "kraken_api", "off");
  assert_ini(&doc, "compat", "kraken_api_port", "65535");
  assert_ini(&doc, "kraken", "auto_mount", "off");
  assert_ini(&doc, "kraken", "inject_ampr", "off");
  assert_ini(&doc, "diagnostics", "enabled", "off");
  assert_ini(&doc, "diagnostics", "running_confirm_seconds", "600");
  assert_ini(&doc, "diagnostics", "error_window_seconds", "10");
  assert_ini(&doc, "api", "allow_launch", "on");
  assert_ini(&doc, "custom", "untouched", "value");
  assert(!sm_ini_get(&doc, "general", "kraken_api") && !sm_ini_get(&doc, "api", "auto_mount"));
  sm_ini_free(&doc);
  data = get_file(CONFIG_FILE);
  assert(strstr(data, "# top comment") && strstr(data, "# compat header comment") &&
         strstr(data, "# port comment") && strstr(data, "untouched = value # custom comment"));
  free(data);

  runtime_config_t cfg = runtime_config();
  assert(!cfg.compat_kraken_api_enabled && cfg.compat_kraken_api_port == 65535u);
  assert(!cfg.kraken_inject_ampr_enabled && !cfg.diagnostics_enabled);
  assert(cfg.diagnostics_running_confirm_seconds == 600u &&
         cfg.diagnostics_error_window_seconds == 10u && cfg.api_allow_launch);
  // kraken_auto_mount waits for the scanner reload, which then rescans.
  assert(cfg.kraken_auto_mount_enabled);
  bool changed = false;
  assert(reload_runtime_config_if_changed(&changed) && changed);
  assert(!runtime_config().kraken_auto_mount_enabled);
  assert(load_runtime_config());
  cfg = runtime_config();
  assert(!cfg.compat_kraken_api_enabled && cfg.compat_kraken_api_port == 65535u &&
         !cfg.kraken_auto_mount_enabled && !cfg.kraken_inject_ampr_enabled &&
         !cfg.diagnostics_enabled && cfg.diagnostics_running_confirm_seconds == 600u &&
         cfg.diagnostics_error_window_seconds == 10u && cfg.api_allow_launch);

  // The other range ends and the "on" spelling round-trip too.
  u = (sm_config_unimount_settings_t){
      .has_compat_kraken_api = true, .compat_kraken_api = true,
      .has_compat_kraken_api_port = true, .compat_kraken_api_port = 1u,
      .has_kraken_auto_mount = true, .kraken_auto_mount = true,
      .has_kraken_inject_ampr = true, .kraken_inject_ampr = true,
      .has_diagnostics_enabled = true, .diagnostics_enabled = true,
      .has_diagnostics_running_confirm_seconds = true,
      .diagnostics_running_confirm_seconds = 5u,
      .has_diagnostics_error_window_seconds = true,
      .diagnostics_error_window_seconds = 3600u};
  assert(write_unimount(&u) && load_runtime_config());
  cfg = runtime_config();
  assert(cfg.compat_kraken_api_enabled && cfg.compat_kraken_api_port == 1u &&
         cfg.kraken_auto_mount_enabled && cfg.kraken_inject_ampr_enabled &&
         cfg.diagnostics_enabled && cfg.diagnostics_running_confirm_seconds == 5u &&
         cfg.diagnostics_error_window_seconds == 3600u && cfg.api_allow_launch);

  // Out-of-range values are rejected before anything is written or published.
  char *before = get_file(CONFIG_FILE);
  const sm_config_unimount_settings_t invalid[] = {
      {.has_compat_kraken_api_port = true, .compat_kraken_api_port = 0u},
      {.has_compat_kraken_api_port = true, .compat_kraken_api_port = 65536u},
      {.has_diagnostics_running_confirm_seconds = true, .diagnostics_running_confirm_seconds = 4u},
      {.has_diagnostics_running_confirm_seconds = true, .diagnostics_running_confirm_seconds = 601u},
      {.has_diagnostics_error_window_seconds = true, .diagnostics_error_window_seconds = 9u},
      {.has_diagnostics_error_window_seconds = true, .diagnostics_error_window_seconds = 3601u},
      // A valid key next to an invalid one is not written either.
      {.has_api_allow_launch = true, .api_allow_launch = false,
       .has_compat_kraken_api_port = true, .compat_kraken_api_port = 0u},
  };
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    errno = 0;
    assert(!write_unimount(&invalid[i]) && errno == EINVAL);
    data = get_file(CONFIG_FILE);
    assert(!strcmp(data, before));
    free(data);
    cfg = runtime_config();
    assert(cfg.compat_kraken_api_port == 1u && cfg.api_allow_launch &&
           cfg.diagnostics_running_confirm_seconds == 5u &&
           cfg.diagnostics_error_window_seconds == 3600u);
  }
  free(before);

  // Without UniMount settings the existing write leaves those keys untouched.
  assert(sm_config_write_web_settings(false, false, true, false, false, 300,
      true, 0, NULL, 0, SM_FAKELIB_FULL));
  assert(sm_ini_read(CONFIG_FILE, &doc, false));
  assert_ini(&doc, "compat", "kraken_api_port", "1");
  assert_ini(&doc, "diagnostics", "error_window_seconds", "3600");
  sm_ini_free(&doc);
  reset_files();
  puts("unimount settings write: sections, comments, subsets, publication and rejection passed");
}

int main(void) {
  char directory[] = "/tmp/shadowmount-config-test-XXXXXX";
  assert(mkdtemp(directory));
  assert(!chdir(directory));

  pthread_t readers[4];
  assert(pthread_barrier_init(&reader_barrier, NULL, 5) == 0);
  for (size_t i = 0; i < 4; i++)
    assert(pthread_create(&readers[i], NULL, init_reader, NULL) == 0);
  (void)pthread_barrier_wait(&reader_barrier);
  for (size_t i = 0; i < 4; i++)
    assert(pthread_join(readers[i], NULL) == 0);

  test_migration();
  test_editing();
  test_concurrent_writers();
  test_logging_flags();
  test_unimount_settings();
  test_unimount_settings_write();
  test_diagnostics_config();
  test_api_allow_launch_config();
  write_config(1);
  assert(load_runtime_config());
  runtime_config_t retained = runtime_config();
  char retained_path[MAX_PATH];
  assert(get_scan_path(0, retained_path));
  bool changed = true;
  assert(reload_runtime_config_if_changed(&changed) && !changed);

  for (size_t i = 0; i < 4; i++)
    assert(pthread_create(&readers[i], NULL, reload_reader, NULL) == 0);
  (void)pthread_barrier_wait(&reader_barrier);
  for (unsigned i = 0; i < 1000; i++) {
    write_config(i % 2u == 0 ? 2u : 1u);
    assert(reload_runtime_config_if_changed(&changed) && changed);
  }
  atomic_store(&reader_stop, true);
  for (size_t i = 0; i < 4; i++)
    assert(pthread_join(readers[i], NULL) == 0);
  assert(atomic_load(&read_count) > 0u);
  assert(retained.api_port == 9101u);
  assert(strcmp(retained.global_fakelib_path, "/data/version1") == 0);
  assert(strcmp(retained_path, "/data/games1") == 0);

  char invalid_path[MAX_PATH] = "stale";
  assert(!get_scan_path(-1, invalid_path) && invalid_path[0] == '\0');
  assert(!get_scan_path(MAX_SCAN_PATHS, invalid_path));
  assert(!get_custom_scan_path(1, invalid_path));
  bool read_only = false;
  uint32_t value = 0;
  assert(!get_image_mode_override("missing.exfat", &read_only));
  assert(!get_image_sector_size_override("missing.exfat", &value));
  assert(!is_global_fakelib_excluded_for_title("PPSA99999"));
  write_config(1);
  assert(reload_runtime_config_if_changed(&changed) && !changed);
  assert(unlink(CONFIG_FILE) == 0);
  assert(!reload_runtime_config_if_changed(&changed));
  assert(runtime_config().api_port == 9101u);
  reset_files();
  assert(!chdir("/tmp"));
  assert(rmdir(directory) == 0);
  assert(pthread_barrier_destroy(&reader_barrier) == 0);
  printf("config: 1000 reloads, %u concurrent reads, snapshots preserved\n",
         atomic_load(&read_count));
  return 0;
}
