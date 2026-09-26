/*
 * io_event.c — lógica común del bucle de eventos, independiente del kernel.
 *
 * Tabla de registros
 * ------------------
 * Array indexado por fd. Cada entrada guarda callback, userdata, interés y
 * una generación que se incrementa en cada io_loop_add. El token enviado al
 * kernel es (generación << 32 | fd). Al despachar, un evento cuyo token no
 * coincide con la entrada actual es obsoleto (el fd se retiró, o se retiró y
 * se reutilizó el número dentro del mismo lote) y se descarta. Así ningún
 * callback recibe eventos de un registro anterior.
 *
 * La tabla puede crecer (realloc) durante el despacho si un callback registra
 * un fd mayor; por eso el despacho nunca conserva punteros a entradas entre
 * callbacks.
 */
#include "io_event.h"

#include "io_event_backend.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* io_loop_stop se usa desde manejadores de señales: requiere atomics sin lock. */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "atomic_int debe ser lock-free");

#define IO_DEFAULT_MAX_EVENTS 256
#define IO_WAKE_TOKEN         UINT64_MAX /* fd == -1: nunca coincide con un registro */

struct io_slot {
    io_callback cb;
    void *userdata;
    uint32_t interest;
    uint32_t gen;
    bool active;
};

struct io_loop {
    struct io_backend *be;
    struct io_ready *ready;
    struct io_slot *slots;
    size_t nslots;
    int wake_rd;
    int wake_wr;
    atomic_int stop_requested;
    bool running;
};

static uint64_t make_token(int fd, uint32_t gen) {
    return ((uint64_t)gen << 32) | (uint64_t)(uint32_t)fd;
}

static int set_nonblock_cloexec(int fd) {
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) {
        return -1;
    }
    int fdfl = fcntl(fd, F_GETFD);
    if (fdfl < 0 || fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC) < 0) {
        return -1;
    }
    return 0;
}

static bool valid_interest(uint32_t interest) {
    return interest != 0 && (interest & ~IO_INTEREST_MASK) == 0;
}

static struct io_slot *slot_for(io_loop *loop, int fd) {
    if (fd < 0 || (size_t)fd >= loop->nslots || !loop->slots[fd].active) {
        return NULL;
    }
    return &loop->slots[fd];
}

static int ensure_slots(io_loop *loop, int fd) {
    size_t need = (size_t)fd + 1;
    if (need <= loop->nslots) {
        return 0;
    }
    size_t n = loop->nslots ? loop->nslots : 64;
    while (n < need) {
        n *= 2;
    }
    struct io_slot *s = realloc(loop->slots, n * sizeof(*s));
    if (s == NULL) {
        errno = ENOMEM;
        return -1;
    }
    memset(s + loop->nslots, 0, (n - loop->nslots) * sizeof(*s));
    loop->slots = s;
    loop->nslots = n;
    return 0;
}

io_loop *io_loop_create(int max_events) {
    if (max_events <= 0) {
        max_events = IO_DEFAULT_MAX_EVENTS;
    }
    io_loop *loop = calloc(1, sizeof(*loop));
    if (loop == NULL) {
        return NULL;
    }
    loop->wake_rd = -1;
    loop->wake_wr = -1;
    atomic_init(&loop->stop_requested, 0);

    loop->ready = calloc((size_t)max_events, sizeof(*loop->ready));
    if (loop->ready == NULL) {
        goto fail;
    }
    loop->be = io_backend_open(max_events);
    if (loop->be == NULL) {
        goto fail;
    }
    int p[2];
    if (pipe(p) < 0) {
        goto fail;
    }
    loop->wake_rd = p[0];
    loop->wake_wr = p[1];
    if (set_nonblock_cloexec(p[0]) < 0 || set_nonblock_cloexec(p[1]) < 0) {
        goto fail;
    }
    if (io_backend_add(loop->be, loop->wake_rd, IO_READ, IO_WAKE_TOKEN) < 0) {
        goto fail;
    }
    return loop;

fail:;
    int saved = errno;
    io_loop_destroy(loop);
    errno = saved;
    return NULL;
}

void io_loop_destroy(io_loop *loop) {
    if (loop == NULL) {
        return;
    }
    if (loop->be != NULL) {
        io_backend_close(loop->be);
    }
    if (loop->wake_rd >= 0) {
        close(loop->wake_rd);
    }
    if (loop->wake_wr >= 0) {
        close(loop->wake_wr);
    }
    free(loop->ready);
    free(loop->slots);
    free(loop);
}

int io_loop_add(io_loop *loop, int fd, uint32_t interest, io_callback cb, void *userdata) {
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    if (loop == NULL || cb == NULL || !valid_interest(interest)) {
        errno = EINVAL;
        return -1;
    }
    if (slot_for(loop, fd) != NULL) {
        errno = EEXIST;
        return -1;
    }
    if (ensure_slots(loop, fd) < 0) {
        return -1;
    }
    struct io_slot *s = &loop->slots[fd];
    /* La generación 0 nunca se usa: al desbordar se vuelve a 1. */
    uint32_t gen = s->gen == UINT32_MAX ? 1 : s->gen + 1;
    if (io_backend_add(loop->be, fd, interest, make_token(fd, gen)) < 0) {
        return -1;
    }
    s->cb = cb;
    s->userdata = userdata;
    s->interest = interest;
    s->gen = gen;
    s->active = true;
    return 0;
}

int io_loop_mod(io_loop *loop, int fd, uint32_t interest) {
    if (loop == NULL || !valid_interest(interest)) {
        errno = EINVAL;
        return -1;
    }
    struct io_slot *s = slot_for(loop, fd);
    if (s == NULL) {
        errno = ENOENT;
        return -1;
    }
    if (io_backend_mod(loop->be, fd, s->interest, interest, make_token(fd, s->gen)) < 0) {
        return -1;
    }
    s->interest = interest;
    return 0;
}

int io_loop_del(io_loop *loop, int fd) {
    if (loop == NULL) {
        errno = EINVAL;
        return -1;
    }
    struct io_slot *s = slot_for(loop, fd);
    if (s == NULL) {
        errno = ENOENT;
        return -1;
    }
    uint32_t interest = s->interest;
    /* Se retira de la tabla antes que del kernel: aunque el kernel falle, los
     * eventos pendientes de este registro ya no se despachan. */
    s->active = false;
    s->cb = NULL;
    s->userdata = NULL;
    s->interest = 0;
    return io_backend_del(loop->be, fd, interest);
}

static void drain_wake_pipe(io_loop *loop) {
    char buf[64];
    for (;;) {
        ssize_t n = read(loop->wake_rd, buf, sizeof(buf));
        if (n > 0) {
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break; /* EAGAIN (vacío) o error: nada más que hacer */
    }
}

static int dispatch_once(io_loop *loop, int timeout_ms) {
    int n = io_backend_wait(loop->be, loop->ready, timeout_ms);
    if (n < 0) {
        return errno == EINTR ? 0 : -1;
    }
    int dispatched = 0;
    for (int i = 0; i < n; i++) {
        uint64_t token = loop->ready[i].token;
        if (token == IO_WAKE_TOKEN) {
            drain_wake_pipe(loop);
            continue;
        }
        int fd = (int)(uint32_t)(token & 0xffffffffu);
        uint32_t gen = (uint32_t)(token >> 32);
        struct io_slot *s = slot_for(loop, fd);
        if (s == NULL || s->gen != gen) {
            continue; /* evento obsoleto */
        }
        /* Descarta READ/WRITE retirados por io_loop_mod dentro del lote. */
        uint32_t ev = loop->ready[i].events & (s->interest | IO_HUP | IO_ERROR);
        if (ev == 0) {
            continue;
        }
        io_callback cb = s->cb;
        void *ud = s->userdata;
        cb(loop, fd, ev, ud);
        dispatched++;
    }
    return dispatched;
}

int io_loop_run_once(io_loop *loop, int timeout_ms) {
    if (loop == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (loop->running) {
        errno = EBUSY;
        return -1;
    }
    loop->running = true;
    int rc = dispatch_once(loop, timeout_ms);
    loop->running = false;
    return rc;
}

int io_loop_run(io_loop *loop) {
    if (loop == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (loop->running) {
        errno = EBUSY;
        return -1;
    }
    loop->running = true;
    int rc = 0;
    while (!atomic_load(&loop->stop_requested)) {
        if (dispatch_once(loop, -1) < 0) {
            rc = -1;
            break;
        }
    }
    int saved = errno;
    atomic_store(&loop->stop_requested, 0);
    loop->running = false;
    errno = saved;
    return rc;
}

void io_loop_stop(io_loop *loop) {
    if (loop == NULL) {
        return;
    }
    int saved = errno;
    atomic_store(&loop->stop_requested, 1);
    /* Despierta una espera bloqueada. Si el pipe está lleno (EAGAIN) ya hay un
     * despertar pendiente; cualquier otro error no es recuperable aquí. */
    ssize_t r;
    do {
        r = write(loop->wake_wr, "", 1);
    } while (r < 0 && errno == EINTR);
    errno = saved;
}
