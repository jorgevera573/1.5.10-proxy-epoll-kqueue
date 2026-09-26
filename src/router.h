/*
 * router.h — tabla de rutas inmutable por dominio.
 *
 * Precedencia de router_lookup:
 *   1. dominio exacto (tabla hash djb2);
 *   2. wildcard "*.sufijo": gana el sufijo MÁS LARGO que coincida. Un
 *      wildcard exige al menos una etiqueta completa delante del sufijo:
 *      "*.example.com" coincide con "a.example.com" y "a.b.example.com", pero
 *      no con "example.com" ni con "badexample.com";
 *   3. ruta por defecto, si existe;
 *   4. ROUTE_NONE (el proxy responde 502).
 * La entrada de router_lookup debe estar normalizada con host_normalize
 * (minúsculas, sin puerto, sin punto final); es lo que produce http_parser.
 *
 * Snapshot y propiedad: un router no cambia tras router_build. Nace con una
 * referencia; router_ref/router_unref usan un contador atómico, de modo que
 * una recarga puede publicar un router nuevo mientras las conexiones en curso
 * conservan el anterior, que se libera con la última referencia. El
 * `target` es un identificador opaco (en la etapa 3, índice del pool de
 * backends dentro del mismo snapshot de configuración).
 */
#ifndef ROUTER_H
#define ROUTER_H

#include <stddef.h>
#include <stdint.h>

typedef struct router router;

struct route_spec {
    const char *pattern; /* "dominio" o "*.dominio"; se normaliza */
    uint32_t target;
};

enum route_match { ROUTE_NONE = 0, ROUTE_EXACT, ROUTE_WILDCARD, ROUTE_DEFAULT };

struct route_result {
    enum route_match match;
    uint32_t target;
};

struct router_error {
    size_t index; /* índice en specs del patrón rechazado */
    char msg[160];
};

/*
 * Construye un router. `default_target` NULL = sin ruta por defecto.
 * Devuelve NULL si falla: con errno = EINVAL y `err` rellenado (si no es
 * NULL) ante un patrón inválido o duplicado tras normalizar, o ENOMEM.
 */
router *router_build(const struct route_spec *specs, size_t n, const uint32_t *default_target,
                     struct router_error *err);

struct route_result router_lookup(const router *r, const char *host, size_t len);

router *router_ref(router *r);
/* Libera al llegar a cero. Acepta NULL. */
void router_unref(router *r);

/* djb2 (Bernstein): h = h * 33 + c, h0 = 5381, en 32 bits. */
uint32_t router_djb2(const char *s, size_t len);

size_t router_exact_count(const router *r);
size_t router_wildcard_count(const router *r);

#endif /* ROUTER_H */
