/*
 * health.c — ver health.h.
 */
#include "health.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "io_event.h"
#include "listener.h"
#include "timer.h"

#define PROBE_BUF 512

struct health_target {
    uint64_t gen;
    uint32_t pool;
    uint32_t backend;
    struct sockaddr_storage addr;
    socklen_t addrlen;
    enum health_type type;
    uint32_t interval_ms;
    uint32_t timeout_ms;
    uint32_t expect_min;
    uint32_t expect_max;
    char request[PROBE_BUF];
    size_t request_len;
};

struct health_plan {
    uint64_t gen;
    size_t n;
    struct health_target *targets;
};

struct probe {
    struct health_checker *hc;
    struct probe *prev;
    struct probe *next;
    struct health_target t;
    int fd;
    bool in_flight;
    bool retired; /* de un plan anterior: se libera al terminar */
    bool connected;
    size_t sent;
    char buf[PROBE_BUF];
    size_t len;
    timer next_run;
    timer deadline;
};

struct health_checker {
    pthread_t thread;
    atomic_int stopping;
    atomic_uint_least64_t probes_done;
    atomic_uint_least64_t dropped;

    /* Solo el hilo de health. */
    io_loop *loop;
    timer_heap timers;
    struct probe *probes;

    /* Buzón de planes (mutex). */
    pthread_mutex_t plan_mu;
    struct health_plan *pending;
    int plan_rd;
    int plan_wr;

    /* Cola de resultados (mutex). */
    pthread_mutex_t res_mu;
    struct health_result queue[HEALTH_QUEUE_CAP];
    size_t head;
    size_t count;
    int notify_rd;
    int notify_wr;
};

/* ------------------------------------------------------------------------- */
/* Plan (hilo del event loop)                                                */
/* ------------------------------------------------------------------------- */

struct health_plan *health_plan_build(const struct config *cfg, uint64_t gen) {
    struct health_plan *plan = calloc(1, sizeof(*plan));
    if (plan == NULL) {
        return NULL;
    }
    plan->gen = gen;
    size_t n = 0;
    for (size_t p = 0; p < cfg->npools; p++) {
        if (cfg->pools[p].health.type != HEALTH_NONE) {
            n += cfg->pools[p].nbackends;
        }
    }
    plan->targets = calloc(n + 1, sizeof(*plan->targets));
    if (plan->targets == NULL) {
        free(plan);
        return NULL;
    }
    for (size_t p = 0; p < cfg->npools; p++) {
        const struct cfg_pool *cp = &cfg->pools[p];
        const struct cfg_health *h = &cp->health;
        if (h->type == HEALTH_NONE) {
            continue;
        }
        for (size_t b = 0; b < cp->nbackends; b++) {
            struct health_target *t = &plan->targets[plan->n++];
            t->gen = gen;
            t->pool = (uint32_t)p;
            t->backend = (uint32_t)b;
            t->addr = cp->backends[b].addr.ss;
            t->addrlen = cp->backends[b].addr.len;
            t->type = h->type;
            t->interval_ms = h->interval_ms;
            t->timeout_ms = h->timeout_ms;
            t->expect_min = h->expect_min;
            t->expect_max = h->expect_max;
            if (h->type == HEALTH_HTTP) {
                const char *host = h->host[0] != '\0' ? h->host : cp->backends[b].addr.text;
                int w = snprintf(t->request, sizeof(t->request),
                                 "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: proxy-health/1\r\n"
                                 "Connection: close\r\n\r\n",
                                 h->path, host);
                /* path y host están acotados a 255 en config: siempre cabe. */
                t->request_len = w > 0 && (size_t)w < sizeof(t->request) ? (size_t)w : 0;
            }
        }
    }
    return plan;
}

void health_plan_free(struct health_plan *plan) {
    if (plan != NULL) {
        free(plan->targets);
        free(plan);
    }
}

size_t health_plan_size(const struct health_plan *plan) {
    return plan->n;
}

/* ------------------------------------------------------------------------- */
/* Sondas (hilo de health)                                                   */
/* ------------------------------------------------------------------------- */

static void wake(int fd) {
    unsigned char b = 1;
    ssize_t r = write(fd, &b, 1); /* lleno: ya hay un aviso pendiente */
    (void)r;
}

static void drain(int fd) {
    unsigned char buf[64];
    while (read(fd, buf, sizeof(buf)) > 0) {
    }
}

static void push_result(struct health_checker *hc, const struct health_result *r) {
    pthread_mutex_lock(&hc->res_mu);
    if (hc->count < HEALTH_QUEUE_CAP) {
        hc->queue[(hc->head + hc->count) % HEALTH_QUEUE_CAP] = *r;
        hc->count++;
    } else {
        atomic_fetch_add(&hc->dropped, 1);
    }
    pthread_mutex_unlock(&hc->res_mu);
    wake(hc->notify_wr);
}

static void probe_free(struct probe *p) {
    struct health_checker *hc = p->hc;
    timer_cancel(&hc->timers, &p->next_run);
    timer_cancel(&hc->timers, &p->deadline);
    if (p->fd >= 0) {
        (void)io_loop_del(hc->loop, p->fd);
        close(p->fd);
    }
    if (p->prev != NULL) {
        p->prev->next = p->next;
    } else {
        hc->probes = p->next;
    }
    if (p->next != NULL) {
        p->next->prev = p->prev;
    }
    free(p);
}

static void probe_finish(struct probe *p, bool ok, uint16_t status, int err) {
    struct health_checker *hc = p->hc;
    timer_cancel(&hc->timers, &p->deadline);
    if (p->fd >= 0) {
        (void)io_loop_del(hc->loop, p->fd);
        close(p->fd);
        p->fd = -1;
    }
    p->in_flight = false;
    atomic_fetch_add(&hc->probes_done, 1);
    struct health_result r = {.gen = p->t.gen,
                              .pool = p->t.pool,
                              .backend = p->t.backend,
                              .ok = ok,
                              .status = status,
                              .err = err};
    push_result(hc, &r);
    if (p->retired) {
        probe_free(p);
    }
}

/* ¿Hay una línea de estado HTTP completa? Devuelve el código o -1/0. */
static int parse_status(const char *buf, size_t len) {
    const char *nl = memchr(buf, '\n', len);
    if (nl == NULL) {
        return 0; /* incompleta */
    }
    if (len < 12 || memcmp(buf, "HTTP/1.", 7) != 0 || buf[8] != ' ') {
        return -1;
    }
    int status = 0;
    for (int i = 9; i < 12; i++) {
        if (buf[i] < '0' || buf[i] > '9') {
            return -1;
        }
        status = status * 10 + (buf[i] - '0');
    }
    return status;
}

static void on_probe_io(io_loop *loop, int fd, uint32_t events, void *ud) {
    (void)loop;
    (void)fd;
    struct probe *p = ud;
    if (!p->connected) {
        int soerr = 0;
        socklen_t sl = sizeof(soerr);
        if (getsockopt(p->fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0) {
            soerr = errno;
        }
        if (soerr != 0) {
            probe_finish(p, false, 0, soerr);
            return;
        }
        if (!(events & IO_WRITE)) {
            return;
        }
        p->connected = true;
        if (p->t.type == HEALTH_TCP) {
            probe_finish(p, true, 0, 0);
            return;
        }
    }
    while (p->sent < p->t.request_len) {
        ssize_t n = send(p->fd, p->t.request + p->sent, p->t.request_len - p->sent, 0);
        if (n > 0) {
            p->sent += (size_t)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            probe_finish(p, false, 0, n < 0 ? errno : EPIPE);
            return;
        }
    }
    for (;;) {
        if (p->len == sizeof(p->buf)) {
            probe_finish(p, false, 0, EPROTO); /* línea de estado demasiado larga */
            return;
        }
        ssize_t n = recv(p->fd, p->buf + p->len, sizeof(p->buf) - p->len, 0);
        if (n > 0) {
            p->len += (size_t)n;
            int st = parse_status(p->buf, p->len);
            if (st < 0) {
                probe_finish(p, false, 0, EPROTO);
                return;
            }
            if (st > 0) {
                bool ok = (uint32_t)st >= p->t.expect_min && (uint32_t)st <= p->t.expect_max;
                probe_finish(p, ok, (uint16_t)st, 0);
                return;
            }
        } else if (n == 0) {
            probe_finish(p, false, 0, ECONNRESET); /* cerró sin responder */
            return;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        } else if (errno != EINTR) {
            probe_finish(p, false, 0, errno);
            return;
        }
    }
}

static void on_probe_deadline(timer_heap *heap, timer *t, void *ud) {
    (void)heap;
    (void)t;
    probe_finish(ud, false, 0, ETIMEDOUT);
}

static void on_probe_start(timer_heap *heap, timer *t, void *ud) {
    (void)t;
    struct probe *p = ud;
    struct health_checker *hc = p->hc;
    uint64_t now = timer_now_ms();
    /* La siguiente se programa desde el inicio de esta (timeout < intervalo). */
    (void)timer_schedule_in(heap, &p->next_run, now, p->t.interval_ms);
    p->in_flight = true;
    p->connected = false;
    p->sent = 0;
    p->len = 0;
    p->fd = socket(p->t.addr.ss_family, SOCK_STREAM, 0);
    if (p->fd < 0 || fd_set_nonblock_cloexec(p->fd) < 0) {
        probe_finish(p, false, 0, errno);
        return;
    }
    if (connect(p->fd, (const struct sockaddr *)&p->t.addr, p->t.addrlen) == 0) {
        p->connected = true;
        if (p->t.type == HEALTH_TCP) {
            probe_finish(p, true, 0, 0);
            return;
        }
    } else if (errno != EINPROGRESS) {
        probe_finish(p, false, 0, errno);
        return;
    }
    if (io_loop_add(hc->loop, p->fd, IO_READ | IO_WRITE, on_probe_io, p) < 0) {
        probe_finish(p, false, 0, errno);
        return;
    }
    (void)timer_schedule_in(heap, &p->deadline, now, p->t.timeout_ms);
}

static void apply_plan(struct health_checker *hc, struct health_plan *plan) {
    struct probe *p = hc->probes;
    while (p != NULL) {
        struct probe *next = p->next;
        if (p->in_flight) {
            p->retired = true; /* informará con su generación y se liberará */
            timer_cancel(&hc->timers, &p->next_run);
        } else {
            probe_free(p);
        }
        p = next;
    }
    uint64_t now = timer_now_ms();
    for (size_t i = 0; plan != NULL && i < plan->n; i++) {
        struct probe *np = calloc(1, sizeof(*np));
        if (np == NULL) {
            continue; /* sin memoria: ese backend queda sin sondas */
        }
        np->hc = hc;
        np->t = plan->targets[i];
        np->fd = -1;
        timer_init(&np->next_run, on_probe_start, np);
        timer_init(&np->deadline, on_probe_deadline, np);
        np->next = hc->probes;
        if (hc->probes != NULL) {
            hc->probes->prev = np;
        }
        hc->probes = np;
        (void)timer_schedule_at(&hc->timers, &np->next_run, now); /* primera sonda ya */
    }
    health_plan_free(plan);
}

static void on_plan(io_loop *loop, int fd, uint32_t events, void *ud) {
    (void)loop;
    (void)events;
    struct health_checker *hc = ud;
    drain(fd);
    pthread_mutex_lock(&hc->plan_mu);
    struct health_plan *plan = hc->pending;
    bool have = plan != NULL;
    hc->pending = NULL;
    pthread_mutex_unlock(&hc->plan_mu);
    if (have) {
        apply_plan(hc, plan);
    }
}

static void *health_main(void *arg) {
    struct health_checker *hc = arg;
    while (!atomic_load(&hc->stopping)) {
        if (timer_loop_run_once(hc->loop, &hc->timers) < 0) {
            break;
        }
    }
    struct probe *p = hc->probes;
    while (p != NULL) {
        struct probe *next = p->next;
        probe_free(p);
        p = next;
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* API (hilo del event loop)                                                 */
/* ------------------------------------------------------------------------- */

static int make_pipe(int *rd, int *wr) {
    int p[2];
    if (pipe(p) < 0) {
        return -1;
    }
    if (fd_set_nonblock_cloexec(p[0]) < 0 || fd_set_nonblock_cloexec(p[1]) < 0) {
        close(p[0]);
        close(p[1]);
        return -1;
    }
    *rd = p[0];
    *wr = p[1];
    return 0;
}

static void destroy(health_checker *hc) {
    int fds[] = {hc->plan_rd, hc->plan_wr, hc->notify_rd, hc->notify_wr};
    for (size_t i = 0; i < sizeof(fds) / sizeof(fds[0]); i++) {
        if (fds[i] >= 0) {
            close(fds[i]);
        }
    }
    health_plan_free(hc->pending);
    timer_heap_destroy(&hc->timers);
    io_loop_destroy(hc->loop);
    pthread_mutex_destroy(&hc->plan_mu);
    pthread_mutex_destroy(&hc->res_mu);
    free(hc);
}

health_checker *health_start(void) {
    health_checker *hc = calloc(1, sizeof(*hc));
    if (hc == NULL) {
        return NULL;
    }
    hc->plan_rd = hc->plan_wr = hc->notify_rd = hc->notify_wr = -1;
    atomic_init(&hc->stopping, 0);
    atomic_init(&hc->probes_done, 0);
    atomic_init(&hc->dropped, 0);
    pthread_mutex_init(&hc->plan_mu, NULL);
    pthread_mutex_init(&hc->res_mu, NULL);
    timer_heap_init(&hc->timers);
    hc->loop = io_loop_create(64);
    if (hc->loop == NULL || make_pipe(&hc->plan_rd, &hc->plan_wr) < 0 ||
        make_pipe(&hc->notify_rd, &hc->notify_wr) < 0 ||
        io_loop_add(hc->loop, hc->plan_rd, IO_READ, on_plan, hc) < 0) {
        int saved = errno;
        destroy(hc);
        errno = saved;
        return NULL;
    }
    /* Las señales de proceso las atiende el hilo principal (self-pipe). */
    sigset_t all;
    sigset_t old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    int rc = pthread_create(&hc->thread, NULL, health_main, hc);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (rc != 0) {
        destroy(hc);
        errno = rc;
        return NULL;
    }
    return hc;
}

int health_notify_fd(const health_checker *hc) {
    return hc->notify_rd;
}

void health_submit_plan(health_checker *hc, struct health_plan *plan) {
    pthread_mutex_lock(&hc->plan_mu);
    struct health_plan *old = hc->pending; /* sustituido sin llegar a aplicarse */
    hc->pending = plan;
    pthread_mutex_unlock(&hc->plan_mu);
    health_plan_free(old);
    wake(hc->plan_wr);
}

size_t health_take_results(health_checker *hc, struct health_result *out, size_t max) {
    drain(hc->notify_rd);
    pthread_mutex_lock(&hc->res_mu);
    size_t n = hc->count < max ? hc->count : max;
    for (size_t i = 0; i < n; i++) {
        out[i] = hc->queue[(hc->head + i) % HEALTH_QUEUE_CAP];
    }
    hc->head = (hc->head + n) % HEALTH_QUEUE_CAP;
    hc->count -= n;
    bool more = hc->count > 0;
    pthread_mutex_unlock(&hc->res_mu);
    if (more) {
        wake(hc->notify_wr); /* quedan resultados: volver a avisar */
    }
    return n;
}

uint64_t health_probes_done(const health_checker *hc) {
    return atomic_load(&hc->probes_done);
}

uint64_t health_results_dropped(const health_checker *hc) {
    return atomic_load(&hc->dropped);
}

void health_stop(health_checker *hc) {
    if (hc == NULL) {
        return;
    }
    atomic_store(&hc->stopping, 1);
    io_loop_stop(hc->loop); /* despierta timer_loop_run_once */
    pthread_join(hc->thread, NULL);
    destroy(hc);
}
