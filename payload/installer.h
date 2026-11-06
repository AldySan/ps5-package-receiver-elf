#ifndef INSTALLER_H
#define INSTALLER_H

/* PKG installs via SCE AppInstUtil, resolved with dlopen at runtime. */

/* queue an install on a detached worker (url is http://... or /data/...);
 * 0 = queued, -1 = could not spawn */
int queue_install(const char *url, const char *name, const char *icon);

/* outcome of the last finished install, for the status endpoint: the
 * page matches lastUrl against the install it queued and reads the
 * real result from ok/err (busy=false alone cannot tell success from
 * failure). ok is 1 = succeeded. */
void install_last_result(int *ok, char *url, size_t url_sz,
    char *name, size_t name_sz, char *err, size_t err_sz);

#ifndef TEST_ONLY
/* stage + register the home-screen launcher (best effort) */
void launcher_install_if_needed(void);
#endif

#endif /* INSTALLER_H */
