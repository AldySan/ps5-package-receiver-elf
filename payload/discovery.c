#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include "common.h"
#include "discovery.h"

/* -- Console LAN IP --------------------------------------------------
 * The listener binds INADDR_ANY so it never learns its own address.
 * Primary: enumerate interfaces via SIOCGIFCONF and take the first
 * AF_INET address on an UP, non-loopback interface. Needs no routing,
 * gateway, DNS or internet, so it works on offline LAN consoles.
 * Secondary fallback: connect() a UDP socket at 8.8.8.8:53 (sends
 * nothing), then getsockname() reveals the LAN source address. */
static char g_lan_ip[64] = "";

void
resolve_lan_ip(void)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	struct ifconf ifc;
	char buf[1024];

	if (fd < 0)
		return;
	/* Primary: interface enumeration (routing-independent).
	 * NOTE: ifreq entries are variable-length on BSD (sa_len);
	 * never index them as a fixed array. Step with the SDK's
	 * _SIZEOF_ADDR_IFREQ macro instead. */
	memset(&ifc, 0, sizeof(ifc));
	ifc.ifc_len = sizeof(buf);
	ifc.ifc_buf = buf;
	if (ioctl(fd, SIOCGIFCONF, &ifc) == 0) {
		char *cp = buf;
		char *end = buf + ifc.ifc_len;
		while (cp + sizeof(struct ifreq) -
		    sizeof(struct sockaddr) < end) {
			struct ifreq *r = (struct ifreq *)cp;
			struct sockaddr_in *a;
			size_t sz;
			if (r->ifr_addr.sa_len == 0)
				break;		/* corrupt/truncated tail */
			sz = _SIZEOF_ADDR_IFREQ(*r);
			if ((size_t)(end - cp) < sz)
				break;		/* don't overrun the buffer */
			if (r->ifr_addr.sa_family == AF_INET) {
				if (ioctl(fd, SIOCGIFFLAGS, r) == 0 &&
				    (r->ifr_flags & IFF_UP) &&
				    !(r->ifr_flags & IFF_LOOPBACK)) {
					a = (struct sockaddr_in *)&r->ifr_addr;
					if (a->sin_addr.s_addr !=
					    htonl(INADDR_LOOPBACK) &&
					    a->sin_addr.s_addr !=
					    htonl(INADDR_ANY)) {
						inet_ntop(AF_INET, &a->sin_addr,
						    g_lan_ip, sizeof(g_lan_ip));
						break;
					}
				}
			}
			cp += sz;
		}
	}
	if (g_lan_ip[0]) {
		close(fd);
		return;
	}
	/* Secondary fallback: route-based lookup (needs internet route). */
	{
		struct sockaddr_in dst, local;
		socklen_t llen;

		memset(&dst, 0, sizeof(dst));
		dst.sin_family = AF_INET;
		dst.sin_port = htons(53);
		dst.sin_addr.s_addr = htonl(0x08080808); /* 8.8.8.8, no packet sent */
		if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) != 0) {
			close(fd);
			return;
		}
		llen = sizeof(local);
		memset(&local, 0, sizeof(local));
		if (getsockname(fd, (struct sockaddr *)&local, &llen) == 0 &&
		    local.sin_family == AF_INET &&
		    local.sin_addr.s_addr != htonl(INADDR_LOOPBACK) &&
		    local.sin_addr.s_addr != htonl(INADDR_ANY))
			inet_ntop(AF_INET, &local.sin_addr, g_lan_ip, sizeof(g_lan_ip));
		close(fd);
	}
}

/* resolved LAN address ("" until resolve_lan_ip ran) */
const char *
lan_ip(void)
{
	return g_lan_ip;
}

/* ── UDP discovery beacon ────────────────────────────────────────────
 * Every 3s broadcast "PKGSENDER v1" to 255.255.255.255:12801 so the PC
 * sender finds the console without a subnet sweep. Fire-and-forget:
 * beacon failure never affects installs. Sender IP = packet source. */
static void *
beacon_worker(void *arg)
{
	(void)arg;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in bc;
	int one = 1;
	char msg[96];

	if (fd < 0)
		return NULL;
	setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
	memset(&bc, 0, sizeof(bc));
	bc.sin_family = AF_INET;
	bc.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	bc.sin_port = htons(BEACON_PORT);
	if (g_lan_ip[0])
		snprintf(msg, sizeof(msg), "%s %s", BEACON_MSG, g_lan_ip);
	else
		snprintf(msg, sizeof(msg), "%s", BEACON_MSG);
	for (;;) {
		sendto(fd, msg, strlen(msg), 0,
		    (struct sockaddr *)&bc, sizeof(bc));
		sleep(3);
	}
	return NULL;
}

void
beacon_start(void)
{
	pthread_t tid;

	if (pthread_create(&tid, NULL, beacon_worker, NULL) == 0)
		pthread_detach(tid);
}
/* last PC auto-announce (UDP 12802), re-served as GET /api/pc. */
static char g_pc_addr[64];
static volatile time_t g_pc_seen;

/* ── PC auto-announce listener ─────────────────────────────────────────
 * While Publish library is on, the PC broadcasts "PKGSENDER-PC ip:port"
 * to UDP 12802 every 3s. Browsers can't hear UDP, so we listen here and
 * re-serve the last announcement to the Library page over HTTP
 * (GET /api/pc). */
#define PC_ANNOUNCE_PORT 12802
#define PC_ANNOUNCE_MAGIC "PKGSENDER-PC "

static void *
pc_listen_worker(void *arg)
{
	(void)arg;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in sa, from;
	socklen_t fl;
	char buf[128];
	ssize_t n;

	if (fd < 0)
		return NULL;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons(PC_ANNOUNCE_PORT);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		close(fd);
		return NULL;
	}
	for (;;) {
		fl = sizeof(from);
		n = recvfrom(fd, buf, sizeof(buf) - 1, 0,
		    (struct sockaddr *)&from, &fl);
		if (n <= 0)
			continue;
		buf[n] = '\0';
		if (strncmp(buf, PC_ANNOUNCE_MAGIC,
		    sizeof(PC_ANNOUNCE_MAGIC) - 1) != 0)
			continue;
		if (from.sin_family != AF_INET)
			continue;
		if (!inet_ntop(AF_INET, &from.sin_addr,
		    g_pc_addr, sizeof(g_pc_addr)))
			continue;
		g_pc_seen = time(NULL);
	}
	return NULL;
}

void
pc_listen_start(void)
{
	pthread_t tid;

	if (pthread_create(&tid, NULL, pc_listen_worker, NULL) == 0)
		pthread_detach(tid);
}

/* last PC that announced itself ("" if none yet) */
const char *
pc_addr(void)
{
	return g_pc_addr;
}

/* seconds since that announce, -1 if none yet */
long
pc_age(void)
{
	time_t now = time(NULL);

	return g_pc_seen > 0 ? (long)(now - g_pc_seen) : -1;
}
