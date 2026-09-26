/*
 * master.c — ver master.h y docs/architecture.md §14.
 */
#include "master.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "diag.h"
#include "io_event.h"
#include "ipc.h"
#include "listener.h"
#include "log.h"
#include "stats.h"
#include "timer.h"
#include "worker.h"

#define KILL_GRACE_MS  5000u /* margen sobre shutdown_timeout_ms antes de SIGKILL */
#define BACKOFF_MAX_MS 30000u
#define POSTPONE_MS    100u /* reposición aplazada mientras hay una recarga */

/* SLOT_RETIRING: retirado por no confirmar una activación; recibió SIGTERM y
 * tiene un plazo (retire_timer) antes de SIGKILL. Se repone al recogerlo. */
enum slot_state {
    SLOT_EMPTY,
    SLOT_STARTING,
    SLOT_READY,
    SLOT_RESTARTING,
    SLOT_RETIRING,
    SLOT_FAILED
};
/* Recuperación tras una activación parcial (la última recarga aplicada). */
enum recovery_state { REC_NONE, REC_PENDING, REC_COMPLETE, REC_EXHAUSTED };
enum reload_state { R_IDLE, R_PREPARING, R_COMMITTING };

struct master;

struct slot {
    struct master *m;
    int index;
    pid_t pid;
    enum slot_state state;
    struct ipc_chan ipc; /* fd -1 sin canal */
    uint64_t gen;        /* generación con la que corre */
    uint32_t restarts;
    uint64_t window_start;
    uint32_t window_count;
    uint32_t backoff_ms;
    timer restart_timer;
    timer ready_timer;
    timer retire_timer;
    bool kill_sent;   /* SIGKILL ya enviado a este pid */
    bool recovering;  /* debe volver con la generación comprometida */
    bool participant; /* en la recarga en curso */
    bool prepared;
    bool committed;
};

struct stats_client {
    struct master *m;
    struct stats_client *prev;
    struct stats_client *next;
    int fd;
    uint64_t req_id;
    bool collecting;
    bool *asked;
    char **texts;
    size_t *lens;
    size_t expected;
    size_t received;
    char *out;
    size_t off;
    size_t len;
    timer collect_timer;
    timer deadline;
};

struct master {
    struct config *cfg;
    char *text;
    size_t text_len;
    uint64_t gen;
    const char *path;
    io_loop *loop;
    timer_heap timers;
    int sig_rd;
    int sig_wr;
    struct slot *slots;
    size_t nslots;
    enum listener_model lmodel;
    int *listen_fds; /* modelo compartido: uno por frontend; NULL si por worker */
    size_t nlisten;
    int stats_fd;
    dev_t stats_dev;
    ino_t stats_ino;
    bool stats_own;
    struct stats_client *clients;
    size_t nclients;
    uint64_t next_req_id;
    enum reload_state rstate;
    uint64_t rgen;
    struct config *rcfg;
    char *rtext;
    size_t rlen;
    size_t replaced;    /* participantes que no confirmaron la activación */
    uint32_t retire_ms; /* plazo de retirada fijado al decidir la activación */
    enum recovery_state recovery;
    uint64_t recovery_gen;
    timer rtimer;
    timer rkick; /* arranca la recarga pendiente desde el bucle (sin recursión) */
    bool reload_pending;
    bool starting;
    timer start_timer;
    bool draining;
    timer kill_timer;
    bool stop;
    int exit_code;
    uint64_t start_ms;
    uint64_t reloads_ok;
    uint64_t reloads_failed;
    uint64_t restarts_total;
    uint64_t stats_rejected;
    uint64_t stats_timeouts;
};

/* ------------------------------------------------------------------------- */
/* Señales                                                                   */
/* ------------------------------------------------------------------------- */

static volatile sig_atomic_t g_master_sig_fd = -1;

static void on_master_signal(int sig) {
    int saved = errno;
    unsigned char b = (unsigned char)sig;
    if (g_master_sig_fd >= 0) {
        ssize_t r = write(g_master_sig_fd, &b, 1);
        (void)r;
    }
    errno = saved;
}

static const int MASTER_SIGNALS[] = {SIGTERM, SIGINT, SIGHUP, SIGCHLD};
#define NMASTER_SIGNALS (sizeof(MASTER_SIGNALS) / sizeof(MASTER_SIGNALS[0]))

static int install_master_signals(struct master *m) {
    int p[2];
    if (pipe(p) < 0) {
        return -1;
    }
    m->sig_rd = p[0];
    m->sig_wr = p[1];
    if (fd_set_nonblock_cloexec(p[0]) < 0 || fd_set_nonblock_cloexec(p[1]) < 0) {
        return -1;
    }
    g_master_sig_fd = p[1];
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_master_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    for (size_t i = 0; i < NMASTER_SIGNALS; i++) {
        if (sigaction(MASTER_SIGNALS[i], &sa, NULL) < 0) {
            return -1;
        }
    }
    struct sigaction ign;
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    (void)sigaction(SIGPIPE, &ign, NULL);
    (void)sigaction(SIGUSR1, &ign, NULL);
    return 0;
}

static void default_signals(void) {
    struct sigaction dfl;
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    for (size_t i = 0; i < NMASTER_SIGNALS; i++) {
        (void)sigaction(MASTER_SIGNALS[i], &dfl, NULL);
    }
    (void)sigaction(SIGUSR1, &dfl, NULL);
}

static void close_fd(int *fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static const char *slot_state_name(enum slot_state s) {
    switch (s) {
    case SLOT_EMPTY:
        return "empty";
    case SLOT_STARTING:
        return "starting";
    case SLOT_READY:
        return "ready";
    case SLOT_RESTARTING:
        return "restarting";
    case SLOT_RETIRING:
        return "retiring";
    case SLOT_FAILED:
        return "failed";
    }
    return "?";
}

static const char *reload_state_name(enum reload_state r) {
    switch (r) {
    case R_IDLE:
        return "idle";
    case R_PREPARING:
        return "preparing";
    case R_COMMITTING:
        return "committing";
    }
    return "?";
}

static const char *recovery_name(enum recovery_state r) {
    switch (r) {
    case REC_NONE:
        return "none";
    case REC_PENDING:
        return "pending";
    case REC_COMPLETE:
        return "complete";
    case REC_EXHAUSTED:
        return "exhausted";
    }
    return "?";
}

static size_t ready_workers(const struct master *m) {
    size_t n = 0;
    for (size_t i = 0; i < m->nslots; i++) {
        n += m->slots[i].state == SLOT_READY;
    }
    return n;
}

static size_t live_workers(const struct master *m) {
    size_t n = 0;
    for (size_t i = 0; i < m->nslots; i++) {
        n += m->slots[i].pid > 0;
    }
    return n;
}

static void describe_status(int status, char *buf, size_t len) {
    if (WIFEXITED(status)) {
        (void)snprintf(buf, len, "código %d", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        (void)snprintf(buf, len, "señal %d", WTERMSIG(status));
    } else {
        (void)snprintf(buf, len, "estado %d", status);
    }
}

/* ------------------------------------------------------------------------- */
/* Canal con un worker                                                       */
/* ------------------------------------------------------------------------- */

static void slot_close_channel(struct slot *s) {
    if (s->ipc.fd >= 0) {
        (void)io_loop_del(s->m->loop, s->ipc.fd);
        close_fd(&s->ipc.fd);
    }
    ipc_free(&s->ipc);
}

static void slot_send(struct slot *s, uint32_t type, uint64_t gen, uint64_t id, const void *data,
                      size_t len) {
    if (s->ipc.fd < 0) {
        return;
    }
    if (ipc_queue(&s->ipc, type, gen, id, data, len) < 0) {
        plog(LOG_ERROR, gen, "ipc: no se pudo encolar el mensaje %u al worker %d", type, s->index);
        return;
    }
    if (ipc_flush(&s->ipc) < 0) {
        plog(LOG_WARN, gen, "ipc: canal con el worker %d roto", s->index);
        slot_close_channel(s); /* la muerte del proceso llegará por SIGCHLD */
    }
}

/* ------------------------------------------------------------------------- */
/* Arranque de workers                                                       */
/* ------------------------------------------------------------------------- */

static void stats_client_free(struct stats_client *c);

/*
 * En el hijo tras fork: cierra y libera todo lo del maestro sin tocar sus
 * registros de epoll/kqueue (io_loop_destroy solo cierra la copia del fd).
 */
static void release_in_child(struct master *m) {
    for (size_t i = 0; i < m->nslots; i++) {
        close_fd(&m->slots[i].ipc.fd);
        ipc_free(&m->slots[i].ipc);
    }
    while (m->clients != NULL) {
        struct stats_client *c = m->clients;
        m->clients = c->next;
        close_fd(&c->fd);
        free(c->asked);
        for (size_t i = 0; c->texts != NULL && i < m->nslots; i++) {
            free(c->texts[i]);
        }
        free((void *)c->texts);
        free(c->lens);
        free(c->out);
        free(c);
    }
    close_fd(&m->stats_fd); /* el socket es del maestro: el hijo no lo borra nunca */
    close_fd(&m->sig_rd);
    close_fd(&m->sig_wr);
    g_master_sig_fd = -1;
    io_loop_destroy(m->loop);
    m->loop = NULL;
    timer_heap_destroy(&m->timers);
    free(m->slots);
    m->slots = NULL;
    free(m->text);
    free(m->rtext);
    config_unref(m->rcfg);
    config_unref(m->cfg);
}

static void child_main(struct master *m, int index, int fd) {
    default_signals();
    struct config *cfg = config_ref(m->cfg);
    uint64_t gen = m->gen;
    /* Modelo compartido: el worker se queda con las copias heredadas de los
     * listeners (release_in_child no las cierra: ya no son del maestro). */
    int *shared = m->listen_fds;
    m->listen_fds = NULL;
    release_in_child(m);
    /* Grupo propio: un Ctrl+C del terminal llega solo al maestro. */
    (void)setpgid(0, 0);
    int rc = worker_run(cfg, gen, fd, index, shared);
    free(shared);
    config_unref(cfg);
    _exit(rc);
}

/* Modelo compartido: el maestro abre un socket por frontend antes de crear
 * workers; cada fork hereda una copia. 0 o -1 (ya registrado). */
static int open_shared_listeners(struct master *m) {
    m->nlisten = m->cfg->nfrontends;
    m->listen_fds = malloc(m->nlisten * sizeof(int));
    if (m->listen_fds == NULL) {
        return -1;
    }
    for (size_t i = 0; i < m->nlisten; i++) {
        m->listen_fds[i] = -1;
    }
    for (size_t i = 0; i < m->nlisten; i++) {
        char err[256];
        m->listen_fds[i] = listener_open(&m->cfg->frontends[i], false, err, sizeof(err));
        if (m->listen_fds[i] < 0) {
            plog(LOG_ERROR, 0, "arranque fallido: no se puede escuchar en %s", err);
            return -1;
        }
    }
    return 0;
}

/* El maestro suelta sus copias: sin ellas, cuando los workers cierran las
 * suyas el puerto deja de aceptar (no se acumulan conexiones en la cola). */
static void close_shared_listeners(struct master *m) {
    for (size_t i = 0; m->listen_fds != NULL && i < m->nlisten; i++) {
        close_fd(&m->listen_fds[i]);
    }
    free(m->listen_fds);
    m->listen_fds = NULL;
}

static void on_slot_io(io_loop *loop, int fd, uint32_t events, void *ud);
static void on_ready_timeout(timer_heap *heap, timer *t, void *ud);

static int spawn(struct master *m, struct slot *s) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        return -1;
    }
    /* Señales bloqueadas durante el fork: el hijo no ejecuta el manejador del
     * maestro antes de restablecer las suyas. */
    sigset_t all;
    sigset_t old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    pid_t pid = fork();
    if (pid == 0) {
        close(sv[0]);
        child_main(m, s->index, sv[1]); /* no retorna */
    }
    int saved = errno;
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    close(sv[1]);
    if (pid < 0) {
        close(sv[0]);
        errno = saved;
        return -1;
    }
    if (fd_set_nonblock_cloexec(sv[0]) < 0) {
        close(sv[0]);
        (void)kill(pid, SIGKILL);
        return -1;
    }
    ipc_init(&s->ipc, sv[0]);
    if (io_loop_add(m->loop, sv[0], IO_READ | IO_WRITE, on_slot_io, s) < 0) {
        close_fd(&s->ipc.fd);
        (void)kill(pid, SIGKILL);
        return -1;
    }
    s->pid = pid;
    s->state = SLOT_STARTING;
    s->gen = m->gen;
    s->participant = false;
    s->kill_sent = false;
    (void)timer_schedule_in(&m->timers, &s->ready_timer, timer_now_ms(), m->cfg->ipc_timeout_ms);
    plog(LOG_INFO, m->gen, "worker %d arrancado (pid %ld, generación %llu)", s->index, (long)pid,
         (unsigned long long)m->gen);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Cierre                                                                    */
/* ------------------------------------------------------------------------- */

static void reload_cancel(struct master *m, const char *msg);

static void begin_shutdown(struct master *m) {
    if (m->draining) {
        plog(LOG_WARN, 0, "segundo aviso: se reenvía a los workers (cierre forzado)");
        for (size_t i = 0; i < m->nslots; i++) {
            if (m->slots[i].pid > 0) {
                (void)kill(m->slots[i].pid, SIGTERM);
            }
        }
        return;
    }
    m->draining = true;
    close_shared_listeners(m); /* no habrá más workers que los hereden */
    plog(LOG_INFO, m->gen, "cierre ordenado del maestro (%zu workers, plazo %u ms)",
         live_workers(m), m->cfg->shutdown_timeout_ms);
    if (m->rstate != R_IDLE) {
        reload_cancel(m, "recarga descartada: cierre en curso");
    }
    m->reload_pending = false;
    for (size_t i = 0; i < m->nslots; i++) {
        struct slot *s = &m->slots[i];
        timer_cancel(&m->timers, &s->restart_timer);
        if (s->pid > 0 && s->state == SLOT_RETIRING) {
            /* Ya recibió SIGTERM al retirarlo: no se duplica. Conserva su
             * plazo de retirada; el de cierre también lo alcanza. */
            plog(LOG_INFO, m->gen, "worker %d (pid %ld) ya en retirada: termina con su plazo",
                 s->index, (long)s->pid);
        } else if (s->pid > 0) {
            (void)kill(s->pid, SIGTERM);
        } else if (s->state == SLOT_RESTARTING) {
            s->state = SLOT_EMPTY;
        }
    }
    if (live_workers(m) == 0) {
        m->stop = true;
        return;
    }
    (void)timer_schedule_in(&m->timers, &m->kill_timer, timer_now_ms(),
                            (uint64_t)m->cfg->shutdown_timeout_ms + KILL_GRACE_MS);
}

static void on_kill_timeout(timer_heap *heap, timer *t, void *ud) {
    (void)heap;
    (void)t;
    struct master *m = ud;
    size_t n = 0;
    for (size_t i = 0; i < m->nslots; i++) {
        struct slot *s = &m->slots[i];
        if (s->pid > 0 && !s->kill_sent) { /* un worker en retirada puede tenerlo ya */
            s->kill_sent = true;
            (void)kill(s->pid, SIGKILL);
            n++;
        }
    }
    if (n > 0) {
        plog(LOG_ERROR, 0, "workers sin terminar tras el plazo de cierre: SIGKILL a %zu", n);
        if (m->exit_code == 0) {
            m->exit_code = 70;
        }
    }
}

static void on_start_timeout(timer_heap *heap, timer *t, void *ud) {
    (void)heap;
    (void)t;
    struct master *m = ud;
    if (!m->starting) {
        return;
    }
    plog(LOG_ERROR, 0, "arranque fallido: no todos los workers estuvieron listos en %u ms",
         m->cfg->ipc_timeout_ms);
    m->exit_code = 1;
    begin_shutdown(m);
}

/* ------------------------------------------------------------------------- */
/* Recarga en dos fases                                                      */
/* ------------------------------------------------------------------------- */

static void begin_reload(struct master *m);

static void reload_release_candidate(struct master *m) {
    config_unref(m->rcfg);
    free(m->rtext);
    m->rcfg = NULL;
    m->rtext = NULL;
    m->rlen = 0;
}

/* Descarta la recarga en curso (antes de la decisión de activar). */
static void reload_cancel(struct master *m, const char *msg) {
    for (size_t i = 0; i < m->nslots; i++) {
        struct slot *s = &m->slots[i];
        if (s->participant) {
            slot_send(s, IPC_ABORT, m->rgen, 0, NULL, 0);
            s->participant = false;
        }
    }
    timer_cancel(&m->timers, &m->rtimer);
    if (m->rstate == R_PREPARING) {
        reload_release_candidate(m);
    }
    m->rstate = R_IDLE;
    plog(LOG_WARN, m->gen, "%s", msg);
}

static void reload_fail(struct master *m, const char *why) {
    char msg[CONFIG_ERR_LEN + 128];
    (void)snprintf(msg, sizeof(msg), "recarga rechazada: %s; se mantiene la generación %llu", why,
                   (unsigned long long)m->gen);
    m->reloads_failed++;
    reload_cancel(m, msg);
    if (m->reload_pending) {
        (void)timer_schedule_in(&m->timers, &m->rkick, timer_now_ms(), 0);
    }
}

static void finish_commit(struct master *m) {
    size_t k = 0;
    for (size_t i = 0; i < m->nslots; i++) {
        k += m->slots[i].committed;
        m->slots[i].participant = false;
    }
    timer_cancel(&m->timers, &m->rtimer);
    m->rstate = R_IDLE;
    m->reloads_ok++;
    bool pending = false;
    for (size_t i = 0; i < m->nslots; i++) {
        pending = pending || m->slots[i].recovering;
    }
    if (m->replaced > 0) {
        plog(LOG_WARN, m->gen,
             "recarga aplicada: generación %llu con capacidad reducida (%zu de %zu workers "
             "confirmados; %zu por reponer con la generación %llu)",
             (unsigned long long)m->gen, k, k + m->replaced, m->replaced,
             (unsigned long long)m->gen);
    } else {
        plog(LOG_INFO, m->gen, "recarga aplicada: generación %llu (%zu workers)",
             (unsigned long long)m->gen, k);
    }
    m->recovery = pending ? REC_PENDING : REC_NONE;
    m->recovery_gen = m->gen;
    if (m->reload_pending) {
        (void)timer_schedule_in(&m->timers, &m->rkick, timer_now_ms(), 0);
    }
}

static void check_committed(struct master *m) {
    if (m->rstate != R_COMMITTING) {
        return;
    }
    for (size_t i = 0; i < m->nslots; i++) {
        if (m->slots[i].participant && !m->slots[i].committed) {
            return;
        }
    }
    finish_commit(m);
}

/*
 * Punto de decisión: todos prepararon. A partir de aquí la generación nueva
 * es la del maestro; un worker que no confirme se repone con ella.
 */
static void commit_decision(struct master *m) {
    /* Plazo de retirada de quien no confirme: el vaciado del worker usa el
     * shutdown_timeout_ms de la generación que aún tiene (la vieja), el
     * maestro ya el de la nueva; se toma el mayor más el margen. */
    uint32_t old_shutdown = m->cfg->shutdown_timeout_ms;
    uint32_t new_shutdown = m->rcfg->shutdown_timeout_ms;
    m->retire_ms = (old_shutdown > new_shutdown ? old_shutdown : new_shutdown) + KILL_GRACE_MS;
    config_unref(m->cfg);
    free(m->text);
    m->cfg = m->rcfg;
    m->text = m->rtext;
    m->text_len = m->rlen;
    m->rcfg = NULL;
    m->rtext = NULL;
    m->gen = m->rgen;
    m->rstate = R_COMMITTING;
    m->replaced = 0;
    size_t n = 0;
    for (size_t i = 0; i < m->nslots; i++) {
        struct slot *s = &m->slots[i];
        s->committed = false;
        if (s->participant) {
            n++;
            slot_send(s, IPC_COMMIT, m->gen, 0, NULL, 0);
        }
    }
    plog(LOG_INFO, m->gen, "activación iniciada: generación %llu (%zu workers, plazo %u ms)",
         (unsigned long long)m->gen, n, m->cfg->ipc_timeout_ms);
    (void)timer_schedule_in(&m->timers, &m->rtimer, timer_now_ms(), m->cfg->ipc_timeout_ms);
    check_committed(m);
}

static void check_prepared(struct master *m) {
    for (size_t i = 0; i < m->nslots; i++) {
        if (m->slots[i].participant && !m->slots[i].prepared) {
            return;
        }
    }
    commit_decision(m);
}

/*
 * El worker no confirmó la activación (plazo vencido o COMMIT_FAILED): se
 * retira. Un solo SIGTERM (termina sus peticiones con la generación que
 * tiene) y un plazo de retirada; si sigue vivo al vencer, SIGKILL
 * (on_retire_timeout). Solo al recogerlo (on_child_exit) se programa su
 * reposición, que arranca con la generación comprometida.
 */
static void retire_worker(struct master *m, struct slot *s, const char *why) {
    s->participant = false;
    if (s->state == SLOT_RETIRING || s->pid <= 0) {
        return; /* ya en retirada, o ya muerto (lo trata on_child_exit) */
    }
    m->replaced++;
    s->recovering = true;
    s->state = SLOT_RETIRING;
    plog(LOG_WARN, m->gen,
         "worker %d (pid %ld) %s: retirada con SIGTERM (plazo %u ms); se repondrá con la "
         "generación %llu",
         s->index, (long)s->pid, why, m->retire_ms, (unsigned long long)m->gen);
    (void)kill(s->pid, SIGTERM);
    (void)timer_schedule_in(&m->timers, &s->retire_timer, timer_now_ms(), m->retire_ms);
}

static void on_retire_timeout(timer_heap *heap, timer *t, void *ud) {
    (void)heap;
    (void)t;
    struct slot *s = ud;
    struct master *m = s->m;
    /* El pid sigue siendo nuestro hijo: solo el maestro hace waitpid y cancela
     * este plazo al recogerlo, así que no puede estar reutilizado. */
    if (s->state != SLOT_RETIRING || s->pid <= 0 || s->kill_sent) {
        return;
    }
    s->kill_sent = true;
    plog(LOG_ERROR, m->gen, "worker %d (pid %ld) sigue vivo tras %u ms de retirada: SIGKILL",
         s->index, (long)s->pid, m->retire_ms);
    (void)kill(s->pid, SIGKILL);
}

static void on_reload_timeout(timer_heap *heap, timer *t, void *ud) {
    (void)heap;
    (void)t;
    struct master *m = ud;
    if (m->rstate == R_PREPARING) {
        char who[256] = "";
        size_t used = 0;
        for (size_t i = 0; i < m->nslots; i++) {
            if (m->slots[i].participant && !m->slots[i].prepared && used < sizeof(who)) {
                int w = snprintf(who + used, sizeof(who) - used, "%s%d", used ? "," : "",
                                 m->slots[i].index);
                used += w > 0 ? (size_t)w : 0;
            }
        }
        char why[384];
        (void)snprintf(why, sizeof(why), "sin respuesta de los workers [%s] en %u ms", who,
                       m->cfg->ipc_timeout_ms);
        reload_fail(m, why);
    } else if (m->rstate == R_COMMITTING) {
        for (size_t i = 0; i < m->nslots; i++) {
            struct slot *s = &m->slots[i];
            if (s->participant && !s->committed) {
                retire_worker(m, s, "no confirmó la activación a tiempo");
            }
        }
        finish_commit(m);
    }
}

static bool any_starting(const struct master *m) {
    for (size_t i = 0; i < m->nslots; i++) {
        if (m->slots[i].state == SLOT_STARTING) {
            return true;
        }
    }
    return false;
}

static void begin_reload(struct master *m) {
    if (m->draining) {
        m->reload_pending = false;
        return;
    }
    if (m->rstate != R_IDLE || m->starting || any_starting(m)) {
        /* Se registra cada aviso; todos los pendientes se agrupan en uno. */
        plog(LOG_INFO, m->gen, "recarga pendiente: se hará al terminar la actual o el arranque");
        m->reload_pending = true;
        return;
    }
    m->reload_pending = false;
    plog(LOG_INFO, m->gen, "recarga iniciada (generación actual %llu)", (unsigned long long)m->gen);
    char err[CONFIG_ERR_LEN];
    size_t len = 0;
    /* Lectura única: todos los workers reciben exactamente estos bytes. */
    char *text = config_read_text(m->path, &len, err, sizeof(err));
    struct config *cfg = text != NULL ? config_load_string(text, m->path, err, sizeof(err)) : NULL;
    char why[CONFIG_ERR_LEN];
    if (cfg == NULL) {
        free(text);
        m->reloads_failed++;
        plog(LOG_WARN, m->gen, "recarga rechazada: %s; se mantiene la generación %llu", err,
             (unsigned long long)m->gen);
        return;
    }
    if (!config_reload_compatible(m->cfg, cfg, why, sizeof(why))) {
        config_unref(cfg);
        free(text);
        m->reloads_failed++;
        plog(LOG_WARN, m->gen, "recarga rechazada: %s; se mantiene la generación %llu", why,
             (unsigned long long)m->gen);
        return;
    }
    m->rgen = m->gen + 1;
    m->rcfg = cfg;
    m->rtext = text;
    m->rlen = len;
    m->rstate = R_PREPARING;
    for (size_t i = 0; i < m->nslots; i++) {
        struct slot *s = &m->slots[i];
        s->participant = s->state == SLOT_READY && s->ipc.fd >= 0;
        s->prepared = false;
        s->committed = false;
        if (s->participant) {
            slot_send(s, IPC_PREPARE, m->rgen, 0, text, len);
        }
    }
    (void)timer_schedule_in(&m->timers, &m->rtimer, timer_now_ms(), m->cfg->ipc_timeout_ms);
    check_prepared(m); /* sin participantes vivos: activación directa */
}

static void on_reload_kick(timer_heap *heap, timer *t, void *ud) {
    (void)heap;
    (void)t;
    struct master *m = ud;
    if (m->reload_pending) {
        begin_reload(m);
    }
}

/* ------------------------------------------------------------------------- */
/* Supervisión                                                               */
/* ------------------------------------------------------------------------- */

static void on_restart_timer(timer_heap *heap, timer *t, void *ud) {
    (void)heap;
    (void)t;
    struct slot *s = ud;
    struct master *m = s->m;
    if (m->draining || s->state != SLOT_RESTARTING) {
        return;
    }
    if (m->rstate != R_IDLE) {
        /* Durante una recarga no se arranca nadie: se aplaza y el sustituto
         * nacerá con la generación resultante. */
        (void)timer_schedule_in(&m->timers, &s->restart_timer, timer_now_ms(), POSTPONE_MS);
        return;
    }
    s->restarts++;
    m->restarts_total++;
    if (spawn(m, s) < 0) {
        char eb[64];
        plog(LOG_ERROR, m->gen, "no se pudo reponer el worker %d: %s", s->index,
             diag_strerror(errno, eb, sizeof(eb)));
        s->state = SLOT_RESTARTING;
        (void)timer_schedule_in(&m->timers, &s->restart_timer, timer_now_ms(), s->backoff_ms);
    }
}

/* Un sustituto quedó listo: ¿terminó la recuperación de la última recarga? */
static void check_recovery(struct master *m) {
    for (size_t i = 0; i < m->nslots; i++) {
        if (m->slots[i].recovering) {
            return;
        }
    }
    if (m->recovery == REC_PENDING) {
        m->recovery = REC_COMPLETE;
        plog(LOG_INFO, m->gen, "recuperación completa: generación %llu (%zu de %zu workers listos)",
             (unsigned long long)m->gen, ready_workers(m), m->nslots);
    } else if (m->recovery == REC_EXHAUSTED) {
        plog(LOG_WARN, m->gen,
             "recuperación terminada con capacidad reducida: generación %llu (%zu de %zu workers "
             "listos)",
             (unsigned long long)m->gen, ready_workers(m), m->nslots);
    }
}

static void schedule_restart(struct master *m, struct slot *s) {
    uint64_t now = timer_now_ms();
    if (now - s->window_start > m->cfg->restart_window_ms) {
        s->window_start = now;
        s->window_count = 0;
    }
    s->window_count++;
    if (s->window_count > m->cfg->max_restarts) {
        s->state = SLOT_FAILED;
        plog(LOG_ERROR, m->gen, "worker %d: %u reinicios en %u ms (máximo %u): se deja sin reponer",
             s->index, s->window_count - 1, m->cfg->restart_window_ms, m->cfg->max_restarts);
        if (s->recovering) {
            s->recovering = false;
            m->recovery = REC_EXHAUSTED;
            plog(LOG_ERROR, m->gen,
                 "recuperación agotada: el worker %d no se repone (max_restarts); la generación "
                 "%llu sigue activa con %zu de %zu workers",
                 s->index, (unsigned long long)m->gen, ready_workers(m), m->nslots);
        }
        bool any = false;
        for (size_t i = 0; i < m->nslots; i++) {
            any = any || m->slots[i].state != SLOT_FAILED;
        }
        if (!any) {
            plog(LOG_ERROR, m->gen, "todos los workers han fallado: el maestro termina");
            m->exit_code = 1;
            begin_shutdown(m);
        }
        return;
    }
    s->state = SLOT_RESTARTING;
    plog(LOG_INFO, m->gen, "worker %d se repondrá en %u ms", s->index, s->backoff_ms);
    (void)timer_schedule_in(&m->timers, &s->restart_timer, now, s->backoff_ms);
    s->backoff_ms = s->backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : s->backoff_ms * 2;
}

static void on_child_exit(struct master *m, struct slot *s, int status) {
    char st[48];
    describe_status(status, st, sizeof(st));
    slot_close_channel(s);
    timer_cancel(&m->timers, &s->ready_timer);
    timer_cancel(&m->timers, &s->retire_timer); /* antes de soltar el pid */
    bool retiring = s->state == SLOT_RETIRING;
    pid_t pid = s->pid;
    s->pid = 0;
    bool clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (m->draining) {
        plog(clean ? LOG_INFO : LOG_WARN, m->gen, "worker %d (pid %ld) terminó (%s)", s->index,
             (long)pid, st);
        s->state = SLOT_EMPTY;
        if (!clean && m->exit_code == 0) {
            m->exit_code = 70;
        }
        if (live_workers(m) == 0) {
            m->stop = true;
        }
        return;
    }
    if (m->starting) {
        plog(LOG_ERROR, m->gen, "arranque fallido: el worker %d (pid %ld) terminó (%s)", s->index,
             (long)pid, st);
        s->state = SLOT_EMPTY;
        m->exit_code = 1;
        begin_shutdown(m);
        return;
    }
    if (retiring) {
        /* Recogido tras la retirada: solo ahora se programa su reposición. */
        plog(LOG_WARN, m->gen, "worker %d (pid %ld) retirado (%s)", s->index, (long)pid, st);
        schedule_restart(m, s);
        return;
    }
    plog(LOG_WARN, m->gen, "worker %d (pid %ld) terminó inesperadamente (%s)", s->index, (long)pid,
         st);
    if (s->participant) {
        s->participant = false;
        if (m->rstate == R_PREPARING) {
            char why[96];
            (void)snprintf(why, sizeof(why), "el worker %d terminó durante la preparación",
                           s->index);
            reload_fail(m, why); /* el sustituto arrancará con la generación vigente */
        } else if (m->rstate == R_COMMITTING) {
            /* La decisión ya está tomada: no hay vuelta atrás. El sustituto
             * arrancará con la nueva generación (la reposición se aplaza
             * hasta que termine la recarga). */
            m->replaced++;
            s->recovering = true;
            check_committed(m);
        }
    }
    schedule_restart(m, s);
}

static void reap_children(struct master *m) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (size_t i = 0; i < m->nslots; i++) {
            if (m->slots[i].pid == pid) {
                on_child_exit(m, &m->slots[i], status);
                break;
            }
        }
    }
}

static void on_ready_timeout(timer_heap *heap, timer *t, void *ud) {
    (void)heap;
    (void)t;
    struct slot *s = ud;
    if (s->state == SLOT_STARTING && s->pid > 0) {
        plog(LOG_ERROR, s->m->gen, "worker %d (pid %ld) no estuvo listo a tiempo: SIGKILL",
             s->index, (long)s->pid);
        (void)kill(s->pid, SIGKILL); /* su salida la trata on_child_exit */
    }
}

/* ------------------------------------------------------------------------- */
/* Estadísticas                                                              */
/* ------------------------------------------------------------------------- */

/* Libera los recursos del cliente; no lo toca en la lista. */
static void stats_client_destroy(struct master *m, struct stats_client *c) {
    timer_cancel(&m->timers, &c->collect_timer);
    timer_cancel(&m->timers, &c->deadline);
    if (c->fd >= 0) {
        (void)io_loop_del(m->loop, c->fd);
        close_fd(&c->fd);
    }
    for (size_t i = 0; c->texts != NULL && i < m->nslots; i++) {
        free(c->texts[i]);
    }
    free((void *)c->texts);
    free(c->lens);
    free(c->asked);
    free(c->out);
    m->nclients--;
    free(c);
}

static void stats_client_free(struct stats_client *c) {
    struct master *m = c->m;
    if (c->prev != NULL) {
        c->prev->next = c->next;
    } else {
        m->clients = c->next;
    }
    if (c->next != NULL) {
        c->next->prev = c->prev;
    }
    stats_client_destroy(m, c);
}

static void stats_client_write(struct stats_client *c) {
    while (c->off < c->len) {
        ssize_t n = send(c->fd, c->out + c->off, c->len - c->off, 0);
        if (n > 0) {
            c->off += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return; /* cliente lento: se espera IO_WRITE dentro de su plazo */
        } else {
            stats_client_free(c);
            return;
        }
    }
    stats_client_free(c); /* respuesta completa: se cierra */
}

static void stats_render(struct stats_client *c) {
    struct master *m = c->m;
    c->collecting = false;
    timer_cancel(&m->timers, &c->collect_timer);
    struct stats_worker_input *in = calloc(m->nslots, sizeof(*in));
    if (in == NULL) {
        stats_client_free(c);
        return;
    }
    for (size_t i = 0; i < m->nslots; i++) {
        in[i] = (struct stats_worker_input){.index = m->slots[i].index,
                                            .pid = (long)m->slots[i].pid,
                                            .state = slot_state_name(m->slots[i].state),
                                            .restarts = m->slots[i].restarts,
                                            .text = c->texts[i],
                                            .len = c->lens[i]};
    }
    struct stats_master_info mi = {.pid = (long)getpid(),
                                   .uptime_ms = timer_now_ms() - m->start_ms,
                                   .generation = m->gen,
                                   .workers_configured = (uint32_t)m->nslots,
                                   .reload_in_progress = m->rstate != R_IDLE,
                                   .reload_state = reload_state_name(m->rstate),
                                   .listener_model = listener_model_name(m->lmodel),
                                   .recovery = recovery_name(m->recovery),
                                   .recovery_generation = m->recovery_gen,
                                   .workers_ready = (uint32_t)ready_workers(m),
                                   .draining = m->draining,
                                   .reloads_ok = m->reloads_ok,
                                   .reloads_failed = m->reloads_failed,
                                   .worker_restarts = m->restarts_total,
                                   .stats_rejected = m->stats_rejected,
                                   .stats_timeouts = m->stats_timeouts};
    c->out = stats_render_json(&mi, in, m->nslots, &c->len);
    free(in);
    if (c->out == NULL) {
        stats_client_free(c);
        return;
    }
    stats_client_write(c);
}

static void on_collect_timeout(timer_heap *heap, timer *t, void *ud) {
    (void)heap;
    (void)t;
    stats_render(ud); /* con los que hayan respondido; el resto figura como ausente */
}

static void on_client_deadline(timer_heap *heap, timer *t, void *ud) {
    (void)heap;
    (void)t;
    struct stats_client *c = ud;
    c->m->stats_timeouts++;
    stats_client_free(c);
}

static void on_client_io(io_loop *loop, int fd, uint32_t events, void *ud) {
    (void)loop;
    (void)fd;
    struct stats_client *c = ud;
    if (!c->collecting && (events & (IO_WRITE | IO_ERROR | IO_HUP))) {
        stats_client_write(c);
    }
}

static void stats_on_reply(struct master *m, size_t slot, uint64_t id, const char *data,
                           size_t len) {
    for (struct stats_client *c = m->clients; c != NULL; c = c->next) {
        if (!c->collecting || c->req_id != id) {
            continue;
        }
        if (!c->asked[slot] || c->texts[slot] != NULL) {
            return; /* duplicada o no pedida: no se suma dos veces */
        }
        c->texts[slot] = malloc(len + 1);
        if (c->texts[slot] == NULL) {
            return;
        }
        memcpy(c->texts[slot], data, len);
        c->texts[slot][len] = '\0';
        c->lens[slot] = len;
        if (++c->received == c->expected) {
            stats_render(c);
        }
        return;
    }
    /* Respuesta tardía de una petición ya contestada: se ignora. */
}

static void on_stats_accept(io_loop *loop, int fd, uint32_t events, void *ud) {
    (void)loop;
    (void)events;
    struct master *m = ud;
    for (;;) {
        int cfd = accept(fd, NULL, NULL);
        if (cfd < 0) {
            return;
        }
        /* También durante el cierre: permite observarlo hasta el final. */
        if (m->nslots == 0 || m->nclients >= m->cfg->stats.max_clients ||
            fd_set_nonblock_cloexec(cfd) < 0) {
            m->stats_rejected++;
            close(cfd);
            continue;
        }
        struct stats_client *c = calloc(1, sizeof(*c));
        if (c != NULL) {
            c->asked = calloc(m->nslots, sizeof(bool));
            c->texts = (char **)calloc(m->nslots, sizeof(char *));
            c->lens = calloc(m->nslots, sizeof(size_t));
        }
        if (c == NULL || c->asked == NULL || c->texts == NULL || c->lens == NULL) {
            if (c != NULL) {
                free(c->asked);
                free((void *)c->texts);
                free(c->lens);
                free(c);
            }
            m->stats_rejected++;
            close(cfd);
            continue;
        }
        c->m = m;
        c->fd = cfd;
        c->req_id = ++m->next_req_id;
        c->collecting = true;
        timer_init(&c->collect_timer, on_collect_timeout, c);
        timer_init(&c->deadline, on_client_deadline, c);
        c->next = m->clients;
        if (m->clients != NULL) {
            m->clients->prev = c;
        }
        m->clients = c;
        m->nclients++;
        if (io_loop_add(m->loop, cfd, IO_WRITE, on_client_io, c) < 0) {
            stats_client_free(c);
            continue;
        }
        uint64_t now = timer_now_ms();
        (void)timer_schedule_in(&m->timers, &c->deadline, now, m->cfg->stats.timeout_ms);
        (void)timer_schedule_in(&m->timers, &c->collect_timer, now, m->cfg->stats.timeout_ms / 2);
        for (size_t i = 0; i < m->nslots; i++) {
            struct slot *s = &m->slots[i];
            if (s->state == SLOT_READY && s->ipc.fd >= 0) {
                c->asked[i] = true;
                c->expected++;
                slot_send(s, IPC_STATS_REQ, m->gen, c->req_id, NULL, 0);
            }
        }
        if (c->expected == 0) {
            stats_render(c);
        }
    }
}

/* Crea el socket sin borrar nunca una ruta existente. */
static int stats_open(struct master *m) {
    const char *path = m->cfg->stats.socket;
    if (path[0] == '\0') {
        return 0;
    }
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(sa.sun_path)) {
        plog(LOG_ERROR, 0, "[stats].socket demasiado largo: %s", path);
        return -1;
    }
    memcpy(sa.sun_path, path, strlen(path) + 1);
    struct stat st;
    if (lstat(path, &st) == 0) {
        plog(LOG_ERROR, 0,
             "la ruta del socket de estadísticas %s ya existe (%s): no se borra; si es un socket "
             "abandonado, elimínelo manualmente",
             path, S_ISSOCK(st.st_mode) ? "socket" : "no es un socket");
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || fd_set_nonblock_cloexec(fd) < 0) {
        close_fd(&fd);
        return -1;
    }
    mode_t old = umask(0177); /* nace 0600; el maestro no tiene hilos */
    int rc = bind(fd, (struct sockaddr *)&sa, sizeof(sa));
    int saved = errno;
    umask(old);
    char eb[64];
    if (rc < 0) {
        plog(LOG_ERROR, 0, "no se puede crear el socket de estadísticas %s: %s", path,
             diag_strerror(saved, eb, sizeof(eb)));
        close(fd);
        return -1;
    }
    if (lstat(path, &st) < 0 || chmod(path, (mode_t)m->cfg->stats.mode) < 0 || listen(fd, 64) < 0) {
        plog(LOG_ERROR, 0, "socket de estadísticas %s: %s", path,
             diag_strerror(errno, eb, sizeof(eb)));
        close(fd);
        (void)unlink(path); /* lo acabamos de crear nosotros */
        return -1;
    }
    m->stats_fd = fd;
    m->stats_dev = st.st_dev;
    m->stats_ino = st.st_ino;
    m->stats_own = true;
    if (io_loop_add(m->loop, fd, IO_READ, on_stats_accept, m) < 0) {
        return -1;
    }
    plog(LOG_INFO, 0, "estadísticas en %s (modo %o)", path, m->cfg->stats.mode);
    return 0;
}

/* Borra el socket solo si sigue siendo el que creamos (mismo dispositivo e
 * inodo); nunca una ruta ajena. */
static void stats_close(struct master *m) {
    while (m->clients != NULL) {
        struct stats_client *c = m->clients;
        m->clients = c->next;
        if (m->clients != NULL) {
            m->clients->prev = NULL;
        }
        stats_client_destroy(m, c);
    }
    if (m->stats_fd >= 0) {
        (void)io_loop_del(m->loop, m->stats_fd);
        close_fd(&m->stats_fd);
    }
    if (m->stats_own) {
        struct stat st;
        const char *path = m->cfg->stats.socket;
        if (lstat(path, &st) == 0 && S_ISSOCK(st.st_mode) && st.st_dev == m->stats_dev &&
            st.st_ino == m->stats_ino) {
            (void)unlink(path);
        } else {
            plog(LOG_WARN, 0, "%s ya no es nuestro socket: no se borra", path);
        }
        m->stats_own = false;
    }
}

/* ------------------------------------------------------------------------- */
/* Mensajes de los workers                                                   */
/* ------------------------------------------------------------------------- */

static void on_slot_msg(void *ctx, const struct ipc_msg *msg) {
    struct slot *s = ctx;
    struct master *m = s->m;
    size_t idx = (size_t)(s - m->slots);
    switch (msg->type) {
    case IPC_READY:
        timer_cancel(&m->timers, &s->ready_timer);
        s->state = SLOT_READY;
        s->backoff_ms = m->cfg->restart_backoff_ms;
        s->gen = msg->gen;
        if (m->starting) {
            bool all = true;
            for (size_t i = 0; i < m->nslots; i++) {
                all = all && m->slots[i].state == SLOT_READY;
            }
            if (all) {
                m->starting = false;
                timer_cancel(&m->timers, &m->start_timer);
                plog(LOG_INFO, m->gen, "proxy: listo (pid %ld, %zu workers, generación %llu)",
                     (long)getpid(), m->nslots, (unsigned long long)m->gen);
            }
        } else {
            plog(LOG_INFO, m->gen, "worker %d repuesto (pid %ld, generación %llu)", s->index,
                 (long)s->pid, (unsigned long long)msg->gen);
            if (s->recovering && msg->gen == m->gen) {
                s->recovering = false;
                check_recovery(m);
            }
        }
        if (m->reload_pending && !m->starting && !any_starting(m)) {
            begin_reload(m);
        }
        break;
    case IPC_PREPARED_OK:
        if (m->rstate == R_PREPARING && s->participant && msg->gen == m->rgen) {
            s->prepared = true;
            check_prepared(m);
        }
        break;
    case IPC_PREPARED_ERR:
        if (m->rstate == R_PREPARING && s->participant && msg->gen == m->rgen) {
            char why[CONFIG_ERR_LEN + 32];
            (void)snprintf(why, sizeof(why), "worker %d: %.*s", s->index, (int)msg->len, msg->data);
            reload_fail(m, why);
        }
        break;
    case IPC_COMMITTED:
        if (m->rstate == R_COMMITTING && s->participant && msg->gen == m->gen) {
            s->committed = true;
            s->gen = msg->gen;
            check_committed(m);
        }
        break;
    case IPC_COMMIT_FAILED:
        if (m->rstate == R_COMMITTING && s->participant && msg->gen == m->gen) {
            retire_worker(m, s, "no pudo activar la generación (COMMIT_FAILED)");
            check_committed(m);
        }
        break;
    case IPC_STATS:
        stats_on_reply(m, idx, msg->id, msg->data, msg->len);
        break;
    default:
        plog(LOG_WARN, m->gen, "ipc: mensaje desconocido %u del worker %d", msg->type, s->index);
        break;
    }
}

static void on_slot_io(io_loop *loop, int fd, uint32_t events, void *ud) {
    (void)loop;
    (void)fd;
    struct slot *s = ud;
    int rc = 0;
    if (events & (IO_READ | IO_HUP | IO_ERROR)) {
        rc = ipc_read(&s->ipc, on_slot_msg, s);
    }
    if (rc == 0 && s->ipc.fd >= 0 && (events & (IO_WRITE | IO_ERROR)) && ipc_flush(&s->ipc) < 0) {
        rc = -1;
    }
    if (rc < 0 && s->ipc.fd >= 0) {
        slot_close_channel(s); /* su terminación se procesa con SIGCHLD */
    }
}

static void on_master_signal_io(io_loop *loop, int fd, uint32_t events, void *ud) {
    (void)loop;
    (void)events;
    struct master *m = ud;
    unsigned char sigs[32];
    ssize_t n;
    while ((n = read(fd, sigs, sizeof(sigs))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            switch (sigs[i]) {
            case SIGTERM:
            case SIGINT:
                begin_shutdown(m);
                break;
            case SIGHUP:
                begin_reload(m);
                break;
            case SIGCHLD:
                reap_children(m);
                break;
            default:
                break;
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Ejecución                                                                 */
/* ------------------------------------------------------------------------- */

int master_run(struct config *cfg, char *text, size_t text_len, const char *path) {
    log_set_role(-1);
    struct master m;
    memset(&m, 0, sizeof(m));
    m.cfg = cfg; /* se queda con la referencia de quien llama */
    m.text = text;
    m.text_len = text_len;
    m.gen = 1;
    m.path = path;
    m.sig_rd = m.sig_wr = m.stats_fd = -1;
    m.start_ms = timer_now_ms();
    timer_heap_init(&m.timers);
    timer_init(&m.rtimer, on_reload_timeout, &m);
    timer_init(&m.rkick, on_reload_kick, &m);
    timer_init(&m.start_timer, on_start_timeout, &m);
    timer_init(&m.kill_timer, on_kill_timeout, &m);
    m.nslots = config_effective_workers(cfg);
    m.slots = calloc(m.nslots, sizeof(*m.slots));
    m.loop = io_loop_create(64);
    int rc = 0;
    char eb[64];
    if (m.slots == NULL || m.loop == NULL || install_master_signals(&m) < 0 ||
        io_loop_add(m.loop, m.sig_rd, IO_READ, on_master_signal_io, &m) < 0) {
        plog(LOG_ERROR, 0, "maestro: sin recursos: %s", diag_strerror(errno, eb, sizeof(eb)));
        rc = 1;
    }
    for (size_t i = 0; m.slots != NULL && i < m.nslots; i++) {
        struct slot *s = &m.slots[i];
        s->m = &m;
        s->index = (int)i;
        s->ipc.fd = -1;
        s->backoff_ms = cfg->restart_backoff_ms;
        s->window_start = m.start_ms;
        timer_init(&s->restart_timer, on_restart_timer, s);
        timer_init(&s->ready_timer, on_ready_timeout, s);
        timer_init(&s->retire_timer, on_retire_timeout, s);
    }
    m.lmodel = listener_default_model();
#ifdef PROXY_TEST_HOOKS
    /* Solo en proxy-testhooks: probar en Linux el modelo de macOS. */
    if (getenv("PROXY_TEST_SHARED_LISTENERS") != NULL) { // NOLINT(concurrency-mt-unsafe)
        m.lmodel = LISTENER_SHARED;
        plog(LOG_WARN, 0, "VARIANTE DE PRUEBAS: modelo de escucha compartido forzado");
    }
#endif
    if (rc == 0) {
        plog(LOG_INFO, 0, "modelo de escucha: %s", listener_model_name(m.lmodel));
    }
    if (rc == 0 && m.lmodel == LISTENER_SHARED && open_shared_listeners(&m) < 0) {
        rc = 1;
    }
    if (rc == 0 && stats_open(&m) < 0) {
        rc = 1;
    }
    if (rc == 0) {
        plog(LOG_INFO, m.gen, "maestro pid %ld: arrancando %zu workers", (long)getpid(), m.nslots);
        m.starting = true;
        (void)timer_schedule_in(&m.timers, &m.start_timer, m.start_ms, cfg->ipc_timeout_ms);
        for (size_t i = 0; i < m.nslots; i++) {
            if (spawn(&m, &m.slots[i]) < 0) {
                plog(LOG_ERROR, 0, "arranque fallido: fork del worker %zu: %s", i,
                     diag_strerror(errno, eb, sizeof(eb)));
                m.exit_code = 1;
                begin_shutdown(&m);
                break;
            }
        }
        while (!m.stop) {
            if (timer_loop_run_once(m.loop, &m.timers) < 0) {
                plog(LOG_ERROR, 0, "maestro: bucle de eventos: %s",
                     diag_strerror(errno, eb, sizeof(eb)));
                /* Último recurso: no dejar hijos huérfanos. */
                for (size_t i = 0; i < m.nslots; i++) {
                    if (m.slots[i].pid > 0) {
                        (void)kill(m.slots[i].pid, SIGKILL);
                        (void)waitpid(m.slots[i].pid, NULL, 0);
                        m.slots[i].pid = 0;
                    }
                }
                m.exit_code = 1;
                break;
            }
        }
        rc = m.exit_code;
        if (rc == 1) {
            plog(LOG_ERROR, 0,
                 "proxy: arranque fallido o sin workers; ver los mensajes anteriores");
        }
    }

    /* Limpieza: sin hijos vivos (el bucle termina al recogerlos todos). */
    close_shared_listeners(&m);
    stats_close(&m);
    if (m.rstate == R_PREPARING) {
        reload_release_candidate(&m);
    }
    for (size_t i = 0; m.slots != NULL && i < m.nslots; i++) {
        timer_cancel(&m.timers, &m.slots[i].restart_timer);
        timer_cancel(&m.timers, &m.slots[i].ready_timer);
        timer_cancel(&m.timers, &m.slots[i].retire_timer);
        slot_close_channel(&m.slots[i]);
    }
    timer_cancel(&m.timers, &m.rtimer);
    timer_cancel(&m.timers, &m.rkick);
    timer_cancel(&m.timers, &m.start_timer);
    timer_cancel(&m.timers, &m.kill_timer);
    default_signals();
    g_master_sig_fd = -1;
    if (m.sig_rd >= 0 && m.loop != NULL) {
        (void)io_loop_del(m.loop, m.sig_rd);
    }
    close_fd(&m.sig_rd);
    close_fd(&m.sig_wr);
    free(m.slots);
    io_loop_destroy(m.loop);
    timer_heap_destroy(&m.timers);
    config_unref(m.cfg);
    free(m.text);
    plog(LOG_INFO, 0, "maestro: fin (código %d)", rc);
    return rc;
}
