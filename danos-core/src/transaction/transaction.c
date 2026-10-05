/*
 * DANOS-Open Core: Transaction Engine implementation (B3, B4, B5)
 *
 * Implements the public DPA transaction API from danos/dpa.h.
 * - B3: state machine OPEN→PREPARE→VALIDATE→COMMIT→VERIFY→DONE
 * - B4: multi-read single-write (global write mutex for commit)
 * - B5: per-phase timeouts
 */

#include <danos/core/transaction.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/types.h>

danos_tx_manager_t *g_tx_mgr = NULL;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t tx_id_seed(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static danos_tx_record_t *reclaim_expired(void);

int danos_tx_manager_init(void)
{
    if (g_tx_mgr) return 0;
    g_tx_mgr = calloc(1, sizeof(*g_tx_mgr));
    if (!g_tx_mgr) return -1;
    pthread_mutex_init(&g_tx_mgr->write_lock, NULL);
    pthread_mutex_init(&g_tx_mgr->pool_lock, NULL);
    g_tx_mgr->free_list = NULL;
    /* Keep transaction WAL ids out of the reserved legacy immediate stream
     * (id 1), and avoid id reuse across daemon restarts. */
    g_tx_mgr->next_tx_id = tx_id_seed();
    g_tx_mgr->active_count = 0;
    return 0;
}

void danos_tx_manager_fini(void)
{
    if (!g_tx_mgr) return;
    pthread_mutex_lock(&g_tx_mgr->pool_lock);
    /* Every record is on the live list exactly once, whether it is
     * currently pooled or in use; walking that list frees all of them.
     * Walking free_list alone would miss any record still checked out. */
    danos_tx_record_t *r = g_tx_mgr->live_list;
    while (r) {
        danos_tx_record_t *next = r->live_next;
        for (size_t i = 0; i < r->staged_count; i++) {
            free(r->staged[i].data);
            free(r->staged[i].base_data);
        }
        free(r->staged);
        pthread_mutex_destroy(&r->lock);
        free(r);
        r = next;
    }
    g_tx_mgr->free_list = NULL;
    g_tx_mgr->live_list = NULL;
    pthread_mutex_unlock(&g_tx_mgr->pool_lock);
    pthread_mutex_destroy(&g_tx_mgr->write_lock);
    pthread_mutex_destroy(&g_tx_mgr->pool_lock);
    free(g_tx_mgr);
    g_tx_mgr = NULL;
}

danos_tx_record_t *danos_tx_record_alloc(void)
{
    pthread_mutex_lock(&g_tx_mgr->pool_lock);
    danos_tx_record_t *r = g_tx_mgr->free_list;
    if (r) {
        g_tx_mgr->free_list = r->next;
        r->next = NULL;
    }
    pthread_mutex_unlock(&g_tx_mgr->pool_lock);

    if (!r) r = reclaim_expired();
    if (!r) {
        r = calloc(1, sizeof(*r));
        if (!r) return NULL;
        pthread_mutex_init(&r->lock, NULL);

        /* Track every record so an abandoned one can still be reclaimed. */
        pthread_mutex_lock(&g_tx_mgr->pool_lock);
        r->seq = g_tx_mgr->next_seq++;
        r->live_next = g_tx_mgr->live_list;
        g_tx_mgr->live_list = r;
        pthread_mutex_unlock(&g_tx_mgr->pool_lock);
    }
    pthread_mutex_lock(&r->lock);
    memset(&r->pub, 0, sizeof(r->pub));
    __atomic_store_n(&r->in_use, false, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&r->lock);
    return r;
}

/*
 * Recycle the oldest transaction whose hard deadline has passed.
 *
 * Terminal transitions retire records, so this only ever fires for a
 * caller that began a transaction and then abandoned it without commit,
 * abort, verify or rollback. Without a backstop, one abandoned
 * transaction per remote gNMI/CLI request is an unbounded memory leak.
 *
 * The record memory stays valid (records are pooled, never freed), so the
 * worst case for a stale caller handle is that it observes a recycled
 * record rather than corrupting memory. Reclaiming only past-deadline
 * transactions keeps that window irrelevant: the transaction was already
 * contractually over.
 */
static danos_tx_record_t *reclaim_expired(void)
{
    uint64_t now = now_ns();
    danos_tx_record_t *best = NULL;

    pthread_mutex_lock(&g_tx_mgr->pool_lock);
    for (danos_tx_record_t *r = g_tx_mgr->live_list; r; r = r->live_next) {
        if (!__atomic_load_n(&r->in_use, __ATOMIC_SEQ_CST) || r->reclaiming) continue;
        if (r->pub.deadline_ns == 0 || now < r->pub.deadline_ns) continue;
        if (!best || r->seq < best->seq) best = r;
    }
    if (best) best->reclaiming = true;
    pthread_mutex_unlock(&g_tx_mgr->pool_lock);

    if (best) {
        pthread_mutex_lock(&best->lock);
        now = now_ns();
        bool expired = __atomic_load_n(&best->in_use, __ATOMIC_SEQ_CST) &&
                       best->pub.deadline_ns != 0 &&
                       now >= best->pub.deadline_ns &&
                       best->pub.state < DANOS_TX_COMMIT;
        if (expired) {
        for (size_t i = 0; i < best->staged_count; i++) {
            free(best->staged[i].data);
            free(best->staged[i].base_data);
        }
        free(best->staged);
        best->staged = NULL;
        best->staged_count = best->staged_capacity = 0;
        __atomic_store_n(&best->in_use, false, __ATOMIC_SEQ_CST);
        best->pub.state = DANOS_TX_ABORT;
        best->pub._internal = NULL;
        __atomic_sub_fetch(&g_tx_mgr->active_count, 1, __ATOMIC_SEQ_CST);
        }
        pthread_mutex_unlock(&best->lock);
        pthread_mutex_lock(&g_tx_mgr->pool_lock);
        best->reclaiming = false;
        pthread_mutex_unlock(&g_tx_mgr->pool_lock);
        if (!expired) best = NULL;
    }
    return best;
}

uint64_t danos_tx_active_count(void)
{
    if (!g_tx_mgr) return 0;
    return __atomic_load_n(&g_tx_mgr->active_count, __ATOMIC_SEQ_CST);
}

void danos_tx_record_free(danos_tx_record_t *r)
{
    if (!r) return;
    pthread_mutex_lock(&g_tx_mgr->pool_lock);
    r->next = g_tx_mgr->free_list;
    g_tx_mgr->free_list = r;
    pthread_mutex_unlock(&g_tx_mgr->pool_lock);
}

danos_tx_record_t *danos_tx_validate_ptr(danos_tx_t *tx)
{
    if (!tx) return NULL;
    return (danos_tx_record_t *)tx->_internal;
}

static ssize_t staged_index(danos_tx_record_t *rec, danos_obj_type_t type,
                            danos_obj_id_t id)
{
    for (size_t i = 0; i < rec->staged_count; i++)
        if (rec->staged[i].type == type && rec->staged[i].id == id)
            return (ssize_t)i;
    return -1;
}

danos_status_t danos_tx_stage_object(danos_tx_t *tx, danos_obj_type_t type,
                                     danos_obj_id_t id, const void *data,
                                     size_t size,
                                     danos_tx_operation_t operation)
{
    if (!tx || (!data && operation != DANOS_TX_OP_DELETE) ||
        operation < DANOS_TX_OP_CREATE || operation > DANOS_TX_OP_DELETE ||
        size > DANOS_MAX_OBJECT_BYTES)
        return DANOS_ERR_INVALID_ARG;
    danos_tx_record_t *rec = danos_tx_validate_ptr(tx);
    if (!rec) return DANOS_ERR_TX_INVALID;
    pthread_mutex_lock(&rec->lock);
    if (!__atomic_load_n(&rec->in_use, __ATOMIC_SEQ_CST) || rec->pub.state != DANOS_TX_OPEN) {
        pthread_mutex_unlock(&rec->lock);
        return DANOS_ERR_TX_INVALID;
    }

    ssize_t index = staged_index(rec, type, id);
    bool exists;
    bool new_item = false;
    danos_object_mutation_t *m = NULL;
    if (index >= 0) {
        m = &rec->staged[index];
        exists = !m->remove;
    } else {
        void *base = NULL;
        size_t base_size = 0;
        danos_status_t read_st = danos_object_read_copy(g_default_store, type,
                                                        id, &base, &base_size);
        exists = read_st == DANOS_OK;
        if (read_st != DANOS_OK && read_st != DANOS_ERR_NOT_FOUND) {
            pthread_mutex_unlock(&rec->lock);
            return read_st;
        }
        if ((operation == DANOS_TX_OP_CREATE && exists) ||
            ((operation == DANOS_TX_OP_UPDATE ||
              operation == DANOS_TX_OP_DELETE) && !exists)) {
            free(base);
            pthread_mutex_unlock(&rec->lock);
            return operation == DANOS_TX_OP_CREATE ? DANOS_ERR_EXISTS
                                                    : DANOS_ERR_NOT_FOUND;
        }
        if (rec->staged_count == rec->staged_capacity) {
            size_t cap = rec->staged_capacity ? rec->staged_capacity * 2 : 8;
            danos_object_mutation_t *next = realloc(rec->staged,
                                                      cap * sizeof(*next));
            if (!next) {
                free(base);
                pthread_mutex_unlock(&rec->lock);
                return DANOS_ERR_NO_MEMORY;
            }
            rec->staged = next;
            rec->staged_capacity = cap;
        }
        m = &rec->staged[rec->staged_count++];
        memset(m, 0, sizeof(*m));
        m->type = type;
        m->id = id;
        m->base_exists = exists;
        m->base_data = base;
        m->base_size = base_size;
        new_item = true;
    }

    if ((operation == DANOS_TX_OP_CREATE && exists) ||
        ((operation == DANOS_TX_OP_UPDATE ||
          operation == DANOS_TX_OP_DELETE) && !exists)) {
        pthread_mutex_unlock(&rec->lock);
        return operation == DANOS_TX_OP_CREATE ? DANOS_ERR_EXISTS
                                                : DANOS_ERR_NOT_FOUND;
    }
    void *copy = NULL;
    if (operation != DANOS_TX_OP_DELETE) {
        copy = malloc(size);
        if (!copy) {
            if (new_item) {
                free(m->base_data);
                memset(m, 0, sizeof(*m));
                rec->staged_count--;
            }
            pthread_mutex_unlock(&rec->lock);
            return DANOS_ERR_NO_MEMORY;
        }
        memcpy(copy, data, size);
    }
    free(m->data);
    m->data = copy;
    m->data_size = size;
    m->remove = operation == DANOS_TX_OP_DELETE;
    pthread_mutex_unlock(&rec->lock);
    return DANOS_OK;
}

danos_status_t danos_tx_read_staged(danos_tx_t *tx, danos_obj_type_t type,
                                    danos_obj_id_t id, void *out, size_t *size,
                                    bool *handled)
{
    if (!handled) return DANOS_ERR_INVALID_ARG;
    *handled = false;
    if (!tx) return DANOS_OK;
    danos_tx_record_t *rec = danos_tx_validate_ptr(tx);
    if (!rec) return DANOS_ERR_TX_INVALID;
    pthread_mutex_lock(&rec->lock);
    if (!__atomic_load_n(&rec->in_use, __ATOMIC_SEQ_CST) || rec->pub.state > DANOS_TX_VALIDATE) {
        pthread_mutex_unlock(&rec->lock);
        return DANOS_ERR_TX_INVALID;
    }
    ssize_t index = staged_index(rec, type, id);
    if (index < 0) {
        pthread_mutex_unlock(&rec->lock);
        return DANOS_OK;
    }
    danos_object_mutation_t *m = &rec->staged[index];
    *handled = true;
    if (m->remove) {
        pthread_mutex_unlock(&rec->lock);
        return DANOS_ERR_NOT_FOUND;
    }
    if (!m->data) {
        pthread_mutex_unlock(&rec->lock);
        return DANOS_ERR_INTERNAL;
    }
    if (!out || !size || *size < m->data_size) {
        if (size) *size = m->data_size;
        pthread_mutex_unlock(&rec->lock);
        return DANOS_ERR_INVALID_ARG;
    }
    memcpy(out, m->data, m->data_size);
    *size = m->data_size;
    pthread_mutex_unlock(&rec->lock);
    return DANOS_OK;
}

/*
 * Retire a transaction: detach it from the caller's handle, drop the
 * active count and hand the record back to the pool.
 *
 * The caller's `_internal` back-pointer is cleared *before* the record
 * becomes reusable, so a stale handle fails with DANOS_ERR_TX_INVALID in
 * validate_ptr instead of touching a record another thread may already
 * own. Idempotent via the in_use guard.
 */
static void tx_retire(danos_tx_record_t *rec, danos_tx_t *tx)
{
    if (!rec) return;
    pthread_mutex_lock(&rec->lock);
    if (!__atomic_load_n(&rec->in_use, __ATOMIC_SEQ_CST)) {
        pthread_mutex_unlock(&rec->lock);
        return;
    }
    for (size_t i = 0; i < rec->staged_count; i++) {
        free(rec->staged[i].data);
        free(rec->staged[i].base_data);
    }
    free(rec->staged);
    rec->staged = NULL;
    rec->staged_count = rec->staged_capacity = 0;
    if (tx) tx->_internal = NULL;
    rec->pub._internal = NULL;
    __atomic_store_n(&rec->in_use, false, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&rec->lock);
    __atomic_sub_fetch(&g_tx_mgr->active_count, 1, __ATOMIC_SEQ_CST);
    danos_tx_record_free(rec);
}

void danos_tx_release(danos_tx_t *tx)
{
    if (!tx) return;
    tx_retire(danos_tx_validate_ptr(tx), tx);
}

bool danos_tx_in_state(danos_tx_record_t *rec, danos_tx_state_t expected)
{
    return rec->pub.state == expected;
}

/* =========================================================================
 * Public DPA Transaction API implementation
 * ========================================================================= */

static const danos_tx_timeouts_t kDefaultTimeouts = {
    .prepare_ms = 100,
    .commit_ms  = 500,
    .verify_ms  = 1000,
};

danos_status_t danos_tx_begin(danos_tx_t *tx,
                              const char *initiator,
                              const danos_tx_timeouts_t *timeouts)
{
    if (!tx) return DANOS_ERR_INVALID_ARG;
    if (!g_tx_mgr) danos_tx_manager_init();

    danos_tx_record_t *rec = danos_tx_record_alloc();
    if (!rec) return DANOS_ERR_NO_MEMORY;

    pthread_mutex_lock(&rec->lock);
    rec->pub.id = __atomic_fetch_add(&g_tx_mgr->next_tx_id, 1, __ATOMIC_SEQ_CST);
    rec->pub.state = DANOS_TX_OPEN;
    rec->pub.start_ns = now_ns();

    const danos_tx_timeouts_t *t = timeouts ? timeouts : &kDefaultTimeouts;
    rec->pub.deadline_ns = rec->pub.start_ns +
        (uint64_t)(t->prepare_ms + t->commit_ms + t->verify_ms) * 1000000ULL;

    if (initiator) {
        /* Store initiator pointer (caller must keep it alive for tx duration) */
        rec->pub.initiator = initiator;
    } else {
        rec->pub.initiator = "unknown";
    }

    /* Set internal pointer so caller's tx can find the record */
    rec->pub._internal = rec;

    /* Copy public view to caller */
    *tx = rec->pub;
    __atomic_store_n(&rec->in_use, true, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&rec->lock);

    __atomic_add_fetch(&g_tx_mgr->active_count, 1, __ATOMIC_SEQ_CST);
    return DANOS_OK;
}

danos_status_t danos_tx_prepare(danos_tx_t *tx)
{
    danos_tx_record_t *rec = danos_tx_validate_ptr(tx);
    if (!rec) return DANOS_ERR_TX_INVALID;
    pthread_mutex_lock(&rec->lock);
    if (rec->pub.state != DANOS_TX_OPEN) {
        pthread_mutex_unlock(&rec->lock);
        return DANOS_ERR_TX_INVALID;
    }
    rec->pub.state = DANOS_TX_PREPARE;
    tx->state = DANOS_TX_PREPARE;
    pthread_mutex_unlock(&rec->lock);
    return DANOS_OK;
}

danos_status_t danos_tx_validate(danos_tx_t *tx)
{
    danos_tx_record_t *rec = danos_tx_validate_ptr(tx);
    if (!rec) return DANOS_ERR_TX_INVALID;
    pthread_mutex_lock(&rec->lock);
    if (rec->pub.state != DANOS_TX_PREPARE) {
        pthread_mutex_unlock(&rec->lock);
        return DANOS_ERR_TX_INVALID;
    }
    rec->pub.state = DANOS_TX_VALIDATE;
    tx->state = DANOS_TX_VALIDATE;
    pthread_mutex_unlock(&rec->lock);
    return DANOS_OK;
}

danos_status_t danos_tx_commit(danos_tx_t *tx)
{
    danos_tx_record_t *rec = danos_tx_validate_ptr(tx);
    if (!rec) return DANOS_ERR_TX_INVALID;
    pthread_mutex_lock(&rec->lock);
    if (rec->pub.state != DANOS_TX_VALIDATE) {
        pthread_mutex_unlock(&rec->lock);
        return DANOS_ERR_TX_INVALID;
    }

    /* Acquire global write lock (single-writer) */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 1;  /* 1s max wait for write lock */
    int rc = pthread_mutex_timedlock(&g_tx_mgr->write_lock, &deadline);
    if (rc == ETIMEDOUT) {
        rec->pub.state = DANOS_TX_ABORT;
        tx->state = DANOS_TX_ABORT;
        pthread_mutex_unlock(&rec->lock);
        tx_retire(rec, tx);
        return DANOS_ERR_TX_TIMEOUT;
    }

    rec->pub.state = DANOS_TX_COMMIT;
    tx->state = DANOS_TX_COMMIT;
    pthread_mutex_unlock(&rec->lock);

    /* Compare captured base snapshots and publish all staged changes under
     * one object-store write lock. */
    danos_status_t apply_status = DANOS_OK;
    if (apply_status == DANOS_OK && rec->staged_count) {
        int applied = danos_object_apply_transaction_batch(g_default_store,
                                               rec->staged, rec->staged_count,
                                               rec->pub.id);
        if (applied < 0)
            apply_status = (danos_status_t)(-applied);
        else if ((size_t)applied != rec->staged_count)
            apply_status = DANOS_ERR_INTERNAL;
    }
    if (apply_status != DANOS_OK) {
        pthread_mutex_lock(&rec->lock);
        rec->pub.state = DANOS_TX_VALIDATE;
        tx->state = DANOS_TX_VALIDATE;
        pthread_mutex_unlock(&rec->lock);
        pthread_mutex_unlock(&g_tx_mgr->write_lock);
        return apply_status;
    }

    /* Release write lock after commit */
    pthread_mutex_unlock(&g_tx_mgr->write_lock);

    /* Commit is the last step for most callers (the management plane
     * never follows it with verify), so retire the record here. Any later
     * verify/rollback still works: those transitions read tx->state, and
     * tx_retire is a no-op when the record is already detached. */
    tx_retire(rec, tx);
    return DANOS_OK;
}

/*
 * Terminal transitions (verify/abort/rollback) read and write the state
 * held in the caller's own danos_tx_t rather than the record's copy.
 *
 * Every caller owns its danos_tx_t on its own stack, so the record lock
 * never serialised anything between threads - it only guarded a struct
 * the caller already had a copy of. Meanwhile commit may retire the
 * record (see danos_tx_commit), so depending on it here would read a
 * record another thread could already own. The state copy is the
 * authoritative view for post-commit transitions; the record exists to
 * carry the cross-thread bookkeeping (id, deadline, active count).
 *
 * State values are ordered so an enum comparison tracks the lifecycle;
 * the explicit spellings below keep each precondition obvious.
 */
danos_status_t danos_tx_verify(danos_tx_t *tx)
{
    if (!tx) return DANOS_ERR_INVALID_ARG;
    if (tx->state != DANOS_TX_COMMIT) return DANOS_ERR_TX_INVALID;

    tx->state = DANOS_TX_VERIFY;
    tx->state = DANOS_TX_DONE;
    tx_retire(danos_tx_validate_ptr(tx), tx);
    return DANOS_OK;
}

danos_status_t danos_tx_abort(danos_tx_t *tx)
{
    if (!tx) return DANOS_ERR_INVALID_ARG;
    danos_tx_record_t *rec = danos_tx_validate_ptr(tx);
    if (!rec) return DANOS_ERR_TX_INVALID;
    pthread_mutex_lock(&rec->lock);
    if (!__atomic_load_n(&rec->in_use, __ATOMIC_SEQ_CST) ||
        rec->pub.state >= DANOS_TX_COMMIT) {
        pthread_mutex_unlock(&rec->lock);
        return DANOS_ERR_TX_INVALID;
    }
    rec->pub.state = DANOS_TX_ABORT;
    tx->state = DANOS_TX_ABORT;
    pthread_mutex_unlock(&rec->lock);
    tx_retire(rec, tx);
    return DANOS_OK;
}

danos_status_t danos_tx_rollback(danos_tx_t *tx)
{
    if (!tx) return DANOS_ERR_INVALID_ARG;
    if (tx->state != DANOS_TX_COMMIT && tx->state != DANOS_TX_VERIFY)
        return DANOS_ERR_TX_INVALID;

    tx->state = DANOS_TX_ROLLBACK;
    tx_retire(danos_tx_validate_ptr(tx), tx);
    return DANOS_OK;
}

danos_status_t danos_tx_get_state(const danos_tx_t *tx, danos_tx_state_t *out)
{
    if (!tx || !out) return DANOS_ERR_INVALID_ARG;
    *out = tx->state;
    return DANOS_OK;
}

danos_status_t danos_tx_commit_atomic(danos_tx_t *tx)
{
    danos_status_t st;
    st = danos_tx_prepare(tx);    if (st != DANOS_OK) return st;
    st = danos_tx_validate(tx);   if (st != DANOS_OK) return st;
    st = danos_tx_commit(tx);     if (st != DANOS_OK) return st;
    st = danos_tx_verify(tx);     return st;
}
