#include <stdio.h>
#include <string.h>

#include "notify.h"

/* ── PS5 system notification (visible toast on the console) ──────────── */
typedef struct notify_request {
	char unused[45];
	char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int device, notify_request_t *request,
                                     size_t size, int unused);

void
notify_user(const char *msg)
{
	notify_request_t req;

	memset(&req, 0, sizeof(req));
	snprintf(req.message, sizeof(req.message), "%s", msg);
	sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}
