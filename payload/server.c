#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include "common.h"
#include "notify.h"
#include "http.h"
#include "files.h"
#include "installer.h"
#include "webui.h"
#include "pull.h"
#include "discovery.h"
#include "server.h"

/* HTTP request router: one connection at a time (see main.c).
 *
 * Endpoints:
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
 *   Files tab + /api/fs/... : TEMP-DISABLED (see ENABLE_FILES_TAB)
 *
 * PKG install is sceAppInstUtilInstallByPackage, which accepts both
 * local paths (/data/xxx.pkg, mapped to /user/data/xxx.pkg) and remote
 * http:// URLs served from the PC (LAN install, no USB needed).
 * Other formats (exfat/ffpkg/ffpfsc/folders) are pushed as raw bytes
 * to /data/homebrew/ — mounting them afterwards is NOT our job.
 */

/* last raw request line, for /api/dbg */
static char g_last_req[256] = "";

static void
do_install_reply_text(int fd, const char *url, const char *name,
                      const char *icon)
{
	char disp[256], out[URL_MAX + 64];

	if (name && *name)
		snprintf(disp, sizeof(disp), "%s", name);
	else {
		const char *base = strrchr(url, '/');
		base = base ? base + 1 : url;
		snprintf(disp, sizeof(disp), "%s", base);
	}
	if (queue_install(url, name, icon) == 0)
		snprintf(out, sizeof(out), "ok: install queued for %s", disp);
	else
		snprintf(out, sizeof(out), "error:queue failed");
	send_text(fd, out);
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
	char url[URL_MAX];
	struct timeval tv;

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
		snprintf(g_last_req, sizeof(g_last_req), "%s %s", method, path);
	}

#ifdef TEST_ONLY
	/* test build: probes + beacon stay alive, everything that can
	 * install or write files is refused up front. */
	if (!strcmp(method, "POST") ||
	    !strncmp(path, "/install", 8) ||
	    !strncmp(path, "/api/files/", 11)) {
		send_text(fd, "Loopayeh: test build, installs disabled");
		goto handled;
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
			n = recv(fd, body + buffered,
			    (size_t)missing, 0);
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

	if (!strcmp(method, "GET") && !strcmp(path, "/api")) {
		/* open probe: identifies us, performs nothing */
		send_json(fd,
		    "{\"status\":\"fail\","
		    "\"error\":\"Unsupported method: use POST /api/install\"}");
	} else if (!strcmp(method, "GET") &&
	           (!strncmp(path, "/install", 8))) {
		char gname[256];
		char gicon[512];

		if (query_url(path, url, sizeof(url))) {
			gname[0] = '\0';
			gicon[0] = '\0';
			query_param(path, "name", gname, sizeof(gname));
			query_param(path, "icon", gicon, sizeof(gicon));
			do_install_reply_text(fd, url, gname[0] ? gname : NULL,
			                      gicon[0] ? gicon : NULL);
		} else {
			send_text(fd, "error:missing url");
		}
	} else if (!strcmp(method, "GET") &&
	           !strncmp(path, "/api/files/stat", 15)) {
		char rpath[URL_MAX], local[PATH_MAX_V];
		struct stat st;

		if (!query_param(path, "path", rpath, sizeof(rpath)) ||
		    jail_path(rpath, local, sizeof(local)) != 0) {
			send_text(fd, "error:bad path");
		} else if (stat(local, &st) == 0) {
			char out[128];
			long long sz = S_ISDIR(st.st_mode) ? 0 : (long long)st.st_size;
			snprintf(out, sizeof(out),
			    "{\"exists\":true,\"size\":%lld}", sz);
			send_json(fd, out);
		} else {
			send_json(fd, "{\"exists\":false,\"size\":0}");
		}
	} else if (!strcmp(method, "POST") &&
	           !strncmp(path, "/api/pull/pause", 15)) {
		long long paused = 1;
		int p;

		json_long(body, "paused", &paused);
		p = paused ? 1 : 0;
		pull_set_paused(p);
		send_json(fd, p ? "{\"ok\":true,\"paused\":true}"
		    : "{\"ok\":true,\"paused\":false}");
	} else if (!strcmp(method, "POST") &&
	           !strncmp(path, "/api/pull/cancel", 16)) {
		/* stop the running pull; the partial file stays for resume */
		pull_cancel();
		pull_set_paused(0);
		send_json(fd, "{\"ok\":true,\"cancelled\":true}");

	} else if (!strcmp(method, "GET") &&
	           !strncmp(path, "/api/version", 12)) {
		send_json(fd, "{\"build\":\"" RECEIVER_BUILD "\"}");
	} else if (!strcmp(method, "GET") &&
	           !strncmp(path, "/api/dbg", 8)) {
		char out[320], esc[256];

		json_escape(g_last_req, esc, sizeof(esc));
		snprintf(out, sizeof(out), "{\"last\":\"%s\"}", esc);
		send_json(fd, out);
	} else if (!strcmp(method, "GET") &&
	           !strncmp(path, "/api/status", 11)) {
		char out[512];

		snprintf(out, sizeof(out), "{\"busy\":%s,\"active\":%d,"
		    "\"pull\":%s,\"pullName\":\"%s\","
		    "\"pullGot\":%lld,\"pullWant\":%lld,\"pullPaused\":%s}",
		    g_active_installs > 0 ? "true" : "false",
		    g_active_installs,
		    pull_active() ? "true" : "false",
		    pull_active() ? pull_name() : "",
		    pull_got(), pull_want(),
		    pull_paused() ? "true" : "false");
		send_json(fd, out);
	} else if (!strcmp(method, "GET") &&
	           !strncmp(path, "/api/pc", 7)) {
		char out[128];

		snprintf(out, sizeof(out), "{\"pc\":\"%s\",\"age\":%ld}}",
		    pc_addr(), pc_age());
		send_json(fd, out);

	} else if (!strcmp(method, "GET") &&
	           !strncmp(path, "/api/space", 10)) {
		char out[128];
		struct statvfs sv;
		long long bfree = -1, btotal = -1;

		if (statvfs("/data", &sv) == 0) {
			bfree = (long long)sv.f_bavail * sv.f_frsize;
			btotal = (long long)sv.f_blocks * sv.f_frsize;
		}
		snprintf(out, sizeof(out), "{\"free\":%lld,\"total\":%lld}",
		    bfree, btotal);
		send_json(fd, out);
#ifdef ENABLE_FILES_TAB
	} else if (!strcmp(method, "GET") &&
	           !strncmp(path, "/api/fs/list", sizeof("/api/fs/list") - 1)) {
		char rpath[PATH_MAX_V], local[PATH_MAX_V];
		char escpath[PATH_MAX_V * 2];
		DIR *dp;
		struct dirent *de;
		fs_entry_t *ents;
		size_t n = 0;
		char *json;
		size_t jlen = 65536, joff;
		int truncated = 0;

		if (!query_param(path, "path", rpath, sizeof(rpath)) ||
		    fs_src_jail(rpath, local, sizeof(local)) != 0) {
			send_text(fd, "error:bad path");
		} else if (!(dp = opendir(local))) {
			send_text(fd, "error:not a directory");
		} else {
			ents = malloc(sizeof(fs_entry_t) * FS_MAX_ENTRIES);
			if (ents) {
				while (n < FS_MAX_ENTRIES &&
				    (de = readdir(dp)) != NULL) {
					int is_dir;
					if (!strcmp(de->d_name, ".") ||
					    !strcmp(de->d_name, ".."))
						continue;
					if (de->d_type == DT_UNKNOWN) {
						char full[PATH_MAX_V * 2];
						struct stat st;
						snprintf(full, sizeof(full),
						    "%s/%s", local, de->d_name);
						is_dir = stat(full, &st) == 0 &&
						    S_ISDIR(st.st_mode);
					} else {
						is_dir = de->d_type == DT_DIR;
					}
					snprintf(ents[n].name,
					    sizeof(ents[n].name), "%s",
					    de->d_name);
					ents[n].is_dir = is_dir;
					ents[n].size = 0;
					if (!is_dir) {
						char full[PATH_MAX_V * 2];
						struct stat st;
						snprintf(full, sizeof(full),
						    "%s/%s", local, de->d_name);
						if (stat(full, &st) == 0)
							ents[n].size =
							    (long long)st.st_size;
					}
					n++;
				}
				if (n == FS_MAX_ENTRIES &&
				    readdir(dp) != NULL)
					truncated = 1;
				qsort(ents, n, sizeof(fs_entry_t),
				    fs_entry_cmp);
			}
			closedir(dp);
			json_escape(local, escpath, sizeof(escpath));
			json = malloc(jlen);
			if (!ents || !json) {
				free(ents);
				free(json);
				send_text(fd, "error:out of memory");
			} else {
				size_t i, toff;
				joff = (size_t)snprintf(json, jlen,
				    "{\"path\":\"%s\",\"truncated\":",
				    escpath);
				toff = joff; /* single flag digit patched below */
				joff += (size_t)snprintf(json + joff,
				    jlen - joff, "0,\"entries\":[");
				for (i = 0; i < n; i++) {
					char nm[512], row[800];
					int need;
					json_escape(ents[i].name, nm,
					    sizeof(nm));
					need = snprintf(row, sizeof(row),
					    "%s{\"name\":\"%s\",\"dir\":%s,\"size\":%lld}",
					    i ? "," : "", nm,
					    ents[i].is_dir ? "true" : "false",
					    ents[i].size);
					if (joff + (size_t)need + 32 >= jlen) {
						truncated = 1;
						break;
					}
					memcpy(json + joff, row,
					    (size_t)need);
					joff += (size_t)need;
				}
				free(ents);
				if (truncated)
					json[toff] = '1';
				memcpy(json + joff, "]}", 3);
				send_json(fd, json);
				free(json);
			}
		}
	} else if (!strcmp(method, "GET") &&
	           !strncmp(path, "/api/fs/info", sizeof("/api/fs/info") - 1)) {
		char rpath[PATH_MAX_V], local[PATH_MAX_V];
		char escname[512];
		struct stat st;
		const char *slash, *dot, *kind = "file";
		char magic[33] = "";
		char out[2048];
		int need;

		if (!query_param(path, "path", rpath, sizeof(rpath)) ||
		    fs_src_jail(rpath, local, sizeof(local)) != 0) {
			send_text(fd, "error:bad path");
		} else if (stat(local, &st) != 0) {
			send_text(fd, "error:not found");
		} else {
			if (S_ISDIR(st.st_mode)) {
				kind = "folder";
			} else {
				int f = open(local, O_RDONLY);
				if (f >= 0) {
					unsigned char hb[16];
					ssize_t n = read(f, hb, sizeof(hb));
					int i;
					for (i = 0; i < n; i++)
						snprintf(magic + i * 2, 3, "%02x", hb[i]);
					close(f);
				}
				slash = strrchr(local, '/');
				dot = strrchr(slash ? slash : local, '.');
				if (dot) {
					if (!strcasecmp(dot, ".pkg"))
						kind = "pkg";
					else if (!strcasecmp(dot, ".exfat") ||
					    !strcasecmp(dot, ".ffpkg") ||
					    !strcasecmp(dot, ".ffpfsc"))
						kind = "image";
					else if (!strcasecmp(dot, ".elf"))
						kind = "elf";
					else if (!strcasecmp(dot, ".png") ||
					    !strcasecmp(dot, ".jpg") ||
					    !strcasecmp(dot, ".jpeg"))
						kind = "picture";
					else if (!strcasecmp(dot, ".json"))
						kind = "json";
				}
			}
			slash = strrchr(local, '/');
			json_escape(slash ? slash + 1 : local, escname, sizeof(escname));
			need = snprintf(out, sizeof(out),
			    "{\"name\":\"%s\",\"kind\":\"%s\",\"size\":%lld,"
			    "\"mtime\":%lld,\"magic\":\"%s\"}",
			    escname, kind,
			    S_ISDIR(st.st_mode) ? 0 : (long long)st.st_size,
			    (long long)st.st_mtime, magic);
			if (need < 0 || (size_t)need >= sizeof(out))
				send_text(fd, "error:name too long");
			else
				send_json(fd, out);
		}
	} else if (!strcmp(method, "POST") &&
	           !strncmp(path, "/api/fs/rename", sizeof("/api/fs/rename") - 1)) {
		char rfrom[PATH_MAX_V], rto[PATH_MAX_V];
		char from[PATH_MAX_V], to[PATH_MAX_V];

		if (!json_string(body, "from", rfrom, sizeof(rfrom)) ||
		    !json_string(body, "to", rto, sizeof(rto)) ||
		    fs_jail(rfrom, from, sizeof(from)) != 0 ||
		    fs_jail(rto, to, sizeof(to)) != 0) {
			send_text(fd, "error:bad path");
		} else if (!strcmp(from, to)) {
			send_text(fd, "error:same path");
		} else if (!strcmp(to, FS_ROOT)) {
			send_text(fd, "error:bad path");
		} else if (rename(from, to) == 0) {
			send_json(fd, "{\"ok\":true}");
		} else {
			send_text(fd, "error:rename failed");
		}
	} else if (!strcmp(method, "POST") &&
	           !strncmp(path, "/api/fs/copy", sizeof("/api/fs/copy") - 1)) {
		char rsrc[PATH_MAX_V], rdst[PATH_MAX_V];
		char src[PATH_MAX_V], dst[PATH_MAX_V];

		if (!json_string(body, "src", rsrc, sizeof(rsrc)) ||
		    !json_string(body, "dst", rdst, sizeof(rdst)) ||
		    fs_src_jail(rsrc, src, sizeof(src)) != 0 ||
		    fs_jail(rdst, dst, sizeof(dst)) != 0) {
			send_text(fd, "error:bad path");
		} else if (!strcmp(dst, FS_ROOT)) {
			send_text(fd, "error:bad path");
		} else if (fs_copy_r(src, dst) == 0) {
			send_json(fd, "{\"ok\":true}");
		} else {
			send_text(fd, "error:copy failed");
		}
	} else if (!strcmp(method, "POST") &&
	           !strncmp(path, "/api/fs/move", sizeof("/api/fs/move") - 1)) {
		char rsrc[PATH_MAX_V], rdst[PATH_MAX_V];
		char src[PATH_MAX_V], dst[PATH_MAX_V];

		if (!json_string(body, "src", rsrc, sizeof(rsrc)) ||
		    !json_string(body, "dst", rdst, sizeof(rdst)) ||
		    fs_src_jail(rsrc, src, sizeof(src)) != 0 ||
		    fs_jail(rdst, dst, sizeof(dst)) != 0) {
			send_text(fd, "error:bad path");
		} else if (!strcmp(dst, FS_ROOT)) {
			send_text(fd, "error:bad path");
		} else if (rename(src, dst) == 0) {
			send_json(fd, "{\"ok\":true}");
		} else if (errno == EXDEV && fs_copy_r(src, dst) == 0 &&
		    fs_rm_r(src) == 0) {
			send_json(fd, "{\"ok\":true}");
		} else {
			send_text(fd, "error:move failed");
		}
#else
	/* Files tab temporarily disabled — kept for later work. */
	} else if (!strncmp(path, "/api/fs/", 8)) {
		send_text(fd, "error:file-explorer-disabled");
#endif
	} else if (!strcmp(method, "GET") &&
	           (!strcmp(path, "/logo.png") ||
	            !strcmp(path, "/favicon.ico"))) {
		send_png(fd, sender_logo, sender_logo_size);
	} else if (!strcmp(method, "GET")) {
		send_html(fd, UI_HTML);
	} else if (!strcmp(method, "POST") &&
	           !strncmp(path, "/api/install", 12)) {
		char gname[256];
		char gicon[512];

		if (json_first_package(body, url, sizeof(url))) {
			char url_fixed[URL_MAX];
			gname[0] = '\0';
			gicon[0] = '\0';
			json_string(body, "name", gname, sizeof(gname));
			json_string(body, "icon_url", gicon, sizeof(gicon));
			/* icon_url arrives percent-encoded like packages (issue #6);
			 * decode it symmetrically, then make any leftover raw
			 * whitespace safe for the console installer. */
			if (gicon[0]) {
				char icon_dec[512];
				url_decode(gicon, icon_dec, sizeof(icon_dec));
				url_encode_spaces(icon_dec, gicon, sizeof(gicon));
			}
			url_encode_spaces(url, url_fixed, sizeof(url_fixed));
			if (queue_install(url_fixed, gname[0] ? gname : NULL,
			                  gicon[0] ? gicon : NULL) == 0)
				send_json(fd, "{\"status\":\"success\"}");
			else
				send_json(fd,
				    "{\"status\":\"fail\","
				    "\"error\":\"queue failed\"}");
		} else {
			send_json(fd,
			    "{\"status\":\"fail\","
			    "\"error\":\"no package url\"}");
		}
	} else if (!strcmp(method, "POST") &&
	           !strncmp(path, "/upload", 7)) {
		if (grab_http_url(body, url, sizeof(url))) {
			char out[URL_MAX + 32];

			if (queue_install(url, NULL, NULL) == 0)
				snprintf(out, sizeof(out),
				    "SUCCESS: %s", url);
			else
				snprintf(out, sizeof(out),
				    "FAILED: queue failed");
			send_text(fd, out);
		} else {
			send_text(fd, "FAILED: no url field");
		}
	} else if (!strcmp(method, "POST") &&
	           !strncmp(path, "/api/files/mkdir", 16)) {
		char rpath[PATH_MAX_V], local[PATH_MAX_V];

		if (!json_string(body, "path", rpath, sizeof(rpath)) ||
		    jail_path(rpath, local, sizeof(local)) != 0) {
			send_text(fd, "error:bad path");
		} else if (mkdir_p(local) == 0) {
			send_json(fd, "{\"ok\":true}");
		} else {
			send_text(fd, "error:mkdir failed");
		}
	} else if (!strcmp(method, "POST") &&
	           !strncmp(path, "/api/files/write", 16)) {
		char rpath[URL_MAX], local[PATH_MAX_V];
		char offs[32];
		long long off;
		int wfd;
		char *wp;
		long left;

		if (!query_param(path, "path", rpath, sizeof(rpath)) ||
		    jail_path(rpath, local, sizeof(local)) != 0 ||
		    !query_param(path, "offset", offs, sizeof(offs))) {
			send_text(fd, "error:bad path/offset");
		} else {
			off = strtoll(offs, NULL, 10);
			if (off < 0) {
				send_text(fd, "error:bad offset");
				goto handled;
			}
			wfd = open(local, O_WRONLY | O_CREAT, 0644);
			if (wfd < 0) {
				send_text(fd, "error:open failed");
				goto handled;
			}
			if (lseek(wfd, (off_t)off, SEEK_SET) == (off_t)-1) {
				close(wfd);
				send_text(fd, "error:seek failed");
				goto handled;
			}
			wp = body;
			left = body_len;
			while (left > 0) {
				ssize_t n = write(wfd, wp, (size_t)left);
				if (n <= 0) {
					if (errno == EINTR)
						continue;
					break;
				}
				wp += n;
				left -= n;
			}
			close(wfd);
			if (left != 0)
				send_text(fd, "error:write failed");
			else
				send_json(fd, "{\"ok\":true}");
		}
	} else if (!strcmp(method, "POST") &&
	           !strncmp(path, "/api/files/done", 15)) {
		char rpath[PATH_MAX_V], local[PATH_MAX_V];
		long long want;
		struct stat st;

		if (!json_string(body, "path", rpath, sizeof(rpath)) ||
		    jail_path(rpath, local, sizeof(local)) != 0 ||
		    !json_long(body, "size", &want)) {
			send_text(fd, "error:bad path/size");
		} else if (stat(local, &st) != 0 ||
		           (!S_ISDIR(st.st_mode) && (long long)st.st_size != want)) {
			send_text(fd, "error:size mismatch");
		} else {
			char out[160], toast[160], base[128];
			const char *b = strrchr(local, '/');
			snprintf(base, sizeof(base), "%s", b ? b + 1 : local);
			snprintf(out, sizeof(out),
			    "{\"ok\":true,\"size\":%lld}", (long long)st.st_size);
			send_json(fd, out);
			snprintf(toast, sizeof(toast),
			    "Loopayeh: received %s", base);
			notify_user(toast);
		}
#ifdef ENABLE_FILES_TAB
	} else if (!strcmp(method, "POST") &&
	           !strncmp(path, "/api/fs/delete", 14)) {
		char rpath[PATH_MAX_V], local[PATH_MAX_V];
		struct stat st;

		if (!json_string(body, "path", rpath, sizeof(rpath)) ||
		    fs_jail(rpath, local, sizeof(local)) != 0) {
			send_text(fd, "error:bad path");
		} else if (!strcmp(local, FS_ROOT)) {
			send_text(fd, "error:refusing to delete root");
		} else if (stat(local, &st) != 0) {
			send_text(fd, "error:not found");
		} else if (S_ISDIR(st.st_mode)) {
			if (fs_rm_r(local) == 0) {
				send_json(fd, "{\"ok\":true}");
			} else {
				send_text(fd, "error:delete failed");
			}
		} else {
			if (unlink(local) == 0) {
				send_json(fd, "{\"ok\":true}");
			} else {
				send_text(fd, "error:delete failed");
			}
		}
#endif /* ENABLE_FILES_TAB */
	} else if (!strcmp(method, "POST") &&
	           !strncmp(path, "/api/files/pull", 15)) {
		char url[URL_MAX], rpath[PATH_MAX_V], local[PATH_MAX_V];
		char mode[16] = "";
		int rc;

		if (!json_string(body, "url", url, sizeof(url)) ||
		    !json_string(body, "path", rpath, sizeof(rpath)) ||
		    strncmp(url, "http://", 7) != 0 ||
		    jail_path(rpath, local, sizeof(local)) != 0) {
			send_text(fd, "error:bad url/path");
		} else {
			json_string(body, "mode", mode, sizeof(mode));
			rc = pull_start(url, local, !strcmp(mode, "resume"));
			if (rc == 0)
				send_json(fd, "{\"ok\":true,\"started\":true}");
			else if (rc == 1)
				send_text(fd, "error:copy already running");
			else if (rc == -1)
				send_text(fd, "error:out of memory");
			else
				send_text(fd, "error:worker failed");
		}

	} else {
		send_text(fd, "Loopayeh: unknown endpoint");
	}
handled:

	close(fd);
}
