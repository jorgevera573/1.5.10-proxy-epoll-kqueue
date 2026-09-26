#!/usr/bin/env bash
# Demostración reproducible del proxy (docs/demo.md).
#
#   bash scripts/demo.sh start           backends A:3000, B:3001 y proxy (7777, 7778)
#   bash scripts/demo.sh start-noroute   proxy adicional sin ruta por defecto (7779)
#   bash scripts/demo.sh status          procesos de la demo y sus logs
#   bash scripts/demo.sh stats [--json]  estadísticas agregadas de los workers
#                                        (socket UNIX, scripts/proxy-stats.py)
#   bash scripts/demo.sh workers         PIDs del maestro y de sus workers
#   bash scripts/demo.sh health A 500    respuesta de /health del backend A (o B)
#   bash scripts/demo.sh backend-stop A  detiene el backend A (caída real)
#   bash scripts/demo.sh backend-start A lo vuelve a arrancar en su puerto
#   bash scripts/demo.sh reload FICHERO  copia FICHERO como configuración activa y
#                                        envía SIGHUP; muestra el resultado
#   bash scripts/demo.sh stop            detiene SOLO los procesos que lanzó este script
#
# El proxy de la demo lee $DEMO_DIR/proxy.toml (copia de proxy.toml), para
# que `reload` no modifique el fichero del repositorio. Al copiar, la ruta
# [stats].socket "/tmp/proxy-stats.sock" se sustituye por $DEMO_DIR/stats.sock
# (privada de la demo); se hace igual en `reload` para que no cambie.
#
# Estado en $DEMO_DIR (por defecto ${TMPDIR:-/tmp}/proxy-demo-<uid>): un fichero
# .pid y un .log por proceso. `stop` envía SIGTERM únicamente a esos PID, tras
# comprobar que su línea de comandos sigue siendo la esperada (un PID puede
# reutilizarse), espera y solo entonces usa SIGKILL con ese mismo PID. Nunca
# usa pkill/killall.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$PWD
DEMO_DIR=${DEMO_DIR:-${TMPDIR:-/tmp}/proxy-demo-$(id -u)}
PROXY_BIN=${PROXY_BIN:-$ROOT/build/src/proxy}
PYTHON=${PYTHON:-python3}

die() {
    echo "demo: $*" >&2
    exit 1
}

port_busy() {
    "$PYTHON" - "$1" <<'EOF'
import socket, sys
s = socket.socket()
try:
    s.bind(("127.0.0.1", int(sys.argv[1])))
except OSError:
    sys.exit(0)
sys.exit(1)
EOF
}

# launch NOMBRE MARCA COMANDO... ; MARCA debe aparecer en /proc/PID/cmdline.
launch() {
    local name=$1 ready=$2
    shift 2
    [[ -e "$DEMO_DIR/$name.pid" ]] && die "$name ya está en marcha (usa stop)"
    "$@" >"$DEMO_DIR/$name.log" 2>&1 &
    local pid=$!
    echo "$pid" >"$DEMO_DIR/$name.pid"
    for _ in $(seq 1 100); do
        if grep -q "$ready" "$DEMO_DIR/$name.log" 2>/dev/null; then
            echo "demo: $name en marcha (pid $pid)"
            return 0
        fi
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done
    cat "$DEMO_DIR/$name.log" >&2
    die "$name no arrancó"
}

install_config() {
    sed "s|\"/tmp/proxy-stats.sock\"|\"$DEMO_DIR/stats.sock\"|" "$1" >"$DEMO_DIR/proxy.toml.new"
    mv "$DEMO_DIR/proxy.toml.new" "$DEMO_DIR/proxy.toml"
}

# ¿Sigue siendo PID el proceso que lanzamos? (compara la línea de comandos)
is_ours() {
    local pid=$1 marker=$2
    kill -0 "$pid" 2>/dev/null || return 1
    if [[ -r /proc/$pid/cmdline ]]; then
        tr '\0' ' ' <"/proc/$pid/cmdline" | grep -qF -- "$marker"
    else
        ps -p "$pid" -o command= | grep -qF -- "$marker" # macOS
    fi
}

stop_one() {
    local name=$1 marker=$2 pidfile=$DEMO_DIR/$1.pid
    [[ -f "$pidfile" ]] || return 0
    local pid
    pid=$(cat "$pidfile")
    if is_ours "$pid" "$marker"; then
        kill -TERM "$pid"
        # El maestro espera a sus workers hasta shutdown_timeout_ms + 5 s.
        for _ in $(seq 1 200); do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.1
        done
        if kill -0 "$pid" 2>/dev/null && is_ours "$pid" "$marker"; then
            echo "demo: $name no terminó con SIGTERM; SIGKILL al pid $pid" >&2
            kill -KILL "$pid"
        fi
        echo "demo: $name detenido (pid $pid)"
    else
        echo "demo: $name (pid $pid) ya no estaba en marcha"
    fi
    rm -f "$pidfile"
}

case "${1:-}" in
start)
    [[ -x "$PROXY_BIN" ]] || die "no existe $PROXY_BIN; compila antes: meson setup build && meson compile -C build"
    for p in 3000 3001 7777 7778; do
        if port_busy "$p"; then die "el puerto $p está ocupado"; fi
    done
    mkdir -p "$DEMO_DIR"
    launch backend-A "escuchando" "$PYTHON" "$ROOT/tests/integration/backend.py" --port 3000 --name A --verbose
    launch backend-B "escuchando" "$PYTHON" "$ROOT/tests/integration/backend.py" --port 3001 --name B --verbose
    chmod 700 "$DEMO_DIR"
    install_config "$ROOT/proxy.toml"
    "$PROXY_BIN" -t -c "$DEMO_DIR/proxy.toml"
    launch proxy "proxy: listo" "$PROXY_BIN" -c "$DEMO_DIR/proxy.toml"
    echo "demo: logs en $DEMO_DIR"
    ;;
start-noroute)
    [[ -f "$DEMO_DIR/proxy.pid" ]] || die "arranca antes la demo principal (start)"
    if port_busy 7779; then die "el puerto 7779 está ocupado"; fi
    launch proxy-noroute "proxy: listo" "$PROXY_BIN" -c "$ROOT/examples/proxy-noroute.toml"
    ;;
stats)
    shift
    [[ -S "$DEMO_DIR/stats.sock" ]] || die "el proxy no está en marcha (no hay $DEMO_DIR/stats.sock)"
    "$PYTHON" "$ROOT/scripts/proxy-stats.py" "$DEMO_DIR/stats.sock" "$@"
    ;;
workers)
    pid=$(cat "$DEMO_DIR/proxy.pid" 2>/dev/null) || die "el proxy no está en marcha"
    echo "maestro pid=$pid"
    grep -oE "worker [0-9]+ (arrancado|repuesto) \(pid [0-9]+" "$DEMO_DIR/proxy.log" |
        tail -n 64 | sed -E 's/\(pid /pid=/'
    ;;
health)
    name=${2:?falta A o B}
    code=${3:?falta el código, p. ej. 500 o 200}
    case "$name" in A) port=3000 ;; B) port=3001 ;; *) die "backend desconocido: $name" ;; esac
    curl -s "http://127.0.0.1:$port/_set?health=$code" >/dev/null
    echo "demo: /health de $name responde ahora $code"
    ;;
backend-stop)
    name=${2:?falta A o B}
    stop_one "backend-$name" "backend.py"
    ;;
backend-start)
    name=${2:?falta A o B}
    case "$name" in A) port=3000 ;; B) port=3001 ;; *) die "backend desconocido: $name" ;; esac
    launch "backend-$name" "escuchando" "$PYTHON" "$ROOT/tests/integration/backend.py" \
        --port "$port" --name "$name" --verbose
    ;;
reload)
    file=${2:?falta el fichero de configuración}
    pid=$(cat "$DEMO_DIR/proxy.pid" 2>/dev/null) || die "el proxy no está en marcha"
    lines=$(wc -l <"$DEMO_DIR/proxy.log")
    install_config "$file"
    kill -HUP "$pid"
    for _ in $(seq 1 100); do
        if tail -n +"$((lines + 1))" "$DEMO_DIR/proxy.log" | grep -qE "recarga (aplicada|rechazada)"; then
            break
        fi
        sleep 0.1
    done
    tail -n +"$((lines + 1))" "$DEMO_DIR/proxy.log" | grep -E "recarga"
    ;;
status)
    for f in "$DEMO_DIR"/*.pid; do
        [[ -e "$f" ]] || { echo "demo: nada en marcha"; exit 0; }
        name=$(basename "$f" .pid)
        pid=$(cat "$f")
        if kill -0 "$pid" 2>/dev/null; then state=activo; else state=terminado; fi
        echo "$name pid=$pid $state log=$DEMO_DIR/$name.log"
    done
    ;;
stop)
    stop_one proxy-noroute "$PROXY_BIN"
    stop_one proxy "$PROXY_BIN"
    stop_one backend-A "backend.py"
    stop_one backend-B "backend.py"
    if [[ -d "$DEMO_DIR" ]]; then
        echo "demo: resumen de cierre del proxy:"
        grep -hE "shutdown conns|terminó|maestro: fin" "$DEMO_DIR"/proxy*.log 2>/dev/null || true
        rm -rf "$DEMO_DIR"
    fi
    ;;
*)
    echo "uso: $0 {start|start-noroute|status|stats|workers|health|backend-stop|backend-start|reload|stop}" >&2
    exit 2
    ;;
esac
