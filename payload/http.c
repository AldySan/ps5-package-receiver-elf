#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/socket.h>

#include "common.h"
#include "http.h"

/* ── tiny helpers ────────────────────────────────────────────────────── */
int
send_all(int fd, const char *buf, size_t len)
{
        size_t off = 0;

        while (off < len) {
                ssize_t n = send(fd, buf + off, len - off, 0);
                if (n <= 0)
                        return -1;
                off += (size_t)n;
        }
        return 0;
}

void
send_text(int fd, const char *body)
{
        char hdr[256];
        int hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Content-Length: %lu\r\n"
            "Connection: close\r\n"
            "\r\n", (unsigned long)strlen(body));

        send_all(fd, hdr, (size_t)hlen);
        send_all(fd, body, strlen(body));
}

/* the embedded WebUI page: raw bytes with an explicit length, so the
 * page is a build artifact (.incbin) instead of a C string */
void
send_page(int fd, const char *body, size_t len)
{
        char hdr[256];
        int hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: %lu\r\n"
            "Cache-Control: no-store\r\n"
            "Connection: close\r\n"
            "\r\n", (unsigned long)len);

        send_all(fd, hdr, (size_t)hlen);
        send_all(fd, body, len);
}

void
send_json(int fd, const char *body)
{
        char hdr[256];
        int hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %lu\r\n"
            "Connection: close\r\n"
            "\r\n", (unsigned long)strlen(body));

        send_all(fd, hdr, (size_t)hlen);
        send_all(fd, body, strlen(body));
}

void
send_png(int fd, const unsigned char *data, size_t len)
{
        char hdr[256];
        int hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: image/png\r\n"
            "Content-Length: %lu\r\n"
            "Cache-Control: max-age=86400\r\n"
            "Connection: close\r\n"
            "\r\n", (unsigned long)len);

        send_all(fd, hdr, (size_t)hlen);
        send_all(fd, (const char *)data, len);
}
/* %XX -> byte, + -> space. dst must fit URL_MAX. A '%' that is not
 * followed by two hex digits is copied through as-is: strtol() would
 * turn "%ZZ" into a NUL byte and silently truncate the result. */
void
url_decode(const char *src, char *dst, size_t dst_sz)
{
        size_t o = 0;

        while (*src && o + 1 < dst_sz) {
                if (*src == '%' &&
                    isxdigit((unsigned char)src[1]) &&
                    isxdigit((unsigned char)src[2])) {
                        char hex[3] = { src[1], src[2], 0 };
                        dst[o++] = (char)strtol(hex, NULL, 16);
                        src += 3;
                } else if (*src == '+') {
                        dst[o++] = ' ';
                        src++;
                } else {
                        dst[o++] = *src++;
                }
        }
        dst[o] = '\0';
}

/* raw spaces/tabs in a URL break the console installer: turn them into
 * %20 after decoding (issue #6). Only the unsafe whitespace is touched,
 * everything else passes through byte-identical. */
void
url_encode_spaces(const char *src, char *dst, size_t dst_sz)
{
        size_t o = 0;

        while (*src && o + 1 < dst_sz) {
                if (*src == ' ' || *src == '\t') {
                        if (o + 3 >= dst_sz)
                                break;
                        dst[o++] = '%';
                        dst[o++] = '2';
                        dst[o++] = '0';
                        src++;
                } else {
                        dst[o++] = *src++;
                }
        }
        dst[o] = '\0';
}

/* first http(s):// token in buf -> dst (stops at ws, quote, <, \r, \n). */
int
grab_http_url(const char *buf, char *dst, size_t dst_sz)
{
        const char *p = strstr(buf, "http");
        size_t i = 0;

        if (!p)
                return 0;
        while (*p && i + 1 < dst_sz && *p != '"' && *p != '\'' &&
               *p != '<' && *p != ' ' && *p != '\t' &&
               *p != '\r' && *p != '\n')
                dst[i++] = *p++;
        dst[i] = '\0';
        return i > 0;
}

/* "packages" : [ " <url> " ] -> decoded url in dst. */
int
json_first_package(const char *body, char *dst, size_t dst_sz)
{
        const char *p = strstr(body, "packages");
        char enc[URL_MAX];
        size_t i = 0;

        if (!p)
                return 0;
        p = strchr(p, '[');
        if (!p)
                return 0;
        p = strchr(p, '"');
        if (!p)
                return 0;
        p++;
        while (*p && *p != '"' && i + 1 < sizeof(enc))
                enc[i++] = *p++;
        enc[i] = '\0';
        if (i == 0)
                return 0;
        url_decode(enc, dst, dst_sz);
        return dst[0] != '\0';
}

int
query_url(const char *path, char *dst, size_t dst_sz)
{
        const char *p = strstr(path, "url=");
        char raw[URL_MAX];
        size_t i = 0;

        if (!p)
                return 0;
        p += 4;
        while (*p && *p != '&' && *p != ' ' && i + 1 < sizeof(raw))
                raw[i++] = *p++;
        raw[i] = '\0';
        if (i == 0)
                return 0;
        url_decode(raw, dst, dst_sz);
        return dst[0] != '\0';
}
/* escape " \ and C0 controls for JSON (shared, always compiled) */
void
json_escape(const char *src, char *dst, size_t dst_sz)
{
        size_t o = 0;

        while (*src && o + 1 < dst_sz) {
                unsigned char c = (unsigned char)*src++;
                if (c == '"' || c == '\\') {
                        if (o + 2 >= dst_sz)
                                break;
                        dst[o++] = '\\';
                        dst[o++] = (char)c;
                } else if (c < 0x20) {
                        dst[o++] = ' ';
                } else {
                        dst[o++] = (char)c;
                }
        }
        dst[o] = '\0';
}

/* query key= -> decoded value (stops at & or space) */
int
query_param(const char *path, const char *key, char *dst, size_t dst_sz)
{
        char pat[64], raw[PATH_MAX_V];
        size_t i = 0;
        const char *p;

        snprintf(pat, sizeof(pat), "%s=", key);
        p = strstr(path, pat);
        if (!p)
                return 0;
        p += strlen(pat);
        while (*p && *p != '&' && *p != ' ' && i + 1 < sizeof(raw))
                raw[i++] = *p++;
        raw[i] = '\0';
        if (i == 0)
                return 0;
        url_decode(raw, dst, dst_sz);
        return dst[0] != '\0';
}

/* "key" : "string" (handles \" and \\) -> dst */
int
json_string(const char *body, const char *key, char *dst, size_t dst_sz)
{
        char pat[64];
        const char *p;
        size_t o = 0;

        snprintf(pat, sizeof(pat), "\"%s\"", key);
        p = strstr(body, pat);
        if (!p)
                return 0;
        p = strchr(p + strlen(pat), ':');
        if (!p)
                return 0;
        p = strchr(p, '"');
        if (!p)
                return 0;
        p++;
        while (*p && *p != '"' && o + 1 < dst_sz) {
                if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) {
                        dst[o++] = p[1];
                        p += 2;
                } else {
                        dst[o++] = *p++;
                }
        }
        dst[o] = '\0';
        return o > 0;
}

/* "key" : 12345 -> value */
int
json_long(const char *body, const char *key, long long *out)
{
        char pat[64];
        const char *p;

        snprintf(pat, sizeof(pat), "\"%s\"", key);
        p = strstr(body, pat);
        if (!p)
                return 0;
        p = strchr(p + strlen(pat), ':');
        if (!p)
                return 0;
        p++;
        while (*p == ' ' || *p == '\t')
                p++;
        *out = strtoll(p, NULL, 10);
        return 1;
}
/* read until end of HTTP headers. returns header length or -1. */
long
read_headers(int fd, char *buf, size_t cap)
{
        size_t total = 0;
        time_t deadline = time(NULL) + HDR_DEADLINE_S;

        while (total + 1 < cap) {
                ssize_t n;

                if (time(NULL) > deadline)
                        return -1;
                n = recv(fd, buf + total, cap - 1 - total, 0);
                if (n <= 0)
                        return -1;
                total += (size_t)n;
                buf[total] = '\0';
                if (strstr(buf, "\r\n\r\n"))
                        return (long)total;
        }
        return -1;
}

long
content_length(const char *hdr)
{
        const char *p = strcasestr(hdr, "content-length:");

        /* must be the field name itself, not a suffix such as
         * "X-Content-Length:" — the match has to sit at the very
         * start of a line */
        if (!p || (p != hdr && p[-1] != '\n'))
                return 0;
        return strtol(p + 15, NULL, 10);
}
