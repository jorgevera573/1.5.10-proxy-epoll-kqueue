/*
 * worker.c — ver worker.h.
 */
#include "worker.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "conn.h"
#include "diag.h"
#include "listener.h"
#include "log.h"

#define ACCEPT_RETRY_MS 100

/* Único escritor del self-pipe; lo lee el bucle. */
static volatile sig_atomic_t g_sig_fd = -1;

static void on_signal(int sig) {
    int saved = errno;
    unsigned char b = (unsigned char)sig;
    if (g_sig_fd >= 0) {
        ssize_t r = write(g_sig_fd, &b, 1); /* lleno = ya hay aviso pendiente */
        (void)r;
    }
    errno = saved;
}

/* SIGHUP y SIGUSR1 se ignoran en el worker: la recarga y las estadísticas
 * llegan del maestro por IPC. */
static const int HANDLED_SIGNALS[] = {SIGTERM, SIGINT};
static const int IGNORED_SIGNALS[] = {SIGHUP, SIGUSR1, SIGPIPE};
#define NSIGNALS (sizeof(HANDLED_SIGNALS) / sizeof(HANDLED_SIGNALS[0]))

static int make_pipe(int *rd, int *wr) {
    int p[2];
    if (pipe(p) < 0) {
        return -1;
    }
    *rd = p[0];
    *wr = p[1];
    return fd_set_nonblock_cloexec(p[0]) < 0 || fd_set_nonblock_cloexec(p[1]) < 0 ? -1 : 0;
}

static int install_signals(struct worker *w) {
    if (make_pipe(&w->sig_rd, &w->sig_wr) < 0) {
        return -1;
    }
    g_sig_fd = w->sig_wr;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    for (size_t i = 0; i < NSIGNALS; i++) {
        if (sigaction(HANDLED_SIGNALS[i], &sa, NULL) < 0) {
            return -1;
        }
    }
    struct sigaction ign;
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    for (size_t i = 0; i < sizeof(IGNORED_SIGNALS) / sizeof(IGNORED_SIGNALS[0]); i++) {
        if (sigaction(IGNORED_SIGNALS[i], &ign, NULL) < 0) {
            return -1;
        }
    }
    /* El maestro bloquea señales durante el fork: el worker empieza sin máscara. */
    sigset_t none;
    sigemptyset(&none);
    return pthread_sigmask(SIG_SETMASK, &none, NULL) == 0 ? 0 : -1;
}

static void close_fd(int *fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void restore_signals(struct worker *w) {
    struct sigaction dfl;
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    for (size_t i = 0; i < NSIGNALS; i++) {
        (void)sigaction(HANDLED_SIGNALS[i], &dfl, NULL);
    }
    g_sig_fd = -1;
    if (w->sig_rd >= 0) {
        (void)io_loop_del(w->loop, w->sig_rd);
    }
    close_fd(&w->sig_rd);
    close_fd(&w->sig_wr);
}

/* Ejecuta fn en un hilo nuevo con todas las señales bloqueadas. */
static int spawn_thread(pthread_t *th, void *(*fn)(void *), void *arg) {
    sigset_t all;
    sigset_t old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    int rc = pthread_create(th, NULL, fn, arg);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    return rc;
}

static const struct config *current_cfg(const struct worker *w) {
    return backend_pools_config(w->gen);
}

/* ------------------------------------------------------------------------- */
/* Salud                                                                     */
/* ------------------------------------------------------------------------- */

void worker_log_transition(struct worker *w, const backend_pools *gen, uint32_t pool,
                           uint32_t backend, enum bp_transition tr, const char *why) {
    (void)w;
    if (tr == BP_NO_CHANGE) {
        return;
    }
    const struct config *cfg = backend_pools_config(gen);
    plog(tr == BP_WENT_DOWN ? LOG_WARN : LOG_INFO, backend_pools_id(gen),
         "health: generación %llu pool=%s backend=%s %s (%s)",
         (unsigned long long)backend_pools_id(gen), cfg->pools[pool].name,
         cfg->pools[pool].backends[backend].addr.text, tr == BP_WENT_DOWN ? "CAÍDO" : "SANO", why);
}

static void on_health_io(io_loop *loop, int fd, uint32_t events, void *ud) {
    (void)loop;
    (void)fd;
    (void)events;
    struct worker *w = ud;
    struct health_result res[64];
    size_t n;
    while ((n = health_take_results(w->health, res, 64)) > 0) {
        for (size_t i = 0; i < n; i++) {
            w->stats.health_results++;
            if (res[i].gen != backend_pools_id(w->gen)) {
                w->stats.health_stale++; /* sonda de una generación anterior */
                continue;
            }
            enum bp_transition tr =
                backend_pool_probe_result(w->gen, res[i].pool, res[i].backend, res[i].ok);
            char why[96];
            char eb[64];
            if (res[i].ok) {
                (void)snprintf(why, sizeof(why), "activo: sonda correcta");
            } else if (res[i].status != 0) {
                (void)snprintf(why, sizeof(why), "activo: estado HTTP %u", res[i].status);
            } else {
                (void)snprintf(why, sizeof(why), "activo: %s",
                               diag_strerror(res[i].err, eb, sizeof(eb)));
            }
            worker_log_transition(w, w->gen, res[i].pool, res[i].backend, tr, why);
        }
    }
}

static void submit_health_plan(struct worker *w) {
    struct health_plan *plan = health_plan_build(current_cfg(w), backend_pools_id(w->gen));
    if (plan == NULL) {
        diag("proxy: sin memoria para el plan de health checks\n");
        return;
    }
    health_submit_plan(w->health, plan);
}

/* ------------------------------------------------------------------------- */
/* Recarga coordinada por el maestro (IPC)                                   */
/* ------------------------------------------------------------------------- */

static void ctl_send(struct worker *w, uint32_t type, uint64_t gen, uint64_t id, const void *data,
                     size_t len) {
    if (w->ctl.fd < 0) {
        return;
    }
    if (ipc_queue(&w->ctl, type, gen, id, data, len) < 0) {
        plog(LOG_ERROR, gen, "ipc: no se pudo encolar el mensaje %u", type);
        return;
    }
    (void)ipc_flush(&w->ctl); /* lo pendiente sale con IO_WRITE */
}

static void prepare_reply_error(struct worker *w, uint64_t gen, const char *why) {
    plog(LOG_WARN, gen, "preparación de la generación %llu rechazada: %s", (unsigned long long)gen,
         why);
    w->stats.reloads_failed++;
    ctl_send(w, IPC_PREPARED_ERR, gen, 0, why, strlen(why));
}

/*
 * Hilo de preparación: analiza los bytes recibidos del maestro. Escribe solo
 * en su job; marca `done` (release) y avisa por el pipe como última acción.
 */
static void *reload_main(void *arg) {
    struct reload_job *job = arg;
    if (job->delay_ms > 0) {
        struct timespec ts = {.tv_sec = job->delay_ms / 1000,
                              .tv_nsec = (long)(job->delay_ms % 1000) * 1000000L};
        while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
        }
    }
    char origin[64];
    (void)snprintf(origin, sizeof(origin), "generación %llu", (unsigned long long)job->gen);
    job->result = config_load_string(job->text, origin, job->err, sizeof(job->err));
    atomic_store_explicit(&job->done, true, memory_order_release);
    unsigned char b = 1;
    ssize_t r = write(job->done_wr, &b, 1); /* última acción: después solo retorna */
    (void)r;
    return NULL;
}

static void begin_prepare(struct worker *w, uint64_t gen, const char *data, size_t len) {
    if (w->draining) {
        prepare_reply_error(w, gen, "cierre en curso");
        return;
    }
    if (w->reload != NULL && w->reload->aborted && w->candidate == NULL) {
        /* El hilo de una preparación abortada aún no ha terminado (no se le
         * puede interrumpir): la nueva espera a que acabe, sin bloquear. */
        char *copy = malloc(len + 1);
        if (copy == NULL) {
            prepare_reply_error(w, gen, "sin memoria");
            return;
        }
        memcpy(copy, data, len);
        copy[len] = '\0';
        free(w->pending_text);
        w->pending_text = copy;
        w->pending_len = len;
        w->pending_gen = gen;
        plog(LOG_INFO, gen, "preparación %llu en espera de que termine la abortada",
             (unsigned long long)gen);
        return;
    }
    if (w->reload != NULL || w->candidate != NULL) {
        prepare_reply_error(w, gen, "ya hay una preparación en curso");
        return;
    }
    struct reload_job *job = calloc(1, sizeof(*job));
    char *text = malloc(len + 1);
    if (job == NULL || text == NULL) {
        free(job);
        free(text);
        prepare_reply_error(w, gen, "sin memoria");
        return;
    }
    memcpy(text, data, len);
    text[len] = '\0';
    job->text = text;
    job->gen = gen;
    job->delay_ms = w->reload_delay_ms;
    atomic_init(&job->done, false);
    job->done_wr = w->reload_wr;
    int rc = spawn_thread(&job->thread, reload_main, job);
    if (rc != 0) {
        free(text);
        free(job);
        prepare_reply_error(w, gen, "no se pudo crear el hilo de preparación");
        return;
    }
    w->reload = job;
    plog(LOG_INFO, gen, "preparando la generación %llu", (unsigned long long)gen);
}

/*
 * Recoge el trabajo del hilo de preparación SOLO si ya lo terminó (`done`).
 * El pthread_join que sigue solo espera a que el hilo retorne tras su última
 * escritura. NULL si no hay o aún no ha terminado.
 */
static struct reload_job *collect_reload(struct worker *w) {
    unsigned char buf[16];
    while (read(w->reload_rd, buf, sizeof(buf)) > 0) {
    }
    struct reload_job *job = w->reload;
    if (job == NULL || !atomic_load_explicit(&job->done, memory_order_acquire)) {
        return NULL;
    }
    pthread_join(job->thread, NULL);
    w->reload = NULL;
    return job;
}

static void drop_candidate(struct worker *w, const char *why) {
    if (w->candidate == NULL) {
        return;
    }
    plog(LOG_INFO, backend_pools_id(w->candidate), "candidata %llu descartada (%s)",
         (unsigned long long)backend_pools_id(w->candidate), why);
    timer_cancel(&w->timers, &w->candidate_timer);
    backend_pools_unref(w->candidate); /* suelta sus entradas nuevas del registro */
    w->candidate = NULL;
}

static void on_candidate_timeout(timer_heap *heap, timer *t, void *ud) {
    (void)heap;
    (void)t;
    drop_candidate(ud, "el maestro no la activó a tiempo");
}

static void on_reload_done(io_loop *loop, int fd, uint32_t events, void *ud) {
    (void)loop;
    (void)fd;
    (void)events;
    struct worker *w = ud;
    struct reload_job *job = collect_reload(w);
    if (job == NULL) {
        return;
    }
    struct config *cfg = job->result;
    char why[CONFIG_ERR_LEN];
    if (job->aborted) {
        plog(LOG_INFO, job->gen, "preparación %llu abortada por el maestro",
             (unsigned long long)job->gen);
        config_unref(cfg);
    } else if (w->draining) {
        config_unref(cfg);
        prepare_reply_error(w, job->gen, "cierre en curso");
    } else if (cfg == NULL) {
        prepare_reply_error(w, job->gen, job->err);
    } else if (!config_reload_compatible(current_cfg(w), cfg, why, sizeof(why))) {
        config_unref(cfg);
        prepare_reply_error(w, job->gen, why);
    } else {
        w->candidate = backend_pools_create(cfg, job->gen, w->registry);
        config_unref(cfg); /* la candidata tiene su propia referencia */
        if (w->candidate == NULL) {
            prepare_reply_error(w, job->gen, "sin memoria");
        } else {
            (void)timer_schedule_in(&w->timers, &w->candidate_timer, timer_now_ms(),
                                    2 * (uint64_t)current_cfg(w)->ipc_timeout_ms);
            plog(LOG_INFO, job->gen, "generación %llu preparada", (unsigned long long)job->gen);
            ctl_send(w, IPC_PREPARED_OK, job->gen, 0, NULL, 0);
        }
    }
    free(job->text);
    free(job);
    if (w->pending_text != NULL) {
        char *text = w->pending_text;
        w->pending_text = NULL;
        begin_prepare(w, w->pending_gen, text, w->pending_len);
        free(text);
    }
}

/* Activa la candidata: las conexiones nuevas (y las siguientes peticiones de
 * las keep-alive) la adoptan; las peticiones en curso siguen con la suya. */
static void commit(struct worker *w, uint64_t gen) {
    if (w->candidate == NULL || backend_pools_id(w->candidate) != gen) {
        plog(LOG_WARN, gen, "activación de %llu sin candidata preparada", (unsigned long long)gen);
        ctl_send(w, IPC_COMMIT_FAILED, gen, 0, NULL, 0);
        return;
    }
    timer_cancel(&w->timers, &w->candidate_timer);
    backend_pools *old = w->gen;
    w->gen = w->candidate;
    w->candidate = NULL;
    backend_pools_unref(old); /* se libera cuando la suelten sus conexiones */
    submit_health_plan(w);
    w->stats.reloads_ok++;
    plog(LOG_INFO, gen, "generación %llu activada", (unsigned long long)gen);
    ctl_send(w, IPC_COMMITTED, gen, 0, NULL, 0);
}

static void abort_prepare(struct worker *w, uint64_t gen) {
    if (w->pending_text != NULL && w->pending_gen == gen) {
        free(w->pending_text);
        w->pending_text = NULL;
    }
    if (w->reload != NULL && w->reload->gen == gen) {
        w->reload->aborted = true; /* el resultado se descartará al recogerlo */
    }
    if (w->candidate != NULL && backend_pools_id(w->candidate) == gen) {
        drop_candidate(w, "abortada por el maestro");
    }
}

/* ------------------------------------------------------------------------- */
/* Listeners y accept                                                        */
/* ------------------------------------------------------------------------- */

static void close_listeners(struct worker *w) {
    for (size_t i = 0; i < w->nlisten; i++) {
        if (w->listen_fds[i] >= 0) {
            (void)io_loop_del(w->loop, w->listen_fds[i]);
            close_fd(&w->listen_fds[i]);
        }
    }
    timer_cancel(&w->timers, &w->accept_retry);
}

/*
 * Sin descriptores (EMFILE/ENFILE): se libera el fd de reserva, se acepta la
 * conexión pendiente y se cierra de inmediato, y se recupera la reserva. Así
 * el cliente recibe un cierre en lugar de quedar colgado en la cola, y el
 * listener edge-triggered se vacía sin bucles. Devuelve false si no se pudo
 * (sin reserva): entonces se reintenta con un temporizador de 100 ms, nunca
 * en bucle.
 */
static bool shed_one(struct worker *w, int lfd) {
    if (w->spare_fd < 0) {
        return false;
    }
    close_fd(&w->spare_fd);
    int fd = accept(lfd, NULL, NULL);
    if (fd >= 0) {
        close(fd);
        w->stats.rejected++;
    }
    w->spare_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    return fd >= 0;
}

static void accept_on(struct worker *w, size_t idx) {
    int lfd = w->listen_fds[idx];
    while (lfd >= 0 && !w->draining) {
        struct sockaddr_storage peer;
        socklen_t plen = sizeof(peer);
        int fd = accept(lfd, (struct sockaddr *)&peer, &plen);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            if (errno == EMFILE || errno == ENFILE) {
                w->stats.accept_emfile++;
                if (shed_one(w, lfd)) {
                    continue;
                }
                (void)timer_schedule_in(&w->timers, &w->accept_retry, timer_now_ms(),
                                        ACCEPT_RETRY_MS);
            } else if (errno == ENOBUFS || errno == ENOMEM) {
                (void)timer_schedule_in(&w->timers, &w->accept_retry, timer_now_ms(),
                                        ACCEPT_RETRY_MS);
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                char eb[64];
                diag("proxy: accept en %s: %s\n", current_cfg(w)->frontends[idx].listen.text,
                     diag_strerror(errno, eb, sizeof(eb)));
            }
            return;
        }
        w->stats.accepted++;
        int one = 1;
        if (fd_set_nonblock_cloexec(fd) < 0) {
            close(fd);
            continue;
        }
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
        (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        if (client_conn_accept(w, fd, &peer, idx) < 0) {
            w->stats.rejected++;
            close(fd); /* max_connections o sin slots */
        }
    }
}

static void on_listen_io(io_loop *loop, int fd, uint32_t events, void *ud) {
    (void)loop;
    (void)events;
    struct worker *w = ud;
    for (size_t i = 0; i < w->nlisten; i++) {
        if (w->listen_fds[i] == fd) {
            accept_on(w, i);
        }
    }
}

static void on_accept_retry(timer_heap *heap, timer *t, void *ud) {
    (void)heap;
    (void)t;
    struct worker *w = ud;
    if (w->spare_fd < 0) {
        w->spare_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    }
    for (size_t i = 0; i < w->nlisten; i++) {
        accept_on(w, i);
    }
}

/* ------------------------------------------------------------------------- */
/* Instantánea, contabilidad y cierre                                        */
/* ------------------------------------------------------------------------- */

static size_t slots_in_use(const struct worker *w) {
    return buffer_pool_capacity(w->bufs) - buffer_pool_available(w->bufs);
}

static const char *health_name(enum bp_health h) {
    switch (h) {
    case BP_UP:
        return "up";
    case BP_DOWN_ACTIVE:
        return "down-active";
    case BP_DOWN_PASSIVE:
        return "down-passive";
    }
    return "?";
}

struct text {
    char *p;
    size_t len;
    size_t cap;
    bool oom;
};

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
static void textf(struct text *t, const char *fmt, ...) {
    if (t->oom) {
        return;
    }
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        t->oom = true;
        return;
    }
    if (t->len + (size_t)n + 1 > t->cap) {
        size_t nc = t->cap ? t->cap * 2 : 4096;
        while (nc < t->len + (size_t)n + 1) {
            nc *= 2;
        }
        char *np = realloc(t->p, nc);
        if (np == NULL) {
            t->oom = true;
            return;
        }
        t->p = np;
        t->cap = nc;
    }
    memcpy(t->p + t->len, tmp, (size_t)n);
    t->len += (size_t)n;
}

/* Instantánea "clave=valor" para el maestro (stats.h). */
static void send_snapshot(struct worker *w, uint64_t req_id) {
    const struct worker_stats *s = &w->stats;
    struct log_counters lc = {0};
    if (w->log != NULL) {
        log_get_counters(w->log, &lc);
    }
    struct text t = {0};
    textf(&t,
          "worker index=%d pid=%ld generation=%llu uptime_ms=%llu conns=%zu upstreams=%zu "
          "slots_in_use=%zu slots_total=%zu backend_active=%llu generations_live=%zu "
          "backends_live=%zu draining=%d reload_running=%d accepted=%llu rejected=%llu "
          "accept_emfile=%llu requests=%llu proxy_4xx=%llu proxy_5xx=%llu "
          "upstream_reusable=%llu reloads_ok=%llu reloads_failed=%llu health_results=%llu "
          "health_stale=%llu health_dropped=%llu log_written=%llu log_dropped=%llu "
          "log_write_errors=%llu log_truncated=%llu\n",
          w->index, (long)getpid(), (unsigned long long)backend_pools_id(w->gen),
          (unsigned long long)(timer_now_ms() - w->start_ms), w->nconns, w->nupstreams,
          slots_in_use(w), buffer_pool_capacity(w->bufs),
          (unsigned long long)backend_pools_active_total(w->gen), backend_pools_live(),
          backend_registry_size(w->registry), w->draining ? 1 : 0,
          (w->reload != NULL || w->candidate != NULL) ? 1 : 0, (unsigned long long)s->accepted,
          (unsigned long long)s->rejected, (unsigned long long)s->accept_emfile,
          (unsigned long long)s->requests, (unsigned long long)s->responses_4xx,
          (unsigned long long)s->responses_5xx, (unsigned long long)s->upstream_reusable,
          (unsigned long long)s->reloads_ok, (unsigned long long)s->reloads_failed,
          (unsigned long long)s->health_results, (unsigned long long)s->health_stale,
          (unsigned long long)(w->health ? health_results_dropped(w->health) : 0),
          (unsigned long long)lc.written, (unsigned long long)lc.dropped,
          (unsigned long long)lc.write_errors, (unsigned long long)lc.truncated);
    const struct config *cfg = current_cfg(w);
    for (size_t i = 0; i < cfg->npools; i++) {
        const struct cfg_pool *p = &cfg->pools[i];
        for (size_t j = 0; j < p->nbackends; j++) {
            const struct backend_state *st = backend_pool_state(w->gen, (uint32_t)i, (uint32_t)j);
            textf(&t,
                  "backend pool=%s addr=%s algo=%s weight=%u max_conns=%u active=%u selected=%llu "
                  "failures=%llu health=%s\n",
                  p->name, p->backends[j].addr.text, lb_algorithm_name(p->algorithm),
                  p->backends[j].weight, p->backends[j].max_conns, st->active,
                  (unsigned long long)st->selected, (unsigned long long)st->connect_failures,
                  p->health.type == HEALTH_NONE ? "up" : health_name(st->health));
        }
    }
    if (!t.oom) {
        ctl_send(w, IPC_STATS, backend_pools_id(w->gen), req_id, t.p, t.len);
    }
    free(t.p);
}

static void on_ctl_msg(void *ctx, const struct ipc_msg *m) {
    struct worker *w = ctx;
    switch (m->type) {
    case IPC_PREPARE:
        begin_prepare(w, m->gen, m->data, m->len);
        break;
    case IPC_COMMIT:
        if (w->hold_commit_gen != 0 && m->gen == w->hold_commit_gen) {
            /* Solo en proxy-testhooks (hold_commit_gen es siempre 0 en
             * producción): el worker queda en la fase de activación, tras la
             * decisión del maestro y sin confirmar. */
            plog(LOG_WARN, m->gen, "VARIANTE DE PRUEBAS: activación de la generación %llu retenida",
                 (unsigned long long)m->gen);
            break;
        }
        if (w->fail_commit_gen != 0 && m->gen == w->fail_commit_gen) {
            plog(LOG_WARN, m->gen, "VARIANTE DE PRUEBAS: activación de la generación %llu fallida",
                 (unsigned long long)m->gen);
            drop_candidate(w, "fallo forzado de la activación");
            ctl_send(w, IPC_COMMIT_FAILED, m->gen, 0, NULL, 0);
            break;
        }
        commit(w, m->gen);
        break;
    case IPC_ABORT:
        abort_prepare(w, m->gen);
        break;
    case IPC_STATS_REQ:
        send_snapshot(w, m->id);
        break;
    default:
        plog(LOG_WARN, 0, "ipc: mensaje desconocido %u del maestro", m->type);
        break;
    }
}

static void begin_drain(struct worker *w);

static void on_ctl_io(io_loop *loop, int fd, uint32_t events, void *ud) {
    (void)loop;
    (void)fd;
    struct worker *w = ud;
    int rc = 0;
    if (events & (IO_READ | IO_HUP | IO_ERROR)) {
        rc = ipc_read(&w->ctl, on_ctl_msg, w);
    }
    if (rc == 0 && (events & (IO_WRITE | IO_ERROR)) && ipc_flush(&w->ctl) < 0) {
        rc = -1;
    }
    if (rc < 0 && w->ctl.fd >= 0) {
        /* El maestro ya no está (o el canal se rompió): cierre ordenado. */
        plog(LOG_ERROR, 0, "canal con el maestro cerrado: se inicia el cierre");
        (void)io_loop_del(w->loop, w->ctl.fd);
        close_fd(&w->ctl.fd);
        if (!w->draining) {
            begin_drain(w);
        }
    }
}

static void on_drain_timeout(timer_heap *heap, timer *t, void *ud) {
    (void)heap;
    (void)t;
    struct worker *w = ud;
    plog(LOG_WARN, 0, "plazo de cierre agotado; se cierran %zu conexiones", w->nconns);
    client_conn_abort_all(w);
    w->stop = true;
}

static void begin_drain(struct worker *w) {
    if (w->draining) {
        plog(LOG_WARN, 0, "segundo aviso; cierre forzado");
        client_conn_abort_all(w);
        w->stop = true;
        return;
    }
    uint32_t timeout = current_cfg(w)->shutdown_timeout_ms;
    plog(LOG_INFO, 0, "cierre ordenado (%zu conexiones activas, plazo %u ms)", w->nconns, timeout);
    w->draining = true;
    close_listeners(w);
    drop_candidate(w, "cierre en curso");
    client_conn_drain_all(w);
    if (w->nconns == 0) {
        w->stop = true;
        return;
    }
    (void)timer_schedule_in(&w->timers, &w->drain_timer, timer_now_ms(), timeout);
}

static void on_signal_io(io_loop *loop, int fd, uint32_t events, void *ud) {
    (void)loop;
    (void)events;
    struct worker *w = ud;
    unsigned char sigs[32];
    ssize_t n;
    while ((n = read(fd, sigs, sizeof(sigs))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (sigs[i] == SIGTERM || sigs[i] == SIGINT) {
                begin_drain(w);
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Ejecución                                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Cierra todo y comprueba la contabilidad antes de liberar los módulos que
 * la guardan. Devuelve true si no queda nada prestado.
 */
static bool worker_cleanup(struct worker *w) {
    close_listeners(w);
    client_conn_abort_all(w);
    timer_cancel(&w->timers, &w->drain_timer);
    restore_signals(w);
    if (w->reload != NULL) {
        /* Solo tras un error fatal del bucle: el camino normal no llega aquí
         * con una preparación en curso (el bucle espera su aviso). */
        plog(LOG_WARN, 0, "esperando al hilo de recarga tras un error del bucle");
        pthread_join(w->reload->thread, NULL);
        config_unref(w->reload->result);
        free(w->reload->text);
        free(w->reload);
        w->reload = NULL;
    }
    free(w->pending_text);
    w->pending_text = NULL;
    drop_candidate(w, "cierre");
    if (w->health != NULL) {
        (void)io_loop_del(w->loop, health_notify_fd(w->health));
        health_stop(w->health);
        w->health = NULL;
    }
    if (w->ctl.fd >= 0) {
        (void)io_loop_del(w->loop, w->ctl.fd);
        close_fd(&w->ctl.fd);
    }
    ipc_free(&w->ctl);

    plog(LOG_INFO, 0,
         "shutdown conns=%zu upstreams=%zu slots_in_use=%zu slots_total=%zu backend_active=%llu "
         "generations_live=%zu backends_live=%zu",
         w->nconns, w->nupstreams, slots_in_use(w), buffer_pool_capacity(w->bufs),
         (unsigned long long)backend_pools_active_total(w->gen), backend_pools_live(),
         backend_registry_size(w->registry));
    bool clean = slots_in_use(w) == 0 && w->nconns == 0 && w->nupstreams == 0 &&
                 backend_pools_active_total(w->gen) == 0 && backend_pools_live() == 1;

    backend_pools_unref(w->gen);
    w->gen = NULL;
    plog(LOG_INFO, 0, "shutdown final generations_live=%zu backends_live=%zu", backend_pools_live(),
         backend_registry_size(w->registry));
    clean = clean && backend_pools_live() == 0 && backend_registry_size(w->registry) == 0;
    backend_registry_destroy(w->registry);
    w->registry = NULL;
    if (w->reload_rd >= 0) {
        (void)io_loop_del(w->loop, w->reload_rd);
    }
    close_fd(&w->reload_rd);
    close_fd(&w->reload_wr);
    close_fd(&w->spare_fd);
    free(w->listen_fds);
    buffer_pool_destroy(w->bufs);
    timer_heap_destroy(&w->timers);
    io_loop_destroy(w->loop);
    return clean;
}

/*
 * Retardo artificial de la preparación, solo en la variante de pruebas
 * `proxy-testhooks` (compilada con PROXY_TEST_HOOKS). El ejecutable `proxy`
 * no contiene este código ni el nombre de la variable.
 */
static uint32_t test_reload_delay(void) {
#ifdef PROXY_TEST_HOOKS
    /* Se lee antes de crear ningún hilo: getenv no compite con setenv. */
    const char *v = getenv("PROXY_TEST_RELOAD_DELAY_MS"); // NOLINT(concurrency-mt-unsafe)
    if (v == NULL) {
        return 0;
    }
    long ms = strtol(v, NULL, 10);
    uint32_t delay = ms > 0 && ms < 60000 ? (uint32_t)ms : 0;
    plog(LOG_WARN, 0, "VARIANTE DE PRUEBAS: retardo de recarga %u ms", delay);
    return delay;
#else
    return 0;
#endif
}

/*
 * Ganchos de la activación, solo en `proxy-testhooks` (valor
 * "<índice de worker>:<generación>"; afectan a una sola generación):
 *   PROXY_TEST_HOLD_COMMIT  ese worker recibe el COMMIT y no lo aplica ni lo
 *                           confirma (retenido tras la decisión del maestro);
 *   PROXY_TEST_FAIL_COMMIT  ese worker descarta su candidata y responde
 *                           COMMIT_FAILED.
 * En `proxy` las funciones devuelven 0 y los nombres no existen.
 */
#ifdef PROXY_TEST_HOOKS
static uint64_t test_commit_hook(const char *name, int index) {
    const char *v = getenv(name); // NOLINT(concurrency-mt-unsafe)
    if (v == NULL) {
        return 0;
    }
    char *end = NULL;
    long wi = strtol(v, &end, 10);
    if (end == NULL || *end != ':' || wi != index) {
        return 0;
    }
    unsigned long long gen = strtoull(end + 1, NULL, 10);
    if (gen == 0) {
        return 0;
    }
    plog(LOG_WARN, 0, "VARIANTE DE PRUEBAS: %s para la generación %llu", name, gen);
    return (uint64_t)gen;
}
#endif

static int worker_start(struct worker *w) {
    const struct config *cfg = current_cfg(w);
    char eb[64];
    if (install_signals(w) < 0 || io_loop_add(w->loop, w->sig_rd, IO_READ, on_signal_io, w) < 0 ||
        make_pipe(&w->reload_rd, &w->reload_wr) < 0 ||
        io_loop_add(w->loop, w->reload_rd, IO_READ, on_reload_done, w) < 0) {
        plog(LOG_ERROR, 0, "señales: %s", diag_strerror(errno, eb, sizeof(eb)));
        return 1;
    }
    if (w->ctl.fd >= 0 && (fd_set_nonblock_cloexec(w->ctl.fd) < 0 ||
                           io_loop_add(w->loop, w->ctl.fd, IO_READ | IO_WRITE, on_ctl_io, w) < 0)) {
        plog(LOG_ERROR, 0, "canal IPC: %s", diag_strerror(errno, eb, sizeof(eb)));
        return 1;
    }
    for (size_t i = 0; i < w->nlisten; i++) {
        char err[256];
        if (w->listen_fds[i] < 0) {
            w->listen_fds[i] = listener_open(&cfg->frontends[i], true, err, sizeof(err));
        }
        if (w->listen_fds[i] < 0) {
            plog(LOG_ERROR, 0, "no se puede escuchar en %s", err);
            return 1;
        }
        if (io_loop_add(w->loop, w->listen_fds[i], IO_READ, on_listen_io, w) < 0) {
            plog(LOG_ERROR, 0, "io_loop_add: %s", diag_strerror(errno, eb, sizeof(eb)));
            return 1;
        }
        plog(LOG_INFO, 0, "escuchando en %s (%s)", cfg->frontends[i].listen.text,
             w->shared_listeners ? "socket compartido heredado del maestro"
                                 : "socket propio con SO_REUSEPORT");
    }
    w->health = health_start();
    if (w->health == NULL ||
        io_loop_add(w->loop, health_notify_fd(w->health), IO_READ, on_health_io, w) < 0) {
        plog(LOG_ERROR, 0, "no se pudo iniciar el hilo de health checks: %s",
             diag_strerror(errno, eb, sizeof(eb)));
        return 1;
    }
    submit_health_plan(w);
    return 0;
}

/* Abre el log del worker: <dir>/worker-<i>.log o stderr. */
static logger *open_worker_log(const struct config *cfg, int index) {
    char path[CONFIG_PATH_LEN + 32];
    const char *p = NULL;
    if (cfg->log.dir[0] != '\0') {
        (void)snprintf(path, sizeof(path), "%s/worker-%d.log", cfg->log.dir, index);
        p = path;
    }
    char err[256];
    logger *lg = log_open(p, cfg->log.level, index, err, sizeof(err));
    if (lg == NULL) {
        plog(LOG_ERROR, 0, "worker %d: log: %s", index, err); /* síncrono a stderr */
    }
    return lg;
}

/* Cierra los listeners heredados que el worker no llegó a adoptar. */
static void close_inherited(const int *fds, size_t n) {
    for (size_t i = 0; fds != NULL && i < n; i++) {
        if (fds[i] >= 0) {
            close(fds[i]);
        }
    }
}

int worker_run(struct config *cfg, uint64_t gen_id, int ctl_fd, int index, const int *shared_fds) {
    log_set_role(index);
    logger *lg = open_worker_log(cfg, index);
    if (lg == NULL) {
        close_inherited(shared_fds, cfg->nfrontends);
        return 1;
    }
    log_install(lg);

    struct worker w;
    memset(&w, 0, sizeof(w));
    w.index = index;
    w.log = lg;
    w.start_ms = timer_now_ms();
    w.sig_rd = w.sig_wr = w.reload_rd = w.reload_wr = -1;
    ipc_init(&w.ctl, ctl_fd);
    w.reload_delay_ms = test_reload_delay();
#ifdef PROXY_TEST_HOOKS
    w.hold_commit_gen = test_commit_hook("PROXY_TEST_HOLD_COMMIT", index);
    w.fail_commit_gen = test_commit_hook("PROXY_TEST_FAIL_COMMIT", index);
#endif
    timer_heap_init(&w.timers);
    timer_init(&w.accept_retry, on_accept_retry, &w);
    timer_init(&w.drain_timer, on_drain_timeout, &w);
    timer_init(&w.candidate_timer, on_candidate_timeout, &w);

    /* 2 slots por cliente + 2 por upstream: el pool no se agota antes que
     * max_connections salvo por un error de contabilidad. */
    w.loop = io_loop_create(256);
    w.bufs = buffer_pool_create((size_t)cfg->max_connections * 4);
    w.registry = backend_registry_create();
    w.gen = w.registry != NULL ? backend_pools_create(cfg, gen_id, w.registry) : NULL;
    w.listen_fds = calloc(cfg->nfrontends, sizeof(int));
    w.spare_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    int rc = 1;
    if (w.loop == NULL || w.bufs == NULL || w.gen == NULL || w.listen_fds == NULL) {
        char eb[64];
        plog(LOG_ERROR, 0, "sin recursos para iniciar: %s", diag_strerror(errno, eb, sizeof(eb)));
        free(w.listen_fds);
        close_inherited(shared_fds, cfg->nfrontends);
        backend_pools_unref(w.gen);
        backend_registry_destroy(w.registry);
        buffer_pool_destroy(w.bufs);
        io_loop_destroy(w.loop);
        close_fd(&w.spare_fd);
        close_fd(&w.ctl.fd);
        (void)log_stop(lg, 2000);
        return 1;
    }
    w.nlisten = cfg->nfrontends;
    for (size_t i = 0; i < w.nlisten; i++) {
        /* Modelo compartido: el worker adopta su copia heredada del maestro
         * (la cierra al dejar de aceptar); si no, abre la suya. */
        w.listen_fds[i] = shared_fds != NULL ? shared_fds[i] : -1;
    }
    w.shared_listeners = shared_fds != NULL;

    rc = worker_start(&w);
    if (rc == 0) {
        plog(LOG_INFO, gen_id, "worker %d listo (pid %ld, generación %llu)", index, (long)getpid(),
             (unsigned long long)gen_id);
        char pid[32];
        int n = snprintf(pid, sizeof(pid), "%ld", (long)getpid());
        ctl_send(&w, IPC_READY, gen_id, 0, pid, n > 0 ? (size_t)n : 0);
        /* Con una preparación en curso el bucle sigue hasta recibir su aviso:
         * nunca se bloquea esperando al hilo. */
        while (!w.stop || w.reload != NULL) {
            if (timer_loop_run_once(w.loop, &w.timers) < 0) {
                char eb[64];
                plog(LOG_ERROR, 0, "bucle de eventos: %s", diag_strerror(errno, eb, sizeof(eb)));
                rc = 1;
                break;
            }
        }
    }
    if (!worker_cleanup(&w)) {
        plog(LOG_ERROR, 0, "ERROR de contabilidad al salir: quedan recursos prestados");
        rc = 70;
    }
    /* Con el destino del log bloqueado log_stop abandona tras el plazo y el
     * proceso termina igualmente (el hilo muere con él). */
    (void)log_stop(lg, 2000);
    return rc;
}
