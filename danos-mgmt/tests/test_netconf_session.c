/*
 * Test: transport-independent NETCONF session engine
 *
 * Drives netconf_session_new/run over pipes so hello parsing, pipelined
 * frames, malformed framing and write/read failures are covered directly.
 */
#include "../src/netconf/netconf_session.h"
#include "../src/netconf/netconf_datastore.h"
#include <danos/security/rbac.h>
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define EOM "]]>]]>"

#define HELLO10 \
    "<hello xmlns=\"urn:ietf:params:netconf:xmlns:base:1.0\">" \
    "<capabilities><capability>urn:ietf:params:netconf:base:1.0" \
    "</capability></capabilities></hello>"
#define HELLO11 \
    "<hello xmlns=\"urn:ietf:params:netconf:xmlns:base:1.0\">" \
    "<capabilities><capability>urn:ietf:params:netconf:base:1.0" \
    "</capability><capability>urn:ietf:params:netconf:base:1.1" \
    "</capability></capabilities></hello>"


static void write_all(int fd, const char *s)
{
    size_t len = strlen(s), off = 0;
    while (off < len) {
        ssize_t n = write(fd, s + off, len - off);
        assert(n > 0);
        off += (size_t)n;
    }
}

static void test_valid_session(void)
{
    int in[2], out[2];
    assert(pipe(in) == 0 && pipe(out) == 0);
    write_all(in[1], HELLO10 EOM
        "<rpc><get-config><source><running/></source></get-config></rpc>" EOM
        "<rpc><close-session/></rpc>" EOM);
    close(in[1]);

    netconf_session_t *s = netconf_session_new(in[0], out[1], "tester",
                                               DANOS_ROLE_ADMIN);
    assert(s);
    assert(netconf_session_id(s) != 0);

    assert(netconf_session_run(s) == 0);
    assert(netconf_session_close_requested(s));

    close(out[1]);
    char replies[8192];
    size_t used = 0;
    for (;;) {
        ssize_t n = read(out[0], replies + used, sizeof(replies) - used - 1);
        if (n <= 0) break;
        used += (size_t)n;
    }
    replies[used] = '\0';
    assert(strstr(replies, "<hello"));
    assert(strstr(replies, "<data>"));
    assert(strstr(replies, "<ok/>"));

    netconf_session_free(s);
    close(in[0]);
    close(out[0]);
    printf("[PASS] valid hello, pipelined get-config and close-session\n");
}

static void test_missing_base10(void)
{
    int in[2], out[2];
    assert(pipe(in) == 0 && pipe(out) == 0);
    write_all(in[1], "<hello><capabilities/></hello>" EOM);
    close(in[1]);
    assert(netconf_session_new(in[0], out[1], "tester", DANOS_ROLE_ADMIN)
           == NULL);
    close(in[0]);
    close(out[0]);
    close(out[1]);
    printf("[PASS] client hello without base 1.0 is rejected\n");
}

static void test_bad_pipelined_frame(void)
{
    int in[2], out[2];
    assert(pipe(in) == 0 && pipe(out) == 0);
    write_all(in[1], HELLO11 EOM "\n#0\n\n##\n");
    close(in[1]);
    assert(netconf_session_new(in[0], out[1], "tester", DANOS_ROLE_ADMIN)
           == NULL);
    close(in[0]);
    close(out[0]);
    close(out[1]);
    printf("[PASS] malformed pipelined chunked frame is rejected\n");
}

static void test_eof_before_hello(void)
{
    int in[2], out[2];
    assert(pipe(in) == 0 && pipe(out) == 0);
    close(in[1]);
    assert(netconf_session_new(in[0], out[1], "tester", DANOS_ROLE_ADMIN)
           == NULL);
    close(in[0]);
    close(out[0]);
    close(out[1]);
    printf("[PASS] EOF before the client hello is rejected\n");
}

static void test_hello_write_failure(void)
{
    int in[2], out[2];
    assert(pipe(in) == 0 && pipe(out) == 0);
    close(in[0]);                       /* reading end gone: hello write fails */
    assert(netconf_session_new(in[0], out[1], "tester", DANOS_ROLE_ADMIN)
           == NULL);
    close(in[1]);
    close(out[0]);
    close(out[1]);
    printf("[PASS] server-hello write failure is reported\n");
}

struct oversized_arg { int fd; };

static void *write_large_hello(void *arg)
{
    struct oversized_arg *a = arg;
    char chunk[4096];
    memset(chunk, 'x', sizeof(chunk));
    for (int i = 0; i < 80; i++) {      /* 320 KiB, no EOM */
        if (write(a->fd, chunk, sizeof(chunk)) <= 0) break;
    }
    return NULL;
}

static void test_oversized_hello(void)
{
    int in[2], out[2];
    assert(pipe(in) == 0 && pipe(out) == 0);
    struct oversized_arg arg = { .fd = in[1] };
    pthread_t th;
    assert(pthread_create(&th, NULL, write_large_hello, &arg) == 0);
    assert(netconf_session_new(in[0], out[1], "tester", DANOS_ROLE_ADMIN)
           == NULL);
    close(in[0]);
    pthread_join(th, NULL);
    close(in[1]);
    close(out[0]);
    close(out[1]);
    printf("[PASS] oversized client hello is bounded and rejected\n");
}

static void test_run_framing_error(void)
{
    int in[2], out[2];
    assert(pipe(in) == 0 && pipe(out) == 0);
    write_all(in[1], HELLO11 EOM);
    netconf_session_t *s = netconf_session_new(in[0], out[1], "tester",
                                               DANOS_ROLE_ADMIN);
    assert(s);
    write_all(in[1], "\n#0\n\n##\n");
    close(in[1]);
    assert(netconf_session_run(s) == -1);
    netconf_session_free(s);
    close(in[0]);
    close(out[0]);
    close(out[1]);
    printf("[PASS] malformed runtime frame fails the session\n");
}

static void test_valid_base11(void)
{
    int in[2], out[2];
    assert(pipe(in) == 0 && pipe(out) == 0);
    const char *rpc =
        "<rpc><get-config><source><running/></source></get-config></rpc>";
    char chunked[512];
    snprintf(chunked, sizeof(chunked), "\n#%zx\n%s\n##\n", strlen(rpc), rpc);
    char request[1024];
    snprintf(request, sizeof(request), HELLO11 EOM "%s", chunked);
    write_all(in[1], request);
    close(in[1]);

    netconf_session_t *s = netconf_session_new(in[0], out[1], "tester",
                                               DANOS_ROLE_ADMIN);
    assert(s);
    assert(netconf_session_run(s) == 0);
    close(out[1]);
    char replies[8192];
    size_t used = 0;
    for (;;) {
        ssize_t n = read(out[0], replies + used, sizeof(replies) - used - 1);
        if (n <= 0) break;
        used += (size_t)n;
    }
    replies[used] = '\0';
    assert(strstr(replies, "<data>"));
    assert(strstr(replies, "\n#"));
    netconf_session_free(s);
    close(in[0]);
    close(out[0]);
    printf("[PASS] base 1.1 chunked request and reply\n");
}

static void test_reply_write_failure(void)
{
    int in[2], out[2];
    assert(pipe(in) == 0 && pipe(out) == 0);
    write_all(in[1], HELLO10 EOM);
    netconf_session_t *s = netconf_session_new(in[0], out[1], "tester",
                                               DANOS_ROLE_ADMIN);
    assert(s);
    close(out[0]);              /* reply write now fails with EPIPE */
    write_all(in[1], "<rpc><get-config><source><running/></source>"
                     "</get-config></rpc>" EOM);
    close(in[1]);
    close(out[1]);
    assert(netconf_session_run(s) == -1);
    netconf_session_free(s);
    close(in[0]);
    printf("[PASS] reply write failure fails the session\n");
}

static void test_invalid_read_fd(void)
{
    int out[2];
    assert(pipe(out) == 0);
    assert(netconf_session_new(-1, out[1], "tester", DANOS_ROLE_ADMIN)
           == NULL);
    close(out[0]);
    close(out[1]);
    printf("[PASS] invalid client-read descriptor is rejected\n");
}

static void test_null_api(void)
{
    assert(netconf_session_run(NULL) == -1);
    assert(netconf_session_id(NULL) == 0);
    assert(!netconf_session_close_requested(NULL));
    netconf_session_free(NULL);
    printf("[PASS] NULL session APIs are safe\n");
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    char path[128];
    snprintf(path, sizeof(path), "/tmp/danos-ncsession-%ld.db",
             (long)getpid());
    unlink(path);
    setenv("DANOS_NETCONF_STARTUP", path, 1);
    netconf_ds_init(path);

    test_valid_session();
    test_missing_base10();
    test_bad_pipelined_frame();
    test_eof_before_hello();
    test_hello_write_failure();
    test_oversized_hello();
    test_run_framing_error();
    test_valid_base11();
    test_reply_write_failure();
    test_invalid_read_fd();
    test_null_api();

    /* A failed datastore init (corrupt running) must not open a session. */
    netconf_ds_fini();
    char corrupt[192];
    snprintf(corrupt, sizeof(corrupt), "%s.running", path);
    FILE *f = fopen(corrupt, "wb");
    assert(f != NULL);
    assert(fwrite("bad", 1, 3, f) == 3);
    fclose(f);
    netconf_ds_init(path);
    assert(!netconf_ds_ready());
    int cin[2], cout[2];
    assert(pipe(cin) == 0 && pipe(cout) == 0);
    assert(netconf_session_new(cin[0], cout[1], "tester", DANOS_ROLE_ADMIN)
           == NULL);
    close(cin[0]); close(cin[1]); close(cout[0]); close(cout[1]);
    unlink(corrupt);
    unsetenv("DANOS_NETCONF_ALLOW_CORRUPT_RUNNING");

    netconf_ds_fini();
    char aux[160];
    snprintf(aux, sizeof(aux), "%s.running", path); unlink(aux);
    snprintf(aux, sizeof(aux), "%s.confirmed", path); unlink(aux);
    unlink(path);
    printf("=== test_netconf_session: ALL PASSED ===\n");
    return 0;
}
