/*
 * host.c — ver host.h.
 */
#include "host.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>

#define LABEL_MAX 63

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static char to_lower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c + ('a' - 'A'));
    }
    return c;
}

static bool is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || is_digit(c) || c == '-' || c == '_' || c == '.';
}

static enum host_status parse_port(const char *p, size_t len, uint16_t *port) {
    if (len == 0 || len > 5) {
        return HOST_BAD_PORT;
    }
    unsigned long v = 0;
    for (size_t i = 0; i < len; i++) {
        if (!is_digit(p[i])) {
            return HOST_BAD_PORT;
        }
        v = v * 10 + (unsigned long)(p[i] - '0');
    }
    if (v == 0 || v > 65535) {
        return HOST_BAD_PORT;
    }
    *port = (uint16_t)v;
    return HOST_OK;
}

static enum host_status normalize_ipv6(const char *in, size_t len, struct host_name *out) {
    /* in apunta a lo que hay entre corchetes. */
    char tmp[INET6_ADDRSTRLEN];
    if (len == 0 || len >= sizeof(tmp)) {
        return HOST_BAD_IPV6;
    }
    memcpy(tmp, in, len);
    tmp[len] = '\0';
    struct in6_addr addr;
    if (inet_pton(AF_INET6, tmp, &addr) != 1) {
        return HOST_BAD_IPV6;
    }
    char canon[INET6_ADDRSTRLEN];
    if (inet_ntop(AF_INET6, &addr, canon, sizeof(canon)) == NULL) {
        return HOST_BAD_IPV6;
    }
    size_t clen = strlen(canon);
    out->name[0] = '[';
    memcpy(out->name + 1, canon, clen);
    out->name[clen + 1] = ']';
    out->name[clen + 2] = '\0';
    out->len = clen + 2;
    out->is_ipv6 = true;
    return HOST_OK;
}

static enum host_status normalize_name(const char *in, size_t len, struct host_name *out) {
    if (len > 0 && in[len - 1] == '.') {
        len--; /* un único punto final (FQDN absoluto) */
    }
    if (len == 0) {
        return HOST_EMPTY;
    }
    if (len > HOST_MAX_LEN) {
        return HOST_TOO_LONG;
    }
    size_t label_len = 0;
    for (size_t i = 0; i < len; i++) {
        char c = to_lower(in[i]);
        if (!is_name_char(c)) {
            return HOST_BAD_CHAR;
        }
        if (c == '.') {
            if (label_len == 0 || out->name[i - 1] == '-') {
                return HOST_BAD_LABEL;
            }
            label_len = 0;
        } else {
            if (label_len == 0 && c == '-') {
                return HOST_BAD_LABEL;
            }
            if (++label_len > LABEL_MAX) {
                return HOST_BAD_LABEL;
            }
        }
        out->name[i] = c;
    }
    if (label_len == 0 || out->name[len - 1] == '-') {
        return HOST_BAD_LABEL;
    }
    out->name[len] = '\0';
    out->len = len;
    return HOST_OK;
}

enum host_status host_normalize(const char *in, size_t len, bool allow_port,
                                struct host_name *out) {
    memset(out, 0, sizeof(*out));
    if (len == 0) {
        return HOST_EMPTY;
    }
    const char *port = NULL;
    size_t port_len = 0;
    enum host_status st;

    if (in[0] == '[') {
        const char *close = memchr(in, ']', len);
        if (close == NULL) {
            return HOST_BAD_IPV6;
        }
        size_t inner = (size_t)(close - in) - 1;
        size_t rest = len - inner - 2;
        if (rest > 0) {
            if (close[1] != ':') {
                return HOST_BAD_IPV6;
            }
            port = close + 2;
            port_len = rest - 1;
        }
        st = normalize_ipv6(in + 1, inner, out);
    } else {
        const char *colon = memchr(in, ':', len);
        size_t name_len = len;
        if (colon != NULL) {
            name_len = (size_t)(colon - in);
            port = colon + 1;
            port_len = len - name_len - 1;
        }
        st = normalize_name(in, name_len, out);
    }
    if (st != HOST_OK) {
        return st;
    }
    if (port != NULL) {
        if (!allow_port) {
            return HOST_BAD_PORT;
        }
        st = parse_port(port, port_len, &out->port);
        if (st != HOST_OK) {
            return st;
        }
        out->has_port = true;
    }
    return HOST_OK;
}

const char *host_status_str(enum host_status st) {
    switch (st) {
    case HOST_OK:
        return "ok";
    case HOST_EMPTY:
        return "host vacío";
    case HOST_TOO_LONG:
        return "host demasiado largo";
    case HOST_BAD_CHAR:
        return "carácter no permitido en host";
    case HOST_BAD_LABEL:
        return "etiqueta de host inválida";
    case HOST_BAD_IPV6:
        return "literal IPv6 inválido";
    case HOST_BAD_PORT:
        return "puerto inválido o no permitido";
    }
    return "error desconocido";
}
