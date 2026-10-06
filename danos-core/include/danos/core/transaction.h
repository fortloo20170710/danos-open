/*
 * DANOS-Open Core: Transaction Engine (B3, B4, B5)
 *
 * State machine: OPEN → PREPARE → VALIDATE → COMMIT → VERIFY → DONE
 * Concurrency:   multi-reader, single-writer (global write mutex)
 * Timeout:       per-phase deadlines
 */

#ifndef DANOS_CORE_TRANSACTION_H__
#define DANOS_CORE_TRANSACTION_H__

#include <danos/dpa.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <danos/core/object_registry.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Transaction record (internal, extends public danos_tx_t) */
typedef struct danos_tx_record {
    danos_tx_t           pub;        /* public view */
    pthread_mutex_t     lock;       /* per-tx lock */
    bool                 in_use;
    bool                 reclaiming;
    uint64_t             seq;        /* allocation order, for expiry reclaim */
    struct danos_tx_record *next;   /* chain in pool */
    struct danos_tx_record *live_next; /* chain of every live record */
    danos_object_mutation_t *staged;
    size_t staged_count;
    size_t staged_capacity;
} danos_tx_record_t;

typedef enum {
    DANOS_TX_OP_CREATE = 1,
    DANOS_TX_OP_UPDATE = 2,
    DANOS_TX_OP_DELETE = 3,
} danos_tx_operation_t;

/* Transaction manager */
typedef struct {
    pthread_mutex_t     write_lock;  /* single-writer mutex */
    pthread_mutex_t     pool_lock;   /* protects tx pool and live list */
    danos_tx_record_t  *free_list;   /* recycled tx records */
    danos_tx_record_t  *live_list;   /* every allocated record, live or not */
    uint64_t            next_tx_id;  /* monotonic ID counter */
    uint64_t            next_seq;    /* monotonic allocation counter */
    uint64_t            active_count;
} danos_tx_manager_t;

/* Global transaction manager */
extern danos_tx_manager_t *g_tx_mgr;

/* Init/fini */
int danos_tx_manager_init(void);
void danos_tx_manager_fini(void);

/* Internal: allocate/free tx record */
danos_tx_record_t *danos_tx_record_alloc(void);
void danos_tx_record_free(danos_tx_record_t *rec);

/* Number of transactions begun but not yet retired (begun minus
 * verify/abort/rollback/commit/release). Exposed for leak assertions. */
uint64_t danos_tx_active_count(void);

/* Check if tx is in expected state */
bool danos_tx_in_state(danos_tx_record_t *rec, danos_tx_state_t expected);

/* Validate tx pointer */
danos_tx_record_t *danos_tx_validate_ptr(danos_tx_t *tx);

/* Private DPA object overlay helpers. Writes are accepted only while OPEN. */
danos_status_t danos_tx_stage_object(danos_tx_t *tx, danos_obj_type_t type,
                                     danos_obj_id_t id, const void *data,
                                     size_t size,
                                     danos_tx_operation_t operation);
danos_status_t danos_tx_read_staged(danos_tx_t *tx, danos_obj_type_t type,
                                    danos_obj_id_t id, void *out, size_t *size,
                                    bool *handled);
/* Iterate a private snapshot of one type, overlaying the candidate. The
 * callback runs without store/transaction locks and may stage changes. */
danos_status_t danos_tx_iterate_objects(danos_tx_t *tx, danos_obj_type_t type,
                                       danos_object_iter_cb_t cb, void *user);

#ifdef __cplusplus
}
#endif

#endif /* DANOS_CORE_TRANSACTION_H__ */
