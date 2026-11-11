#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <pthread.h>
#include <dlfcn.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <strings.h>
#include <poll.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/statvfs.h>

#include "common.h"
#include "notify.h"
#include "http.h"
#include "files.h"
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
typedef int (*terminate_fn)(void);

static pthread_mutex_t g_inst_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_inst_ready = 0;
static void *g_ipmilib;
static void *g_applib;
static int g_session_used = 0; /* an InstallByPackage already ran */
static init_fn p_init;
static install_fn p_install;
static terminate_fn p_terminate;

/* 0 = ok, -1 = sprx not found, -2 = symbol not found, else SCE code.
 * g_inst_lock must be held. */
static int
installer_init_locked(void)
{
        int rc;

        if (g_inst_ready)
                return 0;
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
                if (!g_applib)
                        return -1;
        }
        if (!p_init) {
                p_init = (init_fn)dlsym(g_applib,
                    "sceAppInstUtilInitialize");
                if (!p_init)
                        return -2;
        }
        if (!p_install) {
                p_install = (install_fn)dlsym(g_applib,
                    "sceAppInstUtilInstallByPackage");
                if (!p_install)
                        return -2;
        }
        if (!p_terminate) {
                /* optional: older sprx may not export it */
                p_terminate = (terminate_fn)dlsym(g_applib,
                    "sceAppInstUtilTerminate");
        }
        rc = p_init();
        if (rc == 0)
                g_inst_ready = 1;
        else
                notify_user(APP_TAG ": AppInstUtil init failed");
        return rc;
}

static int
installer_init(void)
{
        int rc;

        pthread_mutex_lock(&g_inst_lock);
        rc = installer_init_locked();
        pthread_mutex_unlock(&g_inst_lock);
        return rc;
}

/* Sony's installer allows one InstallByPackage per initialized session:
 * a second call in the same session misbehaves from FW 9.60 on, and a
 * refused call spends the session too (the pkgmgr and ps5upload
 * projects run every install in a fresh helper process for exactly
 * this reason). A payload cannot fork, so before every call past the
 * first the session is rebuilt in place: Terminate, drop the library,
 * load it fresh (Ipmi stays loaded), Initialize again. */
static int
installer_session_locked(void)
{
        if (!g_session_used)
                return installer_init_locked();
        if (g_inst_ready && p_terminate)
                p_terminate();
        g_inst_ready = 0;
        if (g_applib) {
                dlclose(g_applib);
                g_applib = NULL;
        }
        p_init = NULL;
        p_install = NULL;
        p_terminate = NULL;
        g_session_used = 0;
        return installer_init_locked();
}
/* ── Home-screen launcher ("PS5 PKG Receiver", Media category) ───────
 * Same idea as owendswang's ps5-web-file-manager (reimplemented here):
 * write /user/app/PKGR12800/sce_sys/{param.json,icon0.png} once, then
 * register the web shortcut via AppInstUtil. Best-effort: if the symbol
 * is missing the receiver keeps serving without a launcher.
 * Skipped entirely in TEST_ONLY builds. */
#ifndef TEST_ONLY
#define LAUNCHER_TID "PKGR12800"

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
                notify_user(APP_TAG ": launcher mkdir failed");
                return;
        }
        snprintf(pj, sizeof(pj), "%s/param.json", sdir);
        snprintf(ip, sizeof(ip), "%s/icon0.png", sdir);
        if (write_file_once(pj, launcher_param, launcher_param_size) != 0 ||
            write_file_once(ip, launcher_icon, launcher_icon_size) != 0) {
                notify_user(APP_TAG ": launcher file write failed");
                return;
        }
        p_titledir = (titledir_fn)dlsym(g_applib,
            "sceAppInstUtilAppInstallTitleDir");
        if (!p_titledir) {
                notify_user(APP_TAG ": launcher staged, registration N/A");
                return;
        }
        rc = p_titledir(LAUNCHER_TID, "/user/app/", NULL);
        if (rc == 0)
                notify_user(APP_TAG ": home launcher installed");
        else {
                snprintf(toast, sizeof(toast),
                    APP_TAG ": launcher register 0x%08X", (unsigned)rc);
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
        int slot; /* guard slot below, -1 = unguarded */
} install_job_t;

/* the URLs currently claimed by running install workers, so a second
 * click on the same tile stacks a toast, not a second multi-gigabyte
 * download. A table, not one slot: a second, different install must
 * not knock the first one's claim off and let a duplicate through. */
#define CUR_SLOTS 8
static pthread_mutex_t g_cur_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_cur_urls[CUR_SLOTS][URL_MAX];
static int g_cur_used[CUR_SLOTS];

/* live progress of the install job currently running, published on
 * /api/status: a staged install moves the package twice (our staging
 * pull from the PC, then the console's own copy from the loopback
 * server) and the sender can only count the first hop by itself.
 * One slot for the whole payload: installs are serialized per URL, and
 * a second install stacking onto the first simply replaces the slot
 * until the jobs clear it — readers match by url and ignore a job that
 * is not theirs. */
#define JOBP_CONSOLE 1 /* the console fetches the package itself */
#define JOBP_STAGE   2 /* we are staging the package to /data */
#define JOBP_COPY    3 /* the console copies the staged package */
static pthread_mutex_t g_job_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_job_phase = 0;
static long long g_job_got = 0;
static long long g_job_want = 0;
static char g_job_name[128];
static char g_job_url[512];

static void
job_set(int phase, long long got, long long want)
{
        pthread_mutex_lock(&g_job_lock);
        g_job_phase = phase;
        g_job_got = got;
        g_job_want = want;
        pthread_mutex_unlock(&g_job_lock);
}

static void
job_begin(const char *url, const char *name)
{
        pthread_mutex_lock(&g_job_lock);
        g_job_phase = JOBP_CONSOLE;
        g_job_got = 0;
        g_job_want = 0;
        snprintf(g_job_url, sizeof(g_job_url), "%s", url ? url : "");
        snprintf(g_job_name, sizeof(g_job_name), "%s", name ? name : "");
        pthread_mutex_unlock(&g_job_lock);
}

/* staged local install: knobs and our own error codes (below -2) */
#define STAGE_DIR     "/data/homebrew/.pkg-stage"
#define STAGE_CHUNK   (1024 * 1024)
#define LB_MAX_CONNS  16
#define LB_REQ_MAX    8192
#define LB_IO_S       60
/* the console may queue the staged pull behind another download for a
 * while before its first fetch: declaring the install dead while it is
 * merely waiting unlinks the staged package under a live intent, so the
 * first byte is given three minutes to arrive */
#define LB_FIRST_S   180
#define LB_IDLE_S     300
#define LB_GRACE_S    30
#define LB_PULL_CAP_S 7200
#define STAGE_NET    (-10)
#define STAGE_DISK   (-11)
#define STAGE_SHORT  (-12)
#define STAGE_SPACE  (-13)
#define STAGE_SERVER (-14)
#define STAGE_PULL   (-15)
#define STAGE_GONE   (-16) /* the PC answered "no such package" */
#define STAGE_FRAME  (-17) /* the body arrived chunk-framed */
#define STAGE_NOTPKG (-18) /* the staged copy is not a package */

/* staging resilience knobs: a dropped connection is resumed (the PC
 * speaks Range), a stalled one is retried, and progress is toasted */
#define STAGE_TRIES     4       /* first attempt plus three resumes */
#define STAGE_RETRY_S   2       /* pause between attempts */
#define STAGE_STALL_S   30      /* one recv may stall this long */
#define STAGE_MIN_STEP  (256LL * 1024 * 1024)

/* headroom on top of the two copies a staged install keeps on /data at
 * the same time: our staged package plus the console's own download of
 * it (see the space gate in stage_fetch) */
#define STAGE_HEADROOM (1024LL * 1024 * 1024)

/* one progress toast per slice of the package: an eighth of it, but at
 * least 256 MiB so small packages still report */
static long long
stage_report_step(long long want)
{
        if (want <= 0)
                return 1LL << 30;
        if (want / 8 >= STAGE_MIN_STEP)
                return want / 8;
        return STAGE_MIN_STEP;
}

/* "staging <name> 3.7/10.4 GB" — integer math, no float formatting */
static void
stage_progress(const char *what, const char *name, long long got,
    long long want)
{
        char toast[352];
        long long gt = (got * 10) / (1LL << 30);
        long long wt = want > 0 ? (want * 10) / (1LL << 30) : -1;

        if (wt >= 0)
                snprintf(toast, sizeof(toast),
                    APP_TAG ": %s %s %lld.%lld/%lld.%lld GB", what, name,
                    gt / 10, gt % 10, wt / 10, wt % 10);
        else
                snprintf(toast, sizeof(toast),
                    APP_TAG ": %s %s %lld.%lld GB", what, name,
                    gt / 10, gt % 10);
        notify_user(toast);
}

/* human text for install errors. Negative codes are ours (dlopen
 * failures and the staged-fallback codes below); the rest are SCE codes,
 * the 0x80A3xxxx ones translated with the community's established
 * meanings (same list the ps5upload project measured on hardware). */
static const char *
install_err_text(int rc, char *buf, size_t sz)
{
        uint32_t c = (uint32_t)rc;

        if (rc == -1)
                snprintf(buf, sz, "AppInstUtil sprx not found");
        else if (rc == -2)
                snprintf(buf, sz, "AppInstUtil symbols not found");
        else if (rc == STAGE_NET)
                snprintf(buf, sz, "could not download the package from the "
                    "PC (check its firewall, VPN or proxy)");
        else if (rc == STAGE_GONE)
                snprintf(buf, sz, "the PC stopped serving this package — "
                    "refresh its library and send again");
        else if (rc == STAGE_DISK)
                snprintf(buf, sz, "could not write the staged package");
        else if (rc == STAGE_SHORT)
                snprintf(buf, sz, "staged package is incomplete");
        else if (rc == STAGE_SPACE) {
                struct statvfs sv;
                long long freeb = -1;

                /* say how much is actually free: this failure is disk-
                 * space arithmetic, and the numbers make the fix obvious */
                if (statvfs("/data", &sv) == 0)
                        freeb = (long long)sv.f_bavail * sv.f_frsize;
                if (freeb >= 0)
                        snprintf(buf, sz, "not enough free space on the "
                            "console — a LAN-refused install keeps two "
                            "copies of the package there and needs about "
                            "twice its size free (%lld.%lld GB free "
                            "now); delete something and send again",
                            freeb / (1LL << 30),
                            (freeb % (1LL << 30)) * 10 / (1LL << 30));
                else
                        snprintf(buf, sz, "not enough free space on the "
                            "console — a LAN-refused install keeps two "
                            "copies of the package there and needs about "
                            "twice its size free; delete something and "
                            "send again");
        }
        else if (rc == STAGE_SERVER)
                snprintf(buf, sz, "local install server failed to start");
        else if (rc == STAGE_PULL)
                snprintf(buf, sz, "console did not finish pulling the "
                    "staged package");
        else if (rc == STAGE_FRAME)
                snprintf(buf, sz, "the PC sent the package chunk-framed; "
                    "cannot stage it");
        else if (rc == STAGE_NOTPKG)
                snprintf(buf, sz, "the staged file is not a package "
                    "(corrupted transfer?)");
        else if (c == 0x80A30004u || c == 0x80A3000Au)
                snprintf(buf, sz, "0x%08X (base game not installed)", c);
        else if (c == 0x80A3000Fu)
                snprintf(buf, sz, "0x%08X (patch is for a different game)",
                    c);
        else if (c == 0x80A30011u)
                snprintf(buf, sz, "0x%08X (base game is the wrong version)",
                    c);
        else if (c == 0x80A30019u)
                snprintf(buf, sz, "0x%08X (patch package is invalid)", c);
        else if (c == 0x80A30017u)
                snprintf(buf, sz, "0x%08X (DLC needs its base game)", c);
        else if (c == 0x80A3000Du)
                snprintf(buf, sz, "0x%08X (console firmware is too old)", c);
        else if (c == 0x80A3000Cu)
                snprintf(buf, sz, "0x%08X (close the game first)", c);
        else if (c == 0x80B22404u)
                snprintf(buf, sz, "0x%08X (the console's downloader got "
                    "an HTTP 404 fetching the package)", c);
        else if (c == 0x80B21104u)
                snprintf(buf, sz, "0x%08X (CE-107872-5: not enough free "
                    "space — an install keeps two copies of the package "
                    "on the console at once, so free about twice its "
                    "size; with plenty free, the console may be "
                    "refusing this package build itself)", c);
        else if (c == 0x80A30002u)
                snprintf(buf, sz, "0x%08X (not enough free space)", c);
        else if (c == 0x80A30008u || c == 0x80A30009u)
                snprintf(buf, sz, "0x%08X (package is broken or wrong type)",
                    c);
        else
                snprintf(buf, sz, "0x%08X", c);
        return buf;
}

/* one sceAppInstUtilInstallByPackage call with the given uri. The whole
 * request is zeroed first: Sony reads past the six pointers of
 * pkg_metadata_t, and leftover bytes there make it refuse packages
 * (measured on FW 13.60 by the ps5upload project). */
static int
call_install_uri(const char *uri, const char *content_name,
                 const char *icon_url)
{
        pkg_metadata_t meta;
        pkg_info_t info;
        playgo_info_t playgo;
        int rc;

        memset(&meta, 0, sizeof(meta));
        memset(&info, 0, sizeof(info));
        memset(&playgo, 0, sizeof(playgo));
        meta.uri = uri;
        meta.ex_uri = "";
        meta.playgo_scenario_id = "";
        meta.content_id = "";
        meta.content_name = content_name;
        /* Sender passes the cover URL it serves (/icon/..): the console
         * fetches it itself for the download list. Empty = no cover. */
        meta.icon_url = (icon_url && *icon_url) ? icon_url : "";
        meta.slot = 0;
        meta.is_playgo_enabled = 0;

        pthread_mutex_lock(&g_inst_lock);
        rc = installer_session_locked();
        if (rc == 0) {
                rc = p_install(&meta, &info, &playgo);
                /* success or refusal, this session is spent: the next
                 * call rebuilds it first */
                g_session_used = 1;
        }
        pthread_mutex_unlock(&g_inst_lock);
        return rc;
}

/* ── Staged local install — the fallback for refused LAN installs ────
 * Some firmware refuses an install whose package the console itself has
 * to fetch over the network: sceAppInstUtilInstallByPackage answers an
 * http:// URL with 0x80B22404 (issue #43; the ps5upload project measured
 * the same code on FW 9.60). That code is a downloader error —
 * SCE_PLAYGO_ERROR_CORE_HTTP_STATUS_CODE_404 — because Sony's downloader
 * (ShellCore) probes the URL inside the call: a server that answers the
 * probe with 404 makes the install fail on the spot, even a loopback
 * one. What the console accepts there is a package served from itself:
 * copy the file to /data, serve it back on a loopback-only listener,
 * and hand Sony that URL instead — routing its probe the way ShellCore
 * sends it (see lb_serve).
 * Two other forms are deliberately NOT used, both measured by ps5upload
 * on hardware: a plain filesystem path (refused on some firmware, and
 * the one form known to have wiped installed games) and
 * sceAppInstUtilAppInstallPkg (returns success but leaves an
 * unlaunchable tile on FW 13.60). */
static int
lan_route_refused(int rc)
{
        switch ((uint32_t)rc) {
        case 0x80B22404u: /* the downloader got a 404 probing the URL */
        case 0x80431064u: /* its downloader could not reach the PC */
        case 0x80431068u:
        case 0x8041013du:
        case 0x80431084u: /* a proxy setting blocked the fetch */
                return 1;
        default:
                return 0;
        }
}

/* URL -> plain safe file name for the staged copy (always ends in .pkg) */
static void
stage_name(const char *url, char *out, size_t sz)
{
        const char *p = strrchr(url, '/');
        size_t n = 0;

        for (p = p ? p + 1 : url; *p && *p != '?' && *p != '#' && *p != '&';
            p++) {
                char c = *p;

                if (n + 8 >= sz)
                        break;
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
                        out[n++] = c;
        }
        out[n] = '\0';
        if (n == 0) {
                snprintf(out, sz, "package.pkg");
                return;
        }
        if (n < 4 || strcmp(out + n - 4, ".pkg") != 0)
                snprintf(out + n, sz - n, ".pkg");
}

/* "Content-Range: bytes S-E/T" of a 206 answer: S in *start, T in
 * *total. 0 when the header is absent or unreadable — the caller then
 * treats the body as untrusted and restarts the copy from zero. */
static int
parse_content_range(const char *hdrs, long long *start, long long *total)
{
        const char *cr, *b, *slash, *le;
        char *ep = NULL;
        long long s, t;

        cr = strcasestr(hdrs, "Content-Range:");
        if (!cr)
                return 0;
        le = strstr(cr, "\r\n"); /* never read past this header line */
        b = strcasestr(cr, "bytes");
        if (!b || (le && b > le))
                return 0;
        s = strtoll(b + 5, &ep, 10);
        if (ep == b + 5)
                return 0;
        slash = strchr(ep, '/');
        if (!slash || (le && slash > le))
                return 0;
        t = strtoll(slash + 1, NULL, 10);
        if (t <= 0)
                return 0;
        *start = s;
        *total = t;
        return 1;
}

/* sequential GET of one http:// URL into `local`, resumed across dropped
 * or stalled connections (a Range-capable server lets us continue where
 * we stopped), with progress toasts so a multi-gigabyte copy is never
 * silent. 0 ok (bytes written in *size_out); STAGE_NET / STAGE_DISK /
 * STAGE_SHORT / STAGE_SPACE / STAGE_GONE otherwise. A failed copy is
 * unlinked, never left behind half-written. */
static int
stage_fetch(const char *url, const char *local, const char *name,
    long long *size_out)
{
        char host[256], portstr[16] = "80", get[URL_MAX];
        char req[URL_MAX + 320], hs[4096];
        struct addrinfo hints, *res = NULL, *rp;
        struct statvfs sv;
        struct timeval tv;
        const char *p, *cl, *slash;
        long long want = -1, got = 0, step, reported = 0;
        int s = -1, out = -1, rc = STAGE_NET, attempt;
        char *hb = NULL;
        ssize_t n;

        *size_out = -1;
        p = url + 7; /* skip http:// */
        slash = strchr(p, '/');
        if (!slash || (size_t)(slash - p) >= sizeof(host))
                return STAGE_NET;
        snprintf(host, sizeof(host), "%.*s", (int)(slash - p), p);
        snprintf(get, sizeof(get), "%s", slash);
        p = strchr(host, ':');
        if (p) {
                snprintf(portstr, sizeof(portstr), "%s", p + 1);
                host[p - host] = '\0';
        }
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        hb = malloc(STAGE_CHUNK);
        if (!hb)
                return STAGE_DISK;
        step = stage_report_step(-1);

        for (attempt = 0; attempt < STAGE_TRIES; attempt++) {
                int code = 0, term = 0, fatal = 0, done = 0;
                char range[64] = "";
                size_t hl = 0;
                char c;

                /* the status-code reads below may look past a too-short
                 * header line; keep those bytes defined */
                memset(hs, 0, 16);
                if (attempt > 0)
                        sleep(STAGE_RETRY_S);
                s = -1;
                if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
                        rc = STAGE_NET;
                        continue;
                }
                for (rp = res; rp; rp = rp->ai_next) {
                        s = socket(rp->ai_family, rp->ai_socktype,
                            rp->ai_protocol);
                        if (s < 0)
                                continue;
                        if (connect(s, rp->ai_addr, rp->ai_addrlen) == 0)
                                break;
                        close(s);
                        s = -1;
                }
                freeaddrinfo(res);
                res = NULL;
                if (s < 0) {
                        rc = STAGE_NET;
                        continue;
                }
                tv.tv_sec = STAGE_STALL_S;
                tv.tv_usec = 0;
                setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

                if (got > 0)
                        snprintf(range, sizeof(range),
                            "Range: bytes=%lld-\r\n", got);
                snprintf(req, sizeof(req),
                    "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n"
                    "%s\r\n", get, host, range);
                if (send_all(s, req, strlen(req)) != 0) {
                        close(s);
                        s = -1;
                        rc = STAGE_NET;
                        continue;
                }

                /* headers byte-wise: only the \r\n\r\n terminator decides
                 * where the body starts, so a mid-headers EOF is never
                 * "success" */
                for (;;) {
                        n = recv(s, &c, 1, 0);
                        if (n <= 0)
                                break;
                        if (hl + 1 >= sizeof(hs))
                                break;
                        hs[hl++] = c;
                        hs[hl] = '\0';
                        if (hl >= 4 && !strcmp(hs + hl - 4, "\r\n\r\n")) {
                                term = 1;
                                break;
                        }
                }
                if (!term || strncmp(hs, "HTTP/", 5) != 0) {
                        close(s);
                        s = -1;
                        rc = STAGE_NET;
                        continue;
                }
                if (hs[9] >= '0' && hs[9] <= '9' && hs[10] >= '0' &&
                    hs[10] <= '9' && hs[11] >= '0' && hs[11] <= '9')
                        code = (hs[9] - '0') * 100 + (hs[10] - '0') * 10 +
                            (hs[11] - '0');
                if (code == 404 || code == 410) {
                        /* the PC itself says it does not have the package
                         * anymore: retrying cannot help, and it is not a
                         * firewall problem either */
                        close(s);
                        s = -1;
                        rc = STAGE_GONE;
                        break;
                }
                if (hs[9] != '2') {
                        close(s);
                        s = -1;
                        rc = STAGE_NET;
                        continue;
                }
                if (strcasestr(hs, "Transfer-Encoding:") &&
                    strcasestr(hs, "chunked")) {
                        /* a framed body would be staged with its chunk
                         * size lines mixed into the package bytes; fail
                         * loudly instead of installing garbage */
                        close(s);
                        s = -1;
                        rc = STAGE_FRAME;
                        break;
                }
                cl = strcasestr(hs, "Content-Length:");
                if (!cl)
                        cl = strcasestr(hs, "content-length:");
                {
                        long long clv = cl ? strtoll(cl + 15, NULL, 10) : -1;

                        if (code == 206 && got > 0) {
                                /* a resumed body. Where it actually starts
                                 * is what Content-Range says, not what we
                                 * asked for: a server may answer any
                                 * subrange, and writing its bytes at `got`
                                 * regardless silently corrupts the staged
                                 * package (right size, wrong bytes — the
                                 * install then fails only after the whole
                                 * console-side download). */
                                long long cs = -1, ct = -1;

                                if (parse_content_range(hs, &cs, &ct) &&
                                    ct > 0 && want < 0)
                                        want = ct;
                                if (cs < 0 || cs > got) {
                                        /* cannot tell, or a hole we
                                         * cannot fill: this answer's
                                         * bytes belong nowhere we can
                                         * name. Drop it and pull the
                                         * file from zero on the next
                                         * attempt — staging the body at
                                         * offset 0 would only be caught
                                         * by the magic gate after the
                                         * whole transfer */
                                        got = 0;
                                        reported = 0;
                                        if (out >= 0 &&
                                            ftruncate(out, 0) != 0) {
                                                rc = STAGE_DISK;
                                                fatal = 1;
                                        } else {
                                                close(s);
                                                s = -1;
                                                rc = STAGE_NET;
                                                continue;
                                        }
                                } else if (cs < got) {
                                        /* the server rewound into bytes we
                                         * already have: write the body
                                         * where it says it starts */
                                        got = cs;
                                }
                                if (want < 0 && clv > 0)
                                        want = got + clv; /* fallback
                                         * when no Content-Range said the
                                         * total (non-conforming server) */
                        } else {
                                /* full body from zero. If we asked to
                                 * resume and the server ignored it, start
                                 * over from a clean file */
                                if (got > 0) {
                                        got = 0;
                                        reported = 0;
                                        if (out >= 0 &&
                                            ftruncate(out, 0) != 0) {
                                                rc = STAGE_DISK;
                                                fatal = 1;
                                        }
                                }
                                if (clv > 0) {
                                        want = clv;
                                        step = stage_report_step(want);
                                }
                        }
                }
                if (fatal) {
                        close(s);
                        s = -1;
                        break;
                }
                /* only stage what the whole staged install can hold:
                 * once our copy is down, the console's own downloader
                 * pulls a second full copy of it from the loopback
                 * server into its staging area. Both copies sit on the
                 * same /data volume at the same time, so anything under
                 * roughly twice the package is guaranteed to die
                 * mid-install with 0x80B21104 (CE-107872-5, "not enough
                 * free space") — measured only after a full multi-
                 * gigabyte pull. A partial copy already occupies `got`
                 * of that budget, so count it as available; the gate
                 * runs on every attempt, resume included. */
                if (want > 0 && statvfs("/data", &sv) == 0) {
                        long long freeb =
                            (long long)sv.f_bavail * sv.f_frsize;

                        if (freeb + got < 2 * want + STAGE_HEADROOM) {
                                close(s);
                                s = -1;
                                rc = STAGE_SPACE;
                                break;
                        }
                }
                if (out < 0) {
                        out = open(local, O_WRONLY | O_CREAT | O_TRUNC,
                            0644);
                        if (out < 0) {
                                close(s);
                                s = -1;
                                rc = STAGE_DISK;
                                break;
                        }
                }
                if (lseek(out, got, SEEK_SET) < 0) {
                        close(s);
                        s = -1;
                        rc = STAGE_DISK;
                        break;
                }
                for (;;) {
                        ssize_t off = 0;

                        n = recv(s, hb, STAGE_CHUNK, 0);
                        if (n < 0) {
                                if (errno == EINTR)
                                        continue;
                                rc = STAGE_NET; /* stall or reset */
                                break;
                        }
                        if (n == 0) {
                                if (want > 0 && got < want)
                                        rc = STAGE_SHORT;
                                else {
                                        rc = 0;
                                        done = 1;
                                }
                                break;
                        }
                        while (off < n) {
                                ssize_t w = write(out, hb + off,
                                    (size_t)(n - off));

                                if (w < 0) {
                                        if (errno == EINTR)
                                                continue;
                                        rc = STAGE_DISK;
                                        fatal = 1;
                                        break;
                                }
                                off += w;
                        }
                        if (fatal)
                                break;
                        got += n;
                        job_set(JOBP_STAGE, got, want);
                        if (name && got - reported >= step) {
                                stage_progress("staging", name, got, want);
                                reported = got;
                        }
                }
                if (s >= 0) {
                        close(s);
                        s = -1;
                }
                if (done || fatal)
                        break;
                if (want > 0 && got >= want) {
                        rc = 0;
                        break;
                }
                /* otherwise: dropped or short — resume on the next
                 * attempt instead of failing the whole install */
        }
        free(hb);
        if (s >= 0)
                close(s);
        if (out >= 0)
                close(out);
        if (rc != 0)
                unlink(local);
        else
                *size_out = got;
        return rc;
}

/* ── loopback-only file server for the staged copy ────────────────────
 * Sony's downloader (ShellCore) talks to it like any web server:
 * keep-alive, several parallel connections, HEAD probes, single byte
 * ranges — and it appends "?product=...&serverIpAddr=...&r=..." to
 * every request it sends. Routing must ignore that query: answering
 * the probe with 404 makes InstallByPackage fail the whole install
 * with 0x80B22404 (SCE_PLAYGO_ERROR_CORE_HTTP_STATUS_CODE_404). The
 * response headers are the set the ps5upload project measured Sony
 * accepting on real consoles (their production loopback install route). */
typedef struct lb_server {
        int              listen_fd;
        unsigned short   port;
        volatile int     running;
        pthread_t        listener;
        pthread_mutex_t  lock;
        int              pkg_fd;
        long long        total;
        char             name[192]; /* "/<file name>" served */
        int              conns[LB_MAX_CONNS];
        volatile int     nworkers;
        volatile long long served;
        volatile long long last_ms;
} lb_server_t;

typedef struct {
        lb_server_t *lb;
        int conn;
} lb_worker_arg_t;

static long long
mono_ms(void)
{
        struct timespec ts;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void
http_date(time_t t, char *out, size_t sz)
{
        struct tm tmv;

        gmtime_r(&t, &tmv);
        strftime(out, sz, "%a, %d %b %Y %H:%M:%S GMT", &tmv);
}

/* "bytes=a-b" | "a-" | "-n". 1 = range ok, 0 = none or multi-range
 * (serve the whole file), -1 = unsatisfiable. The start and end outputs
 * are inclusive and clamped to the file. */
static int
parse_range(const char *rv, long long total, long long *start,
    long long *end)
{
        const char *b, *dash;
        long long s, e;

        if (!rv || !(b = strstr(rv, "bytes=")))
                return 0;
        b += 6;
        if (strchr(b, ','))
                return 0;
        dash = strchr(b, '-');
        if (!dash)
                return 0;
        if (dash == b) {
                char *ep = NULL;
                long long n = strtoll(dash + 1, &ep, 10);

                if (ep == dash + 1)
                        return 0; /* malformed: serve the whole file */
                if (n <= 0)
                        return -1;
                if (n > total)
                        n = total;
                s = total - n;
                e = total - 1;
        } else {
                char *ep = NULL;

                s = strtoll(b, &ep, 10);
                if (ep != dash)
                        return 0;
                if (dash[1] >= '0' && dash[1] <= '9')
                        e = strtoll(dash + 1, NULL, 10);
                else
                        e = total - 1; /* open-ended */
        }
        if (total <= 0 || s >= total || s > e)
                return -1;
        if (e >= total)
                e = total - 1;
        *start = s;
        *end = e;
        return 1;
}

/* value of `name:` in a request header block, or NULL */
static const char *
req_header(const char *req, const char *name, char *out, size_t sz)
{
        size_t nlen = strlen(name);
        const char *p = strstr(req, "\r\n");

        if (!p)
                return NULL;
        p += 2;
        while (*p && !(p[0] == '\r' && p[1] == '\n')) {
                const char *le = strstr(p, "\r\n");
                size_t llen = le ? (size_t)(le - p) : strlen(p);

                if (llen > nlen && p[nlen] == ':' &&
                    strncasecmp(p, name, nlen) == 0) {
                        const char *v = p + nlen + 1;
                        size_t i = 0;

                        while (v < p + llen && (*v == ' ' || *v == '\t'))
                                v++;
                        while (*v && v < p + llen && i + 1 < sz)
                                out[i++] = *v++;
                        out[i] = '\0';
                        return out;
                }
                if (!le)
                        break;
                p = le + 2;
        }
        return NULL;
}

/* answer one request; 1 = the connection may carry another, 0 = close */
static int
lb_serve(lb_server_t *lb, int conn, char *req)
{
        char method[8], target[300], version[16], route[300];
        char connv[64], rangev[128], hdr[640], d[64], lm[64];
        long long start = 0, end, off;
        const char *cv;
        int keep, rr, is_head, hlen;
        char *buf;

        pthread_mutex_lock(&lb->lock);
        lb->last_ms = mono_ms();
        pthread_mutex_unlock(&lb->lock);

        if (sscanf(req, "%7s %299s %15s", method, target, version) != 3) {
                hlen = snprintf(hdr, sizeof(hdr),
                    "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                    "Connection: close\r\n\r\n");
                if (hlen > 0)
                        send_all(conn, hdr, (size_t)hlen);
                return 0;
        }
        /* ShellCore appends "?product=...&serverIpAddr=...&r=..." to the
         * path it was handed; route on the bare path or every real
         * request — not just probes — would 404 */
        snprintf(route, sizeof(route), "%s", target);
        {
                char *qm = strchr(route, '?');

                if (qm)
                        *qm = '\0';
        }
        cv = req_header(req, "Connection", connv, sizeof(connv));
        keep = !(cv && strcasestr(connv, "close")) &&
            ((cv && strcasestr(connv, "keep-alive")) ||
                strcmp(version, "HTTP/1.1") == 0);
        is_head = strcmp(method, "HEAD") == 0;
        end = lb->total > 0 ? lb->total - 1 : 0;

        if ((strcmp(method, "GET") != 0 && !is_head) ||
            strcmp(route, lb->name) != 0) {
                hlen = snprintf(hdr, sizeof(hdr),
                    "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                    "Connection: %s\r\n\r\n", keep ? "keep-alive" : "close");
                if (hlen <= 0 || send_all(conn, hdr, (size_t)hlen) != 0)
                        return 0;
                return keep;
        }
        cv = req_header(req, "Range", rangev, sizeof(rangev));
        rr = parse_range(cv, lb->total, &start, &end);
        if (rr < 0) {
                hlen = snprintf(hdr, sizeof(hdr),
                    "HTTP/1.1 416 Range Not Satisfiable\r\n"
                    "Content-Range: bytes */%lld\r\n"
                    "Accept-Ranges: bytes\r\nContent-Length: 0\r\n"
                    "Connection: %s\r\n\r\n", lb->total,
                    keep ? "keep-alive" : "close");
                if (hlen <= 0 || send_all(conn, hdr, (size_t)hlen) != 0)
                        return 0;
                return keep;
        }
        http_date(time(NULL), d, sizeof(d));
        /* fixed past date on every response, so a caching downloader never
         * revalidates mid-install */
        http_date((time_t)1735689600, lm, sizeof(lm));
        if (rr == 0) {
                start = 0;
                end = lb->total > 0 ? lb->total - 1 : 0;
                hlen = snprintf(hdr, sizeof(hdr),
                    "HTTP/1.1 200 OK\r\nDate: %s\r\nLast-Modified: %s\r\n"
                    "Content-Type: application/octet-stream\r\n"
                    "Accept-Ranges: bytes\r\nContent-Length: %lld\r\n"
                    "Connection: %s\r\n\r\n", d, lm, lb->total,
                    keep ? "keep-alive" : "close");
        } else {
                hlen = snprintf(hdr, sizeof(hdr),
                    "HTTP/1.1 206 Partial Content\r\nDate: %s\r\n"
                    "Last-Modified: %s\r\n"
                    "Content-Type: application/octet-stream\r\n"
                    "Accept-Ranges: bytes\r\nContent-Length: %lld\r\n"
                    "Content-Range: bytes %lld-%lld/%lld\r\n"
                    "Connection: %s\r\n\r\n", d, lm, end - start + 1,
                    start, end, lb->total, keep ? "keep-alive" : "close");
        }
        if (hlen <= 0 || send_all(conn, hdr, (size_t)hlen) != 0)
                return 0;
        if (is_head || lb->total == 0)
                return keep;

        /* body — heap buffer, never a worker-thread stack */
        buf = malloc(STAGE_CHUNK);
        if (!buf)
                return 0;
        off = start;
        while (off <= end) {
                long long remain = end - off + 1;
                size_t wantb = remain > (long long)STAGE_CHUNK ?
                    (size_t)STAGE_CHUNK : (size_t)remain;
                ssize_t r = pread(lb->pkg_fd, buf, wantb, (off_t)off);

                if (r <= 0)
                        break;
                if (send_all(conn, buf, (size_t)r) != 0)
                        break;
                pthread_mutex_lock(&lb->lock);
                lb->served += r;
                /* a body still moving is not idle: Sony reads a package's
                 * trailing metadata in one long request */
                lb->last_ms = mono_ms();
                pthread_mutex_unlock(&lb->lock);
                off += r;
        }
        free(buf);
        /* an answer cut short leaves the stream out of step: only a complete
         * answer keeps the connection */
        return off > end ? keep : 0;
}

static void *
lb_worker(void *arg)
{
        lb_worker_arg_t *wa = arg;
        lb_server_t *lb = wa->lb;
        int conn = wa->conn, slot = -1, i, served = 0;
        struct timeval tv = { LB_IO_S, 0 };
        char req[LB_REQ_MAX + 1];
        size_t rlen = 0;

        free(wa);
        {
                int one = 1;

                setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                setsockopt(conn, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
                setsockopt(conn, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        }
        pthread_mutex_lock(&lb->lock);
        for (i = 0; i < LB_MAX_CONNS; i++) {
                if (lb->conns[i] < 0) {
                        lb->conns[i] = conn;
                        slot = i;
                        break;
                }
        }
        pthread_mutex_unlock(&lb->lock);
        if (slot < 0) {
                close(conn);
                __sync_fetch_and_sub(&lb->nworkers, 1);
                return NULL;
        }

        /* one connection may carry many requests (HTTP/1.1 keep-alive), as
         * Sony's installer asks for; requests have no body, so whatever
         * follows one request's blank line is the next */
        while (lb->running && served < 100000) {
                char *end = NULL;

                req[rlen] = '\0';
                while ((end = strstr(req, "\r\n\r\n")) == NULL &&
                    rlen < LB_REQ_MAX) {
                        ssize_t n = recv(conn, req + rlen,
                            LB_REQ_MAX - rlen, 0);

                        if (n <= 0)
                                goto done; /* closed, or idle past LB_IO_S */
                        rlen += (size_t)n;
                        req[rlen] = '\0';
                }
                if (!end)
                        goto done;
                {
                        size_t one = (size_t)(end + 4 - req);
                        char saved = req[one];
                        int keep;

                        req[one] = '\0';
                        keep = lb_serve(lb, conn, req);
                        req[one] = saved;
                        if (!keep)
                                break;
                        memmove(req, req + one, rlen - one);
                        rlen -= one;
                        served++;
                }
        }
done:
        pthread_mutex_lock(&lb->lock);
        lb->conns[slot] = -1;
        pthread_mutex_unlock(&lb->lock);
        close(conn);
        __sync_fetch_and_sub(&lb->nworkers, 1);
        return NULL;
}

static void *
lb_listener(void *arg)
{
        lb_server_t *lb = arg;

        while (lb->running) {
                struct pollfd pf;
                struct sockaddr_in a;
                socklen_t al = sizeof(a);
                lb_worker_arg_t *wa;
                pthread_t t;
                int r, conn, fl;

                pf.fd = lb->listen_fd;
                pf.events = POLLIN;
                pf.revents = 0;
                r = poll(&pf, 1, 250);
                if (r <= 0)
                        continue;
                conn = accept(lb->listen_fd, (struct sockaddr *)&a, &al);
                if (conn < 0)
                        continue;
                fl = fcntl(conn, F_GETFL, 0);
                if (fl >= 0)
                        (void)fcntl(conn, F_SETFL, fl & ~O_NONBLOCK);
                if (lb->nworkers >= LB_MAX_CONNS) {
                        close(conn);
                        continue;
                }
                wa = malloc(sizeof(*wa));
                if (!wa) {
                        close(conn);
                        continue;
                }
                wa->lb = lb;
                wa->conn = conn;
                __sync_fetch_and_add(&lb->nworkers, 1);
                if (pthread_create(&t, NULL, lb_worker, wa) != 0) {
                        __sync_fetch_and_sub(&lb->nworkers, 1);
                        free(wa);
                        close(conn);
                } else {
                        pthread_detach(t);
                }
        }
        return NULL;
}

/* start serving `path` as "/<url_name>" on 127.0.0.1:<ephemeral>.
 * `lb` must be calloc'd by the caller; on failure it is left freed. */
static int
lb_start(lb_server_t *lb, const char *path, const char *url_name)
{
        struct stat st;
        struct sockaddr_in a;
        socklen_t al;
        int i, one = 1, fl;

        lb->listen_fd = -1;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
                return -1;
        lb->pkg_fd = open(path, O_RDONLY);
        if (lb->pkg_fd < 0)
                return -1;
        lb->total = (long long)st.st_size;
        snprintf(lb->name, sizeof(lb->name), "%s", url_name);
        for (i = 0; i < LB_MAX_CONNS; i++)
                lb->conns[i] = -1;
        pthread_mutex_init(&lb->lock, NULL);
        lb->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (lb->listen_fd < 0)
                goto fail;
        setsockopt(lb->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* 127.0.0.1 ONLY */
        a.sin_port = 0;
        if (bind(lb->listen_fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
            listen(lb->listen_fd, LB_MAX_CONNS) != 0)
                goto fail;
        al = sizeof(a);
        if (getsockname(lb->listen_fd, (struct sockaddr *)&a, &al) != 0)
                goto fail;
        lb->port = ntohs(a.sin_port);
        fl = fcntl(lb->listen_fd, F_GETFL, 0);
        if (fl >= 0)
                (void)fcntl(lb->listen_fd, F_SETFL, fl | O_NONBLOCK);
        lb->last_ms = mono_ms();
        lb->running = 1;
        if (pthread_create(&lb->listener, NULL, lb_listener, lb) != 0) {
                lb->running = 0;
                goto fail;
        }
        return 0;
fail:
        if (lb->listen_fd >= 0) {
                close(lb->listen_fd);
                lb->listen_fd = -1;
        }
        close(lb->pkg_fd);
        pthread_mutex_destroy(&lb->lock);
        return -1;
}

/* stop the server. If a worker is still busy inside it, the server is
 * leaked on purpose: freeing memory a live thread still uses would crash
 * the payload. */
static void
lb_stop(lb_server_t *lb)
{
        int i;

        lb->running = 0;
        pthread_join(lb->listener, NULL);
        if (lb->listen_fd >= 0)
                close(lb->listen_fd);
        pthread_mutex_lock(&lb->lock);
        for (i = 0; i < LB_MAX_CONNS; i++) {
                if (lb->conns[i] >= 0)
                        shutdown(lb->conns[i], SHUT_RDWR);
        }
        pthread_mutex_unlock(&lb->lock);
        for (i = 0; i < 200 && lb->nworkers > 0; i++)
                usleep(10000);
        if (lb->nworkers > 0)
                return;
        close(lb->pkg_fd);
        pthread_mutex_destroy(&lb->lock);
        free(lb);
}

static long long
lb_served_bytes(lb_server_t *lb)
{
        long long v;

        pthread_mutex_lock(&lb->lock);
        v = lb->served;
        pthread_mutex_unlock(&lb->lock);
        return v;
}

static long long
lb_idle_ms(lb_server_t *lb)
{
        long long v;

        pthread_mutex_lock(&lb->lock);
        v = mono_ms() - lb->last_ms;
        pthread_mutex_unlock(&lb->lock);
        return v;
}

/* one staged install at a time: two of them would compete for the disk
 * and for the console's single download slot */
static pthread_mutex_t g_stage_lock = PTHREAD_MUTEX_INITIALIZER;

/* ── Stage-folder hygiene ─────────────────────────────────────────────
 * Every exit path of a staged install unlinks its file, but a payload
 * that dies mid-staging cannot: re-injecting SIGKILLs the previous
 * instance (see main.c), and a freeze or power loss takes the process
 * down the same way. Whatever those runs left in the staging folder
 * is an orphan nothing ever deletes — multi-gigabytes of "lost" space
 * that the next install's space gate then counts against the user.
 * Both sweep points below run only while no staged install can own a
 * file (boot, and the start of a staged install under g_stage_lock),
 * so everything found is an orphan by definition. */
static long long
stage_sweep(int *files_out)
{
        DIR *d;
        struct dirent *de;
        long long freed = 0;
        int n = 0;

        if (files_out)
                *files_out = 0;
        d = opendir(STAGE_DIR);
        if (!d)
                return 0;
        while ((de = readdir(d)) != NULL) {
                char p[PATH_MAX_V];
                struct stat st;

                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                        continue;
                snprintf(p, sizeof(p), "%s/%s", STAGE_DIR, de->d_name);
                if (stat(p, &st) == 0 && S_ISREG(st.st_mode) &&
                    unlink(p) == 0) {
                        freed += (long long)st.st_size;
                        n++;
                }
        }
        closedir(d);
        if (files_out)
                *files_out = n;
        return freed;
}

/* the same sweep on demand (POST /api/stage/clear, and boot). A staged
 * install holds g_stage_lock for its whole run, so a trylock that
 * fails means one is live and its file must not be touched — refuse
 * instead of blocking the single-threaded HTTP loop for the length of
 * a multi-gigabyte install. Returns the bytes freed (with *files_out
 * counting files), or -1 while an install is in flight. */
long long
stage_clear(int *files_out)
{
        long long freed;

        if (pthread_mutex_trylock(&g_stage_lock) != 0)
                return -1;
        freed = stage_sweep(files_out);
        pthread_mutex_unlock(&g_stage_lock);
        return freed;
}

/* boot-time sweep: whatever the previous instance could not clean.
 * Say so in a toast — silent multi-gigabyte changes to free space
 * read as a firmware bug, not as hygiene. */
void
stage_sweep_boot(void)
{
        int n = 0;
        long long freed = stage_clear(&n);
        char toast[160];

        if (freed <= 0)
                return;
        snprintf(toast, sizeof(toast),
            APP_TAG ": cleared %d leftover staged package%s (%lld.%lld GB)",
            n, n == 1 ? "" : "s",
            freed / (1LL << 30),
            (freed % (1LL << 30)) * 10 / (1LL << 30));
        notify_user(toast);
}

/* the staged copy must be an actual package before Sony ever sees it:
 * a wrong URL, a framed body or a botched resume would otherwise be
 * handed to the installer and only fail after the console had pulled
 * every byte — an hour wasted and it looks like a firmware bug */
static int
stage_pkg_valid(const char *path)
{
        unsigned char m[4];
        int fd, ok = 0;
        ssize_t n;

        fd = open(path, O_RDONLY);
        if (fd < 0)
                return 0;
        n = read(fd, m, sizeof(m));
        if (n == (ssize_t)sizeof(m))
                ok = (m[0] == 0x7F && m[1] == 'C' && m[2] == 'N' &&
                    m[3] == 'T') /* PS4/legacy CNT container */ ||
                    (m[0] == 0x7F && m[1] == 'F' && m[2] == 'I' &&
                    m[3] == 'H'); /* PS5 native FIH container */
        close(fd);
        return ok;
}

/* The console refused the LAN install: pull the package to /data with
 * our own socket (the direction that already works), then serve it back
 * on loopback and install from there. Keeps the worker busy — and the
 * active-install count up — until the console has pulled the copy. */
static int
staged_install(const char *url, const char *content_name,
    const char *icon_url)
{
        char fname[160], url_name[192], stage_path[PATH_MAX_V], lb_url[256];
        char toast[320];
        long long size = 0, lb_step, lb_reported = 0;
        lb_server_t *lb;
        time_t t0;
        int rc, pulled;

        (void)icon_url; /* the loopback install is self-contained: the
         * package carries its own icon, and an icon_url pointing back
         * at the PC gives the console one more URL its downloader can
         * fail to fetch (measured as an install-killer by the pkgmgr
         * project's offline handling) */

        pthread_mutex_lock(&g_stage_lock);

        /* orphans from an earlier run eat the very space this install
         * is about to be gated on; the lock makes us the only writer,
         * so everything in the folder is fair game (silent — the boot
         * sweep is the loud one) */
        stage_sweep(NULL);

        stage_name(url, fname, sizeof(fname));
        snprintf(url_name, sizeof(url_name), "/%s", fname);
        mkdir_p(STAGE_DIR);
        snprintf(stage_path, sizeof(stage_path), "%s/%s", STAGE_DIR, fname);
        snprintf(toast, sizeof(toast),
            APP_TAG ": LAN install refused, staging %s", content_name);
        notify_user(toast);
        job_set(JOBP_STAGE, 0, 0);

        rc = stage_fetch(url, stage_path, content_name, &size);
        if (rc != 0)
                goto out;
        if (!stage_pkg_valid(stage_path)) {
                rc = STAGE_NOTPKG;
                unlink(stage_path);
                goto out;
        }
        lb = calloc(1, sizeof(*lb));
        if (!lb) {
                rc = STAGE_SERVER;
                unlink(stage_path);
                goto out;
        }
        if (lb_start(lb, stage_path, url_name) != 0) {
                free(lb);
                rc = STAGE_SERVER;
                unlink(stage_path);
                goto out;
        }
        snprintf(lb_url, sizeof(lb_url), "http://127.0.0.1:%u%s",
            (unsigned)lb->port, url_name);
        /* icon: empty on purpose — see the (void)icon_url note above */
        rc = call_install_uri(lb_url, content_name, "");
        if (rc != 0) {
                lb_stop(lb);
                unlink(stage_path);
                goto out;
        }
        job_set(JOBP_COPY, 0, lb->total);
        /* the console pulls the file from us now: serve until it has all of
         * it, or is clearly not coming back — and keep the user posted,
         * this phase is as long as the download was */
        t0 = time(NULL);
        pulled = 0;
        lb_step = stage_report_step(lb->total);
        for (;;) {
                long long served = lb_served_bytes(lb);
                long long idle_ms = lb_idle_ms(lb);

                if (served >= lb->total && idle_ms >= LB_GRACE_S * 1000) {
                        pulled = 1;
                        break;
                }
                if (served == 0 && idle_ms >= LB_FIRST_S * 1000)
                        break; /* accepted but never fetched */
                if (served > 0 && idle_ms >= LB_IDLE_S * 1000)
                        break; /* downloader went away mid-pull */
                if (time(NULL) - t0 > LB_PULL_CAP_S)
                        break; /* hard cap, paused downloads included */
                if (served - lb_reported >= lb_step) {
                        stage_progress("installing", content_name, served,
                            lb->total);
                        lb_reported = served;
                }
                job_set(JOBP_COPY, served, lb->total);
                sleep(1);
        }
        if (!pulled && lb_served_bytes(lb) >= lb->total)
                pulled = 1;
        lb_stop(lb);
        unlink(stage_path);
        rc = pulled ? 0 : STAGE_PULL;
out:
        pthread_mutex_unlock(&g_stage_lock);
        return rc;
}

/* returns 0 on success, SCE error code otherwise (negative codes are
 * ours: -1/-2 dlopen failures, STAGE_* from the staged fallback) */
static int
installer_install(const char *path, const char *want_name,
                  const char *want_icon,
                  char *name_out, size_t name_sz)
{
        char local[URL_MAX + 32];
        const char *uri = path;
        const char *base;
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
                /* strip query/fragment: "/proxy/?b64=.." -> basename would
                 * be garbage */
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

        rc = installer_init();
        if (rc)
                return rc;

        job_begin(path, name_out);
        rc = call_install_uri(uri, name_out,
            (want_icon && *want_icon) ? want_icon : "");
        if (rc == 0)
                return 0;
        /* The console refused to fetch the package itself (issue #43:
         * 0x80B22404 on firmware that gates remote-URL installs). Pull it
         * over here and install it from a loopback copy instead. */
        if (!strncmp(path, "http://", 7) && lan_route_refused(rc))
                return staged_install(path, name_out,
                    (want_icon && *want_icon) ? want_icon : "");
        return rc;
}

/* the last finished install, served by /api/status: an idle console
 * looks the same whether the install succeeded or failed, so the page
 * matches lastUrl against the install it queued and reads the outcome
 * from ok/name/err. */
static pthread_mutex_t g_last_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_last_ok = 0;
static char g_last_url[512];
static char g_last_name[128];
static char g_last_err[256];

/* the record is kept in memory for /api/status and mirrored to
 * /data/homebrew: a receiver that is re-injected (or crashes, or loses
 * power) mid-flight comes back with no memory of the install it ran,
 * and a PC watching its queue then cannot tell a dead install from a
 * finished one. The write lands in a temp file renamed into place, so
 * a power cut can never leave a torn record behind. */
#define LAST_INSTALL_FILE "/data/homebrew/.pkg-receiver-last-install"
#define LAST_INSTALL_TMP  LAST_INSTALL_FILE ".tmp"

/* g_last_lock must be held. */
static void
last_install_set_locked(const char *url, int ok, const char *name,
    const char *err)
{
        g_last_ok = ok;
        snprintf(g_last_url, sizeof(g_last_url), "%s", url ? url : "");
        snprintf(g_last_name, sizeof(g_last_name), "%s", name ? name : "");
        snprintf(g_last_err, sizeof(g_last_err), "%s", err ? err : "");
}

static void
record_last_install(const char *url, int ok, const char *name,
    const char *err)
{
        char eurl[1024], ename[256], eerr[512], line[2048];
        int wrote;
        size_t len;
        FILE *f;

        pthread_mutex_lock(&g_last_lock);
        last_install_set_locked(url, ok, name, err);
        pthread_mutex_unlock(&g_last_lock);

        json_escape(url ? url : "", eurl, sizeof(eurl));
        json_escape(name ? name : "", ename, sizeof(ename));
        json_escape(err ? err : "", eerr, sizeof(eerr));
        wrote = snprintf(line, sizeof(line),
            "{\"ok\":%d,\"url\":\"%s\",\"name\":\"%s\",\"err\":\"%s\"}\n",
            ok ? 1 : 0, eurl, ename, eerr);
        if (wrote < 0 || (size_t)wrote >= sizeof(line))
                return; /* would have been torn; keep the previous record */
        len = (size_t)wrote;
        f = fopen(LAST_INSTALL_TMP, "w");
        if (!f)
                return;
        if (fwrite(line, 1, len, f) == len)
                rename(LAST_INSTALL_TMP, LAST_INSTALL_FILE);
        else
                unlink(LAST_INSTALL_TMP);
        fclose(f);
}

/* boot-time reload of the record persisted above. Best effort: a
 * missing, unreadable or unparseable record leaves the receiver as it
 * was, exactly as before this file existed. */
void
install_last_restore(void)
{
        char buf[2048], url[512], name[128], err[256];
        long long ok = 0;
        size_t n;
        FILE *f;

        f = fopen(LAST_INSTALL_FILE, "r");
        if (!f)
                return;
        n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        if (n == 0)
                return;
        buf[n] = '\0';
        if (!json_string(buf, "url", url, sizeof(url)) || !url[0])
                return;
        json_string(buf, "name", name, sizeof(name));
        json_string(buf, "err", err, sizeof(err));
        json_long(buf, "ok", &ok);
        pthread_mutex_lock(&g_last_lock);
        last_install_set_locked(url, ok != 0, name, err);
        pthread_mutex_unlock(&g_last_lock);
}

void
install_last_result(int *ok, char *url, size_t url_sz, char *name,
    size_t name_sz, char *err, size_t err_sz)
{
        pthread_mutex_lock(&g_last_lock);
        *ok = g_last_ok;
        snprintf(url, url_sz, "%s", g_last_url);
        snprintf(name, name_sz, "%s", g_last_name);
        snprintf(err, err_sz, "%s", g_last_err);
        pthread_mutex_unlock(&g_last_lock);
}

/* live progress of the running install, for /api/status: phase 0 idle,
 * JOBP_CONSOLE the console fetches the package itself, JOBP_STAGE we
 * are staging it, JOBP_COPY the console is copying the staged one */
void
install_job_progress(int *phase, long long *got, long long *want,
    char *name, size_t name_sz, char *url, size_t url_sz)
{
        pthread_mutex_lock(&g_job_lock);
        *phase = g_job_phase;
        *got = g_job_got;
        *want = g_job_want;
        snprintf(name, name_sz, "%s", g_job_name);
        snprintf(url, url_sz, "%s", g_job_url);
        pthread_mutex_unlock(&g_job_lock);
}

static void *
install_worker(void *arg)
{
        install_job_t *job = arg;
        char name[256];
        char toast[352];
        char err[256];
        int rc;

        __sync_fetch_and_add(&g_active_installs, 1);
        rc = installer_install(job->url,
            job->name[0] ? job->name : NULL,
            job->icon[0] ? job->icon : NULL, name, sizeof(name));
        job_set(0, 0, 0);

        if (rc == 0) {
                snprintf(toast, sizeof(toast), APP_TAG ": installing %s", name);
                err[0] = '\0';
        } else {
                snprintf(toast, sizeof(toast), APP_TAG ": install failed %s",
                    install_err_text(rc, err, sizeof(err)));
        }
        notify_user(toast);
        record_last_install(job->url, rc == 0, name, err);
        __sync_fetch_and_sub(&g_active_installs, 1);
        /* release the duplicate guard so a retry can run */
        pthread_mutex_lock(&g_cur_lock);
        if (job->slot >= 0 && g_cur_used[job->slot] &&
            strcmp(job->url, g_cur_urls[job->slot]) == 0)
                g_cur_used[job->slot] = 0;
        pthread_mutex_unlock(&g_cur_lock);
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
        /* one in-flight install per URL: a double click must not stack a
         * second identical staging download on the first */
        pthread_mutex_lock(&g_cur_lock);
        {
                int i, slot = -1, dup = 0;

                for (i = 0; i < CUR_SLOTS; i++) {
                        if (g_cur_used[i]) {
                                if (strcmp(job->url,
                                    g_cur_urls[i]) == 0) {
                                        dup = 1;
                                        break;
                                }
                        } else if (slot < 0) {
                                slot = i;
                        }
                }
                if (dup) {
                        pthread_mutex_unlock(&g_cur_lock);
                        {
                                char toast[320];

                                snprintf(toast, sizeof(toast),
                                    APP_TAG ": already installing %s",
                                    job->name[0] ? job->name : job->url);
                                notify_user(toast);
                        }
                        free(job);
                        return 0;
                }
                if (slot >= 0) {
                        snprintf(g_cur_urls[slot],
                            sizeof(g_cur_urls[slot]), "%s", job->url);
                        g_cur_used[slot] = 1;
                }
                job->slot = slot;
        }
        pthread_mutex_unlock(&g_cur_lock);
        if (pthread_create(&tid, NULL, install_worker, job) != 0) {
                pthread_mutex_lock(&g_cur_lock);
                if (job->slot >= 0 && g_cur_used[job->slot] &&
                    strcmp(job->url, g_cur_urls[job->slot]) == 0)
                        g_cur_used[job->slot] = 0;
                pthread_mutex_unlock(&g_cur_lock);
                free(job);
                return -1;
        }
        pthread_detach(tid);
        return 0;
#endif
}
