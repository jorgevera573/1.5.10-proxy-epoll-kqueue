# Benchmark

Medición de rendimiento del proxy en una sola máquina (WSL2, loopback). Dos
series del 2026-09-27, con el mismo build y el mismo generador:

| Serie | Qué mide | Evidencias en texto |
|---|---|---|
| Serie final (01:40 UTC) | Proxy con 6 workers: calentamiento y 3 × 30 s | [`serie-final-20260927T0140Z/`](benchmark-evidence/serie-final-20260927T0140Z/README.md) |
| Comparación directo/proxy (02:08 UTC) | nginx directo frente al proxy: calentamiento por destino y 3 × 30 s por destino, alternando el orden | [`comparacion-directo-proxy-20260927T0208Z/`](benchmark-evidence/comparacion-directo-proxy-20260927T0208Z/README.md) |

## Resumen

Serie final del 2026-09-27 (01:40 UTC), commit `e52c468`, build release,
proxy con 6 workers delante de nginx, 100 conexiones keep-alive y
respuestas de 16 bytes: **mediana de 81.974,62 req/s** en tres ejecuciones de
30 s (80.534,42–83.881,57), latencia mediana (p50) de 1,13 ms y p99 de
2,47 ms (medianas de las tres ejecuciones), sin errores de socket ni
respuestas con estado > 399.

**Objetivo ≥ 50.000 req/s: alcanzado en este escenario**, y solo en él: una
máquina compartida por cliente, proxy y backend, por loopback, con respuestas
mínimas. No es una capacidad máxima ni se puede extrapolar a otras máquinas,
redes, tamaños de respuesta o niveles de concurrencia (§ Limitaciones). La
comparación posterior dio un resultado del mismo orden para el proxy
(mediana 84.696,99 req/s).

**Comparación directo/proxy** (02:08 UTC, § Comparación directo/proxy):
mediana de 272.084,64 req/s contra nginx directo y 84.696,99 req/s a través
del proxy; **relación proxy/directo 0,311** (0,309 / 0,322 / 0,313 por
ronda), p50 0,334 ms frente a 1,12 ms. Sin errores en ninguna ejecución. La
diferencia incluye que el proxy abre una conexión TCP nueva con nginx por
petición (no reutiliza conexiones con el upstream), mientras que el acceso
directo mantiene 100 conexiones keep-alive.

## Entorno

| | |
|---|---|
| Máquina | Intel Core Ultra 5 125H; la VM WSL2 (Hyper-V) expone 18 CPU lógicas (topología virtual de 9 núcleos × 2 hilos) y 7,5 GiB. Hardware consultado con `lscpu`/`free` al documentar; durante la serie se registró `nproc` = 18 |
| Sistema | Ubuntu 26.04 LTS sobre WSL2, kernel `6.18.33.2-microsoft-standard-WSL2`, clocksource `tsc`, chrony siguiendo `PHC0` |
| Proxy | commit `e52c46857c288676ed0b2bb6d5c018038e4ee84b`, binario `build-bench-release/src/proxy`, SHA-256 `b3bee876fdc6718c44b58e8bbee8ad50b2b8b29dbab526ff6a9536ee9dc077be`; sin cambios en `src/` y `ninja -n` sin trabajo pendiente en el momento de medir |
| Compilador del proxy | GCC 15.2.0 (Ubuntu 15.2.0-16ubuntu1), ld.bfd 2.46 |
| Backend | nginx 1.28.3 (Ubuntu), 2 workers |
| Generador | `wrk-monotonic` (§ Generador), 4 hilos |
| Red | todo por `127.0.0.1` (loopback) |

### Opciones del build release

Meson: `-Dbuildtype=release -Db_sanitize=none -Dtests=disabled
-Dintegration_tests=disabled`. Opciones efectivas: `buildtype=release`,
`optimization=3`, `debug=false`, `b_ndebug=if-release` (→ `-DNDEBUG`),
`b_lto=false`, `b_pgo=off`, `b_sanitize=none`, `io_backend=auto` (epoll),
`c_std=c11`. Flags reales de compilación (`main.c`):

```
-DNDEBUG -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -Werror -std=c11 -O3
-Wshadow -Wconversion -Wsign-conversion -Wstrict-prototypes -Wmissing-prototypes
-Wold-style-definition -Wpointer-arith -Wcast-align -Wcast-qual -Wwrite-strings -Wformat=2
-Wundef -Wvla -Wswitch-enum -Wnull-dereference -Wdouble-promotion
-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -pthread
```

Sin símbolos de depuración, sin LTO ni sanitizers; el binario no contiene
los ganchos de prueba (`PROXY_TEST_*`). Detalle en
`serie/build-bench-release.txt`.

## Configuración completa

nginx (`serie/nginx.conf`; respecto al original solo cambia la ruta del
`pid`):

```nginx
worker_processes 2;
pid /tmp/pbf.WSNpxD/nginx.pid;
error_log error.log warn;

events {
    worker_connections 4096;
}

http {
    access_log off;
    default_type text/plain;
    keepalive_timeout 60s;
    keepalive_requests 1000000;

    server {
        listen 127.0.0.1:18080 backlog=1024;
        server_name bench.local;

        location = /bench {
            return 200 "proxy-benchmark\n";
        }

        location / {
            return 404;
        }
    }
}
```

Proxy (`serie/proxy-6workers.toml`; respecto al original solo cambia la ruta
del socket de estadísticas):

```toml
[server]
workers = 6
max_connections = 1024
shutdown_timeout_ms = 10000

[limits]
max_requests_per_connection = 1000000

[[frontend]]
listen = "127.0.0.1:18081"

[[pool]]
name = "bench"
algorithm = "round_robin"

[[pool.backend]]
address = "127.0.0.1:18080"

[routing]
default_pool = "bench"

[stats]
socket = "/tmp/pbf.WSNpxD/stats.sock"
mode = 0o600
```

El resto de claves toma los valores por defecto de
[`configuration.md`](configuration.md).

## Generador: wrk-monotonic (variante modificada de wrk)

La serie **no** usa el `wrk` instalado sino una variante identificada como
`debian/4.1.0-4build3+wrk-monotonic`:

- **Fuente exacto**: paquete de Ubuntu `wrk_4.1.0-4build3`
  (`archive.ubuntu.com/ubuntu/pool/universe/w/wrk/`). SHA-256 comprobados
  contra el `.dsc`:
  `wrk_4.1.0.orig.tar.gz` =
  `d8f1ba8b70ae5e10aef63ef4d5b6520bc0da103f7f1c59c79f50e2efd6810f8f`,
  `wrk_4.1.0-4build3.debian.tar.xz` =
  `b8a54eb1ac13e220bbe38662807c1b1da3f82718eb11f1ecf0fc5135f74ba2ed`.
  El `.deb` amd64 del archivo es idéntico al `/usr/bin/wrk` instalado
  (SHA-256 `f6536e3f9479584fa2b4a97d12c56671487be9f1011f309dc7a3b1bb0f6ea06c`),
  así que es el fuente del wrk que se usaba. Se aplica el único parche de
  Debian (`debian-changes`).
- **Parche mínimo**
  ([`herramienta/wrk-monotonic.patch`](benchmark-evidence/serie-final-20260927T0140Z/herramienta/wrk-monotonic.patch),
  SHA-256 `26f47b90f453cce7a8faef52959e337de0544cb636bb91c1599c3394a1b0847b`):
  solo el cuerpo de `time_us()` en `src/wrk.c` pasa de `gettimeofday` a
  `clock_gettime(CLOCK_MONOTONIC)`. Afecta a la latencia de cada petición,
  a la duración total (y por tanto a req/s) y al muestreo de `Req/Sec`. El
  timeout (2 s) y el resto de la lógica no cambian; los temporizadores
  internos de `ae.c` siguen con `gettimeofday` (deciden cuándo corre el tick
  de 100 ms, no qué se mide).
- **Verificado en el binario final** (desensamblado): `time_us`,
  `socket_writeable`, `response_complete` y `record_rate` llaman a
  `clock_gettime` con el id `0x1` (`CLOCK_MONOTONIC`); `main` usa `time_us`
  para la duración; `gettimeofday` solo queda en `aeCreateTimeEvent` y
  `aeProcessEvents`.
- **Compilación**: como `debian/rules` (`WITH_LUAJIT`/`WITH_OPENSSL`, flags
  de `dpkg-buildflags`), GCC 15.2.0, cabeceras de `libluajit-5.1-dev`
  2.1.0+openresty20251030-1 y `libssl-dev` 3.5.5-1ubuntu3.5 extraídas sin
  instalar, bibliotecas de ejecución del sistema. Pasos en el
  [README de las evidencias](benchmark-evidence/serie-final-20260927T0140Z/README.md#reproducir-wrk-monotonic).
- **SHA-256 del binario**:
  `90a7220ac118c836a701b676522fe2e51025184655c3a83793ce89ff4c557429`.
- **Verificación funcional** (no forma parte de la serie): 65 s contra nginx
  directo con 3 retrocesos del reloj en la ventana → 0 timeouts
  (`herramienta/verificacion-wrk-monotonic/`).

Por qué: el wrk instalado mide con el reloj de pared y en esta máquina ese
reloj retrocede periódicamente, lo que produce "timeouts" falsos (§ Diagnóstico
del reloj).

## Protocolo y comandos (serie final, 01:40 UTC)

Una sola pasada, sin repeticiones selectivas. Orden
(`serie/run-final-series.sh`):

1. Preparar un directorio de trabajo y adaptar las rutas temporales de las
   configuraciones (`/tmp/pbf.WSNpxD` era el de la ejecución original y ya
   no existe); después arrancar nginx y el proxy:
   ```bash
   DIR=$(mktemp -d /tmp/pbf.XXXXXX)
   EV=docs/benchmark-evidence/serie-final-20260927T0140Z/serie
   cp "$EV/nginx.conf" "$EV/proxy-6workers.toml" "$DIR/"
   sed -i "s|/tmp/pbf.WSNpxD|$DIR|" "$DIR/nginx.conf" "$DIR/proxy-6workers.toml"
   nginx -p "$DIR/" -c "$DIR/nginx.conf" -e "$DIR/error.log"
   build-bench-release/src/proxy -c "$DIR/proxy-6workers.toml"
   ```
   En los comandos siguientes, `wrk-monotonic` es la ruta al binario
   compilado (§ Generador) y el socket de estadísticas es
   `$DIR/stats.sock`.
2. Comprobación puntual antes: una petición `curl` al backend
   (`:18080/bench`) y otra al proxy (`:18081/bench`).
3. Estadísticas del proxy: `python3 scripts/proxy-stats.py "$DIR/stats.sock"`
   (y `--json`).
4. Calentamiento (como la medición original, sin `--latency`):
   ```bash
   wrk-monotonic -t4 -c100 -d10s --timeout 2s http://127.0.0.1:18081/bench
   ```
5. Estadísticas antes de la serie.
6. Tres mediciones seguidas:
   ```bash
   wrk-monotonic -t4 -c100 -d30s --timeout 2s --latency http://127.0.0.1:18081/bench
   ```
7. Estadísticas después, comprobación puntual después, cierre ordenado del
   proxy (`SIGTERM`) y de nginx (`-s quit`).

`clockmon` (`herramienta/clockmon.c`) observó el reloj durante toda la
serie.

## Resultados (serie final, 01:40 UTC)

| Ejecución | Inicio (UTC) | Duración | Completadas | req/s | Media | p50 | p75 | p90 | p99 | Máx | Errores de socket | Estado > 399 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Calentamiento | 01:40:49 | 10,00 s | 860.276 | 86.021,56 | 1,17 ms | – | – | – | – | 27,91 ms | 0 | 0 |
| 1 | 01:40:59 | 30,01 s | 2.459.692 | 81.974,62 | 1,25 ms | 1,13 ms | 1,44 ms | 1,76 ms | 3,14 ms | 37,21 ms | 0 | 0 |
| 2 | 01:41:29 | 30,00 s | 2.416.372 | 80.534,42 | 1,24 ms | 1,16 ms | 1,48 ms | 1,78 ms | 2,47 ms | 11,56 ms | 0 | 0 |
| 3 | 01:41:59 | 30,00 s | 2.516.802 | 83.881,57 | 1,19 ms | 1,12 ms | 1,41 ms | 1,72 ms | 2,40 ms | 11,61 ms | 0 | 0 |
| **Mediana (1–3)** | | | | **81.974,62** | 1,24 ms | 1,13 ms | 1,44 ms | 1,76 ms | 2,47 ms | 11,61 ms | 0 | 0 |

- Rango de req/s en las mediciones: 80.534,42–83.881,57 (−1,8 % y +2,3 %
  respecto a la mediana). Las medianas por columna se calculan por separado: no son una
  ejecución concreta.
- "Errores de socket 0": wrk solo imprime esa línea si algún contador
  (connect, read, write, timeout) es distinto de 0; no apareció en ninguna
  ejecución.
- Estados HTTP: wrk solo cuenta las respuestas con estado > 399
  ("Non-2xx or 3xx responses"); no distingue 2xx de 3xx ni da desglose.
- Estadísticas del proxy (`serie/stats-*`): 8.253.512 peticiones atendidas
  en la serie (calentamiento incluido), 0 respuestas 4xx y 0 5xx generadas
  por el proxy, 0 fallos del backend, 0 conexiones rechazadas, 0 reinicios
  de workers; al final, 0 conexiones abiertas.
- Cierre: los 6 workers y el maestro terminaron con código 0 y contabilidad
  a cero (`serie/proxy.log`).
- Comprobaciones puntuales antes y después: estado 200, 16 bytes y cuerpo
  idéntico a `proxy-benchmark\n`, en el backend y a través del proxy. Son
  **cuatro peticiones en total** (dos antes y dos después): no validan el
  estado ni el cuerpo de cada respuesta de la serie.

## Comparación directo/proxy

Serie separada, a las 02:08 UTC del mismo día, para cumplir el criterio de
AGENTS.md y `requirements.md` de comparar el acceso directo con el proxy.

**Montaje**: el mismo binario del proxy (6 workers,
SHA-256 `b3bee876…c077be`), la misma configuración de nginx (2 workers,
16 bytes) y el mismo wrk-monotonic (`-t4 -c100 --timeout 2s`). nginx y el
proxy se arrancan una vez y siguen activos toda la comparación; mientras se
mide el acceso directo, el proxy está en reposo (su contador de peticiones no
cambia: comprobado tras cada ejecución).

**Orden** (una sola pasada; se registran todas las ejecuciones, sin
selección; `run-comparison.sh`):

1. Calentamiento de 10 s por destino: directo y después proxy (sin
   `--latency`).
2. Ronda 1: directo → proxy. Ronda 2: proxy → directo. Ronda 3: directo →
   proxy. Cada medición, 30 s con `--latency`, contra
   `http://127.0.0.1:18080/bench` (directo) o `…:18081/bench` (proxy).

Para reproducirla se preparan nginx y el proxy igual que en la serie final
(§ Protocolo y comandos), con las configuraciones de
`docs/benchmark-evidence/comparacion-directo-proxy-20260927T0208Z/` y
sustituyendo su ruta temporal `/tmp/pbc.W4XKOt` por el directorio de
trabajo; el orden de las ejecuciones está en `run-comparison.sh`.

Comprobación puntual antes y después: 200, 16 bytes y cuerpo esperado en
ambos destinos. Son **cuatro peticiones en total** (una por destino antes y
otra después): no validan cada respuesta.

| Ejecución | Destino | req/s | Completadas | Media | p50 | p75 | p90 | p99 | Máx | Errores | Estado > 399 | Salto de reloj |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Calentamiento | directo | 321.931,16 | 3.220.231 | 305,44 µs | – | – | – | – | 4,13 ms | 0 | 0 | −2,15 ms |
| Calentamiento | proxy | 88.707,28 | 895.908 | 1,21 ms | – | – | – | – | 37,87 ms | 0 | 0 | – |
| Ronda 1 | directo | 274.032,89 | 8.221.633 | 380,51 µs | 334 µs | 437 µs | 577 µs | 1,39 ms | 14,29 ms | 0 | 0 | −1,45 ms |
| Ronda 1 | proxy | 84.696,99 | 2.541.385 | 1,18 ms | 1,12 ms | 1,38 ms | 1,76 ms | 2,49 ms | 41,84 ms | 0 | 0 | −1,97 ms |
| Ronda 2 | proxy | 87.632,49 | 2.629.334 | 1,14 ms | 1,08 ms | 1,35 ms | 1,63 ms | 2,36 ms | 29,39 ms | 0 | 0 | −1,71 ms |
| Ronda 2 | directo | 272.084,64 | 8.163.237 | 372,30 µs | 334 µs | 411 µs | 532 µs | 1,15 ms | 5,39 ms | 0 | 0 | −1,91 ms |
| Ronda 3 | directo | 266.924,96 | 8.008.295 | 381,96 µs | 338 µs | 415 µs | 554 µs | 1,23 ms | 6,24 ms | 0 | 0 | −1,23 ms |
| Ronda 3 | proxy | 83.613,99 | 2.509.008 | 1,20 ms | 1,13 ms | 1,42 ms | 1,70 ms | 2,35 ms | 14,95 ms | 0 | 0 | −2,09 ms |

Medianas de las tres mediciones de 30 s por destino (cada columna por
separado) y relación proxy/directo:

| | req/s | Media | p50 | p75 | p90 | p99 | Máx |
|---|---|---|---|---|---|---|---|
| Directo | 272.084,64 | 0,381 ms | 0,334 ms | 0,415 ms | 0,554 ms | 1,230 ms | 6,24 ms |
| Proxy | 84.696,99 | 1,180 ms | 1,120 ms | 1,380 ms | 1,700 ms | 2,360 ms | 29,39 ms |
| **Proxy / directo** | **0,311** | 3,10× | 3,35× | 3,33× | 3,07× | 1,92× | – |

- Relación req/s proxy/directo por ronda: 0,309, 0,322 y 0,313. La única
  ronda con el proxy primero (la 2) dio la relación más alta, pero con una
  ronda de cada orden no se puede separar el efecto del orden de la
  variación entre ejecuciones; lo que sí se observa es que la relación
  quedó entre 0,309 y 0,322 en los dos órdenes.
- Estadísticas del proxy tras cada ejecución: 0 respuestas 4xx y 0 5xx
  propias, 0 fallos del backend, 0 reinicios; 101 conexiones aceptadas por
  ejecución contra el proxy (las 100 de wrk y una conexión de prueba que
  wrk abre y cierra al resolver la dirección, `wrk.lua:15`); 0 peticiones
  durante las mediciones directas. El proxy contó entre 84 y 92 peticiones
  más que las que wrk completó en cada ejecución (en vuelo al cierre).
- `upstream_reusable` = 0: ninguna respuesta de nginx habría permitido
  reutilizar la conexión, porque el proxy pide `Connection: close`.
- Reloj: 9 retrocesos en los 260 s observados (uno dentro de casi cada
  ejecución); wrk-monotonic no registró ningún timeout.
- Cierre: proxy con código 0 y contabilidad a cero; nginx detenido.

**Cómo leer la relación**:

- **No es el coste aislado del proxy.** Las tres partes comparten las 18 CPU
  lógicas: en la medición directa, wrk (4 hilos) compite solo con nginx
  (2 workers); a través del proxy compite además con 6 workers del proxy y
  con nginx atendiendo las conexiones de este. Es probable que el reparto de
  la misma CPU entre más procesos y el doble paso por loopback expliquen
  parte de la caída, pero en esta comparación no se midió el uso de CPU por
  proceso: no se puede cuantificar qué parte.
- **Reutilización de conexiones distinta.** Directo: 100 conexiones
  keep-alive de wrk a nginx durante toda la medición. Proxy: las 100
  conexiones de wrk son keep-alive con el proxy, pero el proxy abre una
  conexión TCP nueva con nginx para **cada** petición (8,58 millones en la
  comparación) y la cierra tras la respuesta (§ 5.5 de
  [`architecture.md`](architecture.md)). Ese coste **sí está medido**: forma
  parte de la cifra del proxy. Lo que no mide la comparación es cómo se
  comportaría un proxy con pool de conexiones hacia el upstream.
- Respuesta de 16 bytes: domina el coste por petición, no la transferencia.

Mediciones directas anteriores, en otras condiciones y sustituidas por esta
comparación: verificación de wrk-monotonic (65 s, sin calentamiento,
281.230,21 req/s) y la medición original con el wrk instalado (30 s,
300.411,96 req/s, 96 avisos de timeout). La causa de esos 96 avisos no se
instrumentó; que sean retrocesos del reloj de pared es la hipótesis de
§ Diagnóstico del reloj.

## Diagnóstico del reloj

Las mediciones anteriores con el wrk instalado mostraban avisos de timeout
aunque la latencia máxima reportada no pasaba de 40 ms. Investigado con el
fuente exacto de wrk (detalle completo en el archivo permanente,
`02-diagnostico-wrk/evidence/HALLAZGOS.md`):

- wrk 4.1.0-4build3 solo incrementa `errors.timeout` cuando **llega** una
  respuesta cuya latencia, medida con `gettimeofday`, es ≥ 2 s; la resta es
  sin signo. Un retroceso del reloj de pared convierte una latencia real de
  microsegundos en un valor enorme → "timeout", fuera del histograma.
- En esta máquina `CLOCK_REALTIME` retrocede de forma periódica: todos los
  saltos observados fueron hacia atrás, de 1,3–2,7 ms, cada 30,001 s, en
  reposo y con carga. Durante esta serie hubo 6 en los 170 s observados
  (`serie/clockmon.txt`): uno dentro de cada medición de 30 s, y
  wrk-monotonic contó 0 timeouts en las tres.
- **Demostrado para los 451 avisos de 8 ejecuciones instrumentadas**: todos
  tienen latencia de pared negativa, latencia monotónica de 0,18–2,3 ms y
  ocurren a ≤ 5 ms de un salto detectado; en 33.798.599 muestras ninguna
  alcanzó 2 s con reloj monotónico (máximo 44,9 ms). El proxy registró 0 5xx,
  0 fallos y 0 reinicios.
- **Hipótesis**: que los **194 avisos** de las mediciones originales (y los
  de las otras ejecuciones previas) tengan la misma causa. Mismo entorno y
  patrón compatible (≤ 100 por ejecución de 30 s, 96 incluso contra nginx
  directo), pero entonces no se registró el reloj ni la latencia monotónica.
- **Sin identificar**: qué ajusta el reloj cada 30 s. chrony no lo registra
  y su intervalo es de 8 s; ~2 ms/30 s ≈ 66 ppm, parecido a la corrección de
  frecuencia que informa chrony (62,8–65,8 ppm), pero no está verificado.

## Limitaciones

- **Loopback**: sin red real; no incluye latencia, pérdidas ni límites de
  una interfaz física.
- **Máquina compartida**: wrk (4 hilos), el proxy (6 workers) y nginx
  (2 workers) compiten por las mismas 18 CPU lógicas de una máquina virtual
  WSL2; el resultado mezcla el coste de los tres, y la relación
  proxy/directo no aísla el coste del proxy.
- **Conexiones con el upstream**: el proxy abre y cierra una conexión TCP
  con nginx por petición; el acceso directo reutiliza 100 conexiones.
- **Respuesta de 16 bytes**, cabeceras mínimas, un único backend, 100
  conexiones keep-alive entre wrk y el proxy: mide sobre todo el coste por
  petición del proxy, que incluye abrir y cerrar una conexión con el
  backend; no mide el reenvío de cuerpos grandes ni el coste de establecer
  conexiones de cliente (las de wrk se abren una vez por ejecución).
- **Peticiones pendientes al cierre**: al terminar cada ejecución wrk
  abandona las peticiones en vuelo; no cuentan como completadas, ni como
  timeout, ni en el histograma, y no se avisa. En la serie final el proxy
  atendió 370 peticiones más que las que wrk registró (≤ 4 ejecuciones × 100
  conexiones); en la comparación, entre 84 y 92 más por ejecución.
- **Qué cuenta wrk como timeout**: solo respuestas recibidas con latencia
  ≥ 2 s; una petición sin respuesta no se cuenta en ningún lado.
- **Comprobaciones del cuerpo puntuales**: cuatro peticiones HTTP en total
  por serie (dos antes y dos después); no hay validación del contenido de
  cada respuesta.
- **Dos series** en una sola máquina y la misma noche: la serie final (tres
  mediciones de 30 s del proxy) y la comparación (tres mediciones de 30 s
  por destino). No cubren variaciones entre días, máquinas ni
  configuraciones de workers.
- **Generador modificado**: las cifras dependen de wrk-monotonic; el
  cambio solo afecta al reloj de medición.

## Evidencias y procedencia

- En el repositorio (texto, copiado tal cual y con `SHA256SUMS`):
  [`docs/benchmark-evidence/serie-final-20260927T0140Z/`](benchmark-evidence/serie-final-20260927T0140Z/README.md).
- Archivo permanente completo, fuera del repositorio, en la máquina de
  medición: `~/proxy-benchmark-evidence/serie-final-20260927T0140Z/`, con
  `01-medicion-original-QWIB56/` (mediciones anteriores: backend directo,
  3 y 6 workers con el wrk instalado; copia exacta de
  `/tmp/proxy-bench.QWIB56`), `02-diagnostico-wrk/` (fuentes verificados,
  instrumentación, experimentos E1–E5, `evidence/HALLAZGOS.md`) y
  `03-serie-final/` (la serie final, con el binario de wrk-monotonic).
  Integridad con `sha256sum -c SHA256SUMS` en ese directorio.
- Comparación directo/proxy: en el repositorio,
  [`docs/benchmark-evidence/comparacion-directo-proxy-20260927T0208Z/`](benchmark-evidence/comparacion-directo-proxy-20260927T0208Z/README.md);
  archivo permanente en
  `~/proxy-benchmark-evidence/comparacion-directo-proxy-20260927T0208Z/`
  (con `SHA256SUMS`).
