# 🚄 Proxy inverso L7 — C11 + epoll/kqueue

Proxy inverso HTTP/1.x asíncrono en **C11**, sin frameworks, portable entre
Linux (`epoll`) y macOS (`kqueue`): un maestro supervisa N procesos worker,
cada uno con su event loop edge-triggered. Proyecto de sistemas del curso.

**Rendimiento medido: mediana de 81.974,62 req/s** (3 × 30 s, 6 workers,
commit `e52c468`) en una sola máquina WSL2 por loopback, con 100 conexiones
y respuestas de 16 bytes. El objetivo de ≥ 50.000 req/s se alcanza en ese
escenario; no se generaliza a otros ([docs/benchmark.md](docs/benchmark.md)).

## 🏗️ Arquitectura

```
                 SIGHUP / SIGTERM            socket UNIX de estadísticas (JSON)
                        │                                  ▲
              ┌─────────▼──────────────────────────────────┴──┐
              │ maestro (1 hilo): config, supervisión, recarga │
              └───┬───────────────── socketpair IPC ──────┬────┘
          ┌───────▼────────┐                     ┌────────▼───────┐
          │ worker 0       │        ...          │ worker N-1     │
          │ event loop     │                     │ event loop     │
          │ hilo health    │                     │ hilo health    │
          │ hilo log       │                     │ hilo log       │
          └───────┬────────┘                     └────────┬───────┘
 cliente ─► listener ─► parser HTTP ─► router (Host) ─► pool de backends ─► upstream
```

- **Maestro sin hilos**: lee y valida la configuración, hace `fork` de los
  workers (con señales bloqueadas), los supervisa (`SIGCHLD`, reposición con
  backoff y límite `max_restarts`), coordina la recarga y el cierre y sirve
  las estadísticas. No atiende tráfico.
- **Workers**: un event loop edge-triggered (`EPOLLET` / `EV_CLEAR`) sobre la
  API común `io_event`, más un hilo de health checks y un hilo consumidor del
  log, creados después del `fork`.
- **Listeners por plataforma**: en Linux cada worker abre su socket con
  `SO_REUSEPORT` y el núcleo reparte; en macOS `SO_REUSEPORT` no reparte, así
  que el maestro abre un socket por frontend antes del `fork` y los workers
  aceptan de esa cola compartida.
- **IPC**: tramas sobre `socketpair` no bloqueante entre maestro y cada worker
  (listo, preparar/activar/abortar, estadísticas).
- **Recarga coordinada (`SIGHUP` al maestro)**: el maestro lee el fichero una
  vez y envía los mismos bytes a todos los workers (**preparar**); si todos
  confirman, decide y envía **activar**. Cada worker activa la nueva
  generación al procesar su mensaje: **no es simultáneo**. No hay vuelta
  atrás tras la decisión. Una configuración inválida o no recargable se
  rechaza y nada cambia. Las peticiones en curso terminan con su generación.
- **Plazos de la activación**: `ipc_timeout_ms` limita solo la espera de
  **confirmación** de la activación. Un worker que no confirma a tiempo se
  retira (SIGTERM; SIGKILL si sigue vivo tras el mayor `shutdown_timeout_ms`,
  antiguo o nuevo, más 5 s) y, solo al recogerlo, se repone con backoff y la
  generación nueva. Por eso la retirada y la recuperación de la capacidad
  completa pueden tardar bastante más que `ipc_timeout_ms`.

Detalle: [docs/architecture.md](docs/architecture.md) (§14: maestro, IPC,
recarga, supervisión, log y estadísticas).

### Módulos (`src/`)

| Módulo | Responsabilidad |
|---|---|
| `io_event` (+ `io_event_epoll.c`, `io_event_kqueue.c`) | API común `io_loop_create/add/mod/del/run/stop`; Meson elige el backend |
| `timer` | Min-heap de temporizadores del event loop |
| `buffer_pool` | Arena `mmap` de slots de 16 KB con freelist |
| `listener` | Sockets de escucha no bloqueantes; modelo por plataforma |
| `conn` | Conexiones cliente y upstream, reenvío con backpressure, plazos |
| `host` | Normalización de `Host` (nombres, IPv6 sin zona, puerto) |
| `http_parser` | Parser incremental de petición y respuesta; framing de cuerpos |
| `http_forward` | Cabeceras hacia el upstream (`X-Forwarded-For/Proto`, `X-Real-IP`) y hacia el cliente |
| `router` | Exacto (djb2) → wildcard `*.dominio` → `default` → 502 |
| `config` | TOML (tomlc99), validación y compatibilidad de recarga |
| `backend_pool` | Generaciones; `round_robin`, `weighted`, `least_conn`; salud activa y pasiva |
| `health` | Sondas TCP/HTTP en un hilo propio por worker |
| `worker` | Event loop, accept, preparación/activación de generaciones, cierre |
| `master` | `fork`, supervisión, recarga en dos fases, socket de estadísticas |
| `ipc` | Tramas maestro↔worker |
| `log` | Ring buffer 4096 × 512 B con hilo consumidor; descarta y cuenta si se llena |
| `stats` | Agregación de las instantáneas de los workers en JSON |
| `diag`, `main` | Diagnóstico; argumentos (`-c`, `-t`) |

Sin TLS/HTTPS, HTTP/2 ni WebSocket (rechazados), y sin reutilización de
conexiones hacia los backends.

## ⚙️ Funcionalidades

- Varios frontends; enrutado L7 por `Host`: exacto → wildcard → `default` → `502`.
- `round_robin`, `weighted` y `least_conn`, con `max_conns` por backend.
- Health checks activos (TCP/HTTP) y pasivos; `503` si no queda backend disponible.
- Cabeceras de reenvío con política de proxies de confianza por frontend.
- Keep-alive con el cliente, cuerpos `Content-Length` y `chunked`, límites y plazos (`408`, `413`, `414`, `431`, `502`, `504`).
- Recarga sin cortar conexiones y cierre ordenado (`SIGTERM`/`SIGINT`).
- Estadísticas JSON por socket UNIX; log asíncrono (acceso opcional, sin rutas ni credenciales).

Configuración: [docs/configuration.md](docs/configuration.md).

## 🚀 Compilar, probar y ejecutar

Requisitos: compilador C11 (GCC o Clang), Meson ≥ 0.60 (CI usa 1.10.1) y
Ninja. Para pruebas y demo: Python 3, `curl` y `ps` (en Debian/Ubuntu,
paquete `procps`). cmocka se usa del sistema o, si falta, del subproyecto
fijado en `subprojects/cmocka.wrap`.

### Compilar y probar

```bash
meson setup build && meson compile -C build   # auto: epoll en Linux, kqueue en macOS
meson test -C build                           # 18 suites: 11 unitarias + 7 de integración
meson test -C build --suite unit              # solo unitarias (cmocka)
bash scripts/lint.sh format-check && bash scripts/lint.sh tidy build && bash scripts/lint.sh cppcheck
```

Sanitizers y TSan (configuran un directorio de build nuevo) y Valgrind
(sobre un build ya compilado, p. ej. `build`):

```bash
bash scripts/ci.sh build-test build-asan -Db_sanitize=address,undefined -Db_lundef=false
bash scripts/ci.sh tsan build-tsan
bash scripts/ci.sh valgrind build
```

### Ejecución manual (primer plano)

```bash
./build/src/proxy -t -c proxy.toml   # solo valida (no abre sockets)
./build/src/proxy -c proxy.toml      # maestro + 3 workers en 7777 y 7778; Ctrl+C para detener
```

Solo arranca el proxy: sin backends en 3000/3001 las peticiones fallan
(502/503). Con `proxy.toml` crea el socket de estadísticas
`/tmp/proxy-stats.sock`; `Ctrl+C` hace un cierre ordenado y lo retira.
**Detén esta instancia antes de `bash scripts/demo.sh start`**: la demo usa
los mismos puertos y no arranca si están ocupados.

### Demo

Backends A (3000) y B (3001), proxy con 3 workers en 7777 y 7778. Salidas
reales en [docs/demo.md](docs/demo.md).

```bash
bash scripts/demo.sh start                                # backends + proxy
bash scripts/demo.sh workers                              # PIDs del maestro y de los workers
curl -s -H 'Host: api.example.com' http://127.0.0.1:7777/echo
bash scripts/demo.sh stats                                # resumen (--json: documento completo)
bash scripts/demo.sh reload examples/proxy-reload.toml    # válida: nueva generación en todos
bash scripts/demo.sh reload examples/proxy-invalid.toml   # inválida: se rechaza, nada cambia
bash scripts/demo.sh stop                                 # cierre ordenado; solo sus procesos
```

## ✅ Evidencia (commit `bc74bd3`)

| CI | Entorno | Resultado |
|---|---|---|
| GitHub Actions [36279111225](https://github.com/jorgevera573/1.5.10-proxy-epoll-kqueue/actions/runs/36279111225) | Linux GCC 13.3.0 y Clang 18.1.3 (epoll) | 18/18 suites normal y ASan/UBSan; TSan 18/18 (Clang); Valgrind: unitarias 11/11 e integración 7/7 |
| | macOS 15 arm64, Apple clang 17.0.0 (kqueue) | 18/18 suites normal y ASan/UBSan |
| | lint | clang-format, clang-tidy y cppcheck OK |
| GitLab #3383 | Linux | **Passed** (solo resultado global; sin detalle por job consultado) |

- **Suites y casos**: 18 suites = 11 unitarias (119 casos) + 7 de integración (95 casos).
- **Omitidas**: la prueba de EMFILE (`test_limits`) en macOS (requiere `/proc`) y en el paso Valgrind de Linux.
- Matriz de requisitos R01–R15 y registro completo: [docs/verification.md](docs/verification.md).

## 📈 Rendimiento (commit `e52c468`)

Build release, proxy con 6 workers delante de nginx (2 workers), 100
conexiones keep-alive, 4 hilos de carga, respuesta de 16 bytes, todo en la
misma máquina WSL2 (Intel Core Ultra 5 125H, 18 CPU lógicas) por loopback.
**Serie final** (01:40 UTC): calentamiento de 10 s y tres mediciones de 30 s
del proxy con `--timeout 2s`:

| Serie final: medición | req/s | Media | p50 | p90 | p99 | Errores de socket | Estado > 399 |
|---|---|---|---|---|---|---|---|
| 1 | 81.974,62 | 1,25 ms | 1,13 ms | 1,76 ms | 3,14 ms | 0 | 0 |
| 2 | 80.534,42 | 1,24 ms | 1,16 ms | 1,78 ms | 2,47 ms | 0 | 0 |
| 3 | 83.881,57 | 1,19 ms | 1,12 ms | 1,72 ms | 2,40 ms | 0 | 0 |
| **Mediana** | **81.974,62** | 1,24 ms | 1,13 ms | 1,76 ms | 2,47 ms | 0 | 0 |

- **Generador**: `wrk-monotonic`, una variante de wrk 4.1.0-4build3 cuyo
  único cambio es medir con `CLOCK_MONOTONIC`: en esta máquina el reloj de
  pared retrocede ~2 ms cada 30 s y el wrk original lo contaba como
  timeouts. Parche, fuente exacto, compilación y SHA-256 en
  [docs/benchmark.md](docs/benchmark.md).
- **Proxy durante la serie**: 0 respuestas 4xx/5xx propias, 0 fallos del
  backend, 0 reinicios; cierre con código 0.
- **Comparación directo/proxy** (serie aparte, 02:08 UTC, mismo montaje;
  tres mediciones de 30 s por destino en rondas de orden alterno): mediana
  de 272.084,64 req/s contra nginx directo y 84.696,99 req/s por el proxy,
  **relación 0,311**; p50 0,334 ms frente a 1,12 ms. No es el coste aislado
  del proxy: la máquina es compartida (sin medir el uso de CPU por
  proceso), y la cifra del proxy incluye abrir una conexión TCP nueva con
  nginx por petición, mientras que el acceso directo reutiliza 100
  conexiones keep-alive.
- **Alcance**: una máquina compartida por generador, proxy y backend, sin red
  real y con respuestas mínimas; no es una capacidad máxima. El cuerpo solo
  se comprobó con peticiones puntuales antes y después.
- Evidencias en texto: [serie final](docs/benchmark-evidence/serie-final-20260927T0140Z/README.md)
  y [comparación directo/proxy](docs/benchmark-evidence/comparacion-directo-proxy-20260927T0208Z/README.md).

## ⚠️ Límites conocidos

- **Estado por worker**: `max_connections`, `max_conns`, cursores de balanceo,
  `least_conn`, salud y contadores son de cada worker (con N workers, un
  backend con `max_conns = 1` puede recibir N conexiones; las sondas se
  multiplican por N). Las estadísticas suman los workers que responden.
- **macOS**: cola de aceptación compartida; el reparto depende de qué worker
  acepta antes, sin garantía de uniformidad (tampoco en Linux).
- **Pendientes**: carrera de SIGTERM cuando falla el arranque (limpieza
  interrumpida, sin errores de memoria); `config.c` aceptaría en macOS un
  identificador de zona IPv6 en `listen`/`address`; `test_limits` omitida en
  macOS; combinaciones de retirada sin prueba dirigida.
- **Benchmark de alcance limitado**: dos series en una sola máquina WSL2 por
  loopback, con respuestas de 16 bytes; sin pool de conexiones hacia el
  upstream (una conexión TCP por petición). Detalle en
  [docs/benchmark.md](docs/benchmark.md).

Más: [docs/architecture.md §13](docs/architecture.md) y
[docs/verification.md](docs/verification.md) (§ No verificado).

<!-- BEGIN cc:que-se-valora -->
¡Hola! ¡Qué bueno que estés trabajando en tu proyecto "Proxy Epoll Kqueue"! Sé que es un reto, pero estoy aquí para ayudarte a entender qué es lo que buscamos cuando lo revisamos. Para que te quede claro, he preparado esta sección para el `README` de tu proyecto:

---

## 📋 Qué se valora

Cuando revisemos tu proyecto, nos fijaremos en varias cosas para entender qué tan bien lo has resuelto.

Primero, **lo que más pesa** es que tu proxy funcione como se espera y cumpla con todo lo que pide el enunciado. Queremos ver que hace lo que tiene que hacer, sin fallos y de forma robusta.

También le damos un **peso importante** a la calidad de tu código y a la arquitectura que has elegido. Nos interesa que tu código sea claro, fácil de entender y que la estructura general de tu proyecto tenga sentido y esté bien pensada.

El **vídeo demo** también tiene un **peso importante**. Es tu oportunidad para mostrarnos cómo funciona tu proxy en acción y explicarnos de forma concisa lo que has hecho.

Finalmente, aunque con un **peso menor**, valoramos la documentación que incluyas y las decisiones que hayas tomado. Nos ayuda a entender tu proceso de pensamiento y por qué hiciste las cosas de cierta manera.

Recuerda que el detalle del enunciado es lo que manda para saber qué se espera de tu proyecto, y la evaluación no penaliza por lo que el enunciado no pide explícitamente.

---
<!-- END cc:que-se-valora -->
