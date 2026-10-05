/*
 * DANOS-Open Core: Backend programming pipeline (v0.9/v0.10, ADR-0007)
 *
 * danos_programming_run() walks the desired-state store (the DPA
 * default store, where gNMI/CLI/NETCONF commits land) and drives the
 * active backend ops until every object is PROGRAMMED.
 *
 * The PROGRAMMED ledger is a private store keyed by (type, id) holding
 * a full copy of the last-programmed object bytes:
 *   - desired == ledger  -> in sync, nothing to do
 *   - desired != ledger  -> re-issue (ops are idempotent)
 *   - desired missing    -> tombstone: withdraw from backend (routes)
 *                           and drop the ledger entry
 * Crash-safe: the ledger is rebuilt from scratch, so a restart re-
 * issues whatever is not yet programmed.
 */

#include <danos/core/backend_ops.h>
#include <danos/core/object_registry.h>
#include <danos/dpa.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

static const danos_backend_ops_t *g_ops;
static danos_object_store_t *g_programmed;

/* cumulative programming statistics (v0.13, exposed via Prometheus) */
static struct {
    uint64_t attempted, ok, failed;
} g_prog_stats;
static pthread_mutex_t g_prog_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int g_dirty;   /* set by store mutation events */

void danos_programming_mark_dirty(void)
{
    atomic_store_explicit(&g_dirty, 1, memory_order_release);
}

int danos_programming_dirty_take(void)
{
    return atomic_exchange_explicit(&g_dirty, 0, memory_order_acq_rel);
}

void danos_programming_get_stats(uint64_t *attempted, uint64_t *ok,
                                 uint64_t *failed)
{
    if (attempted) *attempted = g_prog_stats.attempted;
    if (ok)        *ok        = g_prog_stats.ok;
    if (failed)    *failed    = g_prog_stats.failed;
}

void danos_backend_ops_set(const danos_backend_ops_t *ops)
{
    g_ops = ops;
}

const danos_backend_ops_t *danos_backend_ops_get(void)
{
    return g_ops;
}

static danos_object_store_t *ledger(void)
{
    if (!g_programmed)
        g_programmed = danos_object_store_create(256);
    return g_programmed;
}

/* ---- dependency digest (v0.11) -------------------------------------------
 * A route's programmed state depends on the NH group and the NH object
 * it resolves through. The ledger records their digest alongside the
 * route bytes, so a gateway change (or NH deletion) invalidates the
 * entry even though the route object itself is unchanged. */

static uint64_t hash_bytes(const void *data, size_t size)
{
    uint64_t h = 14695981039346656037ULL;
    const uint8_t *b = data;
    for (size_t i = 0; i < size; i++) {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static uint64_t dep_digest(danos_obj_type_t type, const void *data, size_t size)
{
    uint64_t h = 0;
    if (type != DANOS_OBJ_ROUTE || size < sizeof(danos_route_t)) return h;
    const danos_route_t *r = data;
    if (r->nhgroup_id == 0) return h;

    danos_nhgroup_t grp;
    size_t sz = sizeof(grp);
    if (danos_object_read(g_default_store, DANOS_OBJ_NHGROUP, r->nhgroup_id,
                          &grp, &sz) != DANOS_OK) {
        return 0xDEADBEEFULL;   /* group gone: digest differs -> dirty */
    }
    h = hash_bytes(&grp, sizeof(grp));
    for (uint32_t i = 0; i < grp.nh_count && i < 64; i++) {
        danos_nexthop_t nh;
        sz = sizeof(nh);
        if (danos_object_read(g_default_store, DANOS_OBJ_NEXTHOP,
                              grp.nh_ids[i], &nh, &sz) == DANOS_OK) {
            h ^= hash_bytes(&nh, sizeof(nh));
        } else {
            h ^= 0x535A4AULL;  /* member gone */
        }
    }
    return h;
}

/* ---- route next-hop resolution ------------------------------------------ */

/* nhgroup -> first member NH object -> gateway + egress ifindex */
static danos_status_t resolve_route_nh(const danos_route_t *r,
                                       danos_resolved_route_t *out)
{
    memset(out, 0, sizeof(*out));
    out->route = *r;

    if (r->flags & DANOS_ROUTE_FLAG_BLACKHOLE) return DANOS_OK;  /* no gw */
    if (r->nhgroup_id == 0) return DANOS_ERR_RETRY;   /* path not usable yet */

    danos_nhgroup_t grp;
    size_t sz = sizeof(grp);
    if (danos_object_read(g_default_store, DANOS_OBJ_NHGROUP, r->nhgroup_id,
                          &grp, &sz) != DANOS_OK || grp.nh_count == 0)
        return DANOS_ERR_RETRY;

    danos_nexthop_t nh;
    sz = sizeof(nh);
    if (danos_object_read(g_default_store, DANOS_OBJ_NEXTHOP,
                          grp.nh_ids[0], &nh, &sz) != DANOS_OK)
        return DANOS_ERR_RETRY;

    if (nh.gateway.af == DANOS_AF_IPV4) {
        out->has_gw = true;
        memcpy(out->gw, nh.gateway.addr, 4);
    } else if (nh.gateway.af == DANOS_AF_IPV6) {
        out->has_gw = true;
        memcpy(out->gw, nh.gateway.addr, 16);
    }
    out->oif = nh.ifindex;
    out->nh_count = grp.nh_count > 64 ? 64 : grp.nh_count;
    for (uint32_t i = 0; i < out->nh_count; i++) {
        danos_nexthop_t member;
        sz = sizeof(member);
        if (danos_object_read(g_default_store, DANOS_OBJ_NEXTHOP,
                              grp.nh_ids[i], &member, &sz) != DANOS_OK)
            return DANOS_ERR_RETRY;
        memcpy(out->nh_gw[i], member.gateway.addr,
               member.gateway.af == DANOS_AF_IPV6 ? 16 : 4);
        out->nh_oif[i] = member.ifindex;
    }
    return DANOS_OK;
}

static danos_status_t program_one(danos_obj_type_t type, danos_obj_id_t id,
                                  const void *data, size_t size)
{
    (void)id;
    if (type == DANOS_OBJ_IFACE && size >= sizeof(danos_iface_t)) {
        const danos_iface_t *i = data;
        danos_status_t st = g_ops->iface_up ?
            g_ops->iface_up(i->ifindex, i->admin_up, g_ops->user) :
            DANOS_ERR_NOT_SUPPORTED;
        if (st != DANOS_OK) return st;
        if (i->ipv4_address.addr.af != DANOS_AF_UNSPEC ||
            i->ipv6_address.addr.af != DANOS_AF_UNSPEC)
            return g_ops->iface_addr_set ?
                g_ops->iface_addr_set(i, g_ops->user) : DANOS_ERR_NOT_SUPPORTED;
        return DANOS_OK;
    }
    if (type == DANOS_OBJ_ROUTE && size >= sizeof(danos_route_t)) {
        const danos_route_t *r = data;
        if (r->nhgroup_id == 0 && !(r->flags & DANOS_ROUTE_FLAG_BLACKHOLE))
            return DANOS_ERR_RETRY;   /* path not usable yet */
        danos_resolved_route_t rr;
        danos_status_t st = resolve_route_nh(r, &rr);
        if (st != DANOS_OK) return st;
        return g_ops->route_add ? g_ops->route_add(&rr, g_ops->user)
                                : DANOS_ERR_NOT_SUPPORTED;
    }
    if (type == DANOS_OBJ_VRF && size >= sizeof(danos_vrf_t)) {
        return g_ops->vrf_add ? g_ops->vrf_add(data, g_ops->user)
                              : DANOS_ERR_NOT_SUPPORTED;
    }
    return DANOS_ERR_NOT_SUPPORTED;
}

/* types the v0.9/v0.10 pipeline cannot program */
static bool type_skipped(danos_obj_type_t t)
{
    return t == DANOS_OBJ_ACL || t == DANOS_OBJ_QOS ||
           t == DANOS_OBJ_QOS_BIND || t == DANOS_OBJ_BFD ||
           t == DANOS_OBJ_MPLS_LSP || t == DANOS_OBJ_TUNNEL ||
           t == DANOS_OBJ_EVPN || t == DANOS_OBJ_MULTICAST ||
           t == DANOS_OBJ_NEXTHOP || t == DANOS_OBJ_NHGROUP;
}

/* ---- retry / anti-flap policy -------------------------------------------- */

/*
 * Per-object failure tracking.
 *
 * The reconciler ticks every 50 ms when desired state is dirty. An object
 * that cannot be programmed - typically a route whose next hop has not been
 * learned yet - returned DANOS_ERR_RETRY on every pass, so it hammered the
 * backend indefinitely. Failures now back off exponentially and stop after
 * max_retries, and an object that oscillates is parked.
 *
 * Bounded table: a full slot is replaced rather than growing, so the worst
 * case is a lost backoff record (one retry storm) and never unbounded memory.
 */
#define RETRY_SLOTS 1024

typedef struct {
    bool            used;
    danos_obj_type_t type;
    danos_obj_id_t   id;
    uint32_t        attempts;
    uint64_t        next_attempt_ns;
    uint64_t        last_success_ns;
    uint32_t        flap_count;
    bool            flapping;
} retry_slot_t;

static retry_slot_t g_retry[RETRY_SLOTS];
static pthread_mutex_t g_retry_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_max_retries = 5;
static uint32_t g_backoff_initial_ms = 1000;
static uint32_t g_backoff_max_ms = 60000;
static uint32_t g_antiflap_window_ms = 5000;
static uint32_t g_antiflap_max_count = 3;

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void danos_programming_set_policy(uint32_t max_retries,
                                  uint32_t backoff_initial_ms,
                                  uint32_t backoff_max_ms,
                                  uint32_t antiflap_window_ms,
                                  uint32_t antiflap_max_count)
{
    pthread_mutex_lock(&g_retry_lock);
    g_max_retries = max_retries;
    g_backoff_initial_ms = backoff_initial_ms;
    g_backoff_max_ms = backoff_max_ms;
    g_antiflap_window_ms = antiflap_window_ms;
    g_antiflap_max_count = antiflap_max_count;
    memset(g_retry, 0, sizeof(g_retry));
    pthread_mutex_unlock(&g_retry_lock);
}

/* Hash of (type,id) into the retry table. */
static unsigned retry_hash(danos_obj_type_t type, danos_obj_id_t id)
{
    uint64_t h = (uint64_t)type * 1099511628211ULL ^ (id * 2654435761ULL);
    return (unsigned)(h & (RETRY_SLOTS - 1));
}

static retry_slot_t *retry_find(danos_obj_type_t type, danos_obj_id_t id)
{
    for (unsigned i = 0; i < 8; i++) {
        retry_slot_t *s = &g_retry[(retry_hash(type, id) + i) & (RETRY_SLOTS - 1)];
        if (s->used && s->type == type && s->id == id) return s;
    }
    return NULL;
}

/* Find or claim a slot, reusing an empty or evictable one. Caller holds the
 * lock. */
static retry_slot_t *retry_slot_for(danos_obj_type_t type, danos_obj_id_t id)
{
    retry_slot_t *s = retry_find(type, id);
    if (s) return s;
    unsigned base = retry_hash(type, id);
    for (unsigned i = 0; i < RETRY_SLOTS; i++) {
        retry_slot_t *cand = &g_retry[(base + i) & (RETRY_SLOTS - 1)];
        if (!cand->used) {
            memset(cand, 0, sizeof(*cand));
            cand->used = true;
            cand->type = type;
            cand->id = id;
            return cand;
        }
    }
    /* Table full: evict the first probe, accepting a lost backoff record. */
    retry_slot_t *cand = &g_retry[base];
    memset(cand, 0, sizeof(*cand));
    cand->used = true;
    cand->type = type;
    cand->id = id;
    return cand;
}

/* Should this object be skipped this pass? Caller holds the lock. */
static bool retry_defer(danos_obj_type_t type, danos_obj_id_t id, uint64_t now)
{
    retry_slot_t *s = retry_find(type, id);
    if (!s) return false;
    if (s->flapping) return true;
    if (g_max_retries && s->attempts >= g_max_retries) return true;
    return now < s->next_attempt_ns;
}

static void retry_clear(danos_obj_type_t type, danos_obj_id_t id,
                        uint64_t now, bool success)
{
    pthread_mutex_lock(&g_retry_lock);
    retry_slot_t *s = retry_find(type, id);
    if (success && s) {
        /* Count a flap if the object recovered inside the window. */
        if (g_antiflap_max_count && s->attempts > 0) {
            if (now - s->last_success_ns <= g_antiflap_window_ms * 1000000ULL)
                s->flap_count++;
            else
                s->flap_count = 0;
            if (s->flap_count >= g_antiflap_max_count && !s->flapping) {
                s->flapping = true;
            }
        }
        s->last_success_ns = now;
    }
    if (!s || !s->flapping) {
        if (s) memset(s, 0, sizeof(*s));
    }
    pthread_mutex_unlock(&g_retry_lock);
}

static void retry_record_failure(danos_obj_type_t type, danos_obj_id_t id,
                                 uint64_t now)
{
    pthread_mutex_lock(&g_retry_lock);
    retry_slot_t *s = retry_slot_for(type, id);
    s->attempts++;
    uint64_t delay = (uint64_t)g_backoff_initial_ms * 1000000ULL;
    for (uint32_t i = 1; i < s->attempts && delay < g_backoff_max_ms; i++)
        delay *= 2;
    if (delay > (uint64_t)g_backoff_max_ms * 1000000ULL)
        delay = (uint64_t)g_backoff_max_ms * 1000000ULL;
    s->next_attempt_ns = now + delay;
    /* A recovered object starts a fresh flap window. */
    if (now - s->last_success_ns > (uint64_t)g_antiflap_window_ms * 1000000ULL)
        s->flap_count = 0;
    pthread_mutex_unlock(&g_retry_lock);
}

void danos_programming_get_retry_stats(uint64_t *deferred, uint64_t *flapping,
                                       uint64_t *exhausted)
{
    uint64_t d = 0, f = 0, e = 0;
    pthread_mutex_lock(&g_retry_lock);
    for (unsigned i = 0; i < RETRY_SLOTS; i++) {
        if (!g_retry[i].used) continue;
        if (g_retry[i].flapping) f++;
        else if (g_max_retries && g_retry[i].attempts >= g_max_retries) e++;
        else d++;
    }
    pthread_mutex_unlock(&g_retry_lock);
    if (deferred)   *deferred = d;
    if (flapping)   *flapping = f;
    if (exhausted)  *exhausted = e;
}

/* ---- desired-state pass -------------------------------------------------- */

typedef struct {
    uint64_t attempted, ok, failed;
} program_ctx_t;

/* ledger record layout: [u64 dep_digest][object bytes] */
#define LEDGER_REC_MAX (DANOS_MAX_OBJECT_BYTES + 8)

static void program_entry(danos_object_entry_t *e, void *user)
{
    program_ctx_t *c = user;
    if (type_skipped(e->type)) return;

    /* The registry caps payloads at DANOS_MAX_OBJECT_BYTES, so this is
     * unreachable via the public API; the check keeps the fixed buffers
     * below safe if an entry is ever injected by another path. Count it
     * as a failure rather than skipping silently, so an unprogrammable
     * object is visible in the reconciler stats (ADR-0007). */
    if (e->data_size + 8 > LEDGER_REC_MAX) { c->attempted++; c->failed++; return; }

    uint64_t dep = dep_digest(e->type, e->data, e->data_size);

    /* in sync with ledger (object bytes AND dependency digest)? */
    uint8_t last[LEDGER_REC_MAX];
    size_t lsz = sizeof(last);
    bool have_last = danos_object_read(ledger(), e->type, e->id,
                                       last, &lsz) == DANOS_OK;
    if (have_last && lsz == e->data_size + 8) {
        uint64_t last_dep;
        memcpy(&last_dep, last, 8);
        if (last_dep == dep &&
            memcmp(last + 8, e->data, e->data_size) == 0) {
            /* Healthy: drop any accumulated backoff so a later failure
             * starts from a clean slate. */
            retry_clear(e->type, e->id, mono_ns(), true);
            return;   /* in sync */
        }
    }

    /* Backed off, attempt limit reached, or parked as flapping. */
    pthread_mutex_lock(&g_retry_lock);
    bool defer = retry_defer(e->type, e->id, mono_ns());
    pthread_mutex_unlock(&g_retry_lock);
    if (defer) return;

    c->attempted++;
    if (e->type == DANOS_OBJ_IFACE && have_last && lsz == e->data_size + 8 &&
        g_ops->iface_addr_del) {
        danos_iface_t old_iface;
        if (e->data_size == sizeof(old_iface)) {
            memcpy(&old_iface, last + 8, sizeof(old_iface));
            if (memcmp(&old_iface.ipv4_address, &((const danos_iface_t *)e->data)->ipv4_address,
                       sizeof(old_iface.ipv4_address)) != 0 ||
                memcmp(&old_iface.ipv6_address, &((const danos_iface_t *)e->data)->ipv6_address,
                       sizeof(old_iface.ipv6_address)) != 0) {
                danos_status_t dst = g_ops->iface_addr_del(&old_iface, g_ops->user);
                if (dst != DANOS_OK) { c->failed++; return; }
            }
        }
    }
    danos_status_t st = program_one(e->type, e->id, e->data, e->data_size);
    if (st != DANOS_OK) {
        c->failed++;
        retry_record_failure(e->type, e->id, mono_ns());
        /* v0.11: a previously-programmed route whose next hop became
         * unusable must be WITHDRAWN, not left forwarding via a stale
         * gateway. */
        if (st == DANOS_ERR_RETRY && have_last &&
            e->type == DANOS_OBJ_ROUTE && g_ops->route_del &&
            lsz >= 8 + sizeof(danos_route_t)) {
            danos_resolved_route_t rr;
            memset(&rr, 0, sizeof(rr));
            memcpy(&rr.route, last + 8, sizeof(danos_route_t));
            if (g_ops->route_del(&rr, g_ops->user) == DANOS_OK) {
                danos_object_delete(ledger(), e->type, e->id);
                c->failed--;
            }
        }
        return;
    }
    c->ok++;
    retry_clear(e->type, e->id, mono_ns(), true);

    uint8_t rec[LEDGER_REC_MAX];
    memcpy(rec, &dep, 8);
    memcpy(rec + 8, e->data, e->data_size);
    if (have_last && lsz == e->data_size + 8)
        (void)danos_object_update(ledger(), e->type, e->id, rec,
                                  e->data_size + 8);
    else
        (void)danos_object_create(ledger(), e->type, e->id, rec,
                                  e->data_size + 8);
}

uint64_t danos_programming_run(uint64_t *attempted, uint64_t *failed)
{
    program_ctx_t c = {0, 0, 0};
    pthread_mutex_lock(&g_prog_lock);
    if (g_ops && g_default_store) {
        danos_object_iterate(g_default_store, program_entry, &c);
        g_prog_stats.attempted += c.attempted;
        g_prog_stats.ok += c.ok;
        g_prog_stats.failed += c.failed;
    }
    pthread_mutex_unlock(&g_prog_lock);
    if (attempted) *attempted = c.attempted;
    if (failed)    *failed    = c.failed;
    return c.ok;
}

uint64_t danos_programming_programmed_count(danos_obj_type_t type)
{
    return danos_object_count(ledger(), type);
}

/* ---- v0.10: tombstone sweep ----------------------------------------------
 * Ledger entries whose desired object disappeared: routes are withdrawn
 * through ops->route_del (the ledger holds the original bytes, so the
 * withdraw message is fully reconstructible); other types drop their
 * ledger entry. Failures keep the entry for the next sweep.
 */

typedef struct {
    uint64_t issued, failed;
} sweep_ctx_t;

/* The sweep must not mutate the ledger while iterating it (the iterate
 * holds a read lock), so candidates are collected first and withdrawn
 * after the walk completes. */
#define SWEEP_MAX 256

typedef struct {
    danos_obj_type_t type;
    danos_obj_id_t   id;
} sweep_id_t;

static sweep_id_t g_sweep_ids[SWEEP_MAX];
static unsigned g_sweep_n;

static void sweep_collect(danos_object_entry_t *e, void *user)
{
    (void)user;
    if (g_sweep_n >= SWEEP_MAX) return;

    uint8_t last[LEDGER_REC_MAX];
    size_t lsz = sizeof(last);
    if (danos_object_read(ledger(), e->type, e->id, last, &lsz) != DANOS_OK)
        return;   /* not ours */

    /* still desired? */
    uint8_t probe[LEDGER_REC_MAX];
    size_t psz = sizeof(probe);
    if (!user && g_default_store &&
        danos_object_read(g_default_store, e->type, e->id,
                          probe, &psz) == DANOS_OK)
        return;   /* alive */

    g_sweep_ids[g_sweep_n].type = e->type;
    g_sweep_ids[g_sweep_n].id = e->id;
    g_sweep_n++;
}

uint64_t danos_programming_sweep(uint64_t *failed)
{
    sweep_ctx_t c = {0, 0};
    if (!g_programmed) {
        if (failed) *failed = 0;
        return 0;
    }
    g_sweep_n = 0;
    danos_object_iterate(g_programmed, sweep_collect, NULL);

    for (unsigned i = 0; i < g_sweep_n; i++) {
        danos_obj_type_t type = g_sweep_ids[i].type;
        danos_obj_id_t id = g_sweep_ids[i].id;

        uint8_t last[LEDGER_REC_MAX];
        size_t lsz = sizeof(last);
        if (danos_object_read(ledger(), type, id, last, &lsz) != DANOS_OK)
            continue;
        /* No blanket minimum here: each withdraw branch below range-checks
         * its own type. A blanket route-sized guard would skip the ledger
         * delete for every smaller object type and leak the entry. */

        if (type == DANOS_OBJ_ROUTE && g_ops && g_ops->route_del &&
            lsz >= 8 + sizeof(danos_route_t)) {
            danos_route_t r;
            memcpy(&r, last + 8, sizeof(r));
            danos_resolved_route_t rr;
            memset(&rr, 0, sizeof(rr));
            rr.route = r;
            /* best-effort: nh objects may be gone already; the withdraw
             * only needs prefix/table, which the ledger copy provides */
            danos_status_t st = g_ops->route_del(&rr, g_ops->user);
            if (st != DANOS_OK) {
                c.failed++;
                continue;   /* keep ledger entry: retry next sweep */
            }
        }
        if (type == DANOS_OBJ_IFACE && g_ops && g_ops->iface_up &&
            lsz >= 8 + sizeof(danos_iface_t)) {
            /* withdraw = admin down (we never delete kernel ifaces we
             * did not create) */
            danos_iface_t ifc;
            memcpy(&ifc, last + 8, sizeof(ifc));
            (void)g_ops->iface_up(ifc.ifindex, false, g_ops->user);
        }
        /* VRF: registration-only backend ops — ledger drop suffices */
        danos_object_delete(ledger(), type, id);
        c.issued++;
    }
    if (failed) *failed = c.failed;
    return c.issued;
}

uint64_t danos_programming_forget_programmed(void)
{
    if (!g_programmed) return 0;
    g_sweep_n = 0;
    danos_object_iterate(g_programmed, sweep_collect, (void *)1);
    uint64_t forgotten = 0;
    for (unsigned i = 0; i < g_sweep_n; i++) {
        if (danos_object_delete(g_programmed, g_sweep_ids[i].type,
                                g_sweep_ids[i].id) == DANOS_OK)
            forgotten++;
    }
    return forgotten;
}
