/*
 * http_parser.h — parser incremental de la cabecera de una petición HTTP/1.x
 * y framing incremental de su cuerpo.
 *
 * Alcance: HTTP/1.0 y HTTP/1.1 en texto claro. Este módulo solo analiza; la
 * construcción de la petición hacia el upstream está en http_forward.
 *
 * Uso de la cabecera
 * ------------------
 * El llamador acumula los bytes recibidos en un buffer contiguo (un slot de
 * buffer_pool) y llama a http_parse_request con el buffer completo tras cada
 * lectura. El parser recuerda hasta dónde ha explorado, por lo que el coste
 * total es lineal aunque la cabecera llegue byte a byte. Resultados:
 *   HTTP_PARSE_INCOMPLETE  faltan bytes;
 *   HTTP_PARSE_DONE        `req` rellenado; req->head_len bytes consumidos;
 *   HTTP_PARSE_ERROR       `*err` indica el motivo; http_error_status() da el
 *                          código HTTP de respuesta. La conexión debe cerrarse
 *                          tras responder.
 * Los spans de `req` son desplazamientos dentro del buffer: siguen siendo
 * válidos si el buffer se compacta moviendo la cabecera entera.
 *
 * Tras DONE, los bytes siguientes pertenecen al cuerpo (ver http_body) y
 * después, en pipelining, a la siguiente petición: el llamador debe llamar a
 * http_parser_reset y volver a analizar desde el primer byte no consumido.
 *
 * Decisiones (ver docs/architecture.md §6):
 *   - Solo CRLF; LF o CR sueltos, obs-fold, espacio antes de ':' y
 *     caracteres de control -> 400.
 *   - Se ignoran líneas vacías (CRLF) antes de la línea de petición.
 *   - Host obligatorio en HTTP/1.1 y único siempre. En absolute-form
 *     ("http://autoridad/..."), la autoridad sustituye a Host para enrutar.
 *   - Content-Length con Transfer-Encoding -> 400; Transfer-Encoding distinto
 *     de exactamente "chunked" -> 501; Transfer-Encoding en HTTP/1.0 -> 400;
 *     Content-Length repetido solo si todos los valores coinciden.
 *   - CONNECT -> 501 (no hay túneles). Cualquier cabecera Upgrade -> 501:
 *     no hay cambio de protocolo (ni WebSocket ni h2c) y la petición no se
 *     reenvía como si fuera normal.
 *   - Connection que nombra Content-Length, Transfer-Encoding o Host -> 400:
 *     retirarlas como hop-by-hop alteraría el framing o el destino.
 *   - Expect distinto de "100-continue" -> 417.
 */
#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "host.h"

#define HTTP_DEFAULT_MAX_HEAD         ((size_t)16384) /* un slot de buffer_pool */
#define HTTP_DEFAULT_MAX_REQUEST_LINE ((size_t)8192)
#define HTTP_DEFAULT_MAX_HEADERS      ((size_t)100)
#define HTTP_MAX_HEADERS_CAP          ((size_t)100) /* tamaño del array de req */

enum http_parse_result {
    HTTP_PARSE_ERROR = -1,
    HTTP_PARSE_INCOMPLETE = 0,
    HTTP_PARSE_DONE = 1,
};

enum http_error {
    HTTP_ERR_NONE = 0,
    HTTP_ERR_BARE_LF,             /* 400 */
    HTTP_ERR_BAD_REQUEST_LINE,    /* 400 */
    HTTP_ERR_BAD_METHOD,          /* 400 */
    HTTP_ERR_BAD_TARGET,          /* 400 */
    HTTP_ERR_BAD_VERSION,         /* 400 */
    HTTP_ERR_VERSION_UNSUPPORTED, /* 505 */
    HTTP_ERR_URI_TOO_LONG,        /* 414 */
    HTTP_ERR_HEAD_TOO_LARGE,      /* 431 */
    HTTP_ERR_TOO_MANY_HEADERS,    /* 431 */
    HTTP_ERR_BAD_HEADER,          /* 400 */
    HTTP_ERR_OBS_FOLD,            /* 400 */
    HTTP_ERR_HOST_MISSING,        /* 400 */
    HTTP_ERR_HOST_DUPLICATE,      /* 400 */
    HTTP_ERR_HOST_INVALID,        /* 400 */
    HTTP_ERR_BAD_CONTENT_LENGTH,  /* 400 */
    HTTP_ERR_CL_AND_TE,           /* 400 */
    HTTP_ERR_TE_UNSUPPORTED,      /* 501 */
    HTTP_ERR_TE_HTTP10,           /* 400 */
    HTTP_ERR_CONNECT,             /* 501 */
    HTTP_ERR_EXPECT,              /* 417 */
    HTTP_ERR_BODY_TOO_LARGE,      /* 413 */
    HTTP_ERR_BAD_CHUNK,           /* 400 */
    HTTP_ERR_UPGRADE,             /* 501 */
    HTTP_ERR_CONNECTION_FRAMING,  /* 400 */
    HTTP_ERR_BAD_STATUS_LINE,     /* solo respuestas: 502 */
};

struct http_limits {
    size_t max_head;         /* cabecera completa, incluidas líneas vacías previas */
    size_t max_request_line; /* sin CRLF */
    size_t max_headers;      /* <= HTTP_MAX_HEADERS_CAP */
    uint64_t max_body;       /* 0 = sin límite */
};

struct http_span {
    uint32_t off;
    uint32_t len;
};

struct http_header {
    struct http_span name;
    struct http_span value; /* sin OWS inicial ni final */
};

enum http_target_form { HTTP_TARGET_ORIGIN, HTTP_TARGET_ABSOLUTE, HTTP_TARGET_ASTERISK };

enum http_body_kind {
    HTTP_BODY_NONE,
    HTTP_BODY_LENGTH,
    HTTP_BODY_CHUNKED,
    HTTP_BODY_UNTIL_CLOSE, /* solo respuestas: delimitado por el cierre */
};

struct http_request {
    struct http_span method;
    struct http_span target;
    struct http_span path;      /* origin-form del target; vacío = "/" */
    struct http_span authority; /* solo absolute-form */
    enum http_target_form form;
    int version_minor; /* 0 o 1 */

    struct http_header headers[HTTP_MAX_HEADERS_CAP];
    size_t nheaders;

    bool has_host;                /* hay host para enrutar */
    struct host_name host;        /* normalizado; port se conserva aparte */
    struct http_span host_header; /* valor recibido de Host (len 0 si falta) */

    enum http_body_kind body;
    uint64_t content_length;
    bool keep_alive; /* el cliente admite reutilizar la conexión */
    bool expect_continue;

    size_t head_len; /* bytes consumidos, incluidas líneas vacías previas */
};

struct http_response {
    int version_minor;
    int status;
    struct http_span reason;
    struct http_header headers[HTTP_MAX_HEADERS_CAP];
    size_t nheaders;
    enum http_body_kind body;
    uint64_t content_length; /* valor anunciado (también en HEAD/304) */
    bool has_content_length;
    bool informational;  /* 1xx, incluido 101 */
    bool upstream_close; /* el upstream no mantendrá la conexión */
    size_t head_len;
};

struct http_parser {
    struct http_limits limits;
    size_t scanned; /* bytes ya explorados en busca del final */
    size_t start;   /* inicio de la línea de petición */
    bool line_done; /* se vio el CRLF de la línea de petición */
};

/* limits NULL = valores por defecto. max_headers se acota a la capacidad. */
void http_parser_init(struct http_parser *p, const struct http_limits *limits);
void http_parser_reset(struct http_parser *p);

enum http_parse_result http_parse_request(struct http_parser *p, const char *buf, size_t len,
                                          struct http_request *req, enum http_error *err);

/*
 * Cabecera de respuesta del upstream, con el mismo parser incremental. 1xx,
 * 204, 304 y respuestas a HEAD no tienen cuerpo. Sin Content-Length ni
 * Transfer-Encoding el cuerpo termina con el cierre (HTTP_BODY_UNTIL_CLOSE).
 * Transfer-Encoding distinto de "chunked", CL+TE, Connection que nombra
 * cabeceras de framing o una línea de estado inválida son errores (el proxy
 * responde 502 si aún no ha enviado nada al cliente).
 */
enum http_parse_result http_parse_response(struct http_parser *p, const char *buf, size_t len,
                                           bool request_was_head, struct http_response *resp,
                                           enum http_error *err);

int http_error_status(enum http_error err);
const char *http_error_str(enum http_error err);

/* Búsqueda de cabecera sin distinguir mayúsculas; NULL si no existe. */
const struct http_header *http_request_find(const struct http_request *req, const char *buf,
                                            const char *name);

/* Comparación de un span con una cadena, sin distinguir mayúsculas ASCII. */
bool http_span_ieq(const char *buf, struct http_span s, const char *lit);

/*
 * Framing incremental del cuerpo. Se alimenta con los bytes posteriores a la
 * cabecera; `*consumed` indica cuántos pertenecen al cuerpo (a reenviar tal
 * cual) y el resto es la siguiente petición. No almacena datos.
 * chunked: tamaño hexadecimal de hasta 16 dígitos, extensiones de hasta
 * 1024 bytes por línea y sección de trailers de hasta 8192 bytes; los
 * trailers se reenvían sin interpretar.
 */
enum http_body_result { HTTP_BODY_ERROR = -1, HTTP_BODY_MORE = 0, HTTP_BODY_DONE = 1 };

struct http_body {
    enum http_body_kind kind;
    uint64_t remaining; /* LENGTH: bytes que faltan; CHUNKED: del chunk actual */
    uint64_t total;     /* bytes de datos acumulados (para max_body) */
    uint64_t max_body;
    int state;
    unsigned digits;
    size_t line_len;
    size_t trailer_len;
};

void http_body_init(struct http_body *b, const struct http_request *req, uint64_t max_body);
void http_body_init_kind(struct http_body *b, enum http_body_kind kind, uint64_t content_length,
                         uint64_t max_body);
/* Cierre del emisor: DONE si el cuerpo estaba completo o termina por cierre;
 * ERROR si quedó truncado. */
enum http_body_result http_body_eof(const struct http_body *b);
enum http_body_result http_body_feed(struct http_body *b, const char *data, size_t len,
                                     size_t *consumed, enum http_error *err);

#endif /* HTTP_PARSER_H */
