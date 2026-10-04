/*
 * Test: NETCONF Server (E3)
 */
#include "../src/netconf/netconf.h"
#include <danos/core/object_registry.h>
#include <danos/dpa.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

int test_netconf_parse(void)
{
    assert(netconf_parse_rpc("<rpc><get-config/></rpc>") == NETCONF_RPC_GET_CONFIG);
    assert(netconf_parse_rpc("<rpc><edit-config/></rpc>") == NETCONF_RPC_EDIT_CONFIG);
    assert(netconf_parse_rpc("<rpc><commit/></rpc>") == NETCONF_RPC_COMMIT);
    assert(netconf_parse_rpc("<rpc><discard-changes/></rpc>") == NETCONF_RPC_DISCARD);
    assert(netconf_parse_rpc("<rpc><close-session/></rpc>") == NETCONF_RPC_CLOSE_SESSION);
    assert(netconf_parse_rpc("<rpc><lock/></rpc>") == NETCONF_RPC_LOCK);
    assert(netconf_parse_rpc("<rpc><unknown-op/></rpc>") == NETCONF_RPC_UNKNOWN);
    assert(netconf_parse_rpc(NULL) == NETCONF_RPC_UNKNOWN);

    printf("[PASS] test_netconf_parse: RPC type detection\n");
    return 0;
}

int test_netconf_hello(void)
{
    char *hello = netconf_hello_message();
    assert(hello != NULL);
    assert(strstr(hello, "hello") != NULL);
    assert(strstr(hello, "capabilities") != NULL);
    assert(strstr(hello, "netconf:base:1.0") != NULL);
    assert(strstr(hello, "netconf:base:1.1") != NULL);
    assert(strstr(hello, "danos:yang") != NULL);
    free(hello);

    printf("[PASS] test_netconf_hello: hello message has capabilities\n");
    return 0;
}

int test_netconf_handle_rpc(void)
{
    netconf_ctx_t ctx;
    netconf_init(&ctx, 830);

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

    /* commit */
    resp = netconf_handle_rpc(&ctx, "<rpc><commit/></rpc>");
    assert(resp != NULL);
    assert(strstr(resp, "ok") != NULL);
    free(resp);

    /* unknown */
    resp = netconf_handle_rpc(&ctx, "<rpc><foobar/></rpc>");
    assert(resp != NULL);
    assert(strstr(resp, "rpc-error") != NULL);
    free(resp);

    /* Stats */
    uint64_t rpc_count, error_count;
    netconf_get_stats(&ctx, &rpc_count, &error_count);
    assert(rpc_count == 4);
    assert(error_count == 1);

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

    seed_iface(4242, "eth0</name><injected>x", 1500, true);

    char *resp = netconf_handle_rpc(&ctx,
        "<rpc><get-config><source><running/></source></get-config></rpc>");
    assert(resp != NULL);
    assert(strstr(resp, "<injected>") == NULL);
    assert(strstr(resp, "&lt;/name&gt;") != NULL);
    free(resp);

    assert(danos_object_delete(g_default_store, DANOS_OBJ_IFACE, 4242)
           == DANOS_OK);
    printf("[PASS] test_netconf_escapes_name: markup in name is escaped\n");
    return 0;
}

int main(void)
{
    int failed = 0;
    if (test_netconf_parse() != 0) failed++;
    if (test_netconf_hello() != 0) failed++;
    if (test_netconf_handle_rpc() != 0) failed++;
    if (test_netconf_get_config_large() != 0) failed++;
    if (test_netconf_escapes_name() != 0) failed++;
    printf("=== netconf_test: %s ===\n",
           failed == 0 ? "ALL PASSED" : "FAILURES");
    return failed;
}
