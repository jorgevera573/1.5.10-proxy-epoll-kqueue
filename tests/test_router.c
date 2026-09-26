/*
 * test_router.c — precedencia exacto > wildcard (sufijo más largo) > default
 * > ninguno, límites de dominio, colisiones djb2, normalización, validación de
 * patrones y referencias del snapshot.
 */
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>

#include "host.h"
#include "http_parser.h"
#include "router.h"

#define TEST_TIMEOUT_S 5

enum {
    T_EXACT_EXAMPLE = 1,
    T_WILD_EXAMPLE = 2,
    T_WILD_API = 3,
    T_EXACT_API = 4,
    T_DEFAULT = 99,
};

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

static struct route_result lookup(const router *r, const char *host) {
    return router_lookup(r, host, strlen(host));
}

static void expect_route(const router *r, const char *host, enum route_match m, uint32_t target) {
    struct route_result res = lookup(r, host);
    if (res.match != m || (m != ROUTE_NONE && res.target != target)) {
        fail_msg("host \"%s\": esperado match=%d target=%u, obtenido match=%d target=%u", host,
                 (int)m, target, (int)res.match, res.target);
    }
}

static router *build_sample(bool with_default) {
    const struct route_spec specs[] = {
        {"example.com", T_EXACT_EXAMPLE},
        {"*.example.com", T_WILD_EXAMPLE},
        {"*.api.example.com", T_WILD_API},
        {"api.example.com", T_EXACT_API},
    };
    uint32_t def = T_DEFAULT;
    struct router_error err;
    router *r = router_build(specs, 4, with_default ? &def : NULL, &err);
    assert_non_null(r);
    return r;
}

static void test_precedence(void **state) {
    (void)state;
    router *r = build_sample(true);
    assert_int_equal(router_exact_count(r), 2);
    assert_int_equal(router_wildcard_count(r), 2);

    expect_route(r, "example.com", ROUTE_EXACT, T_EXACT_EXAMPLE);
    expect_route(r, "api.example.com", ROUTE_EXACT, T_EXACT_API); /* exacto > wildcard */
    expect_route(r, "www.example.com", ROUTE_WILDCARD, T_WILD_EXAMPLE);
    expect_route(r, "a.b.example.com", ROUTE_WILDCARD, T_WILD_EXAMPLE);
    /* Wildcards solapados: gana el sufijo más largo. */
    expect_route(r, "v1.api.example.com", ROUTE_WILDCARD, T_WILD_API);
    expect_route(r, "x.v1.api.example.com", ROUTE_WILDCARD, T_WILD_API);
    expect_route(r, "other.org", ROUTE_DEFAULT, T_DEFAULT);
    router_unref(r);
}

static void test_no_match_without_default(void **state) {
    (void)state;
    router *r = build_sample(false);
    expect_route(r, "other.org", ROUTE_NONE, 0);
    expect_route(r, "", ROUTE_NONE, 0);
    router_unref(r);

    router *empty = router_build(NULL, 0, NULL, NULL);
    assert_non_null(empty);
    expect_route(empty, "example.com", ROUTE_NONE, 0);
    router_unref(empty);
}

static void test_domain_boundaries(void **state) {
    (void)state;
    router *r = build_sample(false);
    /* El sufijo solo coincide en un límite de etiqueta. */
    expect_route(r, "badexample.com", ROUTE_NONE, 0);
    expect_route(r, "xexample.com", ROUTE_NONE, 0);
    expect_route(r, "example.com.evil.org", ROUTE_NONE, 0);
    expect_route(r, "example.co", ROUTE_NONE, 0);
    expect_route(r, "xapi.example.com", ROUTE_WILDCARD, T_WILD_EXAMPLE);
    /* El wildcard no incluye el propio dominio: sin exacto, no hay ruta. */
    const struct route_spec only_wild[] = {{"*.example.com", 7}};
    router *w = router_build(only_wild, 1, NULL, NULL);
    assert_non_null(w);
    expect_route(w, "example.com", ROUTE_NONE, 0);
    expect_route(w, ".example.com", ROUTE_NONE, 0); /* etiqueta vacía delante */
    expect_route(w, "a.example.com", ROUTE_WILDCARD, 7);
    router_unref(w);
    router_unref(r);
}

static void test_djb2_reference_values(void **state) {
    (void)state;
    assert_int_equal(router_djb2("", 0), 5381u);
    /* 5381 * 33 + 'a' = 177670 */
    assert_int_equal(router_djb2("a", 1), 177670u);
    /* 177670 * 33 + 'b' = 5863208 */
    assert_int_equal(router_djb2("ab", 2), 5863208u);
}

static void test_hash_collisions_resolved_by_key(void **state) {
    (void)state;
    /* 'c'*33 + '-' == 'a'*33 + 'o' (3312): mismo djb2 para estos nombres. */
    const char *a = "c-x.com";
    const char *b = "aox.com";
    assert_int_equal(router_djb2(a, strlen(a)), router_djb2(b, strlen(b)));

    const struct route_spec specs[] = {{"c-x.com", 10}, {"aox.com", 20}, {"*.c-x.com", 30}};
    router *r = router_build(specs, 3, NULL, NULL);
    assert_non_null(r);
    expect_route(r, "c-x.com", ROUTE_EXACT, 10);
    expect_route(r, "aox.com", ROUTE_EXACT, 20);
    /* Mismo hash que un wildcard existente, pero sufijo distinto. */
    expect_route(r, "w.aox.com", ROUTE_NONE, 0);
    expect_route(r, "w.c-x.com", ROUTE_WILDCARD, 30);
    router_unref(r);
}

static void test_many_routes(void **state) {
    (void)state;
    enum { N = 2000 };
    static char names[N][32];
    static struct route_spec specs[N];
    for (int i = 0; i < N; i++) {
        (void)snprintf(names[i], sizeof(names[i]), "host%d.example.net", i);
        specs[i] = (struct route_spec){names[i], (uint32_t)i};
    }
    router *r = router_build(specs, N, NULL, NULL);
    assert_non_null(r);
    for (int i = 0; i < N; i++) {
        expect_route(r, names[i], ROUTE_EXACT, (uint32_t)i);
    }
    expect_route(r, "host2000.example.net", ROUTE_NONE, 0);
    router_unref(r);
}

static void test_patterns_are_normalized(void **state) {
    (void)state;
    const struct route_spec specs[] = {{"Example.COM.", 1}, {"*.Sub.Example.com", 2}, {"[::1]", 3}};
    router *r = router_build(specs, 3, NULL, NULL);
    assert_non_null(r);
    expect_route(r, "example.com", ROUTE_EXACT, 1);
    expect_route(r, "a.sub.example.com", ROUTE_WILDCARD, 2);
    expect_route(r, "[::1]", ROUTE_EXACT, 3);
    router_unref(r);
}

static void test_host_from_parser_routes(void **state) {
    (void)state;
    router *r = build_sample(true);
    const char *reqs[] = {
        "GET / HTTP/1.1\r\nHost: WWW.Example.COM:8080\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: api.example.com.\r\n\r\n",
        "GET http://V2.Api.Example.com/x HTTP/1.1\r\nHost: example.com\r\n\r\n",
    };
    const uint32_t expected[] = {T_WILD_EXAMPLE, T_EXACT_API, T_WILD_API};
    for (size_t i = 0; i < 3; i++) {
        struct http_parser p;
        struct http_request req;
        enum http_error err;
        http_parser_init(&p, NULL);
        assert_int_equal(http_parse_request(&p, reqs[i], strlen(reqs[i]), &req, &err),
                         HTTP_PARSE_DONE);
        struct route_result res = router_lookup(r, req.host.name, req.host.len);
        assert_int_equal(res.target, expected[i]);
    }
    router_unref(r);
}

static void expect_build_error(const struct route_spec *specs, size_t n, size_t bad_index) {
    struct router_error err = {0};
    errno = 0;
    router *r = router_build(specs, n, NULL, &err);
    if (r != NULL) {
        router_unref(r);
        fail_msg("se esperaba error en la ruta %zu (\"%s\")", bad_index,
                 specs[bad_index].pattern ? specs[bad_index].pattern : "(null)");
    }
    assert_int_equal(errno, EINVAL);
    assert_int_equal(err.index, bad_index);
    assert_true(strlen(err.msg) > 0);
}

static void test_invalid_patterns_rejected(void **state) {
    (void)state;
    const char *bad[] = {
        "",
        "*",
        "*.",
        "**.example.com",
        "a.*.com",
        "*example.com",
        "exa mple",
        "a..b",
        "host:80",
        "*.[::1]",
        "[::1]:80",
        "-bad.com",
        "a_b@c",
        "*.*.b.com",
        "example.com/",
        NULL,
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        const struct route_spec specs[] = {{"ok.com", 1}, {bad[i], 2}};
        expect_build_error(specs, 2, 1);
    }
}

static void test_duplicates_rejected_after_normalization(void **state) {
    (void)state;
    const struct route_spec dup_exact[] = {{"example.com", 1}, {"EXAMPLE.com.", 2}};
    expect_build_error(dup_exact, 2, 1);
    const struct route_spec dup_wild[] = {{"*.example.com", 1}, {"*.Example.Com", 2}};
    expect_build_error(dup_wild, 2, 1);
    const struct route_spec dup_v6[] = {{"[::1]", 1}, {"[0::1]", 2}};
    expect_build_error(dup_v6, 2, 1);
    /* Exacto y wildcard del mismo dominio no son duplicados. */
    const struct route_spec both[] = {{"example.com", 1}, {"*.example.com", 2}};
    router *r = router_build(both, 2, NULL, NULL);
    assert_non_null(r);
    router_unref(r);
}

static void test_snapshot_references(void **state) {
    (void)state;
    router *old = build_sample(true);
    /* Una "conexión" toma referencia al snapshot vigente. */
    router *in_flight = router_ref(old);
    /* La recarga publica uno nuevo y suelta la referencia del publicador. */
    const struct route_spec specs[] = {{"example.com", 500}};
    router *current = router_build(specs, 1, NULL, NULL);
    assert_non_null(current);
    router_unref(old);

    /* La conexión en curso sigue viendo la configuración antigua... */
    expect_route(in_flight, "example.com", ROUTE_EXACT, T_EXACT_EXAMPLE);
    expect_route(in_flight, "other.org", ROUTE_DEFAULT, T_DEFAULT);
    /* ...y las nuevas, la nueva. */
    expect_route(current, "example.com", ROUTE_EXACT, 500);
    expect_route(current, "other.org", ROUTE_NONE, 0);

    /* La última referencia libera (ASan/Valgrind detectarían fuga o UAF). */
    router_unref(in_flight);
    router_unref(current);
    router_unref(NULL);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_precedence, setup, teardown),
        cmocka_unit_test_setup_teardown(test_no_match_without_default, setup, teardown),
        cmocka_unit_test_setup_teardown(test_domain_boundaries, setup, teardown),
        cmocka_unit_test_setup_teardown(test_djb2_reference_values, setup, teardown),
        cmocka_unit_test_setup_teardown(test_hash_collisions_resolved_by_key, setup, teardown),
        cmocka_unit_test_setup_teardown(test_many_routes, setup, teardown),
        cmocka_unit_test_setup_teardown(test_patterns_are_normalized, setup, teardown),
        cmocka_unit_test_setup_teardown(test_host_from_parser_routes, setup, teardown),
        cmocka_unit_test_setup_teardown(test_invalid_patterns_rejected, setup, teardown),
        cmocka_unit_test_setup_teardown(test_duplicates_rejected_after_normalization, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_snapshot_references, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
