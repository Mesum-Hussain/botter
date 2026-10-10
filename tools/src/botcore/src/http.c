#define _GNU_SOURCE
#include "http.h"

#include "bearssl.h"
#include "ca_anchors.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_OUT           (32u * 1024 * 1024)
#define MAX_HEADERS       (64u * 1024)
#define CONNECT_TIMEOUT_S 15
#define SLICE_MS          200 /* poll granularity: how fast Ctrl-C and deadlines are noticed */

static volatile sig_atomic_t g_aborted = 0;

void http_abort(void)
{
    g_aborted = 1;
}

/* ---- growable buffer -------------------------------------------------- */

typedef struct {
    char  *p;
    size_t len, cap;
} sb_t;

static int sb_add(sb_t *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1) {
            cap *= 2;
        }
        char *np = realloc(b->p, cap);
        if (!np) {
            return -1;
        }
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
    return 0;
}

static int sb_fmt(sb_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int sb_fmt(sb_t *b, const char *fmt, ...)
{
    char    tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        return -1;
    }
    return sb_add(b, tmp, (size_t)n);
}

static void sb_wipe(sb_t *b)
{
    if (b->p) {
        explicit_bzero(b->p, b->cap);
        free(b->p);
    }
    b->p = NULL;
    b->len = b->cap = 0;
}

static int has_ctl(const char *s)
{
    for (; *s; s++) {
        if ((unsigned char)*s < 0x20 || *s == 0x7f) {
            return 1;
        }
    }
    return 0;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ---- URLs ------------------------------------------------------------- */

typedef struct {
    int         tls;
    char        host[256];      /* without [] for IPv6 */
    char        port[8];
    char        authority[300]; /* Host header / CONNECT target: host[:port] as written */
    char        user[128], pass[128];
    const char *path;           /* "/..." (points into the URL) */
} url_t;

static int hexval(int c)
{
    return isdigit(c) ? c - '0' : isxdigit(c) ? (tolower(c) - 'a' + 10) : -1;
}

static void pct_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '%' && hexval((unsigned char)s[1]) >= 0 && hexval((unsigned char)s[2]) >= 0) {
            *o++ = (char)(hexval((unsigned char)s[1]) * 16 + hexval((unsigned char)s[2]));
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = '\0';
}

/* http(s)://[user:pass@]host[:port][/path]. userinfo only allowed when allow_user (proxy URLs). */
static int url_parse(const char *u, url_t *o, int allow_user)
{
    memset(o, 0, sizeof(*o));
    if (strncasecmp(u, "https://", 8) == 0) {
        o->tls = 1;
        u += 8;
    } else if (strncasecmp(u, "http://", 7) == 0) {
        u += 7;
    } else {
        return -1;
    }
    const char *end = u + strcspn(u, "/?#");
    const char *at = memchr(u, '@', (size_t)(end - u));
    if (at) {
        if (!allow_user) {
            return -1;
        }
        const char *colon = memchr(u, ':', (size_t)(at - u));
        size_t ul = (size_t)((colon ? colon : at) - u);
        if (ul >= sizeof(o->user) || (colon && (size_t)(at - colon - 1) >= sizeof(o->pass))) {
            return -1;
        }
        memcpy(o->user, u, ul);
        if (colon) {
            memcpy(o->pass, colon + 1, (size_t)(at - colon - 1));
        }
        pct_decode(o->user);
        pct_decode(o->pass);
        u = at + 1;
    }
    if ((size_t)(end - u) >= sizeof(o->authority) || end == u) {
        return -1;
    }
    memcpy(o->authority, u, (size_t)(end - u));
    const char *h = u, *he, *pp = NULL;
    if (*h == '[') {
        he = memchr(h, ']', (size_t)(end - h));
        if (!he) {
            return -1;
        }
        h++;
        pp = he + 1 < end && he[1] == ':' ? he + 2 : NULL;
    } else {
        he = memchr(h, ':', (size_t)(end - h));
        if (he) {
            pp = he + 1;
        } else {
            he = end;
        }
    }
    if ((size_t)(he - h) >= sizeof(o->host) || he == h) {
        return -1;
    }
    memcpy(o->host, h, (size_t)(he - h));
    if (pp) {
        size_t pl = (size_t)(end - pp);
        if (pl == 0 || pl >= sizeof(o->port) || strspn(pp, "0123456789") < pl) {
            return -1;
        }
        memcpy(o->port, pp, pl);
    } else {
        strcpy(o->port, o->tls ? "443" : "80");
    }
    o->path = *end == '/' ? end : "/";
    return 0;
}

/* NO_PROXY: comma/space list of hosts or domain suffixes; "*" = everything. */
static int no_proxy_match(const char *host)
{
    const char *np = getenv("no_proxy");
    if (!np || !*np) {
        np = getenv("NO_PROXY");
    }
    if (!np) {
        return 0;
    }
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", np);
    size_t hl = strlen(host);
    for (char *save = NULL, *t = strtok_r(buf, ", ", &save); t; t = strtok_r(NULL, ", ", &save)) {
        if (strcmp(t, "*") == 0) {
            return 1;
        }
        if (*t == '.') {
            t++;
        }
        char *colon = strchr(t, ':');
        if (colon) {
            *colon = '\0';
        }
        size_t tl = strlen(t);
        if (tl && hl >= tl && strcasecmp(host + hl - tl, t) == 0 && (hl == tl || host[hl - tl - 1] == '.')) {
            return 1;
        }
    }
    return 0;
}

static void base64(const unsigned char *in, size_t n, char *out)
{
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)in[i] << 16 | (i + 1 < n ? (unsigned)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = A[(v >> 18) & 63];
        out[o++] = A[(v >> 12) & 63];
        out[o++] = i + 1 < n ? A[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? A[v & 63] : '=';
    }
    out[o] = '\0';
}

/* ---- connection: plain or TLS over a non-blocking socket ------------- */

typedef struct {
    int                    fd;
    int                    tls;
    double                 deadline;
    int                    aborted;
    char                   err[256];
    br_ssl_client_context  cc;
    br_x509_minimal_context xc;
    unsigned char          iobuf[BR_SSL_BUFSIZE_BIDI];
} conn_t;

/* 1 ready, 0 timed out, -1 error, -2 aborted. */
static int wait_fd(conn_t *c, short ev, double deadline)
{
    for (;;) {
        if (g_aborted) {
            c->aborted = 1;
            return -2;
        }
        double left = deadline - now_s();
        if (left <= 0) {
            return 0;
        }
        int ms = left * 1000 < SLICE_MS ? (int)(left * 1000) + 1 : SLICE_MS;
        struct pollfd p = {c->fd, ev, 0};
        int r = poll(&p, 1, ms);
        if (r > 0) {
            return 1;
        }
        if (r < 0 && errno != EINTR) {
            return -1;
        }
    }
}

static int fail(conn_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int fail(conn_t *c, const char *fmt, ...)
{
    if (!c->err[0]) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(c->err, sizeof(c->err), fmt, ap);
        va_end(ap);
    }
    return -1;
}

static int timeout_or_abort(conn_t *c, int w)
{
    return w == -2 ? -1 : w == 0 ? fail(c, "timed out") : fail(c, "network error: %s", strerror(errno));
}

static int tcp_connect(conn_t *c, const char *host, const char *port)
{
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int gr = getaddrinfo(host, port, &hints, &res);
    for (int i = 0; gr == EAI_AGAIN && i < 3 && now_s() + 1 < c->deadline; i++) {
        /* a resolver that is busy or just woke up (first query after a network change) */
        struct timespec ts = {0, 500 * 1000000L};
        nanosleep(&ts, NULL);
        gr = getaddrinfo(host, port, &hints, &res);
    }
    if (gr != 0) {
        return fail(c, "could not resolve host %s: %s", host, gai_strerror(gr));
    }
    double cdl = now_s() + CONNECT_TIMEOUT_S;
    if (cdl > c->deadline) {
        cdl = c->deadline;
    }
    int last = 0;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        int fd = socket(a->ai_family, a->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, a->ai_protocol);
        if (fd < 0) {
            last = errno;
            continue;
        }
        c->fd = fd;
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) {
            freeaddrinfo(res);
            return 0;
        }
        if (errno == EINPROGRESS) {
            int w = wait_fd(c, POLLOUT, cdl);
            int se = 0;
            socklen_t sl = sizeof(se);
            if (w == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &se, &sl) == 0 && se == 0) {
                freeaddrinfo(res);
                return 0;
            }
            if (w == -2) {
                close(fd);
                c->fd = -1;
                freeaddrinfo(res);
                return -1;
            }
            last = w == 0 ? ETIMEDOUT : se ? se : errno;
        } else {
            last = errno;
        }
        close(fd);
        c->fd = -1;
    }
    freeaddrinfo(res);
    return fail(c, "could not connect to %s port %s: %s", host, port, strerror(last ? last : ECONNREFUSED));
}

static ssize_t raw_send_some(conn_t *c, const unsigned char *p, size_t n)
{
    for (;;) {
        ssize_t w = send(c->fd, p, n, MSG_NOSIGNAL);
        if (w >= 0) {
            return w;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return fail(c, "send failed: %s", strerror(errno));
        }
        int r = wait_fd(c, POLLOUT, c->deadline);
        if (r != 1) {
            return timeout_or_abort(c, r);
        }
    }
}

/* >0 bytes, 0 EOF, -1 error. */
static ssize_t raw_recv_some(conn_t *c, unsigned char *p, size_t n)
{
    for (;;) {
        ssize_t r = recv(c->fd, p, n, 0);
        if (r >= 0) {
            return r;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return fail(c, "receive failed: %s", strerror(errno));
        }
        int w = wait_fd(c, POLLIN, c->deadline);
        if (w != 1) {
            return timeout_or_abort(c, w);
        }
    }
}

static const char *tls_err_text(int e)
{
    switch (e) {
    case BR_ERR_X509_NOT_TRUSTED:     return "server certificate is not trusted (unknown issuer; an intercepting proxy?)";
    case BR_ERR_X509_EXPIRED:         return "server certificate expired or not yet valid (check the system clock)";
    case BR_ERR_X509_BAD_SERVER_NAME: return "server certificate does not match the host name";
    case BR_ERR_BAD_VERSION:
    case BR_ERR_UNSUPPORTED_VERSION:  return "server does not support TLS 1.2";
    case BR_ERR_BAD_CIPHER_SUITE:     return "no common TLS cipher suite";
    case BR_ERR_IO:                   return "connection failed during TLS";
    default:                          return NULL;
    }
}

/*
 * TLS profile: TLS 1.2, forward-secret ECDHE with AEAD only (what LLM APIs
 * serve), RSA or ECDSA server certificates. One constant-time implementation
 * of each primitive, instead of br_ssl_client_init_full's everything-and-
 * runtime-pick, keeps the binary small. The CA anchors are compiled in.
 */
static void tls_client_init(br_ssl_client_context *cc, br_x509_minimal_context *xc)
{
    static const uint16_t suites[] = {
        BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,       BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
        BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,       BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
        BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    };
    static const br_hash_class *const hashes[] = {&br_sha1_vtable, &br_sha224_vtable, &br_sha256_vtable,
                                                  &br_sha384_vtable, &br_sha512_vtable};
    br_ssl_engine_context *e = &cc->eng;
    br_ssl_client_zero(cc);
    br_ssl_engine_set_versions(e, BR_TLS12, BR_TLS12);
    br_ssl_engine_set_suites(e, suites, sizeof(suites) / sizeof(suites[0]));
    br_x509_minimal_init(xc, &br_sha256_vtable, TAs, TAs_NUM);
    for (int id = br_sha1_ID; id <= br_sha512_ID; id++) {
        br_ssl_engine_set_hash(e, id, hashes[id - br_sha1_ID]);
        br_x509_minimal_set_hash(xc, id, hashes[id - br_sha1_ID]);
    }
    br_ssl_engine_set_ec(e, &br_ec_all_m31);
    br_ssl_engine_set_ecdsa(e, br_ecdsa_i31_vrfy_asn1);
    br_ssl_engine_set_rsavrfy(e, br_rsa_i31_pkcs1_vrfy);
    br_x509_minimal_set_rsa(xc, br_rsa_i31_pkcs1_vrfy);
    br_x509_minimal_set_ecdsa(xc, &br_ec_all_m31, br_ecdsa_i31_vrfy_asn1);
    br_ssl_engine_set_x509(e, &xc->vtable);
    br_ssl_engine_set_prf_sha256(e, &br_tls12_sha256_prf);
    br_ssl_engine_set_prf_sha384(e, &br_tls12_sha384_prf);
    br_ssl_engine_set_gcm(e, &br_sslrec_in_gcm_vtable, &br_sslrec_out_gcm_vtable);
    br_ssl_engine_set_aes_ctr(e, &br_aes_ct64_ctr_vtable);
    br_ssl_engine_set_ghash(e, &br_ghash_ctmul64);
    br_ssl_engine_set_chapol(e, &br_sslrec_in_chapol_vtable, &br_sslrec_out_chapol_vtable);
    br_ssl_engine_set_chacha20(e, &br_chacha20_ct_run);
    br_ssl_engine_set_poly1305(e, &br_poly1305_ctmul_run);
}

/* Drive the TLS engine until one of `want` (BR_SSL_SENDAPP / BR_SSL_RECVAPP) is ready. */
static int tls_run(conn_t *c, unsigned want)
{
    br_ssl_engine_context *e = &c->cc.eng;
    for (;;) {
        unsigned st = br_ssl_engine_current_state(e);
        if (st & BR_SSL_CLOSED) {
            int le = br_ssl_engine_last_error(e);
            if (le == BR_ERR_OK) {
                return 1; /* clean close_notify */
            }
            const char *t = tls_err_text(le);
            return t ? fail(c, "TLS: %s", t) : fail(c, "TLS error %d", le);
        }
        if (st & BR_SSL_SENDREC) {
            size_t len;
            unsigned char *buf = br_ssl_engine_sendrec_buf(e, &len);
            ssize_t w = raw_send_some(c, buf, len);
            if (w < 0) {
                br_ssl_engine_close(e);
                return -1;
            }
            br_ssl_engine_sendrec_ack(e, (size_t)w);
            continue;
        }
        if (st & want) {
            return 0;
        }
        if (st & BR_SSL_RECVREC) {
            size_t len;
            unsigned char *buf = br_ssl_engine_recvrec_buf(e, &len);
            ssize_t r = raw_recv_some(c, buf, len);
            if (r < 0) {
                return -1;
            }
            if (r == 0) {
                return 2; /* peer closed the socket without close_notify */
            }
            br_ssl_engine_recvrec_ack(e, (size_t)r);
            continue;
        }
        return fail(c, "TLS engine stalled");
    }
}

static int conn_write(conn_t *c, const char *p, size_t n)
{
    if (!c->tls) {
        while (n) {
            ssize_t w = raw_send_some(c, (const unsigned char *)p, n);
            if (w < 0) {
                return -1;
            }
            p += w;
            n -= (size_t)w;
        }
        return 0;
    }
    br_ssl_engine_context *e = &c->cc.eng;
    while (n) {
        int r = tls_run(c, BR_SSL_SENDAPP);
        if (r != 0) {
            return r < 0 ? -1 : fail(c, "connection closed while sending");
        }
        size_t len;
        unsigned char *buf = br_ssl_engine_sendapp_buf(e, &len);
        size_t k = n < len ? n : len;
        memcpy(buf, p, k);
        br_ssl_engine_sendapp_ack(e, k);
        p += k;
        n -= k;
    }
    br_ssl_engine_flush(e, 0);
    return 0;
}

/* >0 bytes, 0 EOF, -1 error. */
static ssize_t conn_read(conn_t *c, char *p, size_t n)
{
    if (!c->tls) {
        return raw_recv_some(c, (unsigned char *)p, n);
    }
    br_ssl_engine_context *e = &c->cc.eng;
    int r = tls_run(c, BR_SSL_RECVAPP);
    if (r != 0) {
        return r < 0 ? -1 : 0;
    }
    size_t len;
    unsigned char *buf = br_ssl_engine_recvapp_buf(e, &len);
    size_t k = n < len ? n : len;
    memcpy(p, buf, k);
    br_ssl_engine_recvapp_ack(e, k);
    return (ssize_t)k;
}

/* Read until the blank line ending the headers; extra bytes stay in *b after *hdr_len. */
static int read_headers(conn_t *c, sb_t *b, size_t *hdr_len)
{
    char buf[4096];
    for (;;) {
        char *e = b->p ? strstr(b->p, "\r\n\r\n") : NULL;
        if (e) {
            *hdr_len = (size_t)(e - b->p) + 4;
            return 0;
        }
        if (b->len > MAX_HEADERS) {
            return fail(c, "response headers too large");
        }
        ssize_t r = conn_read(c, buf, sizeof(buf));
        if (r < 0) {
            return -1;
        }
        if (r == 0) {
            return fail(c, b->len ? "connection closed in the middle of the response" : "empty reply from server");
        }
        if (sb_add(b, buf, (size_t)r)) {
            return fail(c, "out of memory");
        }
    }
}

/* Value of header `name` inside the header block, or NULL (points into hdr; ends at \r). */
static const char *header_val(const char *hdr, size_t hl, const char *name, size_t *vl)
{
    size_t nl = strlen(name);
    for (const char *l = memchr(hdr, '\n', hl); l && (size_t)(l - hdr) + 1 < hl; l = memchr(l + 1, '\n', hl - (size_t)(l + 1 - hdr))) {
        const char *s = l + 1;
        if ((size_t)(hdr + hl - s) > nl && strncasecmp(s, name, nl) == 0 && s[nl] == ':') {
            s += nl + 1;
            while (*s == ' ' || *s == '\t') {
                s++;
            }
            const char *e = strchr(s, '\r');
            *vl = e ? (size_t)(e - s) : strlen(s);
            return s;
        }
    }
    return NULL;
}

/* Chunked transfer decoding over the raw body bytes; 1 complete, 0 need more, -1 malformed. */
static int dechunk(const char *raw, size_t n, sb_t *out)
{
    size_t i = 0;
    out->len = 0;
    for (;;) {
        const char *nl = memchr(raw + i, '\n', n - i);
        if (!nl) {
            return 0;
        }
        char *endp;
        unsigned long sz = strtoul(raw + i, &endp, 16);
        if (endp == raw + i) {
            return -1;
        }
        i = (size_t)(nl - raw) + 1;
        if (sz == 0) {
            /* trailers until an empty line */
            for (;;) {
                const char *tl = memchr(raw + i, '\n', n - i);
                if (!tl) {
                    return 0;
                }
                int empty = tl == raw + i || (tl == raw + i + 1 && raw[i] == '\r');
                i = (size_t)(tl - raw) + 1;
                if (empty) {
                    return 1;
                }
            }
        }
        if (sz > MAX_OUT || n - i < sz + 2) {
            return sz > MAX_OUT ? -1 : 0;
        }
        if (sb_add(out, raw + i, sz)) {
            return -1;
        }
        i += sz;
        i += raw[i] == '\r' ? 2 : 1;
    }
}

/* ---- request ---------------------------------------------------------- */

void http_resp_free(http_resp_t *r)
{
    if (!r) {
        return;
    }
    free(r->body);
    free(r->err);
    r->body = r->err = NULL;
    r->status = 0;
}

/* Through an HTTP proxy: CONNECT host:port, expect 2xx. */
static int proxy_connect(conn_t *c, const url_t *px, const url_t *u)
{
    sb_t rq = {0};
    int ok = sb_fmt(&rq, "CONNECT %s:%s HTTP/1.1\r\nHost: %s:%s\r\n", u->host, u->port, u->host, u->port);
    if (px->user[0]) {
        char cred[260], b64[360];
        snprintf(cred, sizeof(cred), "%s:%s", px->user, px->pass);
        base64((const unsigned char *)cred, strlen(cred), b64);
        ok |= sb_fmt(&rq, "Proxy-Authorization: Basic %s\r\n", b64);
        explicit_bzero(cred, sizeof(cred));
        explicit_bzero(b64, sizeof(b64));
    }
    ok |= sb_add(&rq, "\r\n", 2);
    int rc = ok ? fail(c, "out of memory") : conn_write(c, rq.p, rq.len);
    sb_wipe(&rq);
    if (rc) {
        return -1;
    }
    sb_t rs = {0};
    size_t hl = 0;
    rc = read_headers(c, &rs, &hl);
    int st = rc == 0 && rs.len > 12 ? atoi(rs.p + 9) : 0;
    if (rc == 0 && (st < 200 || st > 299)) {
        rc = fail(c, "proxy %s refused the tunnel (HTTP %d)", px->host, st);
    }
    free(rs.p);
    return rc;
}

int http_request(const char *url, const char *bearer, const char *body,
                 int timeout_s, http_resp_t *out)
{
    return http_request_stream(url, bearer, body, timeout_s, NULL, NULL, out);
}

int http_request_stream(const char *url, const char *bearer, const char *body, int timeout_s,
                        http_body_fn on_body, void *ud, http_resp_t *out)
{
    memset(out, 0, sizeof(*out));
    out->body = strdup("");
    if (!url || has_ctl(url) || (bearer && has_ctl(bearer))) {
        out->err = strdup("invalid characters in URL or credentials");
        return HTTP_ERR_XPORT;
    }
    url_t u;
    if (url_parse(url, &u, 0) != 0) {
        out->err = strdup("invalid URL");
        return HTTP_ERR_XPORT;
    }

    conn_t *c = calloc(1, sizeof(*c));
    if (!c) {
        out->err = strdup("out of memory");
        return HTTP_ERR_XPORT;
    }
    c->fd = -1;
    c->deadline = now_s() + (timeout_s > 0 ? timeout_s : 60);
    g_aborted = 0;

    sb_t rq = {0}, rs = {0}, dec = {0};
    int rc = -1;

    /* https via HTTPS_PROXY (an http:// proxy), unless NO_PROXY matches */
    const char *pe = u.tls ? getenv("https_proxy") : NULL;
    if (u.tls && (!pe || !*pe)) {
        pe = getenv("HTTPS_PROXY");
    }
    url_t px;
    int use_px = pe && *pe && !no_proxy_match(u.host) && url_parse(pe, &px, 1) == 0 && !px.tls;
    if (pe && *pe && !use_px && !no_proxy_match(u.host) && u.tls) {
        fail(c, "HTTPS_PROXY must be an http:// proxy URL");
        goto done;
    }
    if (tcp_connect(c, use_px ? px.host : u.host, use_px ? px.port : u.port) != 0) {
        goto done;
    }
    if (use_px && proxy_connect(c, &px, &u) != 0) {
        goto done;
    }
    if (u.tls) {
        tls_client_init(&c->cc, &c->xc);
        br_ssl_engine_set_buffer(&c->cc.eng, c->iobuf, sizeof(c->iobuf), 1);
        static const char *alpn[] = {"http/1.1"};
        br_ssl_engine_set_protocol_names(&c->cc.eng, alpn, 1);
        if (!br_ssl_client_reset(&c->cc, u.host, 0)) {
            fail(c, "TLS setup failed (%d)", br_ssl_engine_last_error(&c->cc.eng));
            goto done;
        }
        c->tls = 1;
    }

    int ok = sb_fmt(&rq, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: botcore\r\nAccept: %s\r\n"
                         "Connection: close\r\n",
                    body ? "POST" : "GET", u.path, u.authority,
                    on_body ? "text/event-stream, application/json" : "application/json");
    if (bearer && *bearer) {
        ok |= sb_add(&rq, "Authorization: Bearer ", 22);
        ok |= sb_add(&rq, bearer, strlen(bearer));
        ok |= sb_add(&rq, "\r\n", 2);
    }
    if (body) {
        ok |= sb_fmt(&rq, "Content-Type: application/json\r\nContent-Length: %zu\r\n", strlen(body));
    }
    ok |= sb_add(&rq, "\r\n", 2);
    if (body) {
        ok |= sb_add(&rq, body, strlen(body));
    }
    if (ok) {
        fail(c, "out of memory");
        goto done;
    }
    int wr = conn_write(c, rq.p, rq.len);
    sb_wipe(&rq); /* holds the API key */
    if (wr != 0) {
        goto done;
    }

    /* status line + headers (skipping 1xx interim responses) */
    size_t hl = 0;
    int status = 0;
    for (;;) {
        if (read_headers(c, &rs, &hl) != 0) {
            goto done;
        }
        if (rs.len < 12 || strncmp(rs.p, "HTTP/1.", 7) != 0) {
            fail(c, "malformed response from server");
            goto done;
        }
        status = atoi(rs.p + 9);
        if (status >= 200 || status < 100) {
            break;
        }
        memmove(rs.p, rs.p + hl, rs.len - hl + 1);
        rs.len -= hl;
    }

    size_t vl = 0;
    const char *te = header_val(rs.p, hl, "Transfer-Encoding", &vl);
    int chunked = te && memmem(te, vl, "chunked", 7);
    const char *cl = header_val(rs.p, hl, "Content-Length", &vl);
    long long clen = cl && !chunked ? strtoll(cl, NULL, 10) : -1;
    if (clen > (long long)MAX_OUT) {
        fail(c, "response too large");
        goto done;
    }

    char buf[16384];
    size_t sent = 0; /* body bytes already passed to on_body */
    if (status < 200 || status >= 300) {
        on_body = NULL; /* errors are read whole */
    }
    for (;;) {
        size_t have = rs.len - hl;
        int d = 0;
        if (chunked) {
            d = dechunk(rs.p + hl, have, &dec);
            if (d < 0) {
                fail(c, "malformed chunked response");
                goto done;
            }
        }
        if (on_body) {
            const char *bp = chunked ? dec.p : rs.p + hl;
            size_t bl = chunked ? dec.len : clen >= 0 && have > (size_t)clen ? (size_t)clen : have;
            if (bl > sent) {
                on_body(ud, bp + sent, bl - sent);
                sent = bl;
            }
        }
        if ((clen >= 0 && have >= (size_t)clen) || d == 1) {
            break;
        }
        if (have > MAX_OUT) {
            fail(c, "response too large");
            goto done;
        }
        ssize_t r = conn_read(c, buf, sizeof(buf));
        if (r < 0) {
            goto done;
        }
        if (r == 0) {
            if (clen >= 0 || chunked) {
                fail(c, "connection closed before the response was complete");
                goto done;
            }
            break; /* body delimited by close */
        }
        if (sb_add(&rs, buf, (size_t)r)) {
            fail(c, "out of memory");
            goto done;
        }
    }

    free(out->body);
    if (chunked) {
        out->body = dec.p ? dec.p : strdup("");
        dec.p = NULL;
    } else {
        size_t bl = clen >= 0 ? (size_t)clen : rs.len - hl;
        out->body = malloc(bl + 1);
        if (out->body) {
            memcpy(out->body, rs.p + hl, bl);
            out->body[bl] = '\0';
        }
    }
    if (!out->body) {
        out->body = strdup("");
        fail(c, "out of memory");
        goto done;
    }
    out->status = status;
    rc = 0;

done:
    sb_wipe(&rq);
    free(rs.p);
    free(dec.p);
    if (c->fd >= 0) {
        close(c->fd);
    }
    int aborted = c->aborted || g_aborted;
    if (rc != 0 && !aborted) {
        out->err = strdup(c->err[0] ? c->err : "request failed");
    }
    explicit_bzero(c, sizeof(*c)); /* TLS buffers held the request (API key) */
    free(c);
    if (aborted && rc != 0) {
        return HTTP_ERR_ABORT;
    }
    return rc == 0 ? HTTP_OK : HTTP_ERR_XPORT;
}
