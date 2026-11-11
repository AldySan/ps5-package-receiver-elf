#ifndef INSTALLER_H
#define INSTALLER_H

/* PKG installs via SCE AppInstUtil, resolved with dlopen at runtime. */

/* queue an install on a detached worker (url is http://... or /data/...);
 * 0 = queued, -1 = could not spawn */
int queue_install(const char *url, const char *name, const char *icon);

/* delete leftover staged packages (a payload killed mid-staging could
 * not clean up after itself). stage_sweep_boot() is the boot-time pass
 * and toasts what it freed; stage_clear() is the on-demand pass behind
 * POST /api/stage/clear — it returns the bytes freed (files in
 * *files_out), or -1 while a staged install is running. */
void stage_sweep_boot(void);
long long stage_clear(int *files_out);

/* outcome of the last finished install, for the status endpoint: the
 * page matches lastUrl against the install it queued and reads the
 * real result from ok/err (busy=false alone cannot tell success from
 * failure). ok is 1 = succeeded. The record is mirrored to
 * /data/homebrew and reloaded at boot by install_last_restore(), so a
 * receiver restart between two status polls does not erase it. */
void install_last_result(int *ok, char *url, size_t url_sz,
    char *name, size_t name_sz, char *err, size_t err_sz);
void install_last_restore(void);

/* live progress of the running install, for /api/status: 0 = none,
 * 1 = the console fetches the package itself, 2 = staging to /data,
 * 3 = the console is copying the staged package. got/want are the
 * bytes of whichever leg is moving (0/-1 when the leg is unknown). */
void install_job_progress(int *phase, long long *got, long long *want,
    char *name, size_t name_sz, char *url, size_t url_sz);

#ifndef TEST_ONLY
/* stage + register the home-screen launcher (best effort) */
void launcher_install_if_needed(void);
#endif

#endif /* INSTALLER_H */
