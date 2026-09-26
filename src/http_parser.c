/*
 * http_parser.c — ver http_parser.h.
 *
 * La cabecera se analiza en dos fases:
 *   1. exploración incremental (http_parse_request en cada llamada): busca el
 *      final "\r\n\r\n" desde donde lo dejó, rechaza LF sueltos y aplica los
 *      límites de tamaño en cuanto se superan, sin esperar al final;
 *   2. análisis completo de la cabecera ya delimitada (parse_head), una sola
 *      vez.
 * Tras la fase 1 todo '\n' de la cabecera va precedido de '\r'; un '\r'
 * suelto queda dentro de una línea y se rechaza como carácter de control.
 */
#include "http_parser.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Clases de caracteres (RFC 9110 §5.6.2)                                    */
/* ------------------------------------------------------------------------- */

static bool is_tchar(unsigned char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return true;
    }
    return c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static bool is_ows(unsigned char c) {
    return c == ' ' || c == '\t';
}

/* field-vchar / SP / HTAB, incluido obs-text (0x80-0xFF). */
static bool is_field_char(unsigned char c) {
    return c == '\t' || (c >= 0x20 && c != 0x7f);
}

static unsigned char lower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c - 'A' + 'a') : c;
}

static bool mem_ieq(const char *a, size_t alen, const char *lit) {
    size_t n = strlen(lit);
    if (alen != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (lower((unsigned char)a[i]) != lower((unsigned char)lit[i])) {
            return false;
        }
    }
    return true;
}

bool http_span_ieq(const char *buf, struct http_span s, const char *lit) {
    return mem_ieq(buf + s.off, s.len, lit);
}

static struct http_span span(size_t off, size_t len) {
    return (struct http_span){.off = (uint32_t)off, .len = (uint32_t)len};
}

/* ------------------------------------------------------------------------- */
/* Errores                                                                   */
/* ------------------------------------------------------------------------- */

int http_error_status(enum http_error err) {
    switch (err) {
    case HTTP_ERR_NONE:
        return 200;
    case HTTP_ERR_VERSION_UNSUPPORTED:
        return 505;
    case HTTP_ERR_URI_TOO_LONG:
        return 414;
    case HTTP_ERR_HEAD_TOO_LARGE:
    case HTTP_ERR_TOO_MANY_HEADERS:
        return 431;
    case HTTP_ERR_TE_UNSUPPORTED:
    case HTTP_ERR_CONNECT:
    case HTTP_ERR_UPGRADE:
        return 501;
    case HTTP_ERR_BAD_STATUS_LINE:
        return 502;
    case HTTP_ERR_EXPECT:
        return 417;
    case HTTP_ERR_BODY_TOO_LARGE:
        return 413;
    case HTTP_ERR_BARE_LF:
    case HTTP_ERR_BAD_REQUEST_LINE:
    case HTTP_ERR_BAD_METHOD:
    case HTTP_ERR_BAD_TARGET:
    case HTTP_ERR_BAD_VERSION:
    case HTTP_ERR_BAD_HEADER:
    case HTTP_ERR_OBS_FOLD:
    case HTTP_ERR_HOST_MISSING:
    case HTTP_ERR_HOST_DUPLICATE:
    case HTTP_ERR_HOST_INVALID:
    case HTTP_ERR_BAD_CONTENT_LENGTH:
    case HTTP_ERR_CL_AND_TE:
    case HTTP_ERR_TE_HTTP10:
    case HTTP_ERR_BAD_CHUNK:
    case HTTP_ERR_CONNECTION_FRAMING:
        return 400;
    }
    return 400;
}

const char *http_error_str(enum http_error err) {
    switch (err) {
    case HTTP_ERR_NONE:
        return "sin error";
    case HTTP_ERR_BARE_LF:
        return "fin de línea sin CR";
    case HTTP_ERR_BAD_REQUEST_LINE:
        return "línea de petición mal formada";
    case HTTP_ERR_BAD_METHOD:
        return "método inválido";
    case HTTP_ERR_BAD_TARGET:
        return "request-target inválido";
    case HTTP_ERR_BAD_VERSION:
        return "versión HTTP mal formada";
    case HTTP_ERR_VERSION_UNSUPPORTED:
        return "versión HTTP no soportada";
    case HTTP_ERR_URI_TOO_LONG:
        return "línea de petición demasiado larga";
    case HTTP_ERR_HEAD_TOO_LARGE:
        return "cabecera demasiado grande";
    case HTTP_ERR_TOO_MANY_HEADERS:
        return "demasiadas cabeceras";
    case HTTP_ERR_BAD_HEADER:
        return "cabecera mal formada";
    case HTTP_ERR_OBS_FOLD:
        return "cabecera plegada (obs-fold)";
    case HTTP_ERR_HOST_MISSING:
        return "falta Host";
    case HTTP_ERR_HOST_DUPLICATE:
        return "Host duplicado";
    case HTTP_ERR_HOST_INVALID:
        return "Host inválido";
    case HTTP_ERR_BAD_CONTENT_LENGTH:
        return "Content-Length inválido o contradictorio";
    case HTTP_ERR_CL_AND_TE:
        return "Content-Length y Transfer-Encoding a la vez";
    case HTTP_ERR_TE_UNSUPPORTED:
        return "Transfer-Encoding no soportado";
    case HTTP_ERR_TE_HTTP10:
        return "Transfer-Encoding en HTTP/1.0";
    case HTTP_ERR_CONNECT:
        return "CONNECT no soportado";
    case HTTP_ERR_EXPECT:
        return "Expect no soportado";
    case HTTP_ERR_BODY_TOO_LARGE:
        return "cuerpo demasiado grande";
    case HTTP_ERR_BAD_CHUNK:
        return "codificación chunked inválida";
    case HTTP_ERR_UPGRADE:
        return "Upgrade no soportado (sin WebSocket ni h2c)";
    case HTTP_ERR_CONNECTION_FRAMING:
        return "Connection nombra Content-Length, Transfer-Encoding o Host";
    case HTTP_ERR_BAD_STATUS_LINE:
        return "línea de estado inválida";
    }
    return "error desconocido";
}

/* ------------------------------------------------------------------------- */
/* Inicialización                                                            */
/* ------------------------------------------------------------------------- */

void http_parser_init(struct http_parser *p, const struct http_limits *limits) {
    memset(p, 0, sizeof(*p));
    if (limits != NULL) {
        p->limits = *limits;
    } else {
        p->limits = (struct http_limits){
            .max_head = HTTP_DEFAULT_MAX_HEAD,
            .max_request_line = HTTP_DEFAULT_MAX_REQUEST_LINE,
            .max_headers = HTTP_DEFAULT_MAX_HEADERS,
            .max_body = 0,
        };
    }
    if (p->limits.max_headers > HTTP_MAX_HEADERS_CAP) {
        p->limits.max_headers = HTTP_MAX_HEADERS_CAP;
    }
    /* Los spans son de 32 bits. */
    if (p->limits.max_head > UINT32_MAX) {
        p->limits.max_head = UINT32_MAX;
    }
}

void http_parser_reset(struct http_parser *p) {
    p->scanned = 0;
    p->start = 0;
    p->line_done = false;
}

/* ------------------------------------------------------------------------- */
/* Fase 2: análisis de la cabecera completa                                  */
/* ------------------------------------------------------------------------- */

struct head_state {
    size_t host_count;
    size_t te_count;
    bool te_chunked;
    bool has_cl;
    uint64_t cl;
    bool conn_close;
    bool conn_keep_alive;
    bool conn_framing; /* Connection nombra Content-Length, Transfer-Encoding o Host */
    bool has_upgrade;
};

/* Posición del '\r' del siguiente CRLF a partir de `pos` (existe siempre). */
static size_t line_end(const char *buf, size_t pos, size_t end) {
    const char *nl = memchr(buf + pos, '\n', end - pos);
    return (size_t)(nl - buf) - 1;
}

static enum http_error parse_request_line(const char *buf, size_t pos, size_t eol,
                                          struct http_request *req) {
    size_t i = pos;
    while (i < eol && is_tchar((unsigned char)buf[i])) {
        i++;
    }
    if (i == pos) {
        return HTTP_ERR_BAD_METHOD;
    }
    if (i == eol || buf[i] != ' ') {
        return i == eol ? HTTP_ERR_BAD_REQUEST_LINE : HTTP_ERR_BAD_METHOD;
    }
    req->method = span(pos, i - pos);

    size_t t = ++i;
    while (i < eol && (unsigned char)buf[i] > 0x20 && (unsigned char)buf[i] < 0x7f) {
        i++;
    }
    if (i == t) {
        return buf[i] == ' ' ? HTTP_ERR_BAD_REQUEST_LINE : HTTP_ERR_BAD_TARGET;
    }
    if (i == eol || buf[i] != ' ') {
        return HTTP_ERR_BAD_TARGET;
    }
    req->target = span(t, i - t);

    size_t v = ++i;
    if (eol - v != 8 || memcmp(buf + v, "HTTP/", 5) != 0 || buf[v + 5] < '0' || buf[v + 5] > '9' ||
        buf[v + 6] != '.' || buf[v + 7] < '0' || buf[v + 7] > '9') {
        return HTTP_ERR_BAD_VERSION;
    }
    if (buf[v + 5] != '1' || (buf[v + 7] != '0' && buf[v + 7] != '1')) {
        return HTTP_ERR_VERSION_UNSUPPORTED;
    }
    req->version_minor = buf[v + 7] - '0';
    return HTTP_ERR_NONE;
}

static enum http_error parse_target(const char *buf, struct http_request *req) {
    const char *t = buf + req->target.off;
    size_t len = req->target.len;

    if (http_span_ieq(buf, req->method, "CONNECT")) {
        return HTTP_ERR_CONNECT;
    }
    if (t[0] == '/') {
        req->form = HTTP_TARGET_ORIGIN;
        req->path = req->target;
        return HTTP_ERR_NONE;
    }
    if (len == 1 && t[0] == '*') {
        if (!http_span_ieq(buf, req->method, "OPTIONS")) {
            return HTTP_ERR_BAD_TARGET;
        }
        req->form = HTTP_TARGET_ASTERISK;
        return HTTP_ERR_NONE;
    }
    static const char scheme[] = "http://";
    size_t slen = sizeof(scheme) - 1;
    if (len > slen && mem_ieq(t, slen, scheme)) {
        size_t a = slen;
        size_t e = a;
        while (e < len && t[e] != '/' && t[e] != '?' && t[e] != '#') {
            e++;
        }
        if (e == a || memchr(t + a, '@', e - a) != NULL) {
            return HTTP_ERR_BAD_TARGET;
        }
        req->form = HTTP_TARGET_ABSOLUTE;
        req->authority = span(req->target.off + a, e - a);
        req->path = span(req->target.off + e, len - e);
        if (host_normalize(t + a, e - a, true, &req->host) != HOST_OK) {
            return HTTP_ERR_HOST_INVALID;
        }
        return HTTP_ERR_NONE;
    }
    return HTTP_ERR_BAD_TARGET;
}

/* Recorre una lista separada por comas y llama a fn con cada elemento sin OWS. */
typedef bool (*list_fn)(const char *item, size_t len, void *ctx);

static bool for_each_item(const char *v, size_t len, list_fn fn, void *ctx) {
    size_t i = 0;
    while (i <= len) {
        size_t s = i;
        while (i < len && v[i] != ',') {
            i++;
        }
        size_t e = i;
        while (s < e && is_ows((unsigned char)v[s])) {
            s++;
        }
        while (e > s && is_ows((unsigned char)v[e - 1])) {
            e--;
        }
        if (!fn(v + s, e - s, ctx)) {
            return false;
        }
        i++; /* salta la coma (o sale si i == len) */
    }
    return true;
}

static bool cl_item(const char *item, size_t len, void *vctx) {
    struct head_state *st = vctx;
    if (len == 0 || len > 20) {
        return false;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (item[i] < '0' || item[i] > '9') {
            return false;
        }
        uint64_t d = (uint64_t)(item[i] - '0');
        if (v > (UINT64_MAX - d) / 10) {
            return false;
        }
        v = v * 10 + d;
    }
    if (st->has_cl && v != st->cl) {
        return false;
    }
    st->has_cl = true;
    st->cl = v;
    return true;
}

static bool conn_item(const char *item, size_t len, void *vctx) {
    struct head_state *st = vctx;
    if (mem_ieq(item, len, "close")) {
        st->conn_close = true;
    } else if (mem_ieq(item, len, "keep-alive")) {
        st->conn_keep_alive = true;
    } else if (mem_ieq(item, len, "content-length") || mem_ieq(item, len, "transfer-encoding") ||
               mem_ieq(item, len, "host")) {
        /* Retirarlas como hop-by-hop cambiaría el framing o el destino. */
        st->conn_framing = true;
    }
    return true;
}

/* Cabeceras comunes a petición y respuesta: framing y Connection. */
static enum http_error common_header(const char *buf, const struct http_header *h,
                                     struct head_state *st) {
    const char *v = buf + h->value.off;
    size_t vlen = h->value.len;
    if (http_span_ieq(buf, h->name, "content-length")) {
        if (!for_each_item(v, vlen, cl_item, st)) {
            return HTTP_ERR_BAD_CONTENT_LENGTH;
        }
    } else if (http_span_ieq(buf, h->name, "transfer-encoding")) {
        st->te_count++;
        st->te_chunked = mem_ieq(v, vlen, "chunked");
    } else if (http_span_ieq(buf, h->name, "connection")) {
        for_each_item(v, vlen, conn_item, st);
    } else if (http_span_ieq(buf, h->name, "upgrade")) {
        st->has_upgrade = true;
    }
    return HTTP_ERR_NONE;
}

static enum http_error request_header(const char *buf, const struct http_header *h,
                                      struct http_request *req, struct head_state *st) {
    const char *v = buf + h->value.off;
    size_t vlen = h->value.len;

    if (http_span_ieq(buf, h->name, "host")) {
        if (++st->host_count > 1) {
            return HTTP_ERR_HOST_DUPLICATE;
        }
        req->host_header = h->value;
        if (req->form != HTTP_TARGET_ABSOLUTE &&
            host_normalize(v, vlen, true, &req->host) != HOST_OK) {
            return HTTP_ERR_HOST_INVALID;
        }
    } else if (http_span_ieq(buf, h->name, "expect")) {
        if (!mem_ieq(v, vlen, "100-continue")) {
            return HTTP_ERR_EXPECT;
        }
        req->expect_continue = true;
    }
    return common_header(buf, h, st);
}

static enum http_error parse_header_line(const char *buf, size_t pos, size_t eol,
                                         struct http_header *h) {
    if (is_ows((unsigned char)buf[pos])) {
        return HTTP_ERR_OBS_FOLD;
    }
    size_t i = pos;
    while (i < eol && is_tchar((unsigned char)buf[i])) {
        i++;
    }
    if (i == pos || i == eol || buf[i] != ':') {
        return HTTP_ERR_BAD_HEADER; /* nombre vacío, espacio antes de ':'... */
    }
    h->name = span(pos, i - pos);
    size_t s = i + 1;
    size_t e = eol;
    for (size_t k = s; k < e; k++) {
        if (!is_field_char((unsigned char)buf[k])) {
            return HTTP_ERR_BAD_HEADER;
        }
    }
    while (s < e && is_ows((unsigned char)buf[s])) {
        s++;
    }
    while (e > s && is_ows((unsigned char)buf[e - 1])) {
        e--;
    }
    h->value = span(s, e - s);
    return HTTP_ERR_NONE;
}

static enum http_error parse_head(const struct http_parser *p, const char *buf, size_t head_len,
                                  struct http_request *req) {
    memset(req, 0, sizeof(*req));
    req->head_len = head_len;

    size_t pos = p->start;
    size_t eol = line_end(buf, pos, head_len);
    enum http_error err = parse_request_line(buf, pos, eol, req);
    if (err == HTTP_ERR_NONE) {
        err = parse_target(buf, req);
    }
    if (err != HTTP_ERR_NONE) {
        return err;
    }

    struct head_state st = {0};
    for (pos = eol + 2;; pos = eol + 2) {
        eol = line_end(buf, pos, head_len);
        if (eol == pos) {
            break; /* línea vacía: fin de la cabecera */
        }
        if (req->nheaders >= p->limits.max_headers) {
            return HTTP_ERR_TOO_MANY_HEADERS;
        }
        struct http_header *h = &req->headers[req->nheaders];
        err = parse_header_line(buf, pos, eol, h);
        if (err == HTTP_ERR_NONE) {
            err = request_header(buf, h, req, &st);
        }
        if (err != HTTP_ERR_NONE) {
            return err;
        }
        req->nheaders++;
    }

    if (st.host_count == 0 && req->version_minor == 1) {
        return HTTP_ERR_HOST_MISSING;
    }
    req->has_host = req->form == HTTP_TARGET_ABSOLUTE || st.host_count == 1;
    if (st.conn_framing) {
        return HTTP_ERR_CONNECTION_FRAMING;
    }
    if (st.has_upgrade) {
        return HTTP_ERR_UPGRADE; /* sin cambio de protocolo: ni WebSocket ni h2c */
    }

    /* Framing: primero lo ambiguo (400), después lo no soportado (501). */
    if (st.te_count > 0) {
        if (req->version_minor == 0) {
            return HTTP_ERR_TE_HTTP10;
        }
        if (st.has_cl) {
            return HTTP_ERR_CL_AND_TE;
        }
        if (st.te_count > 1 || !st.te_chunked) {
            return HTTP_ERR_TE_UNSUPPORTED;
        }
        req->body = HTTP_BODY_CHUNKED;
    } else if (st.has_cl && st.cl > 0) {
        req->content_length = st.cl;
        if (p->limits.max_body != 0 && req->content_length > p->limits.max_body) {
            return HTTP_ERR_BODY_TOO_LARGE;
        }
        req->body = HTTP_BODY_LENGTH;
    } else {
        req->body = HTTP_BODY_NONE;
    }

    if (req->version_minor == 1) {
        req->keep_alive = !st.conn_close;
    } else {
        req->keep_alive = st.conn_keep_alive && !st.conn_close;
    }
    return HTTP_ERR_NONE;
}

/* ------------------------------------------------------------------------- */
/* Fase 1: exploración incremental                                           */
/* ------------------------------------------------------------------------- */

/*
 * Busca el final de la cabecera desde donde se dejó. DONE deja *head_len;
 * ERROR deja *err. `line_err` es el error de una primera línea excesiva
 * (414 en peticiones).
 */
static enum http_parse_result scan_head(struct http_parser *p, const char *buf, size_t len,
                                        enum http_error line_err, size_t *head_len,
                                        enum http_error *err) {
    const struct http_limits *lim = &p->limits;
    size_t i = p->scanned;
    *err = HTTP_ERR_NONE;

    for (; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];

        /* Antes que nada: el byte que completa la cabecera también cuenta. */
        if (i + 1 > lim->max_head) {
            *err = HTTP_ERR_HEAD_TOO_LARGE;
            return HTTP_PARSE_ERROR;
        }

        if (!p->line_done && i == p->start) {
            /* Líneas vacías antes de la primera línea. */
            if (c == '\n') {
                *err = HTTP_ERR_BARE_LF;
                return HTTP_PARSE_ERROR;
            }
            if (c == '\r') {
                if (i + 1 >= len) {
                    break; /* falta el byte siguiente */
                }
                if (buf[i + 1] != '\n') {
                    *err = HTTP_ERR_BAD_REQUEST_LINE;
                    return HTTP_PARSE_ERROR;
                }
                i++;
                p->start = i + 1;
                continue;
            }
        }

        if (c == '\n') {
            if (buf[i - 1] != '\r') {
                *err = HTTP_ERR_BARE_LF;
                return HTTP_PARSE_ERROR;
            }
            if (!p->line_done) {
                p->line_done = true;
            } else if (i >= p->start + 3 && buf[i - 2] == '\n') {
                /* "\r\n\r\n": buf[i-3] es '\r' porque buf[i-2] pasó la fase 1. */
                *head_len = i + 1;
                p->scanned = i + 1;
                return HTTP_PARSE_DONE;
            }
        } else if (!p->line_done && c != '\r' && i - p->start >= lim->max_request_line) {
            *err = line_err;
            return HTTP_PARSE_ERROR;
        }
    }
    p->scanned = i;
    return HTTP_PARSE_INCOMPLETE;
}

enum http_parse_result http_parse_request(struct http_parser *p, const char *buf, size_t len,
                                          struct http_request *req, enum http_error *err) {
    size_t head_len = 0;
    enum http_parse_result r = scan_head(p, buf, len, HTTP_ERR_URI_TOO_LONG, &head_len, err);
    if (r != HTTP_PARSE_DONE) {
        return r;
    }
    *err = parse_head(p, buf, head_len, req);
    return *err == HTTP_ERR_NONE ? HTTP_PARSE_DONE : HTTP_PARSE_ERROR;
}

/* ------------------------------------------------------------------------- */
/* Respuesta del upstream                                                    */
/* ------------------------------------------------------------------------- */

static enum http_error parse_status_line(const char *buf, size_t pos, size_t eol,
                                         struct http_response *resp) {
    const char *l = buf + pos;
    size_t len = eol - pos;
    /* "HTTP/1.x SP 3DIGIT [SP reason]" */
    if (len < 12 || memcmp(l, "HTTP/1.", 7) != 0 || (l[7] != '0' && l[7] != '1') || l[8] != ' ') {
        return HTTP_ERR_BAD_STATUS_LINE;
    }
    int status = 0;
    for (size_t i = 9; i < 12; i++) {
        if (l[i] < '0' || l[i] > '9') {
            return HTTP_ERR_BAD_STATUS_LINE;
        }
        status = status * 10 + (l[i] - '0');
    }
    if (status < 100 || status > 599 || (len > 12 && l[12] != ' ')) {
        return HTTP_ERR_BAD_STATUS_LINE;
    }
    for (size_t i = 13; i < len; i++) {
        if (!is_field_char((unsigned char)l[i])) {
            return HTTP_ERR_BAD_STATUS_LINE;
        }
    }
    resp->version_minor = l[7] - '0';
    resp->status = status;
    resp->reason = len > 13 ? span(pos + 13, len - 13) : span(pos + len, 0);
    return HTTP_ERR_NONE;
}

static enum http_error response_framing(const struct http_parser *p, const struct head_state *st,
                                        bool request_was_head, struct http_response *resp) {
    if (st->conn_framing) {
        return HTTP_ERR_CONNECTION_FRAMING;
    }
    /* Validación del framing aunque luego no haya cuerpo (HEAD, 204, 304). */
    if (st->te_count > 0) {
        if (resp->version_minor == 0) {
            return HTTP_ERR_TE_HTTP10;
        }
        if (st->has_cl) {
            return HTTP_ERR_CL_AND_TE;
        }
        if (st->te_count > 1 || !st->te_chunked) {
            return HTTP_ERR_TE_UNSUPPORTED;
        }
    }
    resp->informational = resp->status < 200;
    if (resp->informational || resp->status == 204 || resp->status == 304 || request_was_head) {
        resp->body = HTTP_BODY_NONE; /* RFC 9112 §6.3: sin cuerpo */
        resp->content_length = st->has_cl ? st->cl : 0;
    } else if (st->te_count > 0) {
        resp->body = HTTP_BODY_CHUNKED;
    } else if (st->has_cl) {
        resp->content_length = st->cl;
        resp->body = st->cl > 0 ? HTTP_BODY_LENGTH : HTTP_BODY_NONE;
        if (p->limits.max_body != 0 && st->cl > p->limits.max_body) {
            return HTTP_ERR_BODY_TOO_LARGE;
        }
    } else {
        resp->body = HTTP_BODY_UNTIL_CLOSE;
    }
    resp->has_content_length = st->has_cl;
    resp->upstream_close = st->conn_close || (resp->version_minor == 0 && !st->conn_keep_alive);
    return HTTP_ERR_NONE;
}

static enum http_error parse_response_head(const struct http_parser *p, const char *buf,
                                           size_t head_len, bool request_was_head,
                                           struct http_response *resp) {
    memset(resp, 0, sizeof(*resp));
    resp->head_len = head_len;
    size_t pos = p->start;
    size_t eol = line_end(buf, pos, head_len);
    enum http_error err = parse_status_line(buf, pos, eol, resp);
    if (err != HTTP_ERR_NONE) {
        return err;
    }
    struct head_state st = {0};
    for (pos = eol + 2;; pos = eol + 2) {
        eol = line_end(buf, pos, head_len);
        if (eol == pos) {
            break;
        }
        if (resp->nheaders >= p->limits.max_headers) {
            return HTTP_ERR_TOO_MANY_HEADERS;
        }
        struct http_header *h = &resp->headers[resp->nheaders];
        err = parse_header_line(buf, pos, eol, h);
        if (err == HTTP_ERR_NONE) {
            err = common_header(buf, h, &st);
        }
        if (err != HTTP_ERR_NONE) {
            return err;
        }
        resp->nheaders++;
    }
    return response_framing(p, &st, request_was_head, resp);
}

enum http_parse_result http_parse_response(struct http_parser *p, const char *buf, size_t len,
                                           bool request_was_head, struct http_response *resp,
                                           enum http_error *err) {
    size_t head_len = 0;
    enum http_parse_result r = scan_head(p, buf, len, HTTP_ERR_BAD_STATUS_LINE, &head_len, err);
    if (r != HTTP_PARSE_DONE) {
        return r;
    }
    *err = parse_response_head(p, buf, head_len, request_was_head, resp);
    return *err == HTTP_ERR_NONE ? HTTP_PARSE_DONE : HTTP_PARSE_ERROR;
}

const struct http_header *http_request_find(const struct http_request *req, const char *buf,
                                            const char *name) {
    for (size_t i = 0; i < req->nheaders; i++) {
        if (http_span_ieq(buf, req->headers[i].name, name)) {
            return &req->headers[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Framing del cuerpo                                                        */
/* ------------------------------------------------------------------------- */

#define CHUNK_MAX_DIGITS   16u
#define CHUNK_MAX_LINE     1024u
#define CHUNK_MAX_TRAILERS 8192u

enum chunk_state {
    CS_SIZE,          /* primer dígito hexadecimal */
    CS_SIZE_MORE,     /* más dígitos, ';', OWS o CR */
    CS_SIZE_WS,       /* OWS tras el tamaño: solo OWS, ';' o CR */
    CS_EXT,           /* extensión hasta CR */
    CS_SIZE_LF,       /* LF de la línea de tamaño */
    CS_DATA,          /* datos del chunk */
    CS_DATA_CR,       /* CR tras los datos */
    CS_DATA_LF,       /* LF tras los datos */
    CS_TRAILER_START, /* inicio de trailer o CR final */
    CS_TRAILER_LINE,  /* línea de trailer hasta CR */
    CS_TRAILER_LF,    /* LF de la línea de trailer */
    CS_FINAL_LF,      /* LF final */
    CS_DONE,
};

/* Estado tras un delimitador de la línea de tamaño; -1 si no es válido. */
static int after_size(unsigned char c) {
    if (c == ';') {
        return CS_EXT;
    }
    if (c == '\r') {
        return CS_SIZE_LF;
    }
    return is_ows(c) ? CS_SIZE_WS : -1;
}

static int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = lower(c);
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

void http_body_init_kind(struct http_body *b, enum http_body_kind kind, uint64_t content_length,
                         uint64_t max_body) {
    memset(b, 0, sizeof(*b));
    b->kind = kind;
    b->max_body = max_body;
    b->state = CS_SIZE;
    if (b->kind == HTTP_BODY_LENGTH) {
        b->remaining = content_length;
    }
}

void http_body_init(struct http_body *b, const struct http_request *req, uint64_t max_body) {
    http_body_init_kind(b, req->body, req->content_length, max_body);
}

enum http_body_result http_body_eof(const struct http_body *b) {
    switch (b->kind) {
    case HTTP_BODY_NONE:
    case HTTP_BODY_UNTIL_CLOSE:
        return HTTP_BODY_DONE;
    case HTTP_BODY_LENGTH:
        return b->remaining == 0 ? HTTP_BODY_DONE : HTTP_BODY_ERROR;
    case HTTP_BODY_CHUNKED:
        return b->state == CS_DONE ? HTTP_BODY_DONE : HTTP_BODY_ERROR;
    }
    return HTTP_BODY_ERROR;
}

/* Byte de la línea de tamaño: dígitos, OWS, extensión y su CR. */
static enum http_error chunk_size_byte(struct http_body *b, unsigned char c) {
    int next = -1;
    if (b->state == CS_SIZE || b->state == CS_SIZE_MORE) {
        int hv = hex_value(c);
        if (hv >= 0) {
            if (++b->digits > CHUNK_MAX_DIGITS) {
                return HTTP_ERR_BAD_CHUNK;
            }
            b->remaining = (b->remaining << 4) | (uint64_t)hv;
            b->state = CS_SIZE_MORE;
            return HTTP_ERR_NONE;
        }
        /* Hace falta al menos un dígito antes de un delimitador. */
        next = b->state == CS_SIZE ? -1 : after_size(c);
    } else if (b->state == CS_SIZE_WS) {
        next = after_size(c); /* tras OWS ya no puede haber dígitos */
    } else if (b->state == CS_EXT) {
        if (c == '\r') {
            next = CS_SIZE_LF;
        } else if (is_field_char(c) && ++b->line_len <= CHUNK_MAX_LINE) {
            next = CS_EXT;
        }
    }
    if (next < 0) {
        return HTTP_ERR_BAD_CHUNK;
    }
    b->state = next;
    return HTTP_ERR_NONE;
}

/* LF de la línea de tamaño: fija el chunk y comprueba max_body. */
static enum http_error chunk_size_end(struct http_body *b, unsigned char c) {
    if (c != '\n') {
        return HTTP_ERR_BAD_CHUNK;
    }
    b->digits = 0;
    b->line_len = 0;
    if (b->remaining == 0) {
        b->state = CS_TRAILER_START;
        return HTTP_ERR_NONE;
    }
    if (b->max_body != 0 && b->remaining > b->max_body - b->total) {
        return HTTP_ERR_BODY_TOO_LARGE;
    }
    b->total += b->remaining;
    b->state = CS_DATA;
    return HTTP_ERR_NONE;
}

/* Byte de CRLF tras los datos, de trailers o del CRLF final. */
static enum http_error chunk_tail_byte(struct http_body *b, unsigned char c) {
    int st = b->state;
    if (st == CS_DATA_CR || st == CS_DATA_LF) {
        b->state = st == CS_DATA_CR ? CS_DATA_LF : CS_SIZE;
        return c == (st == CS_DATA_CR ? '\r' : '\n') ? HTTP_ERR_NONE : HTTP_ERR_BAD_CHUNK;
    }
    if (st == CS_TRAILER_START || st == CS_TRAILER_LINE) {
        if (c == '\r') {
            b->state = st == CS_TRAILER_START ? CS_FINAL_LF : CS_TRAILER_LF;
            return HTTP_ERR_NONE;
        }
        b->state = CS_TRAILER_LINE;
        return is_field_char(c) && ++b->trailer_len <= CHUNK_MAX_TRAILERS ? HTTP_ERR_NONE
                                                                          : HTTP_ERR_BAD_CHUNK;
    }
    if (st == CS_TRAILER_LF || st == CS_FINAL_LF) {
        b->state = st == CS_TRAILER_LF ? CS_TRAILER_START : CS_DONE;
        return c == '\n' ? HTTP_ERR_NONE : HTTP_ERR_BAD_CHUNK;
    }
    return HTTP_ERR_BAD_CHUNK;
}

static enum http_body_result feed_chunked(struct http_body *b, const char *data, size_t len,
                                          size_t *consumed, enum http_error *err) {
    size_t i = 0;
    while (i < len && b->state != CS_DONE) {
        unsigned char c = (unsigned char)data[i];
        enum http_error e;
        switch ((enum chunk_state)b->state) {
        case CS_DATA: {
            /* Los datos se cuentan en bloque, sin mirar su contenido. */
            size_t avail = len - i;
            size_t take = b->remaining < avail ? (size_t)b->remaining : avail;
            i += take;
            b->remaining -= take;
            if (b->remaining == 0) {
                b->state = CS_DATA_CR;
            }
            continue;
        }
        case CS_SIZE:
        case CS_SIZE_MORE:
        case CS_SIZE_WS:
        case CS_EXT:
            e = chunk_size_byte(b, c);
            break;
        case CS_SIZE_LF:
            e = chunk_size_end(b, c);
            break;
        case CS_DATA_CR:
        case CS_DATA_LF:
        case CS_TRAILER_START:
        case CS_TRAILER_LINE:
        case CS_TRAILER_LF:
        case CS_FINAL_LF:
        case CS_DONE:
            e = chunk_tail_byte(b, c);
            break;
        }
        if (e != HTTP_ERR_NONE) {
            *err = e;
            *consumed = i;
            return HTTP_BODY_ERROR;
        }
        i++;
    }
    *consumed = i;
    return b->state == CS_DONE ? HTTP_BODY_DONE : HTTP_BODY_MORE;
}

enum http_body_result http_body_feed(struct http_body *b, const char *data, size_t len,
                                     size_t *consumed, enum http_error *err) {
    *consumed = 0;
    *err = HTTP_ERR_NONE;
    switch (b->kind) {
    case HTTP_BODY_NONE:
        return HTTP_BODY_DONE;
    case HTTP_BODY_LENGTH: {
        size_t take = b->remaining < len ? (size_t)b->remaining : len;
        b->remaining -= take;
        *consumed = take;
        return b->remaining == 0 ? HTTP_BODY_DONE : HTTP_BODY_MORE;
    }
    case HTTP_BODY_CHUNKED:
        return feed_chunked(b, data, len, consumed, err);
    case HTTP_BODY_UNTIL_CLOSE:
        /* Todo pertenece al cuerpo; termina con el cierre (http_body_eof). */
        if (b->max_body != 0 && len > b->max_body - b->total) {
            *err = HTTP_ERR_BODY_TOO_LARGE;
            return HTTP_BODY_ERROR;
        }
        b->total += len;
        *consumed = len;
        return HTTP_BODY_MORE;
    }
    return HTTP_BODY_ERROR;
}
