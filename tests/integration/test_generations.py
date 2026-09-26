#!/usr/bin/env python3
"""Contabilidad de backends entre generaciones y recarga sin bloqueo.

Las peticiones largas se mantienen de forma controlada en el backend
(/hold?key=K hasta /release?key=K). La sincronización es observable: la
contabilidad del proxy por backend, /_state del backend y el log, siempre con
plazo.
"""

import os
import signal
import socket
import struct
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
    in_background,
    join,
    scaled,
    wait_until,
)


def ms(seconds):
    return int(scaled(seconds) * 1000)


def config(fe, pools, routes, default=None):
    """pools: {nombre: (algoritmo, [(puerto, max_conns), ...])}."""
    out = [f"""
[server]
shutdown_timeout_ms = {ms(5)}
# Plazo de cada fase de la recarga, escalado como los demás: debe superar
# el retardo artificial de las pruebas (3 s × TIME_SCALE).
ipc_timeout_ms = {ms(10)}
[timeouts]
io_idle_ms = {ms(30)}
upstream_response_ms = {ms(30)}
close_ms = 1000
[[frontend]]
listen = "127.0.0.1:{fe}"
"""]
    for name, (algo, backends) in pools.items():
        out.append(f'[[pool]]\nname = "{name}"\nalgorithm = "{algo}"\n')
        for port, maxc in backends:
            out.append(f'[[pool.backend]]\naddress = "127.0.0.1:{port}"\nmax_conns = {maxc}\n')
    for host, pool in routes.items():
        out.append(f'[[route]]\nhost = "{host}"\npool = "{pool}"\n')
    if default:
        out.append(f'[routing]\ndefault_pool = "{default}"\n')
    return "".join(out)


class GenerationsTest(unittest.TestCase):
    def setUp(self):
        self.alarm = Alarm(120)
        self.alarm.__enter__()
        self.wd = Workdir()
        self.ok = False
        self.A = self.wd.add(Backend(self.wd.path, "A"))
        self.B = self.wd.add(Backend(self.wd.path, "B"))
        self.fe = free_port()
        self.holds = []

    def tearDown(self):
        try:
            for b in (self.A, self.B):
                if b.p.poll() is None:
                    for key in ("h", "h1", "h2", "h3"):
                        backend_ctl(b.port, f"/release?key={key}")
            for t, box in self.holds:
                t.join(scaled(10))
            self.wd.cleanup(keep_on_failure=not self.ok)
        finally:
            self.alarm.__exit__(None, None, None)

    # -- utilidades ---------------------------------------------------------

    def start(self, text, testhooks=False, env=None):
        self.proxy = self.wd.add(Proxy(self.wd.path, text, testhooks=testhooks, env=env))
        return self.proxy

    def addr(self, b):
        return f"127.0.0.1:{b.port}"

    def active(self, pool, b):
        return self.proxy.backend(pool, self.addr(b))["active"]

    def hold(self, host, key="h"):
        entry = in_background(lambda: get(self.fe, f"/hold?key={key}", host=host))
        self.holds.append(entry)
        return entry

    def wait_held(self, b, n, key="h"):
        wait_until(lambda: backend_state(b.port)["held"].get(key, 0) == n, 10,
                   f"{n} retenidas en {b.name} ({key})")

    def release(self, b, key="h"):
        backend_ctl(b.port, f"/release?key={key}")

    def finish(self):
        self.proxy.wait_quiescent()
        wait_until(lambda: self.proxy.accounting()["generations_live"] == 1, 10,
                   "solo la generación vigente")
        self.proxy.signal(signal.SIGTERM)
        rc = self.proxy.p.wait(timeout=scaled(15))
        out = self.proxy.output()
        self.assertEqual(rc, 0, out[-3000:])
        self.assertIn("shutdown final generations_live=0 backends_live=0", out)
        self.assertRegex(out, r"shutdown conns=0 upstreams=0 slots_in_use=0 \S+ backend_active=0")
        self.ok = True

    # -- contabilidad entre generaciones ---------------------------------------

    def test_max_conns_slot_stays_taken_after_reload(self):
        pools = {"a": ("round_robin", [(self.A.port, 1)])}
        self.start(config(self.fe, pools, {"a.test": "a"}))
        t, box = self.hold("a.test")
        self.wait_held(self.A, 1)
        # Recarga con otra ruta añadida; "a" y su backend son el mismo lógico.
        self.proxy.reload(config(self.fe, dict(pools, b=("round_robin", [(self.B.port, 0)])),
                                 {"a.test": "a", "b.test": "b"}))
        self.assertEqual(self.proxy.generation(), 2)
        self.assertEqual(self.active("a", self.A), 1)  # la de la generación 1
        self.assertEqual(get(self.fe, host="a.test").status, 503)  # plaza ocupada
        self.release(self.A)
        self.assertEqual(join(t, box).status, 200)
        wait_until(lambda: self.active("a", self.A) == 0, 10, "plaza liberada")
        self.assertEqual(get(self.fe, host="a.test").status, 200)
        self.finish()

    def test_least_conn_sees_connection_from_previous_generation(self):
        pools = {"lc": ("least_conn", [(self.A.port, 0), (self.B.port, 0)])}
        self.start(config(self.fe, pools, {}, default="lc"))
        t, box = self.hold("lc.test")
        busy = wait_until(lambda: [b for b in (self.A, self.B)
                                   if backend_state(b.port)["held"].get("h", 0) == 1], 10,
                          "una retenida")[0]
        other = self.B if busy is self.A else self.A
        self.proxy.reload(config(self.fe, pools, {"x.test": "lc"}, default="lc"))
        self.assertEqual(self.active("lc", busy), 1)
        # La generación 2 cuenta la conexión de la 1: todo va al otro.
        for _ in range(4):
            self.assertEqual(get(self.fe, host="lc.test").header("X-Backend"), other.name)
        self.release(busy)
        self.assertEqual(join(t, box).header("X-Backend"), busy.name)
        self.finish()

    def test_reducing_max_conns_neither_cuts_nor_overcommits(self):
        self.start(config(self.fe, {"a": ("round_robin", [(self.A.port, 3)])}, {}, default="a"))
        held = {}
        for key in ("h1", "h2", "h3"):
            held[key] = self.hold("a.test", key)
            self.wait_held(self.A, 1, key)
        self.proxy.reload(config(self.fe, {"a": ("round_robin", [(self.A.port, 1)])}, {},
                                 default="a"))
        # Límite nuevo 1 con 3 activas: ninguna se corta y ninguna nueva entra
        # mientras active >= 1.
        for key, expected_active in (("h1", 2), ("h2", 1)):
            self.assertEqual(get(self.fe).status, 503)
            self.release(self.A, key)
            t, box = held[key]
            self.assertEqual(join(t, box).status, 200)  # terminó completa
            wait_until(lambda n=expected_active: self.active("a", self.A) == n, 10,
                       f"{expected_active} activas")
        self.assertEqual(get(self.fe).status, 503)  # 1 activa, límite 1
        self.release(self.A, "h3")
        t, box = held["h3"]
        self.assertEqual(join(t, box).status, 200)
        wait_until(lambda: self.active("a", self.A) == 0, 10, "activas a 0")
        self.assertEqual(get(self.fe).status, 200)
        self.finish()

    def test_remove_and_reintroduce_backend_with_active_request(self):
        self.start(config(self.fe, {"p": ("round_robin", [(self.A.port, 0)])}, {}, default="p"))
        t, box = self.hold("p.test")
        self.wait_held(self.A, 1)
        live = self.proxy.accounting()["backends_live"]
        # Generación 2: A desaparece del pool (sigue vivo por la conexión).
        self.proxy.reload(config(self.fe, {"p": ("round_robin", [(self.B.port, 0)])}, {},
                                 default="p"))
        self.assertEqual(get(self.fe).header("X-Backend"), "B")
        acc = self.proxy.accounting()
        self.assertEqual(acc["backends_live"], live + 1)  # A (retenido) + B
        self.assertEqual(acc["generations_live"], 2)
        # Generación 3: A vuelve con max_conns = 1: misma entrada, activa = 1.
        self.proxy.reload(config(self.fe, {"p": ("round_robin", [(self.A.port, 1)])}, {},
                                 default="p"))
        self.assertEqual(self.active("p", self.A), 1)
        self.assertEqual(get(self.fe).status, 503)
        self.release(self.A)
        self.assertEqual(join(t, box).status, 200)
        wait_until(lambda: self.active("p", self.A) == 0, 10, "liberada")
        self.assertEqual(get(self.fe).header("X-Backend"), "A")
        self.finish()

    def test_cancel_after_reload_releases_old_generation_counter(self):
        self.start(config(self.fe, {"a": ("round_robin", [(self.A.port, 1)])}, {}, default="a"))
        c = Conn(self.fe)
        c.send(b"GET /hold?key=h HTTP/1.1\r\nHost: a.test\r\n\r\n")
        self.wait_held(self.A, 1)
        self.proxy.reload(config(self.fe, {"a": ("round_robin", [(self.A.port, 1)])},
                                 {"z.test": "a"}, default="a"))
        self.assertEqual(self.active("a", self.A), 1)
        c.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        c.close()  # RST: se aborta y se descuenta en la generación 1
        wait_until(lambda: self.active("a", self.A) == 0, 10, "descontada")
        self.assertEqual(get(self.fe).status, 200)
        self.finish()

    def test_shutdown_with_requests_from_several_generations(self):
        text = config(self.fe, {"a": ("round_robin", [(self.A.port, 0)])}, {}, default="a")
        self.start(text.replace(f"shutdown_timeout_ms = {ms(5)}", "shutdown_timeout_ms = 300"))
        for gen in range(3):
            self.hold("a.test")
            self.wait_held(self.A, gen + 1)
            if gen < 2:
                self.proxy.reload(text.replace(f"shutdown_timeout_ms = {ms(5)}",
                                               "shutdown_timeout_ms = 300")
                                  + f'[[route]]\nhost = "g{gen}.test"\npool = "a"\n')
        acc = self.proxy.accounting()
        self.assertEqual((acc["generations_live"], acc["backend_active"]), (3, 3))
        self.proxy.signal(signal.SIGTERM)  # plazo 300 ms: cierre forzado
        rc = self.proxy.p.wait(timeout=scaled(15))
        out = self.proxy.output()
        self.assertEqual(rc, 0, out[-3000:])
        self.assertIn("plazo de cierre agotado", out)
        self.assertIn("shutdown final generations_live=0 backends_live=0", out)
        self.assertIn("backend_active=0", out.split("shutdown conns=")[-1])
        self.release(self.A)
        self.ok = True

    # -- recarga sin bloqueo -----------------------------------------------------

    def test_slow_reload_does_not_block_serving(self):
        delay = scaled(3)
        pools = {"a": ("round_robin", [(self.A.port, 0)]), "b": ("round_robin", [(self.B.port, 0)])}
        self.start(config(self.fe, pools, {"api.test": "a"}), testhooks=True,
                   env={"PROXY_TEST_RELOAD_DELAY_MS": str(int(delay * 1000))})
        self.proxy.wait_for(r"VARIANTE DE PRUEBAS", scaled(5))
        self.proxy.write_config(config(self.fe, pools, {"api.test": "b"}))
        start = len(self.proxy.output())
        t0 = time.monotonic()
        self.proxy.signal(signal.SIGHUP)
        self.proxy.wait_for(r"recarga iniciada", scaled(5), start=start)
        served = 0
        worst = 0.0
        # Mientras el hilo de recarga duerme, el bucle atiende con la vigente.
        while time.monotonic() - t0 < delay * 0.6:
            r0 = time.monotonic()
            r = get(self.fe, host="api.test")
            worst = max(worst, time.monotonic() - r0)
            self.assertEqual(r.header("X-Backend"), "A")
            served += 1
        acc = self.proxy.accounting()
        self.assertEqual((acc["reload_running"], acc["generation"]), (1, 1))
        self.assertGreaterEqual(served, 5)
        self.assertLess(worst, scaled(0.5))
        self.proxy.wait_for(r"recarga aplicada: generación 2", scaled(10), start=start)
        self.assertEqual(get(self.fe, host="api.test").header("X-Backend"), "B")
        self.finish()

    def test_shutdown_during_slow_reload_keeps_loop_responsive(self):
        delay = scaled(2)
        pools = {"a": ("round_robin", [(self.A.port, 0)])}
        self.start(config(self.fe, pools, {}, default="a"), testhooks=True,
                   env={"PROXY_TEST_RELOAD_DELAY_MS": str(int(delay * 1000))})
        start = len(self.proxy.output())
        self.proxy.signal(signal.SIGHUP)
        self.proxy.wait_for(r"recarga iniciada", scaled(5), start=start)
        self.proxy.signal(signal.SIGTERM)
        self.proxy.wait_for(r"cierre ordenado", scaled(5), start=start)
        # El bucle no está bloqueado en pthread_join: sigue atendiendo señales.
        acc = self.proxy.accounting()
        self.assertEqual((acc["draining"], acc["reload_running"]), (1, 1))
        rc = self.proxy.p.wait(timeout=scaled(15))
        out = self.proxy.output()
        self.assertEqual(rc, 0, out[-3000:])
        self.assertIn("recarga descartada: cierre en curso", out)
        self.assertNotIn("esperando al hilo de recarga", out)
        self.assertIn("shutdown final generations_live=0 backends_live=0", out)
        self.ok = True

    def test_production_binary_has_no_test_hooks(self):
        hooks = (b"PROXY_TEST_RELOAD_DELAY_MS", b"PROXY_TEST_HOLD_COMMIT")
        with open(os.environ["PROXY_BIN"], "rb") as f:
            data = f.read()
            for h in hooks:
                self.assertNotIn(h, data)
        with open(os.environ["PROXY_TEST_BIN"], "rb") as f:
            data = f.read()
            for h in hooks:
                self.assertIn(h, data)
        pools = {"a": ("round_robin", [(self.A.port, 0)])}
        # Con los dos ganchos en el entorno, el ejecutable de producción
        # recarga a la generación 2 sin retardo y sin retener la activación.
        self.start(config(self.fe, pools, {}, default="a"),
                   env={"PROXY_TEST_RELOAD_DELAY_MS": "8000", "PROXY_TEST_HOLD_COMMIT": "0:2"})
        self.assertNotIn("VARIANTE DE PRUEBAS", self.proxy.output())
        t0 = time.monotonic()
        self.proxy.reload(config(self.fe, pools, {"x.test": "a"}, default="a"))
        self.assertLess(time.monotonic() - t0, scaled(4))  # la variable no tiene efecto
        self.finish()


if __name__ == "__main__":
    unittest.main(verbosity=2)
