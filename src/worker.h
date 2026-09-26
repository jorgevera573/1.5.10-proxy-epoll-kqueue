/*
 * worker.h — un proceso worker: bucle de eventos, temporizadores, listeners
 * propios (SO_REUSEPORT), conexiones, health checks, preparación/activación
 * de configuraciones y cierre ordenado. Lo crea el maestro (master.h) con
 * fork y se comunica con él por un socketpair (ipc.h).
 *
 * Hilos del worker (todos creados DESPUÉS del fork):
 *   - principal (event loop): todo lo de `struct worker`, conexiones,
 *     generaciones y registro de backends;
 *   - consumidor de log (log.h);
 *   - health (health.h): sus sondas; plan y resultados por buzón y cola;
 *   - preparación (efímero): analiza los bytes de una configuración candidata
 *     y avisa por un pipe; el bucle nunca espera a que termine.
 *
 * Señales (manejador async-signal-safe: write() en un self-pipe):
 *   SIGTERM/SIGINT  cierre ordenado; segunda vez, forzado.
 *   SIGHUP, SIGUSR1 ignoradas: recarga y estadísticas llegan por IPC.
 *   SIGPIPE         ignorada.
 * El worker se pone en su propio grupo de procesos para que un Ctrl+C del
 * terminal llegue solo al maestro, que coordina el cierre.
 */
#ifndef WORKER_H
#define WORKER_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_pool.h"
#include "buffer_pool.h"
#include "config.h"
#include "health.h"
#include "io_event.h"
#include "ipc.h"
#include "log.h"
#include "timer.h"

struct client_conn;

struct worker_stats {
    uint64_t accepted;
    uint64_t rejected; /* aceptadas y cerradas por límite, slots o EMFILE */
    uint64_t accept_emfile;
    uint64_t requests;
    uint64_t responses_4xx; /* generadas por el proxy */
    uint64_t responses_5xx;
    uint64_t upstream_reusable; /* candidatas a pool (se cierran igualmente) */
    uint64_t reloads_ok;
    uint64_t reloads_failed;
    uint64_t health_results;
    uint64_t health_stale; /* resultados de generaciones anteriores, descartados */
};

/*
 * Trabajo del hilo de recarga. El hilo escribe `result`/`err`, pone
 * `done` (release) y, como última acción antes de retornar, avisa por el
 * pipe. El event loop solo lee el trabajo y hace pthread_join cuando ve
 * `done` (acquire) tras el aviso: en ese punto el hilo ya no hace nada salvo
 * retornar, así que el join no espera trabajo pendiente.
 */
struct reload_job {
    pthread_t thread;
    char *text;        /* bytes de la configuración candidata (propios) */
    uint64_t gen;      /* generación candidata asignada por el maestro */
    uint32_t delay_ms; /* solo en la variante de pruebas (PROXY_TEST_HOOKS) */
    int done_wr;
    atomic_bool done;
    bool aborted; /* solo el event loop: ABORT llegó antes de terminar */
    struct config *result;
    char err[CONFIG_ERR_LEN];
};

struct worker {
    int index;
    struct ipc_chan ctl; /* canal con el maestro */
    logger *log;
    uint64_t start_ms;
    backend_pools *candidate; /* preparada, pendiente de COMMIT */
    timer candidate_timer;
    backend_registry *registry; /* estado operativo de backends entre generaciones */
    backend_pools *gen;         /* generación vigente (referencia propia) */
    io_loop *loop;
    timer_heap timers;
    buffer_pool *bufs;
    int *listen_fds;
    size_t nlisten;
    bool shared_listeners; /* heredados del maestro (modelo compartido) */
    int spare_fd;          /* reservado para descartar conexiones con EMFILE */
    timer accept_retry;
    int sig_rd;
    int sig_wr;
    struct client_conn *conns;
    size_t nconns;
    size_t nupstreams;
    bool draining;
    bool stop;
    timer drain_timer;
    health_checker *health;
    struct reload_job *reload; /* hilo de preparación en marcha, o NULL */
    /* PREPARE recibido mientras termina una preparación ya abortada: se
     * arranca al recogerla (solo el último; NULL si no hay). */
    char *pending_text;
    size_t pending_len;
    uint64_t pending_gen;
    int reload_rd;
    int reload_wr;
    uint32_t reload_delay_ms;
    uint64_t hold_commit_gen; /* solo proxy-testhooks: COMMIT retenido (0 = ninguno) */
    uint64_t fail_commit_gen; /* solo proxy-testhooks: COMMIT_FAILED forzado (0 = ninguno) */
    struct worker_stats stats;
};

/*
 * Ejecuta el worker `index` con `cfg` como generación `gen_id` y el canal
 * `ctl_fd` con el maestro (-1: sin maestro, solo pruebas). `shared_fds`:
 * listeners heredados del maestro, uno por frontend (modelo compartido; el
 * worker toma su propiedad), o NULL para abrir los suyos con SO_REUSEPORT.
 * Devuelve el código de salida: 0 limpio, 1 fallo de arranque, 70 recursos
 * prestados al salir.
 */
int worker_run(struct config *cfg, uint64_t gen_id, int ctl_fd, int index, const int *shared_fds);

/* Registra en stderr un cambio de salud de un backend. */
void worker_log_transition(struct worker *w, const backend_pools *gen, uint32_t pool,
                           uint32_t backend, enum bp_transition tr, const char *why);

#endif /* WORKER_H */
