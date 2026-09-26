/*
 * io_event.h — API común de bucle de eventos sobre epoll (Linux) y kqueue
 * (macOS/BSD).
 *
 * Modelo
 * ------
 * - Notificación edge-triggered (EPOLLET / EV_CLEAR): el callback recibe un
 *   aviso por cada cambio de estado, no mientras el descriptor siga listo.
 *   El callback DEBE leer/escribir hasta obtener EAGAIN/EWOULDBLOCK o, en caso
 *   contrario, guardar que queda trabajo pendiente; si no, no habrá aviso
 *   nuevo hasta que llegue más actividad.
 * - Los descriptores registrados deben ser no bloqueantes (responsabilidad del
 *   llamador; io_event no cambia sus flags).
 * - Un io_loop pertenece a un único hilo (el que llama a io_loop_run). Todas
 *   las funciones salvo io_loop_stop deben llamarse desde ese hilo.
 *
 * Propiedad de recursos
 * ---------------------
 * - io_loop_create reserva el bucle, el descriptor epoll/kqueue y un pipe de
 *   despertar interno; io_loop_destroy los libera.
 * - Los descriptores registrados SIGUEN siendo del llamador: io_loop_del y
 *   io_loop_destroy no los cierran. Hay que llamar a io_loop_del antes de
 *   close(fd).
 * - `userdata` es opaco para io_event y nunca se libera.
 *
 * Parada
 * ------
 * io_loop_stop es la única función segura desde:
 *   - un callback del propio bucle,
 *   - otro hilo,
 *   - un manejador de señales (async-signal-safe: store atómico lock-free y
 *     write() sobre un pipe no bloqueante; preserva errno).
 * La petición de parada es persistente hasta que io_loop_run retorna: si se
 * solicita antes de entrar en io_loop_run, este retorna inmediatamente sin
 * despachar eventos. El lote de eventos ya obtenido del kernel se termina de
 * despachar antes de retornar para no perder avisos edge-triggered.
 * No se debe llamar a io_loop_stop concurrentemente con io_loop_destroy.
 */
#ifndef IO_EVENT_H
#define IO_EVENT_H

#include <stdint.h>

/* Intereses (io_loop_add/io_loop_mod) y eventos entregados al callback. */
enum {
    IO_READ = 1u << 0,  /* datos disponibles o conexión entrante */
    IO_WRITE = 1u << 1, /* se puede escribir sin bloquear */
    IO_HUP = 1u << 2,   /* solo entregado: el par cerró (lectura o total) */
    IO_ERROR = 1u << 3, /* solo entregado: error pendiente en el socket */
};

#define IO_INTEREST_MASK ((uint32_t)(IO_READ | IO_WRITE))

typedef struct io_loop io_loop;

/*
 * Callback de un descriptor listo. `events` es una combinación de IO_READ,
 * IO_WRITE, IO_HUP e IO_ERROR; IO_READ/IO_WRITE solo aparecen si están en el
 * interés actual. Desde el callback se puede llamar a io_loop_add/mod/del
 * (incluso sobre el propio fd), cerrar descriptores y llamar a io_loop_stop.
 * No se puede llamar a io_loop_destroy ni a io_loop_run/io_loop_run_once.
 */
typedef void (*io_callback)(io_loop *loop, int fd, uint32_t events, void *userdata);

/*
 * Crea un bucle. `max_events` es el máximo de eventos por llamada al kernel
 * (<= 0 usa 256). Devuelve NULL con errno fijado si falla.
 */
io_loop *io_loop_create(int max_events);

/* Libera el bucle. No cierra los descriptores registrados. Acepta NULL. */
void io_loop_destroy(io_loop *loop);

/*
 * Registra `fd` con interés `interest` (IO_READ y/o IO_WRITE, no vacío).
 * Devuelve 0 o -1 con errno: EBADF (fd < 0), EINVAL (interés o callback
 * inválidos), EEXIST (ya registrado), ENOMEM o el error del kernel.
 */
int io_loop_add(io_loop *loop, int fd, uint32_t interest, io_callback cb, void *userdata);

/*
 * Cambia el interés de un fd registrado. Rearmar un interés ya activo
 * provoca un nuevo aviso si el fd sigue listo (útil para reanudar trabajo
 * pendiente). Devuelve 0 o -1 con errno: ENOENT, EINVAL o error del kernel.
 */
int io_loop_mod(io_loop *loop, int fd, uint32_t interest);

/*
 * Retira `fd`. Los eventos del lote en curso para ese fd se descartan, aunque
 * el número se reutilice en el mismo lote. Si el kernel devuelve error el fd
 * igualmente queda retirado de la tabla y se devuelve -1 con errno.
 * ENOENT si no estaba registrado.
 */
int io_loop_del(io_loop *loop, int fd);

/*
 * Ejecuta el bucle hasta io_loop_stop. Reintenta EINTR. Devuelve 0 al parar o
 * -1 con errno si falla la espera del kernel (EBUSY si ya está en ejecución).
 */
int io_loop_run(io_loop *loop);

/*
 * Una única espera de como máximo `timeout_ms` (-1 infinito, 0 sondeo) y
 * despacho del lote. Devuelve el número de callbacks invocados (0 si hubo
 * timeout o EINTR) o -1 con errno. Ni consulta ni consume la petición de
 * parada; sirve para integrar el bucle en pruebas o en otros bucles.
 */
int io_loop_run_once(io_loop *loop, int timeout_ms);

/* Solicita la parada de io_loop_run. Ver "Parada" más arriba. */
void io_loop_stop(io_loop *loop);

#endif /* IO_EVENT_H */
