#!/usr/bin/env python3
"""Maestro y varios workers: arranque, reparto, alcance por worker de los
límites y del balanceo, recarga coordinada, supervisión, cierre, socket de
estadísticas y logging saturado.

Toda la observación es por el socket de estadísticas, /_state del backend y
el log del maestro, siempre con plazo. No se asume un reparto uniforme de
conexiones entre workers (lo decide el núcleo con SO_REUSEPORT): las
propiedades se comprueban por worker con sus propias estadísticas.
"""

import os
import select
import signal
import socket
import stat
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
    fetch_stats,
    free_port,
    get,
    scaled,
    wait_until,
)


def ms(seconds):
    return int(scaled(seconds) * 1000)


def config(fe, pools, workers=3, extra="", routes=None, default=None, fe2=None, ipc_s=5,
           shutdown_s=5):
    """pools: {nombre: (algoritmo, [(puerto, peso, max_conns), ...])}."""
    out = [f"""
[server]
workers = {workers}
shutdown_timeout_ms = {ms(shutdown_s)}
ipc_timeout_ms = {ms(ipc_s)}
restart_backoff_ms = 50
{extra}
[timeouts]
io_idle_ms = {ms(30)}
upstream_response_ms = {ms(30)}
close_ms = 1000
[[frontend]]
listen = "127.0.0.1:{fe}"
"""]
    if fe2:
        out.append(f'[[frontend]]\nlisten = "127.0.0.1:{fe2}"\n')
    for name, (algo, backends) in pools.items():
        out.append(f'[[pool]]\nname = "{name}"\nalgorithm = "{algo}"\n')
        for port, weight, maxc in backends:
            out.append(f'[[pool.backend]]\naddress = "127.0.0.1:{port}"\n'
                       f'weight = {weight}\nmax_conns = {maxc}\n')
    for host, pool in (routes or {}).items():
        out.append(f'[[route]]\nhost = "{host}"\npool = "{pool}"\n')
    out.append(f'[routing]\ndefault_pool = "{default or next(iter(pools))}"\n')
    return "".join(out)


def responded(doc):
    return [w for w in doc["workers"] if w["responded"]]


def worker_backend(w, pool, port):
    for b in w["backends"]:
        if b["pool"] == pool and b["addr"] == f"127.0.0.1:{port}":
            return b
    raise KeyError((pool, port))


def pid_is(pid, name):
    try:
        with open(f"/proc/{pid}/cmdline", "rb") as f:
            return name.encode() in f.read()
    except OSError:
        return False


def pid_alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


class Base(unittest.TestCase):
    ALARM = 120

    def setUp(self):
        self.alarm = Alarm(self.ALARM)
        self.alarm.__enter__()
        self.wd = Workdir()
        self.ok = False
        self.conns = []
        self.stopped = []  # pids que la prueba detuvo con SIGSTOP
        self.A = self.wd.add(Backend(self.wd.path, "A"))
        self.B = self.wd.add(Backend(self.wd.path, "B"))
        self.fe = free_port()

    def tearDown(self):
        try:
            # Un worker detenido por la prueba no debe quedar vivo aunque la
            # prueba falle: SIGKILL solo si sigue siendo este ejecutable.
            for pid in self.stopped:
                if pid_is(pid, "proxy"):
                    os.kill(pid, signal.SIGKILL)
            for b in (self.A, self.B):
                if b.p.poll() is None:
                    backend_ctl(b.port, "/release?key=h")
            for c in self.conns:
                c.close()
        finally:
            self.wd.cleanup(keep_on_failure=not self.ok)
            self.alarm.__exit__(None, None, None)

    def start(self, text, **kw):
        self.proxy = self.wd.add(Proxy(self.wd.path, text, **kw))
        return self.proxy

    def all_ready(self, n):
        doc = self.proxy.stats()
        ws = responded(doc)
        if doc["complete"] and len(ws) == n and all(w["state"] == "ready" for w in ws):
            return doc
        return None

    def assert_clean_exit(self):
        workers = set(self.proxy.worker_pids) | self.proxy.alive_workers()
        rc = self.proxy.stop(timeout=scaled(15))
        self.assertEqual(rc, 0, self.proxy.output()[-3000:])
        self.assertFalse(os.path.exists(self.proxy.stats_sock), "socket de estadísticas no borrado")
        wait_until(lambda: not any(pid_alive(p) for p in workers), 5, "workers terminados")
        return workers


class StartupTest(Base):
    def test_three_workers_serve_and_stop_cleanly(self):
        self.start(config(self.fe, {"a": ("round_robin", [(self.A.port, 1, 0)])}, workers=3))
        doc = wait_until(lambda: self.all_ready(3), 10, "3 workers listos")
        pids = {w["pid"] for w in doc["workers"]}
        self.assertEqual(len(pids), 3)
        self.assertNotIn(self.proxy.p.pid, pids)
        self.assertEqual({w["generation"] for w in doc["workers"]}, {1})
        self.assertEqual(stat.S_IMODE(os.stat(self.proxy.stats_sock).st_mode), 0o600)

        # Peticiones en conexiones nuevas hasta que más de un worker haya
        # atendido alguna (el núcleo reparte; no se exige uniformidad).
        served = set()
        for i in range(300):
            self.assertEqual(get(self.fe).status, 200)
            if i % 10 == 9:
                ws = responded(self.proxy.stats())
                served = {w["index"] for w in ws if w["counters"]["requests"] > 0}
                if len(served) > 1:
                    break
        self.assertGreater(len(served), 1, "un solo worker atendió 300 conexiones")
        acc = self.proxy.wait_quiescent()
        doc = self.proxy.stats()
        self.assertEqual(acc["requests"], sum(w["counters"]["requests"]
                                              for w in responded(doc)))
        self.assert_clean_exit()
        self.ok = True

    def test_workers_zero_means_one_per_cpu(self):
        self.start(config(self.fe, {"a": ("round_robin", [(self.A.port, 1, 0)])}, workers=0))
        ncpu = min(os.cpu_count() or 1, 64)
        self.proxy.wait_for(rf"proxy: listo \(pid \d+, {ncpu} workers", scaled(10))
        doc = wait_until(lambda: self.all_ready(ncpu), 10, f"{ncpu} workers listos")
        self.assertEqual(doc["master"]["workers_configured"], ncpu)
        self.assertEqual(get(self.fe).status, 200)
        self.assert_clean_exit()
        self.ok = True

    def test_routing_across_frontends_with_several_workers(self):
        fe2 = free_port()
        self.start(config(self.fe, {"a": ("round_robin", [(self.A.port, 1, 0)]),
                                    "b": ("round_robin", [(self.B.port, 1, 0)])},
                          workers=2, routes={"b.test": "b"}, default="a", fe2=fe2))
        for port in (self.fe, fe2):
            for _ in range(10):
                self.assertEqual(get(port, host="api.test").json()["backend"], "A")
                self.assertEqual(get(port, host="b.test").json()["backend"], "B")
        self.assert_clean_exit()
        self.ok = True

    def test_startup_fails_cleanly_on_busy_port(self):
        busy = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        busy.bind(("127.0.0.1", self.fe))  # sin SO_REUSEPORT: el proxy no puede compartirlo
        busy.listen(1)
        try:
            p = self.start(config(self.fe, {"a": ("round_robin", [(self.A.port, 1, 0)])},
                                  workers=3), wait_ready=False)
            rc = p.p.wait(timeout=scaled(20))
            self.assertEqual(rc, 1, p.output()[-2000:])
            self.assertIn("arranque fallido", p.output())
            self.assertEqual(p.alive_workers(), set())
            self.assertFalse(os.path.exists(p.stats_sock))
        finally:
            busy.close()
        self.ok = True


class ScopeTest(Base):
    """Límites y cursores de balanceo son de cada worker (docs §14)."""

    def test_max_conns_is_per_worker(self):
        self.start(config(self.fe, {"a": ("round_robin", [(self.A.port, 1, 1)])}, workers=2,
                          extra=""))
        wait_until(lambda: self.all_ready(2), 10, "2 workers listos")
        statuses = []

        def held():
            return backend_state(self.A.port)["held"].get("h", 0)

        # Se abren esperas una a una: cada una o queda retenida en el backend o
        # recibe 503 del worker que ya tiene su única conexión ocupada.
        for _ in range(40):
            before = held()
            c = Conn(self.fe, timeout=scaled(10))
            self.conns.append(c)
            c.send(b"GET /hold?key=h HTTP/1.1\r\nHost: api.test\r\n\r\n")

            def outcome():
                if held() > before:
                    return "held"
                r, _, _ = select.select([c.sock], [], [], 0)
                return "answered" if r else None

            res = wait_until(outcome, 10, "retenida o respondida")
            if res == "answered":
                statuses.append(c.read_response().status)
            if held() >= 2:
                break
        # Con max_conns = 1 y dos workers el backend llega a 2 simultáneas...
        self.assertEqual(held(), 2)
        # ...pero nunca más: cada worker respeta su propio límite.
        doc = self.proxy.stats()
        per_worker = [worker_backend(w, "a", self.A.port)["active"] for w in responded(doc)]
        self.assertEqual(sorted(per_worker), [1, 1])
        self.assertTrue(all(s == 503 for s in statuses), statuses)
        backend_ctl(self.A.port, "/release?key=h")
        for c in self.conns:
            c.close()
        self.proxy.wait_quiescent()
        self.ok = True

    def test_weighted_holds_within_each_worker(self):
        self.start(config(self.fe, {"a": ("weighted", [(self.A.port, 3, 0),
                                                       (self.B.port, 1, 0)])}, workers=3))
        wait_until(lambda: self.all_ready(3), 10, "3 workers listos")
        for _ in range(240):
            self.assertEqual(get(self.fe).status, 200)
        doc = self.proxy.stats()
        checked = 0
        for w in responded(doc):
            a = worker_backend(w, "a", self.A.port)["selected"]
            b = worker_backend(w, "a", self.B.port)["selected"]
            # Smooth weighted round robin propio: tras n selecciones cada
            # backend se desvía como mucho 1 de su cuota exacta.
            n = a + b
            self.assertLessEqual(abs(a - 3 * n / 4), 1, (w["index"], a, b))
            checked += n > 0
        self.assertGreater(checked, 0)
        self.assertEqual(doc["totals"]["counters"]["requests"], 240)
        self.ok = True

    def test_least_conn_balances_within_each_worker(self):
        self.start(config(self.fe, {"a": ("least_conn", [(self.A.port, 1, 0),
                                                         (self.B.port, 1, 0)])}, workers=2))
        wait_until(lambda: self.all_ready(2), 10, "2 workers listos")
        n = 8
        for _ in range(n):
            c = Conn(self.fe, timeout=scaled(10))
            self.conns.append(c)
            c.send(b"GET /hold?key=h HTTP/1.1\r\nHost: api.test\r\n\r\n")
        wait_until(lambda: backend_state(self.A.port)["held"].get("h", 0)
                   + backend_state(self.B.port)["held"].get("h", 0) == n, 10, "esperas retenidas")
        doc = self.proxy.stats()
        for w in responded(doc):
            a = worker_backend(w, "a", self.A.port)["active"]
            b = worker_backend(w, "a", self.B.port)["active"]
            # Dentro de cada worker, least_conn mantiene la diferencia en ≤ 1;
            # el total global puede diferir más (cada worker decide solo).
            self.assertLessEqual(abs(a - b), 1, (w["index"], a, b))
        backend_ctl(self.A.port, "/release?key=h")
        backend_ctl(self.B.port, "/release?key=h")
        for c in self.conns:
            self.assertEqual(c.read_response().status, 200)
            c.close()
        self.proxy.wait_quiescent()
        self.ok = True


class ReloadTest(Base):
    def gens(self):
        doc = self.proxy.stats()
        return {w["generation"] for w in responded(doc)}, doc

    def test_coordinated_valid_and_invalid_reload(self):
        base = {"a": ("round_robin", [(self.A.port, 1, 0)]),
                "b": ("round_robin", [(self.B.port, 1, 0)])}
        self.start(config(self.fe, base, workers=3, default="a"))
        wait_until(lambda: self.all_ready(3), 10, "3 workers listos")
        self.proxy.reload(config(self.fe, base, workers=3, default="b"))
        gens, doc = self.gens()
        self.assertEqual(gens, {2})
        self.assertTrue(doc["complete"])
        self.assertEqual(doc["master"]["generation"], 2)
        for _ in range(20):
            self.assertEqual(get(self.fe).json()["backend"], "B")

        # Inválida: no cambia nada en ningún worker.
        line = self.proxy.reload("[[frontend]\nroto", expect="rechazada")
        self.assertIn("se mantiene la generación 2", line)
        gens, doc = self.gens()
        self.assertEqual(gens, {2})
        self.assertEqual(doc["master"]["reloads_failed"], 1)
        # No recargable (workers): rechazada antes de preparar.
        line = self.proxy.reload(config(self.fe, base, workers=2, default="b"),
                                 expect="rechazada")
        self.assertIn("[server].workers 3 -> 2", line)
        for _ in range(10):
            self.assertEqual(get(self.fe).json()["backend"], "B")
        self.assert_clean_exit()
        self.ok = True

    def test_transfer_in_flight_survives_reload(self):
        base = {"a": ("round_robin", [(self.A.port, 1, 0)]),
                "b": ("round_robin", [(self.B.port, 1, 0)])}
        self.start(config(self.fe, base, workers=2, default="a"))
        c = Conn(self.fe, timeout=scaled(15))
        self.conns.append(c)
        c.send(b"GET /hold?key=h HTTP/1.1\r\nHost: api.test\r\nConnection: close\r\n\r\n")
        wait_until(lambda: backend_state(self.A.port)["held"].get("h", 0) == 1, 10, "retenida")
        self.proxy.reload(config(self.fe, base, workers=2, default="b"))
        self.assertEqual(get(self.fe).json()["backend"], "B")
        acc = self.proxy.accounting()
        self.assertEqual(acc["generations_live"], 2)  # la vieja sigue viva por la transferencia
        backend_ctl(self.A.port, "/release?key=h")
        r = c.read_response()
        self.assertEqual((r.status, r.json()["backend"]), (200, "A"))
        wait_until(lambda: self.proxy.accounting()["generations_live"] == 1, 10,
                   "generación vieja liberada")
        self.proxy.wait_quiescent()
        self.assert_clean_exit()
        self.ok = True

    def test_worker_killed_during_prepare_aborts_then_next_reload_applies(self):
        base = {"a": ("round_robin", [(self.A.port, 1, 0)]),
                "b": ("round_robin", [(self.B.port, 1, 0)])}
        self.start(config(self.fe, base, workers=2, default="a"), testhooks=True,
                   env={"PROXY_TEST_RELOAD_DELAY_MS": str(ms(2))})
        doc = wait_until(lambda: self.all_ready(2), 10, "2 workers listos")
        victim = doc["workers"][0]["pid"]
        start = len(self.proxy.output())
        self.proxy.write_config(config(self.fe, base, workers=2, default="b"))
        self.proxy.signal(signal.SIGHUP)
        self.proxy.wait_for(r"recarga iniciada", scaled(10), start=start)
        # El worker está preparando (retardo de prueba): se mata sin aviso.
        wait_until(lambda: self.proxy.stats()["master"]["reload_in_progress"], 5, "preparando")
        os.kill(victim, signal.SIGKILL)
        m = self.proxy.wait_for(r"recarga (aplicada|rechazada)[^\n]*", scaled(15), start=start)
        self.assertIn("rechazada", m.group(0))
        self.assertIn("se mantiene la generación 1", m.group(0))
        # Se repone con la generación vigente y ningún worker quedó con la candidata.
        doc = wait_until(lambda: self.all_ready(2), 15, "worker repuesto")
        self.assertEqual({w["generation"] for w in doc["workers"]}, {1})
        self.assertEqual(doc["master"]["worker_restarts"], 1)
        self.assertEqual(self.proxy.accounting()["generations_live"], 1)
        self.assertEqual(get(self.fe).json()["backend"], "A")
        self.assertFalse(pid_alive(victim))

        # La siguiente recarga se aplica en todos.
        self.proxy.reload(config(self.fe, base, workers=2, default="b"), timeout=20)
        gens, doc = self.gens()
        self.assertEqual(gens, {2})
        self.assertEqual(get(self.fe).json()["backend"], "B")
        self.assert_clean_exit()
        self.ok = True


    def test_worker_killed_during_commit_is_replaced_with_new_generation(self):
        """Un worker muere tras la decisión de activar y antes de confirmar.

        El gancho de proxy-testhooks PROXY_TEST_HOLD_COMMIT=1:2 hace que el
        worker 1 reciba el COMMIT de la generación 2 y no lo aplique ni lo
        confirme; el plazo de confirmación (ipc_timeout_ms = 60 s escalados)
        no vence durante la prueba, así que la retención es indefinida y la
        prueba decide el momento de la muerte.
        """
        base = {"a": ("round_robin", [(self.A.port, 1, 0)]),
                "b": ("round_robin", [(self.B.port, 1, 0)])}
        self.start(config(self.fe, base, workers=3, default="a", ipc_s=60), testhooks=True,
                   env={"PROXY_TEST_HOLD_COMMIT": "1:2"})
        doc = wait_until(lambda: self.all_ready(3), 10, "3 workers listos")
        held = doc["workers"][1]["pid"]
        others = {doc["workers"][0]["pid"], doc["workers"][2]["pid"]}
        self.assertIn("VARIANTE DE PRUEBAS: PROXY_TEST_HOLD_COMMIT para la generación 2",
                      self.proxy.output())

        start = len(self.proxy.output())
        self.proxy.write_config(config(self.fe, base, workers=3, default="b", ipc_s=60))
        self.proxy.signal(signal.SIGHUP)
        # Evento observable: el worker 1 recibió el COMMIT (el maestro ya
        # decidió) y lo retiene.
        self.proxy.wait_for(rf"pid={held} worker=1 level=WARN gen=2 VARIANTE DE PRUEBAS: "
                            r"activación de la generación 2 retenida", scaled(10), start=start)

        # Ventana de transición: el maestro está en la generación 2, los otros
        # dos confirmaron y el retenido sigue en la 1. No se anuncia éxito.
        def window():
            d = self.proxy.stats()
            gens = {w["index"]: w["generation"] for w in responded(d)}
            if (d["complete"] and d["master"]["generation"] == 2
                    and d["master"]["reload_in_progress"] and gens == {0: 2, 1: 1, 2: 2}):
                return d
            return None

        d = wait_until(window, 10, "dos confirmados y uno retenido")
        self.assertEqual(d["master"]["reload_state"], "committing")
        out = self.proxy.output()[start:]
        self.assertEqual(out.count("generación 2 activada"), 2, out[-2000:])
        self.assertNotIn("recarga aplicada", out)
        self.assertNotIn("recarga rechazada", out)
        self.assertEqual(self.proxy.stats()["master"]["reloads_ok"], 0)

        # Se mata SOLO el worker retenido (pid tomado de las estadísticas de
        # este maestro y comprobado antes).
        self.assertTrue(pid_alive(held))
        os.kill(held, signal.SIGKILL)
        m = self.proxy.wait_for(r"recarga aplicada: generación 2 [^\n]*", scaled(10), start=start)
        self.assertIn("con capacidad reducida (2 de 3 workers confirmados; 1 por reponer con la "
                      "generación 2)", m.group(0))
        r = self.proxy.wait_for(r"worker 1 repuesto \(pid (\d+), generación 2\)", scaled(15),
                                start=start)
        # La reposición llega después del anuncio (se aplaza durante la recarga).
        self.assertLess(m.start(), r.start())
        sub = int(r.group(1))

        c = self.proxy.wait_for(r"recuperación completa: generación 2 \(3 de 3 workers listos\)",
                                scaled(10), start=start)
        self.assertLess(r.start(), c.start())
        doc = wait_until(lambda: self.all_ready(3), 15, "3 workers listos tras la reposición")
        self.assertEqual({w["generation"] for w in doc["workers"]}, {2})
        self.assertEqual(doc["master"]["recovery"], {"state": "complete", "generation": 2})
        self.assertFalse(doc["master"]["degraded"])
        self.assertEqual(doc["workers"][1]["pid"], sub)
        self.assertEqual(doc["workers"][1]["restarts"], 1)
        self.assertEqual({doc["workers"][0]["pid"], doc["workers"][2]["pid"]}, others)
        self.assertEqual(doc["master"]["reloads_ok"], 1)
        self.assertFalse(pid_alive(held))

        # El sustituto atiende tráfico con el destino nuevo: se envían
        # conexiones nuevas hasta que sus propias estadísticas lo muestran.
        def substitute_served():
            for _ in range(10):
                self.assertEqual(get(self.fe).json()["backend"], "B")
            w1 = self.proxy.stats()["workers"][1]
            return w1 if worker_backend(w1, "b", self.B.port)["selected"] > 0 else None

        w1 = wait_until(substitute_served, 20, "el sustituto atiende tráfico", interval=0)
        self.assertEqual(w1["pid"], sub)
        self.assertEqual(worker_backend(w1, "a", self.A.port)["selected"], 0)

        # Otra recarga: sin estados residuales (ni gancho, ni candidata, ni
        # contadores de reposición), todos confirman.
        start = len(self.proxy.output())
        line = self.proxy.reload(config(self.fe, base, workers=3, default="a", ipc_s=60))
        self.assertIn("recarga aplicada: generación 3 (3 workers)", line)
        self.assertNotIn("retenida", self.proxy.output()[start:])
        gens, doc = self.gens()
        self.assertEqual(gens, {3})
        self.assertTrue(doc["complete"])
        for _ in range(10):
            self.assertEqual(get(self.fe).json()["backend"], "A")
        wait_until(lambda: self.proxy.accounting()["generations_live"] == 1, 10,
                   "una sola generación viva")
        self.proxy.wait_quiescent()
        self.assert_clean_exit()
        self.assertFalse(any(pid_alive(p) for p in {held, sub} | others))
        self.ok = True


class ActivationFailureTest(Base):
    """Workers que no confirman la activación: retirada con plazo, SIGKILL,
    reposición con la generación comprometida y estados de recuperación.

    Ganchos (solo proxy-testhooks): PROXY_TEST_HOLD_COMMIT y
    PROXY_TEST_FAIL_COMMIT ("<worker>:<generación>").
    """

    ALARM = 180

    def base(self):
        return {"a": ("round_robin", [(self.A.port, 1, 0)]),
                "b": ("round_robin", [(self.B.port, 1, 0)])}

    def launch(self, workers, hook, default="a", **kw):
        self.kw = dict(workers=workers, **kw)
        self.start(config(self.fe, self.base(), default=default, **self.kw), testhooks=True,
                   env=hook)
        return wait_until(lambda: self.all_ready(workers), 15, f"{workers} workers listos")

    def begin_reload(self, default="b"):
        """SIGHUP con destino nuevo; devuelve el desplazamiento del log."""
        start = len(self.proxy.output())
        self.proxy.write_config(config(self.fe, self.base(), default=default, **self.kw))
        self.proxy.signal(signal.SIGHUP)
        self.proxy.wait_for(r"activación iniciada: generación 2 ", scaled(10), start=start)
        return start

    def after(self, pattern, start, timeout=15):
        return self.proxy.wait_for(pattern, scaled(timeout), start=start)

    def expect_recovered(self, start, held, how):
        """Anuncio parcial → retirado → repuesto con gen 2 → recuperación completa."""
        a = self.after(r"recarga aplicada: generación 2 con capacidad reducida \(2 de 3 workers "
                       r"confirmados; 1 por reponer con la generación 2\)", start)
        x = self.after(rf"worker 1 \(pid {held}\) retirado \({how}\)", start)
        r = self.after(r"worker 1 repuesto \(pid (\d+), generación 2\)", start)
        c = self.after(r"recuperación completa: generación 2 \(3 de 3 workers listos\)", start)
        # Reposición solo después de recoger al retirado.
        self.assertLess(x.start(), r.start())
        self.assertLess(r.start(), c.start())
        out = self.proxy.output()[start:]
        self.assertNotIn("recarga aplicada: generación 2 (3 workers)", out)
        self.assertNotIn("recarga rechazada", out)
        sub = int(r.group(1))
        doc = wait_until(lambda: self.all_ready(3), 15, "3 workers listos")
        self.assertEqual({w["generation"] for w in doc["workers"]}, {2})
        self.assertEqual(doc["workers"][1]["pid"], sub)
        self.assertEqual(doc["workers"][1]["restarts"], 1)
        self.assertEqual(doc["master"]["recovery"], {"state": "complete", "generation": 2})
        self.assertFalse(doc["master"]["degraded"])
        self.assertEqual(doc["master"]["reload_state"], "idle")
        self.assertFalse(pid_alive(held))
        for _ in range(10):
            self.assertEqual(get(self.fe).json()["backend"], "B")
        return a, sub

    def finish_clean(self):
        line = self.proxy.reload(config(self.fe, self.base(), default="a", **self.kw))
        self.assertIn("recarga aplicada: generación 3 (3 workers)", line)
        self.assertEqual(self.gens(), {3})
        for _ in range(5):
            self.assertEqual(get(self.fe).json()["backend"], "A")
        self.proxy.wait_quiescent()
        self.assert_clean_exit()

    def gens(self):
        return {w["generation"] for w in responded(self.proxy.stats())}

    def test_commit_deadline_retires_and_replaces_with_new_generation(self):
        doc = self.launch(3, {"PROXY_TEST_HOLD_COMMIT": "1:2"}, ipc_s=2)
        held = doc["workers"][1]["pid"]
        start = self.begin_reload()
        self.after(r"activación de la generación 2 retenida", start)
        # Retenido: todavía no hay anuncio (el plazo de confirmación sigue).
        self.assertNotIn("recarga aplicada", self.proxy.output()[start:])
        m = self.after(rf"worker 1 \(pid {held}\) no confirmó la activación a tiempo: retirada "
                       r"con SIGTERM \(plazo (\d+) ms\)", start)
        # Plazo de retirada = max(shutdown viejo, nuevo) + 5000 ms.
        self.assertEqual(int(m.group(1)), ms(5) + 5000)
        self.expect_recovered(start, held, "código 0")
        self.assertNotIn("SIGKILL", self.proxy.output()[start:])
        self.finish_clean()
        self.ok = True

    def test_commit_failed_retires_without_waiting_for_deadline(self):
        # Plazo de confirmación de 60 s (escalados): las esperas de la prueba
        # son mucho menores, así que la retirada no depende del plazo.
        doc = self.launch(3, {"PROXY_TEST_FAIL_COMMIT": "1:2"}, ipc_s=60)
        held = doc["workers"][1]["pid"]
        start = self.begin_reload()
        self.after(r"activación de la generación 2 fallida", start)
        self.after(rf"worker 1 \(pid {held}\) no pudo activar la generación \(COMMIT_FAILED\): "
                   r"retirada con SIGTERM", start)
        self.expect_recovered(start, held, "código 0")
        self.assertNotIn("no confirmó la activación a tiempo", self.proxy.output()[start:])
        self.finish_clean()
        self.ok = True

    def test_worker_that_cannot_handle_sigterm_is_killed_and_replaced(self):
        doc = self.launch(3, {"PROXY_TEST_HOLD_COMMIT": "1:2"}, ipc_s=2, shutdown_s=0.5)
        held = doc["workers"][1]["pid"]
        start = self.begin_reload()
        self.after(r"activación de la generación 2 retenida", start)
        # Solo el worker de esta prueba, verificado por su línea de comandos.
        self.assertTrue(pid_is(held, "proxy"))
        self.stopped.append(held)
        os.kill(held, signal.SIGSTOP)
        m = self.after(rf"worker 1 \(pid {held}\) no confirmó la activación a tiempo: retirada "
                       r"con SIGTERM \(plazo (\d+) ms\)", start)
        retire_ms = int(m.group(1))
        self.assertEqual(retire_ms, ms(0.5) + 5000)
        k = self.after(rf"worker 1 \(pid {held}\) sigue vivo tras {retire_ms} ms de retirada: "
                       r"SIGKILL", start, timeout=20)
        self.assertLess(m.start(), k.start())
        self.expect_recovered(start, held, "señal 9")
        self.assertEqual(self.proxy.output()[start:].count("SIGKILL"), 1)
        self.finish_clean()
        self.ok = True

    def test_max_restarts_exhausted_after_partial_activation_is_explicit(self):
        doc = self.launch(3, {"PROXY_TEST_HOLD_COMMIT": "1:2"}, ipc_s=60,
                          extra="max_restarts = 0")
        held = doc["workers"][1]["pid"]
        start = self.begin_reload()
        self.after(r"activación de la generación 2 retenida", start)
        os.kill(held, signal.SIGKILL)
        self.after(r"recarga aplicada: generación 2 con capacidad reducida \(2 de 3 workers "
                   r"confirmados; 1 por reponer con la generación 2\)", start)
        self.after(r"recuperación agotada: el worker 1 no se repone \(max_restarts\); la "
                   r"generación 2 sigue activa con 2 de 3 workers", start)
        out = self.proxy.output()[start:]
        self.assertNotIn("repuesto", out)
        self.assertNotIn("recuperación completa", out)
        doc = self.proxy.stats()
        self.assertEqual(doc["master"]["generation"], 2)  # sin rollback
        self.assertEqual(doc["master"]["recovery"], {"state": "exhausted", "generation": 2})
        self.assertTrue(doc["master"]["degraded"])
        self.assertEqual(doc["master"]["workers_ready"], 2)
        self.assertEqual(doc["workers"][1]["state"], "failed")
        self.assertEqual(doc["missing_workers"], [1])
        self.assertFalse(doc["complete"])
        self.assertEqual({w["generation"] for w in responded(doc)}, {2})
        for _ in range(10):
            self.assertEqual(get(self.fe).json()["backend"], "B")
        self.assert_clean_exit()  # los dos que quedan cierran limpio: código 0
        self.ok = True

    def test_all_slots_exhausted_after_partial_activation_exits_1(self):
        doc = self.launch(1, {"PROXY_TEST_HOLD_COMMIT": "0:2"}, ipc_s=60,
                          extra="max_restarts = 0")
        held = doc["workers"][0]["pid"]
        start = self.begin_reload()
        self.after(r"activación de la generación 2 retenida", start)
        os.kill(held, signal.SIGKILL)
        rc = self.proxy.p.wait(timeout=scaled(15))
        out = self.proxy.output()[start:]
        self.assertEqual(rc, 1, out[-3000:])
        self.assertIn("recarga aplicada: generación 2 con capacidad reducida (0 de 1 workers "
                      "confirmados; 1 por reponer con la generación 2)", out)
        self.assertIn("recuperación agotada: el worker 0 no se repone", out)
        self.assertIn("todos los workers han fallado: el maestro termina", out)
        self.assertNotIn("repuesto", out)
        self.assertEqual(self.proxy.alive_workers(), set())
        self.assertFalse(os.path.exists(self.proxy.stats_sock))
        self.ok = True

    def test_master_shutdown_during_retirement(self):
        doc = self.launch(3, {"PROXY_TEST_HOLD_COMMIT": "1:2"}, ipc_s=2, shutdown_s=0.5)
        held = doc["workers"][1]["pid"]
        others = {doc["workers"][0]["pid"], doc["workers"][2]["pid"]}
        start = self.begin_reload()
        self.after(r"activación de la generación 2 retenida", start)
        self.assertTrue(pid_is(held, "proxy"))
        self.stopped.append(held)
        os.kill(held, signal.SIGSTOP)  # no podrá procesar el SIGTERM de la retirada
        self.after(rf"worker 1 \(pid {held}\) no confirmó la activación a tiempo: retirada", start)
        self.after(r"recarga aplicada: generación 2 con capacidad reducida", start)
        # Cierre global mientras el worker 1 sigue en retirada.
        sd = len(self.proxy.output())
        self.proxy.signal(signal.SIGTERM)
        self.after(rf"worker 1 \(pid {held}\) ya en retirada: termina con su plazo", sd)
        rc = self.proxy.p.wait(timeout=scaled(20))
        out = self.proxy.output()[sd:]
        # Lo mata el primero que venza: su plazo de retirada (empezó antes)
        # o el de cierre del maestro; en ambos casos un único SIGKILL.
        by_retire = f"worker 1 (pid {held}) sigue vivo tras" in out
        by_close = "workers sin terminar tras el plazo de cierre: SIGKILL a 1" in out
        self.assertTrue(by_retire != by_close, out[-3000:])
        self.assertIn(f"worker 1 (pid {held}) terminó (señal 9)", out)
        for pat in ("se repondrá", "repuesto", "arrancado"):
            self.assertNotIn(pat, out)
        self.assertEqual(rc, 70, out[-3000:])  # un worker tuvo que morir con SIGKILL
        self.assertIn("maestro: fin (código 70)", out)
        self.assertFalse(any(pid_alive(p) for p in others | {held}))
        self.assertEqual(self.proxy.alive_workers(), set())
        self.assertFalse(os.path.exists(self.proxy.stats_sock))
        self.ok = True


class SupervisionTest(Base):
    def test_killed_worker_is_restarted_and_counted(self):
        self.start(config(self.fe, {"a": ("round_robin", [(self.A.port, 1, 0)])}, workers=2))
        doc = wait_until(lambda: self.all_ready(2), 10, "2 workers listos")
        victim = doc["workers"][1]
        other = doc["workers"][0]["pid"]
        os.kill(victim["pid"], signal.SIGKILL)
        self.proxy.wait_for(rf"worker 1 \(pid {victim['pid']}\) terminó inesperadamente",
                            scaled(10))

        def replaced():
            d = self.all_ready(2)
            if d and d["workers"][1]["pid"] != victim["pid"]:
                return d
            return None

        doc = wait_until(replaced, 15, "worker repuesto")
        self.assertEqual(doc["workers"][1]["restarts"], 1)
        self.assertEqual(doc["master"]["worker_restarts"], 1)
        self.assertEqual(doc["workers"][0]["pid"], other)  # el otro worker no se tocó
        self.assertEqual(doc["workers"][0]["restarts"], 0)
        self.assertEqual(doc["workers"][1]["counters"]["requests"], 0)  # acumulados desde 0
        for _ in range(20):
            self.assertEqual(get(self.fe).status, 200)
        self.assertFalse(pid_alive(victim["pid"]))
        self.assert_clean_exit()
        self.ok = True

    def test_restart_limit_ends_master_without_children(self):
        self.start(config(self.fe, {"a": ("round_robin", [(self.A.port, 1, 0)])}, workers=1,
                          extra="max_restarts = 0"))
        doc = wait_until(lambda: self.all_ready(1), 10, "worker listo")
        victim = doc["workers"][0]["pid"]
        os.kill(victim, signal.SIGKILL)
        rc = self.proxy.p.wait(timeout=scaled(15))
        out = self.proxy.output()
        self.assertEqual(rc, 1, out[-2000:])
        self.assertIn("todos los workers han fallado", out)
        self.assertEqual(self.proxy.alive_workers(), set())
        self.assertFalse(os.path.exists(self.proxy.stats_sock))
        self.ok = True


class StatsSocketTest(Base):
    def test_foreign_file_at_stats_path_is_left_intact(self):
        path = os.path.join(self.wd.path, "ajeno.sock")
        with open(path, "w") as f:
            f.write("no tocar\n")
        text = config(self.fe, {"a": ("round_robin", [(self.A.port, 1, 0)])}, workers=2)
        text += f'\n[stats]\nsocket = "{path}"\n'
        p = self.start(text, wait_ready=False)
        rc = p.p.wait(timeout=scaled(15))
        self.assertEqual(rc, 1, p.output()[-2000:])
        with open(path) as f:
            self.assertEqual(f.read(), "no tocar\n")
        self.assertEqual(p.alive_workers(), set())
        self.ok = True

    def test_foreign_listening_socket_is_left_intact(self):
        path = os.path.join(self.wd.path, "ajeno.sock")
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(path)
        srv.listen(1)
        try:
            text = config(self.fe, {"a": ("round_robin", [(self.A.port, 1, 0)])}, workers=1)
            text += f'\n[stats]\nsocket = "{path}"\n'
            p = self.start(text, wait_ready=False)
            rc = p.p.wait(timeout=scaled(15))
            self.assertEqual(rc, 1, p.output()[-2000:])
            # El socket ajeno sigue existiendo y aceptando conexiones.
            self.assertTrue(stat.S_ISSOCK(os.lstat(path).st_mode))
            c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            c.settimeout(5)
            c.connect(path)
            c.close()
        finally:
            srv.close()
        self.ok = True

    def test_slow_clients_and_client_limit_do_not_stop_traffic(self):
        # Muchos backends para que el JSON supere el búfer del socket y un
        # cliente que no lee quede realmente pendiente.
        pools = {"a": ("round_robin", [(self.A.port, 1, 0)])}
        # Direcciones que nunca se contactan (no hay rutas a esos pools ni
        # comprobaciones de salud); puertos distintos para no repetir.
        for k in range(12):
            pools[f"x{k}"] = ("round_robin", [(40000 + 100 * k + i, 1, 0) for i in range(60)])
        text = config(self.fe, pools, workers=3, default="a")
        text += (f'\n[stats]\nsocket = "{os.path.join(self.wd.path, "s.sock")}"\n'
                 f"timeout_ms = {ms(3)}\nmax_clients = 3\n")
        p = self.start(text, wait_ready=False)
        p.stats_sock = os.path.join(self.wd.path, "s.sock")
        p.wait_for(r"proxy: listo", scaled(20))
        # El JSON es grande y válido.
        doc = fetch_stats(p.stats_sock)
        self.assertTrue(doc["complete"])
        self.assertEqual(len(doc["totals"]["backends"]), 721)
        # Mayor que el búfer de envío por defecto de un socket UNIX (~208 KiB):
        # un cliente que no lee deja al servidor con salida pendiente.
        import json
        self.assertGreater(len(json.dumps(doc, separators=(",", ":"))), 400_000)

        slow = []
        try:
            for _ in range(3):  # ocupan todas las plazas sin leer
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
                s.connect(p.stats_sock)
                slow.append(s)
            # Tráfico HTTP sin interrupción mientras tanto.
            t0 = time.monotonic()
            for _ in range(30):
                self.assertEqual(get(self.fe).status, 200)
            self.assertLess(time.monotonic() - t0, scaled(10))
            # Cliente sobrante: rechazado (cierre sin datos), no espera.
            extra = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            extra.settimeout(scaled(2))
            extra.connect(p.stats_sock)
            self.assertEqual(extra.recv(1), b"")
            extra.close()
            # Vencido el plazo, los lentos se cierran y el servicio vuelve.
            def expelled():
                try:
                    d = fetch_stats(p.stats_sock)
                except ValueError:  # aún lleno: rechazado sin datos
                    return None
                return d if d["master"]["stats_timeouts"] >= 3 else None

            doc = wait_until(expelled, 15, "clientes lentos expulsados", interval=0.3)
            self.assertGreaterEqual(doc["master"]["stats_rejected"], 1)
        finally:
            for s in slow:
                s.close()
        self.assert_clean_exit()
        self.ok = True


class LoggingTest(Base):
    def test_saturated_log_drops_without_stopping_traffic(self):
        logdir = os.path.join(self.wd.path, "logs")
        os.mkdir(logdir)
        fifo = os.path.join(logdir, "worker-0.log")
        os.mkfifo(fifo, 0o600)
        # Lector que nunca lee: el pipe se llena y el consumidor no avanza.
        reader = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
        try:
            text = config(self.fe, {"a": ("round_robin", [(self.A.port, 1, 0)])}, workers=1)
            text += f'\n[log]\ndir = "{logdir}"\nlevel = "debug"\naccess = true\n'
            self.start(text)
            # Hasta que el ring (4096) y el pipe se llenen y haya descartes.
            n = 0
            t0 = time.monotonic()
            c = None
            while n < 30000 and (n % 1000 or n == 0
                                 or self.proxy.accounting()["log_dropped"] == 0):
                n += 1
                if c is None:  # keep-alive, respetando max_requests_per_connection
                    c = Conn(self.fe, timeout=scaled(10))
                    self.conns.append(c)
                c.send(b"GET /echo HTTP/1.1\r\nHost: api.test\r\n\r\n")
                r = c.read_response()
                self.assertEqual(r.status, 200)
                if (r.header("Connection") or "").lower() == "close":
                    c.close()
                    c = None
            elapsed = time.monotonic() - t0
            if c is not None:
                c.close()
            acc = self.proxy.accounting()
            self.assertEqual(acc["requests"], n)
            self.assertGreater(acc["log_dropped"], 0)
            self.assertLess(elapsed, scaled(60))
            # Cierre acotado aunque el destino del log siga bloqueado.
            t0 = time.monotonic()
            self.assert_clean_exit()
            self.assertLess(time.monotonic() - t0, scaled(12))
        finally:
            os.close(reader)
        self.ok = True

    def test_access_log_records_without_path_or_credentials(self):
        logdir = os.path.join(self.wd.path, "logs")
        os.mkdir(logdir)
        text = config(self.fe, {"a": ("round_robin", [(self.A.port, 1, 0)])}, workers=2)
        text += f'\n[log]\ndir = "{logdir}"\naccess = true\n'
        self.start(text)
        for _ in range(20):
            r = get(self.fe, path="/echo?token=SECRETO",
                    extra=b"Authorization: Bearer SECRETO\r\nCookie: s=SECRETO\r\n")
            self.assertEqual(r.status, 200)
        self.assert_clean_exit()
        lines = []
        for i in range(2):
            with open(os.path.join(logdir, f"worker-{i}.log")) as f:
                lines += f.read().splitlines()
        access = [ln for ln in lines if " access " in ln]
        self.assertEqual(len(access), 20)
        for ln in lines:
            self.assertNotIn("SECRETO", ln)
            self.assertRegex(ln, r"^\d{4}-\d\d-\d\dT\S+ pid=\d+ worker=[01] level=[A-Z]+ gen=")
        self.assertIn("method=GET status=200 backend=127.0.0.1:", access[0])
        self.ok = True


if __name__ == "__main__":
    unittest.main(verbosity=2)
