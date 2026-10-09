#ifndef BC_HTTP_H
#define BC_HTTP_H

#include <stddef.h>

/*
 * Minimal in-process HTTP/1.1 client: plain HTTP (loopback only, enforced by
 * the caller) or HTTPS via vendored BearSSL (TLS 1.2) with compiled-in Mozilla
 * CA anchors; no host certificate files, no curl. HTTPS_PROXY (http:// proxy,
 * CONNECT, optional user:pass) and NO_PROXY are honoured.
 */

typedef struct {
    int   status; /* HTTP status; 0 if no response */
    char *body;   /* NUL-terminated, never NULL after http_request */
    char *err;    /* transport error text or NULL */
} http_resp_t;

#define HTTP_OK          0
#define HTTP_ERR_XPORT  (-1)
#define HTTP_ERR_ABORT  (-2)

/*
 * GET when body == NULL, otherwise POST with Content-Type: application/json.
 * bearer may be NULL. Returns HTTP_OK if a response was received (any status).
 * Caller must http_resp_free() in every case.
 */
int  http_request(const char *url, const char *bearer, const char *body,
                  int timeout_s, http_resp_t *out);

/*
 * Same, but for a 2xx response every piece of the (de-chunked) body is also
 * passed to on_body as it arrives (Server-Sent Events streaming); out->body
 * still gets the whole body. The request asks for text/event-stream.
 */
typedef void (*http_body_fn)(void *ud, const char *p, size_t n);
int  http_request_stream(const char *url, const char *bearer, const char *body, int timeout_s,
                         http_body_fn on_body, void *ud, http_resp_t *out);
void http_resp_free(http_resp_t *r);

/* Async-signal-safe: kill the in-flight request, if any. */
void http_abort(void);

#endif
