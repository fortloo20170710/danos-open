/*
 * VPP Backend Capability registration (D7)
 *
 * Registers VPP backend with the DPA capability registry,
 * advertising supported object types and limits.
 */

#include <danos/dpa.h>
#include <string.h>

/*
 * What this backend can actually program.
 *
 * The programming pipeline in backend_ops.c programs exactly three object
 * types (type_skipped() returns true for everything else), and the VPP
 * adapter implements ops for those three only: iface_up, iface_addr_set,
 * iface_addr_del, route_add, route_del, vrf_add, vrf_del.
 *
 * A capability must therefore be advertised only for a type the pipeline
 * can really program. Claiming more is worse than an empty registry: it
 * makes capability-based backend selection steer traffic to a backend that
 * silently drops those objects, and it breaks the contract that adding a
 * backend means implementing the ops and registering them.
 */

/* IPv4 unicast plus equal-weight multipath. Not IPv6: route_add/route_del
 * reject anything but DANOS_AF_IPV4 (DANOS_ERR_NOT_SUPPORTED), so claiming
 * v6 would promise forwarding that cannot happen. */
static const char *kRouteFeatures[] = {"ipv4", "multipath"};

/* IFACE is limited by what the adapter can program, not by any VPP limit. */
static const danos_capability_t kVppCaps[] = {
    {DANOS_OBJ_IFACE,     true, 1024,    0, NULL,            NULL},
    {DANOS_OBJ_VRF,       true, 4096,    0, NULL,            NULL},
    {DANOS_OBJ_ROUTE,     true, 1000000, 2, kRouteFeatures,  NULL},

    /* Present in the object model but not programmable through this
     * backend: no adapter op and skipped by the pipeline. */
    {DANOS_OBJ_VLAN,      false, 0,      0, NULL,            NULL},
    {DANOS_OBJ_NEXTHOP,   false, 0,      0, NULL,            NULL},
    {DANOS_OBJ_NHGROUP,   false, 0,      0, NULL,            NULL},
    {DANOS_OBJ_ACL,       false, 0,      0, NULL,            NULL},
    {DANOS_OBJ_QOS,       false, 0,      0, NULL,            NULL},
    {DANOS_OBJ_QOS_BIND,  false, 0,      0, NULL,            NULL},
    {DANOS_OBJ_MPLS_LSP,  false, 0,      0, NULL,            NULL},
    {DANOS_OBJ_TUNNEL,    false, 0,      0, NULL,            NULL},
    {DANOS_OBJ_EVPN,      false, 0,      0, NULL,            NULL},
    {DANOS_OBJ_MULTICAST, false, 0,      0, NULL,            NULL},
    {DANOS_OBJ_BFD,       false, 0,      0, NULL,            NULL},
};

int danos_vpp_capability_register(void)
{
    danos_backend_info_t be;
    memset(&be, 0, sizeof(be));
    strncpy(be.name, "vpp", sizeof(be.name) - 1);
    be.api_version = danos_dpa_get_version();
    be.cap_count = sizeof(kVppCaps) / sizeof(kVppCaps[0]);
    be.caps = kVppCaps;
    return (int)danos_backend_register(&be);
}
