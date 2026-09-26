/*
 * http_forward.c — ver http_forward.h.
 */
#include "http_forward.h"

#include <errno.h>
#include <string.h>

struct out_buf {
    char *p;
    size_t cap;
    size_t len;
    bool overflow;
};

static void put(struct out_buf *o, const char *s, size_t n) {
    if (o->overflow || n > o->cap - o->len) {
        o->overflow = true;
        return;
    }
    memcpy(o->p + o->len, s, n);
    o->len += n;
}

static void put_str(struct out_buf *o, const char *s) {
    put(o, s, strlen(s));
}

static void put_span(struct out_buf *o, const char *buf, struct http_span s) {
    put(o, buf + s.off, s.len);
}

static bool valid_ip_text(const char *ip) {
    if (ip == NULL || ip[0] == '\0') {
        return false;
    }
    for (const char *c = ip; *c; c++) {
        bool ok = (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f') ||
                  (*c >= 'A' && *c <= 'F') || *c == ':' || *c == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

static unsigned char lower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c - 'A' + 'a') : c;
}

static bool mem_ieq2(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (lower((unsigned char)a[i]) != lower((unsigned char)b[i])) {
            return false;
        }
    }
    return true;
}

/* ¿Aparece `name` como token en alguna cabecera Connection? */
static bool named_in_connection(const char *buf, const struct http_header *hdrs, size_t n,
                                struct http_span name) {
    for (size_t i = 0; i < n; i++) {
        const struct http_header *h = &hdrs[i];
        if (!http_span_ieq(buf, h->name, "connection")) {
            continue;
        }
        const char *v = buf + h->value.off;
        size_t len = h->value.len;
        size_t k = 0;
        while (k < len) {
            while (k < len && (v[k] == ' ' || v[k] == '\t' || v[k] == ',')) {
                k++;
            }
            size_t s = k;
            while (k < len && v[k] != ',' && v[k] != ' ' && v[k] != '\t') {
                k++;
            }
            if (k - s == name.len && mem_ieq2(v + s, buf + name.off, name.len)) {
                return true;
            }
        }
    }
    return false;
}

static bool is_hop_by_hop(const char *buf, struct http_span name) {
    static const char *const hop[] = {"connection", "keep-alive", "proxy-connection", "te",
                                      "upgrade"};
    for (size_t i = 0; i < sizeof(hop) / sizeof(hop[0]); i++) {
        if (http_span_ieq(buf, name, hop[i])) {
            return true;
        }
    }
    return false;
}

static void put_header(struct out_buf *o, const char *name, const char *value) {
    put_str(o, name);
    put_str(o, ": ");
    put_str(o, value);
    put_str(o, "\r\n");
}

int http_forward_build(const char *buf, const struct http_request *req,
                       const struct http_forward_params *params, char *out, size_t cap,
                       size_t *out_len) {
    *out_len = 0;
    if (params == NULL || !valid_ip_text(params->client_ip)) {
        errno = EINVAL;
        return -1;
    }
    struct out_buf o = {.p = out, .cap = cap};
    bool trusted = params->peer_trusted;

    /* Línea de petición. */
    put_span(&o, buf, req->method);
    put_str(&o, " ");
    if (req->form == HTTP_TARGET_ABSOLUTE) {
        if (req->path.len == 0 || buf[req->path.off] != '/') {
            put_str(&o, "/");
        }
        put_span(&o, buf, req->path);
    } else {
        put_span(&o, buf, req->target);
    }
    put_str(&o, req->version_minor == 1 ? " HTTP/1.1\r\n" : " HTTP/1.0\r\n");

    /* Host primero. */
    struct http_span host = req->form == HTTP_TARGET_ABSOLUTE ? req->authority : req->host_header;
    if (host.len > 0) {
        put_str(&o, "Host: ");
        put_span(&o, buf, host);
        put_str(&o, "\r\n");
    }

    /* Pasada 1: cabeceras de extremo a extremo, filtradas. */
    bool kept_real_ip = false;
    bool kept_proto = false;
    for (size_t i = 0; i < req->nheaders; i++) {
        const struct http_header *h = &req->headers[i];
        struct http_span n = h->name;
        /* Expect: el 100-continue lo responde el propio proxy (§6.2). */
        if (http_span_ieq(buf, n, "host") || http_span_ieq(buf, n, "forwarded") ||
            http_span_ieq(buf, n, "x-forwarded-for") || http_span_ieq(buf, n, "expect") ||
            is_hop_by_hop(buf, n) || named_in_connection(buf, req->headers, req->nheaders, n)) {
            continue;
        }
        if (http_span_ieq(buf, n, "x-real-ip")) {
            if (!trusted || kept_real_ip) {
                continue;
            }
            kept_real_ip = true;
        } else if (http_span_ieq(buf, n, "x-forwarded-proto")) {
            if (!trusted || kept_proto) {
                continue;
            }
            kept_proto = true;
        }
        put_span(&o, buf, n);
        put_str(&o, ": ");
        put_span(&o, buf, h->value);
        put_str(&o, "\r\n");
    }

    /* Pasada 2: X-Forwarded-For combinado (solo se conserva si es confiable). */
    put_str(&o, "X-Forwarded-For: ");
    if (trusted) {
        for (size_t i = 0; i < req->nheaders; i++) {
            const struct http_header *h = &req->headers[i];
            if (http_span_ieq(buf, h->name, "x-forwarded-for") && h->value.len > 0 &&
                !named_in_connection(buf, req->headers, req->nheaders, h->name)) {
                put_span(&o, buf, h->value);
                put_str(&o, ", ");
            }
        }
    }
    put_str(&o, params->client_ip);
    put_str(&o, "\r\n");
    if (!kept_real_ip) {
        put_header(&o, "X-Real-IP", params->client_ip);
    }
    if (!kept_proto) {
        put_header(&o, "X-Forwarded-Proto", "http");
    }
    put_str(&o, "Connection: close\r\n\r\n");

    if (o.overflow) {
        errno = ENOBUFS;
        return -1;
    }
    *out_len = o.len;
    return 0;
}

static void put_uint(struct out_buf *o, unsigned v) {
    char tmp[12];
    size_t n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v > 0);
    while (n > 0) {
        put(o, &tmp[--n], 1);
    }
}

int http_forward_response(const char *buf, const struct http_response *resp, bool keep_alive,
                          int client_minor, char *out, size_t cap, size_t *out_len) {
    *out_len = 0;
    struct out_buf o = {.p = out, .cap = cap};

    /* El proxy habla HTTP/1.1; el motivo se conserva. */
    put_str(&o, "HTTP/1.1 ");
    put_uint(&o, (unsigned)resp->status);
    put_str(&o, " ");
    put_span(&o, buf, resp->reason);
    put_str(&o, "\r\n");

    for (size_t i = 0; i < resp->nheaders; i++) {
        const struct http_header *h = &resp->headers[i];
        if (is_hop_by_hop(buf, h->name) ||
            named_in_connection(buf, resp->headers, resp->nheaders, h->name)) {
            continue;
        }
        put_span(&o, buf, h->name);
        put_str(&o, ": ");
        put_span(&o, buf, h->value);
        put_str(&o, "\r\n");
    }
    if (!keep_alive) {
        put_str(&o, "Connection: close\r\n");
    } else if (client_minor == 0) {
        put_str(&o, "Connection: keep-alive\r\n"); /* HTTP/1.0 lo exige explícito */
    }
    put_str(&o, "\r\n");

    if (o.overflow) {
        errno = ENOBUFS;
        return -1;
    }
    *out_len = o.len;
    return 0;
}
