#include "../src/gnmi/gnmi_grpc.h"
#include "../src/gnmi/gnmi_proto.h"
#include <danos/core/object_registry.h>
#include <danos/dpa.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t req[65536], resp[262144];
static gnmi_pb_t w;
static void begin(void) { gnmi_pb_init(&w, req, sizeof(req)); }
static void op(unsigned field, const char *path, const char *json)
{
    gnmi_path_t p;
    assert(gnmi_path_from_str(&p, path));
    if (field == 2) { gnmi_encode_path(&w, field, &p); return; }
    gnmi_typed_value_t v = { .kind = GNMI_VAL_JSON_IETF };
    snprintf(v.s, sizeof(v.s), "%s", json);
    size_t n = gnmi_pb_begin_nested(&w, field);
    gnmi_encode_path(&w, 1, &p);
    gnmi_encode_typed_value(&w, 3, &v);
    gnmi_pb_end_nested(&w, n);
    assert(!w.overflow);
}
static int apply(void) { return gnmi_handle_set(NULL, req, w.len, resp, sizeof(resp)); }
static danos_iface_t iface(void)
{
    danos_iface_t i;
    size_t n = sizeof(i);
    assert(danos_object_read(g_default_store, DANOS_OBJ_IFACE, 1, &i, &n) == DANOS_OK);
    return i;
}
static unsigned get_paths(const char *path, bool route)
{
    begin();
    gnmi_path_t p;
    assert(gnmi_path_from_str(&p, path));
    gnmi_encode_path(&w, 2, &p);
    int n = gnmi_handle_get(NULL, req, w.len, resp, sizeof(resp));
    assert(n > 0);
    gnmi_pb_reader_t r; gnmi_pbr_init(&r, resp, n);
    uint32_t f, wire; unsigned count = 0;
    while ((f = gnmi_pbr_tag(&r, &wire))) {
        if (f != 1 || wire != 2) { gnmi_pbr_skip(&r, wire); continue; }
        const uint8_t *d; size_t len;
        assert(gnmi_pbr_bytes(&r, &d, &len));
        gnmi_pb_reader_t note; gnmi_pbr_init(&note, d, len);
        while ((f = gnmi_pbr_tag(&note, &wire))) {
            if (f != 4 || wire != 2) { gnmi_pbr_skip(&note, wire); continue; }
            assert(gnmi_pbr_bytes(&note, &d, &len));
            gnmi_pb_reader_t update; gnmi_pbr_init(&update, d, len);
            bool saw_path = false;
            while ((f = gnmi_pbr_tag(&update, &wire))) {
                if (f != 1 || wire != 2) { gnmi_pbr_skip(&update, wire); continue; }
                assert(gnmi_pbr_bytes(&update, &d, &len));
                assert(gnmi_decode_path(d, len, &p));
                assert(p.elem_count == 2 && p.elems[1].has_key);
                if (route) {
                    assert(!strcmp(p.elems[1].key_name, "prefix"));
                    assert(p.elems[1].has_extra_key);
                    assert(!strcmp(p.elems[1].extra_key_name, "vrf"));
                } else assert(!strcmp(p.elems[1].key_name,
                                      !strncmp(path, "vrfs", 4) ? "id" : "name"));
                saw_path = true;
            }
            assert(saw_path); count++;
        }
    }
    return count;
}
int main(void)
{
    g_default_store = danos_object_store_create(4096);
    assert(g_default_store);
    const char *p = "interfaces/interface[name=portA]";
    begin(); op(4, p, "{\"mtu\":9000,\"enabled\":false,\"ipv4-address\":\"10.0.0.1/24\"}");
    assert(apply() > 0);
    assert(iface().mtu == 9000 && !iface().admin_up);
    begin(); op(4, p, "{\"mtu\":8000}"); assert(apply() > 0);
    assert(!iface().admin_up && iface().mtu == 8000);
    begin(); op(3, p, "{\"mtu\":1600}"); assert(apply() > 0);
    assert(iface().admin_up && iface().mtu == 1600);
    assert(iface().ipv4_address.addr.af != DANOS_AF_IPV4);
    assert(danos_object_count(g_default_store, DANOS_OBJ_IFACE) == 1);
    begin(); op(4, p, "{\"mtu\":1800}"); op(3, p, "{\"mtu\":1700}"); op(2, p, NULL);
    assert(apply() > 0); assert(iface().mtu == 1800);
    const char *invalid[] = {
        "{\"mtu\":1}", "{\"mtu\":1700,\"unknown\":1}", "{\"mtu\":1700,\"mtu\":1800}",
        "{\"mtu\":1700,}", "{\"enabled\":\"true\"}", "{\"ifindex\":999}"
    };
    for (size_t i = 0; i < sizeof(invalid)/sizeof(*invalid); i++) {
        begin(); op(3, p, "{\"mtu\":1900}"); op(4, p, invalid[i]);
        assert(apply() < 0); assert(iface().mtu == 1800);
    }
    begin(); op(3, p, "{\"mtu\":1900}");
    assert(gnmi_handle_set(NULL, req, w.len, resp, 1) < 0);
    assert(iface().mtu == 1800);
    begin(); op(3, "interfaces", "{}"); assert(apply() < 0);
    const char *rp = "routes/route[prefix=10.99.0.0/24][vrf=7]";
    begin(); op(4, rp, "{\"gateways\":[\"10.0.0.2\",\"10.0.0.3\"],\"oif\":1}");
    assert(apply() > 0);
    assert(danos_object_count(g_default_store, DANOS_OBJ_NEXTHOP) == 2);
    begin(); op(3, rp, "{\"gateway\":\"10.0.0.4\"}"); op(4, p, "{\"mtu\":1}");
    assert(apply() < 0);
    assert(danos_object_count(g_default_store, DANOS_OBJ_NEXTHOP) == 2);
    begin(); op(3, rp, "{\"gateway\":\"10.0.0.4\"}"); assert(apply() > 0);
    assert(danos_object_count(g_default_store, DANOS_OBJ_NEXTHOP) == 1);
    begin(); op(4, "routes/route[prefix=10.99.0.0/24]", "{\"gateway\":\"10.0.0.5\"}");
    assert(apply() > 0); assert(get_paths("routes", true) == 2);
    assert(get_paths("routes/route[vrf=7][prefix=10.99.0.0/24]", true) == 1);
    begin(); op(2, "routes/route[vrf=7][prefix=10.99.0.0/24]", NULL); assert(apply() > 0);
    assert(get_paths("routes", true) == 1);
    assert(danos_object_count(g_default_store, DANOS_OBJ_NEXTHOP) == 1);
    assert(danos_object_count(g_default_store, DANOS_OBJ_NHGROUP) == 1);
    begin(); op(4, "interfaces/interface[name=portB]", "{}"); assert(apply() > 0);
    assert(get_paths("interfaces", false) == 2);
    assert(get_paths("interfaces/interface[name=portA]", false) == 1);
    assert(get_paths("interfaces/interface", false) == 2);
    begin(); op(4, p, "{\"mtu\":2100}");
    op(6, p, "{\"mtu\":2200}");
    assert(apply() < 0); assert(iface().mtu == 1800);
    /* Prefix composition and sequential candidate reads. */
    begin();
    gnmi_path_t prefix;
    assert(gnmi_path_from_str(&prefix, "interfaces"));
    gnmi_encode_path(&w, 1, &prefix);
    op(3, "interface[name=portA]/config", "{\"mtu\":2000}");
    op(4, "interface[name=portA]", "{\"enabled\":false}");
    assert(apply() > 0);
    assert(iface().mtu == 2000 && !iface().admin_up);
    begin(); op(3, "interfaces/interface[name=portA]/config/mtu", "2100");
    assert(apply() > 0); assert(iface().mtu == 2100 && !iface().admin_up);
    begin(); op(2, "interfaces/interface[name=portA]/config/mtu", NULL);
    assert(apply() > 0); assert(iface().mtu == 1500 && !iface().admin_up);
    for (unsigned id = 1; id <= 24; id++) {
        danos_tx_t tx; danos_vrf_t vrf = { .vrf_id = id, .ipv4_active = true };
        snprintf(vrf.name, sizeof(vrf.name), "vrf%u", id);
        assert(danos_tx_begin(&tx, "fixture", NULL) == DANOS_OK);
        assert(danos_vrf_create(&tx, &vrf) == DANOS_OK);
        assert(danos_tx_prepare(&tx) == DANOS_OK);
        assert(danos_tx_validate(&tx) == DANOS_OK);
        assert(danos_tx_commit(&tx) == DANOS_OK);
    }
    assert(get_paths("vrfs", false) == 24);
    assert(get_paths("vrfs/vrf[id=17]", false) == 1);
    /* No fixed 32-route Get limit. */
    for (unsigned i = 1; i <= 40; i++) {
        char route[96];
        snprintf(route, sizeof(route), "routes/route[prefix=10.100.%u.0/24]", i);
        begin(); op(4, route, "{\"gateway\":\"10.0.0.2\"}"); assert(apply() > 0);
    }
    assert(get_paths("routes", true) == 41);
    gnmi_path_t path;
    assert(!gnmi_path_from_str(&path, "routes/route[a=1][b=2][c=3]"));
    assert(!gnmi_path_from_str(&path, "routes/route[a=1][a=2]"));
    danos_object_store_destroy(g_default_store); g_default_store = NULL;
    puts("PASS: Set ordering, replace defaults/identity, atomic rollback, ECMP retirement, concrete Get paths");
    return 0;
}
