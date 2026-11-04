/* Test: Transaction Engine (B3, B4, B5) */
#include <danos/core/transaction.h>
#include <danos/core/object_registry.h>
#include <danos/core/backend_ops.h>
#include <danos/core/persist.h>
#include <danos/dpa.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>

#define TX_WAL "/tmp/danos-test-tx-rollback.wal"

static bool g_fake_fail;
static uint8_t g_last_addr;

static danos_status_t fake_iface_up(danos_ifindex_t ifindex, bool up, void *user)
{
    (void)ifindex; (void)up; (void)user;
    return g_fake_fail ? DANOS_ERR_BACKEND_IO : DANOS_OK;
}

static danos_status_t fake_addr_set(const danos_iface_t *iface, void *user)
{
    (void)user;
    if (!iface || iface->ipv4_address.addr.af != DANOS_AF_IPV4)
        return DANOS_OK;
    g_last_addr = iface->ipv4_address.addr.addr[3];
    return DANOS_OK;
}

static danos_iface_t tx_iface(uint32_t ifindex, uint16_t mtu, uint8_t host)
{
    danos_iface_t ifc;
    memset(&ifc, 0, sizeof(ifc));
    ifc.ifindex = ifindex;
    snprintf(ifc.name, sizeof(ifc.name), "txrb0");
    ifc.mtu = mtu;
    ifc.admin_up = true;
    ifc.ipv4_address.addr.af = DANOS_AF_IPV4;
    ifc.ipv4_address.addr.addr[0] = 192;
    ifc.ipv4_address.addr.addr[1] = 0;
    ifc.ipv4_address.addr.addr[2] = 2;
    ifc.ipv4_address.addr.addr[3] = host;
    ifc.ipv4_address.prefix_len = 24;
    return ifc;
}

static void tx_commit_iface_update(danos_tx_t *tx, const danos_iface_t *ifc)
{
    assert(danos_tx_begin(tx, "txrb", NULL) == DANOS_OK);
    assert(danos_tx_stage_object(tx, DANOS_OBJ_IFACE, ifc->ifindex,
                                 ifc, sizeof(*ifc),
                                 DANOS_TX_OP_UPDATE) == DANOS_OK);
    assert(danos_tx_prepare(tx) == DANOS_OK);
    assert(danos_tx_validate(tx) == DANOS_OK);
    assert(danos_tx_commit(tx) == DANOS_OK);
}

static danos_iface_t tx_read_iface(uint32_t ifindex)
{
    danos_iface_t out;
    size_t sz = sizeof(out);
    assert(danos_object_read(g_default_store, DANOS_OBJ_IFACE, ifindex,
                             &out, &sz) == DANOS_OK);
    return out;
}

int test_transaction(void)
{
    danos_tx_manager_init();

    /* B3: full lifecycle */
    danos_tx_t tx = {0};
    danos_status_t st = danos_tx_begin(&tx, "test", NULL);
    assert(st == DANOS_OK);
    assert(tx.id > 0);
    assert(tx.state == DANOS_TX_OPEN);

    st = danos_tx_prepare(&tx);
    assert(st == DANOS_OK);
    assert(tx.state == DANOS_TX_PREPARE);

    st = danos_tx_validate(&tx);
    assert(st == DANOS_OK);
    assert(tx.state == DANOS_TX_VALIDATE);

    st = danos_tx_commit(&tx);
    assert(st == DANOS_OK);
    assert(tx.state == DANOS_TX_COMMIT);

    st = danos_tx_verify(&tx);
    assert(st == DANOS_OK);
    assert(tx.state == DANOS_TX_DONE);

    /* B3: abort path */
    danos_tx_t tx2 = {0};
    st = danos_tx_begin(&tx2, "test", NULL);
    assert(st == DANOS_OK);
    st = danos_tx_abort(&tx2);
    assert(st == DANOS_OK);
    assert(tx2.state == DANOS_TX_ABORT);

    /* B3: invalid transitions */
    danos_tx_t tx3 = {0};
    st = danos_tx_begin(&tx3, "test", NULL);
    /* commit without prepare/validate should fail */
    st = danos_tx_commit(&tx3);
    assert(st == DANOS_ERR_TX_INVALID);
    danos_tx_abort(&tx3);

    /* B4: concurrent transactions (multi-begin, serial commit) */
    danos_tx_t txa = {0}, txb = {0};
    assert(danos_tx_begin(&txa, "a", NULL) == DANOS_OK);
    assert(danos_tx_begin(&txb, "b", NULL) == DANOS_OK);
    assert(danos_tx_prepare(&txa) == DANOS_OK);
    assert(danos_tx_prepare(&txb) == DANOS_OK);
    assert(danos_tx_validate(&txa) == DANOS_OK);
    assert(danos_tx_validate(&txb) == DANOS_OK);
    assert(danos_tx_commit(&txa) == DANOS_OK);
    assert(danos_tx_commit(&txb) == DANOS_OK);
    assert(danos_tx_verify(&txa) == DANOS_OK);
    assert(danos_tx_verify(&txb) == DANOS_OK);

    /* B5: atomic commit helper */
    danos_tx_t tx4 = {0};
    st = danos_tx_begin(&tx4, "atomic", NULL);
    assert(st == DANOS_OK);
    st = danos_tx_commit_atomic(&tx4);
    assert(st == DANOS_OK);
    assert(tx4.state == DANOS_TX_DONE);

    /* An empty committed transaction rolls back as a no-op. */
    danos_tx_t tx5 = {0};
    assert(danos_tx_begin(&tx5, "rollback", NULL) == DANOS_OK);
    assert(danos_tx_prepare(&tx5) == DANOS_OK);
    assert(danos_tx_validate(&tx5) == DANOS_OK);
    assert(danos_tx_commit(&tx5) == DANOS_OK);
    assert(danos_tx_rollback(&tx5) == DANOS_OK);

    /* Releasing a committed transaction discards the retained snapshot;
     * rollback then reports the specific impossible-restore status. */
    danos_tx_t tx6 = {0};
    assert(danos_tx_begin(&tx6, "released", NULL) == DANOS_OK);
    assert(danos_tx_prepare(&tx6) == DANOS_OK);
    assert(danos_tx_validate(&tx6) == DANOS_OK);
    assert(danos_tx_commit(&tx6) == DANOS_OK);
    danos_tx_release(&tx6);
    assert(danos_tx_rollback(&tx6) == DANOS_ERR_TX_ROLLBACK);

    danos_tx_manager_fini();
    printf("[PASS] test_transaction: state machine + concurrency + atomic\n");
    return 0;
}

int test_transaction_committed_rollback(void)
{
    danos_object_store_t *saved = g_default_store;
    g_default_store = danos_object_store_create(64);
    assert(g_default_store);
    unlink(TX_WAL);
    assert(danos_persist_enable(TX_WAL) == 0);

    danos_iface_t old = tx_iface(8101, 1500, 1);
    assert(danos_object_create(g_default_store, DANOS_OBJ_IFACE, 8101,
                               &old, sizeof(old)) == DANOS_OK);

    danos_backend_ops_t ops = {
        .name = "tx-rollback-fake",
        .iface_up = fake_iface_up,
        .iface_addr_set = fake_addr_set,
    };
    g_fake_fail = false;
    g_last_addr = 0;
    danos_backend_ops_set(&ops);

    /* commit -> rollback -> reconcile programs the restored address. */
    danos_iface_t changed = old;
    changed.mtu = 9000;
    changed.ipv4_address.addr.addr[3] = 2;
    danos_tx_t tx;
    tx_commit_iface_update(&tx, &changed);
    assert(tx_read_iface(8101).mtu == 9000);
    assert(danos_tx_rollback(&tx) == DANOS_OK);
    assert(tx.state == DANOS_TX_ABORT);
    danos_iface_t restored = tx_read_iface(8101);
    assert(restored.mtu == 1500);
    assert(restored.ipv4_address.addr.addr[3] == 1);
    assert(g_last_addr == 1);   /* reconciler replayed the restored value */

    /* Backend failure after the store restore is reported specifically.
     * Program the committed value first so the restored value is a real
     * backend change during rollback. */
    g_fake_fail = false;
    changed.mtu = 8000;
    changed.ipv4_address.addr.addr[3] = 3;
    tx_commit_iface_update(&tx, &changed);
    uint64_t attempted = 0, failed = 0;
    (void)danos_programming_run(&attempted, &failed);
    g_fake_fail = true;
    assert(danos_tx_rollback(&tx) == DANOS_ERR_TX_ROLLBACK);
    restored = tx_read_iface(8101);
    assert(restored.mtu == 1500);
    assert(restored.ipv4_address.addr.addr[3] == 1);
    g_fake_fail = false;

    /* Rollback is durable: the WAL replays the restored value on restart. */
    changed.mtu = 7000;
    changed.ipv4_address.addr.addr[3] = 4;
    tx_commit_iface_update(&tx, &changed);
    assert(danos_tx_rollback(&tx) == DANOS_OK);
    assert(tx_read_iface(8101).mtu == 1500);

    danos_backend_ops_set(NULL);
    danos_persist_disable();
    danos_object_store_destroy(g_default_store);
    g_default_store = danos_object_store_create(64);
    assert(g_default_store);
    assert(danos_persist_enable(TX_WAL) == 0);
    assert(danos_persist_recover() >= 0);
    restored = tx_read_iface(8101);
    assert(restored.mtu == 1500);
    assert(restored.ipv4_address.addr.addr[3] == 1);

    danos_persist_disable();
    danos_object_store_destroy(g_default_store);
    g_default_store = saved;
    unlink(TX_WAL);
    printf("[PASS] test_transaction_committed_rollback: restore, reconcile, "
           "backend-failure signal and restart replay\n");
    return 0;
}
