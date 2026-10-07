/*
 * DPA Conformance test cases
 *
 * These verify the DPA API contract. Each test creates a transaction,
 * performs operations, and checks results. This is the storage-side contract,
 * not a real Linux/VPP dataplane qualification.
 *
 * Route/NH/NHGroup lifecycle cases live in route_lifecycle_cases.c. The
 * remaining object cases below assert the public storage CRUD contract.
 */

#include <danos/dpa.h>
#include <danos/core/capability_registry.h>
#include <assert.h>
#include <string.h>
#include <time.h>

/* Helper: get monotonic ns timestamp */
__attribute__((unused))
static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Helper: default timeouts */
static const danos_tx_timeouts_t kDefaultTimeouts = {
    .prepare_ms = 100,
    .commit_ms  = 500,
    .verify_ms  = 1000,
};

static danos_status_t conf_commit(danos_tx_t *tx)
{
    danos_status_t st = danos_tx_prepare(tx);
    if (st == DANOS_OK) st = danos_tx_validate(tx);
    if (st == DANOS_OK) st = danos_tx_commit(tx);
    if (st == DANOS_OK) st = danos_tx_verify(tx);
    return st;
}

/* ----------------------------------------------------------------------- */
/* 1. Transaction lifecycle: begin → prepare → validate → commit → done     */
/* ----------------------------------------------------------------------- */
int conf_tx_lifecycle(void)
{
    danos_tx_t tx = {0};
    danos_status_t st;

    st = danos_tx_begin(&tx, "conformance", &kDefaultTimeouts);
    if (st != DANOS_OK) return 1;
    if (tx.id == 0) return 2;

    st = danos_tx_prepare(&tx);
    if (st != DANOS_OK) return 3;

    st = danos_tx_validate(&tx);
    if (st != DANOS_OK) return 4;

    st = danos_tx_commit(&tx);
    if (st != DANOS_OK) return 5;

    /* Verify is best-effort; may not be implemented in skeleton */
    danos_tx_verify(&tx);

    danos_tx_state_t state;
    danos_tx_get_state(&tx, &state);
    /* After commit+verify, state should be DONE or VERIFY */
    if (state != DANOS_TX_DONE && state != DANOS_TX_VERIFY) return 6;

    return 0;
}

/* ----------------------------------------------------------------------- */
/* 2. Transaction abort                                                     */
/* ----------------------------------------------------------------------- */
int conf_tx_concurrent(void)
{
    danos_tx_t tx1 = {0}, tx2 = {0};
    danos_status_t st;

    st = danos_tx_begin(&tx1, "conf-1", &kDefaultTimeouts);
    if (st != DANOS_OK) return 1;

    st = danos_tx_begin(&tx2, "conf-2", &kDefaultTimeouts);
    if (st != DANOS_OK) return 2;

    /* Abort tx1 */
    st = danos_tx_abort(&tx1);
    if (st != DANOS_OK) return 3;

    /* Full lifecycle for tx2: prepare → validate → commit → verify */
    st = danos_tx_prepare(&tx2);
    if (st != DANOS_OK) return 4;
    st = danos_tx_validate(&tx2);
    if (st != DANOS_OK) return 5;
    st = danos_tx_commit(&tx2);
    if (st != DANOS_OK) return 6;
    st = danos_tx_verify(&tx2);
    if (st != DANOS_OK) return 7;

    return 0;
}

/* ----------------------------------------------------------------------- */
/* 3-9. Object CRUD skeletons                                               */
/* Each creates an object, reads it back, updates, deletes.                 */
/* For v0.1 skeleton, we verify the API is callable.                        */
/* ----------------------------------------------------------------------- */
int conf_iface_crud(void)
{
    danos_tx_t tx = {0};
    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 1;

    danos_iface_t iface = {0};
    iface.ifindex = 1;
    iface.type = DANOS_IF_TYPE_PHYS;
    iface.mtu = 1500;
    strncpy(iface.name, "eth0", sizeof(iface.name) - 1);
    iface.admin_up = true;

    /* This used to discard the status and return 0 unconditionally, so the
     * case passed whether or not the CRUD worked - which is how a v0.1
     * "11/11" could be vacuous. Assert the contract instead. */
    if (danos_iface_create(&tx, &iface) != DANOS_OK) return 10;
    if (danos_tx_prepare(&tx) != DANOS_OK) return 11;
    if (danos_tx_validate(&tx) != DANOS_OK) return 12;
    if (danos_tx_commit(&tx) != DANOS_OK) return 13;

    /* Read it back through the public API: create must actually persist. */
    danos_iface_t out;
    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 14;
    if (danos_iface_read(&tx, 1, &out) != DANOS_OK) return 15;
    if (out.mtu != 1500) return 16;
    if (danos_tx_abort(&tx) != DANOS_OK) return 17;

    /* Creating the same id again must be refused, not silently accepted. */
    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 18;
    if (danos_iface_create(&tx, &iface) != DANOS_ERR_EXISTS) return 19;
    if (danos_tx_abort(&tx) != DANOS_OK) return 20;

    /* And it must be removable. */
    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 21;
    if (danos_iface_delete(&tx, 1) != DANOS_OK) return 22;
    if (danos_tx_prepare(&tx) != DANOS_OK) return 23;
    if (danos_tx_validate(&tx) != DANOS_OK) return 24;
    if (danos_tx_commit(&tx) != DANOS_OK) return 25;
    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 26;
    if (danos_iface_read(&tx, 1, &out) != DANOS_ERR_NOT_FOUND) return 27;
    if (danos_tx_abort(&tx) != DANOS_OK) return 28;
    return 0;
}

int conf_vrf_crud(void)
{
    danos_tx_t tx = {0};
    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 1;

    danos_vrf_t vrf = {0};
    vrf.vrf_id = 31001;
    vrf.ipv4_active = true;
    strncpy(vrf.name, "VRF100", sizeof(vrf.name) - 1);

    if (danos_vrf_create(&tx, &vrf) != DANOS_OK) return 2;
    if (conf_commit(&tx) != DANOS_OK) return 3;

    danos_vrf_t out = {0};
    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 4;
    if (danos_vrf_read(&tx, vrf.vrf_id, &out) != DANOS_OK) return 5;
    if (out.vrf_id != vrf.vrf_id || strcmp(out.name, vrf.name) != 0 ||
        !out.ipv4_active || out.ipv6_active) return 6;
    if (danos_tx_abort(&tx) != DANOS_OK) return 7;

    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 8;
    if (danos_vrf_create(&tx, &vrf) != DANOS_ERR_EXISTS) return 9;
    if (danos_tx_abort(&tx) != DANOS_OK) return 10;

    vrf.ipv4_active = false;
    vrf.ipv6_active = true;
    strncpy(vrf.name, "VRF100-updated", sizeof(vrf.name) - 1);
    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 11;
    if (danos_vrf_update(&tx, &vrf) != DANOS_OK) return 12;
    if (conf_commit(&tx) != DANOS_OK) return 13;

    memset(&out, 0, sizeof(out));
    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 14;
    if (danos_vrf_read(&tx, vrf.vrf_id, &out) != DANOS_OK) return 15;
    if (out.vrf_id != vrf.vrf_id || strcmp(out.name, vrf.name) != 0 ||
        out.ipv4_active || !out.ipv6_active) return 16;
    if (danos_tx_abort(&tx) != DANOS_OK) return 17;

    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 18;
    if (danos_vrf_delete(&tx, vrf.vrf_id) != DANOS_OK) return 19;
    if (conf_commit(&tx) != DANOS_OK) return 20;
    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 21;
    if (danos_vrf_read(&tx, vrf.vrf_id, &out) != DANOS_ERR_NOT_FOUND) return 22;
    if (danos_tx_abort(&tx) != DANOS_OK) return 23;
    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 24;
    if (danos_vrf_delete(&tx, vrf.vrf_id) != DANOS_ERR_NOT_FOUND) return 25;
    return danos_tx_abort(&tx) == DANOS_OK ? 0 : 26;
}

int conf_acl_crud(void)
{
    danos_tx_t tx = {0};
    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 1;

    danos_acl_table_t tbl = {0};
    tbl.table_id = 31002;
    tbl.bind_ifindex = 77;
    strncpy(tbl.name, "ACL1", sizeof(tbl.name) - 1);
    tbl.ingress = true;

    if (danos_acl_table_create(&tx, &tbl) != DANOS_OK) return 2;
    if (conf_commit(&tx) != DANOS_OK) return 3;

    danos_acl_table_t out = {0};
    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 4;
    if (danos_acl_table_read(&tx, tbl.table_id, &out) != DANOS_OK) return 5;
    if (out.table_id != tbl.table_id || strcmp(out.name, tbl.name) != 0 ||
        out.bind_ifindex != tbl.bind_ifindex || !out.ingress) return 6;
    if (danos_tx_abort(&tx) != DANOS_OK) return 7;

    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 8;
    if (danos_acl_table_create(&tx, &tbl) != DANOS_ERR_EXISTS) return 9;
    if (danos_tx_abort(&tx) != DANOS_OK) return 10;

    tbl.bind_ifindex = 88;
    tbl.ingress = false;
    strncpy(tbl.name, "ACL1-updated", sizeof(tbl.name) - 1);
    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 11;
    if (danos_acl_table_update(&tx, &tbl) != DANOS_OK) return 12;
    if (conf_commit(&tx) != DANOS_OK) return 13;

    memset(&out, 0, sizeof(out));
    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 14;
    if (danos_acl_table_read(&tx, tbl.table_id, &out) != DANOS_OK) return 15;
    if (out.table_id != tbl.table_id || strcmp(out.name, tbl.name) != 0 ||
        out.bind_ifindex != tbl.bind_ifindex || out.ingress) return 16;
    if (danos_tx_abort(&tx) != DANOS_OK) return 17;

    danos_acl_rule_t rule = {0}, rule_out = {0};
    rule.rule_id = 31003;
    rule.priority = 20;
    rule.match.fields_mask = DANOS_ACL_FIELD_ETHER_TYPE | DANOS_ACL_FIELD_SRC_MAC |
        DANOS_ACL_FIELD_DST_MAC | DANOS_ACL_FIELD_VLAN_ID | DANOS_ACL_FIELD_SRC_IP |
        DANOS_ACL_FIELD_DST_IP | DANOS_ACL_FIELD_L4_PROTO |
        DANOS_ACL_FIELD_L4_SRC_PORT | DANOS_ACL_FIELD_L4_DST_PORT | DANOS_ACL_FIELD_DSCP;
    rule.match.ether_type = 0x0800;
    const uint8_t src_mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    const uint8_t dst_mac[6] = {0x06, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
    memcpy(rule.match.src_mac, src_mac, sizeof(src_mac));
    memcpy(rule.match.dst_mac, dst_mac, sizeof(dst_mac));
    rule.match.vlan_id = 123;
    rule.match.src_ip.addr.af = DANOS_AF_IPV4;
    rule.match.src_ip.addr.addr[12] = 192;
    rule.match.src_ip.addr.addr[14] = 2;
    rule.match.src_ip.prefix_len = 24;
    rule.match.dst_ip.addr.af = DANOS_AF_IPV4;
    rule.match.dst_ip.addr.addr[12] = 198;
    rule.match.dst_ip.addr.addr[13] = 51;
    rule.match.dst_ip.addr.addr[14] = 100;
    rule.match.dst_ip.prefix_len = 24;
    rule.match.l4_proto = 17;
    rule.match.dscp = 12;
    rule.match.l4_src_port_start = 1000;
    rule.match.l4_src_port_end = 2000;
    rule.match.l4_dst_port_start = 3000;
    rule.match.l4_dst_port_end = 4000;
    rule.act.action = DANOS_ACL_ACTION_DENY;
    rule.act.redirect_ifindex = 91;
    rule.act.police_rate_kbps = 54321;
    rule.act.set_dscp = 26;
    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 18;
    if (danos_acl_rule_add(&tx, tbl.table_id, &rule) != DANOS_OK) return 19;
    if (conf_commit(&tx) != DANOS_OK) return 20;
    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 21;
    if (danos_acl_rule_read(&tx, tbl.table_id, rule.rule_id, &rule_out) != DANOS_OK) return 22;
    if (rule_out.rule_id != rule.rule_id || rule_out.priority != rule.priority ||
        rule_out.match.fields_mask != rule.match.fields_mask ||
        rule_out.match.ether_type != rule.match.ether_type ||
        memcmp(rule_out.match.src_mac, rule.match.src_mac, sizeof(rule.match.src_mac)) != 0 ||
        memcmp(rule_out.match.dst_mac, rule.match.dst_mac, sizeof(rule.match.dst_mac)) != 0 ||
        rule_out.match.vlan_id != rule.match.vlan_id ||
        rule_out.match.src_ip.addr.af != rule.match.src_ip.addr.af ||
        memcmp(rule_out.match.src_ip.addr.addr, rule.match.src_ip.addr.addr,
               sizeof(rule.match.src_ip.addr.addr)) != 0 ||
        rule_out.match.src_ip.prefix_len != rule.match.src_ip.prefix_len ||
        rule_out.match.dst_ip.addr.af != rule.match.dst_ip.addr.af ||
        memcmp(rule_out.match.dst_ip.addr.addr, rule.match.dst_ip.addr.addr,
               sizeof(rule.match.dst_ip.addr.addr)) != 0 ||
        rule_out.match.dst_ip.prefix_len != rule.match.dst_ip.prefix_len ||
        rule_out.match.l4_proto != rule.match.l4_proto ||
        rule_out.match.l4_src_port_start != rule.match.l4_src_port_start ||
        rule_out.match.l4_src_port_end != rule.match.l4_src_port_end ||
        rule_out.match.l4_dst_port_start != rule.match.l4_dst_port_start ||
        rule_out.match.l4_dst_port_end != rule.match.l4_dst_port_end ||
        rule_out.match.dscp != rule.match.dscp ||
        rule_out.act.action != rule.act.action ||
        rule_out.act.redirect_ifindex != rule.act.redirect_ifindex ||
        rule_out.act.police_rate_kbps != rule.act.police_rate_kbps ||
        rule_out.act.set_dscp != rule.act.set_dscp) return 23;
    if (danos_tx_abort(&tx) != DANOS_OK) return 24;
    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 25;
    if (danos_acl_rule_add(&tx, tbl.table_id, &rule) != DANOS_ERR_EXISTS) return 26;
    if (danos_tx_abort(&tx) != DANOS_OK) return 27;
    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 28;
    if (danos_acl_rule_delete(&tx, tbl.table_id, rule.rule_id) != DANOS_OK) return 29;
    if (conf_commit(&tx) != DANOS_OK) return 30;
    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 31;
    if (danos_acl_rule_read(&tx, tbl.table_id, rule.rule_id, &rule_out) != DANOS_ERR_NOT_FOUND) return 32;
    if (danos_tx_abort(&tx) != DANOS_OK) return 33;

    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 34;
    if (danos_acl_table_delete(&tx, tbl.table_id) != DANOS_OK) return 35;
    if (conf_commit(&tx) != DANOS_OK) return 36;
    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 37;
    if (danos_acl_table_read(&tx, tbl.table_id, &out) != DANOS_ERR_NOT_FOUND) return 38;
    if (danos_tx_abort(&tx) != DANOS_OK) return 39;
    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 40;
    if (danos_acl_table_delete(&tx, tbl.table_id) != DANOS_ERR_NOT_FOUND) return 41;
    return danos_tx_abort(&tx) == DANOS_OK ? 0 : 42;
}

int conf_qos_crud(void)
{
    danos_tx_t tx = {0};
    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 1;

    danos_qos_policy_t p = {0};
    p.policy_id = 31004;
    strncpy(p.name, "POLICE1", sizeof(p.name) - 1);
    p.cir_bps = 100000000;  /* 100 Mbps */
    p.cb_bytes = 12500000;  /* 100ms burst */

    if (danos_qos_policy_create(&tx, &p) != DANOS_OK) return 2;
    if (conf_commit(&tx) != DANOS_OK) return 3;

    danos_qos_policy_t out = {0};
    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 4;
    if (danos_qos_policy_read(&tx, p.policy_id, &out) != DANOS_OK) return 5;
    if (out.policy_id != p.policy_id || strcmp(out.name, p.name) != 0 ||
        out.cir_bps != p.cir_bps || out.cb_bytes != p.cb_bytes ||
        out.pir_bps != p.pir_bps || out.pb_bytes != p.pb_bytes ||
        out.conform_dscp != p.conform_dscp || out.exceed_dscp != p.exceed_dscp ||
        out.violate_dscp != p.violate_dscp) return 6;
    if (danos_tx_abort(&tx) != DANOS_OK) return 7;

    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 8;
    if (danos_qos_policy_create(&tx, &p) != DANOS_ERR_EXISTS) return 9;
    if (danos_tx_abort(&tx) != DANOS_OK) return 10;

    strncpy(p.name, "POLICE1-updated", sizeof(p.name) - 1);
    p.cir_bps = 200000000;
    p.cb_bytes = 25000000;
    p.pir_bps = 250000000;
    p.pb_bytes = 31250000;
    p.conform_dscp = 10;
    p.exceed_dscp = 20;
    p.violate_dscp = 30;
    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 11;
    if (danos_qos_policy_update(&tx, &p) != DANOS_OK) return 12;
    if (conf_commit(&tx) != DANOS_OK) return 13;

    memset(&out, 0, sizeof(out));
    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 14;
    if (danos_qos_policy_read(&tx, p.policy_id, &out) != DANOS_OK) return 15;
    if (out.policy_id != p.policy_id || strcmp(out.name, p.name) != 0 ||
        out.cir_bps != p.cir_bps || out.cb_bytes != p.cb_bytes ||
        out.pir_bps != p.pir_bps || out.pb_bytes != p.pb_bytes ||
        out.conform_dscp != p.conform_dscp || out.exceed_dscp != p.exceed_dscp ||
        out.violate_dscp != p.violate_dscp) return 16;
    if (danos_tx_abort(&tx) != DANOS_OK) return 17;

    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 18;
    if (danos_qos_policy_delete(&tx, p.policy_id) != DANOS_OK) return 19;
    if (conf_commit(&tx) != DANOS_OK) return 20;
    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 21;
    if (danos_qos_policy_read(&tx, p.policy_id, &out) != DANOS_ERR_NOT_FOUND) return 22;
    if (danos_tx_abort(&tx) != DANOS_OK) return 23;
    if (danos_tx_begin(&tx, "conf", &kDefaultTimeouts) != DANOS_OK) return 24;
    if (danos_qos_policy_delete(&tx, p.policy_id) != DANOS_ERR_NOT_FOUND) return 25;
    return danos_tx_abort(&tx) == DANOS_OK ? 0 : 26;
}

/* ----------------------------------------------------------------------- */
/* 10. Capability query                                                     */
/* ----------------------------------------------------------------------- */
int conf_capability_query(void)
{
    /* This used to discard the status and return 0 unconditionally. With the
     * VPP backend installed the registry now answers, so assert a definite
     * result rather than accepting anything. */
    danos_capability_t cap;
    danos_status_t st = danos_capability_query(NULL, DANOS_OBJ_ROUTE, &cap);
    if (st == DANOS_OK) {
        /* A backend claims ROUTE, so the object type must be set. */
        if (cap.type != DANOS_OBJ_ROUTE) return 30;
        if (!cap.supported) return 31;
        if (cap.max_count == 0) return 32;
    } else if (st != DANOS_ERR_NOT_SUPPORTED) {
        /* Any other status is a real failure, not an acceptable outcome. */
        return 33;
    }

    /* The answer must be stable across calls: a registry that answers
     * differently each time is not usable for backend selection. */
    danos_capability_t cap2;
    if (danos_capability_query(NULL, DANOS_OBJ_ROUTE, &cap2) != st) return 34;
    if (st == DANOS_OK && (cap2.supported != cap.supported ||
                           cap2.max_count != cap.max_count))
        return 35;
    return 0;
}

/* ----------------------------------------------------------------------- */
/* 11. Version negotiation                                                  */
/* ----------------------------------------------------------------------- */
int conf_version_negotiate(void)
{
    danos_version_t v = danos_dpa_get_version();
    if (v.major != 0) return 1;
    if (v.minor != 1) return 2;
    return 0;
}
