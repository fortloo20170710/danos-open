/* Storage-side Route/NH/NHGroup contract. Backend replay is a separate gate. */
#include <danos/dpa.h>
#include <danos/core/transaction.h>
#include <string.h>

static danos_status_t commit_candidate(danos_tx_t *tx)
{
    danos_status_t st = danos_tx_prepare(tx);
    if (st == DANOS_OK) st = danos_tx_validate(tx);
    if (st == DANOS_OK) st = danos_tx_commit(tx);
    return st;
}

#define REQUIRE(expr) do { if (!(expr)) { rc = __LINE__; goto fail; } } while (0)

/* The observer is a separate live transaction, not a NULL read bypass.
 * Use zero-initialized structs so payload equality includes every field. */
#define LIFECYCLE(TYPE, SEED, CHANGED, CREATE, UPDATE, DELETE, READ) \
    TYPE seed = SEED, out = {0}; \
    danos_tx_t tx = {0}, observer = {0}; int rc = 0; \
    uint64_t active = danos_tx_active_count(); \
    REQUIRE(danos_tx_begin(&observer, "contract-observer", NULL) == DANOS_OK); \
    REQUIRE(READ(&observer, &out) == DANOS_ERR_NOT_FOUND); \
    REQUIRE(danos_tx_begin(&tx, "contract-create", NULL) == DANOS_OK); \
    REQUIRE(UPDATE(&tx, &seed) == DANOS_ERR_NOT_FOUND); \
    REQUIRE(DELETE(&tx) == DANOS_ERR_NOT_FOUND); \
    REQUIRE(CREATE(&tx, &seed) == DANOS_OK); \
    REQUIRE(CREATE(&tx, &seed) == DANOS_ERR_EXISTS); \
    REQUIRE(READ(&tx, &out) == DANOS_OK && !memcmp(&seed, &out, sizeof(seed))); \
    REQUIRE(READ(&observer, &out) == DANOS_ERR_NOT_FOUND); \
    REQUIRE(commit_candidate(&tx) == DANOS_OK); \
    REQUIRE(READ(&observer, &out) == DANOS_OK && !memcmp(&seed, &out, sizeof(seed))); \
    TYPE changed = seed; CHANGED; \
    REQUIRE(danos_tx_begin(&tx, "contract-update-abort", NULL) == DANOS_OK); \
    REQUIRE(UPDATE(&tx, &changed) == DANOS_OK); \
    REQUIRE(READ(&tx, &out) == DANOS_OK && !memcmp(&changed, &out, sizeof(out))); \
    REQUIRE(READ(&observer, &out) == DANOS_OK && !memcmp(&seed, &out, sizeof(out))); \
    REQUIRE(danos_tx_abort(&tx) == DANOS_OK); \
    REQUIRE(READ(&observer, &out) == DANOS_OK && !memcmp(&seed, &out, sizeof(out))); \
    REQUIRE(danos_tx_begin(&tx, "contract-update-commit", NULL) == DANOS_OK); \
    REQUIRE(UPDATE(&tx, &changed) == DANOS_OK); \
    REQUIRE(commit_candidate(&tx) == DANOS_OK); \
    REQUIRE(READ(&observer, &out) == DANOS_OK && !memcmp(&changed, &out, sizeof(out))); \
    REQUIRE(danos_tx_begin(&tx, "contract-delete-abort", NULL) == DANOS_OK); \
    REQUIRE(DELETE(&tx) == DANOS_OK); \
    REQUIRE(READ(&tx, &out) == DANOS_ERR_NOT_FOUND); \
    REQUIRE(READ(&observer, &out) == DANOS_OK); \
    REQUIRE(danos_tx_abort(&tx) == DANOS_OK); \
    REQUIRE(READ(&observer, &out) == DANOS_OK && !memcmp(&changed, &out, sizeof(out))); \
    REQUIRE(danos_tx_begin(&tx, "contract-delete-commit", NULL) == DANOS_OK); \
    REQUIRE(DELETE(&tx) == DANOS_OK); \
    REQUIRE(commit_candidate(&tx) == DANOS_OK); \
    REQUIRE(READ(&observer, &out) == DANOS_ERR_NOT_FOUND); \
    REQUIRE(danos_tx_abort(&observer) == DANOS_OK); \
    REQUIRE(danos_tx_active_count() == active); \
    return 0; \
fail: \
    (void)danos_tx_abort(&tx); (void)danos_tx_abort(&observer); \
    return rc

int conf_nh_crud(void)
{
    danos_nexthop_t initial = { .id = 901, .gateway = { .af = DANOS_AF_IPV4,
        .addr = {10, 0, 0, 1} }, .ifindex = 7, .weight = 1 };
#define NH_READ(tx, out) danos_nh_read(tx, initial.id, out)
#define NH_DELETE(tx) danos_nh_delete(tx, initial.id)
    LIFECYCLE(danos_nexthop_t, initial, changed.weight = 3,
              danos_nh_create, danos_nh_update, NH_DELETE, NH_READ);
#undef NH_READ
#undef NH_DELETE
}

int conf_nhgroup_crud(void)
{
    danos_nhgroup_t initial = { .id = 902, .nh_count = 2, .nh_ids = {901, 903} };
#define GROUP_READ(tx, out) danos_nhgroup_read(tx, initial.id, out)
#define GROUP_DELETE(tx) danos_nhgroup_delete(tx, initial.id)
    LIFECYCLE(danos_nhgroup_t, initial, changed.nh_count = 1,
              danos_nhgroup_create, danos_nhgroup_update, GROUP_DELETE, GROUP_READ);
#undef GROUP_READ
#undef GROUP_DELETE
}

int conf_route_crud(void)
{
    danos_route_t initial = { .vrf_id = 100, .prefix = {
        .addr = { .af = DANOS_AF_IPV4, .addr = {198, 51, 100, 0} }, .prefix_len = 24 },
        .protocol = DANOS_ROUTE_PROTO_STATIC, .admin_distance = 1, .nhgroup_id = 902 };
#define ROUTE_READ(tx, out) danos_route_read(tx, initial.vrf_id, initial.prefix, initial.protocol, out)
#define ROUTE_DELETE(tx) danos_route_delete(tx, initial.vrf_id, initial.prefix, initial.protocol)
    LIFECYCLE(danos_route_t, initial, changed.metric = 99,
              danos_route_create, danos_route_update, ROUTE_DELETE, ROUTE_READ);
#undef ROUTE_READ
#undef ROUTE_DELETE
}
