#include <stdio.h>
#include <string.h>

#include "common.h"
#include "http.h"
#include "installer.h"
#include "handlers.h"

/* Endpoints that queue a PKG install on the detached worker:
 *   GET  /install?url=  (used by the WebUI buttons, plain-text reply)
 *   POST /api/install   (used by PKG Sender itself, JSON reply)
 *   POST /upload        (multipart form fallback, url field) */

static void
do_install_reply_text(int fd, const char *url, const char *name,
                      const char *icon)
{
	char disp[256], out[URL_MAX + 64];

	if (name && *name)
		snprintf(disp, sizeof(disp), "%s", name);
	else {
		const char *base = strrchr(url, '/');
		base = base ? base + 1 : url;
		snprintf(disp, sizeof(disp), "%s", base);
	}
	if (queue_install(url, name, icon) == 0)
		snprintf(out, sizeof(out), "ok: install queued for %s", disp);
	else
		snprintf(out, sizeof(out), "error:queue failed");
	send_text(fd, out);
}

void
h_install_get(int fd, const char *path, char *body, long body_len)
{
	char url[URL_MAX], url_fixed[URL_MAX], gname[256], gicon[512],
	    icon_fixed[512];

	if (query_url(path, url, sizeof(url))) {
		gname[0] = '\0';
		gicon[0] = '\0';
		query_param(path, "name", gname, sizeof(gname));
		query_param(path, "icon", gicon, sizeof(gicon));
		/* the query args arrive percent-decoded, so a url or icon with
		 * a space in it reaches the console installer raw and breaks
		 * it — sanitize exactly like the POST path (issue #6) */
		if (gicon[0])
			url_encode_spaces(gicon, icon_fixed,
			    sizeof(icon_fixed));
		else
			icon_fixed[0] = '\0';
		url_encode_spaces(url, url_fixed, sizeof(url_fixed));
		do_install_reply_text(fd, url_fixed, gname[0] ? gname : NULL,
		                      icon_fixed[0] ? icon_fixed : NULL);
	} else {
		send_text(fd, "error:missing url");
	}
}

void
h_install_post(int fd, const char *path, char *body, long body_len)
{
	char url[URL_MAX], gname[256], gicon[512];
	int total = 0, i, queued = 0;

	/* the wire contract is an array: queue EVERY url it carries.
	 * Dropping all but the first used to leave the rest of a
	 * multi-package request silently uninstalled. */
	while (json_package_at(body, total, url, sizeof(url)))
		total++;
	if (total == 0) {
		send_json(fd,
		    "{\"status\":\"fail\","
		    "\"error\":\"no package url\"}");
		return;
	}
	gname[0] = '\0';
	gicon[0] = '\0';
	json_string(body, "name", gname, sizeof(gname));
	json_string(body, "icon_url", gicon, sizeof(gicon));
	/* icon_url arrives percent-encoded like packages (issue #6);
	 * decode it symmetrically, then make any leftover raw
	 * whitespace safe for the console installer. */
	if (gicon[0]) {
		char icon_dec[512];

		url_decode(gicon, icon_dec, sizeof(icon_dec));
		url_encode_spaces(icon_dec, gicon, sizeof(gicon));
	}
	for (i = 0; i < total; i++) {
		char url_fixed[URL_MAX];

		if (!json_package_at(body, i, url, sizeof(url)))
			break;
		url_encode_spaces(url, url_fixed, sizeof(url_fixed));
		/* one name/icon can only describe one package; with several,
		 * each install falls back to its own file name */
		if (queue_install(url_fixed,
		                  total == 1 && gname[0] ? gname : NULL,
		                  total == 1 && gicon[0] ? gicon : NULL) == 0)
			queued++;
	}
	if (queued == total)
		send_json(fd, "{\"status\":\"success\"}");
	else if (queued > 0)
		send_json(fd,
		    "{\"status\":\"fail\","
		    "\"error\":\"some packages failed to queue\"}");
	else
		send_json(fd,
		    "{\"status\":\"fail\","
		    "\"error\":\"queue failed\"}");
}

void
h_upload(int fd, const char *path, char *body, long body_len)
{
	char url[URL_MAX];

	if (grab_http_url(body, url, sizeof(url))) {
		char out[URL_MAX + 32];

		if (queue_install(url, NULL, NULL) == 0)
			snprintf(out, sizeof(out), "SUCCESS: %s", url);
		else
			snprintf(out, sizeof(out), "FAILED: queue failed");
		send_text(fd, out);
	} else {
		send_text(fd, "FAILED: no url field");
	}
}
