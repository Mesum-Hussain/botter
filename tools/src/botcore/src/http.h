#ifndef BC_HTTP_H
#define BC_HTTP_H

/*
 * Minimal HTTP(S) client. Transport is a curl subprocess (interim; see
 * progress.md I1). The request (URL, auth header, body) is fed to curl on
 * stdin as a config file, so secrets never appear in argv.
 */

typedef struct {
    int   status; /* HTTP status; 0 if no response */
    char *body;   /* NUL-terminated, never NULL after http_request */
    char *err;    /* transport error text (curl stderr) or NULL */
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
void http_resp_free(http_resp_t *r);

/* Async-signal-safe: kill the in-flight request, if any. */
void http_abort(void);

#endif
