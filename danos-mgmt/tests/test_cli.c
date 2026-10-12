/*
 * Test: CLI (E1)
 * Verify CLI command parsing and dispatch.
 */
#include "../src/cli/cli.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <danos/core/object_registry.h>
#include <danos/core/backend_ops.h>

static unsigned observed_program_calls;
static danos_status_t observe_route_add(const danos_resolved_route_t *route, void *user)
{
    (void)route; (void)user;
    observed_program_calls++;
    return DANOS_OK;
}

static int test_dataplane_readonly(void)
{
    danos_cli_ctx_t ctx;
    danos_cli_init(&ctx);
    danos_object_store_t *saved = g_default_store;
    g_default_store = danos_object_store_create(16);
    assert(g_default_store);
    danos_route_t route = {0};
    route.flags = DANOS_ROUTE_FLAG_BLACKHOLE;
    assert(danos_object_create(g_default_store, DANOS_OBJ_ROUTE, 123,
                              &route, sizeof(route)) == DANOS_OK);
    uint64_t programmed = danos_programming_programmed_count(DANOS_OBJ_ROUTE);
    danos_capability_t capability = { .type = DANOS_OBJ_ROUTE,
                                     .supported = false, .max_count = 123 };
    danos_backend_info_t backend = { .name = "cli-observe-test",
                                    .caps = &capability, .cap_count = 1 };
    assert(danos_backend_register(&backend) == DANOS_OK);
    const danos_backend_ops_t *saved_ops = danos_backend_ops_get();
    danos_backend_ops_t ops = { .name = "cli-observe-active", .route_add = observe_route_add };
    danos_backend_ops_set(&ops);
    FILE *output = tmpfile();
    assert(output);
    fflush(stdout);
    int original = dup(STDOUT_FILENO);
    assert(original >= 0 && dup2(fileno(output), STDOUT_FILENO) >= 0);
    assert(danos_cli_process(&ctx, "show dataplane") == 0);
    assert(danos_cli_process(&ctx, "show dataplane object 4 123") == 0);
    fflush(stdout);
    assert(dup2(original, STDOUT_FILENO) >= 0);
    close(original);
    rewind(output);
    char text[4096] = {0};
    assert(fread(text, 1, sizeof(text)-1, output) > 0);
    fclose(output);
    assert(strstr(text, "TYPE DESIRED PROGRAMMED"));
    assert(strstr(text, "ledger state, not a hardware FIB readback"));
    assert(strstr(text, "4 1 "));
    assert(strstr(text, "Active programming backend: cli-observe-active"));
    assert(strstr(text, "cli-observe-test 4 no 123"));
    assert(strstr(text, "4 123 yes no"));
    assert(strstr(text, "ledger_matches_sampled_desired=no"));
    assert(strstr(text, "Retry: recorded=no"));
    bool desired, recorded;
    danos_programming_object_status_t status;
    assert(danos_programming_object_status(DANOS_OBJ_ROUTE, 123, &status) == DANOS_OK);
    assert(status.desired && !status.recorded && !status.retry_recorded);
    assert(danos_programming_object_presence(DANOS_OBJ_ROUTE, 123, &desired, &recorded) == DANOS_OK);
    assert(desired && !recorded && observed_program_calls == 0);
    assert(danos_programming_object_presence(DANOS_OBJ_ROUTE, 999, &desired, &recorded) == DANOS_OK);
    assert(!desired && !recorded);
    assert(danos_object_count(g_default_store, DANOS_OBJ_ROUTE) == 1);
    assert(danos_programming_programmed_count(DANOS_OBJ_ROUTE) == programmed);
    assert(danos_cli_process(&ctx, "show dataplane clear") == -1);
    assert(danos_cli_process(&ctx, "show dataplane object -1 123") == -1);
    assert(danos_cli_process(&ctx, "show dataplane object 4 18446744073709551616") == -1);
    assert(danos_cli_process(&ctx, "show dataplane object 4 123 clear") == -1);
    assert(danos_programming_object_presence(DANOS_OBJ_INVALID, 123, &desired, &recorded) == DANOS_ERR_INVALID_ARG);
    assert(danos_programming_run(NULL, NULL) == 1);
    assert(danos_programming_object_presence(DANOS_OBJ_ROUTE, 123, &desired, &recorded) == DANOS_OK);
    assert(desired && recorded && observed_program_calls == 1);
    assert(danos_programming_object_status(DANOS_OBJ_ROUTE, 123, &status) == DANOS_OK);
    assert(status.ledger_matches_sampled_desired && !status.retry_recorded);
    assert(strcmp(status.recorded_backend, "cli-observe-active") == 0);
    danos_backend_ops_t changed_ops = { .name = "cli-not-the-recorded-backend", .route_add = observe_route_add };
    danos_backend_ops_set(&changed_ops);
    assert(danos_programming_object_status(DANOS_OBJ_ROUTE, 123, &status) == DANOS_OK);
    assert(strcmp(status.recorded_backend, "cli-observe-active") == 0);
    assert(observed_program_calls == 1);
    danos_backend_ops_set(&ops);
    route.admin_distance++;
    assert(danos_object_update(g_default_store, DANOS_OBJ_ROUTE, 123, &route, sizeof(route)) == DANOS_OK);
    assert(danos_programming_object_status(DANOS_OBJ_ROUTE, 123, &status) == DANOS_OK);
    assert(!status.ledger_matches_sampled_desired && observed_program_calls == 1);
    route.flags = 0; /* unusable path: explicit test programming pass records retry */
    assert(danos_object_update(g_default_store, DANOS_OBJ_ROUTE, 123, &route, sizeof(route)) == DANOS_OK);
    danos_programming_set_policy(1, 1000, 60000, 5000, 3);
    assert(danos_programming_run(NULL, NULL) == 0);
    assert(danos_programming_object_status(DANOS_OBJ_ROUTE, 123, &status) == DANOS_OK);
    assert(status.retry_recorded && status.retry_matches_sampled_desired);
    assert(status.attempts == 1 && status.last_error == DANOS_ERR_RETRY);
    assert(status.exhausted && status.backoff && !status.flapping);
    route.protocol++;
    assert(danos_object_update(g_default_store, DANOS_OBJ_ROUTE, 123, &route, sizeof(route)) == DANOS_OK);
    assert(danos_programming_object_status(DANOS_OBJ_ROUTE, 123, &status) == DANOS_OK);
    assert(status.retry_recorded && !status.retry_matches_sampled_desired);
    assert(danos_object_delete(g_default_store, DANOS_OBJ_ROUTE, 123) == DANOS_OK);
    assert(danos_programming_object_presence(DANOS_OBJ_ROUTE, 123, &desired, &recorded) == DANOS_OK);
    assert(!desired && recorded && observed_program_calls == 1);
    assert(danos_programming_object_status(DANOS_OBJ_ROUTE, 123, &status) == DANOS_OK);
    assert(!status.desired && status.recorded && !status.retry_matches_sampled_desired);
    assert(strcmp(status.recorded_backend, "cli-observe-active") == 0);
    danos_programming_forget_programmed(); /* test cleanup only, never query */
    assert(danos_programming_object_status(DANOS_OBJ_ROUTE, 123, &status) == DANOS_OK);
    assert(!status.recorded && !status.recorded_backend[0]);
    danos_programming_set_policy(5, 1000, 60000, 5000, 3);
    danos_nexthop_t nh = {0};
    nh.ifindex = 1;
    nh.gateway.af = DANOS_AF_IPV4;
    nh.gateway.addr[0] = 10;
    nh.gateway.addr[3] = 2;
    danos_nhgroup_t group = {0};
    group.nh_count = 1;
    group.nh_ids[0] = 201;
    danos_route_t dependent = {0};
    dependent.nhgroup_id = 200;
    assert(danos_object_create(g_default_store, DANOS_OBJ_NEXTHOP, 201, &nh, sizeof(nh)) == DANOS_OK);
    assert(danos_object_create(g_default_store, DANOS_OBJ_NHGROUP, 200, &group, sizeof(group)) == DANOS_OK);
    assert(danos_object_create(g_default_store, DANOS_OBJ_ROUTE, 500, &dependent, sizeof(dependent)) == DANOS_OK);
    assert(danos_programming_run(NULL, NULL) == 1);
    assert(danos_programming_object_status(DANOS_OBJ_ROUTE, 500, &status) == DANOS_OK);
    assert(status.ledger_matches_sampled_desired && observed_program_calls == 2);
    nh.gateway.addr[3] = 3;
    assert(danos_object_update(g_default_store, DANOS_OBJ_NEXTHOP, 201, &nh, sizeof(nh)) == DANOS_OK);
    assert(danos_programming_object_status(DANOS_OBJ_ROUTE, 500, &status) == DANOS_OK);
    assert(!status.ledger_matches_sampled_desired && observed_program_calls == 2);
    danos_programming_forget_programmed();
    assert(danos_object_delete(g_default_store, DANOS_OBJ_ROUTE, 500) == DANOS_OK);
    unsigned char oversized[DANOS_MAX_OBJECT_BYTES] = {0};
    assert(danos_object_create(g_default_store, DANOS_OBJ_ROUTE, 600, oversized, sizeof(oversized)) == DANOS_OK);
    uint64_t attempted = 0, failed = 0;
    assert(danos_programming_run(&attempted, &failed) == 0);
    assert(attempted == 1 && failed == 1 && observed_program_calls == 2);
    assert(danos_programming_object_status(DANOS_OBJ_ROUTE, 600, &status) == DANOS_OK);
    assert(status.desired && !status.recorded && !status.recorded_backend[0]);
    danos_backend_ops_set(saved_ops);
    assert(danos_backend_unregister("cli-observe-test") == DANOS_OK);
    danos_object_store_destroy(g_default_store);
    g_default_store = saved;
    printf("[PASS] test_dataplane_readonly\n");
    return 0;
}

int test_cli_basic(void)
{
    danos_cli_ctx_t ctx;
    danos_cli_init(&ctx);

    /* Empty line */
    assert(danos_cli_process(&ctx, "") == 0);
    assert(danos_cli_process(&ctx, "   ") == 0);

    /* Help */
    assert(danos_cli_process(&ctx, "help") == 0);
    assert(danos_cli_process(&ctx, "?") == 0);

    /* Show commands */
    assert(danos_cli_process(&ctx, "show interface") == 0);
    assert(danos_cli_process(&ctx, "show route") == 0);
    assert(danos_cli_process(&ctx, "show vrf") == 0);
    assert(danos_cli_process(&ctx, "show int") == 0);  /* abbreviation */

    printf("[PASS] test_cli_basic: basic commands work\n");
    return 0;
}

int test_cli_configure_mode(void)
{
    danos_cli_ctx_t ctx;
    danos_cli_init(&ctx);

    assert(ctx.in_configure_mode == false);
    assert(strcmp(ctx.prompt, "danos> ") == 0);

    /* Enter configure mode */
    assert(danos_cli_process(&ctx, "configure") == 0);
    assert(ctx.in_configure_mode == true);
    assert(strcmp(ctx.prompt, "danos(config)# ") == 0);

    /* Exit configure mode */
    assert(danos_cli_process(&ctx, "exit") == 0);
    assert(ctx.in_configure_mode == false);
    assert(strcmp(ctx.prompt, "danos> ") == 0);

    printf("[PASS] test_cli_configure_mode: configure/exit works\n");
    return 0;
}

int test_cli_exit(void)
{
    danos_cli_ctx_t ctx;
    danos_cli_init(&ctx);

    /* exit at top level returns 1 (quit) */
    assert(danos_cli_process(&ctx, "exit") == 1);
    assert(danos_cli_process(&ctx, "quit") == 1);

    printf("[PASS] test_cli_exit: exit returns 1 at top level\n");
    return 0;
}

int test_cli_unknown(void)
{
    danos_cli_ctx_t ctx;
    danos_cli_init(&ctx);

    /* Unknown command returns -1 */
    assert(danos_cli_process(&ctx, "foobar") == -1);
    assert(danos_cli_process(&ctx, "show foobar") == -1);

    printf("[PASS] test_cli_unknown: unknown commands return -1\n");
    return 0;
}

int main(void)
{
    int failed = 0;
    if (test_cli_basic() != 0) failed++;
    if (test_cli_configure_mode() != 0) failed++;
    if (test_cli_exit() != 0) failed++;
    if (test_cli_unknown() != 0) failed++;
    if (test_dataplane_readonly() != 0) failed++;
    printf("=== cli_test: %s ===\n",
           failed == 0 ? "ALL PASSED" : "FAILURES");
    return failed;
}
