#!/usr/bin/env python3
"""Backend HTTP/1.1 de pruebas y demostración.

Implementado sobre sockets para controlar exactamente el framing de cada
respuesta (incluidas respuestas inválidas o truncadas). Atiende una petición
por conexión y cierra (el proxy siempre envía "Connection: close").

Rutas (la query admite size, seed, chunk, delay_ms, code):
  /echo, cualquier otra   JSON con backend, método, ruta, versión, cabeceras
                          recibidas (en orden), longitud y SHA-256 del cuerpo
  /big?size=N&seed=S      N bytes binarios deterministas (Content-Length)
  /chunked?size=N&chunk=K N bytes en chunks de K bytes
  /close?size=N           cuerpo delimitado por cierre (sin Content-Length)
  /truncate?size=N        anuncia N bytes, envía N/2 y cierra
  /slow?delay_ms=D        espera D ms y responde como /echo
  /drip?size=N&delay_ms=D envía N bytes en 16 trozos con pausas de D ms
  /status?code=204|304    respuesta sin cuerpo (304 anuncia Content-Length)
  /switch                 101 Switching Protocols (no soportado por el proxy)
  /interim                103 Early Hints y después 200 (/echo)
  /garbage                respuesta que no es HTTP
  /hugehead               cabecera de respuesta de 20 KB (mayor que un slot)
  /early?size=N           responde 413 sin leer el cuerpo de la petición
  /hold?key=K             retiene la respuesta hasta /release?key=K (máx. 30 s)
  /release?key=K          libera las peticiones retenidas con esa clave
  /health                 estado según /_set (por defecto 200)
  /_set?health=C&health_delay_ms=D   cambia la respuesta de /health
  /_state                 JSON: peticiones retenidas por clave, sondas /health
                          en curso y atendidas, total de peticiones

Toda respuesta generada lleva "X-Backend: <nombre>".

Datos binarios: random.Random(seed).randbytes(size); las pruebas generan los
mismos bytes para comparar el contenido completo.
"""

import argparse
import hashlib
import json
import random
import socket
import socketserver
import sys
import threading
import time
from urllib.parse import parse_qs, urlsplit

MAX_HEAD = 65536


def payload(size, seed):
    return random.Random(seed).randbytes(size)


class BadRequest(Exception):
    pass


def read_until(sock, buf, marker, limit):
    while marker not in buf:
        if len(buf) > limit:
            raise BadRequest("cabecera demasiado grande")
        data = sock.recv(65536)
        if not data:
            raise EOFError
        buf += data
    return buf


def read_exact(sock, buf, n):
    while len(buf) < n:
        data = sock.recv(65536)
        if not data:
            raise EOFError
        buf += data
    return buf[:n], buf[n:]


def read_chunked(sock, buf):
    body = b""
    while True:
        buf = read_until(sock, buf, b"\r\n", MAX_HEAD)
        line, buf = buf.split(b"\r\n", 1)
        size = int(line.split(b";", 1)[0].strip(), 16)
        if size == 0:
            # Trailers hasta la línea vacía.
            while True:
                buf = read_until(sock, buf, b"\r\n", MAX_HEAD)
                line, buf = buf.split(b"\r\n", 1)
                if line == b"":
                    return body, buf
        data, buf = read_exact(sock, buf, size + 2)
        if data[-2:] != b"\r\n":
            raise BadRequest("chunk sin CRLF")
        body += data[:-2]


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        sock = self.request
        sock.settimeout(30)
        try:
            self.serve(sock)
        except (EOFError, ConnectionError, socket.timeout, BadRequest, ValueError):
            pass

    def send(self, data):
        self.request.sendall(data)

    def respond(self, status, body, ctype="application/octet-stream", extra=b""):
        head = (
            f"HTTP/1.1 {status}\r\nContent-Type: {ctype}\r\n"
            f"Content-Length: {len(body)}\r\nConnection: close\r\n"
            f"X-Backend: {self.server.name}\r\n"
        ).encode() + extra + b"\r\n"
        self.send(head + (b"" if self.method == "HEAD" else body))

    def serve(self, sock):
        buf = read_until(sock, b"", b"\r\n\r\n", MAX_HEAD)
        head, rest = buf.split(b"\r\n\r\n", 1)
        lines = head.decode("latin-1").split("\r\n")
        method, target, version = lines[0].split(" ")
        self.method = method
        headers = []
        for line in lines[1:]:
            name, value = line.split(":", 1)
            headers.append([name, value.strip()])
        lower = {}
        for name, value in headers:
            lower.setdefault(name.lower(), []).append(value)

        url = urlsplit(target)
        q = {k: v[0] for k, v in parse_qs(url.query).items()}
        size = int(q.get("size", "0"))
        seed = int(q.get("seed", "1"))
        path = url.path
        server = self.server

        if path == "/health":
            with server.lock:
                server.health_inflight += 1
                code, delay = server.health_code, server.health_delay_ms
            try:
                time.sleep(delay / 1000)
                self.respond(f"{code} X", b"", "text/plain")
            finally:
                with server.lock:
                    server.health_inflight -= 1
                    server.health_served += 1
            return
        if path == "/_set":
            with server.lock:
                if "health" in q:
                    server.health_code = int(q["health"])
                if "health_delay_ms" in q:
                    server.health_delay_ms = int(q["health_delay_ms"])
            self.respond("200 OK", b"ok\n", "text/plain")
            return
        if path == "/_state":
            with server.lock:
                doc = {
                    "name": server.name,
                    "held": {k: len(v) for k, v in server.held.items() if v},
                    "health_inflight": server.health_inflight,
                    "health_served": server.health_served,
                    "count": server.count,
                }
            self.respond("200 OK", json.dumps(doc).encode(), "application/json")
            return
        if path == "/release":
            key = q.get("key", "")
            with server.lock:
                events = server.held.pop(key, [])
            for ev in events:
                ev.set()
            self.respond("200 OK", b"released %d\n" % len(events), "text/plain")
            return
        if path == "/early":
            # Responde sin leer el cuerpo y cierra.
            self.respond("413 Content Too Large", b"too large\n", "text/plain")
            return

        body = b""
        if "transfer-encoding" in lower:
            body, rest = read_chunked(sock, rest)
        elif "content-length" in lower:
            body, rest = read_exact(sock, rest, int(lower["content-length"][0]))

        if path == "/hold":
            ev = threading.Event()
            key = q.get("key", "")
            with server.lock:
                server.held.setdefault(key, []).append(ev)
            ev.wait(30)
            with server.lock:
                lst = server.held.get(key, [])
                if ev in lst:
                    lst.remove(ev)

        with server.lock:
            server.count += 1
        if server.verbose:
            print(
                f"[{server.name}] {method} {target} Host={lower.get('host', ['-'])[0]} "
                f"XFF={lower.get('x-forwarded-for', ['-'])[0]} body={len(body)}",
                flush=True,
            )

        def echo(status="200 OK"):
            doc = {
                "backend": server.name,
                "method": method,
                "target": target,
                "version": version,
                "headers": headers,
                "body_len": len(body),
                "body_sha256": hashlib.sha256(body).hexdigest(),
            }
            self.respond(status, json.dumps(doc).encode(), "application/json")

        if path == "/big":
            self.respond("200 OK", payload(size, seed))
        elif path == "/chunked":
            data = payload(size, seed)
            chunk = max(1, int(q.get("chunk", "4096")))
            self.send(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n")
            if method != "HEAD":
                for i in range(0, len(data), chunk):
                    part = data[i : i + chunk]
                    self.send(b"%x\r\n" % len(part) + part + b"\r\n")
                self.send(b"0\r\nX-Trailer: ok\r\n\r\n")
        elif path == "/close":
            self.send(b"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n")
            if method != "HEAD":
                self.send(payload(size, seed))
        elif path == "/truncate":
            data = payload(size, seed)
            self.send(f"HTTP/1.1 200 OK\r\nContent-Length: {size}\r\n\r\n".encode())
            self.send(data[: size // 2])
        elif path == "/slow":
            time.sleep(int(q.get("delay_ms", "1000")) / 1000)
            echo()
        elif path == "/drip":
            data = payload(size, seed)
            delay = int(q.get("delay_ms", "50")) / 1000
            self.send(f"HTTP/1.1 200 OK\r\nContent-Length: {size}\r\n\r\n".encode())
            step = max(1, size // 16)
            for i in range(0, size, step):
                self.send(data[i : i + step])
                time.sleep(delay)
        elif path == "/status":
            code = int(q.get("code", "204"))
            if code == 204:
                self.send(b"HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n")
            else:
                self.send(
                    b"HTTP/1.1 304 Not Modified\r\nContent-Length: 5\r\n"
                    b"ETag: \"x\"\r\nConnection: close\r\n\r\n"
                )
        elif path == "/switch":
            self.send(
                b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                b"Connection: Upgrade\r\n\r\n"
            )
        elif path == "/interim":
            self.send(b"HTTP/1.1 103 Early Hints\r\nLink: </s.css>; rel=preload\r\n\r\n")
            echo()
        elif path == "/hugehead":
            self.send(b"HTTP/1.1 200 OK\r\nX-Big: " + b"h" * 20000 + b"\r\nContent-Length: 0\r\n\r\n")
        elif path == "/garbage":
            self.send(b"NOT HTTP AT ALL\r\n\r\n")
        else:
            echo()


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    srv = Server((args.host, args.port), Handler)
    srv.name = args.name
    srv.verbose = args.verbose
    srv.count = 0
    srv.lock = threading.Lock()
    srv.held = {}
    srv.health_code = 200
    srv.health_delay_ms = 0
    srv.health_inflight = 0
    srv.health_served = 0
    print(f"backend {args.name} escuchando en {args.host}:{srv.server_address[1]}", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
