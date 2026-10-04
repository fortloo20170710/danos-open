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
#include <sys/stat.h>

int main(void)
{
    danos_sec_role_t role = DANOS_ROLE_ADMIN;
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

    /* --- authentication ---------------------------------------------------- */
    /* With no credential source configured, authentication is disabled and
     * the daemon is open (pre-existing behaviour). */
    assert(danos_authz_configure(NULL, false) == 0);
    assert(!danos_authz_configured());
    assert(danos_authz_authenticate(NULL, -1, -1, &role) ==
           DANOS_AUTH_DISABLED);

    /* A token file that is group/other readable must be refused: a token any
     * local user can read is not a credential, and silently accepting one
     * would be worse than not starting. */
    char loose[] = "/tmp/danos-authz-loose-XXXXXX";
    int lfd = mkstemp(loose);
    assert(lfd >= 0);
    assert(write(lfd, "s3cret-token\n", 12) == 12);
    close(lfd);
    assert(chmod(loose, 0644) == 0);
    assert(danos_authz_configure(loose, false) == -1);
    assert(!danos_authz_configured());   /* not silently enabled */
    unlink(loose);

    /* A properly restricted token file is accepted and enforced. */
    char tf[] = "/tmp/danos-authz-tok-XXXXXX";
    int tfd = mkstemp(tf);
    assert(tfd >= 0);
    assert(write(tfd, "s3cret-token\n", 12) == 12);
    close(tfd);
    assert(chmod(tf, 0600) == 0);
    assert(danos_authz_configure(tf, false) == 0);
    assert(danos_authz_configured());

    /* Missing credential is rejected, not treated as anonymous-ok. */
    assert(danos_authz_authenticate(NULL, -1, -1, &role) == DANOS_AUTH_NONE);
    /* Wrong token rejected. */
    assert(danos_authz_authenticate("Bearer wrong", -1, -1, &role) ==
           DANOS_AUTH_BAD);
    assert(danos_authz_authenticate("Basic s3cret-token", -1, -1, &role) ==
           DANOS_AUTH_BAD);
    /* A correct token authenticates, and takes the configured role. */
    setenv("DANOS_AUTHZ_DEFAULT_ROLE", "operator", 1);
    assert(danos_authz_authenticate("Bearer s3cret-token", -1, -1, &role) ==
           DANOS_AUTH_OK);
    assert(role == DANOS_ROLE_OPERATOR);
    /* The scheme is case-insensitive per RFC 7235. */
    assert(danos_authz_authenticate("bearer s3cret-token", -1, -1, &role) ==
           DANOS_AUTH_OK);
    /* A prefix of the token must not authenticate. */
    assert(danos_authz_authenticate("Bearer s3cret-toke", -1, -1, &role) ==
           DANOS_AUTH_BAD);
    /* Nor the token with trailing junk. */
    assert(danos_authz_authenticate("Bearer s3cret-tokenX", -1, -1, &role) ==
           DANOS_AUTH_BAD);
    unsetenv("DANOS_AUTHZ_DEFAULT_ROLE");

    /* Peer credentials: uid 0 is admin, any other uid is the default role. */
    assert(danos_authz_configure(NULL, true) == 0);
    assert(danos_authz_configured());
    assert(danos_authz_authenticate(NULL, 0, 42, &role) == DANOS_AUTH_OK);
    assert(role == DANOS_ROLE_ADMIN);
    setenv("DANOS_AUTHZ_DEFAULT_ROLE", "viewer", 1);
    assert(danos_authz_authenticate(NULL, 1000, 42, &role) == DANOS_AUTH_OK);
    assert(role == DANOS_ROLE_VIEWER);
    /* And with a token source present, peercred alone is not enough. */
    assert(danos_authz_configure(tf, true) == 0);
    unsetenv("DANOS_AUTHZ_DEFAULT_ROLE");
    assert(danos_authz_configure(NULL, false) == 0);
    unlink(tf);

    printf("=== authz_test: ALL PASSED ===\n");
    return 0;
}