"""Utilidades de las pruebas de integración.

- Lanza backends y proxies como procesos hijos propios (Popen), cada uno en
  su sesión, y solo termina esos PIDs: nunca pkill/killall.
- Puertos libres elegidos por el sistema (bind a :0) para no chocar con otros
  servicios ni con la demo (7777/3000).
- Ficheros temporales en un directorio propio que se borra al terminar.
- Todo tiene plazo: sockets con timeout, esperas acotadas y un límite global
  por prueba (SIGALRM).

Variables de entorno:
  PROXY_BIN      ejecutable del proxy (obligatoria)
  PROXY_TEST_BIN variante con ganchos de prueba (proxy-testhooks)
  PROXY_WRAPPER  prefijo opcional, p. ej. "valgrind --error-exitcode=99 ..."
  TIME_SCALE     multiplicador de plazos (p. ej. 4 con Valgrind)
  KEEP_LOGS      si existe, se conservan los directorios temporales (logs)
"""

import os
import random
import re
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BACKEND = os.path.join(HERE, "backend.py")
TIME_SCALE = float(os.environ.get("TIME_SCALE", "1"))


def scaled(seconds):
    return seconds * TIME_SCALE


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def closed_port():
    """Puerto en el que no escucha nadie (se reserva y se libera)."""
    return free_port()


def payload(size, seed):
    return random.Random(seed).randbytes(size)


def wait_until(pred, timeout, what="condición", interval=0.02):
    """Sondea `pred` hasta que devuelva algo verdadero o venza el plazo."""
    deadline = time.monotonic() + scaled(timeout)
    last = None
    while time.monotonic() < deadline:
        last = pred()
        if last:
            return last
        time.sleep(interval)
    raise TimeoutError(f"no se cumplió {what} en {scaled(timeout):.1f}s (último: {last!r})")


class PsUnavailable(RuntimeError):
    """`ps` no está instalado o no funciona: no se puede identificar procesos."""


def process_info(pid):
    """(ppid, línea de comandos) del proceso, o None si no existe.

    Usa `ps`, disponible con las mismas opciones en Linux (paquete procps) y
    macOS (no hay /proc en macOS). Un zombi aún no recogido también aparece.
    Si `ps` falta o falla, lanza PsUnavailable con el diagnóstico: nunca se
    confunde con "el proceso no existe".
    """
    argv = ["ps", "-o", "ppid=", "-o", "command=", "-p", str(pid)]
    try:
        r = subprocess.run(argv, capture_output=True, text=True, timeout=10, check=False)
    except FileNotFoundError as e:
        raise PsUnavailable(
            "no se encuentra `ps` (en Debian/Ubuntu: apt-get install procps); las pruebas "
            "lo usan para identificar los workers de su maestro") from e
    except (OSError, subprocess.TimeoutExpired) as e:
        raise PsUnavailable(f"no se pudo ejecutar {' '.join(argv)}: {e}") from e
    line = r.stdout.strip()
    if not line:
        if r.returncode != 0 and r.stderr.strip():
            raise PsUnavailable(f"{' '.join(argv)} falló (código {r.returncode}): "
                                f"{r.stderr.strip()}")
        return None  # sin salida: el proceso no existe
    ppid, _, cmd = line.partition(" ")
    try:
        return int(ppid), cmd.strip()
    except ValueError:
        return None


def process_state(pid):
    """Estado de `ps -o stat=` (p. ej. "S", "R", "T" detenido, "Z"), o None si
    el proceso no existe. Mismo significado en Linux y macOS."""
    argv = ["ps", "-o", "stat=", "-p", str(pid)]
    try:
        r = subprocess.run(argv, capture_output=True, text=True, timeout=10, check=False)
    except FileNotFoundError as e:
        raise PsUnavailable(
            "no se encuentra `ps` (en Debian/Ubuntu: apt-get install procps)") from e
    except (OSError, subprocess.TimeoutExpired) as e:
        raise PsUnavailable(f"no se pudo ejecutar {' '.join(argv)}: {e}") from e
    state = r.stdout.strip()
    if not state and r.returncode != 0 and r.stderr.strip():
        raise PsUnavailable(f"{' '.join(argv)} falló: {r.stderr.strip()}")
    return state or None


def is_stopped(pid):
    state = process_state(pid)
    return state is not None and state.startswith("T")


class Proc:
    """Proceso hijo con salida en fichero, terminado solo por su PID."""

    def __init__(self, argv, log_path, env=None, nofile=None):
        self.log_path = log_path
        # O_APPEND: maestro y workers escriben registros completos concurrentes.
        self.log = open(log_path, "ab")
        preexec = None
        if nofile is not None:
            import resource

            def preexec():  # solo en el hijo: no cambia límites del equipo
                resource.setrlimit(resource.RLIMIT_NOFILE, (nofile, nofile))

        self.p = subprocess.Popen(
            argv,
            stdin=subprocess.DEVNULL,
            stdout=self.log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
            env=env,
            preexec_fn=preexec,
        )

    def output(self):
        with open(self.log_path, "rb") as f:
            return f.read().decode("utf-8", "replace")

    def wait_for(self, pattern, timeout, start=0):
        deadline = time.monotonic() + timeout
        rx = re.compile(pattern)
        while time.monotonic() < deadline:
            text = self.output()[start:]
            m = rx.search(text)
            if m:
                return m
            if self.p.poll() is not None:
                raise RuntimeError(
                    f"el proceso terminó (rc={self.p.returncode}) esperando {pattern!r}:\n"
                    + self.output()[-3000:]
                )
            time.sleep(0.02)
        raise TimeoutError(f"no apareció {pattern!r} en {timeout}s:\n" + self.output()[-3000:])

    def signal(self, sig):
        if self.p.poll() is None:
            self.p.send_signal(sig)

    def stop(self, timeout=10):
        """SIGTERM, espera acotada y SIGKILL al propio PID si hace falta."""
        if self.p.poll() is None:
            self.p.send_signal(signal.SIGTERM)
            try:
                self.p.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self.p.kill()
                self.p.wait(timeout=5)
        self.log.close()
        return self.p.returncode


class Backend(Proc):
    def __init__(self, workdir, name, port=None):
        self.name = name
        self.port = port or free_port()
        super().__init__(
            [sys.executable, BACKEND, "--port", str(self.port), "--name", name],
            os.path.join(workdir, f"backend-{name}.log"),
        )
        self.wait_for(r"escuchando en", scaled(10))


KV_RX = re.compile(r"(\w+)=(\S+)")


def parse_kv(line):
    """"clave=valor ..." -> dict; los valores numéricos se convierten a int."""
    out = {}
    for k, v in KV_RX.findall(line):
        out[k] = int(v) if v.isdigit() else v
    return out


def with_stats(config_text, sock):
    """Añade [stats] con el socket de la prueba si la configuración no lo trae."""
    if "[stats]" in config_text:
        return config_text
    return config_text + f'\n[stats]\nsocket = "{sock}"\ntimeout_ms = 4000\n'


def fetch_stats(sock, timeout=5):
    import json

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(scaled(timeout))
    try:
        s.connect(sock)
        chunks = []
        while True:
            data = s.recv(65536)
            if not data:
                break
            chunks.append(data)
    finally:
        s.close()
    return json.loads(b"".join(chunks))


def health_of(counts):
    total = sum(counts.values())
    for state in ("up", "down-active", "down-passive"):
        if counts.get(state, 0) == total:
            return state
    return "mixed"


class Proxy(Proc):
    """Maestro del proxy. Toda la observación pasa por el socket de
    estadísticas (JSON agregado de todos los workers)."""

    def __init__(self, workdir, config_text, name="proxy", env=None, nofile=None,
                 wrapper=True, testhooks=False, wait_ready=True):
        self.config_path = os.path.join(workdir, f"{name}.toml")
        self.stats_sock = os.path.join(workdir, f"{name}.sock")
        self.worker_pids = set()
        with open(self.config_path, "w") as f:
            f.write(with_stats(config_text, self.stats_sock))
        wrap = shlex.split(os.environ.get("PROXY_WRAPPER", "")) if wrapper else []
        # testhooks: variante compilada con PROXY_TEST_HOOKS (retardos de
        # prueba); el ejecutable normal ignora esas variables.
        self.binary = os.environ["PROXY_TEST_BIN"] if testhooks else os.environ["PROXY_BIN"]
        argv = wrap + [self.binary, "-c", self.config_path]
        full_env = None
        if env:
            full_env = dict(os.environ)
            full_env.update(env)
        super().__init__(argv, os.path.join(workdir, f"{name}.log"), env=full_env,
                         nofile=nofile)
        if wait_ready:
            self.wait_for(r"proxy: listo", scaled(20))

    # -- workers propios (para la limpieza) -----------------------------------

    def _collect_worker_pids(self):
        for m in re.finditer(r"worker \d+ arrancado \(pid (\d+)", self.output()):
            self.worker_pids.add(int(m.group(1)))

    def stop(self, timeout=10):
        rc = super().stop(timeout)
        # Si el maestro murió sin recoger a sus workers, se terminan SOLO los
        # procesos que él mismo arrancó (pids de su log) y que siguen siendo
        # este ejecutable.
        self._collect_worker_pids()
        for pid in self.worker_pids:
            try:
                ours = self.runs_our_binary(pid)
            except PsUnavailable as e:
                # La limpieza no debe abortar, pero tampoco callar.
                print(f"(limpieza de workers omitida: {e})", file=sys.stderr)
                break
            if ours:
                try:
                    os.kill(pid, signal.SIGKILL)
                except OSError:
                    pass
        return rc

    def runs_our_binary(self, pid):
        """¿Sigue `pid` ejecutando el binario de este proxy? (Linux y macOS)"""
        info = process_info(pid)
        return info is not None and os.path.basename(self.binary) in info[1]

    def is_own_worker(self, pid):
        """¿Es `pid` un hijo vivo de ESTE maestro que ejecuta su binario?"""
        info = process_info(pid)
        return (info is not None and info[0] == self.p.pid
                and os.path.basename(self.binary) in info[1])

    def alive_workers(self):
        """PIDs de workers arrancados por este maestro que siguen vivos."""
        self._collect_worker_pids()
        alive = set()
        for pid in self.worker_pids:
            try:
                os.kill(pid, 0)
                alive.add(pid)
            except OSError:
                pass
        return alive

    # -- estadísticas ---------------------------------------------------------

    def stats(self, timeout=5):
        return fetch_stats(self.stats_sock, timeout)

    def accounting(self, with_backends=False):
        """Contabilidad agregada con las claves históricas de las pruebas.

        Suma los contadores de los workers que respondieron; generation y
        reloads_* son los del maestro; generations_live es el máximo por
        worker. Con with_backends=True devuelve (general, {(pool, addr): dict}).
        """
        doc = self.stats()
        tot = doc["totals"]["counters"]
        acc = dict(tot)
        live = [w["counters"]["generations_live"] for w in doc["workers"] if w["responded"]]
        acc["generations_live"] = max(live) if live else 0
        acc["generation"] = doc["master"]["generation"]
        acc["reloads_ok"] = doc["master"]["reloads_ok"]
        acc["reloads_failed"] = doc["master"]["reloads_failed"]
        acc["reload_running"] = int(doc["master"]["reload_in_progress"] or tot["reload_running"] > 0)
        acc["draining"] = int(doc["master"]["draining"] or tot["draining"] > 0)
        acc["complete"] = doc["complete"]
        for w in doc["workers"]:
            if w["pid"]:
                self.worker_pids.add(w["pid"])
        if not with_backends:
            return acc
        backends = {}
        for b in doc["totals"]["backends"]:
            backends[(b["pool"], b["addr"])] = {
                "active": b["active"], "selected": b["selected"], "failures": b["failures"],
                "health": health_of(b["health_by_worker"]), "algo": b["algorithm"],
            }
        return acc, backends

    def backend(self, pool, addr):
        return self.accounting(with_backends=True)[1][(pool, addr)]

    def generation(self):
        return self.accounting()["generation"]

    def write_config(self, config_text):
        """Reescribe el fichero conservando la sección [stats] de la prueba."""
        with open(self.config_path, "w") as f:
            f.write(with_stats(config_text, self.stats_sock))

    def reload(self, config_text, expect="aplicada", timeout=10):
        """Reescribe el fichero, envía SIGHUP y espera el resultado del maestro."""
        start = len(self.output())
        with open(self.config_path, "w") as f:
            f.write(with_stats(config_text, self.stats_sock))
        self.signal(signal.SIGHUP)
        m = self.wait_for(r"recarga (aplicada|rechazada)[^\n]*", scaled(timeout), start=start)
        if expect not in m.group(0):
            raise AssertionError(f"se esperaba recarga {expect}: {m.group(0)}")
        return m.group(0)

    def wait_quiescent(self, timeout=6):
        """Espera a que ningún worker tenga conexiones, upstreams ni slots."""
        deadline = time.monotonic() + scaled(timeout)
        acc = None
        while time.monotonic() < deadline:
            acc = self.accounting()
            if (acc["complete"] and acc["conns"] == 0 and acc["upstreams"] == 0
                    and acc["slots_in_use"] == 0 and acc["backend_active"] == 0):
                return acc
            time.sleep(0.05)
        raise AssertionError(f"el proxy no quedó en reposo: {acc}")


class Workdir:
    def __init__(self):
        self.path = tempfile.mkdtemp(prefix="proxy-it-")
        self.procs = []

    def add(self, proc):
        self.procs.append(proc)
        return proc

    def cleanup(self, keep_on_failure=False):
        for p in reversed(self.procs):
            try:
                p.stop(timeout=scaled(10))
            except Exception:  # noqa: BLE001 - la limpieza no debe abortar
                pass
        self.procs.clear()
        if keep_on_failure or os.environ.get("KEEP_LOGS"):
            print(f"(logs conservados en {self.path})", file=sys.stderr)
        else:
            shutil.rmtree(self.path, ignore_errors=True)


# ---------------------------------------------------------------------------
# Cliente HTTP mínimo (framing explícito)
# ---------------------------------------------------------------------------


class Response:
    def __init__(self, status, reason, headers, body, raw_head):
        self.status = status
        self.reason = reason
        self.headers = headers  # lista [(nombre, valor)]
        self.body = body
        self.raw_head = raw_head
        self.trailers = []

    def header(self, name):
        for k, v in self.headers:
            if k.lower() == name.lower():
                return v
        return None

    def json(self):
        import json

        return json.loads(self.body)


class Conn:
    def __init__(self, port, timeout=5, rcvbuf=None):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if rcvbuf is not None:
            # Antes de connect, para que afecte a la ventana anunciada.
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        self.sock.settimeout(scaled(timeout))
        self.sock.connect(("127.0.0.1", port))
        self.buf = b""
        self.eof = False

    def send(self, data):
        self.sock.sendall(data)

    def _fill(self):
        data = self.sock.recv(65536)
        if not data:
            self.eof = True
            return False
        self.buf += data
        return True

    def read_until(self, marker):
        while marker not in self.buf:
            if not self._fill():
                raise EOFError(f"EOF antes de {marker!r}; recibido {self.buf[:200]!r}")
        head, self.buf = self.buf.split(marker, 1)
        return head

    def read_exact(self, n):
        while len(self.buf) < n:
            if not self._fill():
                raise EOFError(f"EOF: esperados {n}, hay {len(self.buf)}")
        data, self.buf = self.buf[:n], self.buf[n:]
        return data

    def read_to_eof(self):
        while self._fill():
            pass
        data, self.buf = self.buf, b""
        return data

    def read_response(self, head_request=False):
        head = self.read_until(b"\r\n\r\n").decode("latin-1")
        lines = head.split("\r\n")
        parts = lines[0].split(" ", 2)
        status = int(parts[1])
        reason = parts[2] if len(parts) > 2 else ""
        headers = []
        for line in lines[1:]:
            k, v = line.split(":", 1)
            headers.append((k, v.strip()))
        r = Response(status, reason, headers, b"", head)
        if status < 200:
            return r
        te = r.header("Transfer-Encoding")
        cl = r.header("Content-Length")
        if head_request or status in (204, 304):
            return r
        if te is not None:
            assert te.lower() == "chunked", te
            body = b""
            while True:
                size = int(self.read_until(b"\r\n").split(b";")[0], 16)
                if size == 0:
                    while True:
                        line = self.read_until(b"\r\n")
                        if line == b"":
                            break
                        r.trailers.append(line.decode("latin-1"))
                    break
                body += self.read_exact(size)
                assert self.read_exact(2) == b"\r\n"
            r.body = body
        elif cl is not None:
            r.body = self.read_exact(int(cl))
        else:
            r.body = self.read_to_eof()
        return r

    def closed_by_peer(self, timeout=3):
        """True si el proxy cerró la conexión (EOF o RST) dentro del plazo."""
        self.sock.settimeout(scaled(timeout))
        try:
            data = self.sock.recv(1)
        except ConnectionResetError:
            return True
        except socket.timeout:
            return False
        return data == b""

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def get(port, path="/echo", host="api.test", extra=b"", method="GET", version="1.1"):
    """Una petición con Connection: close; devuelve la respuesta."""
    c = Conn(port)
    try:
        req = (f"{method} {path} HTTP/{version}\r\nHost: {host}\r\nConnection: close\r\n").encode()
        c.send(req + extra + b"\r\n")
        return c.read_response(head_request=(method == "HEAD"))
    finally:
        c.close()


def backend_ctl(port, path, timeout=5):
    """Petición directa a un backend (sin proxy) para controlarlo u observarlo."""
    c = Conn(port, timeout=timeout)
    try:
        c.send(f"GET {path} HTTP/1.1\r\nHost: ctl\r\nConnection: close\r\n\r\n".encode())
        return c.read_response()
    finally:
        c.close()


def backend_state(port):
    return backend_ctl(port, "/_state").json()


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


def in_background(fn, *args):
    """Ejecuta fn en un hilo; devuelve (hilo, dict con 'result' o 'error')."""
    import threading

    box = {}

    def run():
        try:
            box["result"] = fn(*args)
        except BaseException as e:  # noqa: BLE001 - se informa al unirse
            box["error"] = e

    t = threading.Thread(target=run, daemon=True)
    t.start()
    return t, box


def join(t, box, timeout=10):
    t.join(scaled(timeout))
    if t.is_alive():
        raise TimeoutError("el hilo auxiliar no terminó")
    if "error" in box:
        raise box["error"]
    return box.get("result")
