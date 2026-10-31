#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/stat.h>

#include "common.h"
#include "notify.h"
#include "http.h"
#include "installer.h"

/* ── SCE AppInstUtil ABI (same layout websrv/ftpsrv use) ─────────────── */
typedef struct pkg_metadata {
        const char *uri;
        const char *ex_uri;
        const char *playgo_scenario_id;
        const char *content_id;
        const char *content_name;
        const char *icon_url;
        uint32_t slot;
        uint32_t is_playgo_enabled;
} pkg_metadata_t;

typedef struct pkg_info {
        char content_id[48];
        int type;
        int platform;
} pkg_info_t;

typedef struct playgo_info {
        char languages[30][8];
        char scenario_ids[64][3];
        char content_ids[64][48];
        long unknown[810];
} playgo_info_t;

/* AppInstUtil is resolved at runtime (dlopen), NOT linked: linking it
 * kills the payload at load time on some setups (proven on 6.02). */
typedef int (*init_fn)(void);
typedef int (*install_fn)(const pkg_metadata_t *, pkg_info_t *,
                           playgo_info_t *);

static pthread_mutex_t g_inst_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_inst_ready = 0;
static void *g_ipmilib;
static void *g_applib;
static init_fn p_init;
static install_fn p_install;

/* 0 = ok, -1 = sprx not found, -2 = symbol not found, else SCE code */
static int
installer_init(void)
{
        int rc;

        pthread_mutex_lock(&g_inst_lock);
        if (g_inst_ready) {
                pthread_mutex_unlock(&g_inst_lock);
                return 0;
        }
        if (!g_applib) {
                /* AppInstUtil dies on load unless Ipmi is loaded first
                 * (proven on 6.02 with t_dl3). */
                if (!g_ipmilib) {
                        g_ipmilib = dlopen("libSceIpmi.sprx", RTLD_LAZY);
                        if (!g_ipmilib)
                                g_ipmilib = dlopen(
                                    "/system/common/lib/libSceIpmi.sprx",
                                    RTLD_LAZY);
                }
                g_applib = dlopen("libSceAppInstUtil.sprx", RTLD_LAZY);
                if (!g_applib)
                        g_applib = dlopen(
                            "/system/common/lib/libSceAppInstUtil.sprx",
                            RTLD_LAZY);
                if (!g_applib) {
                        pthread_mutex_unlock(&g_inst_lock);
                        return -1;
                }
        }
        if (!p_init) {
                p_init = (init_fn)dlsym(g_applib,
                    "sceAppInstUtilInitialize");
                if (!p_init) {
                        pthread_mutex_unlock(&g_inst_lock);
                        return -2;
                }
        }
        if (!p_install) {
                p_install = (install_fn)dlsym(g_applib,
                    "sceAppInstUtilInstallByPackage");
                if (!p_install) {
                        pthread_mutex_unlock(&g_inst_lock);
                        return -2;
                }
        }
        rc = p_init();
        if (rc == 0)
                g_inst_ready = 1;
        else
                notify_user("Loopayeh: AppInstUtil init failed");
        pthread_mutex_unlock(&g_inst_lock);
        return rc;
}
/* ── Home-screen launcher ("pkg remote installer", Media category) ────
 * Same idea as owendswang's ps5-web-file-manager (reimplemented here):
 * write /user/app/PKGS12800/sce_sys/{param.json,icon0.png} once, then
 * register the web shortcut via AppInstUtil. Best-effort: if the symbol
 * is missing the receiver keeps serving without a launcher.
 * Skipped entirely in TEST_ONLY builds. */
#ifndef TEST_ONLY
#define LAUNCHER_TID "PKGS12800"

__asm__(
".section .rodata\n"
".global launcher_param\n"
".global launcher_param_end\n"
".global launcher_param_size\n"
".align 16\n"
"launcher_param:\n"
".incbin \"launcher_param.json\"\n"
"launcher_param_end:\n"
"launcher_param_size:\n"
".quad launcher_param_end - launcher_param\n"
".previous\n");
extern const unsigned char launcher_param[];
extern const size_t launcher_param_size;

__asm__(
".section .rodata\n"
".global launcher_icon\n"
".global launcher_icon_end\n"
".global launcher_icon_size\n"
".align 16\n"
"launcher_icon:\n"
".incbin \"icon0.png\"\n"
"launcher_icon_end:\n"
"launcher_icon_size:\n"
".quad launcher_icon_end - launcher_icon\n"
".previous\n");
extern const unsigned char launcher_icon[];
extern const size_t launcher_icon_size;

typedef int (*titledir_fn)(const char *, const char *, void *);

static int
write_file_once(const char *path, const unsigned char *data, size_t size)
{
        struct stat st;
        FILE *f;

        if (stat(path, &st) == 0)
                return 0;
        if (errno != ENOENT)
                return -1;
        f = fopen(path, "wb");
        if (!f)
                return -1;
        if (fwrite(data, size, 1, f) != 1) {
                fclose(f);
                return -1;
        }
        fclose(f);
        return 0;
}

void
launcher_install_if_needed(void)
{
        char dir[128], sdir[160], pj[192], ip[192];
        char toast[96];
        struct stat st;
        titledir_fn p_titledir;
        int rc;

        snprintf(dir, sizeof(dir), "/user/app/%s", LAUNCHER_TID);
        if (stat(dir, &st) == 0) {
                snprintf(pj, sizeof(pj), "%s/sce_sys/param.json", dir);
                snprintf(ip, sizeof(ip), "%s/sce_sys/icon0.png", dir);
                if (stat(pj, &st) == 0 && stat(ip, &st) == 0)
                        return; /* already installed */
        }
        if (installer_init() != 0) {
                return; /* AppInstUtil off: installs fail with their own toast */
        }
        snprintf(sdir, sizeof(sdir), "%s/sce_sys", dir);
        if ((mkdir(dir, 0755) != 0 && errno != EEXIST) ||
            (mkdir(sdir, 0755) != 0 && errno != EEXIST)) {
                notify_user("Loopayeh: launcher mkdir failed");
                return;
        }
        snprintf(pj, sizeof(pj), "%s/param.json", sdir);
        snprintf(ip, sizeof(ip), "%s/icon0.png", sdir);
        if (write_file_once(pj, launcher_param, launcher_param_size) != 0 ||
            write_file_once(ip, launcher_icon, launcher_icon_size) != 0) {
                notify_user("Loopayeh: launcher file write failed");
                return;
        }
        p_titledir = (titledir_fn)dlsym(g_applib,
            "sceAppInstUtilAppInstallTitleDir");
        if (!p_titledir) {
                notify_user("Loopayeh: launcher staged, registration N/A");
                return;
        }
        rc = p_titledir(LAUNCHER_TID, "/user/app/", NULL);
        if (rc == 0)
                notify_user("Loopayeh: home launcher installed");
        else {
                snprintf(toast, sizeof(toast),
                    "Loopayeh: launcher register 0x%08X", (unsigned)rc);
                notify_user(toast);
        }
}
#endif /* TEST_ONLY */
/* install runs on a detached worker so a slow/hanging SCE call can
 * never wedge the single-threaded HTTP loop. */
typedef struct install_job {
        char url[URL_MAX];
        char name[256];
        char icon[512];
} install_job_t;

/* human text for install errors (-1/-2 are ours, rest are SCE codes) */
static const char *
install_err_text(int rc, char *buf, size_t sz)
{
        if (rc == -1)
                snprintf(buf, sz, "AppInstUtil sprx not found");
        else if (rc == -2)
                snprintf(buf, sz, "AppInstUtil symbols not found");
        else
                snprintf(buf, sz, "0x%08X", (unsigned)rc);
        return buf;
}

/* returns 0 on success, SCE error code otherwise */
static int
installer_install(const char *path, const char *want_name,
                  const char *want_icon,
                  char *name_out, size_t name_sz)
{
        char local[URL_MAX + 32];
        const char *uri = path;
        const char *base;
        pkg_metadata_t meta;
        pkg_info_t info;
        playgo_info_t playgo;
        int rc;

        if (!path || !*path)
                return -1;

        if (!strncmp(path, "/data/", 6)) {
                snprintf(local, sizeof(local), "/user%s", path);
                uri = local;
        }

        base = strrchr(uri, '/');
        base = base ? base + 1 : uri;
        if (want_name && *want_name) {
                /* Sender told us the real game title: use it for the toast
                 * and for content_name shown by the console during install. */
                snprintf(name_out, name_sz, "%s", want_name);
        } else {
        /* strip query/fragment: "/proxy/?b64=.." -> basename would be garbage */
        {
                char tmp[256];
                size_t n = 0;

                while (base[n] && base[n] != '?' && base[n] != '#' &&
                       base[n] != '&' && n + 1 < sizeof(tmp)) {
                        tmp[n] = base[n];
                        n++;
                }
                tmp[n] = '\0';
                /* percent-decode the name too (tool url-encodes it) */
                url_decode(tmp, name_out, name_sz);
                if (!name_out[0])
                        snprintf(name_out, name_sz, "%s", "PKG Sender Package");
        }
        }

        memset(&meta, 0, sizeof(meta));
        memset(&info, 0, sizeof(info));
        memset(&playgo, 0, sizeof(playgo));
        meta.uri = uri;
        meta.ex_uri = "";
        meta.playgo_scenario_id = "";
        meta.content_id = "";
        meta.content_name = name_out;
        /* Sender passes the cover URL it serves (/icon/..): the console
         * fetches it itself for the download list. Empty = no cover. */
        meta.icon_url = (want_icon && *want_icon) ? want_icon : "";
        meta.slot = 0;
        meta.is_playgo_enabled = 0;

        rc = installer_init();
        if (rc)
                return rc;

        pthread_mutex_lock(&g_inst_lock);
        rc = p_install(&meta, &info, &playgo);
        pthread_mutex_unlock(&g_inst_lock);
        return rc;
}

static void *
install_worker(void *arg)
{
        install_job_t *job = arg;
        char name[256];
        char toast[256];
        char err[64];
        int rc;

        __sync_fetch_and_add(&g_active_installs, 1);
        rc = installer_install(job->url,
            job->name[0] ? job->name : NULL,
            job->icon[0] ? job->icon : NULL, name, sizeof(name));

        if (rc == 0)
                snprintf(toast, sizeof(toast), "Loopayeh: installing %s", name);
        else
                snprintf(toast, sizeof(toast), "Loopayeh: install failed %s",
                    install_err_text(rc, err, sizeof(err)));
        notify_user(toast);
        __sync_fetch_and_sub(&g_active_installs, 1);
        free(job);
        return NULL;
}

int
queue_install(const char *url, const char *name, const char *icon)
{
#ifdef TEST_ONLY
        (void)url;
        (void)name;
        (void)icon;
        (void)install_worker; /* keep referenced so -Wunused-function stays quiet */
        /* defense in depth: the early refusal above should already have
         * caught every install route. */
        return -1;
#else
        pthread_t tid;
        install_job_t *job;

        job = malloc(sizeof(*job));

        if (!job)
                return -1;
        snprintf(job->url, sizeof(job->url), "%s", url ? url : "");
        if (name)
                snprintf(job->name, sizeof(job->name), "%s", name);
        else
                job->name[0] = '\0';
        if (icon)
                snprintf(job->icon, sizeof(job->icon), "%s", icon);
        else
                job->icon[0] = '\0';
        if (pthread_create(&tid, NULL, install_worker, job) != 0) {
                free(job);
                return -1;
        }
        pthread_detach(tid);
        return 0;
#endif
}
