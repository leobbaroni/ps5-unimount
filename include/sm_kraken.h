#ifndef SM_KRAKEN_H
#define SM_KRAKEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sm_pkg.h"

#define SM_KRAKEN_PACKAGE_DIR "kraken"

bool sm_kraken_is_package(const char *path);
bool sm_kraken_is_cache(const char *path);
// Header check only. The full parser runs in the requested storage job.
bool sm_kraken_can_mount(const sm_pkg_info_t *info);
// Cheap header check for a local debug LIH patch.
bool sm_kraken_can_apply_patch(const sm_pkg_info_t *info);
typedef struct {
    bool (*cancelled)(void *ctx);
    bool (*finalize)(void *ctx);
    void (*progress)(void *ctx, uint32_t phase, uint64_t done, uint64_t total);
    void *ctx;
    bool fast;
    bool *rebuilt;
} sm_kraken_job_t;
// Reuse a matching cache or build a complete new cache before publishing it.
// The caller must hold the scanner's external mutation guard.
bool sm_kraken_prepare_base(const sm_pkg_info_t *info, const sm_kraken_job_t *job,
                            char cache[MAX_PATH]);
// Validate a patch against its mounted base and publish the cache on success.
bool sm_kraken_apply_patch(const sm_pkg_info_t *base, const sm_pkg_info_t *patch,
                           const sm_kraken_job_t *job);
bool sm_kraken_patch_source(const char *cache, char path[MAX_PATH]);
// Read the backing package path from an SMP Kraken cache, even if it is offline.
bool sm_kraken_source(const char *cache, char path[MAX_PATH]);
// Ejects and manual removals check the backing source without traversing the cache.
bool sm_kraken_source_ready(const char *cache);
// Resolve the verified source and grant the game read access before mounting it.
bool sm_kraken_launch_source(const char *cache, char source[MAX_PATH]);
// Apply the shared AMPR and per-title stub policy before mounting fakelib.
bool sm_kraken_refresh_runtime(const char *cache, const char *emulators_path,
                               bool playgo_stub_enabled);
// Write the AMPR-emu-NG runtime to path: a valid shared module from
// emulators_path (ampr-emu-ng/ first) or the bundled one. label is logged.
bool sm_kraken_install_ampr(const char *path, const char *emulators_path,
                            const char *label);
// Whether this cache can use the bundled PlayGo stub.
bool sm_kraken_has_bundled_playgo(const char *cache);
// Report and recover a registered cache whose selected base path is stale.
bool sm_kraken_can_relink(const char *cache, const sm_pkg_info_t *base);
bool sm_kraken_relink_base(const sm_pkg_info_t *base, const sm_kraken_job_t *job,
                           char cache[MAX_PATH], bool keep_patch);

#endif
