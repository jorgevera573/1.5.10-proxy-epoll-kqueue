/*
 * listener.h — sockets de escucha no bloqueantes para los frontends.
 *
 * Cada socket se crea con SO_REUSEADDR y SO_REUSEPORT (donde exista), en modo
 * no bloqueante y close-on-exec. Con un único worker SO_REUSEPORT no reparte
 * nada; se activa ya para el futuro modelo multiproceso (pendiente).
 */
#ifndef LISTENER_H
#define LISTENER_H

#include <stddef.h>

#include "config.h"

#define LISTENER_BACKLOG 511

/* Devuelve el fd o -1 con un diagnóstico en err. */
int listener_open(const struct cfg_frontend *fe, char *err, size_t errlen);

/* Pone O_NONBLOCK y FD_CLOEXEC. 0 o -1 con errno. */
int fd_set_nonblock_cloexec(int fd);

#endif /* LISTENER_H */
