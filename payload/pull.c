#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "common.h"
#include "notify.h"
#include "files.h"
#include "pull.h"

/* ── Pull downloader (Images tab: copy PC file -> /data/homebrew) ─────
 * Plain sequential HTTP GET (http only, the PC serves plain http).
 * Runs on a detached worker; skips when the same size is already there. */
typedef struct pull_job {
	char url[URL_MAX];
	char local[PATH_MAX_V];
	int resume;
} pull_job_t;

/* pull progress, visible in GET /api/status while a copy runs */
static volatile int g_pull_active = 0;
static volatile int g_pull_paused = 0;
static volatile int g_pull_cancel = 0;
static volatile long long g_pull_got = 0;
static volatile long long g_pull_want = -1;
static char g_pull_name[128];

/* 0 = ok, 1 = skipped (same size present), -1 = error */
#define PULL_SEGS 16
#define PULL_CHUNK (1024 * 1024)

typedef struct pull_seg {
	char host[256];
	char portstr[16];
	char get[URL_MAX];
	char local[PATH_MAX_V];
	long long start;
	long long len;
	int ok;
} pull_seg_t;

/* tuned socket (timeouts + big receive buffer), connected or -1 */
static int
pull_connect(const char *host, const char *portstr)
{
	struct addrinfo hints, *res = NULL, *rp;
	int s = -1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
		return -1;
	for (rp = res; rp; rp = rp->ai_next) {
		s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (s < 0)
			continue;
		if (connect(s, rp->ai_addr, rp->ai_addrlen) == 0)
			break;
		close(s);
		s = -1;
	}
	freeaddrinfo(res);
	if (s >= 0) {
		struct timeval tv;
		int rcv = 4 * 1024 * 1024;

		tv.tv_sec = 30;
		tv.tv_usec = 0;
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
	}
	return s;
}

/* read a response header block; 1 = 2xx headers in hs, 0 = fail */
static int
pull_headers(int s, char *hs, size_t hs_sz)
{
	char c;
	size_t hl = 0;
	ssize_t n = -1;
	int term = 0;

	for (;;) {
		n = recv(s, &c, 1, 0);
		if (n <= 0)
			break;
		if (hl + 1 >= hs_sz)
			break; /* headers bigger than the buffer */
		hs[hl++] = c;
		hs[hl] = '\0';
		if (hl >= 4 && !strcmp(hs + hl - 4, "\r\n\r\n")) {
			term = 1;
			break;
		}
	}
	/* without the terminator we are mid-headers: the bytes after
	 * them are NOT body bytes, so never report success */
	if (!term || hl < 12 || strncmp(hs, "HTTP/", 5) != 0)
		return 0;
	return hs[9] == '2';
}

/* three-digit status code of a complete header block */
static int
http_status(const char *hs)
{
	return (hs[9] - '0') * 100 + (hs[10] - '0') * 10 + (hs[11] - '0');
}

/* Content-Length value in a header block, or -1 */
static long long
pull_clen(const char *hs)
{
	const char *p = strstr(hs, "Content-Length:");

	if (!p)
		p = strstr(hs, "content-length:");
	if (!p)
		return -1;
	return strtoll(p + 15, NULL, 10);
}

/* total size from a 206 header block's Content-Range (bytes x-y/N), or -1 */
static long long
pull_crange_total(const char *hs)
{
	const char *p = strstr(hs, "Content-Range:");

	if (!p)
		p = strstr(hs, "content-range:");
	if (!p)
		return -1;
	p = strchr(p, '/');
	if (!p)
		return -1;
	return strtoll(p + 1, NULL, 10);
}

static ssize_t
pull_send(int s, const char *b, size_t n)
{
	size_t off = 0;

	while (off < n) {
		ssize_t w = send(s, b + off, n - off, 0);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		off += (size_t)w;
	}
	return (ssize_t)off;
}

/* one range segment: own connection, own fd, writes [start, start+len) */
static void *
pull_seg_worker(void *arg)
{
	pull_seg_t *sg = arg;
	char req[URL_MAX + 256], hs[4096];
	char *hb;
	int s = -1, out = -1;
	long long left;
	ssize_t n;

	sg->ok = 0;
	s = pull_connect(sg->host, sg->portstr);
	if (s < 0)
		return NULL;
	snprintf(req, sizeof(req),
	    "GET %s HTTP/1.0\r\nHost: %s\r\n"
	    "Range: bytes=%lld-%lld\r\nConnection: close\r\n\r\n",
	    sg->get, sg->host, sg->start, sg->start + sg->len - 1);
	if (pull_send(s, req, strlen(req)) < 0) {
		close(s);
		return NULL;
	}
	/* a 200 here means the server ignored our Range and would send
	 * the whole file from byte 0 — writing it at this segment's
	 * offset would corrupt the copy, so insist on 206 */
	if (!pull_headers(s, hs, sizeof(hs)) || http_status(hs) != 206) {
		close(s);
		return NULL;
	}
	hb = malloc(PULL_CHUNK);
	out = open(sg->local, O_WRONLY);
	if (!hb || out < 0) {
		free(hb);
		if (out >= 0)
			close(out);
		close(s);
		return NULL;
	}
	if (lseek(out, (off_t)sg->start, SEEK_SET) == (off_t)-1) {
		free(hb);
		close(out);
		close(s);
		return NULL;
	}
	left = sg->len;
	while (left > 0 && !g_pull_cancel) {
		size_t want = (size_t)(left < PULL_CHUNK ? left : PULL_CHUNK);
		size_t got = 0;

		while (g_pull_paused && !g_pull_cancel)
			sleep(1);
		if (g_pull_cancel)
			break;
		n = recv(s, hb, want, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0)
			break;
		while (got < (size_t)n) {
			ssize_t w = write(out, hb + got, (size_t)n - got);

			if (w < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			got += (size_t)w;
		}
		if (got != (size_t)n)
			break;
		left -= n;
		__sync_fetch_and_add(&g_pull_got, n);
	}
	free(hb);
	close(out);
	close(s);
	sg->ok = (left == 0);
	return NULL;
}

/* 0 = ok, 1 = skipped (same size present), -1 = error
 * resume = continue a partial local file instead of starting over. */
static int
pull_download(const char *url, const char *local, int resume)
{
	const char *p = url + 7; /* skip http:// */
	const char *slash = strchr(p, '/');
	char host[256], get[URL_MAX], req[URL_MAX + 256];
	char portstr[16] = "80";
	int s = -1, out = -1, ranges_ok = 0;
	ssize_t n;
	long long want = -1, got = 0, have = -1, base = 0;
	struct stat st;

	if (!slash || (size_t)(slash - p) >= sizeof(host))
		return -1;
	memcpy(host, p, (size_t)(slash - p));
	host[slash - p] = '\0';
	snprintf(get, sizeof(get), "%s", slash);
	p = strchr(host, ':');
	if (p) {
		snprintf(portstr, sizeof(portstr), "%s", p + 1);
		host[p - host] = '\0';
	}
	/* size discovery first: HEAD with a one-byte Range probe. A 206
	 * proves the server honors Range — which the multi-segment path
	 * below requires — and carries the real size in Content-Range. A
	 * plain 200 means Range is ignored, so big files take the single
	 * stream instead of failing all 16 segments. */
	{
		char hs[4096];

		s = pull_connect(host, portstr);
		if (s < 0)
			return -1;
		snprintf(req, sizeof(req),
		    "HEAD %s HTTP/1.0\r\nHost: %s\r\n"
		    "Range: bytes=0-0\r\nConnection: close\r\n\r\n",
		    get, host);
		if (pull_send(s, req, strlen(req)) < 0 ||
		    !pull_headers(s, hs, sizeof(hs)))
			want = -1;
		else if (http_status(hs) == 206) {
			/* 206: Content-Length is just the probed byte; the
			 * total size lives in Content-Range */
			want = pull_crange_total(hs);
			ranges_ok = 1;
		} else
			want = pull_clen(hs);
		close(s);
		s = -1;
	}
	if (want >= 0 && stat(local, &st) == 0 &&
	    (long long)st.st_size == want)
		return 1; /* already there */
	/* resume point: a valid sequential prefix is kept and finished by the
	 * single-stream path below (parallel ranges are never resumed) */
	have = (want >= 0 && stat(local, &st) == 0) ? (long long)st.st_size : -1;
	base = (resume && want > 0 && have > 0 && have < want) ? have : 0;
	/* /data/homebrew may not exist yet — create the parent chain first */
	{
		char dir[PATH_MAX_V], *slash;

		snprintf(dir, sizeof(dir), "%s", local);
		slash = strrchr(dir, '/');
		if (slash && slash != dir) {
			*slash = '\0';
			if (mkdir_p(dir) != 0)
				return -1;
		}
	}
	g_pull_want = want;
	g_pull_got = base;
	/* fast path only for fresh downloads: parallel writers scatter their
	 * ranges, so a partial they leave behind is not a valid prefix and
	 * must never be resumed (and never look complete). */
	if (want >= 2 * 1024 * 1024 && base == 0 && ranges_ok &&
	    strlen(local) + 6 < PATH_MAX_V) {
		/* ── fast path: PULL_SEGS parallel Range streams ──
		 * Range streams write to <local>.part; rename() publishes the
		 * file only after every segment finished, so a failed, cancelled
		 * or power-cut copy never leaves a full-size holey file behind.
		 * heap, not stack: worker threads have small stacks. */
		char part[PATH_MAX_V];
		pull_seg_t *segs = malloc(sizeof(*segs) * PULL_SEGS);
		pthread_t *tids = malloc(sizeof(*tids) * PULL_SEGS);
		long long seglen = want / PULL_SEGS;
		int i, alive = 0, fail = 0, rc = -1;

		snprintf(part, sizeof(part), "%s.part", local);
		if (!segs || !tids) {
			free(segs);
			free(tids);
			return -1;
		}

		out = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (out < 0) {
			free(segs);
			free(tids);
			return -1;
		}
		/* pre-size so every segment has its range on disk */
		if (ftruncate(out, (off_t)want) != 0) {
			close(out);
			free(segs);
			free(tids);
			return -1;
		}
		close(out);
		for (i = 0; i < PULL_SEGS; i++) {
			long long s0 = seglen * i;
			long long s1 = (i == PULL_SEGS - 1) ? want : seglen * (i + 1);

			snprintf(segs[i].host, sizeof(segs[i].host), "%s", host);
			snprintf(segs[i].portstr, sizeof(segs[i].portstr), "%s", portstr);
			snprintf(segs[i].get, sizeof(segs[i].get), "%s", get);
			snprintf(segs[i].local, sizeof(segs[i].local), "%s", part);
			segs[i].start = s0;
			segs[i].len = s1 - s0;
			if (segs[i].len <= 0) {
				segs[i].ok = 1; /* nothing left in this slice */
				continue;
			}
			segs[i].ok = 0;
			if (pthread_create(&tids[alive], NULL, pull_seg_worker, &segs[i]) != 0) {
				fail = 1;
				break;
			}
			alive++;
		}
		if (fail) {
			for (i = 0; i < alive; i++)
				pthread_join(tids[i], NULL);
			free(segs);
			free(tids);
			unlink(part);
			return -1;
		}
		for (i = 0; i < alive; i++)
			pthread_join(tids[i], NULL);
		rc = 0;
		for (i = 0; i < PULL_SEGS; i++)
			if (!segs[i].ok)
				rc = -1;
		free(segs);
		free(tids);
		if (rc != 0) {
			/* a range failed or was cancelled: the scattered partial is
			 * worthless — drop it, never resume it */
			unlink(part);
			return -1;
		}
		if (stat(part, &st) != 0 || (long long)st.st_size != want) {
			unlink(part);
			return -1;
		}
		if (rename(part, local) != 0) {
			unlink(part);
			return -1;
		}
		return 0;
	}
	/* ── small/unknown size: classic single stream ── */
	s = pull_connect(host, portstr);
	if (s < 0)
		return -1;
	if (base > 0)
		snprintf(req, sizeof(req),
		    "GET %s HTTP/1.0\r\nHost: %s\r\n"
		    "Range: bytes=%lld-\r\nConnection: close\r\n\r\n",
		    get, host, base);
	else
		snprintf(req, sizeof(req),
		    "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
		    get, host);
	if (pull_send(s, req, strlen(req)) < 0) {
		close(s);
		return -1;
	}
	{
		char hs[4096];

		if (!pull_headers(s, hs, sizeof(hs)) ||
		    (base > 0 && http_status(hs) != 206)) {
			/* non-206 on a resume means the server ignored the
			 * Range: appending the full body at `base` would
			 * corrupt the file, so fail the copy instead */
			close(s);
			return -1;
		}
		if (want < 0)
			want = pull_clen(hs);
	}
	out = open(local, O_WRONLY | O_CREAT | (base ? 0 : O_TRUNC), 0644);
	if (out < 0) {
		close(s);
		return -1;
	}
	if (base > 0 && lseek(out, (off_t)base, SEEK_SET) == (off_t)-1) {
		close(s);
		close(out);
		return -1;
	}
	g_pull_want = want;
	g_pull_got = base;
	got = base;
	/* heap, not stack: PULL_CHUNK would risk the worker thread's stack */
	{
		char *hb = malloc(PULL_CHUNK);
		int done = 0;

		if (!hb) {
			close(s);
			close(out);
			return -1;
		}
		for (;;) {
			while (g_pull_paused && !g_pull_cancel)
				sleep(1);
			if (g_pull_cancel)
				break;
			n = recv(s, hb, PULL_CHUNK, 0);
			if (n < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			if (n == 0) {
				done = 1;
				break;
			}
			if (write(out, hb, (size_t)n) != n)
				break;
			got += n;
			__sync_fetch_and_add(&g_pull_got, n);
		}
		free(hb);
		close(s);
		close(out);
		if (!done)
			return -1;
		if (want >= 0 && got != want)
			return -1;
		return 0;
	}
}

static void *
pull_worker(void *arg)
{
	pull_job_t *job = arg;
	char toast[256], base[128];
	const char *b = strrchr(job->local, '/');
	int rc;

	snprintf(base, sizeof(base), "%s", b ? b + 1 : job->local);
	/* keep /api/status JSON valid: no quotes/backslashes in the name.
	 * Sanitize before publishing so a concurrent poll never sees a
	 * half-published name with quotes still in it. */
	{
		char *q;

		for (q = base; *q; q++)
			if (*q == '"' || *q == '\\')
				*q = '_';
	}
	snprintf(g_pull_name, sizeof(g_pull_name), "%s", base);
	/* g_pull_active was claimed by pull_start before we existed,
	 * and so was the cancel-flag reset (see pull_start) */
	__sync_fetch_and_add(&g_active_installs, 1);
  	rc = pull_download(job->url, job->local, job->resume);
	__sync_fetch_and_sub(&g_active_installs, 1);
	g_pull_active = 0;
	if (g_pull_cancel)
		snprintf(toast, sizeof(toast), "Loopayeh: copy stopped %s", base);
	else if (rc == 0)
		snprintf(toast, sizeof(toast), "Loopayeh: %s %s",
		    job->resume ? "resumed" : "copied", base);
	else if (rc == 1)
		snprintf(toast, sizeof(toast),
		    "Loopayeh: %s already there", base);
	else
		snprintf(toast, sizeof(toast),
		    "Loopayeh: copy failed %s", base);
	notify_user(toast);
	free(job);
	return NULL;
}

/* one copy at a time: two concurrent pulls would fight over the shared
 * progress state and corrupt the destination (the claim below is
 * race-free — the single server thread is the only caller) */
int
pull_start(const char *url, const char *local, int resume)
{
	pull_job_t *job;
	pthread_t tid;

	if (g_pull_active)
		return 1;
	/* reset any stale cancel from the previous job HERE, on the
	 * server thread, before the worker exists: requests are
	 * serialized by the single-threaded accept loop, so a cancel
	 * that arrives after this reply can no longer be swallowed by
	 * the worker resetting the flag itself */
	g_pull_cancel = 0;
	g_pull_active = 1;
	job = malloc(sizeof(*job));
	if (!job) {
		g_pull_active = 0;
		return -1;
	}
	snprintf(job->url, sizeof(job->url), "%s", url);
	snprintf(job->local, sizeof(job->local), "%s", local);
	job->resume = resume;
	if (pthread_create(&tid, NULL, pull_worker, job) != 0) {
		free(job);
		g_pull_active = 0;
		return -2;
	}
	pthread_detach(tid);
	return 0;
}

/* progress accessors for GET /api/status and the pause/cancel endpoints */
int
pull_active(void)
{
	return g_pull_active;
}

int
pull_paused(void)
{
	return g_pull_paused;
}

void
pull_set_paused(int paused)
{
	g_pull_paused = paused ? 1 : 0;
}

void
pull_cancel(void)
{
	g_pull_cancel = 1;
}

long long
pull_got(void)
{
	return g_pull_got;
}

long long
pull_want(void)
{
	return g_pull_want;
}

const char *
pull_name(void)
{
	return g_pull_name;
}
