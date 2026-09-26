/*
 * test_config.c — configuraciones TOML válidas e inválidas con diagnóstico,
 * instantánea resultante y referencias.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>

#include "config.h"

#define TEST_TIMEOUT_S 5

static int setup(void **state) {
    (void)state;
    alarm(TEST_TIMEOUT_S);
    return 0;
}

static int teardown(void **state) {
    (void)state;
    alarm(0);
    return 0;
}

static const char *const FULL = "[server]\n"
                                "workers = 1\n"
                                "max_connections = 100\n"
                                "shutdown_timeout_ms = 2500\n"
                                "[limits]\n"
                                "max_header_bytes = 8192\n"
                                "max_request_line = 4096\n"
                                "max_headers = 50\n"
                                "max_body_bytes = 1000000\n"
                                "max_requests_per_connection = 10\n"
                                "[timeouts]\n"
                                "client_header_ms = 1\n"
                                "client_idle_ms = 2\n"
                                "upstream_connect_ms = 3\n"
                                "upstream_response_ms = 4\n"
                                "io_idle_ms = 5\n"
                                "close_ms = 6\n"
                                "[[frontend]]\n"
                                "listen = \"127.0.0.1:7777\"\n"
                                "[[frontend]]\n"
                                "listen = \"[::1]:7778\"\n"
                                "trusted_proxies = [\"127.0.0.1\", \"::1\"]\n"
                                "[[pool]]\n"
                                "name = \"a\"\n"
                                "algorithm = \"round_robin\"\n"
                                "[[pool.backend]]\n"
                                "address = \"127.0.0.1:3000\"\n"
                                "weight = 5\n"
                                "max_conns = 7\n"
                                "[[pool.backend]]\n"
                                "address = \"[::1]:3001\"\n"
                                "[[pool]]\n"
                                "name = \"b-2\"\n"
                                "backend = [ { address = \"10.0.0.2:80\" } ]\n"
                                "[[route]]\n"
                                "host = \"API.example.com\"\n"
                                "pool = \"a\"\n"
                                "[[route]]\n"
                                "host = \"*.example.com\"\n"
                                "pool = \"b-2\"\n"
                                "[routing]\n"
                                "default_pool = \"a\"\n";

static void test_full_config_snapshot(void **state) {
    (void)state;
    char err[CONFIG_ERR_LEN];
    struct config *cfg = config_load_string(FULL, "t.toml", err, sizeof(err));
    if (cfg == NULL) {
        fail_msg("%s", err);
    }
    assert_int_equal(cfg->workers, 1);
    assert_int_equal(cfg->max_connections, 100);
    assert_int_equal(cfg->shutdown_timeout_ms, 2500);
    assert_int_equal(cfg->limits.max_header_bytes, 8192);
    assert_int_equal(cfg->limits.max_request_line, 4096);
    assert_int_equal(cfg->limits.max_headers, 50);
    assert_int_equal(cfg->limits.max_body_bytes, 1000000);
    assert_int_equal(cfg->limits.max_requests_per_connection, 10);
    assert_int_equal(cfg->timeouts.client_header_ms, 1);
    assert_int_equal(cfg->timeouts.close_ms, 6);

    assert_int_equal(cfg->nfrontends, 2);
    assert_string_equal(cfg->frontends[0].listen.text, "127.0.0.1:7777");
    assert_int_equal(cfg->frontends[0].listen.ss.ss_family, AF_INET);
    assert_int_equal(ntohs(((struct sockaddr_in *)&cfg->frontends[0].listen.ss)->sin_port), 7777);
    assert_int_equal(cfg->frontends[1].listen.ss.ss_family, AF_INET6);
    assert_int_equal(cfg->frontends[1].ntrusted, 2);

    assert_int_equal(cfg->npools, 2);
    assert_string_equal(cfg->pools[0].name, "a");
    assert_int_equal(cfg->pools[0].nbackends, 2);
    assert_int_equal(cfg->pools[0].backends[0].weight, 5);
    assert_int_equal(cfg->pools[0].backends[0].max_conns, 7);
    assert_int_equal(cfg->pools[0].backends[1].weight, 1); /* por defecto */
    assert_string_equal(cfg->pools[1].backends[0].addr.text, "10.0.0.2:80");

    assert_true(cfg->has_default);
    struct route_result r = router_lookup(cfg->router, "api.example.com", 15);
    assert_int_equal(r.match, ROUTE_EXACT);
    assert_int_equal(r.target, 0);
    r = router_lookup(cfg->router, "x.example.com", 13);
    assert_int_equal(r.match, ROUTE_WILDCARD);
    assert_int_equal(r.target, 1);
    r = router_lookup(cfg->router, "other", 5);
    assert_int_equal(r.match, ROUTE_DEFAULT);

    /* Referencias: la instantánea sobrevive a su creador. */
    struct config *held = config_ref(cfg);
    config_unref(cfg);
    assert_string_equal(held->pools[0].name, "a");
    config_unref(held);
    config_unref(NULL);
}

static void test_defaults(void **state) {
    (void)state;
    char err[CONFIG_ERR_LEN];
    struct config *cfg = config_load_string("[[frontend]]\nlisten = \"0.0.0.0:8080\"\n"
                                            "[[pool]]\nname = \"p\"\n"
                                            "[[pool.backend]]\naddress = \"127.0.0.1:1\"\n"
                                            "[routing]\ndefault_pool = \"p\"\n",
                                            "d.toml", err, sizeof(err));
    if (cfg == NULL) {
        fail_msg("%s", err);
    }
    assert_int_equal(cfg->max_connections, 1024);
    assert_int_equal(cfg->limits.max_header_bytes, 16384);
    assert_int_equal(cfg->timeouts.client_header_ms, 10000);
    assert_int_equal(cfg->timeouts.upstream_response_ms, 30000);
    assert_int_equal(cfg->nroutes, 0);
    config_unref(cfg);
}

static void test_trusted_peer_matching(void **state) {
    (void)state;
    char err[CONFIG_ERR_LEN];
    struct config *cfg = config_load_string(FULL, "t.toml", err, sizeof(err));
    assert_non_null(cfg);
    struct sockaddr_storage peer;
    memset(&peer, 0, sizeof(peer));
    struct sockaddr_in *v4 = (struct sockaddr_in *)&peer;
    v4->sin_family = AF_INET;
    v4->sin_port = htons(55555); /* el puerto no importa */
    assert_int_equal(inet_pton(AF_INET, "127.0.0.1", &v4->sin_addr), 1);
    assert_false(config_peer_trusted(&cfg->frontends[0], &peer));
    assert_true(config_peer_trusted(&cfg->frontends[1], &peer));
    assert_int_equal(inet_pton(AF_INET, "127.0.0.2", &v4->sin_addr), 1);
    assert_false(config_peer_trusted(&cfg->frontends[1], &peer));
    config_unref(cfg);
}

/* Base mínima válida a la que se añade o sustituye una sección. */
#define FE    "[[frontend]]\nlisten = \"127.0.0.1:7777\"\n"
#define POOL  "[[pool]]\nname = \"a\"\n[[pool.backend]]\naddress = \"127.0.0.1:3000\"\n"
#define ROUTE "[[route]]\nhost = \"a.test\"\npool = \"a\"\n"

static void expect_invalid(const char *text, const char *needle) {
    char err[CONFIG_ERR_LEN];
    struct config *cfg = config_load_string(text, "bad.toml", err, sizeof(err));
    if (cfg != NULL) {
        config_unref(cfg);
        fail_msg("se esperaba error con \"%s\" para:\n%s", needle, text);
    }
    if (strstr(err, needle) == NULL) {
        fail_msg("diagnóstico sin \"%s\": %s", needle, err);
    }
    if (strncmp(err, "bad.toml: ", 10) != 0) {
        fail_msg("el diagnóstico no indica el origen: %s", err);
    }
}

static void test_invalid_configs(void **state) {
    (void)state;
    /* Sintaxis y estructura. */
    expect_invalid("[[frontend]\n", "TOML inválido");
    expect_invalid(FE POOL ROUTE "extra = 1\n", "clave desconocida \"extra\"");
    expect_invalid("[server]\nworker = 1\n" FE POOL ROUTE, "[server]: clave desconocida");
    expect_invalid(POOL ROUTE, "al menos un [[frontend]]");
    expect_invalid(FE ROUTE, "al menos un [[pool]]");
    expect_invalid(FE POOL, "ninguna [[route]] ni [routing].default_pool");
    expect_invalid("frontend = 1\n" POOL ROUTE, "lista de tablas");
    /* Tipos y rangos. */
    expect_invalid("[server]\nmax_connections = \"10\"\n" FE POOL ROUTE, "se esperaba un entero");
    expect_invalid("[server]\nmax_connections = 0\n" FE POOL ROUTE, "fuera de rango");
    expect_invalid("[server]\nmax_connections = 40000\n" FE POOL ROUTE, "fuera de rango");
    expect_invalid("[server]\nworkers = 65\n" FE POOL ROUTE, "fuera de rango");
    expect_invalid("[timeouts]\nclient_header_ms = 0\n" FE POOL ROUTE, "client_header_ms");
    expect_invalid("[timeouts]\nio_idle_ms = -5\n" FE POOL ROUTE, "fuera de rango");
    expect_invalid("[limits]\nmax_header_bytes = 20000\n" FE POOL ROUTE, "max_header_bytes");
    expect_invalid("[limits]\nmax_header_bytes = 2048\nmax_request_line = 4096\n" FE POOL ROUTE,
                   "no puede superar max_header_bytes");
    expect_invalid("[limits]\nmax_headers = 101\n" FE POOL ROUTE, "max_headers");
    /* Direcciones. */
    expect_invalid("[[frontend]]\nlisten = \"localhost:7777\"\n" POOL ROUTE,
                   "solo se admiten IP literales");
    expect_invalid("[[frontend]]\nlisten = \"127.0.0.1\"\n" POOL ROUTE, "falta el puerto");
    expect_invalid("[[frontend]]\nlisten = \"127.0.0.1:0\"\n" POOL ROUTE, "fuera de rango");
    expect_invalid("[[frontend]]\nlisten = \"127.0.0.1:70000\"\n" POOL ROUTE, "puerto");
    expect_invalid("[[frontend]]\nlisten = \"::1:80\"\n" POOL ROUTE, "entre corchetes");
    expect_invalid("[[frontend]]\nlisten = \"300.1.1.1:80\"\n" POOL ROUTE, "IP inválida");
    expect_invalid(FE FE POOL ROUTE, "repetido");
    expect_invalid(FE "trusted_proxies = [\"10.0.0.0/8\"]\n" POOL ROUTE, "sin CIDR");
    expect_invalid(FE "trusted_proxies = \"127.0.0.1\"\n" POOL ROUTE, "lista de cadenas");
    /* Pools y backends. */
    expect_invalid(FE "[[pool]]\nname = \"a\"\n" ROUTE, "[[pool.backend]]");
    expect_invalid(FE "[[pool]]\nname = \"a b\"\n[[pool.backend]]\naddress = \"127.0.0.1:1\"\n",
                   "solo [A-Za-z0-9_-]");
    expect_invalid(FE POOL POOL ROUTE, "repetido");
    expect_invalid(FE "[[pool]]\nname = \"a\"\nalgorithm = \"random\"\n"
                      "[[pool.backend]]\naddress = \"127.0.0.1:1\"\n" ROUTE,
                   "desconocido");
    expect_invalid(FE "[[pool]]\nname = \"a\"\n[[pool.backend]]\naddress = \"127.0.0.1:1\"\n"
                      "weight = 0\n" ROUTE,
                   "weight");
    expect_invalid(FE
                   "[[pool]]\nname = \"a\"\n[[pool.backend]]\naddress = \"db.local:5432\"\n" ROUTE,
                   "solo se admiten IP literales");
    expect_invalid(FE "[[pool]]\nname = \"a\"\n[[pool.backend]]\naddress = \"127.0.0.1:1\"\n"
                      "[[pool.backend]]\naddress = \"127.0.0.1:1\"\n" ROUTE,
                   "repetido en el pool");
    expect_invalid(FE "[[pool]]\nname = \"a\"\n[[pool.backend]]\nhost = \"127.0.0.1:1\"\n" ROUTE,
                   "clave desconocida \"host\"");
    /* Rutas. */
    expect_invalid(FE POOL "[[route]]\nhost = \"a.test\"\npool = \"zzz\"\n",
                   "el pool \"zzz\" no existe");
    expect_invalid(FE POOL "[[route]]\npool = \"a\"\n", "falta la clave obligatoria \"host\"");
    expect_invalid(FE POOL "[[route]]\nhost = \"*.*.test\"\npool = \"a\"\n", "[[route]] #1");
    expect_invalid(FE POOL "[[route]]\nhost = \"a.test:80\"\npool = \"a\"\n", "puerto");
    expect_invalid(FE POOL ROUTE ROUTE, "duplicado");
    expect_invalid(FE POOL ROUTE "[routing]\ndefault_pool = \"nope\"\n", "no existe");
}

static void test_health_and_algorithms(void **state) {
    (void)state;
    char err[CONFIG_ERR_LEN];
    struct config *cfg = config_load_string(
        FE "[[pool]]\nname = \"h\"\nalgorithm = \"least_conn\"\n"
           "[pool.health]\ntype = \"http\"\ninterval_ms = 500\ntimeout_ms = 200\nfall = 4\n"
           "rise = 5\npassive_fall = 0\npath = \"/ready?x=1\"\nhost = \"svc.local\"\n"
           "expect_status_min = 200\nexpect_status_max = 204\n"
           "[[pool.backend]]\naddress = \"127.0.0.1:1\"\n"
           "[[pool]]\nname = \"t\"\nalgorithm = \"weighted\"\n"
           "[pool.health]\ntype = \"tcp\"\n"
           "[[pool.backend]]\naddress = \"127.0.0.1:2\"\nweight = 7\n"
           "[routing]\ndefault_pool = \"h\"\n",
        "h.toml", err, sizeof(err));
    if (cfg == NULL) {
        fail_msg("%s", err);
    }
    const struct cfg_health *h = &cfg->pools[0].health;
    assert_int_equal(cfg->pools[0].algorithm, LB_LEAST_CONN);
    assert_int_equal(h->type, HEALTH_HTTP);
    assert_int_equal(h->interval_ms, 500);
    assert_int_equal(h->timeout_ms, 200);
    assert_int_equal(h->fall, 4);
    assert_int_equal(h->rise, 5);
    assert_int_equal(h->passive_fall, 0);
    assert_string_equal(h->path, "/ready?x=1");
    assert_string_equal(h->host, "svc.local");
    assert_int_equal(h->expect_max, 204);
    const struct cfg_health *t = &cfg->pools[1].health;
    assert_int_equal(cfg->pools[1].algorithm, LB_WEIGHTED);
    assert_int_equal(t->type, HEALTH_TCP);
    assert_int_equal(t->interval_ms, 2000); /* por defecto */
    assert_int_equal(t->fall, 3);
    assert_int_equal(t->passive_fall, 3);
    assert_int_equal(cfg->pools[1].backends[0].weight, 7);
    assert_string_equal(lb_algorithm_name(LB_LEAST_CONN), "least_conn");
    config_unref(cfg);

    /* Sin [pool.health]: sin sondas. */
    cfg = config_load_string(FE POOL ROUTE, "n.toml", err, sizeof(err));
    assert_non_null(cfg);
    assert_int_equal(cfg->pools[0].health.type, HEALTH_NONE);
    config_unref(cfg);
}

#define HPOOL(extra)                                                                               \
    FE "[[pool]]\nname = \"a\"\n[pool.health]\n" extra                                             \
       "[[pool.backend]]\naddress = \"127.0.0.1:3000\"\n" ROUTE

static void test_invalid_health(void **state) {
    (void)state;
    expect_invalid(HPOOL("interval_ms = 100\n"), "falta la clave obligatoria \"type\"");
    expect_invalid(HPOOL("type = \"icmp\"\n"), "válidos \"tcp\" o \"http\"");
    expect_invalid(HPOOL("type = \"tcp\"\ninterval_ms = 500\ntimeout_ms = 500\n"),
                   "debe ser menor que interval_ms");
    expect_invalid(HPOOL("type = \"tcp\"\npath = \"/h\"\n"), "solo aplica a type = \"http\"");
    expect_invalid(HPOOL("type = \"http\"\npath = \"health\"\n"), "debe empezar por '/'");
    expect_invalid(HPOOL("type = \"http\"\npath = \"/a b\"\n"), "debe empezar por '/'");
    expect_invalid(HPOOL("type = \"http\"\nhost = \"bad host\"\n"), "nombre de host inválido");
    expect_invalid(HPOOL("type = \"http\"\nexpect_status_min = 300\nexpect_status_max = 200\n"),
                   "expect_status_min");
    expect_invalid(HPOOL("type = \"tcp\"\nfall = 0\n"), "fall");
    expect_invalid(HPOOL("type = \"tcp\"\ninterval = 5\n"), "clave desconocida \"interval\"");
    expect_invalid(HPOOL("type = \"tcp\"\ninterval_ms = 50\n"), "interval_ms");
}

static struct config *load_ok(const char *text) {
    char err[CONFIG_ERR_LEN];
    struct config *cfg = config_load_string(text, "r.toml", err, sizeof(err));
    if (cfg == NULL) {
        fail_msg("%s", err);
    }
    return cfg;
}

static void test_reload_compatibility(void **state) {
    (void)state;
    struct config *base = load_ok("[server]\nmax_connections = 10\n" FE POOL ROUTE);
    char err[CONFIG_ERR_LEN];

    /* Rutas, pools, límites, timeouts y confianza sí son recargables. */
    struct config *ok = load_ok("[server]\nmax_connections = 10\nshutdown_timeout_ms = 5\n"
                                "[timeouts]\nio_idle_ms = 7\n"
                                "[[frontend]]\nlisten = \"127.0.0.1:7777\"\n"
                                "trusted_proxies = [\"10.0.0.1\"]\n"
                                "[[pool]]\nname = \"z\"\nalgorithm = \"least_conn\"\n"
                                "[[pool.backend]]\naddress = \"127.0.0.1:9\"\n"
                                "[routing]\ndefault_pool = \"z\"\n");
    assert_true(config_reload_compatible(base, ok, err, sizeof(err)));
    config_unref(ok);

    struct config *port = load_ok("[server]\nmax_connections = 10\n"
                                  "[[frontend]]\nlisten = \"127.0.0.1:7780\"\n" POOL ROUTE);
    assert_false(config_reload_compatible(base, port, err, sizeof(err)));
    assert_non_null(strstr(err, "127.0.0.1:7777 -> 127.0.0.1:7780"));
    assert_non_null(strstr(err, "no son recargables"));
    config_unref(port);

    struct config *more = load_ok("[server]\nmax_connections = 10\n" FE
                                  "[[frontend]]\nlisten = \"127.0.0.1:7781\"\n" POOL ROUTE);
    assert_false(config_reload_compatible(base, more, err, sizeof(err)));
    assert_non_null(strstr(err, "número de [[frontend]] 1 -> 2"));
    config_unref(more);

    struct config *maxc = load_ok("[server]\nmax_connections = 11\n" FE POOL ROUTE);
    assert_false(config_reload_compatible(base, maxc, err, sizeof(err)));
    assert_non_null(strstr(err, "max_connections 10 -> 11"));
    config_unref(maxc);
    config_unref(base);
}

static void test_load_file_errors(void **state) {
    (void)state;
    char err[CONFIG_ERR_LEN];
    assert_null(config_load_file("/nonexistent/proxy.toml", err, sizeof(err)));
    assert_non_null(strstr(err, "no se puede abrir"));
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_full_config_snapshot, setup, teardown),
        cmocka_unit_test_setup_teardown(test_defaults, setup, teardown),
        cmocka_unit_test_setup_teardown(test_trusted_peer_matching, setup, teardown),
        cmocka_unit_test_setup_teardown(test_invalid_configs, setup, teardown),
        cmocka_unit_test_setup_teardown(test_load_file_errors, setup, teardown),
        cmocka_unit_test_setup_teardown(test_health_and_algorithms, setup, teardown),
        cmocka_unit_test_setup_teardown(test_invalid_health, setup, teardown),
        cmocka_unit_test_setup_teardown(test_reload_compatibility, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
