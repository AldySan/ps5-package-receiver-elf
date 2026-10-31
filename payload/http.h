#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

/* reply helpers */
int  send_all(int fd, const char *buf, size_t len);
void send_text(int fd, const char *body);
void send_page(int fd, const char *body, size_t len);
void send_json(int fd, const char *body);
void send_png(int fd, const unsigned char *data, size_t len);

/* parsing helpers */
void url_decode(const char *src, char *dst, size_t dst_sz);
void url_encode_spaces(const char *src, char *dst, size_t dst_sz);
int  grab_http_url(const char *buf, char *dst, size_t dst_sz);
int  json_first_package(const char *body, char *dst, size_t dst_sz);
int  query_url(const char *path, char *dst, size_t dst_sz);
void json_escape(const char *src, char *dst, size_t dst_sz);
int  query_param(const char *path, const char *key, char *dst,
                 size_t dst_sz);
int  json_string(const char *body, const char *key, char *dst,
                 size_t dst_sz);
int  json_long(const char *body, const char *key, long long *out);

/* request reading */
long read_headers(int fd, char *buf, size_t cap);
long content_length(const char *hdr);

#endif /* HTTP_H */
