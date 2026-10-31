#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "common.h"
#include "http.h"
#include "handlers.h"
#include "server.h"

/* HTTP request router: one connection at a time (see main.c).
 *
 * Endpoints (one handler each, see handlers_*.c):
 *   GET  /              - WebUI: Library tab (PC catalog + covers) and
 *                          Manual URL tab; installs a home-screen launcher
 *                          ("pkg remote installer", PKGS12800) on startup
 *   GET  /api            - probe (open, no action)
 *   GET  /api/status     - {"busy":true/false,"active":N} install state
 *   GET  /api/pc         - {"pc":"1.2.3.4","age":N} last PC announce
 *   GET  /api/space      - {"free":N,"total":N} /data disk space
 *                          (the PC broadcasts "PKGSENDER-PC ip:port" to UDP
 *                          12802 while Publish library is on; browsers can't
 *                          hear UDP, so we re-serve it here)
 *   GET  /install?url=   - install by query arg
 *   POST /api/install    - JSON {"packages":["<url>"]}
 *   POST /upload         - multipart form with a url field
 *   GET  /api/files/stat?path=            - file exists/size
 *   POST /api/files/mkdir  {"path":..}    - mkdir -p under /data/homebrew
 *   POST /api/files/write?path=..&offset= - raw chunk append
 *   POST /api/files/done   {"path":..,"size":N} - verify + toast
 *   POST /api/files/pull   {"url":..,"path":..,"mode":..} - PC -> console copy
 *   POST /api/pull/pause {"paused":1|0}   - pause/resume that copy
 *   POST /api/pull/cancel                   stop it, partial file stays
 *   GET  /logo.png, /favicon.ico - embedded tile logo
 *   Files tab + /api/fs/... : TEMP-DISABLED (see ENABLE_FILES_TAB)
 *
 * PKG install is sceAppInstUtilInstallByPackage, which accepts both
 * local paths (/data/xxx.pkg, mapped to /user/data/xxx.pkg) and remote
 * http:// URLs served from the PC (LAN install, no USB needed).
 * Other formats (exfat/ffpkg/ffpfsc/folders) are pushed as raw bytes
 * to /data/homebrew/ — mounting them afterwards is NOT our job.
 */

/* last raw request line, for /api/dbg */
char g_last_req[256] = "";

/* Route table. A path matches when it starts with `prefix` — or is
 * identical to it when `exact` is set (the plain probes must not
 * swallow the richer paths that share their stem). A NULL prefix (or
 * method) matches anything. First match wins, so the order below IS
 * the routing contract — including the GET catch-all that serves the
 * WebUI ahead of every later POST route. */
static const struct route {
        const char  *method;
        const char  *prefix;
        int          exact;
        handler_fn   fn;
} ROUTES[] = {
        { "GET",  "/api",              1, h_api_probe },
        { "GET",  "/install",          0, h_install_get },
        { "GET",  "/api/files/stat",   0, h_files_stat },
        { "POST", "/api/pull/pause",   0, h_pull_pause },
        { "POST", "/api/pull/cancel",  0, h_pull_cancel },
        { "GET",  "/api/version",      0, h_version },
        { "GET",  "/api/dbg",          0, h_dbg },
        { "GET",  "/api/status",       0, h_status },
        { "GET",  "/api/pc",           0, h_pc },
        { "GET",  "/api/space",        0, h_space },
#ifdef ENABLE_FILES_TAB
        { "GET",  "/api/fs/list",      0, h_fs_list },
        { "GET",  "/api/fs/info",      0, h_fs_info },
        { "POST", "/api/fs/rename",    0, h_fs_rename },
        { "POST", "/api/fs/copy",      0, h_fs_copy },
        { "POST", "/api/fs/move",      0, h_fs_move },
#else
        { NULL,   "/api/fs/",          0, h_fs_disabled },
#endif
        { "GET",  "/logo.png",         1, h_logo },
        { "GET",  "/favicon.ico",      1, h_logo },
        { "GET",  NULL,                0, h_index },
        { "POST", "/api/install",      0, h_install_post },
        { "POST", "/upload",           0, h_upload },
        { "POST", "/api/files/mkdir",  0, h_files_mkdir },
        { "POST", "/api/files/write",  0, h_files_write },
        { "POST", "/api/files/done",   0, h_files_done },
#ifdef ENABLE_FILES_TAB
        { "POST", "/api/fs/delete",    0, h_fs_delete },
#endif
        { "POST", "/api/files/pull",   0, h_files_pull },
};

static const struct route *
find_route(const char *method, const char *path)
{
        size_t i, n = sizeof(ROUTES) / sizeof(ROUTES[0]);

        for (i = 0; i < n; i++) {
                if (ROUTES[i].method &&
                    strcmp(method, ROUTES[i].method) != 0)
                        continue;
                if (ROUTES[i].prefix &&
                    (ROUTES[i].exact
                        ? strcmp(path, ROUTES[i].prefix) != 0
                        : strncmp(path, ROUTES[i].prefix,
                            strlen(ROUTES[i].prefix)) != 0))
                        continue;
                return &ROUTES[i];
        }
        return NULL;
}

void
handle_client(int fd)
{
        /* requests are handled strictly one at a time by the accept loop,
         * so one shared buffer serves every connection — no per-request
         * ~8 MB alloc/free churn for what is usually a ~100-byte poll */
        static char buf[HDR_MAX + BODY_MAX + 1];
        char method[16], path[URL_MAX + 64];
        long hdr_len, body_len;
        char *body;
        struct timeval tv;
        const struct route *r;

        /* a silent connection must never wedge the single-threaded loop */
        tv.tv_sec = 10;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        hdr_len = read_headers(fd, buf, HDR_MAX);
        if (hdr_len < 0) {
                close(fd);
                return;
        }

        if (sscanf(buf, "%15s %2079s", method, path) != 2) {
                close(fd);
                return;
        }
        /* absolute-form URIs (proxied browsers send
         * "GET http://host:port/path?query HTTP/1.1"): strip to origin-form. */
        {
                const char *abs = NULL;

                if (!strncmp(path, "http://", 7))
                        abs = strchr(path + 7, '/');
                else if (!strncmp(path, "https://", 8))
                        abs = strchr(path + 8, '/');
                if (abs && abs != path)
                        memmove(path, abs, strlen(abs) + 1);
                else if (!strncmp(path, "http://", 7) ||
                    !strncmp(path, "https://", 8)) {
                        strcpy(path, "/"); /* "http://host" with no path */
                } else if (path[0] == '\0' || path[0] != '/') {
                        close(fd);
                        return;
                }
        /* /api/dbg must not record itself, or it can only ever show itself */
        if (strncmp(path, "/api/dbg", 8) != 0)
                snprintf(g_last_req, sizeof(g_last_req), "%s %s",
                    method, path);
        }

#ifdef TEST_ONLY
        /* test build: probes + beacon stay alive, everything that can
         * install or write files is refused up front. */
        if (!strcmp(method, "POST") ||
            !strncmp(path, "/install", 8) ||
            !strncmp(path, "/api/files/", 11)) {
                send_text(fd, "Loopayeh: test build, installs disabled");
                close(fd);
                return;
        }
#endif

        body_len = content_length(buf);
        if (body_len < 0 || body_len > BODY_MAX) {
                send_text(fd, "Loopayeh: bad content length");
                close(fd);
                return;
        }
        /* HttpClient/curl wait for this before sending a POST body */
        if (strcasestr(buf, "expect: 100-continue"))
                send_all(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
        body = buf + hdr_len;
        /* header terminator "\r\n\r\n" is 4 bytes; body starts after it */
        {
                char *end = strstr(buf, "\r\n\r\n");

                if (end)
                        body = end + 4;
        }
        if (body_len > 0) {
                /* bytes after headers may already be in buf; a client may
                 * not drip the body forever either */
                long buffered = hdr_len - (body - buf);
                long missing = body_len - buffered;
                time_t deadline = time(NULL) + BODY_DEADLINE_S;

                while (missing > 0) {
                        ssize_t n;

                        if (time(NULL) > deadline)
                                break;
                        n = recv(fd, body + buffered, (size_t)missing, 0);
                        if (n <= 0)
                                break;
                        buffered += n;
                        missing -= n;
                }
                if ((size_t)(body - buf) + (size_t)body_len >=
                    HDR_MAX + BODY_MAX)
                        body_len = buffered;
                else
                        body[body_len] = '\0';
        } else {
                *body = '\0';
        }

        r = find_route(method, path);
        if (r)
                r->fn(fd, path, body, body_len);
        else
                send_text(fd, "Loopayeh: unknown endpoint");

        close(fd);
}
