/* Exercise real adapter serialization without mutating the host network. */
#include <danos/core/backend_ops.h>
#include "../../danos-netlink/danos_netlink.h"
#include <assert.h>
#include <errno.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>

_Alignas(struct nlmsghdr) static unsigned char request[8192];
static size_t request_size;
static int reply_error;
static bool fake_timeout, dump_mode, dump_done;
static unsigned opens, closes;
_Alignas(struct nlmsghdr) static unsigned char stored_route[8192];
static size_t stored_size;
int __wrap_socket(int domain, int type, int protocol)
{ (void)type; assert(domain == AF_NETLINK && protocol == NETLINK_ROUTE); opens++; return 777; }
int __wrap_close(int fd) { assert(fd == 777); closes++; return 0; }
int __wrap_setsockopt(int fd, int level, int option, const void *value, socklen_t len)
{ (void)level; (void)option; (void)value; (void)len; assert(fd == 777); return 0; }
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout)
{ (void)timeout; assert(count == 1); if (fake_timeout) return 0; fds[0].revents = POLLIN; return 1; }
ssize_t __wrap_send(int fd, const void *buf, size_t len, int flags)
{
    (void)flags; assert(fd == 777 && len <= sizeof(request));
    memcpy(request, buf, len); request_size = len;
    const struct nlmsghdr *nh = buf;
    if (nh->nlmsg_type == RTM_NEWROUTE) { memcpy(stored_route, buf, len); stored_size = len; }
    dump_done = false;
    return (ssize_t)len;
}
ssize_t __wrap_recv(int fd, void *buf, size_t len, int flags)
{
    (void)flags; assert(fd == 777);
    if (dump_mode) {
        if (!dump_done) {
            assert(stored_size <= len);
            memcpy(buf, stored_route, stored_size);
            struct nlmsghdr *nh = buf;
            nh->nlmsg_seq = ((struct nlmsghdr *)request)->nlmsg_seq;
            nh->nlmsg_flags = NLM_F_MULTI;
            dump_done = true;
            return (ssize_t)stored_size;
        }
        struct { struct nlmsghdr nh; int error; } done = {0};
        done.nh.nlmsg_len = NLMSG_LENGTH(sizeof(int)); done.nh.nlmsg_type = NLMSG_DONE;
        done.nh.nlmsg_seq = ((struct nlmsghdr *)request)->nlmsg_seq;
        memcpy(buf, &done, done.nh.nlmsg_len);
        return done.nh.nlmsg_len;
    }
    struct { struct nlmsghdr nh; struct nlmsgerr err; } reply;
    memset(&reply, 0, sizeof(reply));
    reply.nh.nlmsg_len = NLMSG_LENGTH(sizeof(reply.err));
    reply.nh.nlmsg_type = NLMSG_ERROR;
    reply.nh.nlmsg_seq = ((struct nlmsghdr *)request)->nlmsg_seq;
    reply.err.error = reply_error;
    assert(len >= reply.nh.nlmsg_len);
    memcpy(buf, &reply, reply.nh.nlmsg_len);
    return reply.nh.nlmsg_len;
}
static struct rtattr *find_attr(unsigned type)
{
    struct nlmsghdr *nh = (struct nlmsghdr *)request;
    assert(nh->nlmsg_len == request_size);
    struct rtmsg *rt = NLMSG_DATA(nh);
    int remaining = (int)nh->nlmsg_len - NLMSG_LENGTH(sizeof(*rt));
    for (struct rtattr *a = RTM_RTA(rt); RTA_OK(a, remaining); a = RTA_NEXT(a, remaining))
        if (a->rta_type == type) return a;
    return NULL;
}
int main(void)
{
    assert(danos_netlink_init(true) == 0);
    danos_netlink_register_backend();
    const danos_backend_ops_t *ops = danos_backend_ops_get();
    danos_resolved_route_t route = {0};
    route.route.prefix.addr.af = DANOS_AF_IPV4;
    route.route.prefix.addr.addr[0] = 203;
    route.route.prefix.prefix_len = 24;
    const uint32_t tables[] = {0, 42, 255, 256, 257, 65537};
    for (unsigned i = 0; i < sizeof(tables)/sizeof(tables[0]); i++) {
        route.route.vrf_id = tables[i];
        assert(ops->route_add(&route, NULL) == DANOS_OK);
        struct rtmsg *rt = NLMSG_DATA((struct nlmsghdr *)request);
        assert(rt->rtm_table == (tables[i] == 0 ? RT_TABLE_MAIN :
                               tables[i] < 256 ? tables[i] : RT_TABLE_UNSPEC));
        struct rtattr *table = find_attr(RTA_TABLE);
        if (tables[i] >= 256) {
            uint32_t value;
            assert(table && RTA_PAYLOAD(table) == sizeof(value));
            memcpy(&value, RTA_DATA(table), sizeof(value));
            assert(value == tables[i]);
        } else assert(!table);
        assert(ops->route_del(&route, NULL) == DANOS_OK);
        assert((find_attr(RTA_TABLE) != NULL) == (tables[i] >= 256));
        reply_error = -ENOENT;
        danos_route_t oper;
        assert(ops->readback(DANOS_OBJ_ROUTE, 1, &route.route, sizeof(oper), &oper, NULL)
               == DANOS_ERR_NOT_FOUND);
        assert((find_attr(RTA_TABLE) != NULL) == (tables[i] >= 256));
        reply_error = 0;
    }
    route.nh_count = 2;
    route.has_gw = true;
    for (unsigned i = 0; i < 2; i++) {
        route.nh_gw[i][0] = 192; route.nh_gw[i][3] = (uint8_t)(i+1);
        route.nh_oif[i] = 10+i;
    }
    assert(ops->route_add(&route, NULL) == DANOS_OK);
    struct rtattr *mp = find_attr(RTA_MULTIPATH);
    assert(mp && !find_attr(RTA_GATEWAY) && !find_attr(RTA_OIF));
    int remaining = RTA_PAYLOAD(mp);
    struct rtnexthop *nh = RTA_DATA(mp);
    for (unsigned i = 0; i < 2; i++) {
        assert(RTNH_OK(nh, remaining) && nh->rtnh_ifindex == (int)(10+i));
        struct rtattr *gw = RTNH_DATA(nh);
        assert(gw->rta_type == RTA_GATEWAY && RTA_PAYLOAD(gw) == 4);
        assert(memcmp(RTA_DATA(gw), route.nh_gw[i], 4) == 0);
        remaining -= RTNH_ALIGN(nh->rtnh_len); nh = RTNH_NEXT(nh);
    }
    assert(remaining == 0);
    route.nh_count = 64;
    assert(ops->route_add(&route, NULL) == DANOS_OK);
    mp = find_attr(RTA_MULTIPATH);
    assert(mp && RTA_PAYLOAD(mp) == 64 * sizeof(struct rtnexthop) + 2 * RTA_SPACE(4));
    route.route.flags = DANOS_ROUTE_FLAG_BLACKHOLE;
    assert(ops->route_add(&route, NULL) == DANOS_OK);
    assert(!find_attr(RTA_MULTIPATH) && !find_attr(RTA_GATEWAY));
    route.route.flags = 0;
    route.nh_count = 0;
    route.gw[0] = 192; route.gw[3] = 9; route.oif = 7;
    assert(ops->route_add(&route, NULL) == DANOS_OK);
    assert(!find_attr(RTA_MULTIPATH));
    assert(find_attr(RTA_GATEWAY) && find_attr(RTA_OIF));
    assert(memcmp(RTA_DATA(find_attr(RTA_GATEWAY)), route.gw, 4) == 0);
    uint32_t oif;
    memcpy(&oif, RTA_DATA(find_attr(RTA_OIF)), sizeof(oif));
    assert(oif == route.oif);
    route.nh_count = 65;
    assert(ops->route_add(&route, NULL) != DANOS_OK);
    route.nh_count = 2;
    assert(ops->route_add(&route, NULL) == DANOS_OK);
    dump_mode = true;
    assert(ops->route_readback(&route, &(danos_route_t){0}, NULL) == DANOS_OK);
    route.nh_gw[0][3]++;
    assert(ops->route_readback(&route, &(danos_route_t){0}, NULL) == DANOS_ERR_VERIFY_FAIL);
    route.nh_gw[0][3]--;
    route.nh_count = 1;
    assert(ops->route_readback(&route, &(danos_route_t){0}, NULL) == DANOS_ERR_VERIFY_FAIL);
    route.nh_count = 2;
    fake_timeout = true;
    unsigned before_close = closes;
    assert(ops->route_readback(&route, &(danos_route_t){0}, NULL) == DANOS_ERR_BACKEND_IO);
    assert(closes == before_close + 1);
    fake_timeout = false; dump_mode = false;
    unsigned before_open = opens;
    assert(ops->route_add(&route, NULL) == DANOS_OK);
    assert(opens == before_open + 1); /* fresh socket, no stale timed-out ACK */
    danos_vrf_t vrf = {0};
    assert(ops->vrf_add(&vrf, NULL) == DANOS_ERR_NOT_SUPPORTED);
    assert(ops->vrf_del(257, NULL) == DANOS_ERR_NOT_SUPPORTED);
    danos_netlink_shutdown();
    puts("[PASS] real adapter table isolation, multipath and unsupported VRF");
    return 0;
}
