/*
 * buffer_pool.h — arena mmap de slots de tamaño fijo (16 KB) con freelist.
 *
 * Ámbito: un pool por worker, usado solo desde el hilo de su event loop. No es
 * thread-safe ni está pensado para compartirse entre procesos (MAP_PRIVATE):
 * tras un fork cada proceso tiene su copia independiente.
 *
 * Propiedad: el pool posee la arena. buffer_pool_acquire presta un slot al
 * llamador hasta que lo devuelve con buffer_pool_release. Destruir el pool
 * invalida todos los slots, estén prestados o no.
 *
 * La freelist vive fuera de la arena (array de índices + estado por slot), de
 * modo que escribir en un slot ya devuelto no puede corromperla, y una
 * devolución inválida o duplicada se detecta y se rechaza sin modificarla.
 *
 * El contenido de un slot recién reservado no está definido (no se limpia al
 * devolverlo).
 */
#ifndef BUFFER_POOL_H
#define BUFFER_POOL_H

#include <stddef.h>

#define BUFFER_POOL_SLOT_SIZE ((size_t)16384)
/* Límite superior: 131072 slots = 2 GiB de arena. */
#define BUFFER_POOL_MAX_SLOTS ((size_t)131072)

typedef struct buffer_pool buffer_pool;

/*
 * Crea un pool de `nslots` slots. Devuelve NULL con errno: EINVAL si
 * nslots == 0 o > BUFFER_POOL_MAX_SLOTS; ENOMEM (u otro error de mmap).
 */
buffer_pool *buffer_pool_create(size_t nslots);

/* Libera la arena y los metadatos. Acepta NULL. */
void buffer_pool_destroy(buffer_pool *pool);

/*
 * Presta un slot de BUFFER_POOL_SLOT_SIZE bytes. Empieza en un múltiplo de
 * BUFFER_POOL_SLOT_SIZE desde el inicio de la arena, que está alineada a
 * página (no a 16 KB absolutos). Devuelve NULL con errno = ENOBUFS
 * si está agotado. Orden LIFO: reutiliza primero el último devuelto (caliente
 * en caché).
 */
void *buffer_pool_acquire(buffer_pool *pool);

/*
 * Devuelve un slot. Devuelve 0, o -1 con errno sin tocar la freelist:
 *   EINVAL   puntero NULL, fuera de la arena o que no es inicio de slot;
 *   EALREADY el slot ya estaba libre (devolución duplicada).
 */
int buffer_pool_release(buffer_pool *pool, void *slot);

size_t buffer_pool_capacity(const buffer_pool *pool);
size_t buffer_pool_available(const buffer_pool *pool);

#endif /* BUFFER_POOL_H */
