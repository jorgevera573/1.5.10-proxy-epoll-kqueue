#!/usr/bin/env bash
# Comparación directo/proxy (AGENTS.md § Rendimiento, docs/requirements.md).
#
# Una sola pasada; se registran todas las ejecuciones, sin selección.
# - Proxy: build-bench-release (e52c468), 6 workers. Backend: nginx 1.28.3,
#   2 workers, respuesta de 16 bytes. Mismas configuraciones que la serie
#   final (copias; solo cambian pid de nginx y socket de estadísticas).
# - Generador: wrk-monotonic (SHA-256 90a7220a…), -t4 -c100 --timeout 2s.
# - Calentamiento de 10 s por destino (directo y luego proxy) y tres rondas
#   de 30 s por destino, en orden: directo→proxy, proxy→directo,
#   directo→proxy.
# - Estadísticas del proxy tras cada ejecución; clockmon toda la comparación;
#   comprobación de estado y cuerpo antes y después.
# - Solo se detienen los procesos que arranca este script.
set -uo pipefail
W=$(cd "$(dirname "$0")" && pwd)
REPO=/mnt/c/MASTER-ING-SOFTWARE/semana-06/1.5.10-proxy-epoll-kqueue
ORIG=/tmp/proxy-bench.QWIB56
PROXY="$REPO/build-bench-release/src/proxy"
WRK="$HOME/proxy-benchmark-evidence/serie-final-20260927T0140Z/03-serie-final/wrk-monotonic"
D=$(mktemp -d /tmp/pbc.XXXXXX)
OUT="$D/out"
mkdir -p "$OUT"
declare -A URL=([directo]=http://127.0.0.1:18080/bench [proxy]=http://127.0.0.1:18081/bench)
EXPECTED="$OUT/expected-body.bin"
printf 'proxy-benchmark\n' > "$EXPECTED"
echo "$D" > "$W/evidence/comparison-dir.txt"

ts() { date -u +%Y-%m-%dT%H:%M:%S.%6NZ; }
log() { echo "$(ts) $*" | tee -a "$OUT/timeline.txt"; }

for p in 18080 18081; do
    if (echo > /dev/tcp/127.0.0.1/$p) 2>/dev/null; then log "puerto $p ocupado: se aborta"; exit 1; fi
done

sed "s|^pid nginx.pid;|pid $D/nginx.pid;|" "$ORIG/nginx.conf" > "$D/nginx.conf"
sed "s|$ORIG/stats.sock|$D/stats.sock|" "$ORIG/proxy-6workers.toml" > "$D/proxy-6workers.toml"
{ diff "$ORIG/nginx.conf" "$D/nginx.conf"; diff "$ORIG/proxy-6workers.toml" "$D/proxy-6workers.toml"; } > "$OUT/config-diff.txt"
cp "$D/nginx.conf" "$D/proxy-6workers.toml" "$OUT/"

{
    echo "fecha: $(ts)"
    uname -a
    echo "CPUs: $(nproc)"
    echo "commit: $(git -C "$REPO" rev-parse HEAD)"
    echo "cambios en src/ desde e52c468: $(git -C "$REPO" diff --stat e52c468 -- src meson.build | wc -l) líneas de diff"
    echo "ninja -n: $(ninja -C "$REPO/build-bench-release" -n 2>&1 | tail -1)"
    sha256sum "$PROXY" "$WRK"
    "$WRK" -v 2>&1 | head -1
    nginx -v 2>&1
    cat /sys/devices/system/clocksource/clocksource0/current_clocksource
    chronyc tracking 2>&1
} > "$OUT/environment.txt"

nginx -p "$D/" -c "$D/nginx.conf" -e "$D/error.log" || { log "nginx no arrancó"; exit 1; }
sleep 1
NGINX_PID=$(cat "$D/nginx.pid")
"$PROXY" -c "$D/proxy-6workers.toml" > "$OUT/proxy.log" 2>&1 &
PROXY_PID=$!
for _ in $(seq 1 50); do grep -q "proxy: listo" "$OUT/proxy.log" && break; sleep 0.1; done
log "nginx pid $NGINX_PID; proxy pid $PROXY_PID"

check() {  # comprobación puntual: una petición a cada destino
    local name=$1 dst
    for dst in directo proxy; do
        curl -s -D "$OUT/$name-$dst.headers" -o "$OUT/$name-$dst.body" \
             -w "%{http_code} %{size_download}\n" "${URL[$dst]}" > "$OUT/$name-$dst.status"
        if cmp -s "$OUT/$name-$dst.body" "$EXPECTED"; then body=igual; else body=DISTINTO; fi
        log "comprobación $name $dst: estado/bytes $(cat "$OUT/$name-$dst.status") cuerpo $body al esperado"
    done
}
stats() {
    python3 "$REPO/scripts/proxy-stats.py" "$D/stats.sock" > "$OUT/stats-$1.txt" 2>&1
    python3 "$REPO/scripts/proxy-stats.py" "$D/stats.sock" --json > "$OUT/stats-$1.json" 2>&1
}
run() {  # run NOMBRE DESTINO DURACIÓN [--latency]
    local name=$1 dst=$2 dur=$3; shift 3
    local f="$OUT/$name.txt"
    {
        echo "# destino: $dst"
        echo "# comando: $WRK -t4 -c100 -d$dur --timeout 2s $* ${URL[$dst]}"
        echo "# inicio: $(ts)"
        "$WRK" -t4 -c100 -d"$dur" --timeout 2s "$@" "${URL[$dst]}" 2>&1
        echo "# código: $?"
        echo "# fin: $(ts)"
    } > "$f"
    log "$name ($dst): $(grep -E 'Requests/sec|Socket errors|Non-2xx' "$f" | tr -s ' ' | tr '\n' ' ')"
    stats "after-$name"
}

check before
stats initial
"$W/clockmon" 260 > "$OUT/clockmon.txt" &
CLOCKMON_PID=$!
sleep 1
run warmup-directo directo 10s
run warmup-proxy proxy 10s
run r1-1-directo directo 30s --latency
run r1-2-proxy proxy 30s --latency
run r2-1-proxy proxy 30s --latency
run r2-2-directo directo 30s --latency
run r3-1-directo directo 30s --latency
run r3-2-proxy proxy 30s --latency
check after
wait "$CLOCKMON_PID"
log "clockmon: $(grep -cE 'BACKWARD|STEP' "$OUT/clockmon.txt") saltos; $(tail -1 "$OUT/clockmon.txt")"

kill -TERM "$PROXY_PID"
wait "$PROXY_PID"
log "proxy terminó (código $?)"
nginx -p "$D/" -c "$D/nginx.conf" -e "$D/error.log" -s quit
for _ in $(seq 1 50); do kill -0 "$NGINX_PID" 2>/dev/null || break; sleep 0.1; done
if kill -0 "$NGINX_PID" 2>/dev/null; then log "nginx sigue vivo"; else log "nginx detenido"; fi
cp "$D/error.log" "$OUT/nginx-error.log"
cp "$0" "$OUT/run-comparison.sh"
log "fin de la comparación"
