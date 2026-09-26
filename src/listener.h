/*
 * listener.h — sockets de escucha no bloqueantes para los frontends.
 *
 * Cada socket se crea con SO_REUSEADDR, en modo no bloqueante y
 * close-on-exec, y con SO_REUSEPORT solo en el modelo por worker.
 *
 * Modelo de escucha con varios workers (docs/architecture.md §14.1):
 * - LISTENER_PER_WORKER (Linux): cada worker abre su propio socket con
 *   SO_REUSEPORT y el núcleo reparte las conexiones nuevas entre ellos.
 * - LISTENER_SHARED (resto, p. ej. macOS): SO_REUSEPORT permite compartir el
 *   puerto pero no reparte (todas las conexiones van a un solo socket). El
 *   maestro abre un socket por frontend antes del fork y los workers lo
 *   heredan y aceptan de la misma cola.
 */
#ifndef LISTENER_H
#define LISTENER_H

#include <stdbool.h>
#include <stddef.h>

#include "config.h"

#define LISTENER_BACKLOG 511

enum listener_model { LISTENER_PER_WORKER, LISTENER_SHARED };

/* Modelo de la plataforma: por worker solo donde SO_REUSEPORT reparte. */
enum listener_model listener_default_model(void);
const char *listener_model_name(enum listener_model model);

/* Devuelve el fd o -1 con un diagnóstico en err. `reuseport`: activar
 * SO_REUSEPORT (modelo por worker). */
int listener_open(const struct cfg_frontend *fe, bool reuseport, char *err, size_t errlen);

/* Pone O_NONBLOCK y FD_CLOEXEC. 0 o -1 con errno. */
int fd_set_nonblock_cloexec(int fd);

#endif /* LISTENER_H */
