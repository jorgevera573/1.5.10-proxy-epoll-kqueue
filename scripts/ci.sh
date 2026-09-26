#!/usr/bin/env bash
# Pasos de CI comunes a GitLab CI y GitHub Actions, para que ambos ejecuten
# exactamente los mismos comandos.
#
#   scripts/ci.sh tools                 venv .venv con ci/requirements.txt
#   scripts/ci.sh build-test DIR [ARGS] meson setup + compile + test
#   scripts/ci.sh valgrind DIR          unitarias y proxy de integración bajo Valgrind
#   scripts/ci.sh tsan DIR              todas las pruebas con ThreadSanitizer
#   scripts/ci.sh lint DIR              format-check + clang-tidy + cppcheck
#
# cmocka se toma siempre del wrap fijado (--force-fallback-for=cmocka): la
# misma versión (1.1.8) en Linux y macOS, sin depender del paquete del SO.
set -euo pipefail

cd "$(dirname "$0")/.."

VENV=${VENV:-.venv}
if [[ -d "$VENV/bin" ]]; then
    PATH="$(cd "$VENV/bin" && pwd):$PATH"
    export PATH
fi

case "${1:-}" in
tools)
    python3 -m venv "$VENV"
    "$VENV/bin/python" -m pip install --disable-pip-version-check -r ci/requirements.txt
    "$VENV/bin/meson" --version
    "$VENV/bin/ninja" --version
    ;;
build-test)
    dir=${2:?falta el directorio de build}
    shift 2
    meson setup "$dir" --force-fallback-for=cmocka "$@"
    meson compile -C "$dir"
    meson test -C "$dir" --print-errorlogs
    ;;
valgrind)
    dir=${2:?falta el directorio de build}
    vg='valgrind --error-exitcode=99 --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all --track-fds=yes'
    # Unitarias: Valgrind envuelve cada binario de prueba.
    meson test -C "$dir" --suite unit --print-errorlogs --timeout-multiplier 4 --wrapper "$vg"
    # Integración: Valgrind envuelve el proxy (no el intérprete de Python);
    # un error de Valgrind hace que el proxy salga con 99 y la prueba falle.
    PROXY_WRAPPER="$vg" TIME_SCALE=4 \
        meson test -C "$dir" --suite integration --print-errorlogs --timeout-multiplier 4
    ;;
tsan)
    # ThreadSanitizer en build aparte: hilo de health, recarga y señales.
    dir=${2:?falta el directorio de build}
    meson setup "$dir" --force-fallback-for=cmocka -Db_sanitize=thread -Db_lundef=false
    meson compile -C "$dir"
    TSAN_OPTIONS="halt_on_error=1 exitcode=66" \
        meson test -C "$dir" --print-errorlogs --timeout-multiplier 3
    ;;
lint)
    dir=${2:?falta el directorio de build}
    bash scripts/lint.sh format-check
    bash scripts/lint.sh tidy "$dir"
    bash scripts/lint.sh cppcheck
    ;;
*)
    echo "uso: $0 {tools|build-test DIR [ARGS]|valgrind DIR|tsan DIR|lint DIR}" >&2
    exit 2
    ;;
esac
