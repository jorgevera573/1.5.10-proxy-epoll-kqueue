#!/usr/bin/env bash
# Serie final del benchmark (una sola pasada; no se repite selectivamente).
#
# - Proxy: build-bench-release (commit e52c468), 6 workers.
# - Backend: nginx 1.28.3, 2 workers, respuesta fija de 16 bytes.
# - Generador: wrk-monotonic (wrk 4.1.0-4build3 + evidence/wrk-monotonic.patch).
# - wrk: -t4 -c100, --timeout 2s explícito; calentamiento 10 s (como el
#   original, sin --latency) y 3 × 30 s con --latency.
# - Configuración: copias de las originales; solo cambian la ruta del socket
#   de estadísticas y el prefijo de nginx (pid/error.log), para no escribir en
#   /tmp/proxy-bench.QWIB56.
# - clockmon observa el reloj durante toda la serie.
# - Solo se detienen los procesos que arranca este script.
set -uo pipefail
W=$(cd "$(dirname "$0")" && pwd)
REPO=/mnt/c/MASTER-ING-SOFTWARE/semana-06/1.5.10-proxy-epoll-kqueue
ORIG=/tmp/proxy-bench.QWIB56
PROXY="$REPO/build-bench-release/src/proxy"
WRK="$W/wrk-monotonic/wrk"
D=$(mktemp -d /tmp/pbf.XXXXXX)
OUT="$D/out"
mkdir -p "$OUT"
URL=http://127.0.0.1:18081/bench
EXPECTED="$OUT/expected-body.bin"
printf 'proxy-benchmark\n' > "$EXPECTED"

ts() { date -u +%Y-%m-%dT%H:%M:%S.%6NZ; }
log() { echo "$(ts) $*" | tee -a "$OUT/timeline.txt"; }
echo "$D" > "$W/evidence/final-series-dir.txt"

for p in 18080 18081; do
    if (echo > /dev/tcp/127.0.0.1/$p) 2>/dev/null; then log "puerto $p ocupado: se aborta"; exit 1; fi
done

# Configuraciones (copias)
sed "s|^pid nginx.pid;|pid $D/nginx.pid;|" "$ORIG/nginx.conf" > "$D/nginx.conf"
sed "s|$ORIG/stats.sock|$D/stats.sock|" "$ORIG/proxy-6workers.toml" > "$D/proxy-6workers.toml"
diff "$ORIG/nginx.conf" "$D/nginx.conf" > "$OUT/config-diff.txt"
diff "$ORIG/proxy-6workers.toml" "$D/proxy-6workers.toml" >> "$OUT/config-diff.txt"
cp "$D/nginx.conf" "$D/proxy-6workers.toml" "$OUT/"

{
    echo "fecha: $(ts)"
    uname -a
    echo "CPUs: $(nproc)"
    echo "commit: $(git -C "$REPO" rev-parse HEAD)"
    echo "git status (src): $(git -C "$REPO" status --porcelain -- src meson.build | wc -l) cambios"
    echo "ninja -n: $(ninja -C "$REPO/build-bench-release" -n 2>&1 | tail -1)"
    sha256sum "$PROXY" "$WRK" /usr/bin/wrk
    "$WRK" -v 2>&1 | head -1
    nginx -v 2>&1
    cat /sys/devices/system/clocksource/clocksource0/current_clocksource
    chronyc tracking 2>&1
} > "$OUT/environment.txt"
cp "$W/evidence/build-bench-release.txt" "$OUT/"

# Arranque
nginx -p "$D/" -c "$D/nginx.conf" -e "$D/error.log" || { log "nginx no arrancó"; exit 1; }
sleep 1
NGINX_PID=$(cat "$D/nginx.pid")
"$PROXY" -c "$D/proxy-6workers.toml" > "$OUT/proxy.log" 2>&1 &
PROXY_PID=$!
for _ in $(seq 1 50); do grep -q "proxy: listo" "$OUT/proxy.log" && break; sleep 0.1; done
log "nginx pid $NGINX_PID; proxy pid $PROXY_PID"

# check NOMBRE: comprobación puntual (una petición) al backend y al proxy
check() {
    local name=$1 target url
    for target in backend:18080 proxy:18081; do
        url="http://127.0.0.1:${target#*:}/bench"
        curl -s -D "$OUT/$name-${target%%:*}.headers" -o "$OUT/$name-${target%%:*}.body" \
             -w "%{http_code} %{size_download}\n" "$url" > "$OUT/$name-${target%%:*}.status"
        if cmp -s "$OUT/$name-${target%%:*}.body" "$EXPECTED"; then body=igual; else body=DISTINTO; fi
        log "comprobación $name ${target%%:*}: estado/bytes $(cat "$OUT/$name-${target%%:*}.status") cuerpo $body al esperado"
    done
}
stats() {
    python3 "$REPO/scripts/proxy-stats.py" "$D/stats.sock" > "$OUT/$1.txt" 2>&1
    python3 "$REPO/scripts/proxy-stats.py" "$D/stats.sock" --json > "$OUT/$1.json" 2>&1
}
# run NOMBRE DURACIÓN [--latency]
run() {
    local name=$1 dur=$2; shift 2
    local f="$OUT/$name.txt"
    {
        echo "# comando: $WRK -t4 -c100 -d$dur --timeout 2s $* $URL"
        echo "# inicio: $(ts)"
        "$WRK" -t4 -c100 -d"$dur" --timeout 2s "$@" "$URL" 2>&1
        echo "# código: $?"
        echo "# fin: $(ts)"
    } > "$f"
    log "$name: $(grep -E 'Requests/sec|Socket errors|Non-2xx' "$f" | tr -s ' ' | tr '\n' ' ')"
}

check before
stats stats-initial
"$W/clockmon" 170 > "$OUT/clockmon.txt" &
CLOCKMON_PID=$!
sleep 1
run warmup 10s
stats stats-before-series
for i in 1 2 3; do run "run$i" 30s --latency; done
stats stats-after-series
check after
wait "$CLOCKMON_PID"
log "clockmon: $(grep -cE 'BACKWARD|STEP' "$OUT/clockmon.txt") saltos; $(tail -1 "$OUT/clockmon.txt")"

# Cierre
kill -TERM "$PROXY_PID"
wait "$PROXY_PID"
log "proxy terminó (código $?)"
nginx -p "$D/" -c "$D/nginx.conf" -e "$D/error.log" -s quit
for _ in $(seq 1 50); do kill -0 "$NGINX_PID" 2>/dev/null || break; sleep 0.1; done
if kill -0 "$NGINX_PID" 2>/dev/null; then log "nginx sigue vivo"; else log "nginx detenido"; fi
cp "$D/error.log" "$OUT/nginx-error.log"
log "fin de la serie"
