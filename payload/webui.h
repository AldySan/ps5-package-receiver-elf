#ifndef WEBUI_H
#define WEBUI_H

#include <stddef.h>

/* the embedded web page and its size in bytes (the test build gets a
 * stub page) */
extern const char UI_HTML[];
extern const size_t UI_HTML_SIZE;

/* tile logo served at /logo.png and /favicon.ico (every build) */
extern const unsigned char sender_logo[];
extern const size_t sender_logo_size;

#endif /* WEBUI_H */
