/*
 * Test: real rtnetlink IPv6 address programming and readback.
 *
 * Adds a documentation-prefix address to loopback, verifies readback and
 * duplicate-add idempotence, then removes it and checks the address is gone.
 * Skips when the process lacks CAP_NET_ADMIN or a real netlink socket.
 */
#include <danos/core/backend_ops.h>
#include "../../danos-netlink/danos_netlink.h"
#include <assert.h>
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>

extern int danos_netlink_last_kernel_error;

static danos_ip_prefix_t v6_prefix(void)
{
    danos_ip_prefix_t p;
    memset(&p, 0, sizeof(p));
    p.addr.af = DANOS_AF_IPV6;
    p.prefix_len = 128;
    p.addr.addr[0] = 0x20;
    p.addr.addr[1] = 0x01;
    p.addr.addr[2] = 0x0d;
    p.addr.addr[3] = 0xb8;
    p.addr.addr[15] = 0x42;
    return p;
}

static bool same_v6(const danos_ip_prefix_t *a, const danos_ip_prefix_t *b)
{
    return a->addr.af == DANOS_AF_IPV6 && b->addr.af == DANOS_AF_IPV6 &&
           a->prefix_len == b->prefix_len &&
           memcmp(a->addr.addr, b->addr.addr, 16) == 0;
}

int main(void)
{
    if (danos_netlink_init(true) != 0 || !danos_netlink_is_real()) {
        puts("[SKIP] real rtnetlink socket unavailable");
        return 0;
    }
    danos_netlink_register_backend();
    const danos_backend_ops_t *ops = danos_backend_ops_get();
    assert(ops && ops->iface_addr_set && ops->iface_addr_del && ops->readback);

    unsigned ifindex = if_nametoindex("lo");
    assert(ifindex != 0);
    danos_iface_t iface;
    memset(&iface, 0, sizeof(iface));
    iface.ifindex = ifindex;
    snprintf(iface.name, sizeof(iface.name), "lo");
    iface.ipv6_address = v6_prefix();

    danos_status_t st = ops->iface_addr_set(&iface, ops->user);
    if (st == DANOS_ERR_BACKEND_IO &&
        (danos_netlink_last_kernel_error == -EPERM ||
         danos_netlink_last_kernel_error == -EACCES)) {
        puts("[SKIP] CAP_NET_ADMIN unavailable");
        danos_netlink_shutdown();
        return 0;
    }
    assert(st == DANOS_OK);

    /* Duplicate add must be idempotent. */
    assert(ops->iface_addr_set(&iface, ops->user) == DANOS_OK);

    danos_iface_t oper;
    assert(ops->readback(DANOS_OBJ_IFACE, ifindex, &iface, sizeof(iface),
                         &oper, ops->user) == DANOS_OK);
    assert(same_v6(&oper.ipv6_address, &iface.ipv6_address));

    /* Invalid prefixes are rejected before any kernel message. */
    danos_iface_t bad = iface;
    bad.ipv6_address.prefix_len = 129;
    assert(ops->iface_addr_set(&bad, ops->user) == DANOS_ERR_INVALID_ARG);
    bad = iface;
    bad.ipv4_address.addr.af = DANOS_AF_IPV4;
    bad.ipv4_address.prefix_len = 33;
    assert(ops->iface_addr_set(&bad, ops->user) == DANOS_ERR_INVALID_ARG);
    assert(ops->iface_addr_set(&bad, ops->user) == DANOS_ERR_INVALID_ARG);

    assert(ops->iface_addr_del(&iface, ops->user) == DANOS_OK);
    assert(ops->readback(DANOS_OBJ_IFACE, ifindex, &iface, sizeof(iface),
                         &oper, ops->user) == DANOS_OK);
    assert(!same_v6(&oper.ipv6_address, &iface.ipv6_address));
    /* Repeated removal is harmless. */
    assert(ops->iface_addr_del(&iface, ops->user) == DANOS_OK);

    danos_netlink_shutdown();
    printf("[PASS] netlink IPv6 add/readback/duplicate/remove and invalid "
           "prefix handling\n");
    return 0;
}
