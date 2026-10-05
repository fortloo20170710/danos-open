/*
 * Test: Configuration persistence (v0.3)
 *
 * Simulates a full restart cycle in one process:
 *   boot 1: enable persistence, create objects, mutate, delete one
 *   "crash": disable persistence (WAL on disk), drop the store
 *   boot 2: recreate store, recover(), verify state matches
 * plus torn-record tolerance and a reconciler run after recovery.
 */

#include <danos/core/persist.h>
#include <danos/core/wal.h>
#include <danos/core/object_registry.h>
#include <danos/core/reconciler.h>
#include <danos/dpa.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>

#define WAL_PATH "/tmp/danos-test-persist.wal"

static void fresh_boot(void)
{
    /* Emulate process start: no store, WAL on disk.
     *
     * A real crash would simply lose the store. Under a leak-checking
     * build (ASan/LSan) dropping the pointer orphans it, so free it here:
     * the allocation being discarded is exactly what this helper models,
     * and LSan cannot tell the difference. */
    if (g_default_store) {
        danos_object_store_destroy(g_default_store);
        g_default_store = NULL;
    }
    unlink(WAL_PATH);
}

int test_enable_and_log(void)
{
    fresh_boot();
    assert(danos_persist_enable(WAL_PATH) == 0);
    assert(danos_persist_is_enabled());

    /* create one interface via the DPA CRUD path */
    danos_tx_t tx;
    assert(danos_tx_begin(&tx, "persist-test", NULL) == DANOS_OK);
    danos_iface_t ifc;
    memset(&ifc, 0, sizeof(ifc));
    ifc.ifindex = 7;
    strcpy(ifc.name, "wan0");
    ifc.mtu = 9000;
    ifc.admin_up = true;
    assert(danos_iface_create(&tx, &ifc) == DANOS_OK);
    assert(danos_tx_prepare(&tx) == DANOS_OK);
    assert(danos_tx_validate(&tx) == DANOS_OK);
    assert(danos_tx_commit(&tx) == DANOS_OK);

    /* An aborted candidate must neither become visible nor enter the WAL. */
    danos_iface_t aborted = {0};
    aborted.ifindex = 9;
    strcpy(aborted.name, "abort0");
    aborted.mtu = 1500;
    assert(danos_tx_begin(&tx, "persist-abort", NULL) == DANOS_OK);
    assert(danos_iface_create(&tx, &aborted) == DANOS_OK);
    assert(danos_tx_abort(&tx) == DANOS_OK);

    /* Simulate a crash after a transactional WAL data record but before its
     * commit marker: recovery must not publish that incomplete transaction. */
    wal_ctx_t incomplete_wal;
    assert(danos_wal_init(&incomplete_wal, WAL_PATH) == 0);
    danos_iface_t incomplete = aborted;
    incomplete.ifindex = 10;
    strcpy(incomplete.name, "partial0");
    wal_record_t incomplete_rec = {0};
    incomplete_rec.magic = WAL_MAGIC;
    incomplete_rec.tx_id = UINT64_C(0xfedcba9876543210);
    incomplete_rec.op_type = WAL_OP_CREATE;
    incomplete_rec.obj_type = WAL_OBJ_IFACE;
    incomplete_rec.obj_id = incomplete.ifindex;
    incomplete_rec.data_len = sizeof(incomplete);
    incomplete_rec.data = (const uint8_t *)&incomplete;
    assert(danos_wal_append(&incomplete_wal, &incomplete_rec) == 0);
    assert(danos_wal_sync(&incomplete_wal) == 0);
    danos_wal_close(&incomplete_wal);

    /* update it */
    assert(danos_tx_begin(&tx, "persist-test", NULL) == DANOS_OK);
    ifc.mtu = 1500;
    assert(danos_iface_update(&tx, &ifc) == DANOS_OK);
    assert(danos_tx_prepare(&tx) == DANOS_OK);
    assert(danos_tx_validate(&tx) == DANOS_OK);
    assert(danos_tx_commit(&tx) == DANOS_OK);

    /* a second object then deleted */
    danos_iface_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.ifindex = 8;
    strcpy(tmp.name, "tmp0");
    tmp.mtu = 1500;
    assert(danos_tx_begin(&tx, "persist-test", NULL) == DANOS_OK);
    assert(danos_iface_create(&tx, &tmp) == DANOS_OK);
    assert(danos_tx_prepare(&tx) == DANOS_OK);
    assert(danos_tx_validate(&tx) == DANOS_OK);
    assert(danos_tx_commit(&tx) == DANOS_OK);
    assert(danos_tx_begin(&tx, "persist-test", NULL) == DANOS_OK);
    assert(danos_iface_delete(&tx, 8) == DANOS_OK);
    assert(danos_tx_prepare(&tx) == DANOS_OK);
    assert(danos_tx_validate(&tx) == DANOS_OK);
    assert(danos_tx_commit(&tx) == DANOS_OK);

    uint64_t logged = 0, recovered = 0;
    danos_persist_get_stats(&logged, &recovered);
    assert(logged == 4);   /* create + update + create + delete */

    printf("[PASS] test_enable_and_log\n");
    return 0;
}

int test_recover_after_restart(void)
{
    /* "crash": persistence off, store dropped (kept for the next boot) */
    danos_persist_disable();
    if (g_default_store) {
        danos_object_store_destroy(g_default_store);
        g_default_store = NULL;
    }

    /* boot 2: re-enable (same WAL), recover */
    assert(danos_persist_enable(WAL_PATH) == 0);
    uint64_t logged_before = 0, rec = 0;
    danos_persist_get_stats(&logged_before, &rec);
    int applied = danos_persist_recover();
    assert(applied >= 2);   /* wan0 create+update applied */

    /* wan0 survived with the LAST mutation (mtu 1500) */
    danos_tx_t tx;
    assert(danos_tx_begin(&tx, "verify", NULL) == DANOS_OK);
    danos_iface_t out;
    assert(danos_iface_read(&tx, 7, &out) == DANOS_OK);
    assert(strcmp(out.name, "wan0") == 0);
    assert(out.mtu == 1500);
    assert(danos_iface_read(&tx, 9, &out) == DANOS_ERR_NOT_FOUND);
    assert(danos_iface_read(&tx, 10, &out) == DANOS_ERR_NOT_FOUND);
    assert(danos_tx_abort(&tx) == DANOS_OK);

    /* tmp0 (deleted) must not come back */
    assert(danos_tx_begin(&tx, "verify-delete", NULL) == DANOS_OK);
    assert(danos_iface_read(&tx, 8, &out) == DANOS_ERR_NOT_FOUND);
    assert(danos_tx_abort(&tx) == DANOS_OK);

    /* store counts: exactly one iface */
    assert(danos_object_count(g_default_store, DANOS_OBJ_IFACE) == 1);

    /* recovery must not have re-logged into the WAL (no feedback loop) */
    uint64_t logged = 0, recovered = 0;
    danos_persist_get_stats(&logged, &recovered);
    assert(logged == logged_before);
    assert(recovered >= 2);

    printf("[PASS] test_recover_after_restart\n");
    return 0;
}

int test_reconcile_after_recovery(void)
{
    /* Recovered objects live in the default store; feed them into the
     * state store as DESIRED and run one reconcile pass. Nothing is
     * PROGRAMMED, so the pass must report every object as a diff. */
    danos_state_store_t *ss = danos_state_store_create();
    assert(ss);

    /* copy recovered default-store objects into desired */
    {
        /* iterate via public API: use count + read by probing ids we know */
        danos_tx_t tx;
        assert(danos_tx_begin(&tx, "recon", NULL) == DANOS_OK);
        danos_iface_t out;
        assert(danos_iface_read(&tx, 7, &out) == DANOS_OK);
        assert(danos_tx_abort(&tx) == DANOS_OK);
        assert(danos_state_set(ss, DANOS_STATE_DESIRED, DANOS_OBJ_IFACE, 7,
                               &out, sizeof(out)) == DANOS_OK);
    }

    danos_reconcile_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    assert(danos_reconciler_init(ss, &cfg) == 0);
    uint64_t diffs = danos_reconciler_run_once();
    assert(diffs >= 1);   /* wan0 desired, nothing programmed */
    danos_reconciler_fini();
    danos_state_store_destroy(ss);

    printf("[PASS] test_reconcile_after_recovery (diffs=%lu)\n",
           (unsigned long)diffs);
    return 0;
}

int test_torn_record_tolerance(void)
{
    danos_persist_disable();
    if (g_default_store) {
        danos_object_store_destroy(g_default_store);
        g_default_store = NULL;
    }

    /* append garbage to the WAL (simulated torn write) */
    FILE *f = fopen(WAL_PATH, "ab");
    assert(f);
    const char junk[] = "HALF-WRITTEN RECORD";
    fwrite(junk, 1, sizeof(junk) - 1, f);
    fclose(f);

    assert(danos_persist_enable(WAL_PATH) == 0);
    int applied = danos_persist_recover();
    /* valid records still recovered; junk discarded */
    assert(applied >= 2);

    danos_tx_t tx;
    assert(danos_tx_begin(&tx, "verify2", NULL) == DANOS_OK);
    danos_iface_t out;
    assert(danos_iface_read(&tx, 7, &out) == DANOS_OK);
    assert(danos_tx_abort(&tx) == DANOS_OK);

    danos_persist_disable();
    unlink(WAL_PATH);
    printf("[PASS] test_torn_record_tolerance\n");
    return 0;
}

/* Routes must be durable.
 *
 * A route's object id is a 64-bit FNV-1a hash of (vrf, prefix, protocol),
 * so it exceeds 32 bits for essentially every prefix. The WAL header used a
 * 32-bit obj_id and the mutation hook dropped anything above 0xFFFFFFFF, so
 * no route was ever written and none survived a restart. This checks a route
 * round-trips, and that its id genuinely needs more than 32 bits - otherwise
 * the test would pass even with the old truncation. */
int test_route_persistence(void)
{
    fresh_boot();
    assert(danos_persist_enable(WAL_PATH) == 0);

    danos_route_t r;
    memset(&r, 0, sizeof(r));
    r.vrf_id = 1;
    r.protocol = DANOS_ROUTE_PROTO_STATIC;
    r.prefix.addr.af = DANOS_AF_IPV4;
    r.prefix.prefix_len = 24;
    r.prefix.addr.addr[0] = 203; r.prefix.addr.addr[1] = 0;
    r.prefix.addr.addr[2] = 113; r.prefix.addr.addr[3] = 0;
    r.nhgroup_id = 42;
    r.metric = 7;
    r.admin_distance = 7;

    /* Recreate the key derivation to prove the id exceeds 32 bits. */
    uint64_t h = 14695981039346656037ULL;
    h ^= (uint64_t)r.vrf_id;             h *= 1099511628211ULL;
    for (int i = 0; i < 16; i++) { h ^= r.prefix.addr.addr[i]; h *= 1099511628211ULL; }
    h ^= r.prefix.prefix_len;            h *= 1099511628211ULL;
    h ^= (uint64_t)r.protocol;           h *= 1099511628211ULL;
    assert(h > 0xFFFFFFFFULL);   /* the bug this test guards needed this */

    danos_tx_t tx;
    assert(danos_tx_begin(&tx, "route", NULL) == DANOS_OK);
    assert(danos_route_create(&tx, &r) == DANOS_OK);
    assert(danos_tx_commit_atomic(&tx) == DANOS_OK);

    /* Simulate a restart: drop the store, keep the WAL. */
    assert(danos_object_count(g_default_store, DANOS_OBJ_ROUTE) == 1);
    danos_persist_disable();
    danos_object_store_destroy(g_default_store);
    g_default_store = NULL;

    assert(danos_persist_enable(WAL_PATH) == 0);
    int applied = danos_persist_recover();
    assert(applied >= 1);

    assert(danos_object_count(g_default_store, DANOS_OBJ_ROUTE) == 1);
    assert(danos_tx_begin(&tx, "verify", NULL) == DANOS_OK);
    danos_route_t out;
    assert(danos_route_read(&tx, 1, r.prefix, DANOS_ROUTE_PROTO_STATIC,
                            &out) == DANOS_OK);
    assert(out.prefix.prefix_len == 24);
    assert(out.prefix.addr.addr[0] == 203 && out.prefix.addr.addr[3] == 0);
    assert(out.nhgroup_id == 42);
    assert(out.metric == 7);
    assert(danos_tx_abort(&tx) == DANOS_OK);

    danos_object_store_destroy(g_default_store);
    g_default_store = NULL;
    printf("[PASS] test_route_persistence: 64-bit route id survives restart\n");
    return 0;
}

int main(void)
{
    int failed = 0;
    if (test_enable_and_log() != 0) failed++;
    if (test_recover_after_restart() != 0) failed++;
    if (test_reconcile_after_recovery() != 0) failed++;
    if (test_torn_record_tolerance() != 0) failed++;
    if (test_route_persistence() != 0) failed++;

    /* Drop the last store and close persistence so the leak checker sees
     * a clean process exit. */
    if (g_default_store) {
        danos_object_store_destroy(g_default_store);
        g_default_store = NULL;
    }
    danos_persist_disable();

    printf("=== persist_test: %s ===\n",
           failed == 0 ? "ALL PASSED" : "FAILURES");
    return failed;
}
