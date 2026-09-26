/*
 * test_stats.c — agregación de instantáneas y JSON: sumas, ausentes, salud
 * mixta, tipos de contador y escapado.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>

#include "stats.h"

static const char W0[] =
    "worker index=0 pid=100 generation=3 uptime_ms=5000 conns=2 upstreams=1 slots_in_use=6 "
    "slots_total=100 backend_active=1 generations_live=1 backends_live=2 draining=0 "
    "reload_running=0 accepted=10 rejected=0 accept_emfile=0 requests=40 proxy_4xx=1 "
    "proxy_5xx=2 upstream_reusable=0 reloads_ok=2 reloads_failed=0 health_results=5 "
    "health_stale=0 health_dropped=0 log_written=50 log_dropped=3 log_write_errors=0 "
    "log_truncated=0\n"
    "backend pool=p addr=127.0.0.1:1 algo=least_conn weight=1 max_conns=0 active=1 selected=20 "
    "failures=0 health=up\n"
    "backend pool=p addr=127.0.0.1:2 algo=least_conn weight=1 max_conns=0 active=0 selected=20 "
    "failures=1 health=up\n";

static const char W1[] =
    "worker index=1 pid=101 generation=3 uptime_ms=4000 conns=1 upstreams=0 slots_in_use=2 "
    "slots_total=100 backend_active=0 generations_live=2 backends_live=2 draining=0 "
    "reload_running=0 accepted=7 rejected=1 accept_emfile=0 requests=13 proxy_4xx=0 "
    "proxy_5xx=0 upstream_reusable=0 reloads_ok=2 reloads_failed=0 health_results=5 "
    "health_stale=1 health_dropped=0 log_written=20 log_dropped=0 log_write_errors=0 "
    "log_truncated=0\n"
    "backend pool=p addr=127.0.0.1:1 algo=least_conn weight=1 max_conns=0 active=0 selected=6 "
    "failures=0 health=up\n"
    "backend pool=p addr=127.0.0.1:2 algo=least_conn weight=1 max_conns=0 active=0 selected=7 "
    "failures=4 health=down-passive\n";

static void test_aggregation_and_missing(void **state) {
    (void)state;
    alarm(5);
    const struct stats_worker_input in[3] = {
        {0, 100, "ready", 0, W0, sizeof(W0) - 1},
        {1, 101, "ready", 1, W1, sizeof(W1) - 1},
        {2, 0, "restarting", 2, NULL, 0},
    };
    struct stats_master_info m = {.pid = 99,
                                  .uptime_ms = 6000,
                                  .generation = 3,
                                  .workers_configured = 3,
                                  .workers_ready = 2,
                                  .reload_state = "idle",
                                  .recovery = "exhausted",
                                  .recovery_generation = 3,
                                  .reloads_ok = 2};
    size_t len = 0;
    char *json = stats_render_json(&m, in, 3, &len);
    assert_non_null(json);
    assert_int_equal(strlen(json), len);
    assert_true(json[len - 1] == '\n');
    assert_non_null(strstr(json, "\"complete\":false"));
    assert_non_null(strstr(json, "\"missing_workers\":[2]"));
    assert_non_null(strstr(json, "\"workers_responded\":2"));
    /* Sumas una sola vez por worker. */
    assert_non_null(strstr(json, "\"requests\":53"));
    assert_non_null(strstr(json, "\"conns\":3"));
    assert_non_null(strstr(json, "\"log_dropped\":3"));
    /* Backends agregados por (pool, dirección). */
    assert_non_null(strstr(json, "\"addr\":\"127.0.0.1:1\",\"algorithm\":\"least_conn\","
                                 "\"active\":1,\"selected\":26,\"failures\":0,"
                                 "\"health_by_worker\":{\"up\":2,\"down-active\":0,"
                                 "\"down-passive\":0},\"health_summary\":\"up\""));
    assert_non_null(strstr(json, "\"selected\":27,\"failures\":5"));
    assert_non_null(strstr(json, "\"health_summary\":\"mixed\""));
    /* Tipos y datos del maestro. */
    assert_non_null(strstr(json, "\"requests\":\"cumulative\""));
    assert_non_null(strstr(json, "\"conns\":\"instant\""));
    assert_non_null(strstr(json, "\"generation_min\":3,\"generation_max\":3"));
    assert_non_null(strstr(json, "\"state\":\"restarting\",\"restarts\":2,\"responded\":false"));
    assert_non_null(strstr(json, "\"pid\":99"));
    /* Estado de la recarga y de la recuperación, capacidad reducida. */
    assert_non_null(strstr(json, "\"workers_configured\":3,\"workers_ready\":2,\"degraded\":true"));
    assert_non_null(strstr(json, "\"reload_state\":\"idle\""));
    assert_non_null(strstr(json, "\"recovery\":{\"state\":\"exhausted\",\"generation\":3}"));
    free(json);
    alarm(0);
}

static void test_all_missing_and_escaping(void **state) {
    (void)state;
    alarm(5);
    static const char W[] = "worker index=0 pid=1 generation=1 uptime_ms=1\n"
                            "backend pool=a\"b addr=x\\y algo=round_robin weight=1 max_conns=0 "
                            "active=0 selected=0 failures=0 health=down-active\n";
    const struct stats_worker_input in[1] = {{0, 1, "ready", 0, W, sizeof(W) - 1}};
    struct stats_master_info m = {.workers_configured = 1, .workers_ready = 1};
    size_t len = 0;
    char *json = stats_render_json(&m, in, 1, &len);
    assert_non_null(json);
    assert_non_null(strstr(json, "\"pool\":\"a\\\"b\""));
    assert_non_null(strstr(json, "\"addr\":\"x\\\\y\""));
    assert_non_null(strstr(json, "\"health_summary\":\"down\""));
    assert_non_null(strstr(json, "\"complete\":true"));
    /* Sin datos de recarga: valores por defecto y sin degradación. */
    assert_non_null(strstr(json, "\"degraded\":false"));
    assert_non_null(strstr(json, "\"reload_state\":\"idle\""));
    assert_non_null(strstr(json, "\"recovery\":{\"state\":\"none\",\"generation\":0}"));
    free(json);

    const struct stats_worker_input none[1] = {{0, 0, "failed", 6, NULL, 0}};
    json = stats_render_json(&m, none, 1, &len);
    assert_non_null(json);
    assert_non_null(strstr(json, "\"complete\":false"));
    assert_non_null(strstr(json, "\"workers_responded\":0"));
    free(json);
    alarm(0);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_aggregation_and_missing),
        cmocka_unit_test(test_all_missing_and_escaping),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
