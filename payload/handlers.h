#ifndef HANDLERS_H
#define HANDLERS_H

#include "common.h"   /* ENABLE_FILES_TAB */

/* One endpoint = one function. fd is the connection to reply on, path
 * is the request path including the query string, body is the fully
 * read request body ("" when none) and body_len its length. Replying
 * is the handler's whole job — handle_client() closes fd afterwards. */
typedef void (*handler_fn)(int fd, const char *path, char *body,
                           long body_len);

/* last raw request line, recorded by handle_client(), served by /api/dbg */
extern char g_last_req[256];

/* handlers_core.c — WebUI page, logo and read-only JSON probes */
void h_index(int fd, const char *path, char *body, long body_len);
void h_logo(int fd, const char *path, char *body, long body_len);
void h_api_probe(int fd, const char *path, char *body, long body_len);
void h_version(int fd, const char *path, char *body, long body_len);
void h_dbg(int fd, const char *path, char *body, long body_len);
void h_status(int fd, const char *path, char *body, long body_len);
void h_pc(int fd, const char *path, char *body, long body_len);
void h_space(int fd, const char *path, char *body, long body_len);

/* handlers_install.c — everything that queues a PKG install */
void h_install_get(int fd, const char *path, char *body, long body_len);
void h_install_post(int fd, const char *path, char *body, long body_len);
void h_upload(int fd, const char *path, char *body, long body_len);

/* handlers_files.c — transfer endpoints under /api/files and /api/pull */
void h_files_stat(int fd, const char *path, char *body, long body_len);
void h_files_mkdir(int fd, const char *path, char *body, long body_len);
void h_files_write(int fd, const char *path, char *body, long body_len);
void h_files_done(int fd, const char *path, char *body, long body_len);
void h_files_pull(int fd, const char *path, char *body, long body_len);
void h_pull_pause(int fd, const char *path, char *body, long body_len);
void h_pull_cancel(int fd, const char *path, char *body, long body_len);

#ifdef ENABLE_FILES_TAB
/* handlers_files.c — file explorer tab (TEMP-DISABLED, see common.h) */
void h_fs_list(int fd, const char *path, char *body, long body_len);
void h_fs_info(int fd, const char *path, char *body, long body_len);
void h_fs_rename(int fd, const char *path, char *body, long body_len);
void h_fs_copy(int fd, const char *path, char *body, long body_len);
void h_fs_move(int fd, const char *path, char *body, long body_len);
void h_fs_delete(int fd, const char *path, char *body, long body_len);
#else
void h_fs_disabled(int fd, const char *path, char *body, long body_len);
#endif /* ENABLE_FILES_TAB */

#endif /* HANDLERS_H */
