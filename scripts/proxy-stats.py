#!/usr/bin/env python3
"""Cliente del socket de estadísticas del proxy.

Protocolo: el cliente se conecta al socket UNIX; el maestro reúne las
instantáneas de los workers y envía UN documento JSON terminado en '\\n' y
cierra. No hace falta enviar nada.

Uso:
  scripts/proxy-stats.py RUTA_SOCKET            resumen legible
  scripts/proxy-stats.py RUTA_SOCKET --json     JSON completo (indentado)
  scripts/proxy-stats.py RUTA_SOCKET --raw      JSON tal como llega
"""

import argparse
import json
import socket
import sys


def fetch(path, timeout=5.0):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(path)
    chunks = []
    try:
        while True:
            data = s.recv(65536)
            if not data:
                break
            chunks.append(data)
    finally:
        s.close()
    return b"".join(chunks)


def summary(doc):
    m = doc["master"]
    t = doc["totals"]
    out = [
        f"maestro pid={m['pid']} uptime={m['uptime_ms'] / 1000:.1f}s generación={m['generation']} "
        f"workers={m['workers_configured']} recarga_en_curso={m['reload_in_progress']} "
        f"reinicios={m['worker_restarts']}",
        f"recarga={m['reload_state']} recuperación={m['recovery']['state']}"
        f"(gen {m['recovery']['generation']}) listos={m['workers_ready']}/{m['workers_configured']}"
        f" degradado={m['degraded']}",
        f"completo={doc['complete']} sin_respuesta={doc['missing_workers']}",
    ]
    for w in doc["workers"]:
        if not w["responded"]:
            out.append(f"  worker {w['index']} pid={w['pid']} estado={w['state']} (sin respuesta)")
            continue
        c = w["counters"]
        out.append(
            f"  worker {w['index']} pid={w['pid']} estado={w['state']} gen={w['generation']} "
            f"conns={c['conns']} peticiones={c['requests']} aceptadas={c['accepted']} "
            f"5xx={c['proxy_5xx']} log_descartados={c['log_dropped']}")
    c = t["counters"]
    out.append(f"totales: peticiones={c['requests']} conns={c['conns']} "
               f"slots={c['slots_in_use']}/{c['slots_total']} log_descartados={c['log_dropped']}")
    for b in t["backends"]:
        out.append(f"  {b['pool']}/{b['addr']} ({b['algorithm']}) activas={b['active']} "
                   f"elegido={b['selected']} fallos={b['failures']} salud={b['health_summary']} "
                   f"{b['health_by_worker']}")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("socket")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--raw", action="store_true")
    ap.add_argument("--timeout", type=float, default=5.0)
    args = ap.parse_args()
    try:
        raw = fetch(args.socket, args.timeout)
    except OSError as e:
        print(f"proxy-stats: {args.socket}: {e}", file=sys.stderr)
        return 1
    if args.raw:
        sys.stdout.write(raw.decode())
        return 0
    try:
        doc = json.loads(raw)
    except ValueError as e:
        print(f"proxy-stats: JSON inválido: {e}", file=sys.stderr)
        return 1
    print(json.dumps(doc, indent=2, ensure_ascii=False) if args.json else summary(doc))
    return 0


if __name__ == "__main__":
    sys.exit(main())
