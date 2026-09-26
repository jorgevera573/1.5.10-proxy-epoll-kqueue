/*
 * io_event_epoll.c — backend Linux sobre epoll, siempre con EPOLLET.
 *
 * EPOLLRDHUP se pide siempre para distinguir el cierre de escritura del par
 * (half-close) sin tener que leer hasta 0. EPOLLERR y EPOLLHUP los reporta el
 * kernel aunque no se pidan.
 */
#include "io_event.h"
#include "io_event_backend.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>

struct io_backend {
    int epfd;
    int capacity;
    struct epoll_event *events;
};

static uint32_t to_epoll(uint32_t interest) {
    uint32_t ev = EPOLLET | EPOLLRDHUP;
    if (interest & IO_READ) {
        ev |= EPOLLIN;
    }
    if (interest & IO_WRITE) {
        ev |= EPOLLOUT;
    }
    return ev;
}

static uint32_t from_epoll(uint32_t ev) {
    uint32_t out = 0;
    if (ev & EPOLLIN) {
        out |= IO_READ;
    }
    if (ev & EPOLLOUT) {
        out |= IO_WRITE;
    }
    if (ev & (EPOLLHUP | EPOLLRDHUP)) {
        out |= IO_HUP;
    }
    if (ev & EPOLLERR) {
        out |= IO_ERROR;
    }
    return out;
}

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
    be->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (be->epfd < 0) {
        int saved = errno;
        free(be->events);
        free(be);
        errno = saved;
        return NULL;
    }
    return be;
}

void io_backend_close(struct io_backend *be) {
    close(be->epfd);
    free(be->events);
    free(be);
}

static int ctl(struct io_backend *be, int op, int fd, uint32_t interest, uint64_t token) {
    struct epoll_event ev = {.events = to_epoll(interest), .data.u64 = token};
    return epoll_ctl(be->epfd, op, fd, &ev);
}

int io_backend_add(struct io_backend *be, int fd, uint32_t interest, uint64_t token) {
    return ctl(be, EPOLL_CTL_ADD, fd, interest, token);
}

int io_backend_mod(struct io_backend *be, int fd, uint32_t old_interest, uint32_t new_interest,
                   uint64_t token) {
    (void)old_interest;
    return ctl(be, EPOLL_CTL_MOD, fd, new_interest, token);
}

int io_backend_del(struct io_backend *be, int fd, uint32_t interest) {
    (void)interest;
    /* El puntero de evento se ignora desde Linux 2.6.9, pero se pasa uno
     * válido por compatibilidad. */
    struct epoll_event ev = {0};
    return epoll_ctl(be->epfd, EPOLL_CTL_DEL, fd, &ev);
}

int io_backend_wait(struct io_backend *be, struct io_ready *out, int timeout_ms) {
    int n = epoll_wait(be->epfd, be->events, be->capacity, timeout_ms);
    for (int i = 0; i < n; i++) {
        out[i].token = be->events[i].data.u64;
        out[i].events = from_epoll(be->events[i].events);
    }
    return n;
}
