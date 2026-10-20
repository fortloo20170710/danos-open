/*
 * DANOS-Open Security: Audit Log Implementation
 *
 * Append-only audit log per §20.9.
 * Format: JSON lines (one entry per line) for easy parsing.
 */

#include <danos/security/audit.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEFAULT_AUDIT_PATH "/var/log/danos/audit.log"
#define MAX_ENTRIES 4096

static FILE *g_log_fp = NULL;
static char g_path[256] = DEFAULT_AUDIT_PATH;
static pthread_mutex_t g_audit_lock = PTHREAD_MUTEX_INITIALIZER;

/* In-memory ring buffer for query */
static danos_audit_entry_t g_ring[MAX_ENTRIES];
static int g_ring_head = 0;
static int g_ring_count = 0;

static const char *event_name(danos_audit_event_t e)
{
    switch (e) {
    case DANOS_AUDIT_TX_BEGIN:    return "tx_begin";
    case DANOS_AUDIT_TX_COMMIT:   return "tx_commit";
    case DANOS_AUDIT_TX_ABORT:    return "tx_abort";
    case DANOS_AUDIT_TX_ROLLBACK: return "tx_rollback";
    case DANOS_AUDIT_AUTH_OK:     return "auth_ok";
    case DANOS_AUDIT_AUTH_FAIL:   return "auth_fail";
    case DANOS_AUDIT_PERMIT:      return "permit";
    case DANOS_AUDIT_DENY:        return "deny";
    case DANOS_AUDIT_KEY_ROTATE:  return "key_rotate";
    default:                      return "unknown";
    }
}

static int audit_init_locked(const char *path)
{
    if (g_log_fp) fclose(g_log_fp);
    if (path) {
        snprintf(g_path, sizeof(g_path), "%s", path);
    }

    /* Audit records are security evidence: never leave them group- or
     * world-readable, regardless of the caller's umask. */
    int fd = open(g_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd >= 0) {
        (void)fchmod(fd, 0600);
        g_log_fp = fdopen(fd, "a");
        if (!g_log_fp) close(fd);
    } else {
        g_log_fp = NULL;
    }
    /* If open fails (e.g. no /var/log), continue with in-memory only */

    g_ring_head = 0;
    g_ring_count = 0;
    return 0;
}

/* Write a JSON string literal. Audit fields carry remote-controlled data
 * (object ids, source addresses, initiators); emitting them raw lets a
 * crafted value inject newlines and forge whole records. */
static void audit_put_string(FILE *fp, const char *s)
{
    fputc('"', fp);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '"':  fputs("\\\"", fp); break;
        case '\\': fputs("\\\\", fp); break;
        case '\b': fputs("\\b", fp); break;
        case '\f': fputs("\\f", fp); break;
        case '\n': fputs("\\n", fp); break;
        case '\r': fputs("\\r", fp); break;
        case '\t': fputs("\\t", fp); break;
        default:
            if (*p < 0x20) fprintf(fp, "\\u%04x", *p);
            else           fputc(*p, fp);
        }
    }
    fputc('"', fp);
}

int danos_audit_init(const char *path)
{
    pthread_mutex_lock(&g_audit_lock);
    int rc = audit_init_locked(path);
    pthread_mutex_unlock(&g_audit_lock);
    return rc;
}

void danos_audit_close(void)
{
    pthread_mutex_lock(&g_audit_lock);
    if (g_log_fp) {
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
    pthread_mutex_unlock(&g_audit_lock);
}

int danos_audit_log(const danos_audit_entry_t *entry)
{
    if (!entry) return -1;
    pthread_mutex_lock(&g_audit_lock);
    if (!g_log_fp && g_ring_count == 0) {
        /* Lazy init if not initialized */
        audit_init_locked(NULL);
    }

    /* Write to in-memory ring buffer */
    g_ring[g_ring_head] = *entry;
    g_ring_head = (g_ring_head + 1) % MAX_ENTRIES;
    if (g_ring_count < MAX_ENTRIES) g_ring_count++;

    /* Write to file (append-only, JSON line) */
    if (g_log_fp) {
        fprintf(g_log_fp, "{\"ts\":%ld.%09ld,\"tx_id\":%lu,\"event\":",
                (long)entry->timestamp.tv_sec, (long)entry->timestamp.tv_nsec,
                (unsigned long)entry->tx_id);
        audit_put_string(g_log_fp, event_name(entry->event));
        fputs(",\"initiator\":", g_log_fp);
        audit_put_string(g_log_fp, entry->initiator);
        fputs(",\"src\":", g_log_fp);
        audit_put_string(g_log_fp, entry->source_ip);
        fputs(",\"obj_type\":", g_log_fp);
        audit_put_string(g_log_fp, entry->obj_type);
        fputs(",\"obj_id\":", g_log_fp);
        audit_put_string(g_log_fp, entry->obj_id);
        fputs(",\"diff\":", g_log_fp);
        audit_put_string(g_log_fp, entry->diff);
        fputs("}\n", g_log_fp);
        fflush(g_log_fp);
    }

    pthread_mutex_unlock(&g_audit_lock);
    return 0;
}

int danos_audit_query(uint64_t tx_id,
                      danos_audit_entry_t *entries, int max_entries)
{
    if (!entries || max_entries <= 0) return 0;
    pthread_mutex_lock(&g_audit_lock);

    int count = 0;
    int start = (g_ring_count < MAX_ENTRIES) ? 0 : g_ring_head;

    for (int i = 0; i < g_ring_count && count < max_entries; i++) {
        int idx = (start + i) % MAX_ENTRIES;
        if (tx_id == 0 || g_ring[idx].tx_id == tx_id) {
            entries[count++] = g_ring[idx];
        }
    }
    pthread_mutex_unlock(&g_audit_lock);
    return count;
}
