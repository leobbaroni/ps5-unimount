#ifndef SM_SHELLCORE_HOOKS_H
#define SM_SHELLCORE_HOOKS_H

#include <stdbool.h>
#include <stddef.h>

// Install/remove the fail-open SceShellCore lifecycle bridge.
bool sm_shellcore_hooks_start(void);
void sm_shellcore_hooks_stop(void);

// Check/repair previously installed hooks; disabled hooks remain disabled.
bool sm_shellcore_hooks_refresh(void);

// Return whether this firmware uses the internal TitleDir bridge.
bool sm_shellcore_install_bridge_enabled(void);

// Invoke the internal TitleDir path through one armed AppInstallAll dispatch.
// Returns false when the bridge is unavailable or was not consumed.
bool sm_shellcore_install_title_dir(const char *title_id,
                                    const char *install_dir,
                                    int *result_out);

// Read-only snapshot for diagnostics. Returns false while the hook state is
// locked by an install/repair, without waiting for it.
typedef struct {
  bool installed;       // Hooks enabled and verified ready.
  const char *state;    // "not_installed", "installed", "stale", "rollback_pending"
  size_t hook_count;    // Patched ShellCore sites (2 base, 3 with AppInstallAll).
  bool install_hook;    // AppInstallAll bridge for per-title registration.
} sm_shellcore_hooks_info_t;
bool sm_shellcore_hooks_get_info(sm_shellcore_hooks_info_t *info);

#endif
