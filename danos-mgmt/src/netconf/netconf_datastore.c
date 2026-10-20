/*
 * DANOS-Open Management: NETCONF configuration datastores (RFC 6241)
 */

#include "netconf_datastore.h"

#include <danos/core/transaction.h>
#include <danos/dpa.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DS_PATH_MAX       4096
#define DS_DEFAULT_PATH   "/var/lib/danos/netconf-startup.db"
#define DS_MAX_OBJECTS    (1u << 20)
#define DS_FILE_MAGIC     "DNCONF1\n"
#define DS_FILE_VERSION   1u
#define DS_HEADER_SIZE    16u
#define DS_RECORD_SIZE    20u

typedef struct {
    bool              initialized;
    char             *path;
    danos_object_store_t *startup;
    bool              startup_valid;   /* false when the file is corrupt */
    danos_object_store_t *candidate;   /* NULL means "equal to running" */

    struct {
        uint64_t session;
        char     identity[64];
    } lock[3];
} ds_state_t;

static ds_state_t g_ds;
static pthread_mutex_t g_ds_lock = PTHREAD_MUTEX_INITIALIZER;

static netconf_ds_status_t status_from_danos(danos_status_t st);
static bool write_allowed_locked(netconf_datastore_t ds, uint64_t session_id);

/* ---- little-endian helpers --------------------------------------------- */

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static void put_u64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)((v >> (8 * i)) & 0xff);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

/* CRC32 (IEEE 802.3), same polynomial as the WAL. */
static uint32_t crc32_calc(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1));
    }
    return ~crc;
}

/* ---- object validation -------------------------------------------------- */

/* Payloads copied from disk or the candidate go back to running through the
 * generic transaction path, which does not run DPA type hooks.  Validate the
 * one modelled type here so a corrupt file cannot publish an iface blob the
 * iface code would overrun. */
static bool payload_valid(uint16_t type, const void *data, size_t size)
{
    if (type == 0 || size == 0 || size > DANOS_MAX_OBJECT_BYTES)
        return false;
    if (type == DANOS_OBJ_IFACE) {
        if (size != sizeof(danos_iface_t)) return false;
        const danos_iface_t *ifc = data;
        if (ifc->name[0] == '\0') return false;
        return memchr(ifc->name, '\0', sizeof(ifc->name)) != NULL;
    }
    return true;
}

/* ---- store helpers ------------------------------------------------------ */

struct clone_ctx {
    danos_object_store_t *dst;
    bool                  failed;
};

static void clone_iter(danos_object_entry_t *e, void *user)
{
    struct clone_ctx *c = user;
    if (c->failed) return;
    if (danos_object_create(c->dst, e->type, e->id, e->data, e->data_size)
        != DANOS_OK)
        c->failed = true;
}

static danos_object_store_t *store_clone(danos_object_store_t *src)
{
    danos_object_store_t *dst = danos_object_store_create(0);
    if (!dst) return NULL;
    if (src) {
        struct clone_ctx c = { .dst = dst, .failed = false };
        danos_object_iterate(src, clone_iter, &c);
        if (c.failed) {
            danos_object_store_destroy(dst);
            return NULL;
        }
    }
    return dst;
}

/* Caller holds g_ds_lock.  Returns NULL with *nomem set on allocation
 * failure, or when the candidate simply has not been initialised. */
static danos_object_store_t *ensure_candidate_locked(bool *nomem)
{
    *nomem = false;
    if (g_ds.candidate) return g_ds.candidate;
    g_ds.candidate = store_clone(g_default_store);
    if (!g_ds.candidate) *nomem = true;
    return g_ds.candidate;
}

static danos_object_store_t *ds_store_locked(netconf_datastore_t ds,
                                             bool *nomem)
{
    *nomem = false;
    switch (ds) {
    case NETCONF_DS_RUNNING:
        return g_default_store;
    case NETCONF_DS_CANDIDATE:
        return ensure_candidate_locked(nomem);
    case NETCONF_DS_STARTUP:
        return g_ds.startup_valid ? g_ds.startup : NULL;
    default:
        return NULL;
    }
}

/* ---- startup file ------------------------------------------------------- */

static int write_all_fd(int fd, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

struct save_ctx {
    int  fd;
    int  rc;
};

static void save_count_iter(danos_object_entry_t *e, void *user)
{
    (void)e;
    (*(uint64_t *)user)++;
}

static void save_write_iter(danos_object_entry_t *e, void *user)
{
    struct save_ctx *s = user;
    if (s->rc != 0) return;

    uint8_t rec[DS_RECORD_SIZE];
    put_u16(rec + 0, (uint16_t)e->type);
    put_u16(rec + 2, 0);
    put_u64(rec + 4, e->id);
    put_u32(rec + 12, (uint32_t)e->data_size);
    put_u32(rec + 16, crc32_calc(e->data, e->data_size));

    s->rc = write_all_fd(s->fd, rec, sizeof(rec));
    if (s->rc == 0)
        s->rc = write_all_fd(s->fd, e->data, e->data_size);
}

static void fsync_parent_dir(const char *path)
{
    char dir[DS_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash) return;
    if (slash == dir) slash[1] = '\0';
    else *slash = '\0';

    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return;
    (void)fsync(fd);
    close(fd);
}

/* Write a versioned, per-record checksummed snapshot.  The caller holds
 * g_ds_lock so two writers cannot race on the temporary file or rename. */
static int startup_save(danos_object_store_t *store)
{
    if (!g_ds.path || !*g_ds.path) return -1;

    uint64_t count = 0;
    danos_object_iterate(store, save_count_iter, &count);
    if (count > DS_MAX_OBJECTS) return -1;

    uint8_t header[DS_HEADER_SIZE];
    memcpy(header, DS_FILE_MAGIC, 8);
    put_u32(header + 8, DS_FILE_VERSION);
    put_u32(header + 12, (uint32_t)count);

    char tmp[DS_PATH_MAX];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", g_ds.path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return -1;

    int fd = mkstemp(tmp);
    if (fd < 0) return -1;
    (void)fchmod(fd, 0600);

    int rc = write_all_fd(fd, header, sizeof(header));
    if (rc == 0) {
        struct save_ctx s = { .fd = fd, .rc = 0 };
        danos_object_iterate(store, save_write_iter, &s);
        rc = s.rc;
    }

    if (rc == 0 && fsync(fd) != 0) rc = -1;
    if (close(fd) != 0) rc = -1;

    if (rc == 0 && rename(tmp, g_ds.path) != 0) rc = -1;
    if (rc != 0) {
        unlink(tmp);
        return -1;
    }
    fsync_parent_dir(g_ds.path);
    return 0;
}

/* Returns 0 when the file is absent or loaded, -1 when it is corrupt. */
static int startup_load(void)
{
    g_ds.startup = danos_object_store_create(0);
    if (!g_ds.startup) return -1;

    FILE *f = fopen(g_ds.path, "rb");
    if (!f) return errno == ENOENT ? 0 : -1;

    uint8_t header[DS_HEADER_SIZE];
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        return -1;
    }
    if (memcmp(header, DS_FILE_MAGIC, 8) != 0 ||
        get_u32(header + 8) != DS_FILE_VERSION) {
        fclose(f);
        return -1;
    }
    uint32_t count = get_u32(header + 12);
    if (count > DS_MAX_OBJECTS) {
        fclose(f);
        return -1;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint8_t rec[DS_RECORD_SIZE];
        if (fread(rec, 1, sizeof(rec), f) != sizeof(rec)) {
            fclose(f);
            return -1;
        }
        uint16_t type = get_u16(rec + 0);
        uint64_t id = get_u64(rec + 4);
        uint32_t size = get_u32(rec + 12);
        uint32_t crc = get_u32(rec + 16);
        if (size == 0 || size > DANOS_MAX_OBJECT_BYTES) {
            fclose(f);
            return -1;
        }
        uint8_t *data = malloc(size);
        if (!data) {
            fclose(f);
            return -1;
        }
        if (fread(data, 1, size, f) != size ||
            crc32_calc(data, size) != crc ||
            !payload_valid(type, data, size)) {
            free(data);
            fclose(f);
            return -1;
        }
        danos_status_t st = danos_object_create(g_ds.startup,
                                                (danos_obj_type_t)type,
                                                id, data, size);
        free(data);
        if (st != DANOS_OK) {
            fclose(f);
            return -1;
        }
    }

    /* Trailing bytes mean the file was not written by this version. */
    if (fgetc(f) != EOF) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

/* ---- lifecycle ---------------------------------------------------------- */

void netconf_ds_init(const char *startup_path)
{
    pthread_mutex_lock(&g_ds_lock);
    if (g_ds.initialized) {
        pthread_mutex_unlock(&g_ds_lock);
        return;
    }

    memset(&g_ds, 0, sizeof(g_ds));
    const char *path = startup_path;
    if (!path || !*path) path = getenv("DANOS_NETCONF_STARTUP");
    if (!path || !*path) path = DS_DEFAULT_PATH;

    g_ds.path = strdup(path);
    if (!g_ds.path) {
        pthread_mutex_unlock(&g_ds_lock);
        return;
    }

    /* The subsystem process hosts NETCONF without mgrd, so the running
     * store may not exist yet.  Create it lazily like the gNMI server. */
    if (!g_default_store)
        g_default_store = danos_object_store_create(1024);
    if (!g_default_store) {
        free(g_ds.path);
        g_ds.path = NULL;
        pthread_mutex_unlock(&g_ds_lock);
        return;
    }

    g_ds.startup_valid = startup_load() == 0;
    g_ds.initialized = true;
    pthread_mutex_unlock(&g_ds_lock);
}

void netconf_ds_fini(void)
{
    pthread_mutex_lock(&g_ds_lock);
    danos_object_store_destroy(g_ds.startup);
    danos_object_store_destroy(g_ds.candidate);
    free(g_ds.path);
    memset(&g_ds, 0, sizeof(g_ds));
    pthread_mutex_unlock(&g_ds_lock);
}

const char *netconf_ds_startup_path(void)
{
    return g_ds.path;
}

/* ---- reads -------------------------------------------------------------- */

/* Caller holds g_ds_lock. */
static netconf_ds_status_t read_locked(netconf_datastore_t ds, uint16_t type,
                                       uint64_t id, void **data, size_t *size)
{
    if (!data || !size) return NETCONF_DS_INVALID;
    bool nomem;
    danos_object_store_t *store = ds_store_locked(ds, &nomem);
    if (!store) return nomem ? NETCONF_DS_NOMEM : NETCONF_DS_INVALID;

    danos_status_t st = danos_object_read_copy(store, (danos_obj_type_t)type,
                                               id, data, size);
    if (st == DANOS_OK) return NETCONF_DS_OK;
    if (st == DANOS_ERR_NOT_FOUND) return NETCONF_DS_NOT_FOUND;
    if (st == DANOS_ERR_NO_MEMORY) return NETCONF_DS_NOMEM;
    return NETCONF_DS_INVALID;
}

netconf_ds_status_t netconf_ds_read(netconf_datastore_t ds, uint16_t type,
                                    uint64_t id, void **data, size_t *size)
{
    pthread_mutex_lock(&g_ds_lock);
    netconf_ds_status_t st = read_locked(ds, type, id, data, size);
    pthread_mutex_unlock(&g_ds_lock);
    return st;
}

netconf_ds_status_t netconf_ds_iterate(netconf_datastore_t ds,
                                       danos_object_iter_cb_t cb, void *user)
{
    bool nomem;
    pthread_mutex_lock(&g_ds_lock);
    danos_object_store_t *store = ds_store_locked(ds, &nomem);
    netconf_ds_status_t rc = NETCONF_DS_OK;
    if (!store) rc = nomem ? NETCONF_DS_NOMEM : NETCONF_DS_INVALID;
    else danos_object_iterate(store, cb, user);
    pthread_mutex_unlock(&g_ds_lock);
    return rc;
}

netconf_ds_status_t netconf_ds_write(netconf_datastore_t ds, uint16_t type,
                                     uint64_t id, const void *data,
                                     size_t size)
{
    if (ds != NETCONF_DS_CANDIDATE || !payload_valid(type, data, size))
        return NETCONF_DS_INVALID;

    bool nomem;
    pthread_mutex_lock(&g_ds_lock);
    danos_object_store_t *store = ensure_candidate_locked(&nomem);
    netconf_ds_status_t rc;
    if (!store) {
        rc = nomem ? NETCONF_DS_NOMEM : NETCONF_DS_INVALID;
    } else {
        danos_status_t st = danos_object_update(store, (danos_obj_type_t)type,
                                                id, data, size);
        if (st == DANOS_ERR_NOT_FOUND)
            st = danos_object_create(store, (danos_obj_type_t)type, id, data,
                                     size);
        rc = status_from_danos(st);
    }
    pthread_mutex_unlock(&g_ds_lock);
    return rc;
}

netconf_ds_status_t netconf_ds_delete(netconf_datastore_t ds, uint16_t type,
                                      uint64_t id)
{
    if (ds != NETCONF_DS_CANDIDATE) return NETCONF_DS_INVALID;

    bool nomem;
    pthread_mutex_lock(&g_ds_lock);
    danos_object_store_t *store = ensure_candidate_locked(&nomem);
    netconf_ds_status_t rc;
    if (!store) {
        rc = nomem ? NETCONF_DS_NOMEM : NETCONF_DS_INVALID;
    } else if (danos_object_delete(store, (danos_obj_type_t)type, id)
               == DANOS_OK) {
        rc = NETCONF_DS_OK;
    } else {
        rc = NETCONF_DS_NOT_FOUND;
    }
    pthread_mutex_unlock(&g_ds_lock);
    return rc;
}

/* ---- publish a snapshot to running -------------------------------------- */

struct ds_ref {
    uint16_t type;
    uint64_t id;
};

struct ref_list {
    struct ds_ref *v;
    size_t         n;
    size_t         cap;
    bool           oom;
};

static void collect_refs_iter(danos_object_entry_t *e, void *user)
{
    struct ref_list *r = user;
    if (r->oom) return;
    if (r->n == r->cap) {
        size_t cap = r->cap ? r->cap * 2 : 32;
        struct ds_ref *v = realloc(r->v, cap * sizeof(*v));
        if (!v) { r->oom = true; return; }
        r->v = v;
        r->cap = cap;
    }
    r->v[r->n].type = (uint16_t)e->type;
    r->v[r->n].id = e->id;
    r->n++;
}

static void ref_list_free(struct ref_list *r)
{
    free(r->v);
    r->v = NULL;
    r->n = r->cap = 0;
}

static bool refs_contain(const struct ref_list *r, uint16_t type, uint64_t id)
{
    for (size_t i = 0; i < r->n; i++)
        if (r->v[i].type == type && r->v[i].id == id) return true;
    return false;
}

static netconf_ds_status_t status_from_danos(danos_status_t st)
{
    if (st == DANOS_OK) return NETCONF_DS_OK;
    if (st == DANOS_ERR_NO_MEMORY) return NETCONF_DS_NOMEM;
    if (st == DANOS_ERR_NOT_FOUND) return NETCONF_DS_NOT_FOUND;
    return NETCONF_DS_FAILED;
}

/* Caller holds g_ds_lock.  Stages the difference between `snapshot` and
 * running into a single transaction and publishes it atomically. */
static netconf_ds_status_t apply_snapshot_locked(danos_object_store_t *snapshot,
                                                 uint64_t session_id)
{
    if (!snapshot || !g_default_store) return NETCONF_DS_INVALID;
    if (!write_allowed_locked(NETCONF_DS_RUNNING, session_id))
        return NETCONF_DS_LOCKED;

    struct ref_list running = {0}, wanted = {0};
    danos_object_iterate(g_default_store, collect_refs_iter, &running);
    danos_object_iterate(snapshot, collect_refs_iter, &wanted);
    if (running.oom || wanted.oom) {
        ref_list_free(&running);
        ref_list_free(&wanted);
        return NETCONF_DS_NOMEM;
    }

    danos_tx_t tx = {0};
    danos_status_t st = danos_tx_begin(&tx, "netconf-commit", NULL);
    if (st != DANOS_OK) {
        ref_list_free(&running);
        ref_list_free(&wanted);
        return status_from_danos(st);
    }

    /* Objects present in the new configuration: create or update. */
    for (size_t i = 0; st == DANOS_OK && i < wanted.n; i++) {
        void *data = NULL, *base = NULL;
        size_t size = 0, base_size = 0;
        st = danos_object_read_copy(snapshot,
                                    (danos_obj_type_t)wanted.v[i].type,
                                    wanted.v[i].id, &data, &size);
        if (st != DANOS_OK) break;

        danos_status_t rs = danos_object_read_copy(g_default_store,
                                    (danos_obj_type_t)wanted.v[i].type,
                                    wanted.v[i].id, &base, &base_size);
        if (rs == DANOS_ERR_NOT_FOUND) {
            st = danos_tx_stage_object(&tx,
                                       (danos_obj_type_t)wanted.v[i].type,
                                       wanted.v[i].id, data, size,
                                       DANOS_TX_OP_CREATE);
        } else if (rs == DANOS_OK) {
            if (size != base_size || memcmp(data, base, size) != 0)
                st = danos_tx_stage_object(&tx,
                                           (danos_obj_type_t)wanted.v[i].type,
                                           wanted.v[i].id, data, size,
                                           DANOS_TX_OP_UPDATE);
        } else {
            st = rs;
        }
        free(base);
        free(data);
    }

    /* Objects only in running: remove. */
    for (size_t i = 0; st == DANOS_OK && i < running.n; i++) {
        if (refs_contain(&wanted, running.v[i].type, running.v[i].id))
            continue;
        st = danos_tx_stage_object(&tx, (danos_obj_type_t)running.v[i].type,
                                   running.v[i].id, NULL, 0,
                                   DANOS_TX_OP_DELETE);
    }

    if (st == DANOS_OK) st = danos_tx_commit_atomic(&tx);
    if (st != DANOS_OK && tx._internal) (void)danos_tx_abort(&tx);

    ref_list_free(&running);
    ref_list_free(&wanted);
    return status_from_danos(st);
}

/* ---- locks -------------------------------------------------------------- */

static bool ds_index(netconf_datastore_t ds, size_t *idx)
{
    if (ds < NETCONF_DS_RUNNING || ds > NETCONF_DS_STARTUP) return false;
    *idx = (size_t)ds - 1;
    return true;
}

netconf_ds_status_t netconf_ds_lock(netconf_datastore_t ds,
                                    uint64_t session_id,
                                    const char *identity)
{
    size_t idx;
    if (!ds_index(ds, &idx) || session_id == 0) return NETCONF_DS_INVALID;

    pthread_mutex_lock(&g_ds_lock);
    netconf_ds_status_t rc = NETCONF_DS_OK;
    if (ds == NETCONF_DS_STARTUP && !g_ds.startup_valid) {
        rc = NETCONF_DS_INVALID;
    } else if (g_ds.lock[idx].session != 0) {
        rc = NETCONF_DS_LOCKED;
    } else {
        g_ds.lock[idx].session = session_id;
        snprintf(g_ds.lock[idx].identity, sizeof(g_ds.lock[idx].identity),
                 "%s", identity ? identity : "");
    }
    pthread_mutex_unlock(&g_ds_lock);
    return rc;
}

netconf_ds_status_t netconf_ds_unlock(netconf_datastore_t ds,
                                      uint64_t session_id)
{
    size_t idx;
    if (!ds_index(ds, &idx)) return NETCONF_DS_INVALID;

    pthread_mutex_lock(&g_ds_lock);
    netconf_ds_status_t rc;
    if (session_id == 0 || g_ds.lock[idx].session != session_id)
        rc = NETCONF_DS_LOCKED;
    else {
        g_ds.lock[idx].session = 0;
        g_ds.lock[idx].identity[0] = '\0';
        rc = NETCONF_DS_OK;
    }
    pthread_mutex_unlock(&g_ds_lock);
    return rc;
}

void netconf_ds_release_session(uint64_t session_id)
{
    if (session_id == 0) return;
    pthread_mutex_lock(&g_ds_lock);
    for (size_t i = 0; i < 3; i++) {
        if (g_ds.lock[i].session == session_id) {
            g_ds.lock[i].session = 0;
            g_ds.lock[i].identity[0] = '\0';
        }
    }
    pthread_mutex_unlock(&g_ds_lock);
}

bool netconf_ds_locked_by_other(netconf_datastore_t ds, uint64_t session_id)
{
    size_t idx;
    if (!ds_index(ds, &idx)) return false;
    pthread_mutex_lock(&g_ds_lock);
    bool other = g_ds.lock[idx].session != 0 &&
                 g_ds.lock[idx].session != session_id;
    pthread_mutex_unlock(&g_ds_lock);
    return other;
}

static bool write_allowed_locked(netconf_datastore_t ds, uint64_t session_id)
{
    size_t idx;
    if (!ds_index(ds, &idx)) return false;
    uint64_t owner = g_ds.lock[idx].session;
    return owner == 0 || owner == session_id;
}

/* ---- commit / discard / copy -------------------------------------------- */

netconf_ds_status_t netconf_ds_commit(uint64_t session_id)
{
    pthread_mutex_lock(&g_ds_lock);
    netconf_ds_status_t rc = NETCONF_DS_OK;

    if (!write_allowed_locked(NETCONF_DS_RUNNING, session_id) ||
        !write_allowed_locked(NETCONF_DS_CANDIDATE, session_id)) {
        rc = NETCONF_DS_LOCKED;
    } else if (g_ds.candidate) {
        rc = apply_snapshot_locked(g_ds.candidate, session_id);
        if (rc == NETCONF_DS_OK) {
            /* The candidate now equals running; drop the snapshot and let
             * the next edit take a fresh one. */
            danos_object_store_destroy(g_ds.candidate);
            g_ds.candidate = NULL;
        }
    }
    pthread_mutex_unlock(&g_ds_lock);
    return rc;
}

netconf_ds_status_t netconf_ds_discard(uint64_t session_id)
{
    pthread_mutex_lock(&g_ds_lock);
    netconf_ds_status_t rc = NETCONF_DS_OK;
    if (!write_allowed_locked(NETCONF_DS_CANDIDATE, session_id)) {
        rc = NETCONF_DS_LOCKED;
    } else {
        danos_object_store_destroy(g_ds.candidate);
        g_ds.candidate = NULL;
    }
    pthread_mutex_unlock(&g_ds_lock);
    return rc;
}

netconf_ds_status_t netconf_ds_copy(netconf_datastore_t from,
                                    netconf_datastore_t to,
                                    uint64_t session_id)
{
    if (from < NETCONF_DS_RUNNING || from > NETCONF_DS_STARTUP ||
        to < NETCONF_DS_RUNNING || to > NETCONF_DS_STARTUP)
        return NETCONF_DS_INVALID;
    if (from == to) return NETCONF_DS_OK;

    pthread_mutex_lock(&g_ds_lock);
    netconf_ds_status_t rc = NETCONF_DS_OK;
    bool nomem = false;

    danos_object_store_t *src = ds_store_locked(from, &nomem);
    if (!src) {
        rc = nomem ? NETCONF_DS_NOMEM : NETCONF_DS_INVALID;
    } else if (!write_allowed_locked(to, session_id)) {
        rc = NETCONF_DS_LOCKED;
    } else if (to == NETCONF_DS_RUNNING) {
        rc = apply_snapshot_locked(src, session_id);
    } else if (to == NETCONF_DS_CANDIDATE) {
        danos_object_store_t *copy = store_clone(src);
        if (!copy) rc = NETCONF_DS_NOMEM;
        else {
            danos_object_store_destroy(g_ds.candidate);
            g_ds.candidate = copy;
        }
    } else {                    /* to == startup */
        danos_object_store_t *copy = store_clone(src);
        if (!copy) {
            rc = NETCONF_DS_NOMEM;
        } else if (startup_save(copy) != 0) {
            danos_object_store_destroy(copy);
            rc = NETCONF_DS_IO;
        } else {
            danos_object_store_destroy(g_ds.startup);
            g_ds.startup = copy;
            g_ds.startup_valid = true;
        }
    }
    pthread_mutex_unlock(&g_ds_lock);
    return rc;
}

netconf_ds_status_t netconf_ds_delete_config(netconf_datastore_t ds,
                                             uint64_t session_id)
{
    if (ds != NETCONF_DS_STARTUP) return NETCONF_DS_INVALID;

    pthread_mutex_lock(&g_ds_lock);
    netconf_ds_status_t rc = NETCONF_DS_OK;
    if (!write_allowed_locked(NETCONF_DS_STARTUP, session_id)) {
        rc = NETCONF_DS_LOCKED;
    } else {
        danos_object_store_t *empty = danos_object_store_create(0);
        if (!empty) {
            rc = NETCONF_DS_NOMEM;
        } else if (startup_save(empty) != 0) {
            danos_object_store_destroy(empty);
            rc = NETCONF_DS_IO;
        } else {
            danos_object_store_destroy(g_ds.startup);
            g_ds.startup = empty;
            g_ds.startup_valid = true;
        }
    }
    pthread_mutex_unlock(&g_ds_lock);
    return rc;
}
