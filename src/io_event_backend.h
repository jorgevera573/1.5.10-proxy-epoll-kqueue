/*
 * io_event_backend.h — contrato interno entre io_event.c (lógica común) y
 * cada backend del kernel (io_event_epoll.c / io_event_kqueue.c).
 *
 * El backend solo traduce intereses a llamadas del kernel y eventos del kernel
 * a io_ready. La tabla de registros, la validación de argumentos, la
 * detección de eventos obsoletos y la parada viven en io_event.c, de modo que
 * se comparten entre plataformas.
 *
 * `token` es un valor opaco de 64 bits que el backend debe devolver intacto
 * en cada evento del descriptor (epoll_data.u64 / kevent.udata).
 */
#ifndef IO_EVENT_BACKEND_H
#define IO_EVENT_BACKEND_H

#include <stdint.h>

struct io_backend;

struct io_ready {
    uint64_t token;
    uint32_t events; /* IO_READ | IO_WRITE | IO_HUP | IO_ERROR */
};

/* Crea el backend con capacidad para `capacity` eventos por espera. */
struct io_backend *io_backend_open(int capacity);
void io_backend_close(struct io_backend *be);

/* Todas devuelven 0 o -1 con errno. Siempre edge-triggered. */
int io_backend_add(struct io_backend *be, int fd, uint32_t interest, uint64_t token);
int io_backend_mod(struct io_backend *be, int fd, uint32_t old_interest, uint32_t new_interest,
                   uint64_t token);
int io_backend_del(struct io_backend *be, int fd, uint32_t interest);

/*
 * Espera como máximo `timeout_ms` (-1 infinito) y rellena hasta `capacity`
 * entradas de `out`. Devuelve el número de entradas o -1 con errno (EINTR
 * incluido; lo gestiona el llamador).
 */
int io_backend_wait(struct io_backend *be, struct io_ready *out, int timeout_ms);

#endif /* IO_EVENT_BACKEND_H */
