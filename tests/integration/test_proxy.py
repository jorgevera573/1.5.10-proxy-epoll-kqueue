#!/usr/bin/env python3
"""Pruebas de integración extremo a extremo del proxy.

Cada prueba termina comprobando, mediante la contabilidad del propio proxy
(SIGUSR1), que no quedan conexiones, upstreams, slots de buffer_pool ni
conexiones activas de backend: no se confía solo en Valgrind/ASan.

Uso: PROXY_BIN=build/src/proxy python3 tests/integration/test_proxy.py [-v]
"""

import hashlib
import os
import select
import signal
import socket
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import (  # noqa: E402
    Backend,
    Conn,
    Proxy,
    Workdir,
    closed_port,
    free_port,
    get,
    payload,
    scaled,
)

PER_TEST_TIMEOUT = 60


def handshake_blocked(addr, wait=0.2):
    """True si un connect nuevo a `addr` no se completa en `wait` s (el núcleo
    descarta el SYN porque la cola de aceptación está llena)."""
    probe = socket.socket()
    probe.setblocking(False)
    try:
        probe.connect_ex(addr)
        _, w, _ = select.select([], [probe], [], wait)
        if not w:
            return True
        err = probe.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
        if err:
            raise RuntimeError(f"connect a {addr}: {os.strerror(err)} (se esperaba pendiente)")
        return False
    finally:
        probe.close()


def fill_accept_queue(addr, limit=512):
    """Abre conexiones sin aceptar hasta que un handshake queda pendiente.

    Devuelve las conexiones (hay que mantenerlas abiertas). Falla de forma
    explícita si no lo consigue, en vez de probar otra cosa.
    """
    fillers = []
    for _ in range(limit):
        if handshake_blocked(addr):
            return fillers
        f = socket.socket()
        f.setblocking(False)
        f.connect_ex(addr)
        _, w, _ = select.select([], [f], [], 1)
        fillers.append(f)
        if not w:  # esta ya quedó pendiente: la cola está llena
            return fillers
    for f in fillers:
        f.close()
    raise RuntimeError(f"no se pudo llenar la cola de aceptación de {addr} con {limit} conexiones")


def config(fe1, fe2, a, b, down, shutdown_ms=3000, extra="", stuck=None):
    return f"""
[server]
workers = 1
max_connections = 64
shutdown_timeout_ms = {shutdown_ms}

[limits]
max_requests_per_connection = 100

[timeouts]
client_header_ms = {int(scaled(1.5) * 1000)}
client_idle_ms = {int(scaled(2) * 1000)}
upstream_connect_ms = {int(scaled(1) * 1000)}
upstream_response_ms = {int(scaled(1.5) * 1000)}
io_idle_ms = {int(scaled(3) * 1000)}
close_ms = {int(scaled(1) * 1000)}

[[frontend]]
listen = "127.0.0.1:{fe1}"

[[frontend]]
listen = "127.0.0.1:{fe2}"
trusted_proxies = ["127.0.0.1"]

[[pool]]
name = "a"
[[pool.backend]]
address = "127.0.0.1:{a}"

[[pool]]
name = "b"
[[pool.backend]]
address = "127.0.0.1:{b}"

[[pool]]
name = "ab"
[[pool.backend]]
address = "127.0.0.1:{a}"
[[pool.backend]]
address = "127.0.0.1:{b}"

[[pool]]
name = "down"
[[pool.backend]]
address = "127.0.0.1:{down}"

[[pool]]
name = "limited"
[[pool.backend]]
address = "127.0.0.1:{a}"
max_conns = 1

[[route]]
host = "api.test"
pool = "a"

[[route]]
host = "*.test"
pool = "b"

[[route]]
host = "down.test"
pool = "down"

[[route]]
host = "limited.test"
pool = "limited"

[[pool]]
name = "stuck"
[[pool.backend]]
address = "127.0.0.1:{stuck or down}"

[[route]]
host = "stuck.test"
pool = "stuck"

[routing]
default_pool = "ab"
{extra}
"""


class Alarm:
    """Límite global por prueba: la limpieza de tearDown se ejecuta igual."""

    def __init__(self, seconds):
        self.seconds = int(scaled(seconds))

    def __enter__(self):
        def boom(signum, frame):
            raise TimeoutError(f"prueba excedió {self.seconds}s")

        self.old = signal.signal(signal.SIGALRM, boom)
        signal.alarm(self.seconds)

    def __exit__(self, *exc):
        signal.alarm(0)
        signal.signal(signal.SIGALRM, self.old)
        return False


class ProxyTest(unittest.TestCase):
    """Proxy compartido por la clase; cada prueba acaba con el proxy en reposo."""

    @classmethod
    def setUpClass(cls):
        cls.wd = Workdir()
        try:
            cls.A = cls.wd.add(Backend(cls.wd.path, "A"))
            cls.B = cls.wd.add(Backend(cls.wd.path, "B"))
            cls.fe1, cls.fe2 = free_port(), free_port()
            cls.down = closed_port()
            # Listener que nunca acepta, con la cola llena: el núcleo descarta
            # los SYN siguientes y el connect del proxy queda pendiente. El
            # tamaño efectivo de la cola depende de la plataforma (Linux y
            # macOS difieren), así que se llena por observación.
            cls.stuck = socket.socket()
            cls.stuck.bind(("127.0.0.1", 0))
            cls.stuck.listen(0)
            cls.fillers = fill_accept_queue(cls.stuck.getsockname())
            cls.proxy = cls.wd.add(
                Proxy(cls.wd.path, config(cls.fe1, cls.fe2, cls.A.port, cls.B.port, cls.down,
                                          stuck=cls.stuck.getsockname()[1]))
            )
        except BaseException:
            cls.wd.cleanup(keep_on_failure=True)
            raise

    @classmethod
    def tearDownClass(cls):
        for f in cls.fillers:
            f.close()
        cls.stuck.close()
        rc = cls.proxy.stop(timeout=scaled(15))
        out = cls.proxy.output()
        failed = rc != 0 or "slots_in_use=0" not in out.split("shutdown conns=")[-1]
        cls.wd.cleanup(keep_on_failure=failed)
        if failed:
            raise AssertionError(f"el proxy terminó mal (rc={rc}):\n{out[-2000:]}")

    def setUp(self):
        self.alarm = Alarm(PER_TEST_TIMEOUT)
        self.alarm.__enter__()

    def tearDown(self):
        try:
            self.proxy.wait_quiescent()
        finally:
            self.alarm.__exit__(None, None, None)

    # -- routing ------------------------------------------------------------

    def test_routing_exact_wildcard_default_on_each_frontend(self):
        for port in (self.fe1, self.fe2):
            self.assertEqual(get(port, host="api.test").json()["backend"], "A")
            self.assertEqual(get(port, host="www.test").json()["backend"], "B")
            self.assertEqual(get(port, host="x.y.test").json()["backend"], "B")
            # Normalización: mayúsculas, puerto y punto final.
            self.assertEqual(get(port, host="API.Test.:8080").json()["backend"], "A")
            r = get(port, host="other.org")
            self.assertEqual(r.status, 200)
            self.assertIn(r.json()["backend"], ("A", "B"))

    def test_round_robin_alternates(self):
        seen = [get(self.fe1, host="rr.example.org").json()["backend"] for _ in range(6)]
        self.assertEqual(set(seen), {"A", "B"})
        for i in range(1, 6):
            self.assertNotEqual(seen[i], seen[i - 1], seen)

    # -- cabeceras ----------------------------------------------------------

    def test_forward_headers_untrusted_client(self):
        extra = (b"X-Forwarded-For: 6.6.6.6\r\nX-Real-IP: 6.6.6.6\r\n"
                 b"X-Forwarded-Proto: https\r\nForwarded: for=6.6.6.6\r\n"
                 b"Keep-Alive: timeout=5\r\nTE: trailers\r\n"
                 b"Connection: close, X-Secret\r\nX-Secret: 1\r\nX-Keep: yes\r\n")
        c = Conn(self.fe1)
        c.send(b"GET /echo HTTP/1.1\r\nHost: api.test:8080\r\n" + extra + b"\r\n")
        doc = c.read_response().json()
        c.close()
        h = {}
        for k, v in doc["headers"]:
            h.setdefault(k.lower(), []).append(v)
        self.assertEqual(h["host"], ["api.test:8080"])
        self.assertEqual(h["x-forwarded-for"], ["127.0.0.1"])
        self.assertEqual(h["x-real-ip"], ["127.0.0.1"])
        self.assertEqual(h["x-forwarded-proto"], ["http"])
        self.assertEqual(h["x-keep"], ["yes"])
        for gone in ("forwarded", "keep-alive", "te", "x-secret", "expect"):
            self.assertNotIn(gone, h)
        self.assertEqual(h["connection"], ["close"])  # la conexión al upstream no se reutiliza

    def test_forward_headers_trusted_proxy(self):
        extra = (b"X-Forwarded-For: 203.0.113.9\r\nX-Real-IP: 203.0.113.9\r\n"
                 b"X-Forwarded-Proto: https\r\n")
        doc = get(self.fe2, extra=extra).json()
        h = {k.lower(): v for k, v in doc["headers"]}
        self.assertEqual(h["x-forwarded-for"], "203.0.113.9, 127.0.0.1")
        self.assertEqual(h["x-real-ip"], "203.0.113.9")
        self.assertEqual(h["x-forwarded-proto"], "https")

    # -- cuerpos de petición -------------------------------------------------

    def test_post_content_length_binary(self):
        body = payload(1_000_000, 11)
        c = Conn(self.fe1)
        c.send(b"POST /echo HTTP/1.1\r\nHost: api.test\r\nConnection: close\r\n"
               b"Content-Length: %d\r\n\r\n" % len(body) + body)
        doc = c.read_response().json()
        c.close()
        self.assertEqual(doc["body_len"], len(body))
        self.assertEqual(doc["body_sha256"], hashlib.sha256(body).hexdigest())

    def test_post_chunked_with_extensions_and_trailers(self):
        body = payload(300_000, 12)
        chunks = b""
        i = 0
        sizes = [1, 7, 4096, 65536, 3]
        k = 0
        while i < len(body):
            n = sizes[k % len(sizes)]
            k += 1
            part = body[i:i + n]
            i += n
            chunks += b"%x;ext=%d\r\n" % (len(part), k) + part + b"\r\n"
        chunks += b"0\r\nX-Trailer: 1\r\n\r\n"
        c = Conn(self.fe1)
        c.send(b"POST /echo HTTP/1.1\r\nHost: api.test\r\nConnection: close\r\n"
               b"Transfer-Encoding: chunked\r\n\r\n" + chunks)
        doc = c.read_response().json()
        c.close()
        self.assertEqual(doc["body_len"], len(body))
        self.assertEqual(doc["body_sha256"], hashlib.sha256(body).hexdigest())

    def test_fragmented_head_and_body(self):
        body = b"fragmented-body-\x00\r\n-binary"
        req = (b"POST /echo HTTP/1.1\r\nHost: api.test\r\nX-Frag: yes\r\n"
               b"Connection: close\r\nContent-Length: %d\r\n\r\n" % len(body) + body)
        c = Conn(self.fe1)
        for b in req:
            c.send(bytes([b]))
            time.sleep(0.001)
        doc = c.read_response().json()
        c.close()
        self.assertEqual(doc["body_sha256"], hashlib.sha256(body).hexdigest())
        self.assertIn(["X-Frag", "yes"], doc["headers"])

    # -- respuestas ----------------------------------------------------------

    def test_large_binary_responses_content_length_chunked_and_close(self):
        r = get(self.fe1, "/big?size=8000000&seed=5")
        self.assertEqual(r.status, 200)
        self.assertEqual(r.body, payload(8_000_000, 5))

        r = get(self.fe1, "/chunked?size=2000000&seed=6&chunk=1000")
        self.assertEqual(r.header("Transfer-Encoding"), "chunked")
        self.assertEqual(r.body, payload(2_000_000, 6))
        self.assertEqual(r.trailers, ["X-Trailer: ok"])

        # Delimitada por cierre: el proxy debe cerrar también con el cliente.
        c = Conn(self.fe1)
        c.send(b"GET /close?size=1000000&seed=7 HTTP/1.1\r\nHost: api.test\r\n\r\n")
        r = c.read_response()
        self.assertEqual(r.header("Connection"), "close")
        self.assertIsNone(r.header("Content-Length"))
        self.assertEqual(r.body, payload(1_000_000, 7))
        self.assertTrue(c.eof)
        c.close()

    def test_slow_client_backpressure_bounded(self):
        # Mayor que todo lo que pueden absorber los buffers del kernel en
        # loopback, para que la presión llegue de verdad hasta el upstream.
        size = 40_000_000
        c = Conn(self.fe1, timeout=10, rcvbuf=4096)
        c.send(b"GET /big?size=%d&seed=8 HTTP/1.1\r\nHost: api.test\r\n"
               b"Connection: close\r\n\r\n" % size)
        head = c.read_until(b"\r\n\r\n")
        self.assertIn(b"200", head)
        got = bytearray(c.buf)
        c.buf = b""
        # Leer despacio y comprobar a mitad que la memoria del proxy está acotada.
        checked = False
        while len(got) < size:
            data = c.sock.recv(16384)
            if not data:
                break
            got += data
            if not checked and len(got) > 1_000_000:
                time.sleep(0.3)  # cliente parado: el proxy debe dejar de leer
                acc = self.proxy.accounting()
                self.assertLessEqual(acc["slots_in_use"], 4)  # 2 cliente + 2 upstream
                self.assertEqual(acc["upstreams"], 1)
                checked = True
            else:
                time.sleep(0.0005)
        c.close()
        self.assertTrue(checked)
        self.assertEqual(bytes(got), payload(size, 8))

    def test_head_and_bodyless_responses_keep_connection(self):
        c = Conn(self.fe1)
        c.send(b"HEAD /big?size=5000 HTTP/1.1\r\nHost: api.test\r\n\r\n")
        r = c.read_response(head_request=True)
        self.assertEqual(r.status, 200)
        self.assertEqual(r.header("Content-Length"), "5000")
        for code in (204, 304):
            c.send(b"GET /status?code=%d HTTP/1.1\r\nHost: api.test\r\n\r\n" % code)
            r = c.read_response()
            self.assertEqual(r.status, code)
        # Si se hubiera esperado un cuerpo, esta petición no se procesaría.
        c.send(b"GET /echo?after=1 HTTP/1.1\r\nHost: api.test\r\nConnection: close\r\n\r\n")
        r = c.read_response()
        self.assertEqual(r.json()["target"], "/echo?after=1")
        c.close()

    def test_informational_103_is_dropped(self):
        c = Conn(self.fe1)
        c.send(b"GET /interim HTTP/1.1\r\nHost: api.test\r\nConnection: close\r\n\r\n")
        r = c.read_response()
        self.assertEqual(r.status, 200)
        self.assertEqual(r.json()["target"], "/interim")
        c.close()

    # -- keep-alive y pipelining ----------------------------------------------

    def test_keep_alive_sequential_requests(self):
        c = Conn(self.fe1)
        for i in range(3):
            c.send(b"GET /echo?i=%d HTTP/1.1\r\nHost: api.test\r\n\r\n" % i)
            r = c.read_response()
            self.assertIsNone(r.header("Connection"))
            self.assertEqual(r.json()["target"], "/echo?i=%d" % i)
        c.close()

    def test_pipelining_sequential_in_order(self):
        c = Conn(self.fe1)
        c.send(b"GET /echo?n=1 HTTP/1.1\r\nHost: api.test\r\n\r\n"
               b"GET /big?size=100000&seed=3 HTTP/1.1\r\nHost: api.test\r\n\r\n"
               b"POST /echo?n=3 HTTP/1.1\r\nHost: api.test\r\nContent-Length: 4\r\n\r\nabcd"
               b"GET /echo?n=4 HTTP/1.1\r\nHost: api.test\r\nConnection: close\r\n\r\n")
        self.assertEqual(c.read_response().json()["target"], "/echo?n=1")
        self.assertEqual(c.read_response().body, payload(100_000, 3))
        doc = c.read_response().json()
        self.assertEqual((doc["target"], doc["body_len"]), ("/echo?n=3", 4))
        r = c.read_response()
        self.assertEqual(r.json()["target"], "/echo?n=4")
        self.assertEqual(r.header("Connection"), "close")
        self.assertTrue(c.closed_by_peer())
        c.close()

    def test_http10_keep_alive(self):
        c = Conn(self.fe1)
        c.send(b"GET /echo HTTP/1.0\r\nHost: api.test\r\nConnection: keep-alive\r\n\r\n")
        r = c.read_response()
        self.assertEqual(r.header("Connection"), "keep-alive")
        c.send(b"GET /echo HTTP/1.0\r\nHost: api.test\r\n\r\n")
        r = c.read_response()
        self.assertEqual(r.header("Connection"), "close")
        self.assertTrue(c.closed_by_peer())
        c.close()

    # -- errores del upstream ------------------------------------------------

    def test_backend_down_is_502_and_counted(self):
        r = get(self.fe1, host="down.test")
        self.assertEqual(r.status, 502)
        self.assertEqual(r.header("Connection"), "close")
        # El fallo de conexión se contabiliza en el backend (distinto de 503).
        self.assertGreater(self.proxy.backend("down", f"127.0.0.1:{self.down}")["failures"], 0)

    def test_no_eligible_backend_is_503(self):
        results = {}

        def slow():
            results["slow"] = get(self.fe1, "/slow?delay_ms=800", host="limited.test").status

        t = threading.Thread(target=slow)
        t.start()
        time.sleep(0.3)
        results["second"] = get(self.fe1, host="limited.test").status
        t.join(scaled(10))
        self.assertEqual(results, {"slow": 200, "second": 503})
        self.assertEqual(get(self.fe1, host="limited.test").status, 200)

    def test_truncated_response_closes_without_second_response(self):
        c = Conn(self.fe1)
        c.send(b"GET /truncate?size=100000&seed=4 HTTP/1.1\r\nHost: api.test\r\n\r\n")
        head = c.read_until(b"\r\n\r\n")
        self.assertIn(b" 200 ", head)
        self.assertIn(b"Content-Length: 100000", head)
        rest = c.read_to_eof()  # incluye lo ya leído tras la cabecera
        c.close()
        self.assertEqual(rest, payload(100_000, 4)[:50_000])
        self.assertNotIn(b"HTTP/1.1 502", rest)

    def test_invalid_upstream_responses_are_502(self):
        self.assertEqual(get(self.fe1, "/garbage").status, 502)
        self.assertEqual(get(self.fe1, "/switch").status, 502)  # 101 no soportado
        self.assertEqual(get(self.fe1, "/hugehead").status, 502)  # cabecera > 16 KB

    def test_upstream_response_timeout_is_504(self):
        a = f"127.0.0.1:{self.A.port}"
        failures0 = self.proxy.backend("a", a)["failures"]
        t0 = time.monotonic()
        r = get(self.fe1, "/slow?delay_ms=%d" % int(scaled(4) * 1000))
        self.assertEqual(r.status, 504)
        self.assertLess(time.monotonic() - t0, scaled(3.5))
        # La conexión se estableció: vence upstream_response_ms, que no es un
        # fallo del backend (no cuenta).
        self.assertEqual(self.proxy.backend("a", a)["failures"], failures0)

    def test_upstream_connect_timeout_is_504(self):
        addr = self.stuck.getsockname()
        stuck = f"127.0.0.1:{addr[1]}"
        failures0 = self.proxy.backend("stuck", stuck)["failures"]
        # La cola sigue llena: un handshake nuevo no se completa. Así el plazo
        # que vence es el de conexión y no el de respuesta.
        self.assertTrue(handshake_blocked(addr), "la cola del listener ya no está llena")
        t0 = time.monotonic()
        r = get(self.fe1, host="stuck.test")
        elapsed = time.monotonic() - t0
        self.assertEqual(r.status, 504)
        self.assertGreaterEqual(elapsed, scaled(0.8))  # upstream_connect_ms = 1 s
        # Un fallo de conexión cuenta exactamente uno para ese backend.
        self.assertEqual(self.proxy.backend("stuck", stuck)["failures"], failures0 + 1)
        self.assertTrue(handshake_blocked(addr), "la cola del listener dejó de estar llena")

    def test_client_disconnect_mid_request_body(self):
        c = Conn(self.fe1)
        c.send(b"POST /echo HTTP/1.1\r\nHost: api.test\r\nContent-Length: 1000000\r\n\r\n")
        c.send(payload(20_000, 1))
        time.sleep(0.2)
        c.close()  # tearDown comprueba que el proxy libera todo

    def test_client_disconnect_mid_response(self):
        c = Conn(self.fe1, rcvbuf=4096)
        c.send(b"GET /big?size=40000000 HTTP/1.1\r\nHost: api.test\r\n\r\n")
        c.read_until(b"\r\n\r\n")
        c.read_exact(100_000)
        c.close()

    def test_oversized_header_is_431(self):
        c = Conn(self.fe1)
        c.send(b"GET / HTTP/1.1\r\nHost: api.test\r\nX-Big: " + b"a" * 20000 + b"\r\n\r\n")
        r = c.read_response()
        self.assertEqual(r.status, 431)
        c.close()

    def test_early_upstream_response_closes_client(self):
        body = payload(2_000_000, 9)
        c = Conn(self.fe1)
        c.send(b"POST /early HTTP/1.1\r\nHost: api.test\r\nContent-Length: %d\r\n\r\n" % len(body))
        try:
            c.send(body)
        except (BrokenPipeError, ConnectionResetError):
            pass
        r = c.read_response()
        self.assertEqual(r.status, 413)
        self.assertTrue(c.closed_by_peer())
        c.close()

    # -- rechazos del proxy ----------------------------------------------------

    def assert_rejected(self, raw, status):
        before = self.proxy.accounting()["upstream_reusable"]
        c = Conn(self.fe1)
        c.send(raw)
        r = c.read_response()
        self.assertEqual(r.status, status, raw)
        self.assertEqual(r.header("Connection"), "close")
        self.assertTrue(c.closed_by_peer())
        c.close()
        return before

    def test_upgrade_and_connect_rejected(self):
        a0 = self.proxy.accounting()["requests"]
        self.assert_rejected(b"GET /ws HTTP/1.1\r\nHost: api.test\r\nConnection: Upgrade\r\n"
                             b"Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
                             b"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n", 501)
        self.assert_rejected(b"CONNECT api.test:443 HTTP/1.1\r\nHost: api.test:443\r\n\r\n", 501)
        # Ninguna llegó a un backend.
        acc = self.proxy.accounting()
        self.assertEqual(acc["requests"], a0)

    def test_ambiguous_and_invalid_requests_rejected(self):
        self.assert_rejected(b"POST /echo HTTP/1.1\r\nHost: api.test\r\nContent-Length: 3\r\n"
                             b"Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 400)
        self.assert_rejected(b"POST /echo HTTP/1.1\r\nHost: api.test\r\nContent-Length: 3\r\n"
                             b"Connection: content-length\r\n\r\nabc", 400)
        self.assert_rejected(b"GET / HTTP/1.1\r\nHost: api.test\r\nHost: www.test\r\n\r\n", 400)
        self.assert_rejected(b"GET / HTTP/1.1\r\n\r\n", 400)
        self.assert_rejected(b"GET / HTTP/1.1\r\nHost: api.test\r\nTransfer-Encoding: gzip\r\n\r\n",
                             501)

    # -- Expect: 100-continue --------------------------------------------------

    def test_expect_100_continue(self):
        body = payload(50_000, 13)
        c = Conn(self.fe1)
        c.send(b"POST /echo HTTP/1.1\r\nHost: api.test\r\nConnection: close\r\n"
               b"Expect: 100-continue\r\nContent-Length: %d\r\n\r\n" % len(body))
        interim = c.read_response()  # sin enviar el cuerpo: no debe bloquearse
        self.assertEqual(interim.status, 100)
        c.send(body)
        r = c.read_response()
        c.close()
        doc = r.json()
        self.assertEqual(doc["body_sha256"], hashlib.sha256(body).hexdigest())
        self.assertNotIn("expect", [k.lower() for k, _ in doc["headers"]])

    def test_expect_other_value_is_417_without_waiting_for_body(self):
        c = Conn(self.fe1)
        c.send(b"POST /echo HTTP/1.1\r\nHost: api.test\r\nExpect: something\r\n"
               b"Content-Length: 10\r\n\r\n")
        r = c.read_response()
        self.assertEqual(r.status, 417)
        c.close()

    def test_expect_ignored_for_http10(self):
        c = Conn(self.fe1)
        c.send(b"POST /echo HTTP/1.0\r\nHost: api.test\r\nExpect: 100-continue\r\n"
               b"Content-Length: 3\r\n\r\nabc")
        r = c.read_response()
        self.assertEqual(r.status, 200)  # sin 100 intermedio para HTTP/1.0
        self.assertEqual(r.json()["body_len"], 3)
        c.close()

    # -- plazos del cliente -------------------------------------------------

    def test_client_header_timeout_is_408(self):
        c = Conn(self.fe1, timeout=10)
        c.send(b"GET /echo HTTP/1.1\r\nHost: api")
        t0 = time.monotonic()
        r = c.read_response()
        self.assertEqual(r.status, 408)
        self.assertGreaterEqual(time.monotonic() - t0, scaled(1.0))
        c.close()

    def test_client_body_timeout_is_408(self):
        # Cuerpo anunciado de 1000 bytes, solo llegan 10: el upstream ya está
        # conectado y esperando, así que el lento es el cliente -> 408.
        c = Conn(self.fe1, timeout=15)
        c.send(b"POST /echo HTTP/1.1\r\nHost: api.test\r\nContent-Length: 1000\r\n\r\n" + b"x" * 10)
        t0 = time.monotonic()
        r = c.read_response()
        self.assertEqual(r.status, 408)
        self.assertGreaterEqual(time.monotonic() - t0, scaled(2.5))  # io_idle_ms = 3 s
        self.assertEqual(r.header("Connection"), "close")
        self.assertTrue(c.closed_by_peer())
        c.close()
        # tearDown: upstream liberado, backend_active = 0, slots = 0

    def test_idle_keep_alive_connection_is_closed(self):
        c = Conn(self.fe1, timeout=10)
        c.send(b"GET /echo HTTP/1.1\r\nHost: api.test\r\n\r\n")
        c.read_response()
        self.assertTrue(c.closed_by_peer(timeout=scaled(5)))
        c.close()


class StandaloneProxyTest(unittest.TestCase):
    """Escenarios que necesitan su propio proxy (sin ruta, cierre)."""

    def setUp(self):
        self.alarm = Alarm(PER_TEST_TIMEOUT)
        self.alarm.__enter__()
        self.wd = Workdir()
        self.ok = False

    def tearDown(self):
        try:
            self.wd.cleanup(keep_on_failure=not self.ok)
        finally:
            self.alarm.__exit__(None, None, None)

    def start(self, shutdown_ms=3000, text=None):
        self.A = self.wd.add(Backend(self.wd.path, "A"))
        self.fe = free_port()
        cfg = text or config(self.fe, free_port(), self.A.port, closed_port(), closed_port(),
                             shutdown_ms=shutdown_ms)
        self.proxy = self.wd.add(Proxy(self.wd.path, cfg))

    def finish(self, max_seconds):
        t0 = time.monotonic()
        try:
            rc = self.proxy.p.wait(timeout=scaled(max_seconds))
        except Exception:
            self.fail("el proxy no terminó a tiempo:\n" + self.proxy.output()[-2000:])
        out = self.proxy.output()
        self.assertEqual(rc, 0, out[-2000:])
        self.assertIn("shutdown conns=0 upstreams=0 slots_in_use=0", out)
        self.assertIn("backend_active=0", out.split("shutdown conns=")[-1])
        return time.monotonic() - t0

    def test_no_route_is_502(self):
        a = self.wd.add(Backend(self.wd.path, "A"))
        fe = free_port()
        self.proxy = self.wd.add(Proxy(self.wd.path, f"""
[[frontend]]
listen = "127.0.0.1:{fe}"
[[pool]]
name = "a"
[[pool.backend]]
address = "127.0.0.1:{a.port}"
[[route]]
host = "api.test"
pool = "a"
"""))
        self.assertEqual(get(fe, host="api.test").status, 200)
        self.assertEqual(get(fe, host="unknown.test").status, 502)
        self.assertEqual(get(fe, host="x.api.test").status, 502)  # sin wildcard
        self.proxy.wait_quiescent()
        self.proxy.signal(signal.SIGTERM)
        self.finish(10)
        self.ok = True

    def test_graceful_shutdown_completes_active_request(self):
        self.start(shutdown_ms=int(scaled(5) * 1000))
        c = Conn(self.fe, timeout=10)
        c.send(b"GET /slow?delay_ms=800 HTTP/1.1\r\nHost: api.test\r\n\r\n")
        time.sleep(0.3)
        idle = Conn(self.fe)  # conexión inactiva: se cierra de inmediato
        time.sleep(0.1)
        self.proxy.signal(signal.SIGTERM)
        self.proxy.wait_for(r"cierre ordenado", scaled(5))
        self.assertTrue(idle.closed_by_peer())
        idle.close()
        with self.assertRaises(OSError):  # ya no se aceptan conexiones
            socket.create_connection(("127.0.0.1", self.fe), timeout=1).close()
        r = c.read_response()
        self.assertEqual(r.status, 200)
        self.assertEqual(r.header("Connection"), "close")  # sin keep-alive durante el cierre
        c.close()
        self.finish(10)
        self.ok = True

    def test_shutdown_deadline_forces_close(self):
        self.start(shutdown_ms=500)
        c = Conn(self.fe, timeout=10)
        c.send(b"GET /slow?delay_ms=5000 HTTP/1.1\r\nHost: api.test\r\n\r\n")
        time.sleep(0.3)
        self.proxy.signal(signal.SIGTERM)
        elapsed = self.finish(10)
        self.assertLess(elapsed, scaled(4))
        self.assertIn("plazo de cierre agotado", self.proxy.output())
        self.assertTrue(c.closed_by_peer())
        c.close()
        self.ok = True

    def test_second_signal_forces_close(self):
        self.start(shutdown_ms=int(scaled(30) * 1000))
        c = Conn(self.fe, timeout=10)
        c.send(b"GET /slow?delay_ms=5000 HTTP/1.1\r\nHost: api.test\r\n\r\n")
        time.sleep(0.3)
        self.proxy.signal(signal.SIGTERM)
        self.proxy.wait_for(r"cierre ordenado", scaled(5))
        self.proxy.signal(signal.SIGINT)
        self.finish(10)
        self.assertIn("segundo aviso", self.proxy.output())
        c.close()
        self.ok = True


class ConfigCliTest(unittest.TestCase):
    """proxy -t: valida sin abrir sockets."""

    def run_check(self, text):
        import subprocess
        import tempfile

        with tempfile.NamedTemporaryFile("w", suffix=".toml", delete=False) as f:
            f.write(text)
            path = f.name
        try:
            p = subprocess.run([os.environ["PROXY_BIN"], "-t", "-c", path], capture_output=True,
                               text=True, timeout=scaled(10))
        finally:
            os.unlink(path)
        return p.returncode, p.stdout + p.stderr

    def test_valid_config_does_not_open_listeners(self):
        port = free_port()
        holder = socket.socket()
        holder.bind(("127.0.0.1", port))
        holder.listen()  # si -t abriera el puerto, fallaría con EADDRINUSE
        try:
            rc, out = self.run_check(config(port, free_port(), 3000, 3001, 1))
        finally:
            holder.close()
        self.assertEqual(rc, 0, out)
        self.assertIn("configuración válida", out)

    def test_invalid_configs_are_rejected_with_diagnostics(self):
        cases = [
            ("[[frontend]]\nlisten = \"localhost:80\"\n", "solo se admiten IP literales"),
            ("[server]\nworkers = 65\n", "fuera de rango"),
            ("[timeouts]\nio_idle_ms = 0\n", "io_idle_ms"),
            ("x = 1\n", "clave desconocida"),
            ("[[frontend]]\nlisten = 7777\n", "se esperaba una cadena"),
            ("not toml [", "TOML inválido"),
        ]
        for text, needle in cases:
            rc, out = self.run_check(text)
            self.assertEqual(rc, 1, text)
            self.assertIn(needle, out, text)


if __name__ == "__main__":
    unittest.main(verbosity=2)
