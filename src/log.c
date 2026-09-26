/*
 * log.c — ver log.h.
 */
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "diag.h"

#define BATCH     64
#define POLL_STEP 100 /* ms */

static const char TRUNC_MARK[] = " [truncado]\n";

struct logger {
    pthread_mutex_t mu;
    pthread_cond_t cv;      /* hay registros o hay que parar */
    pthread_cond_t done_cv; /* el consumidor terminó */
    char *slots;            /* capacity * LOG_RECORD */
    uint16_t *lens;
    size_t capacity;
    size_t head;
    size_t count;
    bool stop;
    bool exited;
    atomic_bool abandon;
    pthread_t thread;
    int fd;
    bool own_fd;
    enum log_level min;
    int worker;
    atomic_uint_least64_t written;
    atomic_uint_least64_t dropped;
    atomic_uint_least64_t write_errors;
    atomic_uint_least64_t truncated;
    uint64_t dropped_reported; /* solo el consumidor */
};

static logger *g_logger; /* se fija antes de crear hilos que lo usen */
static int g_role = -1;  /* modo síncrono */

/* ------------------------------------------------------------------------- */
/* Formato                                                                   */
/* ------------------------------------------------------------------------- */

/* Formatea un registro completo en out (LOG_RECORD); devuelve su longitud. */
__attribute__((format(printf, 5, 0))) static size_t format_record(char *out, int worker,
                                                                  enum log_level level,
                                                                  uint64_t gen, const char *fmt,
                                                                  va_list ap, bool *truncated) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    time_t secs = ts.tv_sec;
    gmtime_r(&secs, &tm);
    char role[24];
    if (worker < 0) {
        (void)snprintf(role, sizeof(role), "role=master");
    } else {
        (void)snprintf(role, sizeof(role), "worker=%d", worker);
    }
    char genbuf[24];
    if (gen == 0) {
        (void)snprintf(genbuf, sizeof(genbuf), "-");
    } else {
        (void)snprintf(genbuf, sizeof(genbuf), "%llu", (unsigned long long)gen);
    }
    int h = snprintf(out, LOG_RECORD,
                     "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ pid=%ld %s level=%s gen=%s ",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                     ts.tv_nsec / 1000000L, (long)getpid(), role, log_level_name(level), genbuf);
    size_t len = h > 0 && (size_t)h < LOG_RECORD ? (size_t)h : 0;
    char msg[4 * LOG_RECORD];
    int m = vsnprintf(msg, sizeof(msg), fmt, ap);
    size_t mlen = m > 0 ? ((size_t)m < sizeof(msg) ? (size_t)m : sizeof(msg) - 1) : 0;
    bool cut = m > 0 && (size_t)m >= sizeof(msg);
    while (mlen > 0 && (msg[mlen - 1] == '\n' || msg[mlen - 1] == '\r')) {
        mlen--; /* el '\n' final lo pone el formato */
    }
    for (size_t i = 0; i < mlen; i++) {
        unsigned char c = (unsigned char)msg[i];
        if (c < 0x20 || c == 0x7f) {
            msg[i] = '?'; /* una línea por registro: sin inyección de líneas */
        }
    }
    size_t room = LOG_RECORD - 1 - len; /* reservando el '\n' */
    if (mlen > room) {
        cut = true;
    }
    if (cut) {
        size_t keep = LOG_RECORD - len - (sizeof(TRUNC_MARK) - 1);
        if (keep > mlen) {
            keep = mlen;
        }
        memcpy(out + len, msg, keep);
        memcpy(out + len + keep, TRUNC_MARK, sizeof(TRUNC_MARK) - 1);
        *truncated = true;
        return len + keep + sizeof(TRUNC_MARK) - 1;
    }
    memcpy(out + len, msg, mlen);
    out[len + mlen] = '\n';
    *truncated = false;
    return len + mlen + 1;
}

/* ------------------------------------------------------------------------- */
/* Consumidor                                                                */
/* ------------------------------------------------------------------------- */

/* Escribe un registro; false si se perdió (error o abandono). */
static bool write_record(logger *lg, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        if (atomic_load(&lg->abandon)) {
            return false;
        }
        ssize_t n = write(lg->fd, buf + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd p = {.fd = lg->fd, .events = POLLOUT};
            (void)poll(&p, 1, POLL_STEP); /* destino lleno: esperar por tramos */
            continue;
        }
        atomic_fetch_add(&lg->write_errors, 1);
        return false;
    }
    return true;
}

__attribute__((format(printf, 5, 6))) static size_t
format_simple(char *out, int worker, enum log_level level, bool *trunc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    size_t len = format_record(out, worker, level, 0, fmt, ap, trunc);
    va_end(ap);
    return len;
}

/* Aviso periódico de descartes, escrito por el propio consumidor. */
static void report_drops(logger *lg) {
    uint64_t d = atomic_load(&lg->dropped);
    if (d == lg->dropped_reported || atomic_load(&lg->abandon)) {
        return;
    }
    char buf[LOG_RECORD];
    bool trunc;
    size_t len = format_simple(buf, lg->worker, LOG_WARN, &trunc,
                               "log: %llu mensajes descartados (ring lleno)",
                               (unsigned long long)(d - lg->dropped_reported));
    lg->dropped_reported = d;
    if (write_record(lg, buf, len)) {
        atomic_fetch_add(&lg->written, 1);
    }
}

static void *consumer_main(void *arg) {
    logger *lg = arg;
    char *batch = malloc((size_t)BATCH * LOG_RECORD);
    uint16_t lens[BATCH];
    pthread_mutex_lock(&lg->mu);
    for (;;) {
        while (lg->count == 0 && !lg->stop) {
            pthread_cond_wait(&lg->cv, &lg->mu);
        }
        if (lg->count == 0 && lg->stop) {
            break;
        }
        size_t n = 0;
        while (lg->count > 0 && n < BATCH && batch != NULL) {
            memcpy(batch + n * LOG_RECORD, lg->slots + lg->head * LOG_RECORD, lg->lens[lg->head]);
            lens[n] = lg->lens[lg->head];
            lg->head = (lg->head + 1) % lg->capacity;
            lg->count--;
            n++;
        }
        pthread_mutex_unlock(&lg->mu);
        for (size_t i = 0; i < n; i++) {
            if (write_record(lg, batch + i * LOG_RECORD, lens[i])) {
                atomic_fetch_add(&lg->written, 1);
            } else if (atomic_load(&lg->abandon)) {
                atomic_fetch_add(&lg->dropped, n - i); /* abandono: el resto se pierde */
                break;
            }
        }
        report_drops(lg);
        pthread_mutex_lock(&lg->mu);
        if (atomic_load(&lg->abandon)) {
            atomic_fetch_add(&lg->dropped, lg->count);
            lg->count = 0;
        }
    }
    lg->exited = true;
    pthread_cond_broadcast(&lg->done_cv);
    pthread_mutex_unlock(&lg->mu);
    free(batch);
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* API                                                                       */
/* ------------------------------------------------------------------------- */

static void destroy(logger *lg) {
    if (lg->own_fd && lg->fd >= 0) {
        close(lg->fd);
    }
    pthread_mutex_destroy(&lg->mu);
    pthread_cond_destroy(&lg->cv);
    pthread_cond_destroy(&lg->done_cv);
    free(lg->slots);
    free(lg->lens);
    free(lg);
}

logger *log_start(int fd, bool own_fd, enum log_level min, size_t capacity, int worker) {
    if (capacity == 0) {
        capacity = LOG_RING_CAP;
    }
    logger *lg = calloc(1, sizeof(*lg));
    if (lg == NULL) {
        return NULL;
    }
    lg->slots = malloc(capacity * LOG_RECORD);
    lg->lens = calloc(capacity, sizeof(*lg->lens));
    lg->capacity = capacity;
    lg->fd = fd;
    lg->own_fd = own_fd;
    lg->min = min;
    lg->worker = worker;
    atomic_init(&lg->abandon, false);
    atomic_init(&lg->written, 0);
    atomic_init(&lg->dropped, 0);
    atomic_init(&lg->write_errors, 0);
    atomic_init(&lg->truncated, 0);
    pthread_mutex_init(&lg->mu, NULL);
    pthread_cond_init(&lg->cv, NULL);
    pthread_cond_init(&lg->done_cv, NULL);
    if (lg->slots == NULL || lg->lens == NULL) {
        lg->own_fd = false;
        destroy(lg);
        errno = ENOMEM;
        return NULL;
    }
    sigset_t all;
    sigset_t old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old); /* las señales las atiende el event loop */
    int rc = pthread_create(&lg->thread, NULL, consumer_main, lg);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (rc != 0) {
        lg->own_fd = false;
        destroy(lg);
        errno = rc;
        return NULL;
    }
    return lg;
}

logger *log_open(const char *path, enum log_level min, int worker, char *err, size_t errlen) {
    int fd = STDERR_FILENO;
    bool own = false;
    if (path != NULL && path[0] != '\0') {
        fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NONBLOCK, 0640);
        if (fd < 0) {
            char eb[64];
            (void)snprintf(err, errlen, "%s: %s", path, diag_strerror(errno, eb, sizeof(eb)));
            return NULL;
        }
        own = true;
    }
    logger *lg = log_start(fd, own, min, LOG_RING_CAP, worker);
    if (lg == NULL) {
        char eb[64];
        (void)snprintf(err, errlen, "no se pudo iniciar el log: %s",
                       diag_strerror(errno, eb, sizeof(eb)));
        if (own) {
            close(fd);
        }
    }
    return lg;
}

void log_install(logger *lg) {
    g_logger = lg;
}

logger *log_installed(void) {
    return g_logger;
}

void log_set_role(int worker) {
    g_role = worker;
}

__attribute__((format(printf, 4, 0))) static void vlog(logger *lg, enum log_level level,
                                                       uint64_t gen, const char *fmt, va_list ap) {
    char rec[LOG_RECORD];
    bool trunc = false;
    if (lg == NULL) {
        /* Modo síncrono (maestro, sin hilos). */
        size_t len = format_record(rec, g_role, level, gen, fmt, ap, &trunc);
        size_t off = 0;
        while (off < len) {
            ssize_t n = write(STDERR_FILENO, rec + off, len - off);
            if (n > 0) {
                off += (size_t)n;
            } else if (!(n < 0 && errno == EINTR)) {
                break;
            }
        }
        return;
    }
    if (level < lg->min) {
        return;
    }
    size_t len = format_record(rec, lg->worker, level, gen, fmt, ap, &trunc);
    if (trunc) {
        atomic_fetch_add(&lg->truncated, 1);
    }
    pthread_mutex_lock(&lg->mu);
    if (lg->count == lg->capacity || lg->stop) {
        pthread_mutex_unlock(&lg->mu);
        atomic_fetch_add(&lg->dropped, 1); /* nunca se espera a la escritura */
        return;
    }
    size_t slot = (lg->head + lg->count) % lg->capacity;
    memcpy(lg->slots + slot * LOG_RECORD, rec, len);
    lg->lens[slot] = (uint16_t)len;
    lg->count++;
    bool was_empty = lg->count == 1;
    pthread_mutex_unlock(&lg->mu);
    if (was_empty) {
        pthread_cond_signal(&lg->cv);
    }
}

void log_msg(logger *lg, enum log_level level, uint64_t gen, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(lg, level, gen, fmt, ap);
    va_end(ap);
}

void plogv(enum log_level level, uint64_t gen, const char *fmt, va_list ap) {
    vlog(g_logger, level, gen, fmt, ap);
}

void plog(enum log_level level, uint64_t gen, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(g_logger, level, gen, fmt, ap);
    va_end(ap);
}

void log_get_counters(logger *lg, struct log_counters *out) {
    out->written = atomic_load(&lg->written);
    out->dropped = atomic_load(&lg->dropped);
    out->write_errors = atomic_load(&lg->write_errors);
    out->truncated = atomic_load(&lg->truncated);
}

static void deadline_in(struct timespec *ts, uint32_t ms) {
    clock_gettime(CLOCK_REALTIME, ts); /* pthread_cond_timedwait usa CLOCK_REALTIME */
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

/* Espera a que el consumidor termine como mucho `ms`; con el mutex tomado. */
static bool wait_exit_locked(logger *lg, uint32_t ms) {
    struct timespec dl;
    deadline_in(&dl, ms);
    while (!lg->exited) {
        if (pthread_cond_timedwait(&lg->done_cv, &lg->mu, &dl) == ETIMEDOUT) {
            break;
        }
    }
    return lg->exited;
}

bool log_stop(logger *lg, uint32_t timeout_ms) {
    if (lg == NULL) {
        return true;
    }
    if (g_logger == lg) {
        g_logger = NULL;
    }
    pthread_mutex_lock(&lg->mu);
    lg->stop = true;
    pthread_cond_signal(&lg->cv);
    bool done = wait_exit_locked(lg, timeout_ms);
    if (!done) {
        /* Destino bloqueado: abandonar lo pendiente; el consumidor lo ve al
         * volver de poll() (como mucho POLL_STEP ms). */
        atomic_store(&lg->abandon, true);
        pthread_cond_signal(&lg->cv);
        done = wait_exit_locked(lg, 3 * POLL_STEP);
    }
    pthread_mutex_unlock(&lg->mu);
    if (!done) {
        return false; /* bloqueado en un write() de un fd heredado: no se libera */
    }
    pthread_join(lg->thread, NULL);
    destroy(lg);
    return true;
}
