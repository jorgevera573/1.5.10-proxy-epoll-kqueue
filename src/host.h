/*
 * host.h — validación y normalización de nombres de host para el enrutado.
 *
 * Lo usan http_parser (cabecera Host y absolute-form) y router (patrones de
 * configuración), de modo que ambos comparan la misma forma canónica.
 *
 * Forma canónica:
 *   - nombre registrado o IPv4: minúsculas, sin punto final. Caracteres
 *     permitidos [a-z0-9-_.]; etiquetas no vacías, de 1..63 bytes, que no
 *     empiezan ni terminan por '-'; longitud total <= 253.
 *   - IPv6: "[...]" validado con inet_pton y reescrito con inet_ntop
 *     ("[0:0::1]" -> "[::1]"). Sin zone id.
 *   - puerto (opcional, solo si se permite): 1..65535, se separa del nombre.
 * No se aceptan '%'-escapes, userinfo ni '*'.
 */
#ifndef HOST_H
#define HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HOST_MAX_LEN 253
/* "[" + INET6_ADDRSTRLEN - 1 + "]" cabe en HOST_MAX_LEN. */

enum host_status {
    HOST_OK = 0,
    HOST_EMPTY,
    HOST_TOO_LONG,
    HOST_BAD_CHAR,
    HOST_BAD_LABEL,
    HOST_BAD_IPV6,
    HOST_BAD_PORT,
};

struct host_name {
    char name[HOST_MAX_LEN + 1]; /* canónico, terminado en NUL */
    size_t len;
    uint16_t port; /* 0 si no hay puerto */
    bool has_port;
    bool is_ipv6;
};

enum host_status host_normalize(const char *in, size_t len, bool allow_port, struct host_name *out);

const char *host_status_str(enum host_status st);

#endif /* HOST_H */
