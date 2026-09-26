#!/usr/bin/env python3
"""Health checks activos (HTTP y TCP) y pasivos, extremo a extremo.

La coordinación se hace sobre estado observable (contabilidad del proxy y
/_state de los backends) con plazos, no con esperas fijas.
"""

import os
import socket
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import (  # noqa: E402
    Alarm,
    Backend,
    Conn,
    Proxy,
    Workdir,
    backend_ctl,
    free_port,
    get,
    in_background,
    join,
    scaled,
    wait_until,
)


def ms(seconds):
    return int(scaled(seconds) * 1000)


def config(fe, a, b, c, d):
    return f"""
[timeouts]
upstream_connect_ms = {ms(1)}
upstream_response_ms = {ms(10)}
close_ms = 1000

[[frontend]]
listen = "127.0.0.1:{fe}"

# HTTP: A y B, round robin.
[[pool]]
name = "hc"
[pool.health]
type = "http"
path = "/health"
interval_ms = {ms(0.2)}
timeout_ms = {ms(0.15)}
fall = 2
rise = 2
passive_fall = 0
[[pool.backend]]
address = "127.0.0.1:{a}"
[[pool.backend]]
address = "127.0.0.1:{b}"

# TCP: un único backend C (se detiene y se vuelve a arrancar).
[[pool]]
name = "tcp1"
[pool.health]
type = "tcp"
interval_ms = {ms(0.2)}
timeout_ms = {ms(0.15)}
fall = 2
rise = 2
[[pool.backend]]
address = "127.0.0.1:{c}"

# Pasivo: sondas lentas (fall alto) para que actúe antes el pasivo.
[[pool]]
name = "pas"
[pool.health]
type = "tcp"
interval_ms = {ms(2)}
timeout_ms = {ms(0.5)}
fall = 3
rise = 1
passive_fall = 1
[[pool.backend]]
address = "127.0.0.1:{d}"
[[pool.backend]]
address = "127.0.0.1:{a}"

# Saturación local: max_conns = 1.
[[pool]]
name = "sat"
[pool.health]
type = "tcp"
interval_ms = {ms(0.2)}
timeout_ms = {ms(0.15)}
passive_fall = 1
[[pool.backend]]
address = "127.0.0.1:{a}"
max_conns = 1

[[route]]
host = "hc.test"
pool = "hc"
[[route]]
host = "tcp1.test"
pool = "tcp1"
[[route]]
host = "pas.test"
pool = "pas"
[[route]]
host = "sat.test"
pool = "sat"
"""


class HealthTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.wd = Workdir()
        try:
            cls.A = cls.wd.add(Backend(cls.wd.path, "A"))
            cls.B = cls.wd.add(Backend(cls.wd.path, "B"))
            cls.C = cls.wd.add(Backend(cls.wd.path, "C"))
            cls.D = cls.wd.add(Backend(cls.wd.path, "D"))
            cls.fe = free_port()
            cls.proxy = cls.wd.add(Proxy(cls.wd.path, config(
                cls.fe, cls.A.port, cls.B.port, cls.C.port, cls.D.port)))
        except BaseException:
            cls.wd.cleanup(keep_on_failure=True)
            raise

    @classmethod
    def tearDownClass(cls):
        rc = cls.proxy.stop(timeout=scaled(15))
        failed = rc != 0
        out = cls.proxy.output()
        cls.wd.cleanup(keep_on_failure=failed)
        if failed:
            raise AssertionError(f"el proxy terminó mal (rc={rc}):\n{out[-2000:]}")

    def setUp(self):
        self.alarm = Alarm(90)
        self.alarm.__enter__()

    def tearDown(self):
        try:
            for b in (self.A, self.B):
                if b.p.poll() is None:
                    backend_ctl(b.port, "/_set?health=200&health_delay_ms=0")
                    backend_ctl(b.port, "/release?key=h")
            self.proxy.wait_quiescent()
        finally:
            self.alarm.__exit__(None, None, None)

    def health(self, pool, backend):
        return self.proxy.backend(pool, f"127.0.0.1:{backend.port}")["health"]

    def wait_health(self, pool, backend, state, timeout=10):
        return wait_until(lambda: self.health(pool, backend) == state, timeout,
                          f"{pool}/{backend.name} en {state}")

    def restart(self, name, attr):
        old = getattr(type(self), attr)
        port = old.port
        new = self.wd.add(Backend(self.wd.path, name, port=port))
        setattr(type(self), attr, new)
        return new

    def test_http_backend_down_and_recovery(self):
        self.wait_health("hc", self.A, "up")
        backend_ctl(self.A.port, "/_set?health=500")
        self.wait_health("hc", self.A, "down-active")
        self.assertIn("CAÍDO (activo: estado HTTP 500)", self.proxy.output())
        # Excluido: todo va a B.
        seen = {get(self.fe, host="hc.test").header("X-Backend") for _ in range(6)}
        self.assertEqual(seen, {"B"})
        backend_ctl(self.A.port, "/_set?health=200")
        self.wait_health("hc", self.A, "up")
        seen = [get(self.fe, host="hc.test").header("X-Backend") for _ in range(4)]
        self.assertEqual(sorted(seen), ["A", "A", "B", "B"])

    def test_tcp_crash_all_down_503_and_recovery(self):
        self.wait_health("tcp1", self.C, "up")
        self.assertEqual(get(self.fe, host="tcp1.test").status, 200)
        self.C.stop()
        self.wait_health("tcp1", self.C, "down-active")
        # Todos los backends del pool caídos: 503 (no 502).
        r = get(self.fe, host="tcp1.test")
        self.assertEqual(r.status, 503)
        c2 = self.restart("C", "C")
        self.wait_health("tcp1", c2, "up")
        self.assertEqual(get(self.fe, host="tcp1.test").header("X-Backend"), "C")

    def test_passive_exclusion_then_recovery_by_probes(self):
        self.wait_health("pas", self.D, "up")
        self.D.stop()
        # Primer fallo real de conexión (ECONNREFUSED) -> excluido en pasivo.
        statuses = []
        for _ in range(2):
            r = get(self.fe, host="pas.test")
            statuses.append(r.status)
            if r.status == 502:
                break
        self.assertIn(502, statuses)
        self.assertEqual(self.health("pas", self.D), "down-passive")
        self.assertIn("CAÍDO (pasivo: fallo de conexión)", self.proxy.output())
        seen = {get(self.fe, host="pas.test").header("X-Backend") for _ in range(4)}
        self.assertEqual(seen, {"A"})
        d2 = self.restart("D", "D")
        self.wait_health("pas", d2, "up", timeout=15)  # la sonda (rise = 1) lo readmite

    def test_no_exclusion_for_saturation_or_client_errors(self):
        self.wait_health("sat", self.A, "up")
        t, box = in_background(lambda: get(self.fe, "/hold?key=h", host="sat.test"))
        wait_until(lambda: self.proxy.backend("sat", f"127.0.0.1:{self.A.port}")["active"] == 1,
                   10, "petición retenida")
        self.assertEqual(get(self.fe, host="sat.test").status, 503)  # saturación local
        # Errores del cliente: petición inválida y cancelación con RST.
        c = Conn(self.fe)
        c.send(b"GET / HTTP/1.1\r\nHost: sat.test\r\nContent-Length: 1\r\n"
               b"Transfer-Encoding: chunked\r\n\r\n")
        self.assertEqual(c.read_response().status, 400)
        c.close()
        backend_ctl(self.A.port, "/release?key=h")
        self.assertEqual(join(t, box).status, 200)
        c = Conn(self.fe)
        c.send(b"POST /echo HTTP/1.1\r\nHost: sat.test\r\nContent-Length: 100000\r\n\r\nabc")
        wait_until(lambda: self.proxy.backend("sat", f"127.0.0.1:{self.A.port}")["active"] == 1,
                   10, "petición en curso")
        c.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        c.close()
        wait_until(lambda: self.proxy.backend("sat", f"127.0.0.1:{self.A.port}")["active"] == 0,
                   10, "cancelada")
        st = self.proxy.backend("sat", f"127.0.0.1:{self.A.port}")
        self.assertEqual((st["health"], st["failures"]), ("up", 0))


if __name__ == "__main__":
    unittest.main(verbosity=2)
