/*
 * router.c — ver router.h.
 *
 * Dos tablas hash con encadenamiento (índices, no punteros), ambas con djb2:
 *   exact     clave = dominio completo;
 *   wildcard  clave = sufijo sin "*." ("*.example.com" -> "example.com").
 * La búsqueda de wildcard recorre los '.' del host de izquierda a derecha y
 * consulta cada sufijo: el primero encontrado es el más largo. Coste
 * O(etiquetas) consultas.
 * Las colisiones de hash se resuelven comparando longitud y bytes.
 */
#include "router.h"

#include "host.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NO_ENTRY UINT32_MAX

struct entry {
    char *key;
    size_t len;
    uint32_t hash;
    uint32_t target;
    uint32_t next; /* siguiente en el bucket o NO_ENTRY */
};

struct table {
    struct entry *entries;
    uint32_t count;
    uint32_t *buckets;
    uint32_t mask;
};

struct router {
    atomic_uint refs;
    struct table exact;
    struct table wild;
    bool has_default;
    uint32_t default_target;
};

uint32_t router_djb2(const char *s, size_t len) {
    uint32_t h = 5381;
    for (size_t i = 0; i < len; i++) {
        h = h * 33u + (unsigned char)s[i];
    }
    return h;
}

static int table_init(struct table *t, size_t capacity) {
    size_t nb = 8;
    while (nb < capacity * 2) {
        nb *= 2;
    }
    t->entries = calloc(capacity ? capacity : 1, sizeof(*t->entries));
    t->buckets = malloc(nb * sizeof(*t->buckets));
    if (t->entries == NULL || t->buckets == NULL) {
        return -1;
    }
    for (size_t i = 0; i < nb; i++) {
        t->buckets[i] = NO_ENTRY;
    }
    t->mask = (uint32_t)(nb - 1);
    return 0;
}

static void table_free(struct table *t) {
    for (uint32_t i = 0; i < t->count; i++) {
        free(t->entries[i].key);
    }
    free(t->entries);
    free(t->buckets);
}

static const struct entry *table_find(const struct table *t, const char *key, size_t len) {
    uint32_t h = router_djb2(key, len);
    for (uint32_t i = t->buckets[h & t->mask]; i != NO_ENTRY; i = t->entries[i].next) {
        const struct entry *e = &t->entries[i];
        if (e->hash == h && e->len == len && memcmp(e->key, key, len) == 0) {
            return e;
        }
    }
    return NULL;
}

/* Inserta; -1 con errno EEXIST si la clave ya existe o ENOMEM. */
static int table_insert(struct table *t, const char *key, size_t len, uint32_t target) {
    if (table_find(t, key, len) != NULL) {
        errno = EEXIST;
        return -1;
    }
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        errno = ENOMEM;
        return -1;
    }
    memcpy(copy, key, len);
    copy[len] = '\0';
    uint32_t h = router_djb2(key, len);
    uint32_t idx = t->count++;
    t->entries[idx] = (struct entry){
        .key = copy, .len = len, .hash = h, .target = target, .next = t->buckets[h & t->mask]};
    t->buckets[h & t->mask] = idx;
    return 0;
}

static void set_err(struct router_error *err, size_t index, const char *pattern, const char *why) {
    if (err == NULL) {
        return;
    }
    err->index = index;
    /* Truncar el mensaje es aceptable. */
    (void)snprintf(err->msg, sizeof(err->msg), "ruta %zu \"%.60s\": %s", index,
                   pattern ? pattern : "(null)", why);
}

router *router_build(const struct route_spec *specs, size_t n, const uint32_t *default_target,
                     struct router_error *err) {
    if (n > UINT32_MAX / 4) {
        errno = EINVAL;
        return NULL;
    }
    router *r = calloc(1, sizeof(*r));
    if (r == NULL) {
        return NULL;
    }
    atomic_init(&r->refs, 1u);
    if (table_init(&r->exact, n) < 0 || table_init(&r->wild, n) < 0) {
        router_unref(r);
        errno = ENOMEM;
        return NULL;
    }
    if (default_target != NULL) {
        r->has_default = true;
        r->default_target = *default_target;
    }

    for (size_t i = 0; i < n; i++) {
        const char *pat = specs[i].pattern;
        if (pat == NULL || pat[0] == '\0') {
            set_err(err, i, pat, "patrón vacío");
            goto invalid;
        }
        size_t len = strlen(pat);
        bool wildcard = len >= 2 && pat[0] == '*' && pat[1] == '.';
        const char *body = wildcard ? pat + 2 : pat;
        size_t body_len = wildcard ? len - 2 : len;

        struct host_name hn;
        enum host_status st = host_normalize(body, body_len, false, &hn);
        if (st != HOST_OK) {
            set_err(err, i, pat, host_status_str(st));
            goto invalid;
        }
        if (wildcard && hn.is_ipv6) {
            set_err(err, i, pat, "wildcard sobre literal IPv6");
            goto invalid;
        }
        struct table *t = wildcard ? &r->wild : &r->exact;
        if (table_insert(t, hn.name, hn.len, specs[i].target) < 0) {
            if (errno == EEXIST) {
                set_err(err, i, pat, "patrón duplicado tras normalizar");
                goto invalid;
            }
            router_unref(r);
            errno = ENOMEM;
            return NULL;
        }
    }
    return r;

invalid:
    router_unref(r);
    errno = EINVAL;
    return NULL;
}

struct route_result router_lookup(const router *r, const char *host, size_t len) {
    const struct entry *e = table_find(&r->exact, host, len);
    if (e != NULL) {
        return (struct route_result){.match = ROUTE_EXACT, .target = e->target};
    }
    if (r->wild.count > 0) {
        /* Sufijos tras cada '.', del más largo al más corto. i > 0 garantiza
         * al menos una etiqueta no vacía delante. */
        for (size_t i = 1; i + 1 < len; i++) {
            if (host[i] != '.') {
                continue;
            }
            e = table_find(&r->wild, host + i + 1, len - i - 1);
            if (e != NULL) {
                return (struct route_result){.match = ROUTE_WILDCARD, .target = e->target};
            }
        }
    }
    if (r->has_default) {
        return (struct route_result){.match = ROUTE_DEFAULT, .target = r->default_target};
    }
    return (struct route_result){.match = ROUTE_NONE, .target = 0};
}

router *router_ref(router *r) {
    atomic_fetch_add_explicit(&r->refs, 1u, memory_order_relaxed);
    return r;
}

void router_unref(router *r) {
    if (r == NULL) {
        return;
    }
    /* acq_rel: las lecturas de otros propietarios ocurren antes de liberar. */
    if (atomic_fetch_sub_explicit(&r->refs, 1u, memory_order_acq_rel) != 1u) {
        return;
    }
    table_free(&r->exact);
    table_free(&r->wild);
    free(r);
}

size_t router_exact_count(const router *r) {
    return r->exact.count;
}

size_t router_wildcard_count(const router *r) {
    return r->wild.count;
}
