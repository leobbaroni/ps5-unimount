#ifndef SM_API_SERVICE_H
#define SM_API_SERVICE_H

#include <stdbool.h>

bool sm_api_service_start(void);
void sm_api_service_stop(void);
bool sm_api_service_reconfigure(void);
// Cancel the current PKG attempt, stop HTTP on sleep and recreate it on resume.
void sm_api_service_on_sleep_change(bool active);
// Start the Kraken base-PKG build+register storage job for a local debug PKG
// (POST /api/v1/packages/mount without HTTP). 0 when accepted; otherwise an
// errno value with *error_out set to an operation reason when known.
int sm_api_service_start_kraken_mount(const char *path, const char **error_out);

#endif
