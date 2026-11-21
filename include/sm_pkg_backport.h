#ifndef SM_PKG_BACKPORT_H
#define SM_PKG_BACKPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "sm_limits.h"

// Add child NSFS redirects immediately before the package process is spawned.
// Files use plain redirects; missing directory subtrees use overlays. Up to
// 256 redirect entries are tracked per sandbox. Missing backports and
// non-NSFS app0 paths are harmless skips.
bool sm_pkg_backport_on_sandbox_ready(const char *title_id);
// Query sources held by installed NSFS redirects, including pending cleanup.
bool sm_pkg_backport_source_is_in_use(const char *path);
// Remove an unbound redirect when ShellCore aborts the launch.
void sm_pkg_backport_on_launch_failed(const char *title_id);
// Bind an installed redirect to the process that consumed the sandbox.
// This never creates a late redirect or retries a failed create ioctl.
// All retained namespaces of the title follow the confirmed replacement PID.
void sm_pkg_backport_on_exec(pid_t pid, const char *title_id);
// Remove the redirect owned by an exited process.
void sm_pkg_backport_on_exit(pid_t pid);
// Bounded EBUSY cleanup on the shared lifecycle watcher. Zero means disabled.
uint64_t sm_pkg_backport_next_cleanup_us(void);
void sm_pkg_backport_poll_cleanup(void);
// Best-effort cleanup of every redirect owned by this payload. Redirects of a
// live, launching or handing-off game (handoff_title_id, NULL if none) are
// kept and logged, also on later calls.
void sm_pkg_backport_shutdown(const char *handoff_title_id);
// Any redirect installed (lock-free; safe under any module lock).
bool sm_pkg_backport_has_redirects(void);
// Add the titles whose redirects a shutdown kept.
void sm_pkg_backport_add_kept_titles(char titles[][MAX_TITLE_ID],
                                     size_t *count, size_t max);

#endif
