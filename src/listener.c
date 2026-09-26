/*
 * listener.c — ver listener.h.
 */
#include "listener.h"

#include "diag.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int fd_set_nonblock_cloexec(int fd) {
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

enum listener_model listener_default_model(void) {
#if defined(__linux__) && defined(SO_REUSEPORT)
    return LISTENER_PER_WORKER;
#else
    return LISTENER_SHARED;
#endif
}

const char *listener_model_name(enum listener_model model) {
    return model == LISTENER_PER_WORKER ? "per_worker_reuseport" : "shared_inherited";
}

int listener_open(const struct cfg_frontend *fe, bool reuseport, char *err, size_t errlen) {
    const struct cfg_addr *a = &fe->listen;
    int fd = socket(a->ss.ss_family, SOCK_STREAM, 0);
    if (fd < 0) {
        char eb[128];
        (void)snprintf(err, errlen, "%s: socket: %s", a->text,
                       diag_strerror(errno, eb, sizeof(eb)));
        return -1;
    }
    int one = 1;
    const char *step = NULL;
    if (fd_set_nonblock_cloexec(fd) < 0) {
        step = "fcntl";
    } else if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
        step = "SO_REUSEADDR";
    }
#ifdef SO_REUSEPORT
    else if (reuseport && setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) < 0) {
        step = "SO_REUSEPORT";
    }
#else
    else if (reuseport) {
        errno = ENOPROTOOPT;
        step = "SO_REUSEPORT";
    }
#endif
    else if (bind(fd, (const struct sockaddr *)&a->ss, a->len) < 0) {
        step = "bind";
    } else if (listen(fd, LISTENER_BACKLOG) < 0) {
        step = "listen";
    }
    if (step != NULL) {
        char eb[128];
        (void)snprintf(err, errlen, "%s: %s: %s", a->text, step,
                       diag_strerror(errno, eb, sizeof(eb)));
        close(fd);
        return -1;
    }
    return fd;
}
