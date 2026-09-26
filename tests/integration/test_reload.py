#!/usr/bin/env python3
"""Recarga de configuración con SIGHUP.

Cada prueba usa su propio proxy (la recarga cambia su estado). La
sincronización es observable: líneas del log ("recarga aplicada/rechazada"),
contabilidad del proxy y /_state de los backends, siempre con plazo.
"""

import os
import signal
import socket
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import (  # noqa: E402
    Alarm,
    Backend,
    Conn,
    Proxy,
    Workdir,
    backend_ctl,
    backend_state,
    free_port,
    get,
    payload,
    scaled,
    wait_until,
)


def ms(seconds):
    return int(scaled(seconds) * 1000)


def config(fe, a, b, api_pool="a", max_conns=64, extra_frontend="", health_a=""):
    return f"""
[server]
max_connections = {max_conns}
shutdown_timeout_ms = {ms(5)}
[timeouts]
io_idle_ms = {ms(20)}
upstream_response_ms = {ms(20)}
client_idle_ms = {ms(30)}
close_ms = 1000

[[frontend]]
listen = "127.0.0.1:{fe}"
{extra_frontend}
[[pool]]
name = "a"
{health_a}
[[pool.backend]]
address = "127.0.0.1:{a}"

[[pool]]
name = "b"
[[pool.backend]]
address = "127.0.0.1:{b}"

[[route]]
host = "api.test"
pool = "{api_pool}"
"""


class ReloadTest(unittest.TestCase):
    def setUp(self):
        self.alarm = Alarm(120)
        self.alarm.__enter__()
        self.wd = Workdir()
        self.ok = False
        self.A = self.wd.add(Backend(self.wd.path, "A"))
        self.B = self.wd.add(Backend(self.wd.path, "B"))
        self.fe = free_port()

    def tearDown(self):
        try:
            self.wd.cleanup(keep_on_failure=not self.ok)
        finally:
            self.alarm.__exit__(None, None, None)

    def start(self, env=None, **kw):
        hooks = bool(env and "PROXY_TEST_RELOAD_DELAY_MS" in env)
        self.proxy = self.wd.add(
            Proxy(self.wd.path, config(self.fe, self.A.port, self.B.port, **kw), env=env,
                  testhooks=hooks))
        return self.proxy

    def cfg(self, **kw):
        return config(self.fe, self.A.port, self.B.port, **kw)

    def finish(self):
        """Reposo, una sola generación viva y cierre limpio."""
        acc = self.proxy.wait_quiescent()
        wait_until(lambda: self.proxy.accounting()["generations_live"] == 1, 10,
                   "solo la generación vigente")
        self.proxy.signal(signal.SIGTERM)
        rc = self.proxy.p.wait(timeout=scaled(15))
        out = self.proxy.output()
        self.assertEqual(rc, 0, out[-3000:])
        tail = out.split("shutdown conns=")[-1]
        for field in ("slots_in_use=0", "backend_active=0", "generations_live=1",
                      "upstreams=0"):
            self.assertIn(field, tail)
        self.assertIn("shutdown final generations_live=0 backends_live=0", out)
        self.ok = True
        return acc

    # ------------------------------------------------------------------------

    def test_valid_reload_changes_route_destination(self):
        self.start()
        self.assertEqual(get(self.fe, host="api.test").header("X-Backend"), "A")
        self.proxy.reload(self.cfg(api_pool="b"))
        self.assertEqual(self.proxy.generation(), 2)
        self.assertEqual(get(self.fe, host="api.test").header("X-Backend"), "B")
        self.assertEqual(self.proxy.accounting()["reloads_ok"], 1)
        self.finish()

    def test_inflight_transfer_uses_old_generation_and_keepalive_uses_new(self):
        self.start()
        size = 40_000_000
        big = Conn(self.fe, timeout=20, rcvbuf=4096)
        big.send(b"GET /big?size=%d&seed=21 HTTP/1.1\r\nHost: api.test\r\n"
                 b"Connection: close\r\n\r\n" % size)
        head = big.read_until(b"\r\n\r\n")
        self.assertIn(b"X-Backend: A", head)
        got = bytearray(big.buf)
        big.buf = b""

        ka = Conn(self.fe)
        ka.send(b"GET /echo?n=1 HTTP/1.1\r\nHost: api.test\r\n\r\n")
        self.assertEqual(ka.read_response().header("X-Backend"), "A")

        self.proxy.reload(self.cfg(api_pool="b"))
        # La transferencia retiene la generación 1: hay dos vivas.
        self.assertEqual(self.proxy.accounting()["generations_live"], 2)

        # Misma conexión keep-alive: la petición siguiente usa la nueva.
        ka.send(b"GET /echo?n=2 HTTP/1.1\r\nHost: api.test\r\n\r\n")
        self.assertEqual(ka.read_response().header("X-Backend"), "B")
        ka.close()
        # Conexión nueva: también la nueva.
        self.assertEqual(get(self.fe, host="api.test").header("X-Backend"), "B")

        while len(got) < size:
            data = big.sock.recv(65536)
            if not data:
                break
            got += data
        big.close()
        self.assertEqual(len(got), size)
        self.assertEqual(bytes(got), payload(size, 21))  # completa y del destino anterior
        self.finish()  # la generación 1 se libera al terminar la transferencia

    def test_invalid_reload_keeps_previous_configuration(self):
        self.start()
        line = self.proxy.reload("[[frontend]\nlisten = 1\n", expect="rechazada")
        self.assertIn("TOML inválido", line)
        self.assertIn("se mantiene la generación 1", line)
        line = self.proxy.reload(self.cfg(api_pool="nope"), expect="rechazada")
        self.assertIn("el pool \"nope\" no existe", line)
        acc = self.proxy.accounting()
        self.assertEqual((acc["generation"], acc["reloads_failed"]), (1, 2))
        self.assertEqual(get(self.fe, host="api.test").header("X-Backend"), "A")
        self.finish()

    def test_non_reloadable_changes_are_rejected_without_interruption(self):
        self.start()
        c = Conn(self.fe)
        c.send(b"GET /echo HTTP/1.1\r\nHost: api.test\r\n\r\n")
        c.read_response()
        new_port = free_port()
        cfg = self.cfg().replace(f"127.0.0.1:{self.fe}", f"127.0.0.1:{new_port}")
        line = self.proxy.reload(cfg, expect="rechazada")
        self.assertIn(f"127.0.0.1:{self.fe} -> 127.0.0.1:{new_port}", line)
        self.assertIn("no son recargables", line)
        with self.assertRaises(OSError):
            socket.create_connection(("127.0.0.1", new_port), timeout=1).close()
        line = self.proxy.reload(self.cfg(max_conns=65), expect="rechazada")
        self.assertIn("max_connections 64 -> 65", line)
        line = self.proxy.reload(
            self.cfg(extra_frontend=f'[[frontend]]\nlisten = "127.0.0.1:{free_port()}"\n'),
            expect="rechazada")
        self.assertIn("número de [[frontend]] 1 -> 2", line)
        line = self.proxy.reload(self.cfg().replace("[server]", "[server]\nworkers = 2"),
                                 expect="rechazada")
        self.assertIn("[server].workers 1 -> 2", line)
        # Sin interrupción: la conexión keep-alive previa y las nuevas siguen.
        c.send(b"GET /echo HTTP/1.1\r\nHost: api.test\r\n\r\n")
        self.assertEqual(c.read_response().status, 200)
        c.close()
        self.assertEqual(self.proxy.generation(), 1)
        self.finish()

    def test_repeated_signals_are_coalesced_and_all_generations_released(self):
        self.start(env={"PROXY_TEST_RELOAD_DELAY_MS": str(ms(1.5))})
        self.proxy.write_config(self.cfg(api_pool="b"))
        start = len(self.proxy.output())
        # El kernel no encola señales estándar repetidas: cada SIGHUP se envía
        # tras observar en el log que el anterior se procesó.
        self.proxy.signal(signal.SIGHUP)
        self.proxy.wait_for(r"recarga iniciada", scaled(5), start=start)
        for n in (1, 2):
            self.proxy.signal(signal.SIGHUP)
            wait_until(lambda: self.proxy.output()[start:].count("recarga pendiente") == n, 5,
                       f"{n} recargas pendientes registradas")
        # 1 en curso + las 2 pendientes agrupadas en una: generación 3.
        wait_until(lambda: (lambda a: a["generation"] == 3 and a["reload_running"] == 0)(
            self.proxy.accounting()), 20, "dos recargas aplicadas")
        text = self.proxy.output()[start:]
        self.assertEqual(text.count("recarga aplicada"), 2)
        self.assertEqual(text.count("recarga pendiente"), 2)
        for i in range(2):
            self.proxy.reload(self.cfg(api_pool="a" if i % 2 == 0 else "b"))
        self.assertEqual(self.proxy.generation(), 5)
        self.assertEqual(get(self.fe, host="api.test").header("X-Backend"), "B")
        self.finish()

    def test_shutdown_during_reload(self):
        self.start(env={"PROXY_TEST_RELOAD_DELAY_MS": str(ms(1.5))})
        self.proxy.write_config(self.cfg(api_pool="b"))
        self.proxy.signal(signal.SIGHUP)
        self.proxy.wait_for(r"recarga iniciada", scaled(5))
        self.proxy.signal(signal.SIGTERM)
        rc = self.proxy.p.wait(timeout=scaled(15))
        out = self.proxy.output()
        self.assertEqual(rc, 0, out[-3000:])
        self.assertIn("recarga descartada: cierre en curso", out)
        self.assertNotIn("recarga aplicada", out)
        self.assertIn("generations_live=1", out.split("shutdown conns=")[-1])
        self.ok = True

    def test_late_probe_result_from_old_generation_is_ignored(self):
        health = f"""[pool.health]
type = "http"
interval_ms = {ms(30)}
timeout_ms = {ms(10)}
fall = 1
rise = 1
"""
        # La sonda de la generación 1 tardará y fallará (500).
        backend_ctl(self.A.port, f"/_set?health=500&health_delay_ms={ms(1.5)}")
        self.start(health_a=health)
        wait_until(lambda: backend_state(self.A.port)["health_inflight"] == 1, 10,
                   "sonda de la generación 1 en curso")
        # La generación 2 no tiene sondas en el pool "a".
        self.proxy.reload(self.cfg())
        acc = wait_until(lambda: (lambda a: a if a["health_stale"] >= 1 else None)(
            self.proxy.accounting()), 10, "resultado tardío recibido")
        self.assertEqual(acc["generation"], 2)
        self.assertEqual(backend_state(self.A.port)["health_served"], 1)
        # El fallo tardío no afectó a la generación 2.
        st = self.proxy.backend("a", f"127.0.0.1:{self.A.port}")
        self.assertEqual(st["health"], "up")
        self.assertNotIn("CAÍDO", self.proxy.output())
        self.assertEqual(get(self.fe, host="api.test").status, 200)
        backend_ctl(self.A.port, "/_set?health=200&health_delay_ms=0")
        self.finish()


if __name__ == "__main__":
    unittest.main(verbosity=2)
