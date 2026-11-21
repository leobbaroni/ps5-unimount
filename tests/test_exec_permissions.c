#include "host_platform.h"
#include "sm_exec_permissions.h"

#include "../src/sm_exec_permissions.c"

static char root[] = "/tmp/sm-exec-permissions-XXXXXX";

static void make_file(const char *path, mode_t mode) {
  int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
  assert(fd >= 0 && close(fd) == 0);
  assert(chmod(path, mode) == 0);
}

static mode_t mode_of(const char *path) {
  struct stat st;
  assert(lstat(path, &st) == 0);
  return st.st_mode & 07777;
}

static void test_file_mode_repair(mode_t before, mode_t expected_after,
                                  int expected_result) {
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/eboot.bin", root);
  make_file(path, before);
  assert(sm_add_exec_permission(path) == expected_result);
  assert(mode_of(path) == expected_after);
  assert(unlink(path) == 0);
}

static void test_missing_file_is_ignored(void) {
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/missing.prx", root);
  assert(sm_add_exec_permission(path) == 0);
}

static void test_directory_is_ignored(void) {
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/sce_module", root);
  const mode_t directory_mode = 0700;
  assert(mkdir(path, directory_mode) == 0);
  assert(sm_add_exec_permission(path) == 0);
  assert(mode_of(path) == directory_mode);
  assert(rmdir(path) == 0);
}

static void test_symlink_target_is_not_changed(void) {
  char target[PATH_MAX], link_path[PATH_MAX];
  snprintf(target, sizeof(target), "%s/outside.bin", root);
  snprintf(link_path, sizeof(link_path), "%s/libc.prx", root);
  const mode_t target_mode = 0644;
  make_file(target, target_mode);
  assert(symlink(target, link_path) == 0);
  assert(sm_add_exec_permission(link_path) == 0);
  assert(mode_of(target) == target_mode);
  assert(unlink(link_path) == 0 && unlink(target) == 0);
}

static void test_failure_memory(void) {
  sm_exec_failure_t list[2];
  memset(list, 0, sizeof(list));
  size_t next = 0;
  struct stat a = {0}, b = {0}, c = {0};
  a.st_dev = 1; a.st_ino = 10; a.st_mode = S_IFREG | 0644;
  a.st_ctim.tv_sec = 100; a.st_ctim.tv_nsec = 5;
  b = a; b.st_ino = 11;
  c = a; c.st_ino = 12;
  assert(!sm_exec_failure_known(list, 2, &a));
  sm_exec_failure_remember(list, 2, &next, &a);
  assert(sm_exec_failure_known(list, 2, &a));
  // Remembering the same file again does not take another slot.
  sm_exec_failure_remember(list, 2, &next, &a);
  assert(next == 1);
  // A changed ctime or mode is a new attempt.
  struct stat changed = a;
  changed.st_ctim.tv_nsec = 6;
  assert(!sm_exec_failure_known(list, 2, &changed));
  changed = a;
  changed.st_mode = S_IFREG | 0755;
  assert(!sm_exec_failure_known(list, 2, &changed));
  // Round robin: the oldest entry is replaced when full.
  sm_exec_failure_remember(list, 2, &next, &b);
  sm_exec_failure_remember(list, 2, &next, &c);
  assert(!sm_exec_failure_known(list, 2, &a));
  assert(sm_exec_failure_known(list, 2, &b) && sm_exec_failure_known(list, 2, &c));
}

int main(void) {
  assert(mkdtemp(root));
  test_failure_memory();
  // Typical FTP/ZIP copy: read/write bits kept, all execute bits added.
  test_file_mode_repair(0644, 0755, 1);
  // Owner-only file: group/other read access is not widened.
  test_file_mode_repair(0600, 0711, 1);
  // Partially executable file only gains the missing bits.
  test_file_mode_repair(0744, 0755, 1);
  // Already executable file is left untouched.
  test_file_mode_repair(0755, 0755, 0);
  test_missing_file_is_ignored();
  test_directory_is_ignored();
  test_symlink_target_is_not_changed();
  assert(rmdir(root) == 0);
  puts("Exec permissions: missing execute bits added; other bits, directories and symlink targets unchanged; failed chmods remembered per file state");
  return 0;
}
