#include "host_platform.h"
#include "sm_kraken.h"
#include "sm_limits.h"
#include "launch_types.inc"

#define TITLE_A "PPSA03527"
#define TITLE_B "PPSA47989"
#define TITLE_C "PPSA99008"

static unsigned wakes, unmounts;
static bool fail_unmount, sleeping, kraken_title, package_missing;

static void sm_scanner_wake(void) { ++wakes; }
static void log_debug(const char *format, ...) { (void)format; }
static bool sm_game_lifecycle_has_active_game(void) { return false; }
static bool should_stop_requested(void) { return false; }
static bool runtime_sleep_mode_active(void) { return sleeping; }
static uint64_t monotonic_time_us(void) { return 100000000; }
static bool shellcore_service_stopping(void) { return false; }
static void runtime_mount_state_lock(void) {}
static void runtime_mount_state_unlock(void) {}
static int sceKernelUsleep(unsigned us) { (void)us; return 0; }
static bool is_supported_game_title_id(const char *title) {
  return title && strncmp(title, "PPSA", 4) == 0;
}
static bool read_mount_link(const char *title, char *path, size_t size) {
  (void)title; strlcpy(path, "/data/game", size); return true;
}
static bool is_under_image_mount_base(const char *path) {
  (void)path; return false;
}
bool sm_kraken_is_cache(const char *path) {
  (void)path; return kraken_title;
}
bool sm_kraken_source_ready(const char *path) {
  (void)path;
  if (package_missing) {
    errno = ENOENT;
    return false;
  }
  return true;
}
static bool wait_for_resumed_runtime_source(const char *title, char *path) {
  (void)title; (void)path; return true;
}
static bool is_data_mounted(const char *title) { (void)title; return true; }
static void sm_fakelib_prepare_title_cache(const char *title, const char *path) {
  (void)title; (void)path;
}
static bool prepare_title_runtime(const char *title, char *path, const bool *ro,
                                  const char **failed_layer) {
  (void)title; (void)path; (void)ro; (void)failed_layer; return true;
}
static void sm_diag_note_launch(const char *title, int status, int native_errno,
                                const char *layer) {
  (void)title; (void)status; (void)native_errno; (void)layer;
}
static bool unmount_title_runtime_layers(const char *title) {
  (void)title; ++unmounts;
  if (fail_unmount) { errno = EIO; return false; }
  return true;
}
static bool unmount_title_runtime_layers_quiet(const char *title) {
  return unmount_title_runtime_layers(title);
}
static bool release_backing_image(const char *title) { (void)title; return true; }
static bool sm_shellcore_title_is_protected(const char *title) {
  const shellcore_mount_owner_t *owners[] = {&g_service.prepared, &g_service.outgoing};
  for (size_t i = 0; i < 2; ++i)
    if (!strcmp(owners[i]->title_id, title) && owners[i]->pid > 0 &&
        !owners[i]->game_exited)
      return true;
  return false;
}
static void sm_fakelib_game_on_launch_failed(const char *title) { (void)title; }
static void sm_pkg_backport_on_launch_failed(const char *title) { (void)title; }

#include "launch_helpers.inc"

static bool preserve_game_written_files(const char *cache, const char *staging) {
  (void)cache;
  (void)staging;
  assert(false); // This test publishes a new cache or defers an active title.
  return false;
}

static const char sm_kraken_revision[] = "test-revision";

static int open_source(const sm_pkg_info_t *info, pkg_kind_t kind) {
  assert(kind == PKG_KIND_BASE);
  return open(info->path, O_RDONLY | O_NOFOLLOW);
}

#include "kraken_publish.inc"

static pthread_mutex_t g_scanner_cycle_mutex = PTHREAD_MUTEX_INITIALIZER;
static char kraken_staging[MAX_PATH], kraken_cache[MAX_PATH], kraken_backup[MAX_PATH];
static bool kraken_published;

static bool sm_shellcore_background_work_blocked(void) { return false; }

static bool run_full_scan_cycle_impl(bool startup, const char *reason, bool *unstable) {
  (void)startup;
  (void)reason;
  (void)unstable;
  assert(g_service.external_mutation_in_progress);
  kraken_published = publish_cache(TITLE_A, kraken_staging, kraken_cache, kraken_backup);
  assert(g_service.external_mutation_in_progress);
  return true;
}

#include "scan_cycle.inc"

static void reset_state(void) {
  clear_mount_owner(&g_service.prepared);
  clear_mount_owner(&g_service.outgoing);
  g_service.prepare_in_progress = g_service.launch_pending = false;
  g_service.external_mutation_in_progress = false;
  g_service.pending_launch_title_id[0] = g_service.releasing_title_id[0] = '\0';
  fail_unmount = sleeping = false;
  kraken_title = package_missing = false;
  wakes = unmounts = 0;
}

static void start_title(const char *title, uint32_t app_id, pid_t pid) {
  assert(handle_launch_request(title) == 0);
  assert(sm_shellcore_service_bind_prepared_app(title, app_id, pid));
}

static void abandon_switch(void) {
  reset_state();
  start_title(TITLE_A, 0x18, 173);
  assert(handle_launch_request(TITLE_B) == 0);
  assert(handle_launch_failed_request(TITLE_B) == 0);
  assert(sm_shellcore_service_note_game_exit(173, 100000000, NULL));
  shellcore_mount_owner_t exited = g_service.outgoing;
  assert(release_title_runtime_expected(TITLE_A, false, &exited));
  assert(!g_service.outgoing.title_id[0]);
  assert(g_service.prepared.pid == 0 && g_service.launch_pending);
  unmounts = 0;
}

static void test_kraken_publication(const char *directory) {
  reset_state();
  snprintf(kraken_staging, sizeof(kraken_staging), "%s/kraken-staging", directory);
  snprintf(kraken_cache, sizeof(kraken_cache), "%s/kraken-cache", directory);
  snprintf(kraken_backup, sizeof(kraken_backup), "%s/kraken-backup", directory);
  assert(mkdir(kraken_staging, 0700) == 0);
  assert(run_full_scan_cycle(false, NULL, NULL));
  assert(kraken_published && !g_service.external_mutation_in_progress);
  assert(access(kraken_cache, F_OK) == 0 && access(kraken_staging, F_OK) != 0);

  assert(mkdir(kraken_staging, 0700) == 0);
  start_title(TITLE_A, 0x18, 173);
  unsigned before = unmounts;
  assert(run_full_scan_cycle(false, NULL, NULL));
  assert(!kraken_published && errno == EBUSY && unmounts == before);
  assert(!g_service.external_mutation_in_progress);
  assert(access(kraken_cache, F_OK) == 0 && access(kraken_staging, F_OK) == 0);
  puts("Kraken cache: scanner-owned publication and active-title protection passed");
}

static void snapshot_source(sm_pkg_info_t *info, const char *path) {
  struct stat st;
  assert(lstat(path, &st) == 0);
  strlcpy(info->path, path, sizeof(info->path));
  info->source_device = st.st_dev;
  info->source_inode = st.st_ino;
  info->source_size = st.st_size;
  info->source_mtime = st.st_mtime;
  info->source_ctime = st.st_ctime;
  info->source_mtime_ns = st.st_mtim.tv_nsec;
  info->source_ctime_ns = st.st_ctim.tv_nsec;
}

static void assert_file_contents(const char *path, const char *expected) {
  char buffer[MAX_PATH + 600];
  FILE *file = fopen(path, "rb");
  assert(file != NULL);
  size_t count = fread(buffer, 1, sizeof(buffer), file);
  assert(!ferror(file) && count == strlen(expected));
  assert(memcmp(buffer, expected, count) == 0);
  assert(fclose(file) == 0);
}

static bool finalize_rebind(void *context) {
  (void)context;
  return true;
}

static void test_kraken_rebind(const char *directory) {
  reset_state();
  char cache[MAX_PATH], original[MAX_PATH], moved[MAX_PATH], image[MAX_PATH], marker[MAX_PATH];
  assert(join_path(directory, TITLE_A, cache) && mkdir(cache, 0700) == 0);
  assert(join_path(directory, "original.pkg", original) && join_path(directory, "moved.pkg", moved));
  assert(join_path(cache, "kraken.image", image) && join_path(cache, KRAKEN_MARKER, marker));
  FILE *file = fopen(original, "wb");
  assert(file && fchmod(fileno(file), 0600) == 0 &&
         fputs("base package bytes", file) >= 0 && fclose(file) == 0);
  sm_pkg_info_t base = {.title_id = TITLE_A};
  snapshot_source(&base, original);
  char signature[280], updated[280];
  assert(write_reader_signature(&base, "verified-1", KRAKEN_INITIAL_READER, 0, signature));
  assert(matches_source_signature(signature, &base, "verified-1"));
  assert(write_reader_signature(&base, KRAKEN_REVISION, KRAKEN_INITIAL_READER, 314, signature));
  assert(rename(original, moved) == 0);
  snapshot_source(&base, moved);
  uint64_t backport = 0;
  assert(same_package_after_rename(&base, signature));
  assert(cached_backport_fingerprint(signature, &backport) && backport == 314);
  base.source_inode++;
  assert(!same_package_after_rename(&base, signature));
  base.source_inode--;
  assert(write_source_signature(&base, KRAKEN_REVISION, backport, updated));
  patch_record_t patch = {.path = "/data/update.pkg", .signature = "stored-patch-signature\n"};
  sm_kraken_job_t job = {.finalize = finalize_rebind};
  assert(write_image_pointer(cache, "original.pkg"));

  assert(mkdir(marker, 0700) == 0);
  assert(!rebind_cache(&base, &job, cache, original, updated, &patch));
  assert_file_contents(image, "kraken/original.pkg");
  assert(rmdir(marker) == 0);

  assert(rebind_cache(&base, &job, cache, original, updated, &patch));
  char expected[MAX_PATH + 600];
  int count = snprintf(expected, sizeof(expected), "%s\n%s%s\n%s", moved, updated,
                      patch.path, patch.signature);
  assert(count > 0 && (size_t)count < sizeof(expected));
  assert_file_contents(marker, expected);
  assert_file_contents(image, "kraken/moved.pkg");
  assert_file_contents(moved, "base package bytes");
  puts("Kraken relink: source identity, patch record and image pointer rollback passed");

  assert(write_reader_signature(&base, KRAKEN_REVISION, KRAKEN_V021_READER, backport, signature));
  assert(write_cache_marker(cache, moved, signature, &patch));
  int fd = open(moved, O_RDONLY | O_NOFOLLOW);
  assert(fd >= 0 && make_launch_source_readable(cache, moved, fd, signature, &patch));
  struct stat st;
  assert(fstat(fd, &st) == 0 && (st.st_mode & 0777) == 0644);
  snapshot_source(&base, moved);
  assert(write_reader_signature(&base, KRAKEN_REVISION, KRAKEN_V021_READER, backport, updated));
  assert(source_stat_matches(updated, &st));
  assert(make_launch_source_readable(cache, moved, fd, updated, &patch));
  assert(close(fd) == 0);
  count = snprintf(expected, sizeof(expected), "%s\n%s%s\n%s", moved, updated,
                   patch.path, patch.signature);
  assert(count > 0 && (size_t)count < sizeof(expected));
  assert_file_contents(marker, expected);
  assert_file_contents(moved, "base package bytes");
  puts("Kraken launch: package read access, previous reader and LIH record preserved");
}

int main(int argc, char **argv) {
  assert(argc == 2);
  test_kraken_publication(argv[1]);
  test_kraken_rebind(argv[1]);
  reset_state();
  kraken_title = package_missing = true;
  launch_mount_failure_t failure;
  assert(mount_managed_title_runtime(TITLE_A, true, false, NULL, &failure) == ENOENT);
  assert(!strcmp(failure.layer, "kraken_source") && failure.native_errno == ENOENT);
  assert(!launch_preparation_pending_locked() && unmounts == 0);
  package_missing = false;
  start_title(TITLE_A, 0x18, 173);

  abandon_switch();
  unsigned before = wakes;
  assert(handle_launch_request(TITLE_C) == 0);
  assert(unmounts == 1 && wakes > before);
  assert(mount_owner_matches(&g_service.prepared, TITLE_C));
  assert(sm_shellcore_service_bind_prepared_app(TITLE_C, 0x2018, 200));
  assert(!launch_preparation_pending_locked());

  abandon_switch();
  assert(handle_launch_request(TITLE_B) == 0 && unmounts == 0);
  abandon_switch();
  assert(mount_managed_title_runtime(TITLE_C, true, true, NULL, &failure) == EBUSY);
  assert(!strcmp(failure.layer, "claim"));
  assert(unmounts == 0); // NOTE_EXEC fallback cannot abandon another launch.

  abandon_switch();
  fail_unmount = true;
  assert(handle_launch_request(TITLE_C) == 0);
  assert(unmounts == 1 && mount_owner_matches(&g_service.prepared, TITLE_C));

  for (unsigned guard = 0; guard < 5; ++guard) {
    abandon_switch();
    if (guard == 0) g_service.prepare_in_progress = true;
    if (guard == 1) g_service.external_mutation_in_progress = true;
    if (guard == 2) strlcpy(g_service.releasing_title_id, TITLE_A, MAX_TITLE_ID);
    if (guard == 3) set_mount_owner(&g_service.outgoing, TITLE_A);
    if (guard == 4) sleeping = true;
    assert(handle_launch_request(TITLE_C) == EBUSY && unmounts == 0);
  }
  reset_state();
  start_title(TITLE_A, 0x18, 173);
  assert(handle_launch_request(TITLE_C) == 0 && unmounts == 0);
  assert(g_service.outgoing.pid == 173);

  // A side app can exit while the original game stays alive in outgoing.
  reset_state();
  start_title(TITLE_A, 0x18, 173);
  start_title(TITLE_B, 0x1a, 189);
  assert(sm_shellcore_service_note_game_exit(189, 100000000, NULL));
  shellcore_mount_owner_t exited = g_service.prepared;
  assert(release_title_runtime_expected(TITLE_B, false, &exited));
  assert(!g_service.prepared.title_id[0] && g_service.outgoing.pid == 173);
  assert(handle_launch_request(TITLE_A) == 0);
  assert(g_service.prepared.pid == 173 && g_service.prepared.app_id == 0x18);
  assert(!g_service.outgoing.title_id[0]);
  assert(!launch_preparation_pending_locked());
  unsigned released = unmounts;
  assert(handle_launch_failed_request(TITLE_A) == 0);
  assert(unmounts == released && !launch_preparation_pending_locked());
  assert(sm_shellcore_service_note_game_exit(173, 110000000, NULL));
  exited = g_service.prepared;
  assert(release_title_runtime_expected(TITLE_A, false, &exited));
  assert(!g_service.prepared.title_id[0] && !g_service.outgoing.title_id[0]);
  assert(handle_launch_request(TITLE_C) == 0);

  reset_state();
  set_mount_owner(&g_service.outgoing, TITLE_A);
  g_service.outgoing.pid = 173;
  g_service.outgoing.game_exited = true;
  bool claimed = true;
  assert(claim_prepared_title(TITLE_A, true, &claimed) == EBUSY);
  assert(!claimed && !g_service.prepared.title_id[0]);
  assert(g_service.outgoing.pid == 173 && g_service.outgoing.game_exited);
  puts("Launch ownership: abandoned switch recovery, retry identity, fallback, live owner and mutation guards passed");
  return 0;
}
