/*
 * test_backend_pool.c — algoritmos de selección, elegibilidad, salud activa y
 * pasiva, herencia entre generaciones y contabilidad.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>

#include "backend_pool.h"

#define TEST_TIMEOUT_S 5

/* Índices de pool en CFG. */
enum { P_RR, P_LIM, P_W31, P_W511, P_LC, P_HC, P_NOHC, P_WHC };

#define HC_POOL                                                                                    \
    "[[pool]]\nname = \"hc\"\n"                                                                    \
    "[pool.health]\ntype = \"http\"\nfall = 2\nrise = 2\npassive_fall = 2\n"                       \
    "interval_ms = 1000\ntimeout_ms = 500\n"                                                       \
    "[[pool.backend]]\naddress = \"127.0.0.1:20\"\n"                                               \
    "[[pool.backend]]\naddress = \"127.0.0.1:21\"\n"

static const char *const CFG =
    "[[frontend]]\nlisten = \"127.0.0.1:7777\"\n"
    "[[pool]]\nname = \"rr\"\n"
    "[[pool.backend]]\naddress = \"127.0.0.1:1\"\n"
    "[[pool.backend]]\naddress = \"127.0.0.1:2\"\n"
    "[[pool.backend]]\naddress = \"127.0.0.1:3\"\n"
    "[[pool]]\nname = \"lim\"\n"
    "[[pool.backend]]\naddress = \"127.0.0.1:4\"\nmax_conns = 1\n"
    "[[pool.backend]]\naddress = \"127.0.0.1:5\"\nmax_conns = 2\n"
    "[[pool]]\nname = \"w31\"\nalgorithm = \"weighted\"\n"
    "[[pool.backend]]\naddress = \"127.0.0.1:6\"\nweight = 3\n"
    "[[pool.backend]]\naddress = \"127.0.0.1:7\"\nweight = 1\n"
    "[[pool]]\nname = \"w511\"\nalgorithm = \"weighted\"\n"
    "[[pool.backend]]\naddress = \"127.0.0.1:8\"\nweight = 5\n"
    "[[pool.backend]]\naddress = \"127.0.0.1:9\"\nweight = 1\n"
    "[[pool.backend]]\naddress = \"127.0.0.1:10\"\nweight = 1\n"
    "[[pool]]\nname = \"lc\"\nalgorithm = \"least_conn\"\n"
    "[[pool.backend]]\naddress = \"127.0.0.1:11\"\n"
    "[[pool.backend]]\naddress = \"127.0.0.1:12\"\n"
    "[[pool.backend]]\naddress = \"127.0.0.1:13\"\n" HC_POOL "[[pool]]\nname = \"nohc\"\n"
    "[[pool.backend]]\naddress = \"127.0.0.1:22\"\n"
    "[[pool]]\nname = \"whc\"\nalgorithm = \"weighted\"\n"
    "[pool.health]\ntype = \"tcp\"\nfall = 1\nrise = 1\n"
    "[[pool.backend]]\naddress = \"127.0.0.1:30\"\nweight = 3\n"
    "[[pool.backend]]\naddress = \"127.0.0.1:31\"\nweight = 1\n"
    "[routing]\ndefault_pool = \"rr\"\n";

struct fx {
    struct config *cfg;
    backend_registry *reg;
    backend_pools *bp;
    size_t live0;
};

static struct config *load(const char *text) {
    char err[CONFIG_ERR_LEN];
    struct config *cfg = config_load_string(text, "bp.toml", err, sizeof(err));
    if (cfg == NULL) {
        fail_msg("%s", err);
    }
    return cfg;
}

static int setup(void **state) {
    alarm(TEST_TIMEOUT_S);
    static struct fx fx;
    fx.live0 = backend_pools_live();
    fx.cfg = load(CFG);
    fx.reg = backend_registry_create();
    fx.bp = backend_pools_create(fx.cfg, 1, fx.reg);
    if (fx.bp == NULL) {
        return -1;
    }
    *state = &fx;
    return 0;
}

static int teardown(void **state) {
    struct fx *fx = *state;
    backend_pools_unref(fx->bp);
    config_unref(fx->cfg);
    assert_int_equal(backend_pools_live(), fx->live0);
    assert_int_equal(backend_registry_size(fx->reg), 0); /* sin entradas huérfanas */
    backend_registry_destroy(fx->reg);
    alarm(0);
    return 0;
}

/* Elige y libera: devuelve el índice elegido. */
static uint32_t pick_release(backend_pools *bp, uint32_t pool) {
    struct bp_choice ch;
    assert_int_equal(backend_pool_pick(bp, pool, &ch), BP_OK);
    assert_int_equal(ch.pool, pool);
    backend_pool_release(bp, &ch);
    return ch.backend;
}

static void expect_sequence(backend_pools *bp, uint32_t pool, const uint32_t *seq, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t got = pick_release(bp, pool);
        if (got != seq[i]) {
            fail_msg("elección %zu: esperado %u, obtenido %u", i, seq[i], got);
        }
    }
}

/* ------------------------------------------------------------------------- */

static void test_round_robin_cycles(void **state) {
    struct fx *fx = *state;
    const uint32_t seq[] = {0, 1, 2, 0, 1, 2, 0};
    expect_sequence(fx->bp, P_RR, seq, 7);
    assert_int_equal(backend_pool_state(fx->bp, P_RR, 0)->selected, 3);
    assert_int_equal(backend_pools_active_total(fx->bp), 0);
}

static void test_active_accounting(void **state) {
    struct fx *fx = *state;
    struct bp_choice held[5];
    for (int i = 0; i < 5; i++) {
        assert_int_equal(backend_pool_pick(fx->bp, P_RR, &held[i]), BP_OK);
    }
    assert_int_equal(backend_pools_active_total(fx->bp), 5);
    assert_int_equal(backend_pool_state(fx->bp, P_RR, 0)->active, 2);
    assert_int_equal(backend_pool_state(fx->bp, P_RR, 2)->active, 1);
    for (int i = 0; i < 5; i++) {
        backend_pool_release(fx->bp, &held[i]);
    }
    assert_int_equal(backend_pools_active_total(fx->bp), 0);
}

static void test_max_conns_exclusion_and_none_eligible(void **state) {
    struct fx *fx = *state;
    struct bp_choice a;
    struct bp_choice b;
    struct bp_choice c;
    struct bp_choice d;
    assert_int_equal(backend_pool_pick(fx->bp, P_LIM, &a), BP_OK);
    assert_int_equal(a.backend, 0);
    assert_int_equal(backend_pool_pick(fx->bp, P_LIM, &b), BP_OK);
    assert_int_equal(b.backend, 1);
    assert_int_equal(backend_pool_pick(fx->bp, P_LIM, &c), BP_OK);
    assert_int_equal(c.backend, 1); /* el 0 está lleno (max 1) */
    assert_int_equal(backend_pool_pick(fx->bp, P_LIM, &d), BP_NONE_ELIGIBLE);
    assert_int_equal(backend_pools_active_total(fx->bp), 3);
    backend_pool_release(fx->bp, &a);
    assert_int_equal(backend_pool_pick(fx->bp, P_LIM, &d), BP_OK);
    assert_int_equal(d.backend, 0);
    backend_pool_release(fx->bp, &b);
    backend_pool_release(fx->bp, &c);
    backend_pool_release(fx->bp, &d);
    assert_int_equal(backend_pools_active_total(fx->bp), 0);
}

static void test_weighted_smooth_sequence(void **state) {
    struct fx *fx = *state;
    /* 3:1 -> A A B A, periódico. */
    const uint32_t s31[] = {0, 0, 1, 0, 0, 0, 1, 0};
    expect_sequence(fx->bp, P_W31, s31, 8);
    /* 5:1:1 -> a a b a c a a (secuencia de nginx). */
    const uint32_t s511[] = {0, 0, 1, 0, 2, 0, 0, 0, 0, 1, 0, 2, 0, 0};
    expect_sequence(fx->bp, P_W511, s511, 14);
    assert_int_equal(backend_pool_state(fx->bp, P_W511, 0)->selected, 10);
    assert_int_equal(backend_pool_state(fx->bp, P_W511, 1)->selected, 2);
    assert_int_equal(backend_pool_state(fx->bp, P_W511, 2)->selected, 2);
}

static void test_weighted_distribution_and_exclusion(void **state) {
    struct fx *fx = *state;
    uint32_t count[2] = {0, 0};
    for (int i = 0; i < 400; i++) {
        count[pick_release(fx->bp, P_W31)]++;
    }
    assert_int_equal(count[0], 300);
    assert_int_equal(count[1], 100);

    /* Con el de peso 3 caído, todo va al de peso 1; al volver, 3:1 otra vez. */
    assert_int_equal(backend_pool_probe_result(fx->bp, P_WHC, 0, false), BP_WENT_DOWN);
    for (int i = 0; i < 8; i++) {
        assert_int_equal(pick_release(fx->bp, P_WHC), 1);
    }
    assert_int_equal(backend_pool_probe_result(fx->bp, P_WHC, 0, true), BP_WENT_UP);
    count[0] = count[1] = 0;
    for (int i = 0; i < 40; i++) {
        count[pick_release(fx->bp, P_WHC)]++;
    }
    assert_int_equal(count[0], 30);
    assert_int_equal(count[1], 10);
}

static void test_least_conn_uses_active_counts(void **state) {
    struct fx *fx = *state;
    struct bp_choice h1;
    struct bp_choice h2;
    /* Empates: rotan desde el cursor. */
    assert_int_equal(backend_pool_pick(fx->bp, P_LC, &h1), BP_OK);
    assert_int_equal(h1.backend, 0);
    assert_int_equal(backend_pool_pick(fx->bp, P_LC, &h2), BP_OK);
    assert_int_equal(h2.backend, 1);
    /* 0 y 1 ocupados: todas las elecciones rápidas van al 2. */
    for (int i = 0; i < 5; i++) {
        assert_int_equal(pick_release(fx->bp, P_LC), 2);
    }
    backend_pool_release(fx->bp, &h1);
    /* Ahora 0 y 2 empatan a 0: gana el primero desde el cursor (tras el 2). */
    assert_int_equal(pick_release(fx->bp, P_LC), 0);
    backend_pool_release(fx->bp, &h2);
    /* Todos a 0: el cursor sigue rotando. */
    const uint32_t seq[] = {1, 2, 0, 1};
    expect_sequence(fx->bp, P_LC, seq, 4);
}

static void test_active_health_fall_and_rise(void **state) {
    struct fx *fx = *state;
    assert_int_equal(backend_pool_probe_result(fx->bp, P_HC, 0, false), BP_NO_CHANGE);
    assert_true(backend_state_eligible(fx->bp, P_HC, 0));
    assert_int_equal(backend_pool_probe_result(fx->bp, P_HC, 0, false), BP_WENT_DOWN);
    assert_int_equal(backend_pool_state(fx->bp, P_HC, 0)->health, BP_DOWN_ACTIVE);
    /* Excluido: todas las elecciones van al 1. */
    for (int i = 0; i < 4; i++) {
        assert_int_equal(pick_release(fx->bp, P_HC), 1);
    }
    /* Un éxito no basta (rise = 2); un fallo reinicia la cuenta. */
    assert_int_equal(backend_pool_probe_result(fx->bp, P_HC, 0, true), BP_NO_CHANGE);
    assert_int_equal(backend_pool_probe_result(fx->bp, P_HC, 0, false), BP_NO_CHANGE);
    assert_int_equal(backend_pool_probe_result(fx->bp, P_HC, 0, true), BP_NO_CHANGE);
    assert_int_equal(backend_pool_probe_result(fx->bp, P_HC, 0, true), BP_WENT_UP);
    assert_true(backend_state_eligible(fx->bp, P_HC, 0));
}

static void test_all_down_is_none_eligible(void **state) {
    struct fx *fx = *state;
    for (uint32_t b = 0; b < 2; b++) {
        (void)backend_pool_probe_result(fx->bp, P_HC, b, false);
        (void)backend_pool_probe_result(fx->bp, P_HC, b, false);
    }
    struct bp_choice ch;
    assert_int_equal(backend_pool_pick(fx->bp, P_HC, &ch), BP_NONE_ELIGIBLE);
    assert_int_equal(backend_pools_active_total(fx->bp), 0);
}

static void test_passive_health(void **state) {
    struct fx *fx = *state;
    struct bp_choice ch = {.pool = P_HC, .backend = 0};
    assert_int_equal(backend_pool_connect_failed(fx->bp, &ch), BP_NO_CHANGE);
    backend_pool_connect_ok(fx->bp, &ch); /* reinicia la cuenta */
    assert_int_equal(backend_pool_connect_failed(fx->bp, &ch), BP_NO_CHANGE);
    assert_int_equal(backend_pool_connect_failed(fx->bp, &ch), BP_WENT_DOWN);
    assert_int_equal(backend_pool_state(fx->bp, P_HC, 0)->health, BP_DOWN_PASSIVE);
    assert_int_equal(backend_pool_state(fx->bp, P_HC, 0)->connect_failures, 3);
    /* Solo las sondas lo devuelven (rise = 2). */
    assert_int_equal(backend_pool_probe_result(fx->bp, P_HC, 0, true), BP_NO_CHANGE);
    assert_int_equal(backend_pool_probe_result(fx->bp, P_HC, 0, true), BP_WENT_UP);

    /* Sin [pool.health] no hay exclusión (nada podría readmitirlo). */
    struct bp_choice n = {.pool = P_NOHC, .backend = 0};
    for (int i = 0; i < 10; i++) {
        assert_int_equal(backend_pool_connect_failed(fx->bp, &n), BP_NO_CHANGE);
    }
    assert_true(backend_state_eligible(fx->bp, P_NOHC, 0));
    assert_int_equal(backend_pool_probe_result(fx->bp, P_NOHC, 0, false), BP_NO_CHANGE);
}

/* Nueva generación a partir de un texto; la referencia del texto se suelta. */
static backend_pools *generation(struct fx *fx, const char *text, uint64_t id) {
    struct config *c = load(text);
    backend_pools *g = backend_pools_create(c, id, fx->reg);
    config_unref(c);
    assert_non_null(g);
    return g;
}

#define FE1 "[[frontend]]\nlisten = \"127.0.0.1:7777\"\n"

static void test_generation_shares_health_by_identity(void **state) {
    struct fx *fx = *state;
    (void)backend_pool_probe_result(fx->bp, P_HC, 0, false);
    (void)backend_pool_probe_result(fx->bp, P_HC, 0, false); /* hc/:20 caído */
    struct bp_choice held;
    assert_int_equal(backend_pool_pick(fx->bp, P_HC, &held), BP_OK); /* :21 activo */

    /* "hc" cambia de posición y gana un backend; "otro" usa la misma
     * dirección :20 pero es otro backend lógico (otro pool). */
    size_t live = backend_pools_live();
    backend_pools *g2 = generation(fx,
                                   FE1 "[[pool]]\nname = \"otro\"\n"
                                       "[[pool.backend]]\naddress = \"127.0.0.1:20\"\n"
                                       "[[pool]]\nname = \"hc\"\n"
                                       "[pool.health]\ntype = \"tcp\"\n"
                                       "[[pool.backend]]\naddress = \"127.0.0.1:23\"\n"
                                       "[[pool.backend]]\naddress = \"127.0.0.1:20\"\n"
                                       "[[pool.backend]]\naddress = \"127.0.0.1:21\"\n"
                                       "[routing]\ndefault_pool = \"hc\"\n",
                                   2);
    assert_int_equal(backend_pools_live(), live + 1);
    assert_int_equal(backend_pool_state(g2, 1, 1)->health, BP_DOWN_ACTIVE); /* mismo lógico */
    assert_int_equal(backend_pool_state(g2, 1, 0)->health, BP_UP);          /* nuevo */
    assert_int_equal(backend_pool_state(g2, 0, 0)->health, BP_UP); /* otro pool: otro lógico */
    /* Las activas son del backend lógico: la de la generación 1 cuenta en la 2. */
    assert_int_equal(backend_pool_state(g2, 1, 2)->active, 1);
    backend_pool_release(fx->bp, &held); /* se libera con la generación del pick */
    assert_int_equal(backend_pool_state(g2, 1, 2)->active, 0);

    backend_pools *conn_ref = backend_pools_ref(g2);
    backend_pools_unref(g2);
    assert_int_equal(backend_pools_live(), live + 1);
    backend_pools_unref(conn_ref);
    assert_int_equal(backend_pools_live(), live);
}

#define LIMITED(max)                                                                               \
    FE1 "[[pool]]\nname = \"lim\"\n"                                                               \
        "[[pool.backend]]\naddress = \"127.0.0.1:4\"\nmax_conns = " #max "\n"                      \
        "[[pool.backend]]\naddress = \"127.0.0.1:5\"\nmax_conns = 2\n"                             \
        "[routing]\ndefault_pool = \"lim\"\n"

static void test_max_conns_counts_previous_generations(void **state) {
    struct fx *fx = *state;
    struct bp_choice held;
    assert_int_equal(backend_pool_pick(fx->bp, P_LIM, &held), BP_OK);
    assert_int_equal(held.backend, 0); /* :4 con max_conns = 1, ocupado */
    backend_pools *g2 = generation(fx, LIMITED(1), 2);
    /* La plaza sigue ocupada tras la recarga: :4 no es elegible en la 2. */
    assert_false(backend_state_eligible(g2, 0, 0));
    for (int i = 0; i < 4; i++) {
        assert_int_equal(pick_release(g2, 0), 1);
    }
    backend_pool_release(fx->bp, &held);
    assert_true(backend_state_eligible(g2, 0, 0));
    backend_pools_unref(g2);
}

static void test_reducing_max_conns_blocks_until_below(void **state) {
    struct fx *fx = *state;
    /* Generación con :4 hasta 3 conexiones; se ocupan 3. */
    backend_pools *g1 = generation(fx, LIMITED(3), 2);
    struct bp_choice h[3];
    for (int i = 0; i < 3; i++) {
        h[i].pool = 0;
        h[i].backend = 0;
    }
    /* Forzar las 3 en :4: con :5 saturado (2) los picks van a :4. */
    struct bp_choice s5[8];
    size_t n4 = 0;
    size_t n5 = 0;
    while (n4 < 3) {
        struct bp_choice ch;
        assert_int_equal(backend_pool_pick(g1, 0, &ch), BP_OK);
        if (ch.backend == 0) {
            h[n4++] = ch;
        } else if (n5 < 8) {
            s5[n5++] = ch;
        } else {
            fail_msg("%s", "demasiadas elecciones de :5");
            return;
        }
    }
    assert_int_equal(backend_pool_state(g1, 0, 0)->active, 3);
    /* Recarga que baja max_conns de :4 a 1: las 3 siguen (nadie las corta)... */
    backend_pools *g2 = generation(fx, LIMITED(1), 3);
    assert_int_equal(backend_pool_state(g2, 0, 0)->active, 3);
    /* ...pero no se asigna ninguna nueva a :4 hasta bajar de 1. */
    backend_pool_release(g1, &h[0]);
    backend_pool_release(g1, &h[1]);
    assert_false(backend_state_eligible(g2, 0, 0)); /* 1 activa, límite 1 */
    backend_pool_release(g1, &h[2]);
    assert_true(backend_state_eligible(g2, 0, 0));
    for (size_t i = 0; i < n5; i++) {
        backend_pool_release(g1, &s5[i]);
    }
    backend_pools_unref(g1);
    backend_pools_unref(g2);
}

static void test_least_conn_counts_previous_generations(void **state) {
    struct fx *fx = *state;
    struct bp_choice held;
    assert_int_equal(backend_pool_pick(fx->bp, P_LC, &held), BP_OK);
    assert_int_equal(held.backend, 0); /* lc/:11 retenida en la generación 1 */
    backend_pools *g2 = generation(fx,
                                   FE1 "[[pool]]\nname = \"lc\"\nalgorithm = \"least_conn\"\n"
                                       "[[pool.backend]]\naddress = \"127.0.0.1:11\"\n"
                                       "[[pool.backend]]\naddress = \"127.0.0.1:12\"\n"
                                       "[routing]\ndefault_pool = \"lc\"\n",
                                   2);
    /* Con :11 ocupado desde la generación 1, la 2 elige siempre :12. */
    for (int i = 0; i < 4; i++) {
        assert_int_equal(pick_release(g2, 0), 1);
    }
    backend_pool_release(fx->bp, &held);
    uint32_t seen[2] = {0, 0};
    for (int i = 0; i < 4; i++) {
        seen[pick_release(g2, 0)]++;
    }
    assert_int_equal(seen[0], 2); /* libres: empates rotan */
    backend_pools_unref(g2);
}

static void test_remove_and_reintroduce_keeps_accounting(void **state) {
    struct fx *fx = *state;
    struct bp_choice held;
    assert_int_equal(backend_pool_pick(fx->bp, P_LC, &held), BP_OK);
    assert_int_equal(held.backend, 0); /* lc/:11 */
    size_t entries = backend_registry_size(fx->reg);

    /* La generación 2 elimina :11; la 1 sigue viva (conexión en curso). */
    backend_pools *g2 = generation(fx,
                                   FE1 "[[pool]]\nname = \"lc\"\nalgorithm = \"least_conn\"\n"
                                       "[[pool.backend]]\naddress = \"127.0.0.1:12\"\n"
                                       "[routing]\ndefault_pool = \"lc\"\n",
                                   2);
    /* Generación 3: lo reintroduce con max_conns = 1 -> misma entrada, con su
     * conexión activa: no es elegible hasta que termine. */
    backend_pools *g3 = generation(fx,
                                   FE1 "[[pool]]\nname = \"lc\"\nalgorithm = \"least_conn\"\n"
                                       "[[pool.backend]]\naddress = \"127.0.0.1:11\"\n"
                                       "max_conns = 1\n"
                                       "[routing]\ndefault_pool = \"lc\"\n",
                                   3);
    assert_int_equal(backend_pool_state(g3, 0, 0)->active, 1);
    struct bp_choice ch;
    assert_int_equal(backend_pool_pick(g3, 0, &ch), BP_NONE_ELIGIBLE);
    backend_pool_release(fx->bp, &held);
    assert_int_equal(backend_pool_pick(g3, 0, &ch), BP_OK);
    backend_pool_release(g3, &ch);
    backend_pools_unref(g2);
    backend_pools_unref(g3);
    assert_int_equal(backend_registry_size(fx->reg), entries);

    /* Si nadie retiene la entrada, se libera y la reintroducción empieza de
     * cero. */
    backend_pools_unref(fx->bp);
    fx->bp = generation(fx,
                        FE1 "[[pool]]\nname = \"solo\"\n"
                            "[[pool.backend]]\naddress = \"127.0.0.1:99\"\n"
                            "[routing]\ndefault_pool = \"solo\"\n",
                        4);
    assert_int_equal(backend_registry_size(fx->reg), 1);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_round_robin_cycles, setup, teardown),
        cmocka_unit_test_setup_teardown(test_active_accounting, setup, teardown),
        cmocka_unit_test_setup_teardown(test_max_conns_exclusion_and_none_eligible, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_weighted_smooth_sequence, setup, teardown),
        cmocka_unit_test_setup_teardown(test_weighted_distribution_and_exclusion, setup, teardown),
        cmocka_unit_test_setup_teardown(test_least_conn_uses_active_counts, setup, teardown),
        cmocka_unit_test_setup_teardown(test_active_health_fall_and_rise, setup, teardown),
        cmocka_unit_test_setup_teardown(test_all_down_is_none_eligible, setup, teardown),
        cmocka_unit_test_setup_teardown(test_passive_health, setup, teardown),
        cmocka_unit_test_setup_teardown(test_generation_shares_health_by_identity, setup, teardown),
        cmocka_unit_test_setup_teardown(test_max_conns_counts_previous_generations, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_reducing_max_conns_blocks_until_below, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_least_conn_counts_previous_generations, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_remove_and_reintroduce_keeps_accounting, setup,
                                        teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
