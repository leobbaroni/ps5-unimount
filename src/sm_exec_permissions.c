#include "sm_platform.h"
#include "sm_exec_permissions.h"

int sm_add_exec_permission(const char *path) {
  struct stat st;
  if (lstat(path, &st) != 0)
    return errno == ENOENT ? 0 : -1;
  if (!S_ISREG(st.st_mode))
    return 0;

  mode_t mode = st.st_mode & 07777;
  if ((mode & SM_EXEC_PERMISSION_BITS) == SM_EXEC_PERMISSION_BITS)
    return 0;
  if (chmod(path, mode | SM_EXEC_PERMISSION_BITS) != 0)
    return -1;
  return 1;
}

static bool exec_failure_matches(const sm_exec_failure_t *entry,
                                 const struct stat *st) {
  return entry->used && entry->dev == (uint64_t)st->st_dev &&
         entry->ino == (uint64_t)st->st_ino &&
         entry->mode == (uint64_t)st->st_mode &&
         entry->ctime_sec == (int64_t)st->st_ctim.tv_sec &&
         entry->ctime_nsec == (int64_t)st->st_ctim.tv_nsec;
}

bool sm_exec_failure_known(const sm_exec_failure_t *list, size_t count,
                           const struct stat *st) {
  for (size_t i = 0; i < count; ++i)
    if (exec_failure_matches(&list[i], st))
      return true;
  return false;
}

void sm_exec_failure_remember(sm_exec_failure_t *list, size_t count,
                              size_t *next, const struct stat *st) {
  if (count == 0 || sm_exec_failure_known(list, count, st))
    return;
  sm_exec_failure_t *entry = &list[*next % count];
  *next = (*next + 1u) % count;
  entry->used = true;
  entry->dev = (uint64_t)st->st_dev;
  entry->ino = (uint64_t)st->st_ino;
  entry->mode = (uint64_t)st->st_mode;
  entry->ctime_sec = (int64_t)st->st_ctim.tv_sec;
  entry->ctime_nsec = (int64_t)st->st_ctim.tv_nsec;
}
