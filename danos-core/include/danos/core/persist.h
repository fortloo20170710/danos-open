/*
 * DANOS-Open Core: Configuration Persistence (v0.3)
 *
 * Durable configuration on top of the WAL (B6):
 *   - Every successful DPA object mutation (create/update/delete) is
 *     appended to the WAL and synced.
 *   - On boot, danos_persist_recover() replays the WAL into the DPA
 *     default object store, restoring the last durable configuration.
 *   - The reconciler then compares desired (recovered) vs backend
 *     (PROGRAMMED) state and re-programs the dataplane.
 *
 * Record flow:
 *   danos_iface_create() → object store → WAL append + fsync
 *   boot: danos_persist_recover() → WAL replay → object store
 *         → danos_reconcile_run() → backend re-program
 */

#ifndef DANOS_PERSIST_H__
#define DANOS_PERSIST_H__

#include <stdbool.h>
#include <danos/core/object_registry.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Enable persistence with the given WAL file path. Subsequent DPA
 * object mutations are logged. Returns 0 on success. */
int danos_persist_enable(const char *wal_path);

/* Disable persistence (WAL is closed and flushed). */
void danos_persist_disable(void);

bool danos_persist_is_enabled(void);

/* Log one mutation (called by the DPA object CRUD layer after the
 * in-memory store is updated). Returns 0 on success. */
int danos_persist_log_op(uint8_t wal_op, uint16_t wal_obj_type,
                         uint64_t obj_id, const void *data, uint32_t len);

/* Mutation hook called by the object registry. No-op unless persistence
 * is enabled, the store is the DPA default store, and recovery is not
 * running. wal_op is a wal_op_type_t value. */
#include <danos/core/object_registry.h>

void danos_persist_on_mutation(danos_object_store_t *store,
                               uint8_t wal_op, int dpa_obj_type,
                               uint64_t obj_id,
                               const void *data, size_t len);

/* Durably append one complete DPA transaction before publishing it in the
 * object store. Returns 0 when persistence is disabled or commit is durable. */
int danos_persist_log_transaction(uint64_t tx_id,
                                  const danos_object_mutation_t *muts,
                                  size_t count);

/* Replay the WAL into the DPA default object store. Returns the number
 * of applied records, or -1 when the log is unusable (allocation failure,
 * callback failure, or a corrupt record). Records before the damage are
 * still applied; use danos_persist_recovery_corrupt() to distinguish a
 * refusing boot from a partial one. Torn records at the tail, the normal
 * signature of a crash during a write, are discarded without error. */
int danos_persist_recover(void);

/* True when the last recovery stopped on a damaged or unreadable record.
 * Callers that require a complete configuration (mgrd) must refuse to
 * boot unless an explicit override is set. */
bool danos_persist_recovery_corrupt(void);

/* Checkpoint: rewrite the WAL as a compact snapshot of the current store
 * contents (drops the history of deleted objects). Must be called while
 * no transaction is in flight; the store lock serializes it against
 * mutations, and the WAL replacement is atomic. */
int danos_persist_checkpoint(void);

/* Stats */
void danos_persist_get_stats(uint64_t *records_logged, uint64_t *recovered);

#ifdef __cplusplus
}
#endif

#endif /* DANOS_PERSIST_H__ */
