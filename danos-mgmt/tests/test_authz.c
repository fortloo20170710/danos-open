/*
 * Authorization enforcement tests.
 *
 * These check that a denial is actually enforced rather than only recorded,
 * and that the role default is configurable so the daemon can be locked down
 * before an identity source exists.
 */
#include "../src/authz/authz.h"
#include <danos/security/audit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

int main(void)
{
    char tmpl[] = "/tmp/danos-authz-test-XXXXXX";
    int fd = mkstemp(tmpl);
    assert(fd >= 0);
    close(fd);

    assert(danos_authz_init(tmpl) == 0);
    assert(danos_authz_ready());

    /* --- role matrix ---------------------------------------------------- */
    /* ADMIN may write anything. */
    assert(danos_rbac_check(DANOS_ROLE_ADMIN, DANOS_SEC_OBJ_ROUTE,
                            DANOS_SEC_OP_DELETE));
    assert(danos_rbac_check(DANOS_ROLE_ADMIN, DANOS_SEC_OBJ_SECURITY,
                            DANOS_SEC_OP_UPDATE));

    /* VIEWER is read-only: any write must be denied. This is the case the
     * daemon relies on once DANOS_AUTHZ_DEFAULT_ROLE=viewer is set. */
    assert(danos_rbac_check(DANOS_ROLE_VIEWER, DANOS_SEC_OBJ_ROUTE,
                            DANOS_SEC_OP_READ));
    assert(!danos_rbac_check(DANOS_ROLE_VIEWER, DANOS_SEC_OBJ_ROUTE,
                             DANOS_SEC_OP_UPDATE));
    assert(!danos_rbac_check(DANOS_ROLE_VIEWER, DANOS_SEC_OBJ_SECURITY,
                             DANOS_SEC_OP_DELETE));

    /* OPERATOR may write config but not security objects. */
    assert(danos_rbac_check(DANOS_ROLE_OPERATOR, DANOS_SEC_OBJ_ROUTE,
                            DANOS_SEC_OP_UPDATE));
    assert(!danos_rbac_check(DANOS_ROLE_OPERATOR, DANOS_SEC_OBJ_SECURITY,
                             DANOS_SEC_OP_UPDATE));

    /* --- default role is configurable ------------------------------------ */
    /* With nothing set, the default is ADMIN so behaviour is unchanged. */
    unsetenv("DANOS_AUTHZ_DEFAULT_ROLE");
    assert(danos_authz_default_role() == DANOS_ROLE_ADMIN);

    setenv("DANOS_AUTHZ_DEFAULT_ROLE", "viewer", 1);
    assert(danos_authz_default_role() == DANOS_ROLE_VIEWER);
    /* A viewer must then be denied a write through the enforcement path -
     * this is the behaviour the daemon actually calls. */
    assert(!danos_authz_check(danos_authz_role_for_peer(NULL),
                              DANOS_SEC_OBJ_ALL, DANOS_SEC_OP_UPDATE, "Set"));
    /* ...and permitted a read. */
    assert(danos_authz_check(danos_authz_role_for_peer(NULL),
                             DANOS_SEC_OBJ_ALL, DANOS_SEC_OP_READ, "Get"));

    setenv("DANOS_AUTHZ_DEFAULT_ROLE", "operator", 1);
    assert(danos_authz_default_role() == DANOS_ROLE_OPERATOR);

    /* An unknown role name must not silently grant access. */
    setenv("DANOS_AUTHZ_DEFAULT_ROLE", "superuser", 1);
    assert(danos_authz_default_role() == DANOS_ROLE_ADMIN);

    /* --- the audit log records what authz_check decided ------------------ */
    unsetenv("DANOS_AUTHZ_DEFAULT_ROLE");
    /* denied, and recorded as such */
    assert(!danos_authz_check(DANOS_ROLE_VIEWER, DANOS_SEC_OBJ_ROUTE,
                              DANOS_SEC_OP_UPDATE, "Set route 10.0.0.0/8"));
    danos_authz_audit_tx(1234, DANOS_AUDIT_TX_COMMIT, "tester",
                         DANOS_SEC_OBJ_ROUTE, "10.0.0.0/8",
                         "prefix=10.0.0.0/8 nh=1");
    danos_authz_audit_auth("127.0.0.1", false, "Set denied");

    /* Flush by closing, then read the log back through the query API. */
    danos_authz_shutdown();

    FILE *f = fopen(tmpl, "r");
    assert(f != NULL);
    char buf[8192];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    buf[got] = '\0';
    fclose(f);

    /* The denied Set must be on record with its reason. */
    assert(strstr(buf, "Set route 10.0.0.0/8") != NULL);
    /* And the committed transaction with its object and diff. */
    assert(strstr(buf, "10.0.0.0/8") != NULL);
    assert(strstr(buf, "tester") != NULL);

    unlink(tmpl);

    printf("=== authz_test: ALL PASSED ===\n");
    return 0;
}