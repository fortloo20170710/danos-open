/*
 * DANOS-Open Core: Object Registry implementation (B1)
 */

#include <danos/core/object_registry.h>
#include <danos/core/persist.h>
#include <danos/core/event_bus.h>
#include <danos/core/backend_ops.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

#define DEFAULT_BUCKET_COUNT 1024

danos_object_store_t *g_default_store = NULL;

danos_object_store_t *danos_object_store_create(size_t bucket_count)
{
    if (bucket_count == 0) bucket_count = DEFAULT_BUCKET_COUNT;

    danos_object_store_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->buckets = calloc(bucket_count, sizeof(danos_object_entry_t *));
    if (!s->buckets) { free(s); return NULL; }

    s->bucket_count = bucket_count;
    pthread_rwlock_init(&s->lock, NULL);
    s->count = 0;
    return s;
}

void danos_object_store_destroy(danos_object_store_t *s)
{
    if (!s) return;
    pthread_rwlock_wrlock(&s->lock);
    for (size_t i = 0; i < s->bucket_count; i++) {
        danos_object_entry_t *e = s->buckets[i];
        while (e) {
            danos_object_entry_t *next = e->next;
            free(e->data);
            free(e);
            e = next;
        }
    }
    free(s->buckets);
    pthread_rwlock_unlock(&s->lock);
    pthread_rwlock_destroy(&s->lock);
    free(s);
}

static danos_object_entry_t *find_entry(danos_object_store_t *s,
                                        danos_obj_type_t type,
                                        danos_obj_id_t id)
{
    size_t h = danos_obj_hash(type, id, s->bucket_count);
    danos_object_entry_t *e = s->buckets[h];
    while (e) {
        if (e->type == type && e->id == id) return e;
        e = e->next;
    }
    return NULL;
}

/* Locked insert-or-replace. Caller holds the write lock. */
static danos_status_t store_entry_locked(danos_object_store_t *s,
                                         danos_obj_type_t type,
                                         danos_obj_id_t id,
                                         const void *data, size_t size)
{
    if (size > DANOS_MAX_OBJECT_BYTES) return DANOS_ERR_INVALID_ARG;

    danos_object_entry_t *e = find_entry(s, type, id);
    if (e) {
        /* Replace in place: the id keeps its position in the chain. */
        void *nd = malloc(size);
        if (!nd) return DANOS_ERR_NO_MEMORY;
        memcpy(nd, data, size);
        free(e->data);
        e->data = nd;
        e->data_size = size;
        return DANOS_OK;
    }

    e = calloc(1, sizeof(*e));
    if (!e) return DANOS_ERR_NO_MEMORY;
    e->data = malloc(size);
    if (!e->data) { free(e); return DANOS_ERR_NO_MEMORY; }

    e->type = type;
    e->id = id;
    e->data_size = size;
    memcpy(e->data, data, size);

    size_t h = danos_obj_hash(type, id, s->bucket_count);
    e->next = s->buckets[h];
    s->buckets[h] = e;
    s->count++;
    return DANOS_OK;
}

/* Locked delete. Caller holds the write lock. */
static danos_status_t remove_entry_locked(danos_object_store_t *s,
                                          danos_obj_type_t type,
                                          danos_obj_id_t id)
{
    size_t h = danos_obj_hash(type, id, s->bucket_count);
    danos_object_entry_t **pp = &s->buckets[h];
    while (*pp) {
        if ((*pp)->type == type && (*pp)->id == id) {
            danos_object_entry_t *e = *pp;
            *pp = e->next;
            free(e->data);
            free(e);
            s->count--;
            return DANOS_OK;
        }
        pp = &(*pp)->next;
    }
    return DANOS_ERR_NOT_FOUND;
}

danos_status_t danos_object_create(danos_object_store_t *s,
                                   danos_obj_type_t type,
                                   danos_obj_id_t id,
                                   const void *data, size_t size)
{
    if (!s || !data || size == 0) return DANOS_ERR_INVALID_ARG;
    if (size > DANOS_MAX_OBJECT_BYTES) return DANOS_ERR_INVALID_ARG;

    pthread_rwlock_wrlock(&s->lock);
    if (find_entry(s, type, id)) {
        pthread_rwlock_unlock(&s->lock);
        return DANOS_ERR_EXISTS;
    }
    danos_status_t rc = store_entry_locked(s, type, id, data, size);
    pthread_rwlock_unlock(&s->lock);
    if (rc != DANOS_OK) return rc;
    danos_persist_on_mutation(s, 1 /* WAL_OP_CREATE */, (int)type, id, data, size);

    danos_programming_mark_dirty();
    if (g_event_bus) {
        danos_event_t ev = {0};
        ev.type = DANOS_EVENT_OBJ_CREATED;
        ev.obj_type = type;
        ev.obj_id = id;
        ev.timestamp_ns = (uint64_t)time(NULL) * 1000000000ULL;
        danos_event_publish(&ev);
    }
    return DANOS_OK;
}

danos_status_t danos_object_read(danos_object_store_t *s,
                                 danos_obj_type_t type,
                                 danos_obj_id_t id,
                                 void *out, size_t *out_size)
{
    if (!s || !out_size) return DANOS_ERR_INVALID_ARG;

    pthread_rwlock_rdlock(&s->lock);
    danos_object_entry_t *e = find_entry(s, type, id);
    if (!e) { pthread_rwlock_unlock(&s->lock); return DANOS_ERR_NOT_FOUND; }

    /* If out is NULL, just return the required size */
    if (out == NULL) {
        *out_size = e->data_size;
        pthread_rwlock_unlock(&s->lock);
        return DANOS_ERR_INVALID_ARG;  /* signals: exists, need bigger buffer */
    }

    if (*out_size < e->data_size) {
        *out_size = e->data_size;
        pthread_rwlock_unlock(&s->lock);
        return DANOS_ERR_INVALID_ARG;  /* buffer too small */
    }
    memcpy(out, e->data, e->data_size);
    *out_size = e->data_size;
    pthread_rwlock_unlock(&s->lock);
    return DANOS_OK;
}

danos_status_t danos_object_read_copy(danos_object_store_t *s,
                                      danos_obj_type_t type,
                                      danos_obj_id_t id,
                                      void **data, size_t *size)
{
    if (!s || !data || !size) return DANOS_ERR_INVALID_ARG;
    *data = NULL;
    *size = 0;
    pthread_rwlock_rdlock(&s->lock);
    danos_object_entry_t *e = find_entry(s, type, id);
    if (!e) {
        pthread_rwlock_unlock(&s->lock);
        return DANOS_ERR_NOT_FOUND;
    }
    void *copy = malloc(e->data_size);
    if (!copy) {
        pthread_rwlock_unlock(&s->lock);
        return DANOS_ERR_NO_MEMORY;
    }
    memcpy(copy, e->data, e->data_size);
    *data = copy;
    *size = e->data_size;
    pthread_rwlock_unlock(&s->lock);
    return DANOS_OK;
}

danos_status_t danos_object_update(danos_object_store_t *s,
                                   danos_obj_type_t type,
                                   danos_obj_id_t id,
                                   const void *data, size_t size)
{
    if (!s || !data || size == 0) return DANOS_ERR_INVALID_ARG;
    if (size > DANOS_MAX_OBJECT_BYTES) return DANOS_ERR_INVALID_ARG;

    pthread_rwlock_wrlock(&s->lock);
    danos_object_entry_t *e = find_entry(s, type, id);
    if (!e) { pthread_rwlock_unlock(&s->lock); return DANOS_ERR_NOT_FOUND; }

    void *new_data = malloc(size);
    if (!new_data) { pthread_rwlock_unlock(&s->lock); return DANOS_ERR_NO_MEMORY; }
    memcpy(new_data, data, size);

    free(e->data);
    e->data = new_data;
    e->data_size = size;
    pthread_rwlock_unlock(&s->lock);
    danos_persist_on_mutation(s, 2 /* WAL_OP_UPDATE */, (int)type, id, data, size);

    danos_programming_mark_dirty();
    if (g_event_bus) {
        danos_event_t ev = {0};
        ev.type = DANOS_EVENT_OBJ_UPDATED;
        ev.obj_type = type;
        ev.obj_id = id;
        ev.timestamp_ns = (uint64_t)time(NULL) * 1000000000ULL;
        danos_event_publish(&ev);
    }
    return DANOS_OK;
}

danos_status_t danos_object_delete(danos_object_store_t *s,
                                   danos_obj_type_t type,
                                   danos_obj_id_t id)
{
    if (!s) return DANOS_ERR_INVALID_ARG;

    pthread_rwlock_wrlock(&s->lock);
    size_t h = danos_obj_hash(type, id, s->bucket_count);
    danos_object_entry_t *prev = NULL;
    danos_object_entry_t *e = s->buckets[h];
    while (e) {
        if (e->type == type && e->id == id) {
            if (prev) prev->next = e->next;
            else      s->buckets[h] = e->next;
            free(e->data);
            free(e);
            s->count--;
            pthread_rwlock_unlock(&s->lock);
            danos_persist_on_mutation(s, 3 /* WAL_OP_DELETE */, (int)type, id,
                                      NULL, 0);
            danos_programming_mark_dirty();
            if (g_event_bus) {
                danos_event_t ev = {0};
                ev.type = DANOS_EVENT_OBJ_DELETED;
                ev.obj_type = type;
                ev.obj_id = id;
                ev.timestamp_ns = (uint64_t)time(NULL) * 1000000000ULL;
                danos_event_publish(&ev);
            }
            return DANOS_OK;
        }
        prev = e;
        e = e->next;
    }
    pthread_rwlock_unlock(&s->lock);
    return DANOS_ERR_NOT_FOUND;
}

uint64_t danos_object_count(danos_object_store_t *s, danos_obj_type_t type)
{
    if (!s) return 0;
    uint64_t cnt = 0;
    pthread_rwlock_rdlock(&s->lock);
    for (size_t i = 0; i < s->bucket_count; i++) {
        danos_object_entry_t *e = s->buckets[i];
        while (e) {
            if (e->type == type) cnt++;
            e = e->next;
        }
    }
    pthread_rwlock_unlock(&s->lock);
    return cnt;
}

danos_obj_id_t danos_object_max_id(danos_object_store_t *s,
                                   danos_obj_type_t type)
{
    if (!s) return 0;
    danos_obj_id_t max = 0;
    pthread_rwlock_rdlock(&s->lock);
    for (size_t i = 0; i < s->bucket_count; i++) {
        for (danos_object_entry_t *e = s->buckets[i]; e; e = e->next)
            if (e->type == type && e->id > max) max = e->id;
    }
    pthread_rwlock_unlock(&s->lock);
    return max;
}

/*
 * Iterate a snapshot of the store.
 *
 * Callbacks run with the store lock *released*. Holding the rwlock across
 * the callback was wrong in two ways:
 *
 *  - A callback that reads the same store re-enters the rwlock for read.
 *    POSIX read locks are not reentrant-safe: if a writer queues between
 *    the two acquisitions the second rdlock blocks behind it while the
 *    first is still held, deadlocking the caller. TSAN flags this as a
 *    lock-order inversion, and it is reachable today:
 *    danos_programming_sweep() iterates the ledger while sweep_collect()
 *    calls danos_object_read() on that same ledger.
 *
 *  - Callbacks do real work. The programming pass reaches netlink and a
 *    VPP socket, so holding the lock blocked every concurrent CRUD for
 *    the duration of a backend round trip.
 *
 * Entries are deep-copied so a concurrent delete cannot free the memory
 * underneath a callback.
 */
/* Apply mutations holding the write lock for the whole batch. See the header
 * for the atomicity contract and its limits. */
int danos_object_apply_batch(danos_object_store_t *store,
                             const danos_object_mutation_t *muts, size_t n)
{
    if (!store || (!muts && n)) return DANOS_ERR_INVALID_ARG;
    if (n == 0) return 0;

    int applied = 0;
    pthread_rwlock_wrlock(&store->lock);
    for (size_t i = 0; i < n; i++) {
        const danos_object_mutation_t *m = &muts[i];
        if (m->data_size > DANOS_MAX_OBJECT_BYTES) {
            pthread_rwlock_unlock(&store->lock);
            return applied ? -DANOS_ERR_PARTIAL : -DANOS_ERR_INVALID_ARG;
        }
        if (m->remove) {
            if (remove_entry_locked(store, m->type, m->id) != DANOS_OK) {
                /* Removing something absent is not an error for a batch:
                 * the desired end state is what matters. */
                applied++;
                continue;
            }
        } else if (store_entry_locked(store, m->type, m->id, m->data,
                                      m->data_size)
                   != DANOS_OK) {
            pthread_rwlock_unlock(&store->lock);
            return applied ? -DANOS_ERR_PARTIAL : -DANOS_ERR_EXISTS;
        }
        applied++;
    }
    pthread_rwlock_unlock(&store->lock);

    /* Events carry no store state and would take the event-bus lock, so they
     * are published after the store lock is released. Publishing them while
     * holding it would invert the lock order against a subscriber that
     * touches this store. */
    for (size_t i = 0; i < (size_t)applied; i++) {
        const danos_object_mutation_t *m = &muts[i];
        danos_persist_on_mutation(store, m->remove ? 3 : 1, (int)m->type,
                                  m->id, m->data, m->data_size);
        danos_programming_mark_dirty();
        if (!g_event_bus) continue;
        danos_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = m->remove ? DANOS_EVENT_OBJ_DELETED
                            : DANOS_EVENT_OBJ_CREATED;
        ev.obj_type = m->type;
        ev.obj_id = m->id;
        ev.timestamp_ns = (uint64_t)time(NULL) * 1000000000ULL;
        danos_event_publish(&ev);
    }
    return applied;
}

int danos_object_apply_transaction_batch(danos_object_store_t *store,
                                        const danos_object_mutation_t *muts,
                                        size_t n, uint64_t tx_id)
{
    if (!store || (!muts && n)) return DANOS_ERR_INVALID_ARG;
    if (!n) return 0;
    danos_object_entry_t **prepared = calloc(n, sizeof(*prepared));
    bool *was_present = calloc(n, sizeof(*was_present));
    if (!prepared || !was_present) {
        free(prepared); free(was_present);
        return -DANOS_ERR_NO_MEMORY;
    }
    for (size_t i = 0; i < n; i++) {
        const danos_object_mutation_t *m = &muts[i];
        if (m->data_size > DANOS_MAX_OBJECT_BYTES ||
            (!m->remove && (!m->data || !m->data_size))) goto invalid_batch;
        for (size_t j = 0; j < i; j++)
            if (muts[j].type == m->type && muts[j].id == m->id)
                goto invalid_batch;
        if (!m->remove) {
            prepared[i] = calloc(1, sizeof(*prepared[i]));
            if (!prepared[i]) goto no_memory;
            prepared[i]->data = malloc(m->data_size);
            if (!prepared[i]->data) goto no_memory;
            memcpy(prepared[i]->data, m->data, m->data_size);
            prepared[i]->type = m->type;
            prepared[i]->id = m->id;
            prepared[i]->data_size = m->data_size;
        }
    }

    pthread_rwlock_wrlock(&store->lock);
    for (size_t i = 0; i < n; i++) {
        const danos_object_mutation_t *m = &muts[i];
        danos_object_entry_t *e = find_entry(store, m->type, m->id);
        was_present[i] = e != NULL;
        bool same = (e != NULL) == m->base_exists;
        if (same && e && m->base_exists)
            same = e->data_size == m->base_size &&
                   memcmp(e->data, m->base_data, e->data_size) == 0;
        if (!same) {
            pthread_rwlock_unlock(&store->lock);
            for (size_t j = 0; j < n; j++)
                if (prepared[j]) { free(prepared[j]->data); free(prepared[j]); }
            free(prepared); free(was_present);
            return -DANOS_ERR_TX_CONFLICT;
        }
    }
    if (danos_persist_log_transaction(tx_id, muts, n) != 0) {
        pthread_rwlock_unlock(&store->lock);
        for (size_t j = 0; j < n; j++)
            if (prepared[j]) { free(prepared[j]->data); free(prepared[j]); }
        free(prepared); free(was_present);
        return -DANOS_ERR_BACKEND_IO;
    }
    for (size_t i = 0; i < n; i++) {
        const danos_object_mutation_t *m = &muts[i];
        danos_object_entry_t *e = find_entry(store, m->type, m->id);
        if (m->remove) {
            if (e) (void)remove_entry_locked(store, m->type, m->id);
        } else if (e) {
            free(e->data);
            e->data = prepared[i]->data;
            e->data_size = prepared[i]->data_size;
            prepared[i]->data = NULL;
        } else {
            size_t h = danos_obj_hash(m->type, m->id, store->bucket_count);
            prepared[i]->next = store->buckets[h];
            store->buckets[h] = prepared[i];
            store->count++;
            prepared[i] = NULL;
        }
    }
    pthread_rwlock_unlock(&store->lock);
    for (size_t i = 0; i < n; i++) {
        const danos_object_mutation_t *m = &muts[i];
        danos_programming_mark_dirty();
        if (g_event_bus) {
            danos_event_t ev = {0};
            ev.type = m->remove ? DANOS_EVENT_OBJ_DELETED
                      : (was_present[i] ? DANOS_EVENT_OBJ_UPDATED
                                         : DANOS_EVENT_OBJ_CREATED);
            ev.obj_type = m->type;
            ev.obj_id = m->id;
            ev.timestamp_ns = (uint64_t)time(NULL) * 1000000000ULL;
            danos_event_publish(&ev);
        }
        if (prepared[i]) { free(prepared[i]->data); free(prepared[i]); }
    }
    free(prepared); free(was_present);
    return (int)n;

invalid_batch:
    for (size_t i = 0; i < n; i++)
        if (prepared[i]) { free(prepared[i]->data); free(prepared[i]); }
    free(prepared); free(was_present);
    return -DANOS_ERR_INVALID_ARG;
no_memory:
    for (size_t i = 0; i < n; i++)
        if (prepared[i]) { free(prepared[i]->data); free(prepared[i]); }
    free(prepared); free(was_present);
    return -DANOS_ERR_NO_MEMORY;
}

void danos_object_iterate(danos_object_store_t *store,
                          danos_object_iter_cb_t cb, void *user)
{
    if (!store || !cb) return;

    size_t n = 0;
    pthread_rwlock_rdlock(&store->lock);
    n = store->count;
    pthread_rwlock_unlock(&store->lock);
    if (n == 0) return;

    /*
     * Over-allocate: a writer may add objects between the count read and
     * the snapshot. Growing is handled below, and one extra slot keeps
     * the common case (no concurrent writes) allocation-free.
     */
    size_t cap = n + 8;
    struct snap {
        danos_obj_type_t type;
        danos_obj_id_t   id;
        void             *data;
        size_t            data_size;
    } *arr = calloc(cap, sizeof(*arr));
    if (!arr) return;   /* iteration is advisory; skip rather than fail */

    size_t k = 0;
    pthread_rwlock_rdlock(&store->lock);
    for (size_t i = 0; i < store->bucket_count && k < cap; i++) {
        for (danos_object_entry_t *e = store->buckets[i]; e && k < cap;
             e = e->next) {
            void *copy = malloc(e->data_size);
            if (!copy) goto done;          /* partial snapshot: drop the rest */
            memcpy(copy, e->data, e->data_size);
            arr[k].type = e->type;
            arr[k].id = e->id;
            arr[k].data = copy;
            arr[k].data_size = e->data_size;
            k++;
        }
    }
done:
    pthread_rwlock_unlock(&store->lock);

    for (size_t i = 0; i < k; i++) {
        danos_object_entry_t e = {
            .type = arr[i].type,
            .id = arr[i].id,
            .data = arr[i].data,
            .data_size = arr[i].data_size,
        };
        cb(&e, user);
    }

    for (size_t i = 0; i < k; i++) free(arr[i].data);
    free(arr);
}
