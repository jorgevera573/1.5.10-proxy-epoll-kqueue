/*
 * conn.h — conexiones del cliente (client_conn) y del upstream
 * (upstream_conn). Ver docs/architecture.md §5.
 *
 * Propiedad:
 *   - El worker posee la lista de client_conn.
 *   - Un client_conn posee su socket, dos slots (entrada y salida), dos
 *     temporizadores, una referencia a la generación (configuración + estado
 *     de backends) de su petición en curso y,
 *     durante un intercambio, un upstream_conn y una selección de backend.
 *   - Un upstream_conn posee su socket y dos slots; se libera en
 *     upstream_release (punto de extensión del futuro pool: hoy siempre
 *     cierra).
 *   - Solo client_run libera un client_conn, al final de su ejecución; ningún
 *     callback accede a la conexión después (los eventos del fd se retiran
 *     con io_loop_del, que descarta los pendientes, y los temporizadores se
 *     cancelan).
 */
#ifndef CONN_H
#define CONN_H

#include <stddef.h>
#include <sys/socket.h>

#include "worker.h"

/*
 * Crea la conexión para un socket ya aceptado y no bloqueante. Devuelve 0 o
 * -1 si se alcanzó max_connections o no hay slots/memoria; en ese caso el
 * llamador cierra `fd`.
 */
int client_conn_accept(struct worker *w, int fd, const struct sockaddr_storage *peer,
                       size_t fe_idx);

/* Inicio del cierre ordenado: cierra las inactivas y desactiva keep-alive. */
void client_conn_drain_all(struct worker *w);

/* Cierre forzado de todas las conexiones. */
void client_conn_abort_all(struct worker *w);

#endif /* CONN_H */
