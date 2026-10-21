/*
 * DANOS-Open Management: NETCONF configuration datastores
 *
 * RFC 6241 defines three configuration datastores: running, candidate and
 * startup.  The core object registry owns the running store (g_default_store)
 * and its transaction engine.  This module adds:
 *
 *   - a candidate snapshot taken from running on first use and edited
 *     without touching running until <commit>;
 *   - a startup snapshot loaded from, and written to, a small checksummed
 *     file;
 *   - the RFC 6241 lock table that serialises writers between sessions.
 *
 * Datastores are process-wide.  The SSH subsystem currently hosts one
 * process per NETCONF session, so candidate state and locks are shared
 * inside a process only.  The API is kept independent of the session
 * transport so a future single-process NETCONF daemon can host it directly.
 */

#ifndef DANOS_NETCONF_DATASTORE_H__
#define DANOS_NETCONF_DATASTORE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <danos/core/object_registry.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NETCONF_DS_RUNNING   = 1,
    NETCONF_DS_CANDIDATE = 2,
    NETCONF_DS_STARTUP   = 3,
} netconf_datastore_t;

typedef enum {
    NETCONF_DS_OK = 0,
    NETCONF_DS_NOT_FOUND,
    NETCONF_DS_LOCKED,      /* another session holds the lock */
    NETCONF_DS_INVALID,     /* unknown/unavailable datastore or payload */
    NETCONF_DS_IO,          /* startup file could not be written */
    NETCONF_DS_NOMEM,
    NETCONF_DS_FAILED,      /* transaction or other runtime failure */
    NETCONF_DS_DENIED,      /* session/token may not confirm or cancel */
} netconf_ds_status_t;

/* Initialise the process-wide datastores.  The first call wins; it loads
 * the startup file named by `startup_path`, or by DANOS_NETCONF_STARTUP,
 * or the built-in default.  A missing file is an empty startup datastore. */
void netconf_ds_init(const char *startup_path);

/* Release every datastore and lock.  A later init starts from the file. */
void netconf_ds_fini(void);

/* Datastore path currently in use (never NULL once init has run). */
const char *netconf_ds_startup_path(void);

/* Read one object as a heap copy.  Returns NOT_FOUND when absent. */
netconf_ds_status_t netconf_ds_read(netconf_datastore_t ds, uint16_t type,
                                    uint64_t id, void **data, size_t *size);

/* Iterate every object in a datastore.  The callback must not call back
 * into this module. */
netconf_ds_status_t netconf_ds_iterate(netconf_datastore_t ds,
                                       danos_object_iter_cb_t cb, void *user);

/* Insert or replace one object in the candidate datastore.  Running is
 * written only through netconf_ds_commit/copy so DPA transactions stay the
 * single writer. */
netconf_ds_status_t netconf_ds_write(netconf_datastore_t ds, uint16_t type,
                                     uint64_t id, const void *data,
                                     size_t size);

netconf_ds_status_t netconf_ds_delete(netconf_datastore_t ds, uint16_t type,
                                      uint64_t id);

/* Validate the payloads in a datastore without applying anything. */
netconf_ds_status_t netconf_ds_validate(netconf_datastore_t ds);

/* Options for <commit>.  Plain commits pass NULL or an all-zero struct.
 * A confirmed commit publishes the candidate, keeps a rollback baseline and
 * reverts to it when no confirming commit arrives within timeout_s
 * (default 600).  persist/ persist_id follow RFC 6241 section 8.4. */
typedef struct {
    bool        confirmed;
    uint32_t    timeout_s;
    const char *persist;
    const char *persist_id;
} netconf_commit_opts_t;

/* Publish candidate as running in one transaction and reset the candidate
 * snapshot to the new running state. */
netconf_ds_status_t netconf_ds_commit(uint64_t session_id,
                                      const netconf_commit_opts_t *opts);

/* RFC 6241 cancel-commit: revert a pending confirmed commit.  A persisted
 * commit requires the matching persist_id; otherwise the issuing session
 * must cancel it. */
netconf_ds_status_t netconf_ds_cancel_commit(uint64_t session_id,
                                             const char *persist_id);

/* True while a confirmed commit is waiting for confirmation. */
bool netconf_ds_confirmed_pending(void);

/* Throw away the candidate snapshot.  Fails when another session holds the
 * candidate lock. */
netconf_ds_status_t netconf_ds_discard(uint64_t session_id);

/* Replace `to` with a copy of `from`.  Running is updated atomically. */
netconf_ds_status_t netconf_ds_copy(netconf_datastore_t from,
                                    netconf_datastore_t to,
                                    uint64_t session_id);

/* RFC 6241 delete-config: only the startup datastore can be cleared. */
netconf_ds_status_t netconf_ds_delete_config(netconf_datastore_t ds,
                                             uint64_t session_id);

netconf_ds_status_t netconf_ds_lock(netconf_datastore_t ds,
                                    uint64_t session_id,
                                    const char *identity);

netconf_ds_status_t netconf_ds_unlock(netconf_datastore_t ds,
                                      uint64_t session_id);

/* Drop every lock held by a closing session. */
void netconf_ds_release_session(uint64_t session_id);

/* True when a different session holds the datastore lock. */
bool netconf_ds_locked_by_other(netconf_datastore_t ds, uint64_t session_id);

#ifdef __cplusplus
}
#endif

#endif /* DANOS_NETCONF_DATASTORE_H__ */
