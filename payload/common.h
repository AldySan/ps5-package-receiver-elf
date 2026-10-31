#ifndef COMMON_H
#define COMMON_H

/* Shared wire-protocol constants and the build tag. */

#define DPI_PORT   12800
#define BEACON_PORT 12801
#define BEACON_MSG  "PKGSENDER v1"
#define HDR_MAX    16384
#define BODY_MAX   (8 * 1024 * 1024)
#define URL_MAX    2048
#define PATH_MAX_V 1024
/* total time a single request may take to arrive; per-recv socket
 * timeouts alone cannot stop a client that drips one byte at a time */
#define HDR_DEADLINE_S 15
#define BODY_DEADLINE_S 30

/* bump on every behavior change; the page shows receiver vs page tags */
#ifdef TEST_ONLY
#define RECEIVER_BUILD "20261031-01-TEST"
#else
#define RECEIVER_BUILD "20261031-01"
#endif

/* File explorer (Files tab + /api/fs/...) is TEMPORARILY DISABLED.
 * Full code is kept in files.c behind ENABLE_FILES_TAB.
 * To re-enable: #define ENABLE_FILES_TAB 1 */
/* #define ENABLE_FILES_TAB 1 */

/* active install/copy job count for GET /api/status (installs and pulls
 * both bump it; defined in main.c) */
extern volatile int g_active_installs;

#endif /* COMMON_H */
