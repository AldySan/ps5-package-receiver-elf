#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>

#include "common.h"
#include "http.h"
#include "webui.h"
#include "pull.h"
#include "discovery.h"
#include "installer.h"
#include "handlers.h"

/* Core endpoints: the WebUI page, the logo it embeds, and the small
 * read-only JSON probes the page polls while it is open. */

void
h_index(int fd, const char *path, char *body, long body_len)
{
        send_page(fd, UI_HTML, UI_HTML_SIZE);
}

void
h_logo(int fd, const char *path, char *body, long body_len)
{
        send_png(fd, sender_logo, sender_logo_size);
}

void
h_api_probe(int fd, const char *path, char *body, long body_len)
{
        /* open probe: identifies us, performs nothing */
        send_json(fd,
            "{\"status\":\"fail\","
            "\"error\":\"Unsupported method: use POST /api/install\"}");
}

void
h_version(int fd, const char *path, char *body, long body_len)
{
        send_json(fd, "{\"build\":\"" RECEIVER_BUILD "\"}");
}

void
h_dbg(int fd, const char *path, char *body, long body_len)
{
        char out[320], esc[256];

        json_escape(g_last_req, esc, sizeof(esc));
        snprintf(out, sizeof(out), "{\"last\":\"%s\"}", esc);
        send_json(fd, out);
}

void
h_status(int fd, const char *path, char *body, long body_len)
{
        /* every string goes through json_escape: a quote or backslash in
         * a file or game name must not produce broken JSON (the page
         * would silently keep the last status forever). The buffer is
         * sized for the worst case: each escaped field can double. */
        char out[3072];
        char pulln[128], lasturl[512], lastname[128], lasterr[160];
        char epulln[256], eurl[1024], ename[256], eerr[320];
        int last_ok;

        snprintf(pulln, sizeof(pulln), "%s",
            pull_active() ? pull_name() : "");
        json_escape(pulln, epulln, sizeof(epulln));
        install_last_result(&last_ok, lasturl, sizeof(lasturl),
            lastname, sizeof(lastname), lasterr, sizeof(lasterr));
        json_escape(lasturl, eurl, sizeof(eurl));
        json_escape(lastname, ename, sizeof(ename));
        json_escape(lasterr, eerr, sizeof(eerr));
        snprintf(out, sizeof(out),
            "{\"busy\":%s,\"active\":%d,"
            "\"pull\":%s,\"pullName\":\"%s\","
            "\"pullGot\":%lld,\"pullWant\":%lld,\"pullPaused\":%s,"
            "\"lastOk\":%s,\"lastUrl\":\"%s\","
            "\"lastName\":\"%s\",\"lastErr\":\"%s\"}",
            g_active_installs > 0 ? "true" : "false",
            g_active_installs,
            pull_active() ? "true" : "false",
            epulln,
            pull_got(), pull_want(),
            pull_paused() ? "true" : "false",
            last_ok ? "true" : "false",
            eurl, ename, eerr);
        send_json(fd, out);
}

void
h_pc(int fd, const char *path, char *body, long body_len)
{
        char out[128];

        snprintf(out, sizeof(out), "{\"pc\":\"%s\",\"age\":%ld}",
            pc_addr(), pc_age());
        send_json(fd, out);
}

void
h_space(int fd, const char *path, char *body, long body_len)
{
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
}
