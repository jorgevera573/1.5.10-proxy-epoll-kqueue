/*
 * config.h — configuración TOML (tomlc99) validada y convertida en una
 * instantánea inmutable.
 *
 * Esquema (docs/configuration.md tiene la referencia completa):
 *
 *   [server]    workers (0 = automático), max_connections, shutdown_timeout_ms,
 *               ipc_timeout_ms, restart_backoff_ms, max_restarts,
 *               restart_window_ms
 *   [log]       dir, level, access
 *   [stats]     socket, mode, timeout_ms, max_clients
 *   [limits]    max_header_bytes, max_request_line, max_headers,
 *               max_body_bytes, max_requests_per_connection
 *   [timeouts]  client_header_ms, client_idle_ms, upstream_connect_ms,
 *               upstream_response_ms, io_idle_ms, close_ms
 *   [[frontend]] listen = "IP:puerto", trusted_proxies = ["IP", ...]
 *   [[pool]]    name, algorithm = "round_robin" | "weighted" | "least_conn",
 *               [[pool.backend]] address = "IP:puerto", weight, max_conns
 *               [pool.health] type = "tcp" | "http", interval_ms, timeout_ms,
 *                             fall, rise, passive_fall, path, host,
 *                             expect_status_min, expect_status_max
 *   [[route]]   host = "dominio" | "*.dominio", pool
 *   [routing]   default_pool
 *
 * Restricciones de esta versión (se rechazan con diagnóstico):
 *   - direcciones solo como IP literal (sin DNS);
 *   - max_connections, max_conns y least_conn son POR WORKER (cada worker
 *     tiene su pool de buffers y su registro de backends).
 * Las claves desconocidas son un error, para detectar erratas.
 *
 * Propiedad: config_load_* devuelve una instantánea con una referencia. Es
 * inmutable; el estado que cambia en ejecución (conexiones activas, cursor de
 * round robin) vive en backend_pool. config_ref/config_unref usan un contador
 * atómico, de modo que una recarga futura podrá publicar otra instantánea
 * mientras las conexiones en curso conservan la suya.
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <netinet/in.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "router.h"

#define CONFIG_ERR_LEN      512
#define CONFIG_ADDR_TEXT    64
#define CONFIG_MAX_FRONTEND 16
#define CONFIG_MAX_POOLS    256
#define CONFIG_MAX_BACKENDS 64 /* por pool */
#define CONFIG_MAX_ROUTES   4096
#define CONFIG_MAX_TRUSTED  64
#define CONFIG_MAX_HEALTH   1024 /* backends con sondas activas en total */
#define CONFIG_HEALTH_PATH  256
#define CONFIG_MAX_WORKERS  64
#define CONFIG_MAX_FILE     ((size_t)4 << 20)
#define CONFIG_PATH_LEN     256
#define CONFIG_SOCK_LEN     104 /* sun_path: 104 en macOS, 108 en Linux */

enum lb_algorithm { LB_ROUND_ROBIN = 0, LB_WEIGHTED, LB_LEAST_CONN };

const char *lb_algorithm_name(enum lb_algorithm a);

enum health_type { HEALTH_NONE = 0, HEALTH_TCP, HEALTH_HTTP };

/*
 * Health checks de un pool. Sin [pool.health] (type = HEALTH_NONE) no hay
 * sondas ni exclusión pasiva: los backends siempre son elegibles (salvo por
 * max_conns), porque nada podría devolver a servicio uno excluido.
 */
struct cfg_health {
    enum health_type type;
    uint32_t interval_ms;          /* entre inicios de sonda */
    uint32_t timeout_ms;           /* < interval_ms */
    uint32_t fall;                 /* fallos activos consecutivos para excluir */
    uint32_t rise;                 /* éxitos activos consecutivos para readmitir */
    uint32_t passive_fall;         /* fallos reales de conexión consecutivos; 0 = sin pasivo */
    char path[CONFIG_HEALTH_PATH]; /* HTTP: ruta de GET */
    char host[CONFIG_HEALTH_PATH]; /* HTTP: cabecera Host; vacío = dirección */
    uint32_t expect_min;           /* HTTP: estado saludable en [min, max] */
    uint32_t expect_max;
};

struct cfg_addr {
    struct sockaddr_storage ss;
    socklen_t len;
    char text[CONFIG_ADDR_TEXT]; /* tal como se escribió, p. ej. "127.0.0.1:3000" */
};

struct cfg_backend {
    struct cfg_addr addr;
    uint32_t weight;    /* 1..1000; reservado para "weighted" */
    uint32_t max_conns; /* 0 = sin límite */
};

struct cfg_pool {
    char *name;
    enum lb_algorithm algorithm;
    struct cfg_backend *backends;
    size_t nbackends;
    struct cfg_health health;
};

struct cfg_frontend {
    struct cfg_addr listen;
    /* IPs de pares cuyas cabeceras X-Forwarded-* se aceptan (§6.3). */
    struct sockaddr_storage *trusted;
    size_t ntrusted;
};

struct cfg_timeouts {
    uint32_t client_header_ms;
    uint32_t client_idle_ms;
    uint32_t upstream_connect_ms;
    uint32_t upstream_response_ms;
    uint32_t io_idle_ms;
    uint32_t close_ms;
};

enum log_level { LOG_DEBUG = 0, LOG_INFO, LOG_WARN, LOG_ERROR };

struct cfg_log {
    char dir[CONFIG_PATH_LEN]; /* vacío = stderr; si no, <dir>/worker-<i>.log */
    enum log_level level;
    bool access; /* una línea por petición (sin ruta ni cabeceras) */
};

struct cfg_stats {
    char socket[CONFIG_SOCK_LEN]; /* vacío = sin socket de estadísticas */
    uint32_t mode;                /* permisos del socket, p. ej. 0600 */
    uint32_t timeout_ms;          /* plazo total por cliente */
    uint32_t max_clients;         /* clientes simultáneos */
};

struct cfg_limits {
    uint32_t max_header_bytes;
    uint32_t max_request_line;
    uint32_t max_headers;
    uint64_t max_body_bytes; /* 0 = sin límite */
    uint32_t max_requests_per_connection;
};

struct config {
    atomic_uint refs;
    char *source;             /* ruta u origen, para diagnósticos */
    uint32_t workers;         /* configurado; 0 = automático (config_effective_workers) */
    uint32_t max_connections; /* por worker */
    uint32_t shutdown_timeout_ms;
    uint32_t ipc_timeout_ms;     /* plazos de arranque, preparación y activación */
    uint32_t restart_backoff_ms; /* espera inicial antes de reponer un worker */
    uint32_t max_restarts;       /* reposiciones por worker dentro de la ventana */
    uint32_t restart_window_ms;
    struct cfg_log log;
    struct cfg_stats stats;
    struct cfg_timeouts timeouts;
    struct cfg_limits limits;
    struct cfg_frontend *frontends;
    size_t nfrontends;
    struct cfg_pool *pools;
    size_t npools;
    size_t nroutes;
    bool has_default;
    uint32_t default_pool;
    router *router; /* target = índice en pools */
};

/*
 * Carga y valida. Devuelve la instantánea o NULL con un diagnóstico legible
 * en err ("origen: sección.clave: motivo").
 */
struct config *config_load_file(const char *path, char *err, size_t errlen);
struct config *config_load_string(const char *text, const char *origin, char *err, size_t errlen);

/*
 * Lee el fichero completo (<= 4 MiB, sin NUL) terminado en '\0'. El maestro
 * lo usa para enviar los MISMOS bytes a todos los workers. NULL con err.
 */
char *config_read_text(const char *path, size_t *out_len, char *err, size_t errlen);

struct config *config_ref(struct config *cfg);
void config_unref(struct config *cfg); /* acepta NULL */

/*
 * ¿Puede `nw` sustituir a `old` en caliente? No son recargables: los
 * frontends (direcciones y puertos, en el mismo orden), workers,
 * max_connections (dimensiona el pool de buffers), [log] y [stats]. Si no es
 * compatible, devuelve false y describe todas las diferencias en err.
 */
bool config_reload_compatible(const struct config *old, const struct config *nw, char *err,
                              size_t errlen);

/* Número de workers efectivo: `workers`, o las CPUs en línea (1..64) si es 0. */
uint32_t config_effective_workers(const struct config *cfg);

const char *log_level_name(enum log_level l);

/* ¿Es `ip` (familia y dirección, sin puerto) uno de los pares confiables? */
bool config_peer_trusted(const struct cfg_frontend *fe, const struct sockaddr_storage *peer);

#endif /* CONFIG_H */
