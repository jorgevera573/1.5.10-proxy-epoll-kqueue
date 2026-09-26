#!/usr/bin/env bash
# Controles de estilo y análisis estático SOLO sobre código propio.
#
#   scripts/lint.sh format-check   clang-format --dry-run -Werror
#   scripts/lint.sh format         clang-format -i
#   scripts/lint.sh tidy [BUILD]   clang-tidy con BUILD/compile_commands.json
#   scripts/lint.sh cppcheck       cppcheck
#
# Se analizan src/ y tests/. subprojects/ (cmocka descargado por su wrap) y
# los directorios de build quedan fuera. clang-tidy solo recibe los .c que
# están en la base de compilación (p. ej. en Linux no se compila
# io_event_kqueue.c y por tanto no se analiza allí).
#
# Versiones fijadas en CI: clang-format 21.1.8, clang-tidy 21.1.6.
set -euo pipefail

cd "$(dirname "$0")/.."

CLANG_FORMAT=${CLANG_FORMAT:-clang-format}
CLANG_TIDY=${CLANG_TIDY:-clang-tidy}
CPPCHECK=${CPPCHECK:-cppcheck}

own_sources() {
    find src tests -type f \( -name '*.c' -o -name '*.h' \) | LC_ALL=C sort
}

case "${1:-}" in
format-check)
    "$CLANG_FORMAT" --version
    own_sources | xargs "$CLANG_FORMAT" --dry-run -Werror
    echo "format-check: OK"
    ;;
format)
    own_sources | xargs "$CLANG_FORMAT" -i
    ;;
tidy)
    build=${2:-build}
    db="$build/compile_commands.json"
    if [[ ! -f "$db" ]]; then
        echo "no existe $db; ejecuta antes: meson setup $build" >&2
        exit 2
    fi
    "$CLANG_TIDY" --version
    # Solo ficheros propios presentes en la base de compilación.
    mapfile -t files < <(own_sources | grep '\.c$' | while read -r f; do
        if grep -q "\"[^\"]*/$f\"\|\"\.\./$f\"" "$db"; then echo "$f"; fi
    done)
    if [[ ${#files[@]} -eq 0 ]]; then
        echo "tidy: ningún fichero propio en $db" >&2
        exit 2
    fi
    "$CLANG_TIDY" -p "$build" --quiet "${files[@]}"
    echo "tidy: OK (${#files[@]} ficheros)"
    ;;
cppcheck)
    "$CPPCHECK" --version
    "$CPPCHECK" --std=c11 --enable=warning,style,portability,performance \
        --error-exitcode=1 --inline-suppr --quiet -I src src tests
    echo "cppcheck: OK"
    ;;
*)
    echo "uso: $0 {format-check|format|tidy [build]|cppcheck}" >&2
    exit 2
    ;;
esac
