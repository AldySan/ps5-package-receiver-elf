#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "common.h"
#include "http.h"
#include "files.h"
#include "notify.h"
#include "pull.h"
#include "handlers.h"

/* File transfer endpoints:
 *   GET  /api/files/stat?path=             file exists/size
 *   POST /api/files/mkdir   {"path":...}    mkdir -p under /data/homebrew
 *   POST /api/files/write?path=&offset=     raw chunk append
 *   POST /api/files/done    {"path":..,"size":N}  verify + toast
 *   POST /api/files/pull    {"url":..,"path":..,"mode":"resume"|"overwrite"}
 *   POST /api/pull/pause    {"paused":1|0}
 *   POST /api/pull/cancel
 * plus the file-explorer endpoints (/api/fs/...), currently disabled —
 * see ENABLE_FILES_TAB in common.h. */

void
h_files_stat(int fd, const char *path, char *body, long body_len)
{
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
}

void
h_files_mkdir(int fd, const char *path, char *body, long body_len)
{
	char rpath[PATH_MAX_V], local[PATH_MAX_V];

	if (!json_string(body, "path", rpath, sizeof(rpath)) ||
	    jail_path(rpath, local, sizeof(local)) != 0) {
		send_text(fd, "error:bad path");
	} else if (mkdir_p(local) == 0) {
		send_json(fd, "{\"ok\":true}");
	} else {
		send_text(fd, "error:mkdir failed");
	}
}

void
h_files_write(int fd, const char *path, char *body, long body_len)
{
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
		return;
	}
	off = strtoll(offs, NULL, 10);
	if (off < 0) {
		send_text(fd, "error:bad offset");
		return;
	}
	wfd = open(local, O_WRONLY | O_CREAT, 0644);
	if (wfd < 0) {
		send_text(fd, "error:open failed");
		return;
	}
	if (lseek(wfd, (off_t)off, SEEK_SET) == (off_t)-1) {
		close(wfd);
		send_text(fd, "error:seek failed");
		return;
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

void
h_files_done(int fd, const char *path, char *body, long body_len)
{
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
}

void
h_files_pull(int fd, const char *path, char *body, long body_len)
{
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
}

void
h_pull_pause(int fd, const char *path, char *body, long body_len)
{
	long long paused = 1;
	int p;

	json_long(body, "paused", &paused);
	p = paused ? 1 : 0;
	pull_set_paused(p);
	send_json(fd, p ? "{\"ok\":true,\"paused\":true}"
	    : "{\"ok\":true,\"paused\":false}");
}

void
h_pull_cancel(int fd, const char *path, char *body, long body_len)
{
	/* stop the running pull; the partial file stays for resume */
	pull_cancel();
	pull_set_paused(0);
	send_json(fd, "{\"ok\":true,\"cancelled\":true}");
}

#ifdef ENABLE_FILES_TAB

void
h_fs_list(int fd, const char *path, char *body, long body_len)
{
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
		return;
	}
	if (!(dp = opendir(local))) {
		send_text(fd, "error:not a directory");
		return;
	}
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
			    sizeof(ents[n].name), "%s", de->d_name);
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
		if (n == FS_MAX_ENTRIES && readdir(dp) != NULL)
			truncated = 1;
		qsort(ents, n, sizeof(fs_entry_t), fs_entry_cmp);
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
		    "{\"path\":\"%s\",\"truncated\":", escpath);
		toff = joff; /* single flag digit patched below */
		joff += (size_t)snprintf(json + joff,
		    jlen - joff, "0,\"entries\":[");
		for (i = 0; i < n; i++) {
			char nm[512], row[800];
			int need;

			json_escape(ents[i].name, nm, sizeof(nm));
			need = snprintf(row, sizeof(row),
			    "%s{\"name\":\"%s\",\"dir\":%s,\"size\":%lld}",
			    i ? "," : "", nm,
			    ents[i].is_dir ? "true" : "false",
			    ents[i].size);
			/* snprintf reports the untruncated length; memcpy must
			 * never copy more than row actually holds */
			if (need < 0 || (size_t)need >= sizeof(row))
				need = (int)sizeof(row) - 1;
			if (joff + (size_t)need + 32 >= jlen) {
				truncated = 1;
				break;
			}
			memcpy(json + joff, row, (size_t)need);
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

void
h_fs_info(int fd, const char *path, char *body, long body_len)
{
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
		return;
	}
	if (stat(local, &st) != 0) {
		send_text(fd, "error:not found");
		return;
	}
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

void
h_fs_rename(int fd, const char *path, char *body, long body_len)
{
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
}

void
h_fs_copy(int fd, const char *path, char *body, long body_len)
{
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
}

void
h_fs_move(int fd, const char *path, char *body, long body_len)
{
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
}

void
h_fs_delete(int fd, const char *path, char *body, long body_len)
{
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
		if (fs_rm_r(local) == 0)
			send_json(fd, "{\"ok\":true}");
		else
			send_text(fd, "error:delete failed");
	} else {
		if (unlink(local) == 0)
			send_json(fd, "{\"ok\":true}");
		else
			send_text(fd, "error:delete failed");
	}
}

#else /* !ENABLE_FILES_TAB */

void
h_fs_disabled(int fd, const char *path, char *body, long body_len)
{
	/* Files tab temporarily disabled — kept for later work. */
	send_text(fd, "error:file-explorer-disabled");
}

#endif /* ENABLE_FILES_TAB */
