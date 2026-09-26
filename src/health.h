/*
 * health.h — health checks activos (TCP y HTTP) en un hilo dedicado.
 *
 * Propiedad y sincronización
 * --------------------------
 * - El hilo de health posee su io_loop, su timer_heap, las sondas en curso y
 *   sus sockets. No accede a `config` ni a `backend_pools`.
 * - El event loop le entrega un PLAN: una copia autocontenida (direcciones,
 *   petición HTTP ya construida, plazos) etiquetada con el id de generación.
 *   Buzón de un solo elemento protegido por mutex: un plan nuevo sustituye al
 *   pendiente no leído. El hilo despierta con un pipe.
 * - El hilo devuelve RESULTADOS (generación, pool, backend, ok) por una cola
 *   circular acotada protegida por mutex, y avisa con un pipe no bloqueante
 *   que el event loop tiene registrado. Si la cola está llena, el resultado
 *   se descarta y se cuenta (health_dropped): es una muestra periódica.
 * - El event loop aplica un resultado solo si su generación es la actual;
 *   los de generaciones anteriores se descartan (health_stale).
 * - Al recibir un plan nuevo, las sondas inactivas del anterior se eliminan;
 *   las que están en curso terminan (acotadas por su timeout) e informan con
 *   su generación antigua.
 *
 * Límites de recursos: una sonda en curso por backend, un socket por sonda,
 * 512 bytes de buffer, timeout_ms < interval_ms, como máximo
 * CONFIG_MAX_HEALTH backends con sondas.
 *
 * Criterio de salud:
 *   tcp  el connect termina sin error antes de timeout_ms;
 *   http además: "GET <path> HTTP/1.1" con Host y "Connection: close"; la
 *        línea de estado llega antes de timeout_ms y su código está en
 *        [expect_status_min, expect_status_max] (por defecto 200-399).
 */
#ifndef HEALTH_H
#define HEALTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "config.h"

#define HEALTH_QUEUE_CAP 1024

struct health_result {
    uint64_t gen;
    uint32_t pool;
    uint32_t backend;
    bool ok;
    uint16_t status; /* HTTP; 0 si no hubo línea de estado */
    int err;         /* errno del fallo; ETIMEDOUT si venció el plazo */
};

struct health_plan;
typedef struct health_checker health_checker;

/* Construye el plan de `cfg` para la generación `gen` (hilo del event loop). */
struct health_plan *health_plan_build(const struct config *cfg, uint64_t gen);
void health_plan_free(struct health_plan *plan);
size_t health_plan_size(const struct health_plan *plan);

/* Arranca el hilo (con todas las señales bloqueadas). NULL con errno. */
health_checker *health_start(void);

/* fd que se vuelve legible cuando hay resultados. */
int health_notify_fd(const health_checker *hc);

/* Entrega un plan (toma su propiedad). */
void health_submit_plan(health_checker *hc, struct health_plan *plan);

/* Extrae hasta `max` resultados (hilo del event loop). */
size_t health_take_results(health_checker *hc, struct health_result *out, size_t max);

uint64_t health_probes_done(const health_checker *hc);
uint64_t health_results_dropped(const health_checker *hc);

/* Detiene y espera al hilo; libera todo. Acepta NULL. */
void health_stop(health_checker *hc);

#endif /* HEALTH_H */
