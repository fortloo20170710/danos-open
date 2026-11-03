/*
 * Test: programming-pipeline OPER readback failure handling.
 *
 * A readback I/O error must preserve the programmed ledger and fail the pass;
 * NOT_FOUND repairs; NOT_SUPPORTED is not drift; a later success repopulates
 * the OPER snapshot.
 */
#include <danos/core/backend_ops.h>
#include <danos/core/object_registry.h>
#include <danos/dpa.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static danos_status_t g_readback_rc;

static danos_status_t fake_iface_up(danos_ifindex_t ifindex, bool up, void *user)
{
    (void)ifindex; (void)up; (void)user;
    return DANOS_OK;
}

static danos_status_t fake_readback(danos_obj_type_t type, danos_obj_id_t id,
                                    const void *desired, size_t size,
                                    void *out, void *user)
{
    (void)type; (void)id; (void)user;
    if (g_readback_rc == DANOS_OK)
        memcpy(out, desired, size);
    return g_readback_rc;
}

int main(void)
{
    if (!g_default_store) g_default_store = danos_object_store_create(256);
    assert(g_default_store);

    danos_tx_t tx;
    assert(danos_tx_begin(&tx, "rb", NULL) == DANOS_OK);
    danos_iface_t iface;
    memset(&iface, 0, sizeof(iface));
    iface.ifindex = 7;
    snprintf(iface.name, sizeof(iface.name), "rb0");
    iface.mtu = 1500;
    iface.admin_up = true;
    assert(danos_iface_create(&tx, &iface) == DANOS_OK);
    assert(danos_tx_commit_atomic(&tx) == DANOS_OK);

    danos_backend_ops_t ops = {
        .name = "fake",
        .iface_up = fake_iface_up,
        .readback = fake_readback,
    };
    danos_backend_ops_set(&ops);

    uint64_t attempted = 0, failed = 0;
    g_readback_rc = DANOS_OK;
    assert(danos_programming_run(&attempted, &failed) == 1);
    assert(attempted == 1 && failed == 0);
    assert(danos_oper_count(DANOS_OBJ_IFACE) == 1);

    /* Probe I/O failure is not drift: ledger preserved, pass fails. */
    g_readback_rc = DANOS_ERR_BACKEND_IO;
    attempted = failed = 0;
    assert(danos_programming_run(&attempted, &failed) == 0);
    assert(attempted == 1 && failed == 1);
    assert(danos_programming_programmed_count(DANOS_OBJ_IFACE) == 1);
    assert(danos_oper_count(DANOS_OBJ_IFACE) == 0);

    /* No readback capability is not drift either. */
    g_readback_rc = DANOS_ERR_NOT_SUPPORTED;
    attempted = failed = 0;
    assert(danos_programming_run(&attempted, &failed) == 0);
    assert(attempted == 0 && failed == 0);

    /* NOT_FOUND repairs in the same pass. */
    g_readback_rc = DANOS_ERR_NOT_FOUND;
    attempted = failed = 0;
    assert(danos_programming_run(&attempted, &failed) == 1);
    assert(attempted == 1 && failed == 0);
    assert(danos_programming_programmed_count(DANOS_OBJ_IFACE) == 1);

    /* A later healthy probe repopulates OPER. */
    g_readback_rc = DANOS_OK;
    attempted = failed = 0;
    assert(danos_programming_run(&attempted, &failed) == 0);
    assert(attempted == 0 && failed == 0);
    assert(danos_oper_count(DANOS_OBJ_IFACE) == 1);

    danos_backend_ops_set(NULL);
    (void)danos_object_delete(g_default_store, DANOS_OBJ_IFACE, 7);
    printf("[PASS] backend readback failure: I/O preserves ledger, "
           "NOT_SUPPORTED is not drift, NOT_FOUND repairs, success repopulates "
           "OPER\n");
    return 0;
}
