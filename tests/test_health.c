/*
 * test_health.c — hilo de health checks contra sockets reales: TCP, criterio
 * de estado HTTP, timeout y resultados de un plan retirado que llegan con su
 * generación antigua.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cmocka.h>

#include "config.h"
#include "health.h"
#include "timer.h"

#define TEST_TIMEOUT_S 15

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

/* ------------------------------------------------------------------------- */
/* Servidores de prueba                                                      */
/* ------------------------------------------------------------------------- */

static int listen_any(uint16_t *port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fail_msg("%s", "socket");
        return -1;
    }
    struct sockaddr_in a = {.sin_family = AF_INET};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert_int_equal(bind(fd, (struct sockaddr *)&a, sizeof(a)), 0);
    assert_int_equal(listen(fd, 16), 0);
    socklen_t l = sizeof(a);
    assert_int_equal(getsockname(fd, (struct sockaddr *)&a, &l), 0);
    *port = ntohs(a.sin_port);
    return fd;
}

static uint16_t closed_port(void) {
    uint16_t port;
    int fd = listen_any(&port);
    close(fd);
    return port;
}

/* Servidor HTTP mínimo: responde `code` tras `delay_ms` a cada conexión. */
struct http_srv {
    int lfd;
    uint16_t port;
    atomic_int code;
    atomic_int delay_ms;
    atomic_int accepted;
    atomic_int stop;
    pthread_t th;
};

static void sleep_ms(long ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
    }
}

static void *http_srv_main(void *arg) {
    struct http_srv *s = arg;
    while (!atomic_load(&s->stop)) {
        struct pollfd pfd = {.fd = s->lfd, .events = POLLIN};
        if (poll(&pfd, 1, 50) <= 0) {
            continue;
        }
        int c = accept(s->lfd, NULL, NULL);
        if (c < 0) {
            continue;
        }
        atomic_fetch_add(&s->accepted, 1);
        char buf[1024];
        size_t len = 0;
        while (len < sizeof(buf) - 1) {
            ssize_t n = recv(c, buf + len, sizeof(buf) - 1 - len, 0);
            if (n <= 0) {
                break;
            }
            len += (size_t)n;
            buf[len] = '\0';
            if (strstr(buf, "\r\n\r\n") != NULL) {
                break;
            }
        }
        sleep_ms(atomic_load(&s->delay_ms));
        char resp[128];
        int w = snprintf(resp, sizeof(resp), "HTTP/1.1 %d X\r\nContent-Length: 0\r\n\r\n",
                         atomic_load(&s->code));
        (void)send(c, resp, (size_t)w, MSG_NOSIGNAL);
        close(c);
    }
    return NULL;
}

static void http_srv_start(struct http_srv *s, int code, int delay_ms) {
    memset(s, 0, sizeof(*s));
    s->lfd = listen_any(&s->port);
    atomic_init(&s->code, code);
    atomic_init(&s->delay_ms, delay_ms);
    atomic_init(&s->accepted, 0);
    atomic_init(&s->stop, 0);
    assert_int_equal(pthread_create(&s->th, NULL, http_srv_main, s), 0);
}

static void http_srv_stop(struct http_srv *s) {
    atomic_store(&s->stop, 1);
    pthread_join(s->th, NULL);
    close(s->lfd);
}

/* ------------------------------------------------------------------------- */
/* Utilidades                                                                */
/* ------------------------------------------------------------------------- */

static struct config *pool_config(const char *health, const uint16_t *ports, size_t n) {
    char text[2048];
    int off = snprintf(text, sizeof(text),
                       "[[frontend]]\nlisten = \"127.0.0.1:1\"\n"
                       "[[pool]]\nname = \"p\"\n[pool.health]\n%s",
                       health);
    for (size_t i = 0; i < n; i++) {
        off += snprintf(text + off, sizeof(text) - (size_t)off,
                        "[[pool.backend]]\naddress = \"127.0.0.1:%u\"\n", ports[i]);
    }
    (void)snprintf(text + off, sizeof(text) - (size_t)off, "[routing]\ndefault_pool = \"p\"\n");
    char err[CONFIG_ERR_LEN];
    struct config *cfg = config_load_string(text, "h.toml", err, sizeof(err));
    if (cfg == NULL) {
        fail_msg("%s", err);
    }
    return cfg;
}

/* Recoge resultados hasta tener `want` o vencer `timeout_ms`. */
static size_t collect(health_checker *hc, struct health_result *out, size_t want, int timeout_ms) {
    size_t got = 0;
    uint64_t deadline = timer_now_ms() + (uint64_t)timeout_ms;
    while (got < want) {
        uint64_t now = timer_now_ms();
        if (now >= deadline) {
            break;
        }
        struct pollfd pfd = {.fd = health_notify_fd(hc), .events = POLLIN};
        (void)poll(&pfd, 1, (int)(deadline - now));
        got += health_take_results(hc, out + got, want - got);
    }
    return got;
}

/* Primer resultado de cada backend en `res`. */
static const struct health_result *first_for(const struct health_result *res, size_t n,
                                             uint32_t backend) {
    for (size_t i = 0; i < n; i++) {
        if (res[i].backend == backend) {
            return &res[i];
        }
    }
    return NULL;
}

/* No nulo o fin de la prueba (abort es noreturn para el compilador). */
static const struct health_result *must(const struct health_result *r) {
    if (r == NULL) {
        fail_msg("%s", "falta el resultado de un backend");
        abort();
    }
    return r;
}

/* ------------------------------------------------------------------------- */

static void test_tcp_probe_ok_and_refused(void **state) {
    (void)state;
    uint16_t up_port;
    int lfd = listen_any(&up_port); /* acepta en el backlog: connect correcto */
    const uint16_t ports[] = {up_port, closed_port()};
    struct config *cfg =
        pool_config("type = \"tcp\"\ninterval_ms = 200\ntimeout_ms = 100\n", ports, 2);
    health_checker *hc = health_start();
    assert_non_null(hc);
    struct health_plan *plan = health_plan_build(cfg, 3);
    assert_int_equal(health_plan_size(plan), 2);
    health_submit_plan(hc, plan);

    struct health_result res[16];
    size_t n = collect(hc, res, 4, 3000);
    assert_true(n >= 2);
    const struct health_result *ok = must(first_for(res, n, 0));
    const struct health_result *bad = must(first_for(res, n, 1));
    assert_int_equal(ok->gen, 3);
    assert_int_equal(ok->pool, 0);
    assert_true(ok->ok);
    assert_false(bad->ok);
    assert_int_equal(bad->err, ECONNREFUSED);
    assert_true(health_probes_done(hc) >= 2);
    health_stop(hc);
    config_unref(cfg);
    close(lfd);
}

static void test_http_status_criterion(void **state) {
    (void)state;
    struct http_srv good;
    struct http_srv bad;
    http_srv_start(&good, 204, 0);
    http_srv_start(&bad, 503, 0);
    const uint16_t ports[] = {good.port, bad.port};
    struct config *cfg = pool_config("type = \"http\"\ninterval_ms = 300\ntimeout_ms = 200\n"
                                     "path = \"/ready\"\n",
                                     ports, 2);
    health_checker *hc = health_start();
    assert_non_null(hc);
    health_submit_plan(hc, health_plan_build(cfg, 1));
    struct health_result res[16];
    size_t n = collect(hc, res, 4, 3000);
    const struct health_result *g = must(first_for(res, n, 0));
    const struct health_result *b = must(first_for(res, n, 1));
    assert_true(g->ok);
    assert_int_equal(g->status, 204);
    assert_false(b->ok); /* fuera de 200-399 */
    assert_int_equal(b->status, 503);
    health_stop(hc);
    http_srv_stop(&good);
    http_srv_stop(&bad);
    config_unref(cfg);
}

static void test_http_timeout(void **state) {
    (void)state;
    struct http_srv slow;
    http_srv_start(&slow, 200, 600);
    const uint16_t ports[] = {slow.port};
    struct config *cfg =
        pool_config("type = \"http\"\ninterval_ms = 1000\ntimeout_ms = 150\n", ports, 1);
    health_checker *hc = health_start();
    assert_non_null(hc);
    uint64_t t0 = timer_now_ms();
    health_submit_plan(hc, health_plan_build(cfg, 1));
    struct health_result r;
    assert_int_equal(collect(hc, &r, 1, 3000), 1);
    uint64_t elapsed = timer_now_ms() - t0;
    assert_false(r.ok);
    assert_int_equal(r.err, ETIMEDOUT);
    assert_true(elapsed >= 150 && elapsed < 600); /* por el plazo, no por la respuesta */
    health_stop(hc);
    http_srv_stop(&slow);
    config_unref(cfg);
}

static void test_retired_probe_reports_old_generation(void **state) {
    (void)state;
    struct http_srv slow;
    http_srv_start(&slow, 200, 400);
    const uint16_t ports[] = {slow.port};
    struct config *cfg =
        pool_config("type = \"http\"\ninterval_ms = 5000\ntimeout_ms = 2000\n", ports, 1);
    health_checker *hc = health_start();
    assert_non_null(hc);
    health_submit_plan(hc, health_plan_build(cfg, 7));
    /* Sincronización observable: la sonda de la generación 7 está en curso. */
    uint64_t deadline = timer_now_ms() + 3000;
    while (atomic_load(&slow.accepted) == 0 && timer_now_ms() < deadline) {
        sleep_ms(5);
    }
    assert_int_equal(atomic_load(&slow.accepted), 1);

    /* Nueva generación sin sondas: la en curso termina e informa como 7. */
    struct config *empty = config_load_string("[[frontend]]\nlisten = \"127.0.0.1:1\"\n"
                                              "[[pool]]\nname = \"p\"\n"
                                              "[[pool.backend]]\naddress = \"127.0.0.1:2\"\n"
                                              "[routing]\ndefault_pool = \"p\"\n",
                                              "e.toml", (char[CONFIG_ERR_LEN]){0}, CONFIG_ERR_LEN);
    assert_non_null(empty);
    struct health_plan *p8 = health_plan_build(empty, 8);
    assert_int_equal(health_plan_size(p8), 0);
    health_submit_plan(hc, p8);

    struct health_result r[4] = {0};
    size_t n = collect(hc, r, 4, 1500);
    assert_int_equal(n, 1); /* solo la retirada; ninguna nueva ni repetida */
    assert_int_equal(r[0].gen, 7);
    assert_true(r[0].ok);
    assert_int_equal(atomic_load(&slow.accepted), 1);
    health_stop(hc);
    http_srv_stop(&slow);
    config_unref(cfg);
    config_unref(empty);
}

static void test_stop_with_probes_in_flight(void **state) {
    (void)state;
    struct http_srv slow;
    http_srv_start(&slow, 200, 800);
    const uint16_t ports[] = {slow.port};
    struct config *cfg =
        pool_config("type = \"http\"\ninterval_ms = 3000\ntimeout_ms = 2000\n", ports, 1);
    health_checker *hc = health_start();
    assert_non_null(hc);
    health_submit_plan(hc, health_plan_build(cfg, 1));
    uint64_t deadline = timer_now_ms() + 3000;
    while (atomic_load(&slow.accepted) == 0 && timer_now_ms() < deadline) {
        sleep_ms(5);
    }
    uint64_t t0 = timer_now_ms();
    health_stop(hc); /* no espera a la sonda: cierra su socket */
    assert_true(timer_now_ms() - t0 < 500);
    http_srv_stop(&slow);
    config_unref(cfg);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_tcp_probe_ok_and_refused, setup, teardown),
        cmocka_unit_test_setup_teardown(test_http_status_criterion, setup, teardown),
        cmocka_unit_test_setup_teardown(test_http_timeout, setup, teardown),
        cmocka_unit_test_setup_teardown(test_retired_probe_reports_old_generation, setup, teardown),
        cmocka_unit_test_setup_teardown(test_stop_with_probes_in_flight, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
