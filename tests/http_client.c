/* Test driver for botcore's HTTP client: http_client URL [BEARER [JSON_BODY]]
 * Prints "rc=<rc> status=<status> err=<err>" then the body. Built by tests/run.sh. */
#include "http.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s URL [BEARER [JSON_BODY]]\n", argv[0]);
        return 2;
    }
    http_resp_t r;
    int rc = http_request(argv[1], argc > 2 && *argv[2] ? argv[2] : NULL, argc > 3 ? argv[3] : NULL, 20, &r);
    printf("rc=%d status=%d err=%s\n%s\n", rc, r.status, r.err ? r.err : "-", r.body);
    http_resp_free(&r);
    return 0;
}
