#ifndef SM_UNIMOUNT_H
#define SM_UNIMOUNT_H

// Product identity. The Makefile passes UNIMOUNT_VERSION from the root VERSION
// file and KRAKEN_MOUNT_REVISION from vendor/kraken/REVISION.
#define UNIMOUNT_PRODUCT "UniMount"

#ifndef UNIMOUNT_VERSION
#define UNIMOUNT_VERSION "unknown"
#endif

#ifndef KRAKEN_MOUNT_REVISION
#define KRAKEN_MOUNT_REVISION "unknown"
#endif

// Kraken Mount release matching vendor/kraken/REVISION (e1e7845 = v0.3.0).
// Reported as "version" by the Kraken-compatible /api/state.
#define KRAKEN_MOUNT_RELEASE "0.3.1"

#endif
