#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

#include "files.h"

#define JAIL_PREFIX "/data/homebrew"

/* ── /data/homebrew file receiver ──────────────────────────────────── */
#define JAIL_PREFIX "/data/homebrew"

/* decoded absolute path must stay inside /data/homebrew */
int
jail_path(const char *in, char *out, size_t sz)
{
	size_t pre = strlen(JAIL_PREFIX);

	if (!in || strlen(in) + 1 > sz)
		return -1;
	if (strcmp(in, JAIL_PREFIX) != 0 && strncmp(in, JAIL_PREFIX "/", pre + 1) != 0)
		return -1;
	if (strstr(in, ".."))
		return -1;
	strcpy(out, in);
	return 0;
}

int
mkdir_p(const char *path)
{
	char tmp[PATH_MAX_V];
	size_t i, n = strlen(path);

	if (n == 0 || n >= sizeof(tmp))
		return -1;
	strcpy(tmp, path);
	for (i = 1; i < n; i++) {
		if (tmp[i] == '/') {
			tmp[i] = '\0';
			if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
				return -1;
			tmp[i] = '/';
		}
	}
	if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
		return -1;
	return 0;
}
/* ── /data file browser (Files tab) ────────────────────────────────────
 * TEMP-DISABLED: see ENABLE_FILES_TAB above. Kept for later work. */
#ifdef ENABLE_FILES_TAB

int
fs_jail(const char *in, char *out, size_t sz)
{
	size_t pre = strlen(FS_ROOT);

	if (!in || strlen(in) + 1 > sz)
		return -1;
	if (strcmp(in, FS_ROOT) != 0 && strncmp(in, FS_ROOT "/", pre + 1) != 0)
		return -1;
	if (strstr(in, ".."))
		return -1;
	strcpy(out, in);
	return 0;
}

/* ── USB sources: /mnt/usb0..7 are browsable and copyable-from,
 * never written (read-only sticks stay safe). */
static int
fs_usb_src(const char *in, char *out, size_t sz)
{
	if (!in || strlen(in) + 1 > sz)
		return -1;
	if (strncmp(in, "/mnt/usb", 8) != 0)
		return -1;
	if (in[8] < '0' || in[8] > '7')
		return -1;
	if (in[9] != '\0' && in[9] != '/')
		return -1;
	if (strstr(in, ".."))
		return -1;
	strcpy(out, in);
	return 0;
}

/* readable source: /data anywhere, or a USB stick */
int
fs_src_jail(const char *in, char *out, size_t sz)
{
	if (fs_jail(in, out, sz) == 0)
		return 0;
	return fs_usb_src(in, out, sz);
}

/* join dir + name safely; 0 ok, -1 truncates */
static int
fs_join(char *dst, size_t sz, const char *dir, const char *name)
{
	int n = snprintf(dst, sz, "%s/%s", dir, name);

	return (n > 0 && (size_t)n < sz) ? 0 : -1;
}

/* recursive delete (files + dirs). best-effort: 0 ok, -1 on first error */
int
fs_rm_r(const char *path)
{
	struct stat st;
	DIR *d;
	struct dirent *e;

	if (lstat(path, &st) != 0)
		return -1;
	if (!S_ISDIR(st.st_mode))
		return unlink(path);
	d = opendir(path);
	if (!d)
		return -1;
	while ((e = readdir(d)) != NULL) {
		char full[PATH_MAX_V];

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (fs_join(full, sizeof(full), path, e->d_name) != 0) {
			closedir(d);
			return -1;
		}
		if (fs_rm_r(full) != 0) {
			closedir(d);
			return -1;
		}
	}
	closedir(d);
	return rmdir(path);
}

/* copy one regular file, overwriting dst */
static int
fs_copy_file(const char *src, const char *dst)
{
	char *hb = malloc(256 * 1024);
	int in = -1, out = -1, rc = -1;
	ssize_t n;

	if (!hb)
		return -1;
	in = open(src, O_RDONLY);
	if (in < 0)
		goto out;
	out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out < 0)
		goto out;
	rc = 0;
	for (;;) {
		n = read(in, hb, 256 * 1024);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			rc = -1;
			break;
		}
		if (n == 0)
			break;
		if (write(out, hb, (size_t)n) != n) {
			rc = -1;
			break;
		}
	}
out:
	free(hb);
	if (in >= 0)
		close(in);
	if (out >= 0)
		close(out);
	return rc;
}

/* recursive copy src -> dst (dst may exist when merging dirs) */
int
fs_copy_r(const char *src, const char *dst)
{
	struct stat st;
	DIR *d;
	struct dirent *e;

	if (stat(src, &st) != 0)
		return -1;
	if (!S_ISDIR(st.st_mode))
		return fs_copy_file(src, dst);
	if (mkdir(dst, 0755) != 0 && errno != EEXIST)
		return -1;
	d = opendir(src);
	if (!d)
		return -1;
	while ((e = readdir(d)) != NULL) {
		char fsrc[PATH_MAX_V], fdst[PATH_MAX_V];

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (fs_join(fsrc, sizeof(fsrc), src, e->d_name) != 0 ||
		    fs_join(fdst, sizeof(fdst), dst, e->d_name) != 0) {
			closedir(d);
			return -1;
		}
		if (fs_copy_r(fsrc, fdst) != 0) {
			closedir(d);
			return -1;
		}
	}
	closedir(d);
	return 0;
}

int
fs_entry_cmp(const void *a, const void *b)
{
	const fs_entry_t *x = a, *y = b;

	if (x->is_dir != y->is_dir)
		return y->is_dir - x->is_dir; /* dirs first */
	return strcmp(x->name, y->name);
}
#endif /* ENABLE_FILES_TAB */
