/*
 * conn.c — ver conn.h y docs/architecture.md §5.
 *
 * Modelo de ejecución
 * -------------------
 * Todo evento (E/S del cliente, E/S del upstream, temporizador) termina en
 * client_run(c), que repite `step` mientras haya progreso. Cada paso intenta
 * leer, procesar y escribir en ambos sentidos. Con edge-triggered se guardan
 * las marcas `readable`/`writable`: se ponen al llegar un evento y solo se
 * quitan al obtener EAGAIN. Así, si una lectura se detuvo porque el buffer
 * estaba lleno (backpressure), se reanuda en cuanto hay hueco sin esperar a
 * un evento nuevo, que no llegaría.
 *
 * Cada run tiene un presupuesto de pasos; si se agota, un temporizador de 0 ms
 * reanuda la conexión en la siguiente vuelta del bucle, para no acaparar el
 * worker (y, de nuevo, sin depender de un evento nuevo).
 *
 * Memoria acotada: 2 slots de 16 KB por cliente + 2 por upstream. Cuando un
 * buffer se llena se deja de leer del origen.
 */
#include "conn.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "http_forward.h"
#include "http_parser.h"
#include "listener.h"
#include "log.h"

#define SLOT       BUFFER_POOL_SLOT_SIZE
#define RUN_BUDGET 64

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0 /* macOS: SO_NOSIGPIPE por socket */
#endif

static const char CONTINUE_100[] = "HTTP/1.1 100 Continue\r\n\r\n";

enum phase {
    PH_HEAD,     /* esperando/analizando la cabecera de una petición */
    PH_EXCHANGE, /* petición en curso con el upstream */
    PH_CLOSING,  /* vaciando la salida, shutdown(SHUT_WR) y descarte hasta EOF */
    PH_CLOSED,   /* se libera al final de client_run */
};

/* Datos válidos en [off, len). */
struct buf {
    char *p;
    size_t off;
    size_t len;
};

struct upstream_conn {
    int fd;
    bool connected;
    bool readable;
    bool writable;
    bool eof;
    bool write_broken; /* el upstream dejó de aceptar datos */
    struct buf out;    /* proxy -> upstream: cabecera reescrita + cuerpo */
    struct buf in;     /* upstream -> proxy: respuesta sin procesar */
    struct bp_choice choice;
};

struct client_conn {
    struct worker *w;
    struct client_conn *prev;
    struct client_conn *next;
    backend_pools *gen; /* generación de la petición en curso (referencia propia) */
    size_t fe_idx;      /* frontend: índice estable entre generaciones */
    struct sockaddr_storage peer;
    int fd;
    char peer_ip[INET6_ADDRSTRLEN];
    bool peer_trusted;

    enum phase phase;
    bool readable;
    bool writable;
    bool eof;
    bool shut_wr;
    struct buf in;  /* cliente -> proxy */
    struct buf out; /* proxy -> cliente */

    timer deadline;
    timer resume;
    bool activity;
    uint64_t head_started_ms; /* primer byte de la petición actual (0 = ninguno) */
    uint64_t closing_started_ms;
    uint32_t requests;

    /* Petición en curso. */
    char method[16]; /* para el log de acceso (copias: el buffer se reutiliza) */
    char host[HOST_MAX_LEN + 1];
    uint64_t req_start_ms;
    struct http_parser rp;
    struct http_request req;
    bool is_head;
    int client_minor;
    struct http_body req_body;
    bool req_done;
    bool keep_alive;
    bool expect_100;
    int pending_error; /* código HTTP pendiente de escribir; 0 = ninguno */
    bool response_started;

    /* Respuesta. */
    struct upstream_conn *up;
    struct http_parser sp;
    struct http_response resp;
    bool resp_head_done;
    struct http_body resp_body;
    bool resp_done;
};

/* ------------------------------------------------------------------------- */
/* Buffers                                                                   */
/* ------------------------------------------------------------------------- */

static size_t buf_avail(const struct buf *b) {
    return b->len - b->off;
}

/* Espacio libre al final, compactando si hace falta. */
static size_t buf_space(struct buf *b) {
    if (b->len == SLOT && b->off > 0) {
        memmove(b->p, b->p + b->off, b->len - b->off);
        b->len -= b->off;
        b->off = 0;
    }
    return SLOT - b->len;
}

static void buf_consume(struct buf *b, size_t n) {
    b->off += n;
    if (b->off == b->len) {
        b->off = 0;
        b->len = 0;
    }
}

static bool buf_acquire(buffer_pool *pool, struct buf *b) {
    b->p = buffer_pool_acquire(pool);
    b->off = 0;
    b->len = 0;
    return b->p != NULL;
}

static void buf_release(buffer_pool *pool, struct buf *b) {
    if (b->p != NULL) {
        (void)buffer_pool_release(pool, b->p);
        b->p = NULL;
    }
}

/* ------------------------------------------------------------------------- */
/* Generación y health pasivo                                                */
/* ------------------------------------------------------------------------- */

static const struct config *cfg_of(const struct client_conn *c) {
    return backend_pools_config(c->gen);
}

static struct http_limits request_limits(const struct config *cfg) {
    return (struct http_limits){.max_head = cfg->limits.max_header_bytes,
                                .max_request_line = cfg->limits.max_request_line,
                                .max_headers = cfg->limits.max_headers,
                                .max_body = cfg->limits.max_body_bytes};
}

/*
 * Antes del primer byte de cada petición la conexión adopta la generación
 * vigente: una recarga afecta a las peticiones nuevas, también a las
 * siguientes de una conexión keep-alive, y nunca a la que está en curso.
 */
static void adopt_current_generation(struct client_conn *c) {
    struct worker *w = c->w;
    if (c->gen == w->gen) {
        return;
    }
    backend_pools_unref(c->gen); /* sin upstream en PH_HEAD: nada más lo usa */
    c->gen = backend_pools_ref(w->gen);
    const struct config *cfg = cfg_of(c);
    c->peer_trusted = config_peer_trusted(&cfg->frontends[c->fe_idx], &c->peer);
    struct http_limits lim = request_limits(cfg);
    http_parser_init(&c->rp, &lim);
}

/*
 * ¿Es el error de conexión atribuible al backend? Solo esos cuentan para el
 * health pasivo; los locales (sin fds, sin puertos efímeros, sin memoria) no.
 */
static bool backend_fault(int err) {
    switch (err) {
    case ECONNREFUSED:
    case ETIMEDOUT:
    case EHOSTUNREACH:
    case ENETUNREACH:
    case ECONNRESET:
    case ECONNABORTED:
    case ENETDOWN:
#ifdef EHOSTDOWN
    case EHOSTDOWN:
#endif
        return true;
    default:
        return false;
    }
}

static void passive_failure(struct client_conn *c, const struct bp_choice *ch, int err) {
    if (!backend_fault(err)) {
        return;
    }
    enum bp_transition tr = backend_pool_connect_failed(c->gen, ch);
    worker_log_transition(c->w, c->gen, ch->pool, ch->backend, tr, "pasivo: fallo de conexión");
}

/* ------------------------------------------------------------------------- */
/* Respuestas de error del proxy                                             */
/* ------------------------------------------------------------------------- */

static const char *reason_phrase(int status) {
    switch (status) {
    case 400:
        return "Bad Request";
    case 408:
        return "Request Timeout";
    case 413:
        return "Content Too Large";
    case 414:
        return "URI Too Long";
    case 417:
        return "Expectation Failed";
    case 431:
        return "Request Header Fields Too Large";
    case 501:
        return "Not Implemented";
    case 502:
        return "Bad Gateway";
    case 503:
        return "Service Unavailable";
    case 504:
        return "Gateway Timeout";
    case 505:
        return "HTTP Version Not Supported";
    default:
        return "Error";
    }
}

/* Escribe la respuesta de error en `out`; false si no cabe todavía. */
static bool write_error(struct client_conn *c, int status) {
    char body[64];
    int blen = snprintf(body, sizeof(body), "%d %s\n", status, reason_phrase(status));
    char head[256];
    int hlen = snprintf(head, sizeof(head),
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Type: text/plain; charset=utf-8\r\n"
                        "Content-Length: %d\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        status, reason_phrase(status), blen);
    if (blen < 0 || hlen < 0) {
        return true; /* imposible con estos formatos; no bloquear */
    }
    size_t need = (size_t)hlen + (c->is_head ? 0 : (size_t)blen);
    if (buf_space(&c->out) < need) {
        return false;
    }
    memcpy(c->out.p + c->out.len, head, (size_t)hlen);
    c->out.len += (size_t)hlen;
    if (!c->is_head) {
        memcpy(c->out.p + c->out.len, body, (size_t)blen);
        c->out.len += (size_t)blen;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Upstream                                                                  */
/* ------------------------------------------------------------------------- */

static void on_upstream_io(io_loop *loop, int fd, uint32_t events, void *ud);

/*
 * Libera el upstream y la selección de backend. `reusable` indica si la
 * conexión habría podido volver a un pool (respuesta con framing propio,
 * ambos cuerpos completos y sin "Connection: close" del upstream). Punto de
 * extensión: hoy se cierra siempre y solo se contabiliza.
 */
static void upstream_release(struct client_conn *c, bool reusable) {
    struct upstream_conn *up = c->up;
    if (up == NULL) {
        return;
    }
    struct worker *w = c->w;
    if (reusable) {
        w->stats.upstream_reusable++;
    }
    if (up->fd >= 0) {
        (void)io_loop_del(w->loop, up->fd);
        close(up->fd);
    }
    buf_release(w->bufs, &up->out);
    buf_release(w->bufs, &up->in);
    backend_pool_release(c->gen, &up->choice);
    free(up);
    c->up = NULL;
    w->nupstreams--;
}

/* Abre el socket y lanza el connect no bloqueante. 0, o el código HTTP. */
static int upstream_open(struct client_conn *c, const struct bp_choice *ch) {
    struct worker *w = c->w;
    const struct cfg_backend *be = &cfg_of(c)->pools[ch->pool].backends[ch->backend];
    struct upstream_conn *up = calloc(1, sizeof(*up));
    if (up == NULL) {
        backend_pool_release(c->gen, ch);
        return 503;
    }
    up->fd = -1;
    up->choice = *ch;
    c->up = up;
    w->nupstreams++;
    /* Desde aquí, upstream_release deshace todo (slots, backend, fd). */
    if (!buf_acquire(w->bufs, &up->out) || !buf_acquire(w->bufs, &up->in)) {
        return 503; /* sin slots: tratado como falta de capacidad */
    }

    struct http_forward_params prm = {.client_ip = c->peer_ip, .peer_trusted = c->peer_trusted};
    size_t n = 0;
    if (http_forward_build(c->in.p + c->in.off, &c->req, &prm, up->out.p, SLOT, &n) < 0) {
        return errno == ENOBUFS ? 431 : 500;
    }
    up->out.len = n;

    up->fd = socket(be->addr.ss.ss_family, SOCK_STREAM, 0);
    if (up->fd < 0 || fd_set_nonblock_cloexec(up->fd) < 0) {
        return 503; /* saturación local (p. ej. sin fds): no es culpa del backend */
    }
    int one = 1;
    (void)setsockopt(up->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    (void)setsockopt(up->fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    if (connect(up->fd, (const struct sockaddr *)&be->addr.ss, be->addr.len) == 0) {
        up->connected = true;
    } else if (errno != EINPROGRESS) {
        passive_failure(c, ch, errno);
        return backend_fault(errno) ? 502 : 503;
    }
    if (io_loop_add(w->loop, up->fd, IO_READ | IO_WRITE, on_upstream_io, c) < 0) {
        return 502;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Transiciones                                                              */
/* ------------------------------------------------------------------------- */

static void enter_closing(struct client_conn *c) {
    if (c->phase != PH_CLOSING && c->phase != PH_CLOSED) {
        c->phase = PH_CLOSING;
        c->closing_started_ms = timer_now_ms();
    }
    c->keep_alive = false;
}

/* Cierre inmediato sin más escrituras (cliente desaparecido o flujo roto). */
static void conn_abort(struct client_conn *c) {
    upstream_release(c, false);
    c->phase = PH_CLOSED;
}

/*
 * Log de acceso (si [log].access): cliente, host normalizado, método, estado,
 * backend y duración. Nunca la ruta ni la query (pueden llevar tokens), ni
 * cabeceras ni cuerpos.
 */
static void access_log(const struct client_conn *c, int status) {
    const struct config *cfg = cfg_of(c);
    if (!cfg->log.access) {
        return;
    }
    const char *backend = "-";
    if (c->up != NULL) {
        backend = cfg->pools[c->up->choice.pool].backends[c->up->choice.backend].addr.text;
    }
    plog(LOG_INFO, backend_pools_id(c->gen),
         "access client=%s host=%s method=%s status=%d backend=%s ms=%llu", c->peer_ip,
         c->host[0] ? c->host : "-", c->method[0] ? c->method : "-", status, backend,
         (unsigned long long)(c->req_start_ms ? timer_now_ms() - c->req_start_ms : 0));
}

/*
 * Error antes de haber enviado la cabecera de respuesta: respuesta de error
 * propia y cierre. Nunca se inserta tras una respuesta ya empezada.
 */
static void fail_request(struct client_conn *c, int status) {
    if (!c->response_started) {
        access_log(c, status);
    }
    upstream_release(c, false);
    if (c->response_started) {
        conn_abort(c);
        return;
    }
    c->expect_100 = false;
    c->pending_error = status;
    c->response_started = true;
    if (status >= 500) {
        c->w->stats.responses_5xx++;
    } else {
        c->w->stats.responses_4xx++;
    }
    enter_closing(c);
}

/*
 * Fallo del upstream: si aún no se ha enviado nada de la respuesta, error
 * propio (502/504); si ya se envió una parte, se entrega lo recibido y se
 * cierra sin completar, para que el cliente detecte el truncado.
 */
static void upstream_fail(struct client_conn *c, int status) {
    if (!c->response_started) {
        fail_request(c, status);
        return;
    }
    upstream_release(c, false);
    enter_closing(c);
}

static void next_request(struct client_conn *c) {
    http_parser_reset(&c->rp);
    c->method[0] = '\0';
    c->host[0] = '\0';
    c->req_start_ms = 0;
    c->phase = PH_HEAD;
    c->head_started_ms = 0;
    c->is_head = false;
    c->req_done = false;
    c->expect_100 = false;
    c->response_started = false;
    c->resp_head_done = false;
    c->resp_done = false;
}

static void start_exchange(struct client_conn *c) {
    struct worker *w = c->w;
    const struct config *cfg = cfg_of(c);
    c->requests++;
    w->stats.requests++;
    c->is_head = http_span_ieq(c->in.p + c->in.off, c->req.method, "HEAD");
    c->req_start_ms = timer_now_ms();
    size_t ml =
        c->req.method.len < sizeof(c->method) - 1 ? c->req.method.len : sizeof(c->method) - 1;
    memcpy(c->method, c->in.p + c->in.off + c->req.method.off, ml);
    c->method[ml] = '\0';
    (void)snprintf(c->host, sizeof(c->host), "%s", c->req.has_host ? c->req.host.name : "");
    c->client_minor = c->req.version_minor;

    const char *host = c->req.has_host ? c->req.host.name : "";
    size_t hlen = c->req.has_host ? c->req.host.len : 0;
    struct route_result rr = router_lookup(cfg->router, host, hlen);
    if (rr.match == ROUTE_NONE) {
        fail_request(c, 502);
        return;
    }
    struct bp_choice ch;
    if (backend_pool_pick(c->gen, rr.target, &ch) != BP_OK) {
        fail_request(c, 503);
        return;
    }
    int status = upstream_open(c, &ch); /* construye la cabecera: antes de consumir */
    if (status != 0) {
        fail_request(c, status);
        return;
    }
    buf_consume(&c->in, c->req.head_len);

    c->keep_alive =
        c->req.keep_alive && !w->draining && c->requests < cfg->limits.max_requests_per_connection;
    http_body_init(&c->req_body, &c->req, cfg->limits.max_body_bytes);
    c->req_done = c->req.body == HTTP_BODY_NONE;
    /* 100-continue lo contesta el proxy (Expect no se reenvía); en HTTP/1.0
     * se ignora (RFC 9110 §10.1.1). */
    c->expect_100 = c->req.expect_continue && c->client_minor == 1 && !c->req_done;
    c->response_started = false;
    c->resp_head_done = false;
    c->resp_done = false;
    struct http_limits lim = {.max_head = SLOT, .max_request_line = 8192, .max_headers = 100};
    http_parser_init(&c->sp, &lim);
    c->phase = PH_EXCHANGE;
}

/* ------------------------------------------------------------------------- */
/* Pasos                                                                     */
/* ------------------------------------------------------------------------- */

static bool client_read(struct client_conn *c) {
    if (!c->readable || c->eof) {
        return false;
    }
    if (c->phase == PH_CLOSING) {
        c->in.off = 0; /* se descarta lo que llegue durante el cierre */
        c->in.len = 0;
    }
    size_t space = buf_space(&c->in);
    if (space == 0) {
        return false; /* backpressure: readable sigue activo */
    }
    ssize_t n = recv(c->fd, c->in.p + c->in.len, space, 0);
    if (n > 0) {
        c->in.len += (size_t)n;
        if (c->phase == PH_HEAD && c->head_started_ms == 0) {
            c->head_started_ms = timer_now_ms();
        }
        return true;
    }
    if (n == 0) {
        c->eof = true;
        return true;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        c->readable = false;
        return false;
    }
    if (errno == EINTR) {
        return true;
    }
    conn_abort(c);
    return true;
}

static bool client_write(struct client_conn *c) {
    if (!c->writable || buf_avail(&c->out) == 0 || c->phase == PH_CLOSED) {
        return false;
    }
    ssize_t n = send(c->fd, c->out.p + c->out.off, buf_avail(&c->out), SEND_FLAGS);
    if (n > 0) {
        buf_consume(&c->out, (size_t)n);
        return true;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        c->writable = false;
        return false;
    }
    if (n < 0 && errno == EINTR) {
        return true;
    }
    conn_abort(c);
    return true;
}

static bool handle_head(struct client_conn *c) {
    if (c->rp.scanned == 0) {
        adopt_current_generation(c);
    }
    size_t avail = buf_avail(&c->in);
    if (avail == 0) {
        if (c->eof || c->w->draining) {
            enter_closing(c); /* inactiva: se vacía la salida y se cierra */
            return true;
        }
        return false;
    }
    enum http_error err;
    enum http_parse_result r =
        http_parse_request(&c->rp, c->in.p + c->in.off, avail, &c->req, &err);
    if (r == HTTP_PARSE_INCOMPLETE) {
        if (buf_space(&c->in) == 0) {
            /* Buffer lleno sin fin de cabecera: con max_header_bytes igual al
             * slot, el byte que la excede no llegaría a leerse nunca. */
            fail_request(c, 431);
            return true;
        }
        if (c->eof) {
            enter_closing(c); /* el cliente se fue a mitad de cabecera */
            return true;
        }
        return false;
    }
    if (r == HTTP_PARSE_ERROR) {
        fail_request(c, http_error_status(err));
        return true;
    }
    start_exchange(c);
    return true;
}

static bool pump_request_body(struct client_conn *c) {
    struct upstream_conn *up = c->up;
    if (c->req_done || up == NULL) {
        return false;
    }
    size_t avail = buf_avail(&c->in);
    if (avail == 0) {
        if (c->eof) {
            conn_abort(c); /* el cliente cortó el cuerpo: no hay a quién responder */
            return true;
        }
        return false;
    }
    size_t space = buf_space(&up->out);
    size_t n = avail < space ? avail : space;
    if (n == 0) {
        return false; /* backpressure hacia el upstream */
    }
    size_t consumed = 0;
    enum http_error err;
    enum http_body_result r = http_body_feed(&c->req_body, c->in.p + c->in.off, n, &consumed, &err);
    memcpy(up->out.p + up->out.len, c->in.p + c->in.off, consumed);
    up->out.len += consumed;
    buf_consume(&c->in, consumed);
    if (r == HTTP_BODY_DONE) {
        c->req_done = true;
    } else if (r == HTTP_BODY_ERROR) {
        fail_request(c, http_error_status(err));
        return true;
    }
    return consumed > 0 || r != HTTP_BODY_MORE;
}

static bool upstream_connect_check(struct client_conn *c) {
    struct upstream_conn *up = c->up;
    if (!up->writable && !up->readable) {
        return false;
    }
    int soerr = 0;
    socklen_t sl = sizeof(soerr);
    if (getsockopt(up->fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0) {
        soerr = errno;
    }
    if (soerr != 0) {
        passive_failure(c, &up->choice, soerr);
        upstream_fail(c, 502);
        return true;
    }
    if (!up->writable) {
        return false;
    }
    up->connected = true;
    backend_pool_connect_ok(c->gen, &up->choice);
    return true;
}

static bool upstream_io(struct client_conn *c) {
    struct upstream_conn *up = c->up;
    if (up == NULL) {
        return false;
    }
    bool progress = false;
    if (!up->connected) {
        progress = upstream_connect_check(c);
        if (c->up == NULL || !up->connected) {
            return progress;
        }
    }
    while (up->writable && !up->write_broken && buf_avail(&up->out) > 0) {
        ssize_t n = send(up->fd, up->out.p + up->out.off, buf_avail(&up->out), SEND_FLAGS);
        if (n > 0) {
            buf_consume(&up->out, (size_t)n);
            progress = true;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            up->writable = false;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            /* El upstream cerró su lado de lectura (p. ej. respondió antes de
             * leer el cuerpo). La lectura decide: respuesta o 502. */
            up->write_broken = true;
            progress = true;
        }
    }
    while (up->readable && !up->eof) {
        size_t space = buf_space(&up->in);
        if (space == 0) {
            break; /* backpressure hacia el cliente */
        }
        ssize_t n = recv(up->fd, up->in.p + up->in.len, space, 0);
        if (n > 0) {
            up->in.len += (size_t)n;
            progress = true;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            up->readable = false;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            up->eof = true; /* FIN, o ECONNRESET y similares: fin del flujo */
            progress = true;
        }
    }
    return progress;
}

static bool response_head(struct client_conn *c) {
    struct upstream_conn *up = c->up;
    size_t avail = buf_avail(&up->in);
    if (avail == 0) {
        if (up->eof) {
            upstream_fail(c, 502); /* cerró sin responder */
            return true;
        }
        return false;
    }
    enum http_error err;
    enum http_parse_result r =
        http_parse_response(&c->sp, up->in.p + up->in.off, avail, c->is_head, &c->resp, &err);
    if (r == HTTP_PARSE_INCOMPLETE) {
        if (up->eof || buf_space(&up->in) == 0) {
            upstream_fail(c, 502); /* cabecera truncada o mayor que un slot */
            return true;
        }
        return false;
    }
    if (r == HTTP_PARSE_ERROR) {
        upstream_fail(c, 502);
        return true;
    }
    if (c->resp.informational) {
        if (c->resp.status == 101) {
            /* Nunca se reenvía Upgrade: un 101 es una respuesta inválida. */
            upstream_fail(c, 502);
            return true;
        }
        /* Otras 1xx (100, 103) se descartan: el 100 del cliente lo da el
         * proxy y las pistas tempranas no son necesarias. */
        buf_consume(&up->in, c->resp.head_len);
        http_parser_reset(&c->sp);
        return true;
    }
    if (c->resp.body == HTTP_BODY_CHUNKED && c->client_minor == 0) {
        upstream_fail(c, 502); /* no se puede entregar chunked a HTTP/1.0 */
        return true;
    }
    /* Sin delimitador propio no se puede mantener la conexión del cliente. */
    bool keep = c->keep_alive && c->resp.body != HTTP_BODY_UNTIL_CLOSE;
    c->expect_100 = false; /* la respuesta final hace innecesario el 100 */
    size_t n = 0;
    size_t space = buf_space(&c->out);
    if (http_forward_response(up->in.p + up->in.off, &c->resp, keep, c->client_minor,
                              c->out.p + c->out.len, space, &n) < 0) {
        if (buf_avail(&c->out) > 0) {
            return false; /* esperar a que se vacíe la salida */
        }
        upstream_fail(c, 502);
        return true;
    }
    c->out.len += n;
    c->response_started = true;
    c->keep_alive = keep;
    buf_consume(&up->in, c->resp.head_len);
    http_body_init_kind(&c->resp_body, c->resp.body, c->resp.content_length, 0);
    c->resp_head_done = true;
    c->resp_done = c->resp.body == HTTP_BODY_NONE;
    return true;
}

static bool response_body(struct client_conn *c) {
    struct upstream_conn *up = c->up;
    size_t avail = buf_avail(&up->in);
    if (avail == 0) {
        if (!up->eof) {
            return false;
        }
        if (http_body_eof(&c->resp_body) == HTTP_BODY_DONE) {
            c->resp_done = true; /* delimitada por cierre */
        } else {
            upstream_fail(c, 502); /* truncada: se entrega lo recibido y se cierra */
        }
        return true;
    }
    size_t space = buf_space(&c->out);
    size_t n = avail < space ? avail : space;
    if (n == 0) {
        return false; /* backpressure hacia el cliente */
    }
    size_t consumed = 0;
    enum http_error err;
    enum http_body_result r =
        http_body_feed(&c->resp_body, up->in.p + up->in.off, n, &consumed, &err);
    memcpy(c->out.p + c->out.len, up->in.p + up->in.off, consumed);
    c->out.len += consumed;
    buf_consume(&up->in, consumed);
    if (r == HTTP_BODY_DONE) {
        c->resp_done = true;
    } else if (r == HTTP_BODY_ERROR) {
        upstream_fail(c, 502); /* chunked inválido a mitad: cierre */
        return true;
    }
    return consumed > 0 || r != HTTP_BODY_MORE;
}

static bool pump_response(struct client_conn *c) {
    if (c->up == NULL || c->resp_done) {
        return false;
    }
    return c->resp_head_done ? response_body(c) : response_head(c);
}

static bool check_done(struct client_conn *c) {
    if (c->phase != PH_EXCHANGE || !c->resp_done) {
        return false;
    }
    bool reusable = c->req_done && c->resp.body != HTTP_BODY_UNTIL_CLOSE &&
                    !c->resp.upstream_close && c->up != NULL && !c->up->write_broken;
    access_log(c, c->resp.status);
    upstream_release(c, reusable);
    if (c->keep_alive && c->req_done && !c->eof && !c->w->draining) {
        next_request(c);
    } else {
        enter_closing(c); /* p. ej. respuesta anticipada con cuerpo pendiente */
    }
    return true;
}

static bool emit_pending(struct client_conn *c) {
    bool progress = false;
    if (c->expect_100 && buf_space(&c->out) >= sizeof(CONTINUE_100) - 1) {
        memcpy(c->out.p + c->out.len, CONTINUE_100, sizeof(CONTINUE_100) - 1);
        c->out.len += sizeof(CONTINUE_100) - 1;
        c->expect_100 = false;
        progress = true;
    }
    if (c->pending_error != 0 && write_error(c, c->pending_error)) {
        c->pending_error = 0;
        progress = true;
    }
    return progress;
}

static bool closing_step(struct client_conn *c) {
    if (c->pending_error != 0 || buf_avail(&c->out) > 0) {
        return false;
    }
    if (!c->shut_wr) {
        /* Cierre "con espera": el cliente recibe la respuesta completa antes
         * del FIN; lo que siga enviando se descarta hasta su EOF o close_ms. */
        (void)shutdown(c->fd, SHUT_WR);
        c->shut_wr = true;
        return true;
    }
    if (c->eof) {
        c->phase = PH_CLOSED;
        return true;
    }
    return false;
}

static bool step(struct client_conn *c) {
    if (c->phase == PH_CLOSED) {
        return false;
    }
    bool p = client_read(c);
    switch (c->phase) {
    case PH_HEAD:
        p |= handle_head(c);
        break;
    case PH_EXCHANGE:
        p |= pump_request_body(c);
        p |= upstream_io(c);
        p |= pump_response(c);
        p |= check_done(c);
        break;
    case PH_CLOSING:
    case PH_CLOSED:
        break;
    }
    if (c->phase == PH_CLOSED) {
        return false;
    }
    p |= emit_pending(c);
    p |= client_write(c);
    if (c->phase == PH_CLOSING) {
        p |= closing_step(c);
    }
    if (p) {
        c->activity = true;
    }
    return p && c->phase != PH_CLOSED;
}

/* ------------------------------------------------------------------------- */
/* Plazos                                                                    */
/* ------------------------------------------------------------------------- */

/* Instante de vencimiento del plazo de la fase actual. */
static uint64_t deadline_for(const struct client_conn *c, uint64_t now) {
    const struct cfg_timeouts *t = &cfg_of(c)->timeouts;
    switch (c->phase) {
    case PH_HEAD:
        if (c->head_started_ms != 0) {
            return c->head_started_ms + t->client_header_ms; /* absoluto: sin slowloris */
        }
        return now + (c->requests == 0 ? t->client_header_ms : t->client_idle_ms);
    case PH_EXCHANGE:
        if (c->up != NULL && !c->up->connected) {
            return now + t->upstream_connect_ms;
        }
        if (!c->resp_head_done && c->req_done && c->up != NULL && buf_avail(&c->up->out) == 0) {
            return now + t->upstream_response_ms;
        }
        return now + t->io_idle_ms;
    case PH_CLOSING:
        return c->closing_started_ms + t->close_ms;
    case PH_CLOSED:
        break;
    }
    return now;
}

static void arm_deadline(struct client_conn *c) {
    if (c->phase != PH_CLOSED) {
        (void)timer_schedule_at(&c->w->timers, &c->deadline, deadline_for(c, timer_now_ms()));
    }
}

static void client_free(struct client_conn *c) {
    struct worker *w = c->w;
    timer_cancel(&w->timers, &c->deadline);
    timer_cancel(&w->timers, &c->resume);
    upstream_release(c, false);
    if (c->fd >= 0) {
        (void)io_loop_del(w->loop, c->fd);
        close(c->fd);
    }
    buf_release(w->bufs, &c->in);
    buf_release(w->bufs, &c->out);
    if (c->prev != NULL) {
        c->prev->next = c->next;
    } else {
        w->conns = c->next;
    }
    if (c->next != NULL) {
        c->next->prev = c->prev;
    }
    w->nconns--;
    backend_pools_unref(c->gen);
    free(c);
    if (w->draining && w->nconns == 0) {
        w->stop = true;
    }
}

static void client_run(struct client_conn *c) {
    int i = 0;
    while (i < RUN_BUDGET && step(c)) {
        i++;
    }
    if (c->phase == PH_CLOSED) {
        client_free(c);
        return;
    }
    if (i == RUN_BUDGET) {
        /* Presupuesto agotado con progreso pendiente: continuar en la
         * siguiente vuelta del bucle (sin esperar un evento que no vendrá). */
        (void)timer_schedule_in(&c->w->timers, &c->resume, timer_now_ms(), 0);
    }
    if (c->activity) {
        c->activity = false;
        arm_deadline(c);
    }
}

/* ------------------------------------------------------------------------- */
/* Callbacks                                                                 */
/* ------------------------------------------------------------------------- */

static void on_deadline(timer_heap *heap, timer *t, void *ud) {
    (void)heap;
    (void)t;
    struct client_conn *c = ud;
    struct upstream_conn *up = c->up;
    switch (c->phase) {
    case PH_HEAD:
        if (buf_avail(&c->in) > 0) {
            fail_request(c, 408); /* cabecera incompleta dentro del plazo */
        } else if (buf_avail(&c->out) > 0) {
            conn_abort(c); /* inactiva y sin leer lo pendiente */
        } else {
            c->phase = PH_CLOSED; /* keep-alive inactiva: cierre silencioso */
        }
        break;
    case PH_EXCHANGE:
        if (up != NULL && !up->connected) {
            passive_failure(c, &up->choice, ETIMEDOUT); /* el backend no contestó */
            upstream_fail(c, 504);
        } else if (!c->resp_head_done) {
            /* Si hay datos esperando a entrar en el upstream, el lento es él
             * (504); si no, es el cliente que no envía el cuerpo (408). */
            bool upstream_slow = c->req_done || (up != NULL && buf_avail(&up->out) > 0);
            upstream_fail(c, upstream_slow ? 504 : 408);
        } else {
            conn_abort(c); /* respuesta a medias: no cabe otra respuesta */
        }
        break;
    case PH_CLOSING:
        conn_abort(c);
        break;
    case PH_CLOSED:
        break;
    }
    c->activity = true;
    client_run(c);
}

static void on_resume(timer_heap *heap, timer *t, void *ud) {
    (void)heap;
    (void)t;
    client_run(ud);
}

static void on_client_io(io_loop *loop, int fd, uint32_t events, void *ud) {
    (void)loop;
    (void)fd;
    struct client_conn *c = ud;
    if (events & (IO_READ | IO_HUP | IO_ERROR)) {
        c->readable = true;
    }
    if (events & (IO_WRITE | IO_HUP | IO_ERROR)) {
        c->writable = true;
    }
    client_run(c);
}

static void on_upstream_io(io_loop *loop, int fd, uint32_t events, void *ud) {
    (void)loop;
    struct client_conn *c = ud;
    struct upstream_conn *up = c->up;
    if (up != NULL && up->fd == fd) {
        if (events & (IO_READ | IO_HUP | IO_ERROR)) {
            up->readable = true;
        }
        if (events & (IO_WRITE | IO_HUP | IO_ERROR)) {
            up->writable = true;
        }
    }
    client_run(c);
}

/* ------------------------------------------------------------------------- */
/* API                                                                       */
/* ------------------------------------------------------------------------- */

int client_conn_accept(struct worker *w, int fd, const struct sockaddr_storage *peer,
                       size_t fe_idx) {
    const struct config *cfg = backend_pools_config(w->gen);
    if (w->nconns >= cfg->max_connections) {
        return -1;
    }
    struct client_conn *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return -1;
    }
    if (!buf_acquire(w->bufs, &c->in) || !buf_acquire(w->bufs, &c->out)) {
        buf_release(w->bufs, &c->in);
        free(c);
        return -1;
    }
    c->w = w;
    c->fd = fd;
    c->fe_idx = fe_idx;
    c->peer = *peer;
    const void *addr = peer->ss_family == AF_INET
                           ? (const void *)&((const struct sockaddr_in *)peer)->sin_addr
                           : (const void *)&((const struct sockaddr_in6 *)peer)->sin6_addr;
    if (inet_ntop(peer->ss_family, addr, c->peer_ip, sizeof(c->peer_ip)) == NULL) {
        buf_release(w->bufs, &c->in);
        buf_release(w->bufs, &c->out);
        free(c);
        return -1;
    }
    c->peer_trusted = config_peer_trusted(&cfg->frontends[fe_idx], peer);
    struct http_limits lim = request_limits(cfg);
    http_parser_init(&c->rp, &lim);
    timer_init(&c->deadline, on_deadline, c);
    timer_init(&c->resume, on_resume, c);
    if (io_loop_add(w->loop, fd, IO_READ | IO_WRITE, on_client_io, c) < 0) {
        buf_release(w->bufs, &c->in);
        buf_release(w->bufs, &c->out);
        free(c);
        return -1;
    }
    c->gen = backend_pools_ref(w->gen);
    c->next = w->conns;
    if (w->conns != NULL) {
        w->conns->prev = c;
    }
    w->conns = c;
    w->nconns++;
    c->phase = PH_HEAD;
    c->activity = true;
    client_run(c);
    return 0;
}

void client_conn_drain_all(struct worker *w) {
    struct client_conn *c = w->conns;
    while (c != NULL) {
        struct client_conn *next = c->next; /* client_run puede liberar c */
        c->keep_alive = false;
        c->activity = true;
        client_run(c);
        c = next;
    }
}

void client_conn_abort_all(struct worker *w) {
    struct client_conn *c = w->conns;
    while (c != NULL) {
        struct client_conn *next = c->next;
        conn_abort(c);
        client_run(c);
        c = next;
    }
}
