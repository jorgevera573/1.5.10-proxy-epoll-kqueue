/*
 * io_event_kqueue.c — backend macOS/BSD sobre kqueue, siempre con EV_CLEAR.
 *
 * kqueue registra filtros independientes (EVFILT_READ, EVFILT_WRITE) por fd,
 * por lo que un fd con ambos intereses puede producir dos entradas io_ready
 * en el mismo lote. Los cambios se aplican con EV_RECEIPT para conocer el
 * resultado de cada filtro y deshacer los aplicados si alguno falla.
 *
 * ESTADO: sin verificar. Este fichero no se ha compilado ni ejecutado todavía
 * en macOS/BSD; ver docs/verification.md (R01).
 */
#include "io_event.h"
#include "io_event_backend.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* El token de 64 bits viaja en udata (puntero). */
_Static_assert(sizeof(void *) >= sizeof(uint64_t), "kqueue udata debe poder guardar 64 bits");

struct io_backend {
    int kq;
    int capacity;
    struct kevent *events;
};

struct io_backend *io_backend_open(int capacity) {
    struct io_backend *be = calloc(1, sizeof(*be));
    if (be == NULL) {
        return NULL;
    }
    be->capacity = capacity;
    be->events = calloc((size_t)capacity, sizeof(*be->events));
    if (be->events == NULL) {
        free(be);
        errno = ENOMEM;
        return NULL;
    }
    be->kq = kqueue();
    if (be->kq < 0 || fcntl(be->kq, F_SETFD, FD_CLOEXEC) < 0) {
        int saved = errno;
        if (be->kq >= 0) {
            close(be->kq);
        }
        free(be->events);
        free(be);
        errno = saved;
        return NULL;
    }
    return be;
}

void io_backend_close(struct io_backend *be) {
    close(be->kq);
    free(be->events);
    free(be);
}

static void *token_ptr(uint64_t token) {
    return (void *)(uintptr_t)token;
}

/*
 * Aplica hasta dos cambios con EV_RECEIPT. Devuelve 0 si todos se aplicaron;
 * si no, -1 con errno del primer fallo y `ok[i]` indica cuáles sí se
 * aplicaron para que el llamador los deshaga.
 */
static int apply(struct io_backend *be, struct kevent *ch, int n, int ok[2]) {
    struct kevent res[2];
    for (int i = 0; i < n; i++) {
        ch[i].flags |= EV_RECEIPT;
        ok[i] = 0;
    }
    int r;
    do {
        r = kevent(be->kq, ch, n, res, n, NULL);
    } while (r < 0 && errno == EINTR);
    if (r < 0) {
        return -1;
    }
    int first_err = 0;
    for (int i = 0; i < r; i++) {
        /* Con EV_RECEIPT cada resultado lleva EV_ERROR; data = 0 si fue bien. */
        int err = (res[i].flags & EV_ERROR) ? (int)res[i].data : 0;
        for (int j = 0; j < n; j++) {
            if (res[i].filter == ch[j].filter) {
                ok[j] = (err == 0);
            }
        }
        if (err != 0 && first_err == 0) {
            first_err = err;
        }
    }
    if (first_err != 0) {
        errno = first_err;
        return -1;
    }
    return 0;
}

/* Tipo de transición de cada cambio, para poder deshacerlo. */
enum { CH_REARM = 0, CH_ADDED = 1, CH_DELETED = 2 };

static void undo(struct io_backend *be, const struct kevent *ch, const int *kind, int n,
                 const int ok[2]) {
    struct kevent rb[2];
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (!ok[i] || kind[i] == CH_REARM) {
            continue; /* un rearme no altera qué filtros existen */
        }
        rb[m] = ch[i];
        rb[m].flags = kind[i] == CH_ADDED ? EV_DELETE : (EV_ADD | EV_CLEAR);
        m++;
    }
    if (m > 0) {
        /* Mejor esfuerzo: si también falla no hay más recuperación posible. */
        (void)kevent(be->kq, rb, m, NULL, 0, NULL);
    }
}

/* Construye los cambios para pasar de `from` a `to`. */
static int build(struct kevent *ch, int *kind, int fd, uint32_t from, uint32_t to, uint64_t token) {
    int n = 0;
    static const struct {
        uint32_t bit;
        short filter;
    } map[] = {{IO_READ, EVFILT_READ}, {IO_WRITE, EVFILT_WRITE}};
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        uint32_t bit = map[i].bit;
        if ((to & bit) && !(from & bit)) {
            EV_SET(&ch[n], (uintptr_t)fd, map[i].filter, EV_ADD | EV_CLEAR, 0, 0, token_ptr(token));
            kind[n++] = CH_ADDED;
        } else if (!(to & bit) && (from & bit)) {
            EV_SET(&ch[n], (uintptr_t)fd, map[i].filter, EV_DELETE, 0, 0, token_ptr(token));
            kind[n++] = CH_DELETED;
        } else if ((to & bit) && (from & bit)) {
            /* Rearme: EV_ADD sobre un filtro existente actualiza y reactiva. */
            EV_SET(&ch[n], (uintptr_t)fd, map[i].filter, EV_ADD | EV_CLEAR, 0, 0, token_ptr(token));
            kind[n++] = CH_REARM;
        }
    }
    return n;
}

int io_backend_add(struct io_backend *be, int fd, uint32_t interest, uint64_t token) {
    struct kevent ch[2];
    int kind[2];
    int ok[2];
    int n = build(ch, kind, fd, 0, interest, token);
    if (apply(be, ch, n, ok) < 0) {
        int saved = errno;
        undo(be, ch, kind, n, ok);
        errno = saved;
        return -1;
    }
    return 0;
}

int io_backend_mod(struct io_backend *be, int fd, uint32_t old_interest, uint32_t new_interest,
                   uint64_t token) {
    struct kevent ch[2];
    int kind[2];
    int ok[2];
    int n = build(ch, kind, fd, old_interest, new_interest, token);
    if (apply(be, ch, n, ok) < 0) {
        int saved = errno;
        undo(be, ch, kind, n, ok);
        errno = saved;
        return -1;
    }
    return 0;
}

int io_backend_del(struct io_backend *be, int fd, uint32_t interest) {
    struct kevent ch[2];
    int kind[2];
    int ok[2];
    int n = build(ch, kind, fd, interest, 0, 0);
    if (apply(be, ch, n, ok) < 0) {
        /* ENOENT: el filtro ya no existía (p. ej. fd cerrado); no es error. */
        return errno == ENOENT ? 0 : -1;
    }
    return 0;
}

int io_backend_wait(struct io_backend *be, struct io_ready *out, int timeout_ms) {
    struct timespec ts;
    struct timespec *tsp = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }
    int n = kevent(be->kq, NULL, 0, be->events, be->capacity, tsp);
    for (int i = 0; i < n; i++) {
        const struct kevent *kev = &be->events[i];
        uint32_t ev = 0;
        if (kev->flags & EV_ERROR) {
            ev |= IO_ERROR;
        } else {
            if (kev->filter == EVFILT_READ) {
                ev |= IO_READ;
            } else if (kev->filter == EVFILT_WRITE) {
                ev |= IO_WRITE;
            }
            if (kev->flags & EV_EOF) {
                ev |= IO_HUP;
                if (kev->fflags != 0) {
                    ev |= IO_ERROR; /* fflags contiene el errno del socket */
                }
            }
        }
        out[i].token = (uint64_t)(uintptr_t)kev->udata;
        out[i].events = ev;
    }
    return n;
}
