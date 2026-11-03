/*
 * DANOS-Open Management: NETCONF peer-to-RBAC role policy
 */

#include "netconf_role.h"

#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NETCONF_ROLE_MAP_DEFAULT "/etc/danos/netconf-roles"
#define NETCONF_ROLE_LINE_MAX    256

static bool line_is_comment(const char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    return *line == '\0' || *line == '#' || *line == '\n';
}

static bool group_list_contains(const char *user, uid_t uid, gid_t gid)
{
    struct passwd *pw = getpwuid(uid);
    if (!pw) return false;
    const char *name = (user && *user) ? user : pw->pw_name;

    long max = sysconf(_SC_NGROUPS_MAX);
    if (max < 1) max = 64;
    gid_t *groups = calloc((size_t)max, sizeof(*groups));
    if (!groups) return false;

    int count = (int)max;
    bool found = false;
    if (getgrouplist(name, pw->pw_gid, groups, &count) >= 0) {
        for (int i = 0; i < count; i++) {
            if (groups[i] == gid) { found = true; break; }
        }
    }
    free(groups);
    return found;
}

/* Match a mapping key against the authenticated identity.  Numeric keys match
 * the uid, textual keys match the NSS user name, and "@group" keys match any
 * group (primary or supplementary) of that user. */
static bool key_matches(const char *key, const char *user, uid_t uid)
{
    if (!key || !*key) return false;
    if (key[0] == '@') {
        struct group *gr = getgrnam(key + 1);
        return gr && group_list_contains(user, uid, gr->gr_gid);
    }
    if (key[0] >= '0' && key[0] <= '9') {
        char *end = NULL;
        unsigned long parsed = strtoul(key, &end, 10);
        return end && *end == '\0' && (uid_t)parsed == uid;
    }
    return user && strcmp(key, user) == 0;
}

danos_sec_role_t netconf_role_resolve(const char *user, uid_t uid)
{
    const char *path = getenv("DANOS_NETCONF_ROLE_MAP");
    if (!path || !*path) path = NETCONF_ROLE_MAP_DEFAULT;

    danos_sec_role_t fallback = uid == 0 ? DANOS_ROLE_ADMIN
                                         : DANOS_ROLE_VIEWER;
    FILE *f = fopen(path, "re");
    if (!f) return fallback;    /* no policy file: safe uid defaults */

    char line[NETCONF_ROLE_LINE_MAX];
    while (fgets(line, sizeof(line), f)) {
        if (line_is_comment(line)) continue;

        char key[128], role_name[64];
        if (sscanf(line, "%127s %63s", key, role_name) != 2) continue;
        if (!key_matches(key, user, uid)) continue;

        danos_sec_role_t role;
        if (danos_rbac_role_by_name(role_name, &role) != 0) continue;
        fclose(f);
        return role;
    }
    fclose(f);
    return fallback;
}
