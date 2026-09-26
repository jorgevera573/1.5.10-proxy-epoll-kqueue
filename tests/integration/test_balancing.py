#!/usr/bin/env python3
"""Balanceo weighted y least_conn, y contabilidad de conexiones activas.

least_conn no se demuestra con peticiones secuenciales: se retienen
peticiones en el backend (/hold) y se observa la contabilidad del proxy.
"""

import os
import signal
import socket
import struct
import sys
import time
import unittest
from collections import Counter

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


def config(fe, a, b, shutdown_ms=5000):
    return f"""
[server]
shutdown_timeout_ms = {shutdown_ms}
[timeouts]
upstream_response_ms = {int(scaled(20) * 1000)}
io_idle_ms = {int(scaled(20) * 1000)}
close_ms = 1000

[[frontend]]
listen = "127.0.0.1:{fe}"

[[pool]]
name = "w"
algorithm = "weighted"
[[pool.backend]]
address = "127.0.0.1:{a}"
weight = 3
[[pool.backend]]
address = "127.0.0.1:{b}"
weight = 1

[[pool]]
name = "lc"
algorithm = "least_conn"
[[pool.backend]]
address = "127.0.0.1:{a}"
[[pool.backend]]
address = "127.0.0.1:{b}"

[[route]]
host = "w.test"
pool = "w"
[[route]]
host = "lc.test"
pool = "lc"
[routing]
default_pool = "lc"
"""


def rst_close(conn):
    """Cierre abortivo (RST): el cliente cancela."""
    conn.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    conn.close()


class BalancingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.wd = Workdir()
        try:
            cls.A = cls.wd.add(Backend(cls.wd.path, "A"))
            cls.B = cls.wd.add(Backend(cls.wd.path, "B"))
            cls.fe = free_port()
            cls.addr = {"A": f"127.0.0.1:{cls.A.port}", "B": f"127.0.0.1:{cls.B.port}"}
            cls.proxy = cls.wd.add(Proxy(cls.wd.path, config(cls.fe, cls.A.port, cls.B.port)))
        except BaseException:
            cls.wd.cleanup(keep_on_failure=True)
            raise

    @classmethod
    def tearDownClass(cls):
        rc = cls.proxy.stop(timeout=scaled(15))
        out = cls.proxy.output()
        failed = rc != 0
        cls.wd.cleanup(keep_on_failure=failed)
        if failed:
            raise AssertionError(f"el proxy terminó mal (rc={rc}):\n{out[-2000:]}")

    def setUp(self):
        self.alarm = Alarm(90)
        self.alarm.__enter__()

    def tearDown(self):
        try:
            self.proxy.wait_quiescent()
        finally:
            for b in (self.A, self.B):
                backend_ctl(b.port, "/release?key=all")
            self.alarm.__exit__(None, None, None)

    def active(self, pool):
        _, bk = self.proxy.accounting(with_backends=True)
        return {name: bk[(pool, addr)]["active"] for name, addr in self.addr.items()}

    def hold(self, key, host="lc.test"):
        """Petición retenida en segundo plano; devuelve (hilo, caja)."""
        return in_background(lambda: get(self.fe, f"/hold?key={key}", host=host))

    def test_weighted_distribution_follows_weights(self):
        seen = [get(self.fe, host="w.test").header("X-Backend") for _ in range(40)]
        self.assertEqual(Counter(seen), {"A": 30, "B": 10})
        # Intercalado (smooth WRR): nunca más de 3 A seguidas.
        run = best = 0
        for s in seen:
            run = run + 1 if s == "A" else 0
            best = max(best, run)
        self.assertLessEqual(best, 3)

    def test_least_conn_with_held_connections(self):
        t1, b1 = self.hold("all")
        busy = wait_until(lambda: [k for k, v in self.active("lc").items() if v == 1],
                          10, "una conexión activa")[0]
        other = "B" if busy == "A" else "A"
        # Con `busy` ocupado, todas las peticiones rápidas van al otro.
        for _ in range(4):
            self.assertEqual(get(self.fe, host="lc.test").header("X-Backend"), other)
        # La siguiente retenida va al menos cargado (el otro).
        t2, b2 = self.hold("all")
        wait_until(lambda: self.active("lc") == {"A": 1, "B": 1}, 10, "1 activa en cada uno")
        # Empate 1-1: la tercera va a cualquiera; después hay 2 y 1.
        t3, b3 = self.hold("all")
        acc = wait_until(lambda: (lambda a: a if sum(a.values()) == 3 else None)(self.active("lc")),
                         10, "3 activas")
        light = min(acc, key=acc.get)
        self.assertEqual(sorted(acc.values()), [1, 2])
        self.assertEqual(get(self.fe, host="lc.test").header("X-Backend"), light)
        for b in (self.A, self.B):
            backend_ctl(b.port, "/release?key=all")
        r1, r2, r3 = join(t1, b1), join(t2, b2), join(t3, b3)
        self.assertEqual(r1.header("X-Backend"), busy)
        self.assertEqual(r2.header("X-Backend"), other)
        self.assertEqual({r1.status, r2.status, r3.status}, {200})
        wait_until(lambda: self.active("lc") == {"A": 0, "B": 0}, 10, "activas a 0")

    def test_active_counter_released_when_client_cancels(self):
        c = Conn(self.fe)
        c.send(b"GET /hold?key=all HTTP/1.1\r\nHost: lc.test\r\n\r\n")
        wait_until(lambda: sum(self.active("lc").values()) == 1, 10, "conexión activa")
        rst_close(c)  # el proxy recibe ECONNRESET y aborta el intercambio
        wait_until(lambda: sum(self.active("lc").values()) == 0, 10, "activa liberada")


class ShutdownCountersTest(unittest.TestCase):
    def test_forced_close_during_shutdown_releases_counters(self):
        with Alarm(60):
            wd = Workdir()
            ok = False
            try:
                a = wd.add(Backend(wd.path, "A"))
                b = wd.add(Backend(wd.path, "B"))
                fe = free_port()
                proxy = wd.add(Proxy(wd.path, config(fe, a.port, b.port, shutdown_ms=300)))
                threads = [in_background(lambda: get(fe, "/hold?key=x", host="lc.test"))
                           for _ in range(3)]
                wait_until(lambda: proxy.accounting()["backend_active"] == 3, 10, "3 activas")
                proxy.signal(signal.SIGTERM)
                rc = proxy.p.wait(timeout=scaled(10))
                out = proxy.output()
                self.assertEqual(rc, 0, out[-2000:])
                self.assertIn("plazo de cierre agotado", out)
                tail = out.split("shutdown conns=")[-1]
                self.assertIn("backend_active=0", tail)
                self.assertIn("slots_in_use=0", tail)
                for t, box in threads:
                    t.join(scaled(10))  # el cliente ve el cierre (EOF)
                for be in (a, b):
                    backend_ctl(be.port, "/release?key=x")
                ok = True
            finally:
                wd.cleanup(keep_on_failure=not ok)


if __name__ == "__main__":
    unittest.main(verbosity=2)
