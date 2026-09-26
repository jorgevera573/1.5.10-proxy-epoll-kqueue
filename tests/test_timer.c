/*
 * test_timer.c — orden de vencimiento, cancelación y reprogramación con
 * tiempo simulado (sin esperas), más dos pruebas de integración con io_loop
 * con márgenes amplios.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cmocka.h>

#include "io_event.h"
#include "timer.h"

#define TEST_TIMEOUT_S 5
#define MAX_LOG        64

struct fire_log {
    int ids[MAX_LOG];
    size_t n;
};

struct tctx {
    int id;
    struct fire_log *log;
    /* Acciones opcionales al disparar. */
    timer *cancel_other;
    timer *reschedule;
    uint64_t reschedule_at;
};

static void cb_log(timer_heap *heap, timer *t, void *ud) {
    (void)t;
    struct tctx *c = ud;
    if (c->log->n < MAX_LOG) {
        c->log->ids[c->log->n] = c->id;
    }
    c->log->n++; /* assert_log detecta un exceso */
    if (c->cancel_other != NULL) {
        timer_cancel(heap, c->cancel_other);
    }
    if (c->reschedule != NULL) {
        assert_int_equal(timer_schedule_at(heap, c->reschedule, c->reschedule_at), 0);
    }
}

struct fixture {
    timer_heap heap;
    struct fire_log log;
};

static int setup(void **state) {
    alarm(TEST_TIMEOUT_S);
    struct fixture *fx = calloc(1, sizeof(*fx));
    if (fx == NULL) {
        return -1;
    }
    timer_heap_init(&fx->heap);
    *state = fx;
    return 0;
}

static int teardown(void **state) {
    struct fixture *fx = *state;
    timer_heap_destroy(&fx->heap);
    free(fx);
    alarm(0);
    return 0;
}

static void assert_log(const struct fire_log *log, const int *expected, size_t n) {
    assert_int_equal(log->n, n);
    for (size_t i = 0; i < n; i++) {
        assert_int_equal(log->ids[i], expected[i]);
    }
}

static void test_fires_in_deadline_order(void **state) {
    struct fixture *fx = *state;
    timer t[5];
    struct tctx c[5];
    const uint64_t deadlines[5] = {50, 10, 40, 20, 30};
    for (int i = 0; i < 5; i++) {
        c[i] = (struct tctx){.id = i, .log = &fx->log};
        timer_init(&t[i], cb_log, &c[i]);
        assert_int_equal(timer_schedule_at(&fx->heap, &t[i], deadlines[i]), 0);
    }
    assert_int_equal(timer_heap_size(&fx->heap), 5);

    assert_int_equal(timer_heap_run_expired(&fx->heap, 9), 0);
    assert_int_equal(timer_heap_run_expired(&fx->heap, 25), 2);
    assert_int_equal(timer_heap_run_expired(&fx->heap, 1000), 3);
    const int expected[] = {1, 3, 4, 2, 0};
    assert_log(&fx->log, expected, 5);
    assert_int_equal(timer_heap_size(&fx->heap), 0);
    for (int i = 0; i < 5; i++) {
        assert_false(timer_is_active(&t[i]));
    }
}

static void test_equal_deadlines_fire_fifo(void **state) {
    struct fixture *fx = *state;
    timer t[4];
    struct tctx c[4];
    for (int i = 0; i < 4; i++) {
        c[i] = (struct tctx){.id = i, .log = &fx->log};
        timer_init(&t[i], cb_log, &c[i]);
        assert_int_equal(timer_schedule_at(&fx->heap, &t[i], 100), 0);
    }
    assert_int_equal(timer_heap_run_expired(&fx->heap, 100), 4);
    const int expected[] = {0, 1, 2, 3};
    assert_log(&fx->log, expected, 4);
}

static void test_cancel_prevents_firing_and_is_idempotent(void **state) {
    struct fixture *fx = *state;
    timer a;
    timer b;
    struct tctx ca = {.id = 1, .log = &fx->log};
    struct tctx cb = {.id = 2, .log = &fx->log};
    timer_init(&a, cb_log, &ca);
    timer_init(&b, cb_log, &cb);

    timer_cancel(&fx->heap, &a); /* nunca programado: no hace nada */
    assert_int_equal(timer_schedule_at(&fx->heap, &a, 10), 0);
    assert_int_equal(timer_schedule_at(&fx->heap, &b, 20), 0);
    timer_cancel(&fx->heap, &a);
    timer_cancel(&fx->heap, &a);
    assert_false(timer_is_active(&a));
    assert_int_equal(timer_heap_size(&fx->heap), 1);

    assert_int_equal(timer_heap_run_expired(&fx->heap, 100), 1);
    const int expected[] = {2};
    assert_log(&fx->log, expected, 1);
}

static void test_reschedule_moves_timer(void **state) {
    struct fixture *fx = *state;
    timer a;
    timer b;
    struct tctx ca = {.id = 1, .log = &fx->log};
    struct tctx cb = {.id = 2, .log = &fx->log};
    timer_init(&a, cb_log, &ca);
    timer_init(&b, cb_log, &cb);
    assert_int_equal(timer_schedule_at(&fx->heap, &a, 10), 0);
    assert_int_equal(timer_schedule_at(&fx->heap, &b, 20), 0);

    /* Retrasar a: ahora b va primero; no queda duplicado en el heap. */
    assert_int_equal(timer_schedule_at(&fx->heap, &a, 30), 0);
    assert_int_equal(timer_heap_size(&fx->heap), 2);
    assert_int_equal(timer_heap_run_expired(&fx->heap, 25), 1);
    /* Adelantar b tras dispararlo y reprogramarlo. */
    assert_int_equal(timer_schedule_at(&fx->heap, &b, 100), 0);
    assert_int_equal(timer_schedule_at(&fx->heap, &b, 5), 0);
    assert_int_equal(timer_heap_run_expired(&fx->heap, 30), 2);
    const int expected[] = {2, 2, 1};
    assert_log(&fx->log, expected, 3);
}

static void test_callback_cancels_other_expired_timer(void **state) {
    struct fixture *fx = *state;
    timer a;
    timer b;
    struct tctx ca = {.id = 1, .log = &fx->log, .cancel_other = &b};
    struct tctx cb = {.id = 2, .log = &fx->log};
    timer_init(&a, cb_log, &ca);
    timer_init(&b, cb_log, &cb);
    assert_int_equal(timer_schedule_at(&fx->heap, &a, 10), 0);
    assert_int_equal(timer_schedule_at(&fx->heap, &b, 11), 0);

    /* Ambos vencidos a la vez; a cancela b antes de que dispare. */
    assert_int_equal(timer_heap_run_expired(&fx->heap, 50), 1);
    const int expected[] = {1};
    assert_log(&fx->log, expected, 1);
    assert_false(timer_is_active(&b));
}

static void test_self_reschedule_does_not_loop(void **state) {
    struct fixture *fx = *state;
    timer a;
    struct tctx ca = {.id = 7, .log = &fx->log, .reschedule = &a, .reschedule_at = 0};
    timer_init(&a, cb_log, &ca);
    assert_int_equal(timer_schedule_at(&fx->heap, &a, 10), 0);

    /* Se reprograma en el pasado dentro de su callback: no dispara otra vez
     * en la misma llamada, sino en la siguiente. */
    assert_int_equal(timer_heap_run_expired(&fx->heap, 10), 1);
    assert_true(timer_is_active(&a));
    assert_int_equal(timer_heap_next_timeout(&fx->heap, 10), 0);
    ca.reschedule = NULL;
    assert_int_equal(timer_heap_run_expired(&fx->heap, 10), 1);
    assert_int_equal(fx->log.n, 2);
}

static void test_next_timeout_values(void **state) {
    struct fixture *fx = *state;
    timer a;
    struct tctx ca = {.id = 1, .log = &fx->log};
    timer_init(&a, cb_log, &ca);

    assert_int_equal(timer_heap_next_timeout(&fx->heap, 0), -1);
    assert_int_equal(timer_schedule_in(&fx->heap, &a, 1000, 250), 0);
    assert_int_equal(timer_heap_next_timeout(&fx->heap, 1000), 250);
    assert_int_equal(timer_heap_next_timeout(&fx->heap, 1249), 1);
    assert_int_equal(timer_heap_next_timeout(&fx->heap, 1250), 0);
    assert_int_equal(timer_heap_next_timeout(&fx->heap, 5000), 0);

    /* Plazos enormes se saturan (INT_MAX y UINT64_MAX sin desbordar). */
    assert_int_equal(timer_schedule_in(&fx->heap, &a, 0, (uint64_t)INT_MAX * 10), 0);
    assert_int_equal(timer_heap_next_timeout(&fx->heap, 0), INT_MAX);
    assert_int_equal(timer_schedule_in(&fx->heap, &a, UINT64_MAX - 5, 100), 0);
    assert_int_equal(timer_heap_next_timeout(&fx->heap, UINT64_MAX - 5), 5);
    timer_cancel(&fx->heap, &a);
}

static void test_many_timers_heap_order(void **state) {
    struct fixture *fx = *state;
    enum { N = 1000 };
    timer *t = calloc(N, sizeof(*t));
    struct tctx *c = calloc(N, sizeof(*c));
    assert_non_null(t);
    assert_non_null(c);
    /* Plazos pseudoaleatorios deterministas; se cancela 1 de cada 7. */
    uint32_t x = 12345;
    for (int i = 0; i < N; i++) {
        x = x * 1103515245u + 12345u;
        c[i] = (struct tctx){.id = (int)(x % 5000)};
        timer_init(&t[i], NULL, &c[i]);
        assert_int_equal(timer_schedule_at(&fx->heap, &t[i], x % 5000), 0);
    }
    for (int i = 0; i < N; i += 7) {
        timer_cancel(&fx->heap, &t[i]);
    }
    size_t expected = timer_heap_size(&fx->heap);
    /* Extrae por el heap y comprueba orden no decreciente. */
    uint64_t last = 0;
    size_t popped = 0;
    for (uint64_t now = 0; now < 5000; now++) {
        while (timer_heap_next_timeout(&fx->heap, now) == 0) {
            timer *top = fx->heap.items[0];
            assert_true(top->deadline_ms >= last);
            last = top->deadline_ms;
            timer_cancel(&fx->heap, top);
            popped++;
        }
    }
    assert_int_equal(popped, expected);
    free(t);
    free(c);
}

/* --- Integración con io_loop --------------------------------------------- */

static void cb_count(timer_heap *heap, timer *t, void *ud) {
    (void)heap;
    (void)t;
    (*(int *)ud)++;
}

static void test_loop_wakes_for_timer_without_io(void **state) {
    struct fixture *fx = *state;
    io_loop *loop = io_loop_create(8);
    assert_non_null(loop);
    int fired = 0;
    timer a;
    timer_init(&a, cb_count, &fired);
    uint64_t t0 = timer_now_ms();
    assert_int_equal(timer_schedule_in(&fx->heap, &a, t0, 20), 0);

    /* Sin E/S registrada: solo el plazo del timer puede despertar la espera. */
    while (fired == 0) {
        assert_true(timer_loop_run_once(loop, &fx->heap) >= 0);
    }
    uint64_t elapsed = timer_now_ms() - t0;
    assert_true(elapsed >= 20);
    assert_true(elapsed < 2000); /* margen amplio: solo detecta esperas sin fin */
    io_loop_destroy(loop);
}

static void cb_io(io_loop *loop, int fd, uint32_t events, void *ud) {
    (void)loop;
    (void)events;
    char buf[8];
    while (read(fd, buf, sizeof(buf)) > 0) {
    }
    (*(int *)ud)++;
}

static void test_io_is_not_delayed_by_distant_timer(void **state) {
    struct fixture *fx = *state;
    io_loop *loop = io_loop_create(8);
    assert_non_null(loop);
    int sv[2];
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    assert_int_equal(fcntl(sv[0], F_SETFL, O_NONBLOCK), 0);

    int io_calls = 0;
    int fired = 0;
    timer far;
    timer_init(&far, cb_count, &fired);
    /* Un timer a 1 h no debe retrasar una E/S ya lista. */
    assert_int_equal(timer_schedule_in(&fx->heap, &far, timer_now_ms(), (uint64_t)3600 * 1000), 0);
    assert_int_equal(io_loop_add(loop, sv[0], IO_READ, cb_io, &io_calls), 0);
    assert_int_equal(write(sv[1], "x", 1), 1);

    assert_int_equal(timer_loop_run_once(loop, &fx->heap), 1);
    assert_int_equal(io_calls, 1);
    assert_int_equal(fired, 0);

    timer_cancel(&fx->heap, &far);
    assert_int_equal(io_loop_del(loop, sv[0]), 0);
    close(sv[0]);
    close(sv[1]);
    io_loop_destroy(loop);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_fires_in_deadline_order, setup, teardown),
        cmocka_unit_test_setup_teardown(test_equal_deadlines_fire_fifo, setup, teardown),
        cmocka_unit_test_setup_teardown(test_cancel_prevents_firing_and_is_idempotent, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_reschedule_moves_timer, setup, teardown),
        cmocka_unit_test_setup_teardown(test_callback_cancels_other_expired_timer, setup, teardown),
        cmocka_unit_test_setup_teardown(test_self_reschedule_does_not_loop, setup, teardown),
        cmocka_unit_test_setup_teardown(test_next_timeout_values, setup, teardown),
        cmocka_unit_test_setup_teardown(test_many_timers_heap_order, setup, teardown),
        cmocka_unit_test_setup_teardown(test_loop_wakes_for_timer_without_io, setup, teardown),
        cmocka_unit_test_setup_teardown(test_io_is_not_delayed_by_distant_timer, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
