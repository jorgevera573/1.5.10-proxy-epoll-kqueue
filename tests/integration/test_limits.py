#!/usr/bin/env python3
"""accept ante EMFILE, en un proceso proxy aislado con RLIMIT_NOFILE bajo.

El límite se fija solo en el proceso hijo (preexec setrlimit); no se tocan
límites del equipo. Bajo Valgrind se omite: Valgrind necesita sus propios
descriptores y altera el límite efectivo.
"""

import os
import socket
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import (  # noqa: E402
    Alarm,
    Backend,
    Proxy,
    Workdir,
    free_port,
    get,
    scaled,
    wait_until,
)

NOFILE = 40


def cpu_seconds(pid):
    """utime + stime del proceso, en segundos (/proc, Linux)."""
    with open(f"/proc/{pid}/stat") as f:
        fields = f.read().rsplit(")", 1)[1].split()
    ticks = int(fields[11]) + int(fields[12])
    return ticks / os.sysconf("SC_CLK_TCK")


@unittest.skipIf(os.environ.get("PROXY_WRAPPER"), "bajo Valgrind el límite de fds no es fiable")
@unittest.skipUnless(os.path.exists("/proc/self/stat"), "requiere /proc (Linux)")
class EmfileTest(unittest.TestCase):
    def test_accept_under_emfile_sheds_without_spinning_and_recovers(self):
        with Alarm(90):
            wd = Workdir()
            ok = False
            clients = []
            try:
                a = wd.add(Backend(wd.path, "A"))
                fe = free_port()
                proxy = wd.add(Proxy(wd.path, f"""
[timeouts]
client_header_ms = {int(scaled(60) * 1000)}
[[frontend]]
listen = "127.0.0.1:{fe}"
[[pool]]
name = "a"
[[pool.backend]]
address = "127.0.0.1:{a.port}"
[routing]
default_pool = "a"
""", nofile=NOFILE))
                self.assertEqual(get(fe).status, 200)

                # Más conexiones inactivas que descriptores libres.
                for _ in range(NOFILE):
                    s = socket.create_connection(("127.0.0.1", fe), timeout=5)
                    clients.append(s)
                acc = wait_until(lambda: (lambda x: x if x["accept_emfile"] > 0 else None)(
                    proxy.accounting()), 10, "EMFILE en accept")
                held = acc["conns"]
                self.assertGreater(held, 0)
                self.assertLess(held, NOFILE)

                # Las sobrantes se cierran de inmediato (no quedan colgadas).
                def closed(s):
                    s.settimeout(0.01)
                    try:
                        return s.recv(1) == b""
                    except socket.timeout:
                        return False
                    except ConnectionError:
                        return True

                wait_until(lambda: sum(closed(s) for s in clients) >= NOFILE - held, 10,
                           "sobrantes cerradas")
                late = socket.create_connection(("127.0.0.1", fe), timeout=5)
                late.settimeout(scaled(2))
                self.assertEqual(late.recv(1), b"")  # descartada al instante
                late.close()

                # Sin bucle: en saturación sostenida el proceso casi no usa CPU.
                wpid = proxy.stats()["workers"][0]["pid"]  # el que acepta es el worker
                c0 = cpu_seconds(wpid)
                time.sleep(1.0)
                self.assertLess(cpu_seconds(wpid) - c0, 0.2)

                acc = proxy.accounting()
                self.assertGreaterEqual(acc["rejected"], NOFILE - held)

                # Recuperación al liberar descriptores.
                for s in clients:
                    s.close()
                clients.clear()
                proxy.wait_quiescent()
                self.assertEqual(get(fe).status, 200)
                ok = True
            finally:
                for s in clients:
                    s.close()
                wd.cleanup(keep_on_failure=not ok)


if __name__ == "__main__":
    unittest.main(verbosity=2)
