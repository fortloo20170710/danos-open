/*
 * Test: VPP Binary API Client (D1)
 * Verify mock mode operation.
 */
#include "../src/api/vpp_api.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

int test_mock_mode(void)
{
    danos_vpp_api_init();
    danos_vpp_api_enable_mock();

    assert(danos_vpp_api_is_connected() == true);

    /* Send a message */
    uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    assert(danos_vpp_api_send(100, payload, sizeof(payload)) == 0);

    /* Verify last mock message */
    uint16_t msg_id;
    uint32_t msg_size;
    danos_vpp_api_get_last_mock_msg(&msg_id, &msg_size);
    assert(msg_id == 100);
    assert(msg_size == sizeof(payload));

    /* Receive (mock returns 6-byte header) */
    uint8_t buf[64];
    int n = danos_vpp_api_recv(buf, sizeof(buf));
    assert(n == 6);

    /* Stats */
    uint64_t sent, received, connects, reconnects;
    danos_vpp_api_get_stats(&sent, &received, &connects, &reconnects);
    assert(sent == 1);
    assert(received == 1);

    printf("[PASS] test_mock_mode: mock send/recv works\n");
    return 0;
}

int test_stat_query(void)
{
    danos_vpp_api_init();
    danos_vpp_api_enable_mock();

    /* Mock stat query returns hash of name */
    uint64_t v1 = danos_vpp_api_stat_query("/if/0/rx-packets");
    uint64_t v2 = danos_vpp_api_stat_query("/if/0/rx-packets");
    uint64_t v3 = danos_vpp_api_stat_query("/if/0/tx-packets");
    assert(v1 == v2);   /* same name → same value */
    assert(v1 != v3);   /* different name → different value */
    assert(v1 != 0);

    /* NULL name */
    assert(danos_vpp_api_stat_query(NULL) == 0);

    printf("[PASS] test_stat_query: stat segment query works\n");
    return 0;
}

int test_disconnect(void)
{
    danos_vpp_api_init();
    danos_vpp_api_enable_mock();
    assert(danos_vpp_api_is_connected() == true);

    danos_vpp_api_disconnect();
    assert(danos_vpp_api_is_connected() == false);

    printf("[PASS] test_disconnect: disconnect works\n");
    return 0;
}

int test_stat_named_counters(void)
{
    danos_vpp_api_init();
    danos_vpp_api_enable_mock();
    danos_vpp_api_stat_reset();

    /* Register named counters (simulating VPP stat segment paths) */
    danos_vpp_api_stat_set("/sys/node/ip4-input", 1000000);
    danos_vpp_api_stat_set("/sys/node/ip4-output", 950000);
    danos_vpp_api_stat_set("/eth0/tx", 500000);
    danos_vpp_api_stat_set("/eth0/rx", 510000);
    danos_vpp_api_stat_set("/err/ip4-input/unknown", 42);

    /* Query registered counters */
    assert(danos_vpp_api_stat_query("/sys/node/ip4-input") == 1000000);
    assert(danos_vpp_api_stat_query("/sys/node/ip4-output") == 950000);
    assert(danos_vpp_api_stat_query("/eth0/tx") == 500000);
    assert(danos_vpp_api_stat_query("/eth0/rx") == 510000);
    assert(danos_vpp_api_stat_query("/err/ip4-input/unknown") == 42);

    /* Update existing counter */
    danos_vpp_api_stat_set("/eth0/tx", 600000);
    assert(danos_vpp_api_stat_query("/eth0/tx") == 600000);

    /* List counters */
    int count = danos_vpp_api_stat_list(NULL, NULL, 100);
    assert(count == 5);

    uint64_t values[10];
    const char *names[10];
    count = danos_vpp_api_stat_list(names, values, 10);
    assert(count == 5);

    /* Unregistered counter falls back to hash */
    uint64_t v = danos_vpp_api_stat_query("/not/registered");
    assert(v != 0);

    /* Reset */
    danos_vpp_api_stat_reset();
    assert(danos_vpp_api_stat_list(NULL, NULL, 100) == 0);

    printf("[PASS] test_stat_named_counters: named stat registration/query\n");
    return 0;
}

/* The binary API context is shared by several daemon threads. Run the mock
 * surface from many threads at once: functional assertions catch deadlocks
 * and lost updates, and a ThreadSanitizer build catches unsynchronised
 * access to the context, message table and stat table. */
#define CONC_THREADS 8
#define CONC_ITERATIONS 1200

static void *concurrent_worker(void *arg)
{
    unsigned id = (unsigned)(uintptr_t)arg;
    char name[64];
    for (int i = 0; i < CONC_ITERATIONS; i++) {
        snprintf(name, sizeof(name), "/t%u/counter/%d", id % 4, i % 8);
        switch (i % 6) {
        case 0:
            danos_vpp_api_stat_set(name, (uint64_t)i);
            break;
        case 1:
            (void)danos_vpp_api_stat_query(name);
            break;
        case 2:
            (void)danos_vpp_api_stat_list(NULL, NULL, 64);
            break;
        case 3:
            assert(danos_vpp_api_send((uint16_t)(100 + id), name, 8) == 0);
            break;
        case 4: {
            uint8_t buf[16];
            assert(danos_vpp_api_recv(buf, sizeof(buf)) == 6);
            break;
        }
        default: {
            uint64_t a, b, c, d;
            uint16_t mid = 0;
            danos_vpp_api_get_stats(&a, &b, &c, &d);
            (void)danos_vpp_api_lookup_msg_id("show_version", &mid);
            (void)danos_vpp_api_is_connected();
            break;
        }
        }
    }
    return NULL;
}

int test_mock_concurrent(void)
{
    danos_vpp_api_init();
    danos_vpp_api_enable_mock();
    danos_vpp_api_stat_reset();

    uint64_t expect_sent = 0, expect_received = 0;
    for (int i = 0; i < CONC_ITERATIONS; i++) {
        if (i % 6 == 3) expect_sent++;
        if (i % 6 == 4) expect_received++;
    }

    pthread_t threads[CONC_THREADS];
    for (unsigned i = 0; i < CONC_THREADS; i++)
        assert(pthread_create(&threads[i], NULL, concurrent_worker,
                              (void *)(uintptr_t)i) == 0);
    for (unsigned i = 0; i < CONC_THREADS; i++)
        assert(pthread_join(threads[i], NULL) == 0);

    /* No send or receive may be lost to an interleaving. */
    uint64_t sent, received, connects, reconnects;
    danos_vpp_api_get_stats(&sent, &received, &connects, &reconnects);
    assert(sent == expect_sent * CONC_THREADS);
    assert(received == expect_received * CONC_THREADS);
    assert(danos_vpp_api_is_connected() == true);

    printf("[PASS] test_mock_concurrent: %d threads, %llu sends, %llu receives\n",
           CONC_THREADS, (unsigned long long)sent,
           (unsigned long long)received);
    return 0;
}

int main(void)
{
    int failed = 0;
    if (test_mock_mode() != 0) failed++;
    if (test_stat_query() != 0) failed++;
    if (test_stat_named_counters() != 0) failed++;
    if (test_mock_concurrent() != 0) failed++;
    if (test_disconnect() != 0) failed++;
    printf("=== vpp_api_test: %s ===\n",
           failed == 0 ? "ALL PASSED" : "FAILURES");
    return failed;
}
