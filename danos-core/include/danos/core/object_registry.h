/*
 * DANOS-Open Core: Object Registry (B1)
 *
 * Generic object store keyed by (type, id). Each object type registers
 * create/read/update/delete hooks. The registry is thread-safe (rwlock).
 */

#ifndef DANOS_CORE_OBJECT_REGISTRY_H__
#define DANOS_CORE_OBJECT_REGISTRY_H__

#include <danos/dpa.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Object store holds opaque blobs keyed by (type, id). */
typedef struct danos_object_entry {
    danos_obj_type_t  type;
    danos_obj_id_t    id;
    void             *data;          /* owned by store, malloc'd */
    size_t            data_size;
    struct danos_object_entry *next;  /* hash chain */
} danos_object_entry_t;

typedef struct {
    danos_object_entry_t **buckets;
    size_t                bucket_count;
    pthread_rwlock_t      lock;
    uint64_t              count;     /* total objects */
} danos_object_store_t;

/* Global default store */
extern danos_object_store_t *g_default_store;

/* Create/destroy store */
danos_object_store_t *danos_object_store_create(size_t bucket_count);
void danos_object_store_destroy(danos_object_store_t *store);

/* CRUD: data is copied into the store. Returns OK/EXISTS/NOT_FOUND. */
danos_status_t danos_object_create(danos_object_store_t *store,
                                   danos_obj_type_t type,
                                   danos_obj_id_t id,
                                   const void *data, size_t size);

danos_status_t danos_object_read(danos_object_store_t *store,
                                 danos_obj_type_t type,
                                 danos_obj_id_t id,
                                 void *out, size_t *out_size);

danos_status_t danos_object_update(danos_object_store_t *store,
                                   danos_obj_type_t type,
                                   danos_obj_id_t id,
                                   const void *data, size_t size);

danos_status_t danos_object_delete(danos_object_store_t *store,
                                   danos_obj_type_t type,
                                   danos_obj_id_t id);

/* Count objects of a given type */
uint64_t danos_object_count(danos_object_store_t *store, danos_obj_type_t type);

/* Highest id in use for a type, or 0 when the type has no objects.
 *
 * Lets callers allocate a fresh id without copying every object into a
 * fixed-size array first: a bounded scan silently returns the max of only
 * the first N entries, so the caller can then pick an id that is already
 * taken. This walks the store directly, so it is correct at any size. */
danos_obj_id_t danos_object_max_id(danos_object_store_t *store,
                                   danos_obj_type_t type);

/* ---- batch mutation ---------------------------------------------------- */

/* One staged change. `data` must remain valid for the duration of the
 * apply_batch call; the registry copies it. */
typedef struct {
    danos_obj_type_t type;
    danos_obj_id_t   id;
    void            *data;
    size_t           data_size;
    bool             remove;   /* delete instead of write */
    bool             base_exists; /* optimistic conflict snapshot */
    void            *base_data;
    size_t           base_size;
} danos_object_mutation_t;

/* Return the current payload as a heap copy for transaction conflict checks. */
danos_status_t danos_object_read_copy(danos_object_store_t *store,
                                      danos_obj_type_t type,
                                      danos_obj_id_t id,
                                      void **data, size_t *size);

/* Apply a set of mutations as one atomic step.
 *
 * All the store's write lock is taken once for the whole batch, so a
 * concurrent reader sees either none of the changes or all of them - never a
 * partially applied set. That is the primitive transaction commit needs: the
 * alternative, calling create/update/delete in a loop, takes and releases the
 * lock per object, so a reader (or the reconciler) can observe half of a
 * configuration.
 *
 * Mutations are applied in order. If one fails, the remainder is skipped and
 * the store holds the changes applied so far - callers that need all-or-
 * nothing must pre-validate. Returns the number applied, or a negative
 * status on the first failure (the count applied is lost, so treat a
 * non-zero return as "do not trust the batch"). */
int danos_object_apply_batch(danos_object_store_t *store,
                             const danos_object_mutation_t *muts,
                             size_t n);

/* Transaction commit variant: compare every key to its captured base
 * snapshot and apply all changes atomically under the store write lock. */
int danos_object_apply_transaction_batch(danos_object_store_t *store,
                                        const danos_object_mutation_t *muts,
                                        size_t n, uint64_t tx_id);

/* Iterate all entries (all types) under a read lock. The callback must
 * not modify the store. */
typedef void (*danos_object_iter_cb_t)(danos_object_entry_t *entry, void *user);
void danos_object_iterate(danos_object_store_t *store,
                          danos_object_iter_cb_t cb, void *user);

/* Hash helper */
static inline size_t danos_obj_hash(danos_obj_type_t type, danos_obj_id_t id,
                                    size_t bucket_count)
{
    uint64_t h = (uint64_t)type * 2654435761ULL + id;
    return (size_t)(h % bucket_count);
}

#ifdef __cplusplus
}
#endif

#endif /* DANOS_CORE_OBJECT_REGISTRY_H__ */
