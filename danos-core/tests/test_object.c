/* Test: Object Registry (B1) + v0.2 Tunnel/EVPN CRUD */
#include <danos/core/object_registry.h>
#include <danos/dpa.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>

int test_object(void)
{
    danos_object_store_t *s = danos_object_store_create(64);
    assert(s != NULL);

    /* Create */
    const char *data = "hello";
    danos_status_t st = danos_object_create(s, DANOS_OBJ_ROUTE, 1, data, 6);
    assert(st == DANOS_OK);

    /* Duplicate create fails */
    st = danos_object_create(s, DANOS_OBJ_ROUTE, 1, data, 6);
    assert(st == DANOS_ERR_EXISTS);

    /* Read */
    char buf[16] = {0};
    size_t sz = sizeof(buf);
    st = danos_object_read(s, DANOS_OBJ_ROUTE, 1, buf, &sz);
    assert(st == DANOS_OK);
    assert(sz == 6);
    assert(strcmp(buf, "hello") == 0);

    /* Update */
    st = danos_object_update(s, DANOS_OBJ_ROUTE, 1, "world", 6);
    assert(st == DANOS_OK);
    sz = sizeof(buf);
    st = danos_object_read(s, DANOS_OBJ_ROUTE, 1, buf, &sz);
    assert(strcmp(buf, "world") == 0);

    /* Count */
    assert(danos_object_count(s, DANOS_OBJ_ROUTE) == 1);
    danos_object_create(s, DANOS_OBJ_ROUTE, 2, "foo", 4);
    assert(danos_object_count(s, DANOS_OBJ_ROUTE) == 2);

    /* Delete */
    st = danos_object_delete(s, DANOS_OBJ_ROUTE, 1);
    assert(st == DANOS_OK);
    st = danos_object_delete(s, DANOS_OBJ_ROUTE, 1);
    assert(st == DANOS_ERR_NOT_FOUND);
    assert(danos_object_count(s, DANOS_OBJ_ROUTE) == 1);

    danos_object_store_destroy(s);
    printf("[PASS] test_object: CRUD works\n");
    return 0;
}

/* Oversized payload rejection.
 *
 * The programming pipeline keeps fixed-size ledger buffers derived from
 * DANOS_MAX_OBJECT_BYTES, so the registry must refuse anything larger at
 * the boundary. Before this bound existed, any object larger than the
 * buffer smashed the reconciler stack. Two directions are checked:
 * rejection above the limit, and acceptance of the largest real DPA type
 * (danos_nhgroup_t, 536 bytes) which is wider than the old 512-byte read
 * buffer and must therefore still round-trip. */
int test_object_size_bounds(void)
{
    danos_object_store_t *s = danos_object_store_create(8);
    assert(s != NULL);

    /* Largest real object type must be accepted at full size. */
    assert(sizeof(danos_nhgroup_t) <= DANOS_MAX_OBJECT_BYTES);
    danos_nhgroup_t grp;
    memset(&grp, 0, sizeof(grp));
    grp.id = 7;
    grp.nh_count = 64;
    for (uint32_t i = 0; i < 64; i++) grp.nh_ids[i] = i + 1;

    danos_status_t st = danos_object_create(s, DANOS_OBJ_NHGROUP, 7,
                                            &grp, sizeof(grp));
    assert(st == DANOS_OK);

    /* Read it back through a buffer that is not oversized, i.e. exactly
     * the object size, matching how the ledger reads entries. */
    static_assert(sizeof(danos_nhgroup_t) <= DANOS_MAX_OBJECT_BYTES,
                  "ledger buffer must cover the largest DPA type");
    uint8_t buf[DANOS_MAX_OBJECT_BYTES];
    size_t sz = sizeof(buf);
    st = danos_object_read(s, DANOS_OBJ_NHGROUP, 7, buf, &sz);
    assert(st == DANOS_OK);
    assert(sz == sizeof(grp));
    assert(memcmp(buf, &grp, sizeof(grp)) == 0);

    /* Exactly at the limit is still accepted. */
    static uint8_t at_limit[DANOS_MAX_OBJECT_BYTES];
    memset(at_limit, 0xA5, sizeof(at_limit));
    st = danos_object_create(s, DANOS_OBJ_TUNNEL, 1,
                             at_limit, DANOS_MAX_OBJECT_BYTES);
    assert(st == DANOS_OK);

    /* One byte over the limit is rejected on create... */
    st = danos_object_create(s, DANOS_OBJ_TUNNEL, 2,
                             at_limit, DANOS_MAX_OBJECT_BYTES + 1);
    assert(st == DANOS_ERR_INVALID_ARG);
    assert(danos_object_count(s, DANOS_OBJ_TUNNEL) == 1);

    /* ...and on update of an existing entry, which must leave the
     * original payload intact rather than half-written. */
    st = danos_object_update(s, DANOS_OBJ_TUNNEL, 1,
                             at_limit, DANOS_MAX_OBJECT_BYTES + 64);
    assert(st == DANOS_ERR_INVALID_ARG);
    sz = sizeof(buf);
    assert(danos_object_read(s, DANOS_OBJ_TUNNEL, 1, buf, &sz) == DANOS_OK);
    assert(sz == DANOS_MAX_OBJECT_BYTES);
    assert(buf[0] == 0xA5 && buf[DANOS_MAX_OBJECT_BYTES - 1] == 0xA5);

    danos_object_store_destroy(s);
    printf("[PASS] test_object_size_bounds: oversized payloads rejected, "
           "largest DPA type round-trips\n");
    return 0;
}

/* Batch mutation: end state, mixed write+delete, and size enforcement.
 *
 * apply_batch exists so a transaction commit can publish a whole
 * configuration in one step; applying mutations one at a time takes and
 * releases the write lock per object, letting a reader - or the reconciler -
 * observe half a configuration, such as a route installed before the
 * interface it depends on.
 *
 * The guarantee is structural (one lock acquisition for the batch). A
 * concurrent reader is run here as a smoke check, but see the note at the
 * end of the function about what it can actually detect.
 */
/* Shared with the reader thread, so these must be real atomics: `volatile`
 * only suppresses compiler optimisation and TSAN correctly reports it as a
 * data race. */
struct batch_reader {
    danos_object_store_t *store;
    atomic_int stop;
    atomic_int torn;
    atomic_uint expected_low, expected_high;
};

static void *batch_reader_thread(void *arg)
{
    struct batch_reader *r = arg;
    while (!atomic_load_explicit(&r->stop, memory_order_relaxed)) {
        uint64_t n = danos_object_count(r->store, DANOS_OBJ_IFACE);
        unsigned lo = atomic_load_explicit(&r->expected_low,
                                           memory_order_relaxed);
        unsigned hi = atomic_load_explicit(&r->expected_high,
                                           memory_order_relaxed);
        if (n != lo && n != hi) atomic_store_explicit(&r->torn, 1,
                                                      memory_order_relaxed);
    }
    return NULL;
}

int test_object_batch_atomic(void)
{
    danos_object_store_t *s = danos_object_store_create(64);
    assert(s != NULL);

    /* Seed 3 interfaces. */
    danos_iface_t ifc;
    for (uint32_t i = 1; i <= 3; i++) {
        memset(&ifc, 0, sizeof(ifc));
        ifc.ifindex = i;
        snprintf(ifc.name, sizeof(ifc.name), "eth%u", i);
        ifc.mtu = 1500;
        assert(danos_object_create(s, DANOS_OBJ_IFACE, i, &ifc, sizeof(ifc))
               == DANOS_OK);
    }
    assert(danos_object_count(s, DANOS_OBJ_IFACE) == 3);

    struct batch_reader r;
    memset(&r, 0, sizeof(r));
    r.store = s;
    atomic_init(&r.stop, 0);
    atomic_init(&r.torn, 0);
    atomic_init(&r.expected_low, 3);
    atomic_init(&r.expected_high, 4);
    pthread_t rd;
    assert(pthread_create(&rd, NULL, batch_reader_thread, &r) == 0);

    /* Replace 3 and add 1 in one batch: 3 -> 4. */
    danos_iface_t next[4];
    danos_object_mutation_t muts[4];
    for (uint32_t i = 1; i <= 3; i++) {
        memset(&next[i - 1], 0, sizeof(next[0]));
        next[i - 1].ifindex = i;
        snprintf(next[i - 1].name, sizeof(next[i - 1].name), "eth%u", i);
        next[i - 1].mtu = 9000;
        muts[i - 1].type = DANOS_OBJ_IFACE;
        muts[i - 1].id = i;
        muts[i - 1].data = &next[i - 1];
        muts[i - 1].data_size = sizeof(next[0]);
        muts[i - 1].remove = false;
    }
    memset(&next[3], 0, sizeof(next[0]));
    next[3].ifindex = 4;
    snprintf(next[3].name, sizeof(next[3].name), "eth4");
    next[3].mtu = 1500;
    muts[3] = muts[0];
    muts[3].id = 4;
    muts[3].data = &next[3];

    assert(danos_object_apply_batch(s, muts, 4) == 4);
    assert(danos_object_count(s, DANOS_OBJ_IFACE) == 4);

    /* Mixed write + delete: drop eth1, so 4 -> 3. */
    /* Reader still running: 4 -> 3 must also look atomic. */
    atomic_store(&r.expected_low, 3);
    atomic_store(&r.expected_high, 4);
    uint8_t big[DANOS_MAX_OBJECT_BYTES + 1];
    memset(big, 0, sizeof(big));
    danos_object_mutation_t m2[2];
    m2[0].type = DANOS_OBJ_IFACE; m2[0].id = 1;
    m2[0].data = NULL; m2[0].data_size = 0; m2[0].remove = true;
    m2[1] = muts[0];
    m2[1].id = 2;              /* rewrite eth2, not the one we just deleted */
    m2[1].data = &next[1];
    assert(danos_object_apply_batch(s, m2, 2) == 2);
    assert(danos_object_count(s, DANOS_OBJ_IFACE) == 3);

    /* Oversized payload is refused rather than written. */
    danos_object_mutation_t m3 = { .type = DANOS_OBJ_IFACE, .id = 2,
                                   .data = big, .data_size = sizeof(big),
                                   .remove = false };
    assert(danos_object_apply_batch(s, &m3, 1) < 0);
    /* eth2 untouched. */
    uint8_t back[sizeof(danos_iface_t)];
    size_t bsz = sizeof(back);
    assert(danos_object_read(s, DANOS_OBJ_IFACE, 2, back, &bsz) == DANOS_OK);
    assert(((danos_iface_t *)back)->mtu == 9000);

    atomic_store(&r.stop, 1);
    pthread_join(rd, NULL);

    /* What this can and cannot show.
     *
     * The end-state assertions above are real: they prove the batch produces
     * the intended store contents and that an oversized payload is refused
     * without disturbing existing objects.
     *
     * The reader check is only a smoke test. Releasing the write lock per
     * mutation leaves a window a few nanoseconds wide, and sampling from
     * another thread does not reliably observe it - verified by rebuilding
     * apply_batch with per-mutation locking, which this test still passes.
     * The atomicity guarantee is structural: apply_batch takes the write lock
     * once for the whole batch, so no reader can interleave. Do not read
     * `torn` as proof of that; it catches gross violations only. */
    assert(atomic_load(&r.torn) == 0);

    danos_object_store_destroy(s);
    printf("[PASS] test_object_batch_atomic: batch end state, mixed "
           "write+delete, oversized refusal\n");
    return 0;
}

/* v0.2: Tunnel CRUD via DPA public API */
int test_tunnel_crud(void)
{
    danos_tx_t tx = {0};
    assert(danos_tx_begin(&tx, "test", NULL) == DANOS_OK);

    /* Create VXLAN tunnel */
    danos_tunnel_t tun;
    memset(&tun, 0, sizeof(tun));
    tun.id = 1;
    tun.type = DANOS_TUNNEL_VXLAN;
    tun.ifindex = 100;
    tun.src.af = DANOS_AF_IPV4;
    tun.src.addr[0] = 10; tun.src.addr[3] = 1;  /* 10.0.0.1 */
    tun.dst.af = DANOS_AF_IPV4;
    tun.dst.addr[0] = 10; tun.dst.addr[3] = 2;  /* 10.0.0.2 */
    tun.vni = 10000;

    danos_status_t st = danos_tunnel_create(&tx, &tun);
    assert(st == DANOS_OK);

    /* Read back */
    danos_tunnel_t out;
    st = danos_tunnel_read(&tx, 1, &out);
    assert(st == DANOS_OK);
    assert(out.id == 1);
    assert(out.type == DANOS_TUNNEL_VXLAN);
    assert(out.ifindex == 100);
    assert(out.vni == 10000);

    /* Update */
    tun.vni = 20000;
    st = danos_tunnel_update(&tx, &tun);
    assert(st == DANOS_OK);
    st = danos_tunnel_read(&tx, 1, &out);
    assert(st == DANOS_OK);
    assert(out.vni == 20000);

    /* Delete */
    st = danos_tunnel_delete(&tx, 1);
    assert(st == DANOS_OK);
    st = danos_tunnel_read(&tx, 1, &out);
    assert(st == DANOS_ERR_NOT_FOUND);

    /* Full lifecycle, and assert it: commit alone would fail its state
     * precondition (OPEN != VALIDATE) and leave the record unretired. */
    assert(danos_tx_commit_atomic(&tx) == DANOS_OK);
    printf("[PASS] test_tunnel_crud: Tunnel create/read/update/delete\n");
    return 0;
}

/* v0.2: EVPN EVI CRUD via DPA public API */
int test_evpn_crud(void)
{
    danos_tx_t tx = {0};
    assert(danos_tx_begin(&tx, "test", NULL) == DANOS_OK);

    /* Create EVPN EVI */
    danos_evpn_evi_t evi;
    memset(&evi, 0, sizeof(evi));
    evi.evi = 100;
    evi.rd.af = DANOS_AF_IPV4;
    evi.rd.addr[0] = 10; evi.rd.addr[3] = 1;  /* 10.0.0.1 */
    evi.rt_import.af = DANOS_AF_IPV4;
    evi.rt_import.addr[0] = 10; evi.rt_import.addr[3] = 2;
    evi.rt_export.af = DANOS_AF_IPV4;
    evi.rt_export.addr[0] = 10; evi.rt_export.addr[3] = 3;
    evi.vni = 100100;
    evi.irb = true;

    danos_status_t st = danos_evpn_evi_create(&tx, &evi);
    assert(st == DANOS_OK);

    /* Read back */
    danos_evpn_evi_t out;
    st = danos_evpn_evi_read(&tx, 100, &out);
    assert(st == DANOS_OK);
    assert(out.evi == 100);
    assert(out.vni == 100100);
    assert(out.irb == true);

    /* Update */
    evi.irb = false;
    st = danos_evpn_evi_update(&tx, &evi);
    assert(st == DANOS_OK);
    st = danos_evpn_evi_read(&tx, 100, &out);
    assert(st == DANOS_OK);
    assert(out.irb == false);

    /* Delete */
    st = danos_evpn_evi_delete(&tx, 100);
    assert(st == DANOS_OK);
    st = danos_evpn_evi_read(&tx, 100, &out);
    assert(st == DANOS_ERR_NOT_FOUND);

    /* Full lifecycle, and assert it: commit alone would fail its state
     * precondition (OPEN != VALIDATE) and leave the record unretired. */
    assert(danos_tx_commit_atomic(&tx) == DANOS_OK);
    printf("[PASS] test_evpn_crud: EVPN EVI create/read/update/delete\n");
    return 0;
}

/* v0.2: Multicast mroute CRUD via DPA public API */
int test_mroute_crud(void)
{
    danos_tx_t tx = {0};
    assert(danos_tx_begin(&tx, "test", NULL) == DANOS_OK);

    /* Create (*,G) mroute: vrf=0, group=239.1.1.1, RPF if=1, 2 OIFs */
    danos_mroute_t mr;
    memset(&mr, 0, sizeof(mr));
    mr.vrf_id = 0;
    mr.source.af = DANOS_AF_UNSPEC;          /* (*,G) */
    mr.group.af = DANOS_AF_IPV4;
    mr.group.addr[0] = 239; mr.group.addr[1] = 1;
    mr.group.addr[2] = 1;   mr.group.addr[3] = 1;
    mr.incoming_if = 1;
    mr.oif_count = 2;
    mr.oif_list[0] = 10;
    mr.oif_list[1] = 11;

    danos_status_t st = danos_mroute_create(&tx, &mr);
    assert(st == DANOS_OK);

    /* Read back */
    danos_mroute_t out;
    st = danos_mroute_read(&tx, 0, &mr.group, &out);
    assert(st == DANOS_OK);
    assert(out.vrf_id == 0);
    assert(out.incoming_if == 1);
    assert(out.oif_count == 2);
    assert(out.oif_list[0] == 10);
    assert(out.oif_list[1] == 11);

    /* Update: add a third OIF */
    mr.oif_count = 3;
    mr.oif_list[2] = 12;
    st = danos_mroute_update(&tx, &mr);
    assert(st == DANOS_OK);
    st = danos_mroute_read(&tx, 0, &mr.group, &out);
    assert(st == DANOS_OK);
    assert(out.oif_count == 3);
    assert(out.oif_list[2] == 12);

    /* Delete */
    st = danos_mroute_delete(&tx, 0, &mr.group);
    assert(st == DANOS_OK);
    st = danos_mroute_read(&tx, 0, &mr.group, &out);
    assert(st == DANOS_ERR_NOT_FOUND);

    /* Full lifecycle, and assert it: commit alone would fail its state
     * precondition (OPEN != VALIDATE) and leave the record unretired. */
    assert(danos_tx_commit_atomic(&tx) == DANOS_OK);
    printf("[PASS] test_mroute_crud: mroute create/read/update/delete\n");
    return 0;
}
