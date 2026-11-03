/*
 * Test: NETCONF peer-to-RBAC role policy
 *
 * The daemon must derive authorization from the local peer uid, not from the
 * client-supplied preamble role.  This pins the default and mapped policy.
 */
#include "../src/netconf/netconf_role.h"
#include <danos/security/rbac.h>
#include <assert.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    char path[128];
    snprintf(path, sizeof(path), "/tmp/danos-netconf-roles-%ld", (long)getpid());
    unlink(path);

    /* No policy file: root is admin, every other uid is viewer. */
    setenv("DANOS_NETCONF_ROLE_MAP", path, 1);
    struct passwd *pw = getpwuid(getuid());
    assert(pw != NULL);
    assert(netconf_role_resolve("root", 0) == DANOS_ROLE_ADMIN);
    assert(netconf_role_resolve(pw->pw_name, getuid()) == DANOS_ROLE_VIEWER);

    /* Explicit name, numeric uid and unknown-role handling. */
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    fprintf(f, "# comment\n");
    fprintf(f, "%s operator\n", pw->pw_name);
    fprintf(f, "0 admin\n");
    fprintf(f, "12345 viewer\n");
    fclose(f);

    assert(netconf_role_resolve(pw->pw_name, getuid()) == DANOS_ROLE_OPERATOR);
    assert(netconf_role_resolve(NULL, 0) == DANOS_ROLE_ADMIN);
    assert(netconf_role_resolve("someone", 12345) == DANOS_ROLE_VIEWER);
    assert(netconf_role_resolve("unmapped", 54321) == DANOS_ROLE_VIEWER);

    /* An invalid role name is not a match; the uid fallback applies. */
    f = fopen(path, "w");
    assert(f != NULL);
    fprintf(f, "%s superuser\n", pw->pw_name);
    fclose(f);
    assert(netconf_role_resolve(pw->pw_name, getuid()) == DANOS_ROLE_VIEWER);

    unlink(path);
    printf("[PASS] test_netconf_role: uid policy, name/uid maps, fail-safe "
           "defaults\n");
    return 0;
}
