/*
 * test_io_event.c — pruebas de comportamiento de io_event con sockets reales.
 *
 * Cada prueba usa un socketpair AF_UNIX no bloqueante y el backend real de la
 * plataforma (epoll o kqueue). Ninguna prueba puede bloquearse
 * indefinidamente: el setup arma alarm(TEST_TIMEOUT_S), cuya acción por
 * defecto termina el proceso y hace fallar la suite, y Meson aplica además su
 * propio timeout.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cmocka.h>

#include "io_event.h"

#define TEST_TIMEOUT_S 5
#define QUIET_MS       50 /* espera para afirmar que NO llega un evento */

/* ------------------------------------------------------------------------- */
/* Utilidades                                                                */
/* ------------------------------------------------------------------------- */

struct fixture {
    io_loop *loop;
    int sv[2]; /* sv[0] se registra en el bucle; sv[1] es el par remoto */
};

struct recorder {
    int calls;
    int fd;
    uint32_t events;
    uint32_t events_union;
};

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL);
    assert_true(fl >= 0);
    assert_int_equal(fcntl(fd, F_SETFL, fl | O_NONBLOCK), 0);
}

static void make_pair(int sv[2]) {
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    set_nonblock(sv[0]);
    set_nonblock(sv[1]);
}

static void close_if_open(int *fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void write_all(int fd, const void *buf, size_t len) {
    ssize_t n = write(fd, buf, len);
    assert_int_equal(n, (ssize_t)len);
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(long ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
    }
}

static int setup(void **state) {
    alarm(TEST_TIMEOUT_S);
    struct fixture *fx = calloc(1, sizeof(*fx));
    if (fx == NULL) {
        return -1;
    }
    fx->loop = io_loop_create(16);
    if (fx->loop == NULL) {
        free(fx);
        return -1;
    }
    make_pair(fx->sv);
    *state = fx;
    return 0;
}

static int teardown(void **state) {
    struct fixture *fx = *state;
    io_loop_destroy(fx->loop);
    close_if_open(&fx->sv[0]);
    close_if_open(&fx->sv[1]);
    free(fx);
    alarm(0);
    return 0;
}

static void cb_record(io_loop *loop, int fd, uint32_t events, void *ud) {
    (void)loop;
    struct recorder *r = ud;
    r->calls++;
    r->fd = fd;
    r->events = events;
    r->events_union |= events;
}

/* ------------------------------------------------------------------------- */
/* Creación y validación de argumentos                                       */
/* ------------------------------------------------------------------------- */

static void test_invalid_arguments(void **state) {
    struct fixture *fx = *state;
    struct recorder r = {0};

    errno = 0;
    assert_int_equal(io_loop_add(fx->loop, -1, IO_READ, cb_record, &r), -1);
    assert_int_equal(errno, EBADF);

    errno = 0;
    assert_int_equal(io_loop_add(fx->loop, fx->sv[0], 0, cb_record, &r), -1);
    assert_int_equal(errno, EINVAL);

    errno = 0;
    assert_int_equal(io_loop_add(fx->loop, fx->sv[0], IO_HUP, cb_record, &r), -1);
    assert_int_equal(errno, EINVAL);

    errno = 0;
    assert_int_equal(io_loop_add(fx->loop, fx->sv[0], IO_READ, NULL, &r), -1);
    assert_int_equal(errno, EINVAL);

    errno = 0;
    assert_int_equal(io_loop_mod(fx->loop, fx->sv[0], IO_READ), -1);
    assert_int_equal(errno, ENOENT);

    errno = 0;
    assert_int_equal(io_loop_del(fx->loop, fx->sv[0]), -1);
    assert_int_equal(errno, ENOENT);

    assert_int_equal(io_loop_add(fx->loop, fx->sv[0], IO_READ, cb_record, &r), 0);
    errno = 0;
    assert_int_equal(io_loop_add(fx->loop, fx->sv[0], IO_READ, cb_record, &r), -1);
    assert_int_equal(errno, EEXIST);

    errno = 0;
    assert_int_equal(io_loop_mod(fx->loop, fx->sv[0], 0), -1);
    assert_int_equal(errno, EINVAL);

    /* Un fd que no es válido para el kernel se rechaza y no queda registrado. */
    int closed_fd = dup(fx->sv[1]);
    assert_true(closed_fd >= 0);
    close(closed_fd);
    errno = 0;
    assert_int_equal(io_loop_add(fx->loop, closed_fd, IO_READ, cb_record, &r), -1);
    assert_int_equal(errno, EBADF);
    errno = 0;
    assert_int_equal(io_loop_del(fx->loop, closed_fd), -1);
    assert_int_equal(errno, ENOENT);

    assert_int_equal(r.calls, 0);
    io_loop_destroy(NULL); /* debe aceptar NULL */
}

/* ------------------------------------------------------------------------- */
/* Registro y lectura                                                        */
/* ------------------------------------------------------------------------- */

static void test_run_once_times_out_without_events(void **state) {
    struct fixture *fx = *state;
    struct recorder r = {0};
    assert_int_equal(io_loop_add(fx->loop, fx->sv[0], IO_READ, cb_record, &r), 0);

    int64_t t0 = now_ms();
    assert_int_equal(io_loop_run_once(fx->loop, QUIET_MS), 0);
    int64_t elapsed = now_ms() - t0;

    assert_int_equal(r.calls, 0);
    assert_true(elapsed >= QUIET_MS - 5); /* tolerancia de redondeo del kernel */
}

static void test_registered_fd_reports_read(void **state) {
    struct fixture *fx = *state;
    struct recorder r = {0};
    assert_int_equal(io_loop_add(fx->loop, fx->sv[0], IO_READ, cb_record, &r), 0);

    write_all(fx->sv[1], "hola", 4);
    assert_int_equal(io_loop_run_once(fx->loop, 1000), 1);

    assert_int_equal(r.calls, 1);
    assert_int_equal(r.fd, fx->sv[0]);
    assert_true(r.events & IO_READ);
    assert_false(r.events & IO_WRITE); /* no pedido: no se entrega */

    char buf[8];
    assert_int_equal(read(fx->sv[0], buf, sizeof(buf)), 4);
    assert_memory_equal(buf, "hola", 4);
}

struct partial_reader {
    int calls;
    size_t consumed;
};

/* Lee un único byte por aviso: incumple deliberadamente el drenaje. */
static void cb_read_one_byte(io_loop *loop, int fd, uint32_t events, void *ud) {
    (void)loop;
    (void)events;
    struct partial_reader *p = ud;
    char c;
    if (read(fd, &c, 1) == 1) {
        p->consumed++;
    }
    p->calls++;
}

static void test_edge_triggered_does_not_repeat_undrained(void **state) {
    struct fixture *fx = *state;
    struct partial_reader p = {0};
    assert_int_equal(io_loop_add(fx->loop, fx->sv[0], IO_READ, cb_read_one_byte, &p), 0);

    write_all(fx->sv[1], "0123456789", 10);
    assert_int_equal(io_loop_run_once(fx->loop, 1000), 1);
    assert_int_equal(p.calls, 1);

    /* Quedan 9 bytes, pero en edge-triggered no hay aviso sin nuevo cambio. */
    assert_int_equal(io_loop_run_once(fx->loop, QUIET_MS), 0);
    assert_int_equal(p.calls, 1);

    /* Llegan datos nuevos: nuevo flanco, nuevo aviso. */
    write_all(fx->sv[1], "x", 1);
    assert_int_equal(io_loop_run_once(fx->loop, 1000), 1);
    assert_int_equal(p.calls, 2);
    assert_int_equal(p.consumed, 2);
}

static void test_mod_rearm_redelivers_pending_data(void **state) {
    struct fixture *fx = *state;
    struct partial_reader p = {0};
    assert_int_equal(io_loop_add(fx->loop, fx->sv[0], IO_READ, cb_read_one_byte, &p), 0);

    write_all(fx->sv[1], "abc", 3);
    assert_int_equal(io_loop_run_once(fx->loop, 1000), 1);
    assert_int_equal(io_loop_run_once(fx->loop, QUIET_MS), 0);

    /* Rearmar el mismo interés vuelve a notificar el dato pendiente. */
    assert_int_equal(io_loop_mod(fx->loop, fx->sv[0], IO_READ), 0);
    assert_int_equal(io_loop_run_once(fx->loop, 1000), 1);
    assert_int_equal(p.calls, 2);
}

struct drainer {
    int calls;
    size_t total;
    int reads;
    int last_errno;
};

/* Callback correcto para edge-triggered: lee hasta EAGAIN. */
static void cb_drain(io_loop *loop, int fd, uint32_t events, void *ud) {
    (void)loop;
    (void)events;
    struct drainer *d = ud;
    char buf[1000]; /* deliberadamente pequeño: obliga a muchas lecturas */
    d->calls++;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            d->total += (size_t)n;
            d->reads++;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        d->last_errno = n < 0 ? errno : 0;
        break;
    }
}

static void test_drain_until_eagain(void **state) {
    struct fixture *fx = *state;
    struct drainer d = {0};

    /* Llena el socket emisor hasta EAGAIN para tener un volumen mayor que el
     * buffer de lectura del callback. */
    char chunk[4096];
    memset(chunk, 'z', sizeof(chunk));
    size_t written = 0;
    for (;;) {
        ssize_t n = write(fx->sv[1], chunk, sizeof(chunk));
        if (n > 0) {
            written += (size_t)n;
            continue;
        }
        assert_true(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        break;
    }
    assert_true(written > sizeof(chunk));

    assert_int_equal(io_loop_add(fx->loop, fx->sv[0], IO_READ, cb_drain, &d), 0);
    assert_int_equal(io_loop_run_once(fx->loop, 1000), 1);

    assert_int_equal(d.calls, 1);
    assert_int_equal(d.total, written);
    assert_true(d.reads > 1);
    assert_true(d.last_errno == EAGAIN || d.last_errno == EWOULDBLOCK);

    /* Drenado: no hay aviso espurio posterior. */
    assert_int_equal(io_loop_run_once(fx->loop, QUIET_MS), 0);
    assert_int_equal(d.calls, 1);
}

/* ------------------------------------------------------------------------- */
/* Modificación y retirada                                                   */
/* ------------------------------------------------------------------------- */

static void test_mod_enables_and_disables_write(void **state) {
    struct fixture *fx = *state;
    struct recorder r = {0};

    /* El socket es escribible, pero solo se pide lectura: silencio. */
    assert_int_equal(io_loop_add(fx->loop, fx->sv[0], IO_READ, cb_record, &r), 0);
    assert_int_equal(io_loop_run_once(fx->loop, QUIET_MS), 0);

    assert_int_equal(io_loop_mod(fx->loop, fx->sv[0], IO_READ | IO_WRITE), 0);
    assert_int_equal(io_loop_run_once(fx->loop, 1000), 1);
    assert_true(r.events & IO_WRITE);
    assert_false(r.events & IO_READ);

    /* Sin escritura en el interés, nuevos datos solo producen IO_READ. */
    assert_int_equal(io_loop_mod(fx->loop, fx->sv[0], IO_READ), 0);
    write_all(fx->sv[1], "q", 1);
    r = (struct recorder){0};
    int n = io_loop_run_once(fx->loop, 1000);
    assert_int_equal(n, 1);
    assert_true(r.events_union & IO_READ);
    assert_false(r.events_union & IO_WRITE);
}

static void test_del_stops_delivery_and_allows_readd(void **state) {
    struct fixture *fx = *state;
    struct recorder r = {0};

    assert_int_equal(io_loop_add(fx->loop, fx->sv[0], IO_READ, cb_record, &r), 0);
    assert_int_equal(io_loop_del(fx->loop, fx->sv[0]), 0);

    write_all(fx->sv[1], "a", 1);
    assert_int_equal(io_loop_run_once(fx->loop, QUIET_MS), 0);
    assert_int_equal(r.calls, 0);

    /* Se puede volver a registrar; el dato pendiente se notifica al añadir. */
    assert_int_equal(io_loop_add(fx->loop, fx->sv[0], IO_READ, cb_record, &r), 0);
    assert_int_equal(io_loop_run_once(fx->loop, 1000), 1);
    assert_int_equal(r.calls, 1);
}

/* Estado compartido para la prueba de retirada dentro del mismo lote. */
struct batch_ctx {
    int fd_a;
    int fd_b;
    int replacement_src; /* socket que ocupará el número del fd retirado */
    int killer_calls;
    int stale_calls;
    int victim_fd;
};

static void cb_stale(io_loop *loop, int fd, uint32_t events, void *ud) {
    (void)loop;
    (void)fd;
    (void)events;
    struct batch_ctx *c = ud;
    c->stale_calls++;
}

/*
 * El primero que se ejecute retira el otro fd, lo cierra y reutiliza su
 * número para un socket nuevo registrado con otro callback. El evento del
 * otro fd, ya recogido del kernel en este lote, es obsoleto y no debe llegar
 * ni al callback original ni al nuevo registro.
 */
static void cb_killer(io_loop *loop, int fd, uint32_t events, void *ud) {
    (void)events;
    struct batch_ctx *c = ud;
    c->killer_calls++;
    if (c->killer_calls > 1) {
        return;
    }
    int victim = fd == c->fd_a ? c->fd_b : c->fd_a;
    assert_int_equal(io_loop_del(loop, victim), 0);
    close(victim);
    assert_int_equal(dup2(c->replacement_src, victim), victim);
    assert_int_equal(io_loop_add(loop, victim, IO_READ, cb_stale, c), 0);
    c->victim_fd = victim;
}

static void test_del_in_callback_discards_stale_batch_event(void **state) {
    struct fixture *fx = *state;
    int other[2];
    int repl[2];
    make_pair(other);
    make_pair(repl);

    struct batch_ctx c = {.fd_a = fx->sv[0], .fd_b = other[0], .replacement_src = repl[0]};
    assert_int_equal(io_loop_add(fx->loop, c.fd_a, IO_READ, cb_killer, &c), 0);
    assert_int_equal(io_loop_add(fx->loop, c.fd_b, IO_READ, cb_killer, &c), 0);

    /* Ambos listos antes de esperar: llegan en el mismo lote. */
    write_all(fx->sv[1], "1", 1);
    write_all(other[1], "2", 1);
    assert_int_equal(io_loop_run_once(fx->loop, 1000), 1);

    assert_int_equal(c.killer_calls, 1);
    assert_int_equal(c.stale_calls, 0);

    /* El nuevo registro sí funciona con sus propios eventos. */
    write_all(repl[1], "3", 1);
    assert_int_equal(io_loop_run_once(fx->loop, 1000), 1);
    assert_int_equal(c.stale_calls, 1);

    /* Limpieza: el fd reutilizado sustituye a sv[0] o a other[0]. */
    assert_int_equal(io_loop_del(fx->loop, c.victim_fd), 0);
    if (c.victim_fd == fx->sv[0]) {
        close(fx->sv[0]);
        fx->sv[0] = -1;
    } else {
        close(other[0]);
    }
    close(other[1]);
    close(repl[0]);
    close(repl[1]);
}

struct mask_ctx {
    int fd_a;
    int fd_b;
    int calls;
    uint32_t second_events;
};

/*
 * El primero que se ejecute quita IO_WRITE al otro fd. El otro ya tiene en el
 * lote un evento con escritura lista, que no debe entregarse.
 */
static void cb_mod_other(io_loop *loop, int fd, uint32_t events, void *ud) {
    struct mask_ctx *c = ud;
    c->calls++;
    if (c->calls == 1) {
        int other = fd == c->fd_a ? c->fd_b : c->fd_a;
        assert_int_equal(io_loop_mod(loop, other, IO_READ), 0);
    } else {
        c->second_events |= events;
    }
}

static void test_mod_in_callback_masks_batch_events(void **state) {
    struct fixture *fx = *state;
    int other[2];
    make_pair(other);

    struct mask_ctx c = {.fd_a = fx->sv[0], .fd_b = other[0]};
    /* Ambos listos para leer y escribir en el mismo lote. */
    write_all(fx->sv[1], "1", 1);
    write_all(other[1], "2", 1);
    assert_int_equal(io_loop_add(fx->loop, c.fd_a, IO_READ | IO_WRITE, cb_mod_other, &c), 0);
    assert_int_equal(io_loop_add(fx->loop, c.fd_b, IO_READ | IO_WRITE, cb_mod_other, &c), 0);

    int n = io_loop_run_once(fx->loop, 1000);
    assert_true(n >= 2);
    assert_true(c.second_events & IO_READ);
    assert_false(c.second_events & IO_WRITE);

    assert_int_equal(io_loop_del(fx->loop, other[0]), 0);
    close(other[0]);
    close(other[1]);
}

static void cb_del_self(io_loop *loop, int fd, uint32_t events, void *ud) {
    (void)events;
    struct recorder *r = ud;
    r->calls++;
    assert_int_equal(io_loop_del(loop, fd), 0);
}

static void test_del_self_inside_callback(void **state) {
    struct fixture *fx = *state;
    struct recorder r = {0};
    assert_int_equal(io_loop_add(fx->loop, fx->sv[0], IO_READ | IO_WRITE, cb_del_self, &r), 0);

    write_all(fx->sv[1], "d", 1);
    assert_int_equal(io_loop_run_once(fx->loop, 1000), 1);
    write_all(fx->sv[1], "e", 1);
    assert_int_equal(io_loop_run_once(fx->loop, QUIET_MS), 0);
    assert_int_equal(r.calls, 1);
}

static void test_peer_close_reports_hup_and_eof(void **state) {
    struct fixture *fx = *state;
    struct recorder r = {0};
    assert_int_equal(io_loop_add(fx->loop, fx->sv[0], IO_READ, cb_record, &r), 0);

    close_if_open(&fx->sv[1]);
    assert_int_equal(io_loop_run_once(fx->loop, 1000), 1);
    assert_true(r.events & IO_HUP);

    char c;
    assert_int_equal(read(fx->sv[0], &c, 1), 0); /* EOF */
}

static void test_slot_table_grows_for_high_fd(void **state) {
    struct fixture *fx = *state;
    struct recorder r = {0};
    /* Número alto para forzar el crecimiento de la tabla indexada por fd. */
    int high = fcntl(fx->sv[0], F_DUPFD, 900);
    assert_true(high >= 900);

    assert_int_equal(io_loop_add(fx->loop, high, IO_READ, cb_record, &r), 0);
    write_all(fx->sv[1], "h", 1);
    assert_int_equal(io_loop_run_once(fx->loop, 1000), 1);
    assert_int_equal(r.fd, high);

    assert_int_equal(io_loop_del(fx->loop, high), 0);
    close(high);
}

/* ------------------------------------------------------------------------- */
/* Ejecución y parada                                                        */
/* ------------------------------------------------------------------------- */

static void cb_stop_after_drain(io_loop *loop, int fd, uint32_t events, void *ud) {
    cb_drain(loop, fd, events, ud);
    io_loop_stop(loop);
}

static void test_stop_from_callback(void **state) {
    struct fixture *fx = *state;
    struct drainer d = {0};
    assert_int_equal(io_loop_add(fx->loop, fx->sv[0], IO_READ, cb_stop_after_drain, &d), 0);

    write_all(fx->sv[1], "stop", 4);
    assert_int_equal(io_loop_run(fx->loop), 0);
    assert_int_equal(d.calls, 1);
    assert_int_equal(d.total, 4);

    /* La petición se consumió: el bucle vuelve a ser utilizable. */
    write_all(fx->sv[1], "again", 5);
    assert_int_equal(io_loop_run(fx->loop), 0);
    assert_int_equal(d.calls, 2);
}

static void test_stop_before_run_returns_immediately(void **state) {
    struct fixture *fx = *state;
    io_loop_stop(fx->loop);
    int64_t t0 = now_ms();
    assert_int_equal(io_loop_run(fx->loop), 0);
    assert_true(now_ms() - t0 < 1000);
}

static void *thread_stopper(void *arg) {
    sleep_ms(QUIET_MS);
    io_loop_stop(arg);
    return NULL;
}

static void test_stop_from_other_thread(void **state) {
    struct fixture *fx = *state;
    struct recorder r = {0};
    /* Sin datos: io_loop_run queda bloqueado en el kernel hasta el despertar. */
    assert_int_equal(io_loop_add(fx->loop, fx->sv[0], IO_READ, cb_record, &r), 0);

    pthread_t th;
    assert_int_equal(pthread_create(&th, NULL, thread_stopper, fx->loop), 0);
    assert_int_equal(io_loop_run(fx->loop), 0);
    assert_int_equal(pthread_join(th, NULL), 0);
    assert_int_equal(r.calls, 0);
}

static io_loop *volatile g_signal_loop;
static volatile sig_atomic_t g_signal_seen;

static void on_sigusr1(int sig) {
    (void)sig;
    g_signal_seen = 1;
    io_loop_stop(g_signal_loop);
}

static void *thread_signal_main(void *arg) {
    sleep_ms(QUIET_MS);
    pthread_kill(*(pthread_t *)arg, SIGUSR1);
    return NULL;
}

static void test_stop_from_signal_handler(void **state) {
    struct fixture *fx = *state;
    struct sigaction sa;
    struct sigaction old;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigusr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* sin SA_RESTART: la espera del kernel recibe EINTR */
    g_signal_loop = fx->loop;
    g_signal_seen = 0;
    assert_int_equal(sigaction(SIGUSR1, &sa, &old), 0);

    /* La señal se dirige al hilo que ejecuta el bucle. */
    pthread_t self = pthread_self();
    pthread_t th;
    assert_int_equal(pthread_create(&th, NULL, thread_signal_main, &self), 0);
    int rc = io_loop_run(fx->loop);
    assert_int_equal(pthread_join(th, NULL), 0);
    assert_int_equal(sigaction(SIGUSR1, &old, NULL), 0);

    assert_int_equal(rc, 0);
    assert_int_equal(g_signal_seen, 1);
}

struct reentry {
    int run_errno;
    int run_once_errno;
};

static void cb_reenter(io_loop *loop, int fd, uint32_t events, void *ud) {
    (void)fd;
    (void)events;
    struct reentry *re = ud;
    errno = 0;
    if (io_loop_run(loop) < 0) {
        re->run_errno = errno;
    }
    errno = 0;
    if (io_loop_run_once(loop, 0) < 0) {
        re->run_once_errno = errno;
    }
    io_loop_stop(loop);
}

static void test_reentrant_run_is_rejected(void **state) {
    struct fixture *fx = *state;
    struct reentry re = {0};
    assert_int_equal(io_loop_add(fx->loop, fx->sv[0], IO_WRITE, cb_reenter, &re), 0);
    assert_int_equal(io_loop_run(fx->loop), 0);
    assert_int_equal(re.run_errno, EBUSY);
    assert_int_equal(re.run_once_errno, EBUSY);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_invalid_arguments, setup, teardown),
        cmocka_unit_test_setup_teardown(test_run_once_times_out_without_events, setup, teardown),
        cmocka_unit_test_setup_teardown(test_registered_fd_reports_read, setup, teardown),
        cmocka_unit_test_setup_teardown(test_edge_triggered_does_not_repeat_undrained, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_mod_rearm_redelivers_pending_data, setup, teardown),
        cmocka_unit_test_setup_teardown(test_drain_until_eagain, setup, teardown),
        cmocka_unit_test_setup_teardown(test_mod_enables_and_disables_write, setup, teardown),
        cmocka_unit_test_setup_teardown(test_del_stops_delivery_and_allows_readd, setup, teardown),
        cmocka_unit_test_setup_teardown(test_del_in_callback_discards_stale_batch_event, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_mod_in_callback_masks_batch_events, setup, teardown),
        cmocka_unit_test_setup_teardown(test_del_self_inside_callback, setup, teardown),
        cmocka_unit_test_setup_teardown(test_peer_close_reports_hup_and_eof, setup, teardown),
        cmocka_unit_test_setup_teardown(test_slot_table_grows_for_high_fd, setup, teardown),
        cmocka_unit_test_setup_teardown(test_stop_from_callback, setup, teardown),
        cmocka_unit_test_setup_teardown(test_stop_before_run_returns_immediately, setup, teardown),
        cmocka_unit_test_setup_teardown(test_stop_from_other_thread, setup, teardown),
        cmocka_unit_test_setup_teardown(test_stop_from_signal_handler, setup, teardown),
        cmocka_unit_test_setup_teardown(test_reentrant_run_is_rejected, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
