/*
 * backend_pool.c — ver backend_pool.h.
 */
#include "backend_pool.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "router.h"

#define REG_BUCKETS 1024u

/* Entrada del registro: un backend lógico (pool, dirección). */
struct backend_rt {
    struct backend_rt *next; /* en su bucket */
    char *pool_name;
    struct cfg_addr addr;
    uint32_t hash;
    unsigned refs; /* generaciones que la contienen */
    struct backend_state st;
};

struct backend_registry {
    struct backend_rt *buckets[REG_BUCKETS];
    size_t count;
};

/* Estado de algoritmo de un pool en una generación. */
struct pool_gen {
    size_t rr_next;
    struct backend_rt **rt; /* entradas del registro, por índice de backend */
    int64_t *swrr;          /* weighted: contador por backend */
};

struct backend_pools {
    uint64_t id;
    unsigned refs;
    struct config *cfg;
    backend_registry *reg;
    struct pool_gen *pools;
};

static size_t g_live; /* solo el hilo del event loop */

/* ------------------------------------------------------------------------- */
/* Registro                                                                  */
/* ------------------------------------------------------------------------- */

backend_registry *backend_registry_create(void) {
    return calloc(1, sizeof(struct backend_registry));
}

void backend_registry_destroy(backend_registry *reg) {
    if (reg == NULL) {
        return;
    }
    assert(reg->count == 0);
    free(reg);
}

size_t backend_registry_size(const backend_registry *reg) {
    return reg->count;
}

static uint32_t identity_hash(const char *pool, const struct cfg_addr *a) {
    uint32_t h = router_djb2(pool, strlen(pool));
    const unsigned char *p = (const unsigned char *)&a->ss;
    for (socklen_t i = 0; i < a->len; i++) {
        h = h * 33u + p[i];
    }
    return h;
}

static bool same_identity(const struct backend_rt *e, const char *pool, const struct cfg_addr *a) {
    return strcmp(e->pool_name, pool) == 0 && e->addr.len == a->len &&
           memcmp(&e->addr.ss, &a->ss, a->len) == 0;
}

/* Busca o crea la entrada y toma una referencia. NULL sin memoria. */
static struct backend_rt *registry_acquire(backend_registry *reg, const char *pool,
                                           const struct cfg_addr *a) {
    uint32_t h = identity_hash(pool, a);
    struct backend_rt **bucket = &reg->buckets[h % REG_BUCKETS];
    for (struct backend_rt *e = *bucket; e != NULL; e = e->next) {
        if (e->hash == h && same_identity(e, pool, a)) {
            e->refs++;
            return e;
        }
    }
    struct backend_rt *e = calloc(1, sizeof(*e));
    size_t n = strlen(pool) + 1;
    char *name = malloc(n);
    if (e == NULL || name == NULL) {
        free(e);
        free(name);
        return NULL;
    }
    memcpy(name, pool, n);
    e->pool_name = name;
    e->addr = *a;
    e->hash = h;
    e->refs = 1;
    e->st.health = BP_UP;
    e->next = *bucket;
    *bucket = e;
    reg->count++;
    return e;
}

static void registry_release(backend_registry *reg, struct backend_rt *e) {
    if (e == NULL || --e->refs > 0) {
        return;
    }
    assert(e->st.active == 0); /* la última generación no tiene conexiones */
    struct backend_rt **pp = &reg->buckets[e->hash % REG_BUCKETS];
    while (*pp != e) {
        pp = &(*pp)->next;
    }
    *pp = e->next;
    free(e->pool_name);
    free(e);
    reg->count--;
}

/* ------------------------------------------------------------------------- */
/* Generaciones                                                              */
/* ------------------------------------------------------------------------- */

static void generation_free(backend_pools *bp) {
    for (size_t i = 0; bp->pools != NULL && i < bp->cfg->npools; i++) {
        struct pool_gen *pg = &bp->pools[i];
        for (size_t j = 0; pg->rt != NULL && j < bp->cfg->pools[i].nbackends; j++) {
            registry_release(bp->reg, pg->rt[j]);
        }
        free((void *)pg->rt);
        free(pg->swrr);
    }
    free(bp->pools);
    config_unref(bp->cfg);
    free(bp);
    g_live--;
}

backend_pools *backend_pools_create(struct config *cfg, uint64_t id, backend_registry *reg) {
    backend_pools *bp = calloc(1, sizeof(*bp));
    if (bp == NULL) {
        return NULL;
    }
    bp->id = id;
    bp->refs = 1;
    bp->cfg = config_ref(cfg);
    bp->reg = reg;
    g_live++;
    bp->pools = calloc(cfg->npools, sizeof(*bp->pools));
    if (bp->pools == NULL) {
        generation_free(bp);
        return NULL;
    }
    for (size_t i = 0; i < cfg->npools; i++) {
        const struct cfg_pool *cp = &cfg->pools[i];
        struct pool_gen *pg = &bp->pools[i];
        pg->rt = (struct backend_rt **)calloc(cp->nbackends, sizeof(struct backend_rt *));
        pg->swrr = calloc(cp->nbackends, sizeof(int64_t));
        if (pg->rt == NULL || pg->swrr == NULL) {
            generation_free(bp);
            return NULL;
        }
        for (size_t j = 0; j < cp->nbackends; j++) {
            pg->rt[j] = registry_acquire(reg, cp->name, &cp->backends[j].addr);
            if (pg->rt[j] == NULL) {
                generation_free(bp);
                return NULL;
            }
            if (cp->health.type == HEALTH_NONE) {
                /* Sin sondas nada lo readmitiría: sano y contadores a cero. */
                struct backend_state *st = &pg->rt[j]->st;
                st->health = BP_UP;
                st->probe_fails = st->probe_oks = st->passive_fails = 0;
            }
        }
    }
    return bp;
}

backend_pools *backend_pools_ref(backend_pools *bp) {
    bp->refs++;
    return bp;
}

void backend_pools_unref(backend_pools *bp) {
    if (bp == NULL) {
        return;
    }
    assert(bp->refs > 0);
    if (--bp->refs == 0) {
        generation_free(bp);
    }
}

uint64_t backend_pools_id(const backend_pools *bp) {
    return bp->id;
}

struct config *backend_pools_config(const backend_pools *bp) {
    return bp->cfg;
}

size_t backend_pools_live(void) {
    return g_live;
}

static struct backend_state *state_of(const backend_pools *bp, uint32_t pool, uint32_t backend) {
    return &bp->pools[pool].rt[backend]->st;
}

bool backend_state_eligible(const backend_pools *bp, uint32_t pool, uint32_t backend) {
    const struct backend_state *st = state_of(bp, pool, backend);
    const struct cfg_pool *cp = &bp->cfg->pools[pool];
    uint32_t max = cp->backends[backend].max_conns;
    bool healthy = cp->health.type == HEALTH_NONE || st->health == BP_UP;
    return healthy && (max == 0 || st->active < max);
}

/* ------------------------------------------------------------------------- */
/* Selección                                                                 */
/* ------------------------------------------------------------------------- */

static long pick_round_robin(const backend_pools *bp, uint32_t pool) {
    const struct pool_gen *pg = &bp->pools[pool];
    size_t n = bp->cfg->pools[pool].nbackends;
    for (size_t k = 0; k < n; k++) {
        size_t i = (pg->rr_next + k) % n;
        if (backend_state_eligible(bp, pool, (uint32_t)i)) {
            return (long)i;
        }
    }
    return -1;
}

static long pick_weighted(backend_pools *bp, uint32_t pool) {
    const struct cfg_pool *cp = &bp->cfg->pools[pool];
    int64_t *cur = bp->pools[pool].swrr;
    int64_t total = 0;
    long best = -1;
    for (size_t i = 0; i < cp->nbackends; i++) {
        if (!backend_state_eligible(bp, pool, (uint32_t)i)) {
            continue;
        }
        cur[i] += cp->backends[i].weight;
        total += cp->backends[i].weight;
        if (best < 0 || cur[i] > cur[best]) {
            best = (long)i;
        }
    }
    if (best >= 0) {
        cur[best] -= total;
    }
    return best;
}

static long pick_least_conn(const backend_pools *bp, uint32_t pool) {
    const struct pool_gen *pg = &bp->pools[pool];
    size_t n = bp->cfg->pools[pool].nbackends;
    long best = -1;
    uint32_t best_active = 0;
    for (size_t k = 0; k < n; k++) {
        size_t i = (pg->rr_next + k) % n;
        if (!backend_state_eligible(bp, pool, (uint32_t)i)) {
            continue;
        }
        /* Activas de todas las generaciones; en empate gana el primero desde
         * el cursor (estrictamente menor). */
        uint32_t a = pg->rt[i]->st.active;
        if (best < 0 || a < best_active) {
            best = (long)i;
            best_active = a;
        }
    }
    return best;
}

enum bp_result backend_pool_pick(backend_pools *bp, uint32_t pool, struct bp_choice *out) {
    long i = -1;
    switch (bp->cfg->pools[pool].algorithm) {
    case LB_ROUND_ROBIN:
        i = pick_round_robin(bp, pool);
        break;
    case LB_WEIGHTED:
        i = pick_weighted(bp, pool);
        break;
    case LB_LEAST_CONN:
        i = pick_least_conn(bp, pool);
        break;
    }
    if (i < 0) {
        return BP_NONE_ELIGIBLE;
    }
    struct pool_gen *pg = &bp->pools[pool];
    pg->rr_next = ((size_t)i + 1) % bp->cfg->pools[pool].nbackends;
    pg->rt[i]->st.active++;
    pg->rt[i]->st.selected++;
    *out = (struct bp_choice){.pool = pool, .backend = (uint32_t)i};
    return BP_OK;
}

/* Modifica el estado del registro a través de bp: no se declara const. */
// cppcheck-suppress constParameterPointer
void backend_pool_release(backend_pools *bp, const struct bp_choice *ch) {
    struct backend_state *st = state_of(bp, ch->pool, ch->backend);
    assert(st->active > 0);
    if (st->active > 0) { /* en release, sin assert: no desbordar */
        st->active--;
    }
}

/* ------------------------------------------------------------------------- */
/* Salud                                                                     */
/* ------------------------------------------------------------------------- */

enum bp_transition backend_pool_connect_failed(backend_pools *bp, const struct bp_choice *ch) {
    const struct cfg_health *h = &bp->cfg->pools[ch->pool].health;
    struct backend_state *st = state_of(bp, ch->pool, ch->backend);
    st->connect_failures++;
    if (h->type == HEALTH_NONE || h->passive_fall == 0) {
        return BP_NO_CHANGE;
    }
    st->passive_fails++;
    if (st->health == BP_UP && st->passive_fails >= h->passive_fall) {
        st->health = BP_DOWN_PASSIVE;
        st->probe_oks = 0;
        return BP_WENT_DOWN;
    }
    return BP_NO_CHANGE;
}

/* Modifica el estado del registro a través de bp: no se declara const. */
// cppcheck-suppress constParameterPointer
void backend_pool_connect_ok(backend_pools *bp, const struct bp_choice *ch) {
    state_of(bp, ch->pool, ch->backend)->passive_fails = 0;
}

enum bp_transition backend_pool_probe_result(backend_pools *bp, uint32_t pool, uint32_t backend,
                                             bool ok) {
    const struct cfg_health *h = &bp->cfg->pools[pool].health;
    struct backend_state *st = state_of(bp, pool, backend);
    if (h->type == HEALTH_NONE) {
        return BP_NO_CHANGE;
    }
    if (ok) {
        st->probe_fails = 0;
        st->probe_oks++;
        if (st->health != BP_UP && st->probe_oks >= h->rise) {
            st->health = BP_UP;
            st->passive_fails = 0;
            return BP_WENT_UP;
        }
        return BP_NO_CHANGE;
    }
    st->probe_oks = 0;
    st->probe_fails++;
    if (st->health == BP_UP && st->probe_fails >= h->fall) {
        st->health = BP_DOWN_ACTIVE;
        return BP_WENT_DOWN;
    }
    return BP_NO_CHANGE;
}

const struct backend_state *backend_pool_state(const backend_pools *bp, uint32_t pool,
                                               uint32_t backend) {
    return state_of(bp, pool, backend);
}

uint64_t backend_pools_active_total(const backend_pools *bp) {
    uint64_t total = 0;
    for (size_t i = 0; i < bp->cfg->npools; i++) {
        for (size_t j = 0; j < bp->cfg->pools[i].nbackends; j++) {
            total += state_of(bp, (uint32_t)i, (uint32_t)j)->active;
        }
    }
    return total;
}
