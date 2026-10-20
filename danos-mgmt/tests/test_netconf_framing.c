/*
 * Test: NETCONF message framing (RFC 6242)
 */
#include "../src/netconf/netconf_framing.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int count;
    char last[512];
    size_t last_len;
} sink_t;

static int sink_cb(const char *msg, size_t len, void *user)
{
    sink_t *s = user;
    s->count++;
    s->last_len = len;
    if (len < sizeof(s->last)) {
        memcpy(s->last, msg, len);
        s->last[len] = '\0';
    }
    return 0;
}

static void feed(netconf_frame_decoder_t *d, const char *s, sink_t *sink)
{
    assert(netconf_frame_feed(d, s, strlen(s), sink_cb, sink) == 0);
}

int test_framing_eom(void)
{
    sink_t sink = {0};
    netconf_frame_decoder_t *d = netconf_frame_decoder_new(false);
    assert(d != NULL);

    /* Delimiter split across reads. */
    feed(d, "<hello", &sink);
    assert(sink.count == 0);
    feed(d, "/>]]>", &sink);
    assert(sink.count == 0);
    feed(d, "]]>", &sink);
    assert(sink.count == 1);
    assert(strcmp(sink.last, "<hello/>") == 0);
    assert(sink.last_len == 8);

    /* Two messages in one read, and a pipelined prefix afterwards. */
    feed(d, "<a/>]]>]]><b/>]]>]]><c", &sink);
    assert(sink.count == 3);
    assert(strcmp(sink.last, "<b/>") == 0);
    feed(d, "/>]]>]]>", &sink);
    assert(sink.count == 4);
    assert(strcmp(sink.last, "<c/>") == 0);

    netconf_frame_decoder_free(d);
    printf("[PASS] test_framing_eom: delimiter split and pipelined messages\n");
    return 0;
}

int test_framing_chunked(void)
{
    sink_t sink = {0};
    netconf_frame_decoder_t *d = netconf_frame_decoder_new(true);
    assert(d != NULL);

    feed(d, "\n#5\n<rpc>", &sink);
    assert(sink.count == 0);
    feed(d, "\n#6\n</rpc>\n##\n", &sink);
    assert(sink.count == 1);
    assert(strcmp(sink.last, "<rpc></rpc>") == 0);

    /* Byte-at-a-time delivery must produce the same message. */
    const char *frame = "\n#5\n<rpc>\n#6\n</rpc>\n##\n";
    for (const char *p = frame; *p; p++) {
        char one[2] = { *p, '\0' };
        feed(d, one, &sink);
    }
    assert(sink.count == 2);
    assert(strcmp(sink.last, "<rpc></rpc>") == 0);

    /* Back-to-back messages. */
    feed(d, "\n#1\na\n##\n\n#1\nb\n##\n", &sink);
    assert(sink.count == 4);
    assert(strcmp(sink.last, "b") == 0);

    netconf_frame_decoder_free(d);
    printf("[PASS] test_framing_chunked: chunks, reassembly and pipelining\n");
    return 0;
}

int test_framing_chunked_rejects(void)
{
    static const char *const bad[] = {
        "#3\nabc\n##\n",        /* no leading LF */
        "\n#0\n\n##\n",         /* zero-size chunk */
        "\n#g\n",               /* non-hex size */
        "\n#3\nabc##\n",        /* data not followed by LF */
        "\n#FFFFFFF\n",         /* declared chunk above the local limit */
        "\n#3\nabcX",           /* truncated chunk data, bad next byte */
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        sink_t sink = {0};
        netconf_frame_decoder_t *d = netconf_frame_decoder_new(true);
        assert(d != NULL);
        int rc = netconf_frame_feed(d, bad[i], strlen(bad[i]), sink_cb, &sink);
        assert(rc == -1);
        assert(sink.count == 0);
        netconf_frame_decoder_free(d);
    }

    printf("[PASS] test_framing_chunked_rejects: malformed framing fails\n");
    return 0;
}

int test_framing_encode(void)
{
    /* Base 1.0: message followed by the delimiter. */
    size_t len = 0;
    char *out = netconf_frame_encode(false, "<rpc/>", 6, &len);
    assert(out != NULL);
    assert(len == 6 + 6);
    assert(strcmp(out, "<rpc/>]]>]]>") == 0);
    free(out);

    sink_t sink = {0};
    netconf_frame_decoder_t *d = netconf_frame_decoder_new(false);
    out = netconf_frame_encode(false, "<rpc/>", 6, &len);
    assert(netconf_frame_feed(d, out, len, sink_cb, &sink) == 0);
    assert(sink.count == 1 && strcmp(sink.last, "<rpc/>") == 0);
    free(out);
    netconf_frame_decoder_free(d);

    /* Base 1.1: a message larger than the encoder's chunk size must split
     * into several chunks and still decode to the same bytes. */
    size_t big_len = 3u * 1024u * 1024u + 7u;
    char *big = malloc(big_len);
    assert(big != NULL);
    for (size_t i = 0; i < big_len; i++) big[i] = (char)('a' + (i % 26));

    d = netconf_frame_decoder_new(true);
    memset(&sink, 0, sizeof(sink));
    out = netconf_frame_encode(true, big, big_len, &len);
    assert(out != NULL);
    assert(len > big_len);
    assert(netconf_frame_feed(d, out, len, sink_cb, &sink) == 0);
    assert(sink.count == 1);
    assert(sink.last_len == big_len);

    free(out);
    netconf_frame_decoder_free(d);
    free(big);

    printf("[PASS] test_framing_encode: both framings round-trip\n");
    return 0;
}

int test_framing_base11_detection(void)
{
    assert(netconf_hello_advertises_base11(
        "<capability>urn:ietf:params:netconf:base:1.1</capability>"));
    assert(!netconf_hello_advertises_base11(
        "<capability>urn:ietf:params:netconf:base:1.0</capability>"));
    assert(!netconf_hello_advertises_base11(NULL));

    printf("[PASS] test_framing_base11_detection: hello capability parsed\n");
    return 0;
}

int main(void)
{
    int failed = 0;
    if (test_framing_eom() != 0) failed++;
    if (test_framing_chunked() != 0) failed++;
    if (test_framing_chunked_rejects() != 0) failed++;
    if (test_framing_encode() != 0) failed++;
    if (test_framing_base11_detection() != 0) failed++;
    printf("=== netconf_framing_test: %s ===\n",
           failed == 0 ? "ALL PASSED" : "FAILURES");
    return failed;
}
