/*
 * test_http_parser.c — host, cabecera de petición incremental, framing del
 * cuerpo y construcción de la cabecera de reenvío.
 */
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>

#include "host.h"
#include "http_forward.h"
#include "http_parser.h"

#define TEST_TIMEOUT_S 10

static int setup(void **state) {
    (void)state;
    alarm(TEST_TIMEOUT_S);
    return 0;
}

static int teardown(void **state) {
    (void)state;
    alarm(0);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Utilidades                                                                */
/* ------------------------------------------------------------------------- */

struct outcome {
    enum http_parse_result res;
    enum http_error err;
    size_t fed; /* bytes entregados cuando terminó (DONE o ERROR) */
};

/* Análisis de una sola vez. */
static struct outcome parse_once(const char *s, size_t len, const struct http_limits *lim,
                                 struct http_request *req) {
    struct http_parser p;
    http_parser_init(&p, lim);
    struct outcome o = {0};
    o.res = http_parse_request(&p, s, len, req, &o.err);
    o.fed = len;
    return o;
}

/* Entrega el mensaje byte a byte (el buffer crece como tras cada recv). */
static struct outcome parse_bytewise(const char *s, size_t len, const struct http_limits *lim,
                                     struct http_request *req) {
    struct http_parser p;
    http_parser_init(&p, lim);
    struct outcome o = {0};
    for (size_t n = 1; n <= len; n++) {
        o.res = http_parse_request(&p, s, n, req, &o.err);
        o.fed = n;
        if (o.res != HTTP_PARSE_INCOMPLETE) {
            break;
        }
    }
    return o;
}

/* Entrega en dos fragmentos partidos en `cut`. */
static struct outcome parse_split(const char *s, size_t len, size_t cut,
                                  const struct http_limits *lim, struct http_request *req) {
    struct http_parser p;
    http_parser_init(&p, lim);
    struct outcome o = {0};
    o.res = http_parse_request(&p, s, cut, req, &o.err);
    o.fed = cut;
    if (o.res == HTTP_PARSE_INCOMPLETE) {
        o.res = http_parse_request(&p, s, len, req, &o.err);
        o.fed = len;
    }
    return o;
}

/*
 * Comprueba que un mensaje da el mismo resultado de una vez, byte a byte y
 * partido en cualquier posición. Devuelve el resultado de una sola vez.
 */
static struct outcome parse_all_ways(const char *s, const struct http_limits *lim,
                                     struct http_request *req) {
    size_t len = strlen(s);
    struct outcome once = parse_once(s, len, lim, req);
    struct http_request tmp;

    struct outcome bw = parse_bytewise(s, len, lim, &tmp);
    assert_int_equal(bw.res, once.res);
    assert_int_equal(bw.err, once.err);
    if (once.res == HTTP_PARSE_DONE) {
        assert_int_equal(tmp.head_len, req->head_len);
        assert_int_equal(bw.fed, req->head_len); /* no antes del final */
    }
    for (size_t cut = 1; cut < len; cut++) {
        struct outcome sp = parse_split(s, len, cut, lim, &tmp);
        assert_int_equal(sp.res, once.res);
        assert_int_equal(sp.err, once.err);
    }
    return once;
}

static void assert_span(const char *buf, struct http_span sp, const char *expected) {
    assert_int_equal(sp.len, strlen(expected));
    assert_memory_equal(buf + sp.off, expected, sp.len);
}

static void expect_error(const char *s, enum http_error expected, int status) {
    struct http_request req;
    struct outcome o = parse_all_ways(s, NULL, &req);
    if (o.res != HTTP_PARSE_ERROR || o.err != expected) {
        fail_msg("para %.60s: esperado error %d (%s), obtenido res=%d err=%d (%s)", s,
                 (int)expected, http_error_str(expected), (int)o.res, (int)o.err,
                 http_error_str(o.err));
    }
    assert_int_equal(http_error_status(o.err), status);
}

static void expect_ok(const char *s, struct http_request *req) {
    struct outcome o = parse_all_ways(s, NULL, req);
    if (o.res != HTTP_PARSE_DONE) {
        fail_msg("para %.60s: esperado DONE, obtenido res=%d err=%s", s, (int)o.res,
                 http_error_str(o.err));
    }
}

/* ------------------------------------------------------------------------- */
/* host                                                                      */
/* ------------------------------------------------------------------------- */

static void assert_host(const char *in, bool allow_port, const char *name, uint16_t port) {
    struct host_name h;
    enum host_status st = host_normalize(in, strlen(in), allow_port, &h);
    if (st != HOST_OK) {
        fail_msg("host \"%s\": %s", in, host_status_str(st));
    }
    assert_string_equal(h.name, name);
    assert_int_equal(h.len, strlen(name));
    assert_int_equal(h.port, port);
    assert_int_equal(h.has_port, port != 0);
}

static void assert_host_bad(const char *in, bool allow_port, enum host_status expected) {
    struct host_name h;
    enum host_status st = host_normalize(in, strlen(in), allow_port, &h);
    if (st != expected) {
        fail_msg("host \"%s\": esperado %s, obtenido %s", in, host_status_str(expected),
                 host_status_str(st));
    }
}

static void test_host_normalization(void **state) {
    (void)state;
    assert_host("example.com", true, "example.com", 0);
    assert_host("Example.COM", true, "example.com", 0);
    assert_host("EXAMPLE.com.", true, "example.com", 0);
    assert_host("example.com:8080", true, "example.com", 8080);
    assert_host("example.com.:443", true, "example.com", 443);
    assert_host("10.0.0.1:80", true, "10.0.0.1", 80);
    assert_host("under_score.local", true, "under_score.local", 0);
    assert_host("[::1]", true, "[::1]", 0);
    assert_host("[0:0:0:0:0:0:0:1]:8443", true, "[::1]", 8443);
    assert_host("[2001:DB8::A]", true, "[2001:db8::a]", 0);

    char label63[80];
    memset(label63, 'a', 63);
    memcpy(label63 + 63, ".com", 5);
    struct host_name h;
    assert_int_equal(host_normalize(label63, strlen(label63), false, &h), HOST_OK);
}

static void test_host_rejections(void **state) {
    (void)state;
    assert_host_bad("", true, HOST_EMPTY);
    assert_host_bad(".", true, HOST_EMPTY);
    assert_host_bad("exa mple.com", true, HOST_BAD_CHAR);
    assert_host_bad("a%2eb.com", true, HOST_BAD_CHAR);
    assert_host_bad("*.example.com", true, HOST_BAD_CHAR);
    assert_host_bad("user@example.com", true, HOST_BAD_CHAR);
    assert_host_bad("a..b", true, HOST_BAD_LABEL);
    assert_host_bad(".a.b", true, HOST_BAD_LABEL);
    assert_host_bad("a.b..", true, HOST_BAD_LABEL);
    assert_host_bad("-a.com", true, HOST_BAD_LABEL);
    assert_host_bad("a-.com", true, HOST_BAD_LABEL);
    assert_host_bad("example.com:", true, HOST_BAD_PORT);
    assert_host_bad("example.com:0", true, HOST_BAD_PORT);
    assert_host_bad("example.com:65536", true, HOST_BAD_PORT);
    assert_host_bad("example.com:80a", true, HOST_BAD_PORT);
    assert_host_bad("example.com:1:2", true, HOST_BAD_PORT);
    assert_host_bad("example.com:80", false, HOST_BAD_PORT);
    assert_host_bad("[::1", true, HOST_BAD_IPV6);
    assert_host_bad("[zz::1]", true, HOST_BAD_IPV6);
    assert_host_bad("[::1]x", true, HOST_BAD_IPV6);
    /* Identificadores de zona: rechazados por política propia (no depende de
     * inet_pton, que en macOS los acepta). */
    assert_host_bad("[fe80::1%25eth0]", true, HOST_BAD_IPV6);
    assert_host_bad("[fe80::1%eth0]", true, HOST_BAD_IPV6);
    assert_host_bad("[fe80::1%1]", true, HOST_BAD_IPV6);
    assert_host_bad("[fe80::1%25eth0]:8080", true, HOST_BAD_IPV6);
    assert_host_bad("[::1 ]", true, HOST_BAD_IPV6);
    assert_host_bad("[::1/64]", true, HOST_BAD_IPV6);
    /* IPv4 embebida sigue siendo válida ('.' está permitido). */
    struct host_name h4;
    assert_int_equal(host_normalize("[::ffff:192.0.2.1]", 18, true, &h4), HOST_OK);
    assert_true(h4.is_ipv6);

    char label64[80];
    memset(label64, 'a', 64);
    memcpy(label64 + 64, ".com", 5);
    assert_host_bad(label64, false, HOST_BAD_LABEL);

    char longname[300];
    size_t n = 0;
    while (n < 260) { /* 260 + "com" = 263 > 253 */
        /* Sin NUL a propósito: se termina tras el bucle. */
        memcpy(longname + n, "abcdefghi.", 10); // NOLINT(bugprone-not-null-terminated-result)
        n += 10;
    }
    memcpy(longname + n, "com", 4);
    assert_host_bad(longname, false, HOST_TOO_LONG);
}

/* ------------------------------------------------------------------------- */
/* Petición: casos válidos                                                   */
/* ------------------------------------------------------------------------- */

static void test_simple_get(void **state) {
    (void)state;
    const char *s = "GET /index.html?q=1 HTTP/1.1\r\n"
                    "Host: Example.COM:8080\r\n"
                    "User-Agent: test\r\n"
                    "Accept:  */*  \r\n"
                    "\r\n";
    struct http_request req;
    expect_ok(s, &req);
    assert_span(s, req.method, "GET");
    assert_span(s, req.target, "/index.html?q=1");
    assert_span(s, req.path, "/index.html?q=1");
    assert_int_equal(req.form, HTTP_TARGET_ORIGIN);
    assert_int_equal(req.version_minor, 1);
    assert_true(req.has_host);
    assert_string_equal(req.host.name, "example.com");
    assert_int_equal(req.host.port, 8080);
    assert_span(s, req.host_header, "Example.COM:8080");
    assert_int_equal(req.nheaders, 3);
    assert_span(s, req.headers[2].name, "Accept");
    assert_span(s, req.headers[2].value, "*/*"); /* OWS recortado */
    assert_int_equal(req.body, HTTP_BODY_NONE);
    assert_true(req.keep_alive);
    assert_int_equal(req.head_len, strlen(s));

    const struct http_header *ua = http_request_find(&req, s, "user-agent");
    assert_non_null(ua);
    assert_span(s, ua->value, "test");
    assert_null(http_request_find(&req, s, "cookie"));
}

static void test_http10_without_host_uses_default_route(void **state) {
    (void)state;
    const char *s = "GET / HTTP/1.0\r\n\r\n";
    struct http_request req;
    expect_ok(s, &req);
    assert_int_equal(req.version_minor, 0);
    assert_false(req.has_host);
    assert_false(req.keep_alive);
}

static void test_leading_empty_lines_are_skipped(void **state) {
    (void)state;
    const char *s = "\r\n\r\nGET / HTTP/1.1\r\nHost: a\r\n\r\n";
    struct http_request req;
    expect_ok(s, &req);
    assert_span(s, req.method, "GET");
    assert_int_equal(req.head_len, strlen(s));
}

static void test_absolute_form(void **state) {
    (void)state;
    const char *s = "GET http://Api.Example.com:81/p?q=1 HTTP/1.1\r\nHost: ignored.test\r\n\r\n";
    struct http_request req;
    expect_ok(s, &req);
    assert_int_equal(req.form, HTTP_TARGET_ABSOLUTE);
    assert_string_equal(req.host.name, "api.example.com");
    assert_int_equal(req.host.port, 81);
    assert_span(s, req.authority, "Api.Example.com:81");
    assert_span(s, req.path, "/p?q=1");

    const char *s2 = "GET HTTP://h.test HTTP/1.1\r\nHost: h.test\r\n\r\n";
    expect_ok(s2, &req);
    assert_int_equal(req.path.len, 0);
    assert_string_equal(req.host.name, "h.test");
}

static void test_asterisk_form_only_for_options(void **state) {
    (void)state;
    struct http_request req;
    expect_ok("OPTIONS * HTTP/1.1\r\nHost: a\r\n\r\n", &req);
    assert_int_equal(req.form, HTTP_TARGET_ASTERISK);
    expect_error("GET * HTTP/1.1\r\nHost: a\r\n\r\n", HTTP_ERR_BAD_TARGET, 400);
}

static void test_keep_alive_semantics(void **state) {
    (void)state;
    struct http_request req;
    expect_ok("GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n", &req);
    assert_false(req.keep_alive);
    expect_ok("GET / HTTP/1.1\r\nHost: a\r\nConnection: Keep-Alive, CLOSE\r\n\r\n", &req);
    assert_false(req.keep_alive);
    expect_ok("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", &req);
    assert_true(req.keep_alive);
    expect_ok("GET / HTTP/1.0\r\nConnection: foo\r\n\r\n", &req);
    assert_false(req.keep_alive);
}

static void test_upgrade_and_connect_rejected(void **state) {
    (void)state;
    /* Sin cambio de protocolo: la petición no se reenvía como normal. */
    expect_error("GET /ws HTTP/1.1\r\nHost: a\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
                 "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n",
                 HTTP_ERR_UPGRADE, 501);
    expect_error("GET / HTTP/1.1\r\nHost: a\r\nConnection: Upgrade, HTTP2-Settings\r\n"
                 "Upgrade: h2c\r\nHTTP2-Settings: AAMAAABkAAQAoAAAAAIAAAAA\r\n\r\n",
                 HTTP_ERR_UPGRADE, 501);
    /* Upgrade sin el token en Connection también se rechaza. */
    expect_error("GET / HTTP/1.1\r\nHost: a\r\nupgrade: websocket\r\n\r\n", HTTP_ERR_UPGRADE, 501);
    expect_error("CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n\r\n",
                 HTTP_ERR_CONNECT, 501);
}

static void test_connection_naming_framing_rejected(void **state) {
    (void)state;
    expect_error("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\n"
                 "Connection: Content-Length\r\n\r\n",
                 HTTP_ERR_CONNECTION_FRAMING, 400);
    expect_error("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n"
                 "Connection: keep-alive, transfer-encoding\r\n\r\n",
                 HTTP_ERR_CONNECTION_FRAMING, 400);
    expect_error("GET / HTTP/1.1\r\nHost: a\r\nConnection: host\r\n\r\n",
                 HTTP_ERR_CONNECTION_FRAMING, 400);
    /* Nombrar otras cabeceras sigue permitido (se eliminarán al reenviar). */
    struct http_request req;
    expect_ok("GET / HTTP/1.1\r\nHost: a\r\nConnection: X-Foo\r\nX-Foo: 1\r\n\r\n", &req);
}

static void test_expect_policy(void **state) {
    (void)state;
    struct http_request req;
    expect_ok("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\nExpect: 100-Continue\r\n\r\n",
              &req);
    assert_true(req.expect_continue);
    expect_error("POST / HTTP/1.1\r\nHost: a\r\nExpect: something\r\n\r\n", HTTP_ERR_EXPECT, 417);
}

static void test_content_length_forms(void **state) {
    (void)state;
    struct http_request req;
    expect_ok("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 42\r\n\r\n", &req);
    assert_int_equal(req.body, HTTP_BODY_LENGTH);
    assert_int_equal(req.content_length, 42);
    expect_ok("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5, 5\r\nContent-Length: 5\r\n\r\n",
              &req);
    assert_int_equal(req.content_length, 5);
    expect_ok("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\n\r\n", &req);
    assert_int_equal(req.body, HTTP_BODY_NONE);
    expect_ok("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: Chunked\r\n\r\n", &req);
    assert_int_equal(req.body, HTTP_BODY_CHUNKED);
}

/* ------------------------------------------------------------------------- */
/* Petición: rechazos                                                        */
/* ------------------------------------------------------------------------- */

static void test_host_errors(void **state) {
    (void)state;
    expect_error("GET / HTTP/1.1\r\n\r\n", HTTP_ERR_HOST_MISSING, 400);
    expect_error("GET / HTTP/1.1\r\nHost: a\r\nHost: a\r\n\r\n", HTTP_ERR_HOST_DUPLICATE, 400);
    expect_error("GET / HTTP/1.1\r\nHost: a\r\nhOsT: b\r\n\r\n", HTTP_ERR_HOST_DUPLICATE, 400);
    expect_error("GET / HTTP/1.0\r\nHost: a\r\nHost: b\r\n\r\n", HTTP_ERR_HOST_DUPLICATE, 400);
    expect_error("GET / HTTP/1.1\r\nHost:\r\n\r\n", HTTP_ERR_HOST_INVALID, 400);
    expect_error("GET / HTTP/1.1\r\nHost: a b\r\n\r\n", HTTP_ERR_HOST_INVALID, 400);
    expect_error("GET / HTTP/1.1\r\nHost: a:99999\r\n\r\n", HTTP_ERR_HOST_INVALID, 400);
    expect_error("GET http://u@h/ HTTP/1.1\r\nHost: h\r\n\r\n", HTTP_ERR_BAD_TARGET, 400);
    expect_error("GET https://h/ HTTP/1.1\r\nHost: h\r\n\r\n", HTTP_ERR_BAD_TARGET, 400);
    expect_error("GET http:/// HTTP/1.1\r\nHost: h\r\n\r\n", HTTP_ERR_BAD_TARGET, 400);
    /* Absolute-form sigue exigiendo Host en HTTP/1.1. */
    expect_error("GET http://h/ HTTP/1.1\r\n\r\n", HTTP_ERR_HOST_MISSING, 400);
}

static void test_ambiguous_framing(void **state) {
    (void)state;
    expect_error("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n"
                 "Transfer-Encoding: chunked\r\n\r\n",
                 HTTP_ERR_CL_AND_TE, 400);
    expect_error("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n"
                 "Content-Length: 5\r\n\r\n",
                 HTTP_ERR_CL_AND_TE, 400);
    expect_error("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n",
                 HTTP_ERR_BAD_CONTENT_LENGTH, 400);
    expect_error("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5, 6\r\n\r\n",
                 HTTP_ERR_BAD_CONTENT_LENGTH, 400);
    expect_error("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: -1\r\n\r\n",
                 HTTP_ERR_BAD_CONTENT_LENGTH, 400);
    expect_error("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: +5\r\n\r\n",
                 HTTP_ERR_BAD_CONTENT_LENGTH, 400);
    expect_error("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 0x10\r\n\r\n",
                 HTTP_ERR_BAD_CONTENT_LENGTH, 400);
    expect_error("POST / HTTP/1.1\r\nHost: a\r\nContent-Length:\r\n\r\n",
                 HTTP_ERR_BAD_CONTENT_LENGTH, 400);
    expect_error("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 99999999999999999999\r\n\r\n",
                 HTTP_ERR_BAD_CONTENT_LENGTH, 400);
    expect_error("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: gzip\r\n\r\n",
                 HTTP_ERR_TE_UNSUPPORTED, 501);
    expect_error("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: gzip, chunked\r\n\r\n",
                 HTTP_ERR_TE_UNSUPPORTED, 501);
    expect_error("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n"
                 "Transfer-Encoding: chunked\r\n\r\n",
                 HTTP_ERR_TE_UNSUPPORTED, 501);
    expect_error("POST / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n", HTTP_ERR_TE_HTTP10, 400);
}

static void test_request_line_errors(void **state) {
    (void)state;
    expect_error("GET / HTTP/1.1\nHost: a\r\n\r\n", HTTP_ERR_BARE_LF, 400);
    expect_error("\nGET / HTTP/1.1\r\nHost: a\r\n\r\n", HTTP_ERR_BARE_LF, 400);
    expect_error("\rGET / HTTP/1.1\r\nHost: a\r\n\r\n", HTTP_ERR_BAD_REQUEST_LINE, 400);
    expect_error("GET  / HTTP/1.1\r\nHost: a\r\n\r\n", HTTP_ERR_BAD_REQUEST_LINE, 400);
    expect_error("GET / HTTP/1.1 \r\nHost: a\r\n\r\n", HTTP_ERR_BAD_VERSION, 400);
    expect_error("GE(T / HTTP/1.1\r\nHost: a\r\n\r\n", HTTP_ERR_BAD_METHOD, 400);
    expect_error(" GET / HTTP/1.1\r\nHost: a\r\n\r\n", HTTP_ERR_BAD_METHOD, 400);
    expect_error("GET foo HTTP/1.1\r\nHost: a\r\n\r\n", HTTP_ERR_BAD_TARGET, 400);
    expect_error("GET /\x7f HTTP/1.1\r\nHost: a\r\n\r\n", HTTP_ERR_BAD_TARGET, 400);
    expect_error("GET /\r\nHost: a\r\n\r\n", HTTP_ERR_BAD_TARGET, 400);
    expect_error("GET / http/1.1\r\nHost: a\r\n\r\n", HTTP_ERR_BAD_VERSION, 400);
    expect_error("GET / HTTP/1.10\r\nHost: a\r\n\r\n", HTTP_ERR_BAD_VERSION, 400);
    expect_error("GET / HTTP/1.2\r\nHost: a\r\n\r\n", HTTP_ERR_VERSION_UNSUPPORTED, 505);
    expect_error("GET / HTTP/2.0\r\nHost: a\r\n\r\n", HTTP_ERR_VERSION_UNSUPPORTED, 505);
    expect_error("GET / HTTP/0.9\r\nHost: a\r\n\r\n", HTTP_ERR_VERSION_UNSUPPORTED, 505);
    expect_error("CONNECT a:443 HTTP/1.1\r\nHost: a\r\n\r\n", HTTP_ERR_CONNECT, 501);
    /* Prefacio de HTTP/2 con conocimiento previo: sin soporte HTTP/2. */
    expect_error("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", HTTP_ERR_VERSION_UNSUPPORTED, 505);
}

static void test_header_syntax_errors(void **state) {
    (void)state;
    expect_error("GET / HTTP/1.1\r\nHost: a\r\nX: 1\n\r\n", HTTP_ERR_BARE_LF, 400);
    expect_error("GET / HTTP/1.1\r\nHost: a\r\nX: 1\r\n folded\r\n\r\n", HTTP_ERR_OBS_FOLD, 400);
    expect_error("GET / HTTP/1.1\r\nHost: a\r\nX : 1\r\n\r\n", HTTP_ERR_BAD_HEADER, 400);
    expect_error("GET / HTTP/1.1\r\nHost: a\r\n: 1\r\n\r\n", HTTP_ERR_BAD_HEADER, 400);
    expect_error("GET / HTTP/1.1\r\nHost: a\r\nNoColon\r\n\r\n", HTTP_ERR_BAD_HEADER, 400);
    expect_error("GET / HTTP/1.1\r\nHost: a\r\nX: a\x01z\r\n\r\n", HTTP_ERR_BAD_HEADER, 400);
    expect_error("GET / HTTP/1.1\r\nHost: a\r\nX: a\rz\r\n\r\n", HTTP_ERR_BAD_HEADER, 400);
    expect_error("GET / HTTP/1.1\r\nHost: a\r\nX(y): 1\r\n\r\n", HTTP_ERR_BAD_HEADER, 400);
}

/* NUL no se puede expresar en una cadena C; se prueba con longitud explícita. */
static void test_nul_in_header_is_rejected(void **state) {
    (void)state;
    const char s[] = "GET / HTTP/1.1\r\nHost: a\r\nX: a\0b\r\n\r\n";
    struct http_request req;
    struct outcome o = parse_once(s, sizeof(s) - 1, NULL, &req);
    assert_int_equal(o.res, HTTP_PARSE_ERROR);
    assert_int_equal(o.err, HTTP_ERR_BAD_HEADER);
}

/* ------------------------------------------------------------------------- */
/* Límites                                                                   */
/* ------------------------------------------------------------------------- */

static void test_request_line_limit_detected_early(void **state) {
    (void)state;
    struct http_limits lim = {.max_head = 256, .max_request_line = 32, .max_headers = 10};
    char buf[128];
    /* Línea de exactamente 32 bytes: aceptada. */
    (void)snprintf(buf, sizeof(buf), "GET /%s HTTP/1.1\r\nHost: a\r\n\r\n", "aaaaaaaaaaaaaaaaaa");
    assert_int_equal(strlen("GET /aaaaaaaaaaaaaaaaaa HTTP/1.1"), 32);
    struct http_request req;
    assert_int_equal(parse_all_ways(buf, &lim, &req).res, HTTP_PARSE_DONE);

    /* 33 bytes sin CRLF todavía: error inmediato, sin esperar más datos. */
    const char *longline = "GET /aaaaaaaaaaaaaaaaaaa HTTP/1.1";
    struct http_parser p;
    http_parser_init(&p, &lim);
    enum http_error err;
    assert_int_equal(http_parse_request(&p, longline, 32, &req, &err), HTTP_PARSE_INCOMPLETE);
    assert_int_equal(http_parse_request(&p, longline, 33, &req, &err), HTTP_PARSE_ERROR);
    assert_int_equal(err, HTTP_ERR_URI_TOO_LONG);
    assert_int_equal(http_error_status(err), 414);
}

static void test_head_size_limit(void **state) {
    (void)state;
    struct http_limits lim = {.max_head = 64, .max_request_line = 64, .max_headers = 10};
    struct http_request req;
    /* 64 bytes exactos: aceptado. */
    const char *fits = "GET / HTTP/1.1\r\nHost: a\r\nX-Pad: aaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n\r\n";
    assert_int_equal(strlen(fits), 64);
    assert_int_equal(parse_all_ways(fits, &lim, &req).res, HTTP_PARSE_DONE);

    /* Un byte más: 431 en cuanto se supera, sin esperar el final. */
    const char *big = "GET / HTTP/1.1\r\nHost: a\r\nX-Pad: aaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n\r\n";
    struct outcome o = parse_bytewise(big, strlen(big), &lim, &req);
    assert_int_equal(o.res, HTTP_PARSE_ERROR);
    assert_int_equal(o.err, HTTP_ERR_HEAD_TOO_LARGE);
    assert_int_equal(o.fed, 65);
    assert_int_equal(http_error_status(o.err), 431);

    /* Un flujo de CRLF previos también cuenta para el límite. */
    char crlfs[80];
    for (size_t i = 0; i < 70; i += 2) {
        crlfs[i] = '\r';
        crlfs[i + 1] = '\n';
    }
    o = parse_bytewise(crlfs, 70, &lim, &req);
    assert_int_equal(o.res, HTTP_PARSE_ERROR);
    assert_int_equal(o.err, HTTP_ERR_HEAD_TOO_LARGE);
}

static void test_header_count_limit(void **state) {
    (void)state;
    struct http_limits lim = {.max_head = 1024, .max_request_line = 64, .max_headers = 3};
    struct http_request req;
    assert_int_equal(
        parse_all_ways("GET / HTTP/1.1\r\nHost: a\r\nA: 1\r\nB: 2\r\n\r\n", &lim, &req).res,
        HTTP_PARSE_DONE);
    struct outcome o =
        parse_all_ways("GET / HTTP/1.1\r\nHost: a\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\n", &lim, &req);
    assert_int_equal(o.res, HTTP_PARSE_ERROR);
    assert_int_equal(o.err, HTTP_ERR_TOO_MANY_HEADERS);
    assert_int_equal(http_error_status(o.err), 431);
}

static void test_body_limit_from_content_length(void **state) {
    (void)state;
    struct http_limits lim = {
        .max_head = 1024, .max_request_line = 64, .max_headers = 10, .max_body = 100};
    struct http_request req;
    assert_int_equal(
        parse_all_ways("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 100\r\n\r\n", &lim, &req).res,
        HTTP_PARSE_DONE);
    struct outcome o =
        parse_all_ways("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 101\r\n\r\n", &lim, &req);
    assert_int_equal(o.err, HTTP_ERR_BODY_TOO_LARGE);
    assert_int_equal(http_error_status(o.err), 413);
}

/* ------------------------------------------------------------------------- */
/* Cuerpo y pipelining                                                       */
/* ------------------------------------------------------------------------- */

/* Alimenta el cuerpo en fragmentos de `step` bytes; devuelve bytes de cuerpo. */
static enum http_body_result feed_body(struct http_body *b, const char *data, size_t len,
                                       size_t step, size_t *body_bytes, enum http_error *err) {
    size_t off = 0;
    enum http_body_result r = HTTP_BODY_MORE;
    *body_bytes = 0;
    while (off < len) {
        size_t n = len - off < step ? len - off : step;
        size_t consumed;
        r = http_body_feed(b, data + off, n, &consumed, err);
        *body_bytes += consumed;
        if (r != HTTP_BODY_MORE) {
            return r;
        }
        assert_int_equal(consumed, n); /* sin terminar, consume todo */
        off += n;
    }
    return r;
}

static void test_content_length_body_framing(void **state) {
    (void)state;
    const char *s = "POST /a HTTP/1.1\r\nHost: a\r\nContent-Length: 11\r\n\r\n"
                    "hello worldGET /b HTTP/1.1\r\nHost: b\r\n\r\n";
    size_t len = strlen(s);
    struct http_parser p;
    struct http_request req;
    enum http_error err;
    http_parser_init(&p, NULL);
    assert_int_equal(http_parse_request(&p, s, len, &req, &err), HTTP_PARSE_DONE);

    for (size_t step = 1; step <= 16; step++) {
        struct http_body b;
        http_body_init(&b, &req, 0);
        size_t body = 0;
        assert_int_equal(feed_body(&b, s + req.head_len, len - req.head_len, step, &body, &err),
                         HTTP_BODY_DONE);
        assert_int_equal(body, 11);
    }

    /* Pipelining: la siguiente petición empieza tras el cuerpo. */
    size_t next = req.head_len + 11;
    http_parser_reset(&p);
    assert_int_equal(http_parse_request(&p, s + next, len - next, &req, &err), HTTP_PARSE_DONE);
    assert_span(s + next, req.target, "/b");
    assert_string_equal(req.host.name, "b");
}

static void test_pipelined_requests_without_body(void **state) {
    (void)state;
    const char *s = "GET /1 HTTP/1.1\r\nHost: a\r\n\r\n"
                    "GET /2 HTTP/1.1\r\nHost: a\r\n\r\n"
                    "GET /3 HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n";
    size_t len = strlen(s);
    struct http_parser p;
    struct http_request req;
    enum http_error err;
    http_parser_init(&p, NULL);
    const char *expected[] = {"/1", "/2", "/3"};
    size_t off = 0;
    for (int i = 0; i < 3; i++) {
        http_parser_reset(&p);
        assert_int_equal(http_parse_request(&p, s + off, len - off, &req, &err), HTTP_PARSE_DONE);
        assert_span(s + off, req.target, expected[i]);
        assert_int_equal(req.keep_alive, i < 2);
        off += req.head_len;
    }
    assert_int_equal(off, len);
}

static struct http_request chunked_request(void) {
    const char *h = "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n";
    struct http_parser p;
    struct http_request req;
    enum http_error err;
    http_parser_init(&p, NULL);
    assert_int_equal(http_parse_request(&p, h, strlen(h), &req, &err), HTTP_PARSE_DONE);
    return req;
}

static void test_chunked_body_framing(void **state) {
    (void)state;
    struct http_request req = chunked_request();
    const char *body = "4\r\nWiki\r\n"
                       "5;name=\"v\"\r\npedia\r\n"
                       "E \t; ext\r\n in\r\n\r\nchunks.\r\n"
                       "0\r\n"
                       "Trailer-A: 1\r\n"
                       "Trailer-B: 2\r\n"
                       "\r\n";
    const char *next = "GET / HTTP/1.1\r\n";
    char all[256];
    (void)snprintf(all, sizeof(all), "%s%s", body, next);

    for (size_t step = 1; step <= strlen(all); step++) {
        struct http_body b;
        enum http_error err;
        size_t consumed;
        http_body_init(&b, &req, 0);
        assert_int_equal(feed_body(&b, all, strlen(all), step, &consumed, &err), HTTP_BODY_DONE);
        assert_int_equal(consumed, strlen(body));
        assert_int_equal(b.total, 4 + 5 + 14);
    }
}

static void expect_bad_chunk(const char *body, enum http_error expected, uint64_t max_body) {
    struct http_request req = chunked_request();
    for (size_t step = 1; step <= strlen(body); step++) {
        struct http_body b;
        enum http_error err = HTTP_ERR_NONE;
        size_t consumed;
        http_body_init(&b, &req, max_body);
        enum http_body_result r = feed_body(&b, body, strlen(body), step, &consumed, &err);
        if (r != HTTP_BODY_ERROR || err != expected) {
            fail_msg("chunked \"%.40s\" paso %zu: r=%d err=%s", body, step, (int)r,
                     http_error_str(err));
        }
    }
}

static void test_chunked_body_errors(void **state) {
    (void)state;
    expect_bad_chunk("zz\r\n", HTTP_ERR_BAD_CHUNK, 0);
    expect_bad_chunk("\r\n", HTTP_ERR_BAD_CHUNK, 0);
    expect_bad_chunk("-1\r\n", HTTP_ERR_BAD_CHUNK, 0);
    expect_bad_chunk("4\nWiki\r\n0\r\n\r\n", HTTP_ERR_BAD_CHUNK, 0);
    expect_bad_chunk("4\r\nWikiX\r\n", HTTP_ERR_BAD_CHUNK, 0);
    expect_bad_chunk("4\r\nWiki\rX", HTTP_ERR_BAD_CHUNK, 0);
    expect_bad_chunk("4 x\r\n", HTTP_ERR_BAD_CHUNK, 0);
    expect_bad_chunk("4;a\x01\r\n", HTTP_ERR_BAD_CHUNK, 0);
    expect_bad_chunk("00000000000000001\r\n", HTTP_ERR_BAD_CHUNK, 0); /* 17 dígitos */
    expect_bad_chunk("0\r\nT: 1\n", HTTP_ERR_BAD_CHUNK, 0);
    expect_bad_chunk("0\r\n\rX", HTTP_ERR_BAD_CHUNK, 0);
    expect_bad_chunk("8\r\n", HTTP_ERR_BODY_TOO_LARGE, 7);
    expect_bad_chunk("4\r\nabcd\r\n4\r\n", HTTP_ERR_BODY_TOO_LARGE, 7);

    char ext[1200] = "1;";
    memset(ext + 2, 'e', 1100);
    ext[1102] = '\0';
    expect_bad_chunk(ext, HTTP_ERR_BAD_CHUNK, 0);
}

static void test_chunk_size_max_digits_accepted(void **state) {
    (void)state;
    struct http_request req = chunked_request();
    struct http_body b;
    enum http_error err;
    size_t consumed;
    http_body_init(&b, &req, 0);
    /* 16 dígitos es el máximo admitido. */
    const char *s = "0000000000000003\r\nabc\r\n0\r\n\r\n";
    assert_int_equal(http_body_feed(&b, s, strlen(s), &consumed, &err), HTTP_BODY_DONE);
    assert_int_equal(consumed, strlen(s));
}

/* ------------------------------------------------------------------------- */
/* Construcción de la cabecera de reenvío                                    */
/* ------------------------------------------------------------------------- */

static void build(const char *s, const char *ip, bool trusted, char *out, size_t cap) {
    struct http_request req;
    expect_ok(s, &req);
    struct http_forward_params prm = {.client_ip = ip, .peer_trusted = trusted};
    size_t n = 0;
    assert_int_equal(http_forward_build(s, &req, &prm, out, cap - 1, &n), 0);
    out[n] = '\0';
}

static void test_forward_untrusted_rewrites_headers(void **state) {
    (void)state;
    const char *s = "POST /p HTTP/1.1\r\n"
                    "Host: Example.com:8080\r\n"
                    "X-Forwarded-For: 6.6.6.6\r\n"
                    "X-Real-IP: 6.6.6.6\r\n"
                    "X-Forwarded-Proto: https\r\n"
                    "Forwarded: for=6.6.6.6\r\n"
                    "Connection: keep-alive, X-Secret\r\n"
                    "Keep-Alive: timeout=5\r\n"
                    "X-Secret: 1\r\n"
                    "Expect: 100-continue\r\n"
                    "TE: trailers\r\n"
                    "Proxy-Connection: keep-alive\r\n"
                    "Content-Length: 3\r\n"
                    "X-Keep: yes\r\n"
                    "\r\n";
    char out[1024];
    build(s, "192.0.2.10", false, out, sizeof(out));
    assert_string_equal(out, "POST /p HTTP/1.1\r\n"
                             "Host: Example.com:8080\r\n"
                             "Content-Length: 3\r\n"
                             "X-Keep: yes\r\n"
                             "X-Forwarded-For: 192.0.2.10\r\n"
                             "X-Real-IP: 192.0.2.10\r\n"
                             "X-Forwarded-Proto: http\r\n"
                             "Connection: close\r\n"
                             "\r\n");
}

static void test_forward_trusted_appends_xff(void **state) {
    (void)state;
    const char *s = "GET / HTTP/1.1\r\n"
                    "Host: a\r\n"
                    "X-Forwarded-For: 203.0.113.1\r\n"
                    "X-Forwarded-For: 198.51.100.2, 198.51.100.3\r\n"
                    "X-Real-IP: 203.0.113.1\r\n"
                    "X-Forwarded-Proto: https\r\n"
                    "Forwarded: for=203.0.113.1\r\n"
                    "\r\n";
    char out[1024];
    build(s, "10.0.0.5", true, out, sizeof(out));
    assert_string_equal(out, "GET / HTTP/1.1\r\n"
                             "Host: a\r\n"
                             "X-Real-IP: 203.0.113.1\r\n"
                             "X-Forwarded-Proto: https\r\n"
                             "X-Forwarded-For: 203.0.113.1, 198.51.100.2, 198.51.100.3, "
                             "10.0.0.5\r\n"
                             "Connection: close\r\n"
                             "\r\n");
}

static void test_forward_absolute_form_and_http10(void **state) {
    (void)state;
    char out[512];
    build("GET http://h.test:81?x=1 HTTP/1.1\r\nHost: other\r\n\r\n", "::1", false, out,
          sizeof(out));
    assert_string_equal(out, "GET /?x=1 HTTP/1.1\r\n"
                             "Host: h.test:81\r\n"
                             "X-Forwarded-For: ::1\r\n"
                             "X-Real-IP: ::1\r\n"
                             "X-Forwarded-Proto: http\r\n"
                             "Connection: close\r\n"
                             "\r\n");

    build("GET /x HTTP/1.0\r\n\r\n", "127.0.0.1", false, out, sizeof(out));
    assert_string_equal(out, "GET /x HTTP/1.0\r\n"
                             "X-Forwarded-For: 127.0.0.1\r\n"
                             "X-Real-IP: 127.0.0.1\r\n"
                             "X-Forwarded-Proto: http\r\n"
                             "Connection: close\r\n"
                             "\r\n");
}

static void test_forward_errors(void **state) {
    (void)state;
    const char *s = "GET / HTTP/1.1\r\nHost: a\r\n\r\n";
    struct http_request req;
    expect_ok(s, &req);
    char out[512];
    size_t n = 99;
    struct http_forward_params prm = {.client_ip = "1.2.3.4\r\nX-Evil: 1"};
    errno = 0;
    assert_int_equal(http_forward_build(s, &req, &prm, out, sizeof(out), &n), -1);
    assert_int_equal(errno, EINVAL);
    prm.client_ip = "";
    assert_int_equal(http_forward_build(s, &req, &prm, out, sizeof(out), &n), -1);

    prm.client_ip = "1.2.3.4";
    size_t full = 0;
    assert_int_equal(http_forward_build(s, &req, &prm, out, sizeof(out), &full), 0);
    errno = 0;
    assert_int_equal(http_forward_build(s, &req, &prm, out, full - 1, &n), -1);
    assert_int_equal(errno, ENOBUFS);
    assert_int_equal(n, 0);
    assert_int_equal(http_forward_build(s, &req, &prm, out, full, &n), 0);
    assert_int_equal(n, full);
}

/* ------------------------------------------------------------------------- */
/* Respuestas del upstream                                                   */
/* ------------------------------------------------------------------------- */

/* Analiza una respuesta de una vez y byte a byte; exige el mismo resultado. */
static struct outcome parse_resp(const char *s, bool head, struct http_response *resp) {
    size_t len = strlen(s);
    struct http_parser p;
    struct outcome once = {0};
    http_parser_init(&p, NULL);
    once.res = http_parse_response(&p, s, len, head, resp, &once.err);

    struct http_response tmp;
    struct outcome bw = {0};
    http_parser_init(&p, NULL);
    for (size_t n = 1; n <= len; n++) {
        bw.res = http_parse_response(&p, s, n, head, &tmp, &bw.err);
        if (bw.res != HTTP_PARSE_INCOMPLETE) {
            break;
        }
    }
    assert_int_equal(bw.res, once.res);
    assert_int_equal(bw.err, once.err);
    return once;
}

static void expect_resp(const char *s, bool head, int status, enum http_body_kind body,
                        struct http_response *resp) {
    struct outcome o = parse_resp(s, head, resp);
    if (o.res != HTTP_PARSE_DONE) {
        fail_msg("respuesta %.50s: %s", s, http_error_str(o.err));
    }
    assert_int_equal(resp->status, status);
    assert_int_equal(resp->body, body);
}

static void expect_resp_error(const char *s, enum http_error err) {
    struct http_response resp;
    struct outcome o = parse_resp(s, false, &resp);
    if (o.res != HTTP_PARSE_ERROR || o.err != err) {
        fail_msg("respuesta %.50s: esperado %s, obtenido res=%d %s", s, http_error_str(err),
                 (int)o.res, http_error_str(o.err));
    }
}

static void test_response_framing(void **state) {
    (void)state;
    struct http_response r;
    expect_resp("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n", false, 200, HTTP_BODY_LENGTH, &r);
    assert_int_equal(r.content_length, 5);
    assert_false(r.upstream_close);
    expect_resp("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n", false, 200,
                HTTP_BODY_CHUNKED, &r);
    expect_resp("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n", false, 200, HTTP_BODY_UNTIL_CLOSE,
                &r);
    assert_true(r.upstream_close);
    expect_resp("HTTP/1.0 200 OK\r\n\r\n", false, 200, HTTP_BODY_UNTIL_CLOSE, &r);
    assert_true(r.upstream_close);
    expect_resp("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", false, 200, HTTP_BODY_NONE, &r);
    /* Sin cuerpo aunque se anuncie longitud: HEAD, 204, 304. */
    expect_resp("HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n", true, 200, HTTP_BODY_NONE, &r);
    assert_int_equal(r.content_length, 1000);
    assert_true(r.has_content_length);
    expect_resp("HTTP/1.1 204 No Content\r\n\r\n", false, 204, HTTP_BODY_NONE, &r);
    expect_resp("HTTP/1.1 304 Not Modified\r\nContent-Length: 10\r\n\r\n", false, 304,
                HTTP_BODY_NONE, &r);
    expect_resp("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n", true, 200, HTTP_BODY_NONE,
                &r);
    /* Informativas. */
    expect_resp("HTTP/1.1 100 Continue\r\n\r\n", false, 100, HTTP_BODY_NONE, &r);
    assert_true(r.informational);
    expect_resp("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n", false, 101,
                HTTP_BODY_NONE, &r);
    assert_true(r.informational);
    /* Motivo vacío o ausente. */
    expect_resp("HTTP/1.1 404 \r\nContent-Length: 0\r\n\r\n", false, 404, HTTP_BODY_NONE, &r);
    expect_resp("HTTP/1.1 500\r\nContent-Length: 0\r\n\r\n", false, 500, HTTP_BODY_NONE, &r);
    assert_int_equal(r.reason.len, 0);
}

static void test_response_errors(void **state) {
    (void)state;
    expect_resp_error("FOO\r\n\r\n", HTTP_ERR_BAD_STATUS_LINE);
    expect_resp_error("HTTP/1.1 20 OK\r\n\r\n", HTTP_ERR_BAD_STATUS_LINE);
    expect_resp_error("HTTP/1.1 2000 OK\r\n\r\n", HTTP_ERR_BAD_STATUS_LINE);
    expect_resp_error("HTTP/1.1 099 X\r\n\r\n", HTTP_ERR_BAD_STATUS_LINE);
    expect_resp_error("HTTP/2.0 200 OK\r\n\r\n", HTTP_ERR_BAD_STATUS_LINE);
    expect_resp_error("HTTP/1.1 200 OK\nContent-Length: 1\r\n\r\n", HTTP_ERR_BARE_LF);
    expect_resp_error("HTTP/1.1 200 OK\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n",
                      HTTP_ERR_CL_AND_TE);
    expect_resp_error("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n",
                      HTTP_ERR_TE_UNSUPPORTED);
    expect_resp_error("HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n", HTTP_ERR_TE_HTTP10);
    expect_resp_error("HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n",
                      HTTP_ERR_BAD_CONTENT_LENGTH);
    expect_resp_error("HTTP/1.1 200 OK\r\nConnection: content-length\r\nContent-Length: 1\r\n\r\n",
                      HTTP_ERR_CONNECTION_FRAMING);
    expect_resp_error("HTTP/1.1 200 OK\r\n folded\r\n\r\n", HTTP_ERR_OBS_FOLD);
}

static void test_body_eof_detects_truncation(void **state) {
    (void)state;
    struct http_body b;
    size_t consumed;
    enum http_error err;
    http_body_init_kind(&b, HTTP_BODY_LENGTH, 10, 0);
    assert_int_equal(http_body_feed(&b, "12345", 5, &consumed, &err), HTTP_BODY_MORE);
    assert_int_equal(http_body_eof(&b), HTTP_BODY_ERROR);
    assert_int_equal(http_body_feed(&b, "67890", 5, &consumed, &err), HTTP_BODY_DONE);
    assert_int_equal(http_body_eof(&b), HTTP_BODY_DONE);

    http_body_init_kind(&b, HTTP_BODY_CHUNKED, 0, 0);
    assert_int_equal(http_body_feed(&b, "3\r\nabc\r\n", 8, &consumed, &err), HTTP_BODY_MORE);
    assert_int_equal(http_body_eof(&b), HTTP_BODY_ERROR);

    http_body_init_kind(&b, HTTP_BODY_UNTIL_CLOSE, 0, 0);
    assert_int_equal(http_body_feed(&b, "anything", 8, &consumed, &err), HTTP_BODY_MORE);
    assert_int_equal(consumed, 8);
    assert_int_equal(http_body_eof(&b), HTTP_BODY_DONE);
}

static void test_forward_response_head(void **state) {
    (void)state;
    const char *s = "HTTP/1.1 200 Fine\r\n"
                    "Content-Type: text/plain\r\n"
                    "Connection: close, X-Hop\r\n"
                    "Keep-Alive: timeout=5\r\n"
                    "X-Hop: 1\r\n"
                    "Content-Length: 3\r\n"
                    "\r\n";
    struct http_response r;
    expect_resp(s, false, 200, HTTP_BODY_LENGTH, &r);
    char out[512];
    size_t n;
    /* Cliente 1.1 que mantiene la conexión: sin cabecera Connection. */
    assert_int_equal(http_forward_response(s, &r, true, 1, out, sizeof(out), &n), 0);
    out[n] = '\0';
    assert_string_equal(out, "HTTP/1.1 200 Fine\r\n"
                             "Content-Type: text/plain\r\n"
                             "Content-Length: 3\r\n"
                             "\r\n");
    assert_int_equal(http_forward_response(s, &r, true, 0, out, sizeof(out), &n), 0);
    out[n] = '\0';
    assert_string_equal(out, "HTTP/1.1 200 Fine\r\n"
                             "Content-Type: text/plain\r\n"
                             "Content-Length: 3\r\n"
                             "Connection: keep-alive\r\n"
                             "\r\n");
    assert_int_equal(http_forward_response(s, &r, false, 1, out, sizeof(out), &n), 0);
    out[n] = '\0';
    assert_string_equal(out, "HTTP/1.1 200 Fine\r\n"
                             "Content-Type: text/plain\r\n"
                             "Content-Length: 3\r\n"
                             "Connection: close\r\n"
                             "\r\n");
    errno = 0;
    assert_int_equal(http_forward_response(s, &r, false, 1, out, 20, &n), -1);
    assert_int_equal(errno, ENOBUFS);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_host_normalization, setup, teardown),
        cmocka_unit_test_setup_teardown(test_host_rejections, setup, teardown),
        cmocka_unit_test_setup_teardown(test_simple_get, setup, teardown),
        cmocka_unit_test_setup_teardown(test_http10_without_host_uses_default_route, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_leading_empty_lines_are_skipped, setup, teardown),
        cmocka_unit_test_setup_teardown(test_absolute_form, setup, teardown),
        cmocka_unit_test_setup_teardown(test_asterisk_form_only_for_options, setup, teardown),
        cmocka_unit_test_setup_teardown(test_keep_alive_semantics, setup, teardown),
        cmocka_unit_test_setup_teardown(test_upgrade_and_connect_rejected, setup, teardown),
        cmocka_unit_test_setup_teardown(test_connection_naming_framing_rejected, setup, teardown),
        cmocka_unit_test_setup_teardown(test_expect_policy, setup, teardown),
        cmocka_unit_test_setup_teardown(test_content_length_forms, setup, teardown),
        cmocka_unit_test_setup_teardown(test_host_errors, setup, teardown),
        cmocka_unit_test_setup_teardown(test_ambiguous_framing, setup, teardown),
        cmocka_unit_test_setup_teardown(test_request_line_errors, setup, teardown),
        cmocka_unit_test_setup_teardown(test_header_syntax_errors, setup, teardown),
        cmocka_unit_test_setup_teardown(test_nul_in_header_is_rejected, setup, teardown),
        cmocka_unit_test_setup_teardown(test_request_line_limit_detected_early, setup, teardown),
        cmocka_unit_test_setup_teardown(test_head_size_limit, setup, teardown),
        cmocka_unit_test_setup_teardown(test_header_count_limit, setup, teardown),
        cmocka_unit_test_setup_teardown(test_body_limit_from_content_length, setup, teardown),
        cmocka_unit_test_setup_teardown(test_content_length_body_framing, setup, teardown),
        cmocka_unit_test_setup_teardown(test_pipelined_requests_without_body, setup, teardown),
        cmocka_unit_test_setup_teardown(test_chunked_body_framing, setup, teardown),
        cmocka_unit_test_setup_teardown(test_chunked_body_errors, setup, teardown),
        cmocka_unit_test_setup_teardown(test_chunk_size_max_digits_accepted, setup, teardown),
        cmocka_unit_test_setup_teardown(test_forward_untrusted_rewrites_headers, setup, teardown),
        cmocka_unit_test_setup_teardown(test_forward_trusted_appends_xff, setup, teardown),
        cmocka_unit_test_setup_teardown(test_forward_absolute_form_and_http10, setup, teardown),
        cmocka_unit_test_setup_teardown(test_forward_errors, setup, teardown),
        cmocka_unit_test_setup_teardown(test_response_framing, setup, teardown),
        cmocka_unit_test_setup_teardown(test_response_errors, setup, teardown),
        cmocka_unit_test_setup_teardown(test_body_eof_detects_truncation, setup, teardown),
        cmocka_unit_test_setup_teardown(test_forward_response_head, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
