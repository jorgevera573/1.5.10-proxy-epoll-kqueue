/*
 * buffer_pool.c — ver buffer_pool.h.
 *
 * Estructuras:
 *   arena    nslots * 16 KB reservados con mmap (páginas bajo demanda).
 *   free_idx pila de índices libres; free_top es su tamaño.
 *   in_use   1 byte por slot: 1 prestado, 0 libre.
 * Reserva y devolución son O(1).
 */
#include "buffer_pool.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>

struct buffer_pool {
    unsigned char *arena;
    size_t arena_len;
    size_t nslots;
    uint32_t *free_idx;
    size_t free_top;
    unsigned char *in_use;
};

buffer_pool *buffer_pool_create(size_t nslots) {
    if (nslots == 0 || nslots > BUFFER_POOL_MAX_SLOTS) {
        errno = EINVAL;
        return NULL;
    }
    buffer_pool *pool = calloc(1, sizeof(*pool));
    if (pool == NULL) {
        return NULL;
    }
    pool->nslots = nslots;
    pool->arena_len = nslots * BUFFER_POOL_SLOT_SIZE;
    pool->free_idx = calloc(nslots, sizeof(*pool->free_idx));
    pool->in_use = calloc(nslots, 1);
    if (pool->free_idx == NULL || pool->in_use == NULL) {
        buffer_pool_destroy(pool);
        errno = ENOMEM;
        return NULL;
    }
    void *arena =
        mmap(NULL, pool->arena_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (arena == MAP_FAILED) {
        int saved = errno;
        buffer_pool_destroy(pool);
        errno = saved;
        return NULL;
    }
    pool->arena = arena;
    /* Apila de forma que el primer acquire devuelva el slot 0. */
    for (size_t i = 0; i < nslots; i++) {
        pool->free_idx[i] = (uint32_t)(nslots - 1 - i);
    }
    pool->free_top = nslots;
    return pool;
}

void buffer_pool_destroy(buffer_pool *pool) {
    if (pool == NULL) {
        return;
    }
    if (pool->arena != NULL) {
        munmap(pool->arena, pool->arena_len);
    }
    free(pool->free_idx);
    free(pool->in_use);
    free(pool);
}

void *buffer_pool_acquire(buffer_pool *pool) {
    if (pool->free_top == 0) {
        errno = ENOBUFS;
        return NULL;
    }
    uint32_t idx = pool->free_idx[--pool->free_top];
    pool->in_use[idx] = 1;
    return pool->arena + (size_t)idx * BUFFER_POOL_SLOT_SIZE;
}

/* El slot se devuelve en propiedad: se mantiene void * como en free(). */
// cppcheck-suppress constParameterPointer
int buffer_pool_release(buffer_pool *pool, void *slot) {
    /* Comparación por direcciones enteras: comparar punteros a objetos
     * distintos con < es comportamiento indefinido en C. */
    uintptr_t p = (uintptr_t)slot;
    uintptr_t base = (uintptr_t)pool->arena;
    if (slot == NULL || p < base || p - base >= pool->arena_len ||
        (p - base) % BUFFER_POOL_SLOT_SIZE != 0) {
        errno = EINVAL;
        return -1;
    }
    size_t idx = (p - base) / BUFFER_POOL_SLOT_SIZE;
    if (!pool->in_use[idx]) {
        errno = EALREADY;
        return -1;
    }
    pool->in_use[idx] = 0;
    pool->free_idx[pool->free_top++] = (uint32_t)idx;
    return 0;
}

size_t buffer_pool_capacity(const buffer_pool *pool) {
    return pool->nslots;
}

size_t buffer_pool_available(const buffer_pool *pool) {
    return pool->free_top;
}
