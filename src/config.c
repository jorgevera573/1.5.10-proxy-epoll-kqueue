/*
 * config.c — ver config.h.
 */
#include "config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "diag.h"
#include "host.h"
#include "toml.h"

#define MAX_TIMEOUT_MS 3600000u

#if defined(__GNUC__)
#define PRINTF_LIKE(f, a) __attribute__((format(printf, f, a)))
#else
#define PRINTF_LIKE(f, a)
#endif

struct ctx {
    char *err;
    size_t errlen;
    const char *origin;
};

PRINTF_LIKE(2, 3)
static void fail_impl(struct ctx *c, const char *fmt, ...) {
    int n = snprintf(c->err, c->errlen, "%s: ", c->origin);
    if (n < 0 || (size_t)n >= c->errlen) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(c->err + n, c->errlen - (size_t)n, fmt, ap);
    va_end(ap);
}

/* Registra el diagnóstico y vale `false` (explícito para el analizador). */
#define fail(...) (fail_impl(__VA_ARGS__), false)

static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d != NULL) {
        memcpy(d, s, n);
    }
    return d;
}

/* ------------------------------------------------------------------------- */
/* Lectura tipada                                                            */
/* ------------------------------------------------------------------------- */

static bool check_keys(struct ctx *c, const toml_table_t *tab, const char *const *allowed,
                       const char *where) {
    for (int i = 0;; i++) {
        const char *key = toml_key_in(tab, i);
        if (key == NULL) {
            return true;
        }
        bool ok = false;
        for (const char *const *a = allowed; *a != NULL; a++) {
            if (strcmp(key, *a) == 0) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            return fail(c, "%s: clave desconocida \"%s\"", where, key);
        }
    }
}

static bool get_u64(struct ctx *c, const toml_table_t *tab, const char *key, const char *where,
                    uint64_t min, uint64_t max, uint64_t def, uint64_t *out) {
    if (tab == NULL || !toml_key_exists(tab, key)) {
        *out = def;
        return true;
    }
    toml_datum_t d = toml_int_in(tab, key);
    if (!d.ok) {
        return fail(c, "%s.%s: se esperaba un entero", where, key);
    }
    if (d.u.i < 0 || (uint64_t)d.u.i < min || (uint64_t)d.u.i > max) {
        return fail(c, "%s.%s: %lld fuera de rango [%llu, %llu]", where, key, (long long)d.u.i,
                    (unsigned long long)min, (unsigned long long)max);
    }
    *out = (uint64_t)d.u.i;
    return true;
}

static bool get_u32(struct ctx *c, const toml_table_t *tab, const char *key, const char *where,
                    uint32_t min, uint32_t max, uint32_t def, uint32_t *out) {
    uint64_t v = def;
    if (!get_u64(c, tab, key, where, min, max, def, &v)) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

/* Cadena obligatoria; el llamador libera *out. */
static bool get_str(struct ctx *c, const toml_table_t *tab, const char *key, const char *where,
                    char **out) {
    if (!toml_key_exists(tab, key)) {
        return fail(c, "%s: falta la clave obligatoria \"%s\"", where, key);
    }
    toml_datum_t d = toml_string_in(tab, key);
    if (!d.ok || d.u.s == NULL) {
        return fail(c, "%s.%s: se esperaba una cadena", where, key);
    }
    *out = d.u.s;
    return true;
}

/* Array de tablas opcional ([[x]]); NULL si no existe. */
static bool get_table_array(struct ctx *c, const toml_table_t *tab, const char *key,
                            const char *where, toml_array_t **out) {
    *out = NULL;
    if (!toml_key_exists(tab, key)) {
        return true;
    }
    toml_array_t *arr = toml_array_in(tab, key);
    if (arr == NULL || (toml_array_nelem(arr) > 0 && toml_array_kind(arr) != 't')) {
        return fail(c, "%s: \"%s\" debe ser una lista de tablas ([[%s]])", where, key, key);
    }
    *out = arr;
    return true;
}

static bool get_str_buf(struct ctx *c, const toml_table_t *t, const char *key, const char *where,
                        const char *def, char *buf, size_t len);

/* ------------------------------------------------------------------------- */
/* Direcciones                                                               */
/* ------------------------------------------------------------------------- */

static bool parse_ip(const char *text, struct sockaddr_storage *ss, socklen_t *len) {
    memset(ss, 0, sizeof(*ss));
    struct sockaddr_in *v4 = (struct sockaddr_in *)ss;
    struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)ss;
    if (inet_pton(AF_INET, text, &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        *len = sizeof(*v4);
        return true;
    }
    if (inet_pton(AF_INET6, text, &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        *len = sizeof(*v6);
        return true;
    }
    return false;
}

static bool looks_like_name(const char *s) {
    for (; *s; s++) {
        if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z')) {
            if (!((*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F'))) {
                return true;
            }
        }
    }
    return false;
}

/* "IPv4:puerto" o "[IPv6]:puerto". `why` recibe el motivo del rechazo. */
static bool parse_ip_port(const char *text, struct cfg_addr *out, const char **why) {
    size_t len = strlen(text);
    if (len == 0 || len >= CONFIG_ADDR_TEXT) {
        *why = "dirección vacía o demasiado larga";
        return false;
    }
    char host[CONFIG_ADDR_TEXT];
    const char *port;
    if (text[0] == '[') {
        const char *close = strchr(text, ']');
        if (close == NULL || close[1] != ':') {
            *why = "formato esperado \"[IPv6]:puerto\"";
            return false;
        }
        size_t hl = (size_t)(close - text) - 1;
        memcpy(host, text + 1, hl);
        host[hl] = '\0';
        port = close + 2;
    } else {
        const char *colon = strrchr(text, ':');
        if (colon == NULL) {
            *why = "falta el puerto (\"IP:puerto\")";
            return false;
        }
        size_t hl = (size_t)(colon - text);
        memcpy(host, text, hl);
        host[hl] = '\0';
        if (strchr(host, ':') != NULL) {
            *why = "una IPv6 debe ir entre corchetes: \"[IPv6]:puerto\"";
            return false;
        }
        port = colon + 1;
    }
    unsigned long p = 0;
    size_t nd = 0;
    for (; port[nd] != '\0'; nd++) {
        if (port[nd] < '0' || port[nd] > '9' || nd >= 5) {
            *why = "puerto inválido";
            return false;
        }
        p = p * 10 + (unsigned long)(port[nd] - '0');
    }
    if (nd == 0 || p == 0 || p > 65535) {
        *why = "puerto fuera de rango (1-65535)";
        return false;
    }
    if (!parse_ip(host, &out->ss, &out->len)) {
        *why = looks_like_name(host) ? "solo se admiten IP literales; la resolución DNS no está "
                                       "implementada"
                                     : "IP inválida";
        return false;
    }
    if (out->ss.ss_family == AF_INET) {
        ((struct sockaddr_in *)&out->ss)->sin_port = htons((uint16_t)p);
    } else {
        ((struct sockaddr_in6 *)&out->ss)->sin6_port = htons((uint16_t)p);
    }
    memcpy(out->text, text, len + 1);
    return true;
}

static bool same_ip(const struct sockaddr_storage *a, const struct sockaddr_storage *b) {
    if (a->ss_family != b->ss_family) {
        return false;
    }
    if (a->ss_family == AF_INET) {
        return memcmp(&((const struct sockaddr_in *)a)->sin_addr,
                      &((const struct sockaddr_in *)b)->sin_addr, sizeof(struct in_addr)) == 0;
    }
    return memcmp(&((const struct sockaddr_in6 *)a)->sin6_addr,
                  &((const struct sockaddr_in6 *)b)->sin6_addr, sizeof(struct in6_addr)) == 0;
}

static bool same_addr(const struct cfg_addr *a, const struct cfg_addr *b) {
    return a->len == b->len && memcmp(&a->ss, &b->ss, a->len) == 0;
}

bool config_peer_trusted(const struct cfg_frontend *fe, const struct sockaddr_storage *peer) {
    for (size_t i = 0; i < fe->ntrusted; i++) {
        if (same_ip(&fe->trusted[i], peer)) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* Secciones                                                                 */
/* ------------------------------------------------------------------------- */

static bool load_scalars(struct ctx *c, const toml_table_t *root, struct config *cfg) {
    static const char *const server_keys[] = {"workers",
                                              "max_connections",
                                              "shutdown_timeout_ms",
                                              "ipc_timeout_ms",
                                              "restart_backoff_ms",
                                              "max_restarts",
                                              "restart_window_ms",
                                              NULL};
    static const char *const limit_keys[] = {"max_header_bytes",
                                             "max_request_line",
                                             "max_headers",
                                             "max_body_bytes",
                                             "max_requests_per_connection",
                                             NULL};
    static const char *const timeout_keys[] = {"client_header_ms",
                                               "client_idle_ms",
                                               "upstream_connect_ms",
                                               "upstream_response_ms",
                                               "io_idle_ms",
                                               "close_ms",
                                               NULL};

    const toml_table_t *server = toml_table_in(root, "server");
    const toml_table_t *limits = toml_table_in(root, "limits");
    const toml_table_t *tmo = toml_table_in(root, "timeouts");
    if ((server && !check_keys(c, server, server_keys, "[server]")) ||
        (limits && !check_keys(c, limits, limit_keys, "[limits]")) ||
        (tmo && !check_keys(c, tmo, timeout_keys, "[timeouts]"))) {
        return false;
    }

    struct cfg_limits *l = &cfg->limits;
    struct cfg_timeouts *t = &cfg->timeouts;
    if (!get_u32(c, server, "workers", "[server]", 0, CONFIG_MAX_WORKERS, 1, &cfg->workers) ||
        !get_u32(c, server, "ipc_timeout_ms", "[server]", 100, 600000, 10000,
                 &cfg->ipc_timeout_ms) ||
        !get_u32(c, server, "restart_backoff_ms", "[server]", 10, 60000, 500,
                 &cfg->restart_backoff_ms) ||
        !get_u32(c, server, "max_restarts", "[server]", 0, 1000, 5, &cfg->max_restarts) ||
        !get_u32(c, server, "restart_window_ms", "[server]", 1000, MAX_TIMEOUT_MS, 60000,
                 &cfg->restart_window_ms) ||
        !get_u32(c, server, "max_connections", "[server]", 1, 32768, 1024, &cfg->max_connections) ||
        !get_u32(c, server, "shutdown_timeout_ms", "[server]", 0, MAX_TIMEOUT_MS, 10000,
                 &cfg->shutdown_timeout_ms) ||
        !get_u32(c, limits, "max_header_bytes", "[limits]", 1024, 16384, 16384,
                 &l->max_header_bytes) ||
        !get_u32(c, limits, "max_request_line", "[limits]", 64, 16384, 8192,
                 &l->max_request_line) ||
        !get_u32(c, limits, "max_headers", "[limits]", 1, 100, 100, &l->max_headers) ||
        !get_u64(c, limits, "max_body_bytes", "[limits]", 0, (uint64_t)1 << 62, 0,
                 &l->max_body_bytes) ||
        !get_u32(c, limits, "max_requests_per_connection", "[limits]", 1, 10000000, 1000,
                 &l->max_requests_per_connection) ||
        !get_u32(c, tmo, "client_header_ms", "[timeouts]", 1, MAX_TIMEOUT_MS, 10000,
                 &t->client_header_ms) ||
        !get_u32(c, tmo, "client_idle_ms", "[timeouts]", 1, MAX_TIMEOUT_MS, 60000,
                 &t->client_idle_ms) ||
        !get_u32(c, tmo, "upstream_connect_ms", "[timeouts]", 1, MAX_TIMEOUT_MS, 3000,
                 &t->upstream_connect_ms) ||
        !get_u32(c, tmo, "upstream_response_ms", "[timeouts]", 1, MAX_TIMEOUT_MS, 30000,
                 &t->upstream_response_ms) ||
        !get_u32(c, tmo, "io_idle_ms", "[timeouts]", 1, MAX_TIMEOUT_MS, 30000, &t->io_idle_ms) ||
        !get_u32(c, tmo, "close_ms", "[timeouts]", 1, MAX_TIMEOUT_MS, 2000, &t->close_ms)) {
        return false;
    }
    if (l->max_request_line > l->max_header_bytes) {
        return fail(c, "[limits].max_request_line (%u) no puede superar max_header_bytes (%u)",
                    l->max_request_line, l->max_header_bytes);
    }
    return true;
}

static bool load_trusted(struct ctx *c, const toml_table_t *t, const char *where,
                         struct cfg_frontend *fe) {
    if (!toml_key_exists(t, "trusted_proxies")) {
        return true;
    }
    toml_array_t *tp = toml_array_in(t, "trusted_proxies");
    int nt = tp ? toml_array_nelem(tp) : -1;
    if (tp == NULL || (nt > 0 && toml_array_type(tp) != 's')) {
        return fail(c, "%s.trusted_proxies: se esperaba una lista de cadenas (IP)", where);
    }
    if (nt > CONFIG_MAX_TRUSTED) {
        return fail(c, "%s.trusted_proxies: demasiadas entradas (máximo %d)", where,
                    CONFIG_MAX_TRUSTED);
    }
    fe->trusted = calloc((size_t)nt + 1, sizeof(*fe->trusted));
    if (fe->trusted == NULL) {
        return fail(c, "sin memoria");
    }
    for (int j = 0; j < nt; j++) {
        toml_datum_t d = toml_string_at(tp, j);
        socklen_t sl;
        bool good = d.ok && d.u.s != NULL && parse_ip(d.u.s, &fe->trusted[j], &sl);
        if (!good) {
            fail_impl(c,
                      "%s.trusted_proxies[%d] \"%s\": se esperaba una IP literal (sin CIDR ni "
                      "nombres)",
                      where, j, (d.ok && d.u.s != NULL) ? d.u.s : "?");
        }
        if (d.ok) {
            free(d.u.s);
        }
        if (!good) {
            return false;
        }
        fe->ntrusted++;
    }
    return true;
}

const char *log_level_name(enum log_level l) {
    switch (l) {
    case LOG_DEBUG:
        return "DEBUG";
    case LOG_INFO:
        return "INFO";
    case LOG_WARN:
        return "WARN";
    case LOG_ERROR:
        return "ERROR";
    }
    return "?";
}

uint32_t config_effective_workers(const struct config *cfg) {
    if (cfg->workers > 0) {
        return cfg->workers;
    }
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) {
        return 1;
    }
    return n > CONFIG_MAX_WORKERS ? CONFIG_MAX_WORKERS : (uint32_t)n;
}

static bool load_log(struct ctx *c, const toml_table_t *root, struct config *cfg) {
    static const char *const keys[] = {"dir", "level", "access", NULL};
    struct cfg_log *lg = &cfg->log;
    lg->level = LOG_INFO;
    const toml_table_t *t = toml_table_in(root, "log");
    if (t == NULL) {
        return true;
    }
    char level[16];
    if (!check_keys(c, t, keys, "[log]") ||
        !get_str_buf(c, t, "dir", "[log]", "", lg->dir, sizeof(lg->dir)) ||
        !get_str_buf(c, t, "level", "[log]", "info", level, sizeof(level))) {
        return false;
    }
    static const char *const names[] = {"debug", "info", "warn", "error"};
    bool found = false;
    for (size_t i = 0; i < 4; i++) {
        if (strcmp(level, names[i]) == 0) {
            lg->level = (enum log_level)i;
            found = true;
        }
    }
    if (!found) {
        return fail(c, "[log].level \"%s\": válidos debug, info, warn, error", level);
    }
    if (lg->dir[0] != '\0' && lg->dir[0] != '/') {
        return fail(c, "[log].dir \"%s\": debe ser una ruta absoluta", lg->dir);
    }
    if (toml_key_exists(t, "access")) {
        toml_datum_t d = toml_bool_in(t, "access");
        if (!d.ok) {
            return fail(c, "[log].access: se esperaba true o false");
        }
        lg->access = d.u.b != 0;
    }
    return true;
}

static bool load_stats(struct ctx *c, const toml_table_t *root, struct config *cfg) {
    static const char *const keys[] = {"socket", "mode", "timeout_ms", "max_clients", NULL};
    struct cfg_stats *st = &cfg->stats;
    st->mode = 0600;
    st->timeout_ms = 2000;
    st->max_clients = 16;
    const toml_table_t *t = toml_table_in(root, "stats");
    if (t == NULL) {
        return true;
    }
    if (!check_keys(c, t, keys, "[stats]") ||
        !get_str_buf(c, t, "socket", "[stats]", "", st->socket, sizeof(st->socket)) ||
        !get_u32(c, t, "mode", "[stats]", 0, 0777, 0600, &st->mode) ||
        !get_u32(c, t, "timeout_ms", "[stats]", 50, 60000, 2000, &st->timeout_ms) ||
        !get_u32(c, t, "max_clients", "[stats]", 1, 256, 16, &st->max_clients)) {
        return false;
    }
    if (st->socket[0] != '\0' && st->socket[0] != '/') {
        return fail(c, "[stats].socket \"%s\": debe ser una ruta absoluta", st->socket);
    }
    if ((st->mode & 0600) != 0600) {
        return fail(c, "[stats].mode %o: el propietario necesita lectura y escritura (0600)",
                    st->mode);
    }
    return true;
}

static bool load_frontends(struct ctx *c, const toml_table_t *root, struct config *cfg) {
    static const char *const keys[] = {"listen", "trusted_proxies", NULL};
    toml_array_t *arr;
    if (!get_table_array(c, root, "frontend", "raíz", &arr)) {
        return false;
    }
    int n = arr ? toml_array_nelem(arr) : 0;
    if (n == 0) {
        return fail(c, "hace falta al menos un [[frontend]]");
    }
    if (n > CONFIG_MAX_FRONTEND) {
        return fail(c, "demasiados [[frontend]] (%d, máximo %d)", n, CONFIG_MAX_FRONTEND);
    }
    cfg->frontends = calloc((size_t)n, sizeof(*cfg->frontends));
    if (cfg->frontends == NULL) {
        return fail(c, "sin memoria");
    }
    for (int i = 0; i < n; i++) {
        char where[48];
        (void)snprintf(where, sizeof(where), "[[frontend]] #%d", i + 1);
        const toml_table_t *t = toml_table_at(arr, i);
        struct cfg_frontend *fe = &cfg->frontends[i];
        cfg->nfrontends++;
        char *listen = NULL;
        if (!check_keys(c, t, keys, where) || !get_str(c, t, "listen", where, &listen)) {
            return false;
        }
        const char *why = NULL;
        bool ok = parse_ip_port(listen, &fe->listen, &why);
        if (!ok) {
            fail_impl(c, "%s.listen \"%s\": %s", where, listen, why);
        }
        free(listen);
        if (!ok) {
            return false;
        }
        for (int j = 0; j < i; j++) {
            if (same_addr(&cfg->frontends[j].listen, &fe->listen)) {
                return fail(c, "%s.listen \"%s\": repetido (ya en [[frontend]] #%d)", where,
                            fe->listen.text, j + 1);
            }
        }
        if (!load_trusted(c, t, where, fe)) {
            return false;
        }
    }
    return true;
}

static bool valid_pool_name(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n > 64) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

static bool load_backend(struct ctx *c, const toml_table_t *t, const char *where,
                         struct cfg_backend *be) {
    static const char *const keys[] = {"address", "weight", "max_conns", NULL};
    char *address = NULL;
    if (!check_keys(c, t, keys, where) || !get_str(c, t, "address", where, &address)) {
        return false;
    }
    const char *why = NULL;
    bool ok = parse_ip_port(address, &be->addr, &why);
    if (!ok) {
        fail_impl(c, "%s.address \"%s\": %s", where, address, why);
    }
    free(address);
    return ok && get_u32(c, t, "weight", where, 1, 1000, 1, &be->weight) &&
           get_u32(c, t, "max_conns", where, 0, 100000, 0, &be->max_conns);
}

const char *lb_algorithm_name(enum lb_algorithm a) {
    switch (a) {
    case LB_ROUND_ROBIN:
        return "round_robin";
    case LB_WEIGHTED:
        return "weighted";
    case LB_LEAST_CONN:
        return "least_conn";
    }
    return "?";
}

static bool load_algorithm(struct ctx *c, const toml_table_t *t, const char *where,
                           enum lb_algorithm *out) {
    *out = LB_ROUND_ROBIN;
    if (!toml_key_exists(t, "algorithm")) {
        return true;
    }
    char *algo = NULL;
    if (!get_str(c, t, "algorithm", where, &algo)) {
        return false;
    }
    bool ok = true;
    if (strcmp(algo, "round_robin") == 0) {
        *out = LB_ROUND_ROBIN;
    } else if (strcmp(algo, "weighted") == 0) {
        *out = LB_WEIGHTED;
    } else if (strcmp(algo, "least_conn") == 0) {
        *out = LB_LEAST_CONN;
    } else {
        ok = fail(c,
                  "%s.algorithm \"%s\": desconocido (válidos: \"round_robin\", \"weighted\", "
                  "\"least_conn\")",
                  where, algo);
    }
    free(algo);
    return ok;
}

/* Copia una cadena opcional en un buffer fijo; `def` si falta. */
static bool get_str_buf(struct ctx *c, const toml_table_t *t, const char *key, const char *where,
                        const char *def, char *buf, size_t len) {
    if (!toml_key_exists(t, key)) {
        (void)snprintf(buf, len, "%s", def);
        return true;
    }
    char *v = NULL;
    if (!get_str(c, t, key, where, &v)) {
        return false;
    }
    bool ok = strlen(v) < len;
    if (ok) {
        memcpy(buf, v, strlen(v) + 1);
    } else {
        fail_impl(c, "%s.%s: demasiado largo (máximo %zu)", where, key, len - 1);
    }
    free(v);
    return ok;
}

static bool valid_health_path(const char *p) {
    if (p[0] != '/') {
        return false;
    }
    for (; *p; p++) {
        if ((unsigned char)*p <= 0x20 || (unsigned char)*p >= 0x7f) {
            return false;
        }
    }
    return true;
}

static bool load_health_http(struct ctx *c, const toml_table_t *t, const char *where,
                             struct cfg_health *h) {
    if (!get_str_buf(c, t, "path", where, "/health", h->path, sizeof(h->path)) ||
        !get_str_buf(c, t, "host", where, "", h->host, sizeof(h->host)) ||
        !get_u32(c, t, "expect_status_min", where, 100, 599, 200, &h->expect_min) ||
        !get_u32(c, t, "expect_status_max", where, 100, 599, 399, &h->expect_max)) {
        return false;
    }
    if (!valid_health_path(h->path)) {
        return fail(c,
                    "%s.path \"%s\": debe empezar por '/' y no contener espacios ni "
                    "caracteres de control",
                    where, h->path);
    }
    if (h->host[0] != '\0') {
        struct host_name hn;
        if (host_normalize(h->host, strlen(h->host), true, &hn) != HOST_OK) {
            return fail(c, "%s.host \"%s\": nombre de host inválido", where, h->host);
        }
    }
    if (h->expect_min > h->expect_max) {
        return fail(c, "%s: expect_status_min (%u) > expect_status_max (%u)", where, h->expect_min,
                    h->expect_max);
    }
    return true;
}

static bool load_health(struct ctx *c, const toml_table_t *t, const char *pool_where,
                        struct cfg_health *h) {
    static const char *const keys[] = {
        "type", "interval_ms",       "timeout_ms",        "fall", "rise", "passive_fall", "path",
        "host", "expect_status_min", "expect_status_max", NULL};
    memset(h, 0, sizeof(*h));
    if (t == NULL) {
        return true; /* sin sondas ni health pasivo */
    }
    char where[128];
    (void)snprintf(where, sizeof(where), "%s.health", pool_where);
    char *type = NULL;
    if (!check_keys(c, t, keys, where) || !get_str(c, t, "type", where, &type)) {
        return false;
    }
    bool ok = true;
    if (strcmp(type, "tcp") == 0) {
        h->type = HEALTH_TCP;
    } else if (strcmp(type, "http") == 0) {
        h->type = HEALTH_HTTP;
    } else {
        ok = fail(c, "%s.type \"%s\": válidos \"tcp\" o \"http\"", where, type);
    }
    free(type);
    if (!ok || !get_u32(c, t, "interval_ms", where, 100, MAX_TIMEOUT_MS, 2000, &h->interval_ms) ||
        !get_u32(c, t, "timeout_ms", where, 10, 60000, 1000, &h->timeout_ms) ||
        !get_u32(c, t, "fall", where, 1, 100, 3, &h->fall) ||
        !get_u32(c, t, "rise", where, 1, 100, 2, &h->rise) ||
        !get_u32(c, t, "passive_fall", where, 0, 100, 3, &h->passive_fall)) {
        return false;
    }
    if (h->timeout_ms >= h->interval_ms) {
        return fail(c, "%s: timeout_ms (%u) debe ser menor que interval_ms (%u)", where,
                    h->timeout_ms, h->interval_ms);
    }
    if (h->type == HEALTH_TCP) {
        static const char *const http_only[] = {"path", "host", "expect_status_min",
                                                "expect_status_max"};
        for (size_t i = 0; i < sizeof(http_only) / sizeof(http_only[0]); i++) {
            if (toml_key_exists(t, http_only[i])) {
                return fail(c, "%s.%s: solo aplica a type = \"http\"", where, http_only[i]);
            }
        }
        return true;
    }
    return load_health_http(c, t, where, h);
}

static bool load_pool(struct ctx *c, const toml_table_t *t, int idx, struct cfg_pool *pool,
                      const struct config *cfg) {
    static const char *const keys[] = {"name", "algorithm", "backend", "health", NULL};
    char where[96];
    (void)snprintf(where, sizeof(where), "[[pool]] #%d", idx + 1);
    if (!check_keys(c, t, keys, where) || !get_str(c, t, "name", where, &pool->name)) {
        return false;
    }
    if (!valid_pool_name(pool->name)) {
        return fail(c, "%s.name \"%s\": solo [A-Za-z0-9_-], de 1 a 64 caracteres", where,
                    pool->name);
    }
    for (size_t j = 0; j + 1 < cfg->npools; j++) {
        if (cfg->pools[j].name != NULL && strcmp(cfg->pools[j].name, pool->name) == 0) {
            return fail(c, "%s.name \"%s\": repetido", where, pool->name);
        }
    }
    (void)snprintf(where, sizeof(where), "[[pool]] \"%s\"", pool->name);
    if (!load_algorithm(c, t, where, &pool->algorithm) ||
        !load_health(c, toml_table_in(t, "health"), where, &pool->health)) {
        return false;
    }

    toml_array_t *arr;
    if (!get_table_array(c, t, "backend", where, &arr)) {
        return false;
    }
    int n = arr ? toml_array_nelem(arr) : 0;
    if (n == 0 || n > CONFIG_MAX_BACKENDS) {
        return fail(c, "%s: hacen falta entre 1 y %d [[pool.backend]] (hay %d)", where,
                    CONFIG_MAX_BACKENDS, n);
    }
    pool->backends = calloc((size_t)n, sizeof(*pool->backends));
    if (pool->backends == NULL) {
        return fail(c, "sin memoria");
    }
    for (int i = 0; i < n; i++) {
        char bwhere[128];
        (void)snprintf(bwhere, sizeof(bwhere), "%s backend #%d", where, i + 1);
        struct cfg_backend *be = &pool->backends[i];
        if (!load_backend(c, toml_table_at(arr, i), bwhere, be)) {
            return false;
        }
        pool->nbackends++;
        for (int j = 0; j < i; j++) {
            if (same_addr(&pool->backends[j].addr, &be->addr)) {
                return fail(c, "%s.address \"%s\": repetido en el pool", bwhere, be->addr.text);
            }
        }
    }
    return true;
}

static bool load_pools(struct ctx *c, const toml_table_t *root, struct config *cfg) {
    toml_array_t *arr;
    if (!get_table_array(c, root, "pool", "raíz", &arr)) {
        return false;
    }
    int n = arr ? toml_array_nelem(arr) : 0;
    if (n == 0) {
        return fail(c, "hace falta al menos un [[pool]]");
    }
    if (n > CONFIG_MAX_POOLS) {
        return fail(c, "demasiados [[pool]] (máximo %d)", CONFIG_MAX_POOLS);
    }
    cfg->pools = calloc((size_t)n, sizeof(*cfg->pools));
    if (cfg->pools == NULL) {
        return fail(c, "sin memoria");
    }
    size_t health_targets = 0;
    for (int i = 0; i < n; i++) {
        cfg->npools++;
        if (!load_pool(c, toml_table_at(arr, i), i, &cfg->pools[i], cfg)) {
            return false;
        }
        if (cfg->pools[i].health.type != HEALTH_NONE) {
            health_targets += cfg->pools[i].nbackends;
        }
    }
    if (health_targets > CONFIG_MAX_HEALTH) {
        return fail(c, "demasiados backends con health checks (%zu, máximo %d)", health_targets,
                    CONFIG_MAX_HEALTH);
    }
    return true;
}

static bool find_pool(const struct config *cfg, const char *name, uint32_t *idx) {
    for (size_t i = 0; i < cfg->npools; i++) {
        if (cfg->pools[i].name != NULL && strcmp(cfg->pools[i].name, name) == 0) {
            *idx = (uint32_t)i;
            return true;
        }
    }
    return false;
}

static bool load_routes(struct ctx *c, const toml_table_t *root, struct config *cfg) {
    static const char *const route_keys[] = {"host", "pool", NULL};
    static const char *const routing_keys[] = {"default_pool", NULL};

    const toml_table_t *routing = toml_table_in(root, "routing");
    if (routing != NULL) {
        char *def = NULL;
        if (!check_keys(c, routing, routing_keys, "[routing]") ||
            !get_str(c, routing, "default_pool", "[routing]", &def)) {
            return false;
        }
        bool found = find_pool(cfg, def, &cfg->default_pool);
        if (!found) {
            fail_impl(c, "[routing].default_pool: el pool \"%s\" no existe", def);
        }
        free(def);
        if (!found) {
            return false;
        }
        cfg->has_default = true;
    }

    toml_array_t *arr;
    if (!get_table_array(c, root, "route", "raíz", &arr)) {
        return false;
    }
    int n = arr ? toml_array_nelem(arr) : 0;
    if (n > CONFIG_MAX_ROUTES) {
        return fail(c, "demasiadas [[route]] (máximo %d)", CONFIG_MAX_ROUTES);
    }
    if (n == 0 && !cfg->has_default) {
        return fail(c, "no hay ninguna [[route]] ni [routing].default_pool");
    }
    struct route_spec *specs = calloc((size_t)n + 1, sizeof(*specs));
    char **hosts = (char **)calloc((size_t)n + 1, sizeof(char *));
    bool ok = specs != NULL && hosts != NULL;
    if (!ok) {
        fail_impl(c, "sin memoria");
    }
    for (int i = 0; ok && i < n; i++) {
        char where[48];
        (void)snprintf(where, sizeof(where), "[[route]] #%d", i + 1);
        const toml_table_t *t = toml_table_at(arr, i);
        char *pool = NULL;
        ok = check_keys(c, t, route_keys, where) && get_str(c, t, "host", where, &hosts[i]) &&
             get_str(c, t, "pool", where, &pool);
        if (ok && !find_pool(cfg, pool, &specs[i].target)) {
            ok = fail(c, "%s (host \"%s\"): el pool \"%s\" no existe", where, hosts[i], pool);
        }
        free(pool);
        specs[i].pattern = hosts[i];
    }
    if (ok) {
        struct router_error rerr = {0};
        cfg->router =
            router_build(specs, (size_t)n, cfg->has_default ? &cfg->default_pool : NULL, &rerr);
        if (cfg->router == NULL) {
            ok = errno == EINVAL ? fail(c, "[[route]] #%zu: %s", rerr.index + 1, rerr.msg)
                                 : fail(c, "sin memoria");
        }
        cfg->nroutes = (size_t)n;
    }
    for (int i = 0; hosts != NULL && i < n; i++) {
        free(hosts[i]);
    }
    free((void *)hosts);
    free(specs);
    return ok;
}

/* ------------------------------------------------------------------------- */
/* API                                                                       */
/* ------------------------------------------------------------------------- */

static void config_free(struct config *cfg) {
    for (size_t i = 0; i < cfg->nfrontends; i++) {
        free(cfg->frontends[i].trusted);
    }
    free(cfg->frontends);
    for (size_t i = 0; i < cfg->npools; i++) {
        free(cfg->pools[i].name);
        free(cfg->pools[i].backends);
    }
    free(cfg->pools);
    router_unref(cfg->router);
    free(cfg->source);
    free(cfg);
}

struct config *config_load_string(const char *text, const char *origin, char *err, size_t errlen) {
    static const char *const root_keys[] = {"server", "limits", "timeouts", "frontend", "log",
                                            "stats",  "pool",   "route",    "routing",  NULL};
    struct ctx c = {.err = err, .errlen = errlen, .origin = origin};
    err[0] = '\0';

    char *copy = dup_str(text); /* toml_parse no garantiza no modificar su entrada */
    struct config *cfg = calloc(1, sizeof(*cfg));
    if (copy == NULL || cfg == NULL) {
        free(copy);
        free(cfg);
        fail_impl(&c, "sin memoria");
        return NULL;
    }
    atomic_init(&cfg->refs, 1u);
    cfg->source = dup_str(origin);

    char terr[256];
    toml_table_t *root = toml_parse(copy, terr, (int)sizeof(terr));
    free(copy);
    if (root == NULL) {
        fail_impl(&c, "TOML inválido: %s", terr);
        config_free(cfg);
        return NULL;
    }
    bool ok = cfg->source != NULL && check_keys(&c, root, root_keys, "raíz") &&
              load_scalars(&c, root, cfg) && load_log(&c, root, cfg) && load_stats(&c, root, cfg) &&
              load_frontends(&c, root, cfg) && load_pools(&c, root, cfg) &&
              load_routes(&c, root, cfg);
    toml_free(root);
    if (!ok) {
        if (err[0] == '\0') {
            fail_impl(&c, "sin memoria");
        }
        config_free(cfg);
        return NULL;
    }
    return cfg;
}

char *config_read_text(const char *path, size_t *out_len, char *err, size_t errlen) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        char eb[128];
        (void)snprintf(err, errlen, "%s: no se puede abrir: %s", path,
                       diag_strerror(errno, eb, sizeof(eb)));
        return NULL;
    }
    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    for (;;) {
        if (len + 4096 + 1 > cap) {
            cap = cap ? cap * 2 : 8192;
            if (cap > CONFIG_MAX_FILE + 8192) {
                (void)snprintf(err, errlen, "%s: fichero demasiado grande (máximo 4 MiB)", path);
                free(buf);
                (void)fclose(f);
                return NULL;
            }
            char *nb = realloc(buf, cap);
            if (nb == NULL) {
                (void)snprintf(err, errlen, "%s: sin memoria", path);
                free(buf);
                (void)fclose(f);
                return NULL;
            }
            buf = nb;
        }
        size_t r = fread(buf + len, 1, 4096, f);
        len += r;
        if (r < 4096) {
            break;
        }
    }
    bool rerr = ferror(f) != 0;
    (void)fclose(f);
    if (rerr || len > CONFIG_MAX_FILE) {
        (void)snprintf(err, errlen, rerr ? "%s: error de lectura" : "%s: fichero demasiado grande",
                       path);
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    if (memchr(buf, '\0', len) != NULL) {
        (void)snprintf(err, errlen, "%s: contiene bytes NUL", path);
        free(buf);
        return NULL;
    }
    *out_len = len;
    return buf;
}

struct config *config_load_file(const char *path, char *err, size_t errlen) {
    size_t len = 0;
    char *buf = config_read_text(path, &len, err, errlen);
    if (buf == NULL) {
        return NULL;
    }
    struct config *cfg = config_load_string(buf, path, err, errlen);
    free(buf);
    return cfg;
}

bool config_reload_compatible(const struct config *old, const struct config *nw, char *err,
                              size_t errlen) {
    size_t used = 0;
    bool ok = true;
#define ADD(...)                                                                                   \
    do {                                                                                           \
        ok = false;                                                                                \
        if (used < errlen) {                                                                       \
            int w_ = snprintf(err + used, errlen - used, __VA_ARGS__);                             \
            used += w_ > 0 ? (size_t)w_ : 0;                                                       \
        }                                                                                          \
    } while (0)
    err[0] = '\0';
    if (old->workers != nw->workers) {
        ADD("[server].workers %u -> %u; ", old->workers, nw->workers);
    }
    if (old->max_connections != nw->max_connections) {
        ADD("[server].max_connections %u -> %u; ", old->max_connections, nw->max_connections);
    }
    if (old->nfrontends != nw->nfrontends) {
        ADD("número de [[frontend]] %zu -> %zu; ", old->nfrontends, nw->nfrontends);
    } else {
        for (size_t i = 0; i < old->nfrontends; i++) {
            if (!same_addr(&old->frontends[i].listen, &nw->frontends[i].listen)) {
                ADD("[[frontend]] #%zu listen %s -> %s; ", i + 1, old->frontends[i].listen.text,
                    nw->frontends[i].listen.text);
            }
        }
    }
    if (strcmp(old->log.dir, nw->log.dir) != 0 || old->log.level != nw->log.level ||
        old->log.access != nw->log.access) {
        ADD("[log] cambiado; ");
    }
    if (strcmp(old->stats.socket, nw->stats.socket) != 0 || old->stats.mode != nw->stats.mode ||
        old->stats.timeout_ms != nw->stats.timeout_ms ||
        old->stats.max_clients != nw->stats.max_clients) {
        ADD("[stats] cambiado; ");
    }
#undef ADD
    if (!ok && used < errlen) {
        (void)snprintf(err + used, errlen - used,
                       "no son recargables (requieren reiniciar el proceso)");
    }
    return ok;
}

struct config *config_ref(struct config *cfg) {
    atomic_fetch_add_explicit(&cfg->refs, 1u, memory_order_relaxed);
    return cfg;
}

void config_unref(struct config *cfg) {
    if (cfg == NULL) {
        return;
    }
    if (atomic_fetch_sub_explicit(&cfg->refs, 1u, memory_order_acq_rel) == 1u) {
        config_free(cfg);
    }
}
