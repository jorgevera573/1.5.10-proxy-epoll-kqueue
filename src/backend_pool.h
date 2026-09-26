/*
 * backend_pool.h — generaciones y registro de backends.
 *
 * Dos niveles de estado
 * ---------------------
 * 1. REGISTRO (backend_registry): estado OPERATIVO de cada backend lógico,
 *    que sobrevive a las recargas. Identidad estable = (nombre del pool,
 *    dirección IP:puerto). El mismo pool y dirección en dos generaciones es el
 *    mismo backend lógico y comparte: conexiones activas, salud (y sus
 *    contadores consecutivos), veces elegido y fallos de conexión. La misma
 *    dirección en dos pools distintos son dos backends lógicos: límites y
 *    contadores POR POOL/BACKEND, no por dirección global (max_conns se
 *    configura por entrada de pool).
 *    Cada entrada está referenciada por las generaciones que la contienen y se
 *    libera con la última; así perdura mientras exista una conexión de una
 *    generación antigua que la use.
 *
 * 2. GENERACIÓN (backend_pools): una instantánea de configuración (con su
 *    max_conns, peso, salud configurada...) más el estado propio de sus
 *    algoritmos (cursor de round robin, contadores de weighted), que dependen
 *    de la composición del pool en esa configuración. Cada recarga válida crea
 *    una con id creciente.
 *
 * Todo lo usa solo el hilo del event loop (referencias no atómicas). El hilo
 * de health recibe copias (health.h).
 *
 * Contabilidad: backend_pool_pick incrementa `active` de la entrada del
 * registro; backend_pool_release (con la generación y la selección del pick)
 * la decrementa: siempre la misma entrada, aunque entretanto haya habido
 * recargas. Como la conexión retiene su generación y esta sus entradas, la
 * entrada existe hasta el release.
 *
 * Elegibilidad en una generación: sano (si el pool tiene [pool.health]) y
 * active < max_conns de ESA generación (0 = sin límite). `active` incluye las
 * conexiones de generaciones anteriores del mismo backend lógico: si una
 * recarga baja max_conns por debajo de las activas, estas terminan y no se
 * asignan nuevas hasta bajar del límite. least_conn compara esas mismas
 * activas.
 *
 * Eliminar y reintroducir: si un backend desaparece de la configuración
 * mientras tiene conexiones de generaciones antiguas, su entrada sigue viva
 * (la retienen esas generaciones); si una recarga posterior lo reintroduce en
 * el mismo pool, reutiliza la entrada con sus activas y su salud. Si ya no
 * quedaba ninguna referencia, se crea una entrada nueva (sana, a cero).
 *
 * Salud: activa (fall/rise) y pasiva (passive_fall), como antes, sobre la
 * entrada. En un pool sin [pool.health] el backend se considera sano y, al
 * crear la generación, la entrada se reinicia a sano (nada la actualizaría).
 *
 * Selección: round_robin, weighted (smooth WRR de nginx) y least_conn
 * (empates: el primero desde el cursor, que rota). Sin elegibles ->
 * BP_NONE_ELIGIBLE (503).
 */
#ifndef BACKEND_POOL_H
#define BACKEND_POOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"

enum bp_health { BP_UP = 0, BP_DOWN_ACTIVE, BP_DOWN_PASSIVE };

/* Estado operativo de un backend lógico (vive en el registro). */
struct backend_state {
    uint32_t active;           /* conexiones en curso, de cualquier generación */
    uint64_t selected;         /* veces elegido */
    uint64_t connect_failures; /* fallos reales de conexión */
    enum bp_health health;
    uint32_t probe_fails;
    uint32_t probe_oks;
    uint32_t passive_fails;
};

typedef struct backend_registry backend_registry;
typedef struct backend_pools backend_pools;

enum bp_result { BP_OK = 0, BP_NONE_ELIGIBLE };
enum bp_transition { BP_NO_CHANGE = 0, BP_WENT_DOWN, BP_WENT_UP };

struct bp_choice {
    uint32_t pool;
    uint32_t backend;
};

backend_registry *backend_registry_create(void);
/* Libera el registro; debe estar vacío (sin generaciones vivas). */
void backend_registry_destroy(backend_registry *reg);
/* Entradas vivas (backends lógicos con alguna generación que los contiene). */
size_t backend_registry_size(const backend_registry *reg);

/*
 * Crea la generación `id` para `cfg` (toma una referencia a cfg) enlazando
 * cada backend con su entrada del registro (existente o nueva). Nace con una
 * referencia. NULL sin memoria.
 */
backend_pools *backend_pools_create(struct config *cfg, uint64_t id, backend_registry *reg);
backend_pools *backend_pools_ref(backend_pools *bp);
void backend_pools_unref(backend_pools *bp); /* acepta NULL */

uint64_t backend_pools_id(const backend_pools *bp);
struct config *backend_pools_config(const backend_pools *bp);
/* Generaciones vivas en el proceso (contabilidad). */
size_t backend_pools_live(void);

enum bp_result backend_pool_pick(backend_pools *bp, uint32_t pool, struct bp_choice *out);
void backend_pool_release(backend_pools *bp, const struct bp_choice *ch);

/* Health pasivo. Devuelven la transición provocada (para registrarla). */
enum bp_transition backend_pool_connect_failed(backend_pools *bp, const struct bp_choice *ch);
void backend_pool_connect_ok(backend_pools *bp, const struct bp_choice *ch);

/* Resultado de una sonda activa de esta generación. */
enum bp_transition backend_pool_probe_result(backend_pools *bp, uint32_t pool, uint32_t backend,
                                             bool ok);

const struct backend_state *backend_pool_state(const backend_pools *bp, uint32_t pool,
                                               uint32_t backend);
bool backend_state_eligible(const backend_pools *bp, uint32_t pool, uint32_t backend);
/* Suma de `active` de los backends de esta generación (incluye las
 * conexiones de generaciones anteriores del mismo backend lógico). */
uint64_t backend_pools_active_total(const backend_pools *bp);

#endif /* BACKEND_POOL_H */
