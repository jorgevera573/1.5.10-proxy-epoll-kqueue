#!/usr/bin/env bash
# Compila una copia de wrk 4.1.0-4build3 igual que debian/rules
# (WITH_LUAJIT/WITH_OPENSSL), con las cabeceras de un prefijo privado
# (paquetes -dev extraídos, no instalados) y las bibliotecas de ejecución
# del sistema (las mismas que usa /usr/bin/wrk).
#
# - CFLAGS/LDFLAGS van por el entorno: así se suman a los del Makefile
#   (-std=c99 -Wall -O2 -D_REENTRANT ...) en vez de sustituirlos.
# - PATH limpio: la regla de bytecode.o expande $(PATH) dentro de sh -c y el
#   PATH de WSL contiene rutas de Windows con paréntesis.
#
# Uso: build.sh DIRECTORIO_FUENTE VERSION
set -euo pipefail
W=$(cd "$(dirname "$0")" && pwd)
dir=$1
ver=$2
cd "$dir"
export CFLAGS="$(cat "$W/evidence/debian-cflags.txt") -I$W/prefix/usr/include/luajit-2.1 -I$W/prefix/usr/include/x86_64-linux-gnu"
export LDFLAGS="$(cat "$W/evidence/debian-ldflags.txt") -L$W/linkdir"
export PATH="$W/luajit-bin/usr/bin:/usr/bin:/bin"
make -s PKG_CONFIG=/bin/false \
    WITH_LUAJIT="$W/prefix/usr" WITH_OPENSSL="$W/prefix/usr" VER="$ver"
