# Evidencias de la serie final del benchmark (2026-09-27 01:40 UTC)

Subconjunto de texto de la serie documentada en [`docs/benchmark.md`](../../benchmark.md).

## Procedencia

- **Archivo permanente completo** (fuera del repositorio):
  `~/proxy-benchmark-evidence/serie-final-20260927T0140Z/` en la máquina
  donde se midió. Contiene además la medición original
  (`01-medicion-original-QWIB56/`, copia exacta de `/tmp/proxy-bench.QWIB56`),
  el diagnóstico de los timeouts de wrk (`02-diagnostico-wrk/`, con
  `evidence/HALLAZGOS.md`) y los binarios y paquetes que aquí no se incluyen.
  Su integridad se comprueba con `sha256sum -c SHA256SUMS` allí.
- Los ficheros de este directorio se copiaron **tal cual** de ese archivo.
  `SHA256SUMS` lista sus hashes, que se comprobaron iguales a los del archivo
  permanente. El `.gitattributes` evita que Git convierta los fines de línea
  (las cabeceras HTTP llevan CRLF) o marque espacios finales de las salidas.
- Las rutas absolutas que aparecen dentro (`/tmp/pbf.WSNpxD/…`,
  `/tmp/claude-1000/…/wrk-investigation/…`) son las de la ejecución original:
  directorios temporales que ya no existen.

## Contenido

| Ruta | Qué es |
|---|---|
| `serie/nginx.conf`, `serie/proxy-6workers.toml` | Configuraciones usadas (copias de las originales) |
| `serie/config-diff.txt` | Diferencias con las originales: solo el pid de nginx y el socket de estadísticas |
| `serie/environment.txt`, `serie/build-bench-release.txt` | Entorno, SHA-256 de los binarios, opciones y flags del build release |
| `serie/warmup.txt`, `serie/run1.txt` … `run3.txt` | Salida completa de wrk-monotonic de cada ejecución, con comando y marcas de tiempo |
| `serie/stats-*.txt`, `serie/stats-*.json` | Estadísticas del proxy: inicial, antes y después de la serie |
| `serie/before-*`, `serie/after-*`, `serie/expected-body.bin` | Comprobaciones puntuales (una petición al backend y otra al proxy, antes y después): estado, cabeceras y cuerpo |
| `serie/clockmon.txt` | Saltos de `CLOCK_REALTIME` observados durante la serie |
| `serie/proxy.log`, `serie/nginx-error.log` | Logs del proxy (arranque y cierre) y de nginx |
| `serie/timeline.txt`, `serie/analysis.txt` | Cronología y extracción de resultados |
| `serie/run-final-series.sh` | Script que ejecutó la serie (rutas del entorno original) |
| `herramienta/wrk-monotonic.patch` | Parche mínimo sobre wrk 4.1.0-4build3 |
| `herramienta/wrk-monotonic-build.txt` | Fuente, versiones, comando de compilación y SHA-256 |
| `herramienta/build.sh` | Script de compilación usado |
| `herramienta/debian-cflags.txt`, `herramienta/debian-ldflags.txt` | Flags de `dpkg-buildflags` que lee `build.sh` |
| `herramienta/clockmon.c` | Monitor de saltos del reloj usado durante la serie |
| `herramienta/verificacion-wrk-monotonic/` | Verificación de la herramienta (65 s contra nginx directo); no forma parte de la serie |

No se incluyen binarios, paquetes `.deb` ni árboles de fuentes o de
compilación.

## Reproducir wrk-monotonic

Variante **modificada** de wrk, solo para medir; no sustituye a
`/usr/bin/wrk`.

1. Descargar de `http://archive.ubuntu.com/ubuntu/pool/universe/w/wrk/`:
   `wrk_4.1.0-4build3.dsc`, `wrk_4.1.0.orig.tar.gz` y
   `wrk_4.1.0-4build3.debian.tar.xz`, y comprobar sus SHA-256 con el `.dsc`
   (`orig`: `d8f1ba8b…6810f8f`, `debian`: `b8a54eb1…2ed`; completos en
   `herramienta/wrk-monotonic-build.txt`).
2. Descomprimir el `orig`, extraer dentro el `debian.tar.xz` y aplicar
   `debian/patches/debian-changes` (el único de `series`) con `patch -p1`.
3. Aplicar `herramienta/wrk-monotonic.patch` (SHA-256
   `26f47b90f453cce7a8faef52959e337de0544cb636bb91c1599c3394a1b0847b`):
   solo cambia el cuerpo de `time_us()` en `src/wrk.c`.
4. Obtener las cabeceras sin instalarlas:
   `apt-get download libluajit-5.1-dev=2.1.0+openresty20251030-1 libssl-dev=3.5.5-1ubuntu3.5 luajit=2.1.0+openresty20251030-1`
   y extraerlas con `dpkg-deb -x` en un prefijo privado; enlazar contra las
   bibliotecas de ejecución del sistema.
5. Compilar con `herramienta/build.sh DIR_FUENTE VERSION` (flags de
   `dpkg-buildflags`, `WITH_LUAJIT`/`WITH_OPENSSL` como `debian/rules`). El
   script busca, junto a sí mismo:
   - `prefix/`: los `-dev` extraídos (`dpkg-deb -x … prefix`);
   - `luajit-bin/`: el paquete `luajit` extraído (para generar el bytecode);
   - `linkdir/`: enlaces `libluajit-5.1.so`, `libssl.so` y `libcrypto.so` a
     `/usr/lib/x86_64-linux-gnu/libluajit-5.1.so.2`, `libssl.so.3` y
     `libcrypto.so.3`;
   - `evidence/debian-cflags.txt` y `evidence/debian-ldflags.txt`: los flags
     usados, incluidos aquí en `herramienta/`.
6. Comprobar: `wrk -v` debe mostrar `debian/4.1.0-4build3+wrk-monotonic`.
   El SHA-256 obtenido aquí fue
   `90a7220ac118c836a701b676522fe2e51025184655c3a83793ce89ff4c557429`
   (GCC 15.2.0, Ubuntu 26.04); con otro compilador o bibliotecas puede
   diferir aunque el código sea el mismo.
