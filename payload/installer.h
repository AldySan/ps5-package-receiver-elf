#ifndef INSTALLER_H
#define INSTALLER_H

/* PKG installs via SCE AppInstUtil, resolved with dlopen at runtime. */

/* queue an install on a detached worker (url is http://... or /data/...);
 * 0 = queued, -1 = could not spawn */
int queue_install(const char *url, const char *name, const char *icon);

#ifndef TEST_ONLY
/* stage + register the home-screen launcher (best effort) */
void launcher_install_if_needed(void);
#endif

#endif /* INSTALLER_H */
