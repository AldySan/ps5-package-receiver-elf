/* PKG Sender receiver - standalone PS5 payload.
 *
 * No etaHEN, no arsenal. Chain: jailbreak -> kstuff -> pkg-receiver.elf
 *
 * Layout: main.c (startup + accept loop), server.c (HTTP router),
 * installer.c (AppInstUtil + install queue + home-screen launcher),
 * pull.c (PC -> /data/homebrew copy), http.c (reply + parse helpers),
 * files.c (path jailing), webui.c (embedded page + logo),
 * discovery.c (LAN IP + UDP beacon + PC announce), notify.c (toasts).
 *
 * Listens on TCP 12800 (INADDR_ANY, LAN-reachable); endpoints are
 * documented at the top of server.c. UDP beacon: "PKGSENDER v1"
 * broadcast to 255.255.255.255:12801 every 3s.
 *
 * TEST_ONLY build (make TEST_ONLY=1): beacon + /api + /api/status work,
 * every install/file path is refused. Discovery testing only — nobody
 * can install anything with it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>

#include "common.h"
#include "notify.h"
#include "discovery.h"
#include "installer.h"
#include "server.h"

/* active install count for GET /api/status (multi-PKG queue pacing) */
volatile int g_active_installs = 0;

/* ── Process identity + self-replacement ───────────────────────────────
 * Name our main thread so process managers (e.g. itsPLK's
 * ps5-payload-manager, which lists ki_comm/ki_tdname) show us as
 * pkg-receiver.elf, and kill any previous instance on startup so
 * re-injecting just works without a console reboot. */
#define RECEIVER_NAME "pkg-receiver.elf"

/* pid of another live process whose thread name matches ours, else -1 */
static pid_t
find_receiver_peer(void)
{
	int mib[4] = { 1, 14, 8, 0 };
	pid_t self = getpid();
	pid_t found = -1;
	size_t len = 0;
	uint8_t *buf, *p, *end;

	if (sysctl(mib, 4, NULL, &len, NULL, 0) != 0 || len == 0)
		return -1;
	buf = malloc(len);
	if (!buf)
		return -1;
	if (sysctl(mib, 4, buf, &len, NULL, 0) != 0) {
		free(buf);
		return -1;
	}
	end = buf + len;
	for (p = buf; p + (int)sizeof(int) <= end;) {
		int sz = *(int *)p;
		pid_t pid;
		if (sz < 468 || p + sz > end)
			break;
		pid = *(pid_t *)(p + 72);
		if (pid != self && pid > 0 &&
		    strncmp((char *)(p + 447), RECEIVER_NAME,
		        sizeof(RECEIVER_NAME)) == 0)
			found = pid;
		p += sz;
	}
	free(buf);
	return found;
}

int
main(void)
{
	int srv, cl;
	int opt = 1;
	struct sockaddr_in sa;

	/* a browser or sender that hangs up mid-reply must not kill the
	 * payload: writes to a closed socket raise SIGPIPE, whose default
	 * action terminates the process */
	signal(SIGPIPE, SIG_IGN);

	syscall(SYS_thr_set_name, -1, RECEIVER_NAME);

	/* replace any previous instance: re-inject needs no reboot */
	for (;;) {
		pid_t old = find_receiver_peer();
		if (old <= 0)
			break;
		if (kill(old, SIGKILL) != 0)
			break;
		sleep(1);
	}

	srv = socket(AF_INET, SOCK_STREAM, 0);
	if (srv < 0) {
		notify_user("Loopayeh: socket failed, exiting");
		return 1;
	}
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons(DPI_PORT);

	if (bind(srv, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		notify_user("Loopayeh: port 12800 busy, exiting");
		return 1;
	}
	if (listen(srv, 8) < 0) {
		notify_user("Loopayeh: listen failed, exiting");
		return 1;
	}

	resolve_lan_ip();
	{
		char hello[128];
#ifdef TEST_ONLY
		if (lan_ip()[0])
			snprintf(hello, sizeof(hello),
			    "Loopayeh: TEST %s:12800 (no installs)", lan_ip());
		else
			snprintf(hello, sizeof(hello),
			    "Loopayeh: TEST BUILD listening (no installs)");
#else
		if (lan_ip()[0])
			snprintf(hello, sizeof(hello),
			    "Loopayeh: %s:12800 listening", lan_ip());
		else
			snprintf(hello, sizeof(hello),
			    "Loopayeh: listening on port 12800");
#endif
		notify_user(hello);
	}

	beacon_start();
	pc_listen_start();

#ifndef TEST_ONLY
	launcher_install_if_needed();
#endif

	for (;;) {
		cl = accept(srv, NULL, NULL);
		if (cl < 0)
			continue;
		handle_client(cl);
	}

	return 0;
}
