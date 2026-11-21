#ifndef SM_EXEC_PERMISSIONS_H
#define SM_EXEC_PERMISSIONS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

// Owner, group and other execute bits. The PS5 loader rejects eboot.bin and
// PRX/SPRX modules that lack them with a "mount flag / attribute error".
#define SM_EXEC_PERMISSION_BITS 0111

// Add any missing execute bits to a regular file, keeping all other bits.
// Returns 1 when the mode changed, 0 when nothing was needed (including
// missing files and non-regular files), or -1 with errno set on failure.
int sm_add_exec_permission(const char *path);

// A chmod that failed is remembered by the file's identity, mode and ctime,
// so an unchanged file is not retried (and logged) on every scan pass. Any
// change to the file (mode, ctime, replacement) makes it a new attempt.
typedef struct {
  bool used;
  uint64_t dev, ino, mode;
  int64_t ctime_sec, ctime_nsec;
} sm_exec_failure_t;

bool sm_exec_failure_known(const sm_exec_failure_t *list, size_t count,
                           const struct stat *st);
// Overwrites the slot after *next (round robin) when the file is new.
void sm_exec_failure_remember(sm_exec_failure_t *list, size_t count,
                              size_t *next, const struct stat *st);

#endif
