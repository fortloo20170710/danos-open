/*
 * Test: NETCONF Server (E3)
 */
#include "../src/netconf/netconf.h"
#include "../src/netconf/netconf_datastore.h"
#include <danos/core/object_registry.h>
#include <danos/dpa.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <assert.h>

int test_netconf_parse(void)
{
    assert(netconf_parse_rpc("<rpc><get-config/></rpc>") == NETCONF_RPC_GET_CONFIG);
    assert(netconf_parse_rpc("<rpc><edit-config/></rpc>") == NETCONF_RPC_EDIT_CONFIG);
    assert(netconf_parse_rpc("<rpc><commit/></rpc>") == NETCONF_RPC_COMMIT);
    assert(netconf_parse_rpc("<rpc><discard-changes/></rpc>") == NETCONF_RPC_DISCARD);
    assert(netconf_parse_rpc("<rpc><close-session/></rpc>") == NETCONF_RPC_CLOSE_SESSION);
    assert(netconf_parse_rpc("<rpc><lock/></rpc>") == NETCONF_RPC_LOCK);
    assert(netconf_parse_rpc("<rpc><copy-config/></rpc>") == NETCONF_RPC_COPY_CONFIG);
    assert(netconf_parse_rpc("<rpc><delete-config/></rpc>") == NETCONF_RPC_DELETE_CONFIG);
    assert(netconf_parse_rpc("<rpc><validate/></rpc>") == NETCONF_RPC_VALIDATE);
    assert(netconf_parse_rpc("<rpc><cancel-commit/></rpc>") == NETCONF_RPC_CANCEL_COMMIT);
    assert(netconf_parse_rpc("<rpc><unknown-op/></rpc>") == NETCONF_RPC_UNKNOWN);
    assert(netconf_parse_rpc(NULL) == NETCONF_RPC_UNKNOWN);

    printf("[PASS] test_netconf_parse: RPC type detection\n");
    return 0;
}

int test_netconf_hello(void)
{
    char *hello = netconf_hello_message(4242);
    assert(hello != NULL);
    assert(strstr(hello, "hello") != NULL);
    assert(strstr(hello, "capabilities") != NULL);
    assert(strstr(hello, "netconf:base:1.0") != NULL);
    assert(strstr(hello, "netconf:base:1.1") != NULL);
    assert(strstr(hello, "danos:yang") != NULL);
    assert(strstr(hello, "<session-id>4242</session-id>") != NULL);

    /* Only implemented capabilities are advertised. */
    assert(strstr(hello, "capability:writable-running") != NULL);
    assert(strstr(hello, "capability:candidate") != NULL);
    assert(strstr(hello, "capability:startup") != NULL);
    assert(strstr(hello, "rollback-on-error") == NULL);
    assert(strstr(hello, "capability:validate") != NULL);
    assert(strstr(hello, "confirmed-commit") != NULL);

    uint64_t sid = netconf_new_session_id();
    assert(sid >= 1 && sid <= 0x7fffffffULL);
    free(hello);

    printf("[PASS] test_netconf_hello: capabilities match implemented set\n");
    return 0;
}

int test_netconf_handle_rpc(void)
{
    netconf_ctx_t ctx;
    netconf_init(&ctx, 830);
    ctx.session_id = 11;

    /* get-config */
    char *resp = netconf_handle_rpc(&ctx, "<rpc><get-config><source><running/></source></get-config></rpc>");
    assert(resp != NULL);
    assert(strstr(resp, "data") != NULL);
    free(resp);

    /* edit-config */
    resp = netconf_handle_rpc(&ctx, "<rpc><edit-config><target><running/></target></edit-config></rpc>");
    assert(resp != NULL);
    assert(strstr(resp, "ok") != NULL);
    free(resp);

    /* commit with no candidate changes is a successful no-op. */
    resp = netconf_handle_rpc(&ctx, "<rpc><commit/></rpc>");
    assert(resp != NULL);
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);

    /* lock and unlock a datastore. */
    resp = netconf_handle_rpc(&ctx, "<rpc><lock><target><running/></target></lock></rpc>");
    assert(resp != NULL);
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);

    resp = netconf_handle_rpc(&ctx, "<rpc><unlock><target><running/></target></unlock></rpc>");
    assert(resp != NULL);
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);

    /* validate a datastore without changing it. */
    resp = netconf_handle_rpc(&ctx, "<rpc><validate><source><running/></source></validate></rpc>");
    assert(resp != NULL);
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);

    /* unknown */
    resp = netconf_handle_rpc(&ctx, "<rpc><foobar/></rpc>");
    assert(resp != NULL);
    assert(strstr(resp, "rpc-error") != NULL);
    free(resp);

    /* Stats */
    uint64_t rpc_count, error_count;
    netconf_get_stats(&ctx, &rpc_count, &error_count);
    assert(rpc_count == 7);
    assert(error_count == 1);
    netconf_fini(&ctx);

    printf("[PASS] test_netconf_handle_rpc: RPC dispatch works\n");
    return 0;
}

/* Seed through the real DPA transaction path so objects land in
 * g_default_store, exactly where the NETCONF get-config view reads. */
static int seed_iface(int ifindex, const char *name, unsigned mtu, bool up)
{
    danos_iface_t ifc;
    memset(&ifc, 0, sizeof(ifc));
    ifc.ifindex = ifindex;
    snprintf(ifc.name, sizeof(ifc.name), "%s", name);
    ifc.mtu = mtu;
    ifc.admin_up = up;

    danos_tx_t tx;
    assert(danos_tx_begin(&tx, "seed", NULL) == DANOS_OK);
    assert(danos_iface_create(&tx, &ifc) == DANOS_OK);
    assert(danos_tx_prepare(&tx) == DANOS_OK);
    assert(danos_tx_validate(&tx) == DANOS_OK);
    assert(danos_tx_commit(&tx) == DANOS_OK);
    return 0;
}

/*
 * get-config must survive a store far larger than any fixed reply buffer.
 *
 * The original implementation appended into a fixed 8 KiB buffer with
 * `off += snprintf(buf + off, sizeof - off, ...)`. Past the capacity the
 * remaining-size argument underflowed to a huge size_t and the write ran
 * off the end. Creating enough interfaces to overrun the old buffer makes
 * this a regression test rather than a shape check.
 */
int test_netconf_get_config_large(void)
{
    netconf_ctx_t ctx;
    netconf_init(&ctx, 830);
    ctx.session_id = 21;

    /* Long names inflate each <interface> block, so 400 of them overflow
     * the previous 8 KiB budget several times over. The filler is kept
     * short enough that "%s%d" always fits ifc.name under -Werror. */
    char name[32];
    memset(name, 'n', sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';

    for (int i = 0; i < 400; i++) {
        char nm[64];
        snprintf(nm, sizeof(nm), "%s%d", name, i);
        seed_iface(i + 1, nm, 1500, (i & 1) != 0);
    }

    char *resp = netconf_handle_rpc(&ctx,
        "<rpc><get-config><source><running/></source></get-config></rpc>");
    assert(resp != NULL);

    /* Well-formed: every emitted element is closed exactly once. */
    const char *open = strstr(resp, "<interfaces");
    assert(open != NULL);
    assert(strstr(open, "</interfaces>") != NULL);
    assert(strstr(open, "</rpc-reply>") != NULL);

    /* Every interface we created is actually present. */
    int found = 0;
    char needle[80];
    for (int i = 0; i < 400; i++) {
        snprintf(needle, sizeof(needle), "<name>%s%d</name>", name, i);
        if (strstr(resp, needle)) found++;
    }
    assert(found == 400);

    free(resp);

    /* Tear down so later tests see an empty store. */
    for (int i = 0; i < 400; i++)
        assert(danos_object_delete(g_default_store, DANOS_OBJ_IFACE,
                                    i + 1) == DANOS_OK);
    netconf_fini(&ctx);

    printf("[PASS] test_netconf_get_config_large: 400 interfaces, "
           "no overflow, all present\n");
    return 0;
}

/* An interface name is attacker-controllable via gNMI/CLI; it must not be
 * able to inject markup into the get-config reply. */
int test_netconf_escapes_name(void)
{
    netconf_ctx_t ctx;
    netconf_init(&ctx, 830);
    ctx.session_id = 31;

    seed_iface(4242, "eth0</name><injected>x", 1500, true);

    char *resp = netconf_handle_rpc(&ctx,
        "<rpc><get-config><source><running/></source></get-config></rpc>");
    assert(resp != NULL);
    assert(strstr(resp, "<injected>") == NULL);
    assert(strstr(resp, "&lt;/name&gt;") != NULL);
    free(resp);

    assert(danos_object_delete(g_default_store, DANOS_OBJ_IFACE, 4242)
           == DANOS_OK);
    netconf_fini(&ctx);
    printf("[PASS] test_netconf_escapes_name: markup in name is escaped\n");
    return 0;
}

struct find_iface { const char *name; danos_iface_t out; bool found; };

static void find_iface_iter(danos_object_entry_t *e, void *user)
{
    struct find_iface *f = user;
    if (f->found) return;
    if (e->type != DANOS_OBJ_IFACE || e->data_size < sizeof(danos_iface_t))
        return;
    const danos_iface_t *i = e->data;
    if (strcmp(i->name, f->name) == 0) {
        memcpy(&f->out, i, sizeof(*i));
        f->found = true;
    }
}

static bool find_iface(const char *name, danos_iface_t *out)
{
    struct find_iface f = { .name = name, .found = false };
    danos_object_iterate(g_default_store, find_iface_iter, &f);
    if (f.found && out) *out = f.out;
    return f.found;
}

/* edit-config used to stop after the first <interface> block, so changes to
 * every later interface were silently dropped. Both blocks must apply, and a
 * failure in the second must leave the first unchanged. */
int test_netconf_edit_config_multi(void)
{
    netconf_ctx_t ctx;
    netconf_init(&ctx, 830);
    ctx.session_id = 41;

    seed_iface(9001, "et-a", 1500, true);
    seed_iface(9002, "et-b", 1500, false);

    char *resp = netconf_handle_rpc(&ctx,
        "<rpc><edit-config><target><running/></target><config>"
        "<interfaces xmlns=\"urn:danos:yang:danos-iface\">"
        "<interface><name>et-a</name><mtu>9000</mtu><enabled>false</enabled></interface>"
        "<interface><name>et-b</name><mtu>1400</mtu><enabled>true</enabled></interface>"
        "</interfaces></config></edit-config></rpc>");
    assert(resp != NULL);
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);

    danos_iface_t a, b;
    assert(find_iface("et-a", &a));
    assert(find_iface("et-b", &b));
    assert(a.mtu == 9000 && !a.admin_up);
    assert(b.mtu == 1400 && b.admin_up);

    /* Second block names an interface that does not exist: the whole
     * edit-config fails and et-a keeps its previous MTU. */
    resp = netconf_handle_rpc(&ctx,
        "<rpc><edit-config><target><running/></target><config>"
        "<interfaces xmlns=\"urn:danos:yang:danos-iface\">"
        "<interface><name>et-a</name><mtu>1234</mtu></interface>"
        "<interface><name>et-missing</name><mtu>1234</mtu></interface>"
        "</interfaces></config></edit-config></rpc>");
    assert(resp != NULL);
    assert(strstr(resp, "rpc-error") != NULL);
    free(resp);
    assert(find_iface("et-a", &a));
    assert(a.mtu == 9000);

    assert(danos_object_delete(g_default_store, DANOS_OBJ_IFACE, 9001)
           == DANOS_OK);
    assert(danos_object_delete(g_default_store, DANOS_OBJ_IFACE, 9002)
           == DANOS_OK);
    netconf_fini(&ctx);

    printf("[PASS] test_netconf_edit_config_multi: all blocks applied "
           "atomically\n");
    return 0;
}

/* ---- candidate datastore ------------------------------------------------ */

static char *nc_call(netconf_ctx_t *ctx, const char *xml)
{
    char *resp = netconf_handle_rpc(ctx, xml);
    assert(resp != NULL);
    return resp;
}

static void seed_candidate_iface(void)
{
    seed_iface(7001, "cand0", 1500, true);
}

int test_netconf_candidate(void)
{
    netconf_ctx_t ctx;
    netconf_init(&ctx, 830);
    ctx.session_id = 51;
    seed_candidate_iface();

    /* An edit to candidate leaves running untouched. */
    char *resp = nc_call(&ctx,
        "<rpc><edit-config><target><candidate/></target><config>"
        "<interfaces xmlns=\"urn:danos:yang:danos-iface\">"
        "<interface><name>cand0</name><mtu>9000</mtu></interface>"
        "</interfaces></config></edit-config></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);

    danos_iface_t ifc;
    assert(find_iface("cand0", &ifc));
    assert(ifc.mtu == 1500);

    resp = nc_call(&ctx,
        "<rpc><get-config><source><candidate/></source></get-config></rpc>");
    assert(strstr(resp, "<name>cand0</name>") != NULL);
    assert(strstr(resp, "<mtu>9000</mtu>") != NULL);
    free(resp);

    /* Commit publishes the candidate to running. */
    resp = nc_call(&ctx, "<rpc><commit/></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);
    assert(find_iface("cand0", &ifc));
    assert(ifc.mtu == 9000);

    /* Discard throws away uncommitted candidate edits. */
    resp = nc_call(&ctx,
        "<rpc><edit-config><target><candidate/></target><config>"
        "<interfaces xmlns=\"urn:danos:yang:danos-iface\">"
        "<interface><name>cand0</name><mtu>1200</mtu></interface>"
        "</interfaces></config></edit-config></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);

    resp = nc_call(&ctx, "<rpc><discard-changes/></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);

    resp = nc_call(&ctx,
        "<rpc><get-config><source><candidate/></source></get-config></rpc>");
    assert(strstr(resp, "<mtu>9000</mtu>") != NULL);
    free(resp);
    assert(find_iface("cand0", &ifc));
    assert(ifc.mtu == 9000);

    /* A confirmed commit succeeds and stays pending until confirmed. */
    resp = nc_call(&ctx, "<rpc><commit><confirmed/></commit></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);
    assert(netconf_ds_confirmed_pending());
    assert(find_iface("cand0", &ifc));
    assert(ifc.mtu == 9000);

    /* The confirming commit keeps the configuration and disarms the timer. */
    resp = nc_call(&ctx, "<rpc><commit/></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);
    assert(!netconf_ds_confirmed_pending());
    assert(find_iface("cand0", &ifc));
    assert(ifc.mtu == 9000);

    assert(danos_object_delete(g_default_store, DANOS_OBJ_IFACE, 7001)
           == DANOS_OK);
    netconf_fini(&ctx);

    printf("[PASS] test_netconf_candidate: edit isolation, commit, discard\n");
    return 0;
}

/* Candidate is writable through edit-config; startup only through
 * copy-config. */
int test_netconf_startup_readonly(void)
{
    netconf_ctx_t ctx;
    netconf_init(&ctx, 830);
    ctx.session_id = 52;

    char *resp = nc_call(&ctx,
        "<rpc><edit-config><target><startup/></target><config>"
        "<interfaces xmlns=\"urn:danos:yang:danos-iface\">"
        "<interface><name>cand0</name><mtu>9000</mtu></interface>"
        "</interfaces></config></edit-config></rpc>");
    assert(strstr(resp, "invalid-value") != NULL);
    free(resp);
    netconf_fini(&ctx);

    printf("[PASS] test_netconf_startup_readonly: startup rejects edit-config\n");
    return 0;
}

/* ---- locks -------------------------------------------------------------- */

int test_netconf_locks(void)
{
    netconf_ctx_t a, b;
    netconf_init(&a, 830);
    netconf_init(&b, 830);
    a.session_id = 61;
    b.session_id = 62;
    seed_iface(7101, "lock0", 1500, true);

    char *resp = nc_call(&a,
        "<rpc><lock><target><running/></target></lock></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);

    /* Another session cannot edit, lock again, or unlock. */
    resp = nc_call(&b,
        "<rpc><edit-config><target><running/></target><config>"
        "<interfaces xmlns=\"urn:danos:yang:danos-iface\">"
        "<interface><name>lock0</name><mtu>1000</mtu></interface>"
        "</interfaces></config></edit-config></rpc>");
    assert(strstr(resp, "rpc-error") != NULL);
    assert(strstr(resp, "in-use") != NULL);
    free(resp);

    resp = nc_call(&b,
        "<rpc><lock><target><running/></target></lock></rpc>");
    assert(strstr(resp, "lock-denied") != NULL);
    free(resp);

    resp = nc_call(&b,
        "<rpc><unlock><target><running/></target></unlock></rpc>");
    assert(strstr(resp, "lock-denied") != NULL);
    free(resp);

    danos_iface_t ifc;
    assert(find_iface("lock0", &ifc));
    assert(ifc.mtu == 1500);

    /* The owner may edit and unlock. */
    resp = nc_call(&a,
        "<rpc><edit-config><target><running/></target><config>"
        "<interfaces xmlns=\"urn:danos:yang:danos-iface\">"
        "<interface><name>lock0</name><mtu>1600</mtu></interface>"
        "</interfaces></config></edit-config></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);

    resp = nc_call(&a,
        "<rpc><unlock><target><running/></target></unlock></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);

    resp = nc_call(&b,
        "<rpc><edit-config><target><running/></target><config>"
        "<interfaces xmlns=\"urn:danos:yang:danos-iface\">"
        "<interface><name>lock0</name><mtu>1700</mtu></interface>"
        "</interfaces></config></edit-config></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);
    assert(find_iface("lock0", &ifc));
    assert(ifc.mtu == 1700);

    /* close-session releases locks. */
    resp = nc_call(&a,
        "<rpc><lock><target><candidate/></target></lock></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);

    /* Another session cannot discard or commit the locked candidate. */
    resp = nc_call(&b, "<rpc><discard-changes/></rpc>");
    assert(strstr(resp, "in-use") != NULL);
    free(resp);
    resp = nc_call(&b, "<rpc><commit/></rpc>");
    assert(strstr(resp, "in-use") != NULL);
    free(resp);
    /* A confirmed commit is still a commit: it publishes the candidate and
     * must be refused while another session holds the candidate lock. */
    resp = nc_call(&b, "<rpc><commit><confirmed/></commit></rpc>");
    assert(strstr(resp, "in-use") != NULL);
    free(resp);

    resp = nc_call(&a, "<rpc><discard-changes/></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);

    resp = nc_call(&a, "<rpc><close-session/></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);
    resp = nc_call(&b,
        "<rpc><lock><target><candidate/></target></lock></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);
    resp = nc_call(&b,
        "<rpc><unlock><target><candidate/></target></unlock></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);

    assert(danos_object_delete(g_default_store, DANOS_OBJ_IFACE, 7101)
           == DANOS_OK);
    netconf_fini(&a);
    netconf_fini(&b);

    printf("[PASS] test_netconf_locks: ownership, in-use, release on close\n");
    return 0;
}

/* Exercise the lock table across every datastore and session combination,
 * including locks taken/released directly through the datastore API. */
int test_netconf_lock_matrix(void)
{
    const uint64_t s1 = 8101, s2 = 8102;
    assert(netconf_ds_lock(NETCONF_DS_RUNNING, s1, "s1") == NETCONF_DS_OK);
    assert(netconf_ds_lock(NETCONF_DS_RUNNING, s1, "s1")
           == NETCONF_DS_LOCKED);       /* same-session relock is refused */
    assert(netconf_ds_lock(NETCONF_DS_RUNNING, s2, "s2")
           == NETCONF_DS_LOCKED);
    assert(netconf_ds_locked_by_other(NETCONF_DS_RUNNING, s2));
    assert(!netconf_ds_locked_by_other(NETCONF_DS_RUNNING, s1));
    assert(netconf_ds_unlock(NETCONF_DS_RUNNING, s2) == NETCONF_DS_LOCKED);
    assert(netconf_ds_unlock(NETCONF_DS_RUNNING, 0) == NETCONF_DS_LOCKED);
    assert(netconf_ds_unlock((netconf_datastore_t)0, s1)
           == NETCONF_DS_INVALID);
    assert(netconf_ds_lock((netconf_datastore_t)0, s1, "x")
           == NETCONF_DS_INVALID);
    assert(netconf_ds_lock(NETCONF_DS_RUNNING, 0, "x")
           == NETCONF_DS_INVALID);
    assert(netconf_ds_unlock(NETCONF_DS_RUNNING, s1) == NETCONF_DS_OK);
    assert(netconf_ds_unlock(NETCONF_DS_RUNNING, s1) == NETCONF_DS_LOCKED);
    assert(netconf_ds_lock(NETCONF_DS_CANDIDATE, s2, "s2") == NETCONF_DS_OK);
    assert(netconf_ds_locked_by_other(NETCONF_DS_CANDIDATE, s1));
    assert(netconf_ds_copy(NETCONF_DS_RUNNING, NETCONF_DS_CANDIDATE, s1)
           == NETCONF_DS_LOCKED);
    assert(netconf_ds_lock(NETCONF_DS_RUNNING, s1, "s1") == NETCONF_DS_OK);
    assert(netconf_ds_copy(NETCONF_DS_CANDIDATE, NETCONF_DS_RUNNING, s2)
           == NETCONF_DS_LOCKED);
    assert(netconf_ds_commit(s2, NULL) == NETCONF_DS_LOCKED);
    assert(netconf_ds_discard(s1) == NETCONF_DS_LOCKED);
    assert(netconf_ds_lock(NETCONF_DS_STARTUP, s1, "s1") == NETCONF_DS_OK);
    assert(netconf_ds_delete_config(NETCONF_DS_STARTUP, s2)
           == NETCONF_DS_LOCKED);
    assert(netconf_ds_delete_config(NETCONF_DS_STARTUP, s1) == NETCONF_DS_OK);
    assert(netconf_ds_unlock(NETCONF_DS_STARTUP, s1) == NETCONF_DS_OK);
    assert(netconf_ds_unlock(NETCONF_DS_STARTUP, s1) == NETCONF_DS_LOCKED);

    /* Session release drops every lock owned by that session. */
    netconf_ds_release_session(s1);
    assert(!netconf_ds_locked_by_other(NETCONF_DS_RUNNING, s2));
    assert(!netconf_ds_locked_by_other(NETCONF_DS_CANDIDATE, s2));
    netconf_ds_release_session(s2);
    assert(!netconf_ds_locked_by_other(NETCONF_DS_CANDIDATE, s1));
    netconf_ds_release_session(0);       /* no-op */

    printf("[PASS] test_netconf_lock_matrix: every datastore/session lock "
           "transition and release\n");
    return 0;
}

/* ---- copy-config and startup persistence -------------------------------- */

int test_netconf_copy_and_startup(const char *path)
{
    netconf_ctx_t ctx;
    netconf_init(&ctx, 830);
    ctx.session_id = 71;
    seed_iface(7201, "start0", 1500, false);

    char *resp = nc_call(&ctx,
        "<rpc><copy-config><target><startup/></target>"
        "<source><running/></source></copy-config></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);

    resp = nc_call(&ctx,
        "<rpc><get-config><source><startup/></source></get-config></rpc>");
    assert(strstr(resp, "<name>start0</name>") != NULL);
    assert(strstr(resp, "<mtu>1500</mtu>") != NULL);
    free(resp);

    /* Startup can seed the candidate, which then commits to running. */
    resp = nc_call(&ctx,
        "<rpc><copy-config><target><candidate/></target>"
        "<source><startup/></source></copy-config></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);
    resp = nc_call(&ctx, "<rpc><commit/></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);

    /* The startup file is durable across a datastore reload. */
    netconf_ds_fini();
    netconf_ds_init(path);
    resp = nc_call(&ctx,
        "<rpc><get-config><source><startup/></source></get-config></rpc>");
    assert(strstr(resp, "<name>start0</name>") != NULL);
    free(resp);

    /* delete-config only accepts startup. */
    resp = nc_call(&ctx,
        "<rpc><delete-config><target><running/></target></delete-config></rpc>");
    assert(strstr(resp, "invalid-value") != NULL);
    free(resp);

    resp = nc_call(&ctx,
        "<rpc><delete-config><target><startup/></target></delete-config></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);

    resp = nc_call(&ctx,
        "<rpc><get-config><source><startup/></source></get-config></rpc>");
    assert(strstr(resp, "<name>start0</name>") == NULL);
    free(resp);

    assert(danos_object_delete(g_default_store, DANOS_OBJ_IFACE, 7201)
           == DANOS_OK);
    assert(netconf_ds_running_changed() == NETCONF_DS_OK);
    netconf_fini(&ctx);

    printf("[PASS] test_netconf_copy_and_startup: copy, reload, delete\n");
    return 0;
}

int test_netconf_corrupt_startup(const char *path)
{
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    assert(fwrite("not a netconf snapshot", 1, 22, f) == 22);
    fclose(f);

    netconf_ds_fini();
    netconf_ds_init(path);

    netconf_ctx_t ctx;
    netconf_init(&ctx, 830);
    ctx.session_id = 72;

    char *resp = nc_call(&ctx,
        "<rpc><get-config><source><startup/></source></get-config></rpc>");
    assert(strstr(resp, "invalid-value") != NULL);
    free(resp);

    /* delete-config is the recovery path and writes a valid empty file. */
    resp = nc_call(&ctx,
        "<rpc><delete-config><target><startup/></target></delete-config></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);

    resp = nc_call(&ctx,
        "<rpc><get-config><source><startup/></source></get-config></rpc>");
    assert(strstr(resp, "<data>") != NULL);
    assert(strstr(resp, "<name>") == NULL);
    free(resp);
    netconf_fini(&ctx);

    printf("[PASS] test_netconf_corrupt_startup: bad file rejected, recoverable\n");
    return 0;
}

/* ---- validate and confirmed commit -------------------------------------- */

int test_netconf_validate(void)
{
    netconf_ctx_t ctx;
    netconf_init(&ctx, 830);
    ctx.session_id = 71;
    seed_iface(7201, "val0", 1500, true);

    char *resp = nc_call(&ctx,
        "<rpc><validate><source><running/></source></validate></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);

    resp = nc_call(&ctx,
        "<rpc><validate><source><candidate/></source></validate></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);

    /* An inline config is checked without being applied. */
    resp = nc_call(&ctx,
        "<rpc><validate><config>"
        "<interfaces xmlns=\"urn:danos:yang:danos-iface\">"
        "<interface><name>val0</name><mtu>1400</mtu></interface>"
        "</interfaces></config></validate></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);
    danos_iface_t ifc;
    assert(find_iface("val0", &ifc));
    assert(ifc.mtu == 1500);

    resp = nc_call(&ctx,
        "<rpc><validate><config>"
        "<interfaces xmlns=\"urn:danos:yang:danos-iface\">"
        "<interface><name>val-missing</name><mtu>1400</mtu></interface>"
        "</interfaces></config></validate></rpc>");
    assert(strstr(resp, "rpc-error") != NULL);
    free(resp);

    resp = nc_call(&ctx,
        "<rpc><validate><config>"
        "<interfaces xmlns=\"urn:danos:yang:danos-iface\">"
        "<interface><name>val0</name><mtu>bogus</mtu></interface>"
        "</interfaces></config></validate></rpc>");
    assert(strstr(resp, "rpc-error") != NULL);
    free(resp);

    resp = nc_call(&ctx, "<rpc><validate/></rpc>");
    assert(strstr(resp, "invalid-value") != NULL);
    free(resp);

    assert(danos_object_delete(g_default_store, DANOS_OBJ_IFACE, 7201)
           == DANOS_OK);
    assert(netconf_ds_running_changed() == NETCONF_DS_OK);
    netconf_fini(&ctx);

    printf("[PASS] test_netconf_validate: datastore and inline validation\n");
    return 0;
}

static void edit_candidate_mtu(netconf_ctx_t *ctx, int ifindex,
                               const char *name, const char *mtu)
{
    char xml[512];
    snprintf(xml, sizeof(xml),
             "<rpc><edit-config><target><candidate/></target><config>"
             "<interfaces xmlns=\"urn:danos:yang:danos-iface\">"
             "<interface><name>%s</name><mtu>%s</mtu></interface>"
             "</interfaces></config></edit-config></rpc>",
             name, mtu);
    (void)ifindex;
    char *resp = nc_call(ctx, xml);
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);
}

int test_netconf_confirmed_commit(void)
{
    netconf_ctx_t ctx;
    netconf_init(&ctx, 830);
    ctx.session_id = 81;
    seed_iface(7202, "cc0", 1500, true);
    /* Drop any candidate snapshot left by earlier tests so the next edit
     * starts from the current running store, which now holds cc0. */
    assert(netconf_ds_discard(ctx.session_id) == NETCONF_DS_OK);
    danos_iface_t ifc;

    /* A persisted confirmed commit publishes immediately and stays armed. */
    edit_candidate_mtu(&ctx, 7202, "cc0", "9000");
    char *resp = nc_call(&ctx,
        "<rpc><commit><confirmed/><confirm-timeout>60</confirm-timeout>"
        "<persist>tok1</persist></commit></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);
    assert(netconf_ds_confirmed_pending());
    assert(find_iface("cc0", &ifc));
    assert(ifc.mtu == 9000);

    /* Only the matching persist-id may cancel or confirm. */
    resp = nc_call(&ctx,
        "<rpc><cancel-commit><persist-id>wrong</persist-id></cancel-commit></rpc>");
    assert(strstr(resp, "access-denied") != NULL);
    free(resp);
    assert(netconf_ds_confirmed_pending());

    resp = nc_call(&ctx,
        "<rpc><cancel-commit><persist-id>tok1</persist-id></cancel-commit></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);
    assert(!netconf_ds_confirmed_pending());
    assert(find_iface("cc0", &ifc));
    assert(ifc.mtu == 1500);

    /* Confirming commit keeps the published configuration. */
    edit_candidate_mtu(&ctx, 7202, "cc0", "8000");
    resp = nc_call(&ctx,
        "<rpc><commit><confirmed/><persist>tok2</persist></commit></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);
    assert(netconf_ds_confirmed_pending());

    resp = nc_call(&ctx,
        "<rpc><commit><persist-id>wrong</persist-id></commit></rpc>");
    assert(strstr(resp, "access-denied") != NULL);
    free(resp);

    resp = nc_call(&ctx,
        "<rpc><commit><persist-id>tok2</persist-id></commit></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);
    assert(!netconf_ds_confirmed_pending());
    assert(find_iface("cc0", &ifc));
    assert(ifc.mtu == 8000);

    /* Without a confirming commit the timer restores the baseline. */
    edit_candidate_mtu(&ctx, 7202, "cc0", "7000");
    resp = nc_call(&ctx,
        "<rpc><commit><confirmed/><confirm-timeout>1</confirm-timeout></commit></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);
    assert(find_iface("cc0", &ifc));
    assert(ifc.mtu == 7000);

    usleep(1500000);
    assert(!netconf_ds_confirmed_pending());
    assert(find_iface("cc0", &ifc));
    assert(ifc.mtu == 8000);

    /* Closing the issuing session also rolls a non-persisted commit back. */
    edit_candidate_mtu(&ctx, 7202, "cc0", "6000");
    resp = nc_call(&ctx, "<rpc><commit><confirmed/></commit></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);
    assert(find_iface("cc0", &ifc));
    assert(ifc.mtu == 6000);
    netconf_fini(&ctx);
    assert(!netconf_ds_confirmed_pending());
    assert(find_iface("cc0", &ifc));
    assert(ifc.mtu == 8000);

    assert(danos_object_delete(g_default_store, DANOS_OBJ_IFACE, 7202)
           == DANOS_OK);

    printf("[PASS] test_netconf_confirmed_commit: persist, confirm, cancel, "
           "timeout, session close\n");
    return 0;
}

/* RFC 6241 8.4: a pending confirmed commit must not survive a reboot.  The
 * parent arms one and forks/execs a fresh copy of this test binary; that
 * process initializes the datastore from the same durable state and must find
 * the pre-commit configuration restored. */
static int recover_check(const char *startup_path)
{
    netconf_ds_init(startup_path);
    danos_iface_t ifc;
    bool found = find_iface("rec0", &ifc);
    int rc = (found && ifc.mtu == 1500) ? 0 : 1;
    netconf_ds_fini();
    return rc;
}

static int running_check(const char *startup_path, unsigned expected_mtu)
{
    netconf_ds_init(startup_path);
    danos_iface_t ifc;
    bool found = find_iface("recp2", &ifc);
    int rc = (found && ifc.mtu == expected_mtu) ? 0 : 1;
    netconf_ds_fini();
    return rc;
}

int test_netconf_confirmed_recovery(const char *base_path)
{
    char path[160], confirmed[192];
    snprintf(path, sizeof(path), "%s.recover", base_path);
    snprintf(confirmed, sizeof(confirmed), "%s.confirmed", path);
    unlink(path);
    unlink(confirmed);

    netconf_ds_fini();
    netconf_ds_init(path);
    seed_iface(7301, "rec0", 1500, true);

    netconf_ctx_t ctx;
    netconf_init(&ctx, 830);
    ctx.session_id = 91;
    edit_candidate_mtu(&ctx, 7301, "rec0", "9000");
    char *resp = nc_call(&ctx,
        "<rpc><commit><confirmed/><confirm-timeout>60</confirm-timeout>"
        "<persist>reboot-token</persist></commit></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);
    danos_iface_t ifc;
    assert(find_iface("rec0", &ifc));
    assert(ifc.mtu == 9000);
    assert(access(confirmed, F_OK) == 0);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        execl("/proc/self/exe", "netconf_test", "--recover-check", path,
              (char *)NULL);
        _exit(127);
    }
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    /* The recovery process removed the pending state after rollback. */
    assert(access(confirmed, F_OK) != 0);

    resp = nc_call(&ctx,
        "<rpc><cancel-commit><persist-id>reboot-token</persist-id>"
        "</cancel-commit></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);
    assert(find_iface("rec0", &ifc));
    assert(ifc.mtu == 1500);

    netconf_fini(&ctx);
    netconf_ds_fini();
    unlink(path);
    unlink(confirmed);

    printf("[PASS] test_netconf_confirmed_recovery: reboot rolls back an "
           "unconfirmed persisted commit\n");
    return 0;
}

int test_netconf_running_persistence(const char *base_path)
{
    char path[160], running[192], confirmed[192];
    snprintf(path, sizeof(path), "%s.run", base_path);
    snprintf(running, sizeof(running), "%s.running", path);
    snprintf(confirmed, sizeof(confirmed), "%s.confirmed", path);
    unlink(path);
    unlink(running);
    unlink(confirmed);

    netconf_ds_fini();
    netconf_ds_init(path);
    seed_iface(7399, "recp2", 1400, true);

    netconf_ctx_t ctx;
    netconf_init(&ctx, 830);
    ctx.session_id = 92;
    edit_candidate_mtu(&ctx, 7399, "recp2", "9000");
    char *resp = nc_call(&ctx, "<rpc><commit/></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);
    danos_iface_t ifc;
    assert(find_iface("recp2", &ifc));
    assert(ifc.mtu == 9000);
    assert(access(running, F_OK) == 0);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        execl("/proc/self/exe", "netconf_test", "--running-check", path,
              "9000", (char *)NULL);
        _exit(127);
    }
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);

    netconf_fini(&ctx);
    netconf_ds_fini();
    unlink(path);
    unlink(running);
    unlink(confirmed);

    printf("[PASS] test_netconf_running_persistence: a plain commit survives "
           "a fresh process restart\n");
    return 0;
}

/* A durable-write failure must fail the commit and restore the pre-commit
 * running state rather than acknowledging state that would not survive a
 * restart.  Pointing the running path at a directory makes the rename fail. */
int test_netconf_running_save_failure(const char *base_path)
{
    char path[160], dir[192];
    snprintf(path, sizeof(path), "%s.savefail", base_path);
    snprintf(dir, sizeof(dir), "%s.dir", path);
    unlink(path);
    rmdir(dir);
    assert(mkdir(dir, 0700) == 0);

    netconf_ds_fini();
    setenv("DANOS_NETCONF_RUNNING", dir, 1);
    setenv("DANOS_NETCONF_ALLOW_CORRUPT_RUNNING", "1", 1);
    netconf_ds_init(path);
    assert(netconf_ds_ready());
    seed_iface(7401, "sf0", 1500, true);

    netconf_ctx_t ctx;
    netconf_init(&ctx, 830);
    ctx.session_id = 93;
    edit_candidate_mtu(&ctx, 7401, "sf0", "9000");
    char *resp = nc_call(&ctx, "<rpc><commit/></rpc>");
    assert(strstr(resp, "operation-failed") != NULL);
    free(resp);
    danos_iface_t ifc;
    assert(find_iface("sf0", &ifc));
    assert(ifc.mtu == 1500);        /* rolled back after the failed write */
    resp = nc_call(&ctx, "<rpc><discard-changes/></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);

    unsetenv("DANOS_NETCONF_RUNNING");
    unsetenv("DANOS_NETCONF_ALLOW_CORRUPT_RUNNING");
    netconf_fini(&ctx);
    netconf_ds_fini();
    rmdir(dir);
    unlink(path);
    printf("[PASS] test_netconf_running_save_failure: commit fails and "
           "restores state when running cannot be persisted\n");
    return 0;
}

/* The same fail-closed rule for the pending confirmed-commit file. */
int test_netconf_confirmed_save_failure(const char *base_path)
{
    char path[160], dir[192];
    snprintf(path, sizeof(path), "%s.cfsavefail", base_path);
    snprintf(dir, sizeof(dir), "%s.dir", path);
    unlink(path);
    rmdir(dir);
    assert(mkdir(dir, 0700) == 0);

    netconf_ds_fini();
    setenv("DANOS_NETCONF_CONFIRMED", dir, 1);
    netconf_ds_init(path);
    seed_iface(7402, "cf0", 1500, true);

    netconf_ctx_t ctx;
    netconf_init(&ctx, 830);
    ctx.session_id = 94;
    edit_candidate_mtu(&ctx, 7402, "cf0", "9000");
    char *resp = nc_call(&ctx,
        "<rpc><commit><confirmed/><confirm-timeout>60</confirm-timeout>"
        "</commit></rpc>");
    assert(strstr(resp, "operation-failed") != NULL);
    free(resp);
    danos_iface_t ifc;
    assert(find_iface("cf0", &ifc));
    assert(ifc.mtu == 1500);
    assert(!netconf_ds_confirmed_pending());
    resp = nc_call(&ctx, "<rpc><discard-changes/></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);

    unsetenv("DANOS_NETCONF_CONFIRMED");
    netconf_fini(&ctx);
    netconf_ds_fini();
    rmdir(dir);
    unlink(path);
    printf("[PASS] test_netconf_confirmed_save_failure: armed commit fails "
           "closed when the rollback file cannot be written\n");
    return 0;
}

int test_netconf_datastore_api_errors(void)
{
    uint8_t buf[64];
    size_t size = sizeof(buf);
    assert(netconf_ds_read((netconf_datastore_t)0, DANOS_OBJ_IFACE, 1,
                           (void **)&buf, &size) == NETCONF_DS_INVALID);
    assert(netconf_ds_write(NETCONF_DS_RUNNING, DANOS_OBJ_IFACE, 1,
                            buf, sizeof(buf)) == NETCONF_DS_INVALID);
    assert(netconf_ds_delete(NETCONF_DS_RUNNING, DANOS_OBJ_IFACE, 1)
           == NETCONF_DS_INVALID);
    assert(netconf_ds_validate((netconf_datastore_t)0)
           == NETCONF_DS_INVALID);
    assert(netconf_ds_copy((netconf_datastore_t)0, NETCONF_DS_RUNNING, 1)
           == NETCONF_DS_INVALID);
    assert(netconf_ds_copy(NETCONF_DS_RUNNING, NETCONF_DS_RUNNING, 1)
           == NETCONF_DS_OK);
    assert(netconf_ds_delete_config(NETCONF_DS_RUNNING, 1)
           == NETCONF_DS_INVALID);
    assert(netconf_ds_lock((netconf_datastore_t)0, 1, "x")
           == NETCONF_DS_INVALID);
    assert(netconf_ds_unlock((netconf_datastore_t)0, 1)
           == NETCONF_DS_INVALID);
    assert(!netconf_ds_locked_by_other((netconf_datastore_t)0, 1));

    netconf_commit_opts_t opts = {0};
    opts.persist = "token";
    assert(netconf_ds_commit(1, &opts) == NETCONF_DS_INVALID);
    opts = (netconf_commit_opts_t){ .confirmed = true, .persist_id = "x" };
    assert(netconf_ds_commit(1, &opts) == NETCONF_DS_INVALID);
    char too_long[128];
    memset(too_long, 'a', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    opts = (netconf_commit_opts_t){ .confirmed = true, .persist = too_long };
    assert(netconf_ds_commit(1, &opts) == NETCONF_DS_INVALID);
    assert(netconf_ds_cancel_commit(1, "nope") == NETCONF_DS_INVALID);

    printf("[PASS] test_netconf_datastore_api_errors: invalid arguments and "
           "option combinations are refused\n");
    return 0;
}

/* A damaged running snapshot must not silently load as an empty config.
 * Without the override, startup is refused; with it, the daemon may start
 * empty and the next commit replaces the bad file. */
int test_netconf_corrupt_running(const char *base_path)
{
    char path[160], running[192];
    snprintf(path, sizeof(path), "%s.corruptrun", base_path);
    snprintf(running, sizeof(running), "%s.running", path);
    unlink(path);
    unlink(running);
    FILE *f = fopen(running, "wb");
    assert(f != NULL);
    assert(fwrite("not a running snapshot", 1, 22, f) == 22);
    fclose(f);

    netconf_ds_fini();
    unsetenv("DANOS_NETCONF_ALLOW_CORRUPT_RUNNING");
    netconf_ds_init(path);
    assert(!netconf_ds_ready());
    netconf_ds_init(path);          /* repeated init stays failed */
    assert(!netconf_ds_ready());
    netconf_ds_fini();

    setenv("DANOS_NETCONF_ALLOW_CORRUPT_RUNNING", "1", 1);
    netconf_ds_init(path);
    assert(netconf_ds_ready());
    seed_iface(7501, "cr0", 1500, true);
    netconf_ctx_t ctx;
    netconf_init(&ctx, 830);
    ctx.session_id = 95;
    edit_candidate_mtu(&ctx, 7501, "cr0", "9000");
    char *resp = nc_call(&ctx, "<rpc><commit/></rpc>");
    assert(strstr(resp, "<ok/>") != NULL);
    free(resp);
    netconf_fini(&ctx);
    netconf_ds_fini();

    /* The override allowed a commit to replace the damaged file. */
    unsetenv("DANOS_NETCONF_ALLOW_CORRUPT_RUNNING");
    netconf_ds_init(path);
    assert(netconf_ds_ready());
    danos_iface_t ifc;
    assert(find_iface("cr0", &ifc));
    assert(ifc.mtu == 9000);
    netconf_ds_fini();

    unlink(running);
    unlink(path);
    printf("[PASS] test_netconf_corrupt_running: damaged state refuses startup "
           "unless explicitly overridden\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--recover-check") == 0)
        return recover_check(argv[2]);
    if (argc == 4 && strcmp(argv[1], "--running-check") == 0)
        return running_check(argv[2], (unsigned)strtoul(argv[3], NULL, 10));

    char path[128];
    snprintf(path, sizeof(path), "/tmp/danos-netconf-test-%ld.db",
             (long)getpid());
    unlink(path);
    setenv("DANOS_NETCONF_STARTUP", path, 1);
    netconf_ds_fini();
    netconf_ds_init(path);

    int failed = 0;
    if (test_netconf_parse() != 0) failed++;
    if (test_netconf_hello() != 0) failed++;
    if (test_netconf_handle_rpc() != 0) failed++;
    if (test_netconf_get_config_large() != 0) failed++;
    if (test_netconf_escapes_name() != 0) failed++;
    if (test_netconf_edit_config_multi() != 0) failed++;
    if (test_netconf_candidate() != 0) failed++;
    if (test_netconf_startup_readonly() != 0) failed++;
    if (test_netconf_locks() != 0) failed++;
    if (test_netconf_lock_matrix() != 0) failed++;
    if (test_netconf_copy_and_startup(path) != 0) failed++;
    if (test_netconf_corrupt_startup(path) != 0) failed++;
    if (test_netconf_validate() != 0) failed++;
    if (test_netconf_confirmed_commit() != 0) failed++;
    if (test_netconf_confirmed_recovery(path) != 0) failed++;
    if (test_netconf_running_persistence(path) != 0) failed++;
    if (test_netconf_running_save_failure(path) != 0) failed++;
    if (test_netconf_confirmed_save_failure(path) != 0) failed++;
    if (test_netconf_datastore_api_errors() != 0) failed++;
    if (test_netconf_corrupt_running(path) != 0) failed++;

    netconf_ds_fini();
    char aux[192];
    snprintf(aux, sizeof(aux), "%s.running", path);
    unlink(aux);
    snprintf(aux, sizeof(aux), "%s.confirmed", path);
    unlink(aux);
    snprintf(aux, sizeof(aux), "%s.recover", path);
    unlink(aux);
    snprintf(aux, sizeof(aux), "%s.recover.running", path);
    unlink(aux);
    snprintf(aux, sizeof(aux), "%s.recover.confirmed", path);
    unlink(aux);
    unlink(path);
    printf("=== netconf_test: %s ===\n",
           failed == 0 ? "ALL PASSED" : "FAILURES");
    return failed;
}
