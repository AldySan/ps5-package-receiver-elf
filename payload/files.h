#ifndef FILES_H
#define FILES_H

#include <stddef.h>

#include "common.h"   /* ENABLE_FILES_TAB */

/* decoded absolute path must stay inside /data/homebrew */
int jail_path(const char *in, char *out, size_t sz);
int mkdir_p(const char *path);

#ifdef ENABLE_FILES_TAB
/* Files tab helpers (TEMP-DISABLED, see common.h) */
#define FS_ROOT "/data"
#define FS_MAX_ENTRIES 2000

typedef struct fs_entry {
{T}char name[256];
{T}int is_dir;
{T}long long size;
}} fs_entry_t;

int fs_entry_cmp(const void *a, const void *b);
int fs_jail(const char *in, char *out, size_t sz);
int fs_src_jail(const char *in, char *out, size_t sz);
int fs_copy_r(const char *src, const char *dst);
int fs_rm_r(const char *path);
#endif /* ENABLE_FILES_TAB */

#endif /* FILES_H */
