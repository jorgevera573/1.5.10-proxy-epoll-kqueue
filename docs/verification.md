# Verificación: matriz de requisitos y evidencias

Estados:

- **Verificado (Linux)**: pruebas automatizadas ejercitan todos los
  criterios de aceptación de `docs/requirements.md` y pasan en Linux. No se
  ha ejecutado nada en macOS; la portabilidad se sigue en R01/R02.
- **Parcial**: parte de los criterios tiene evidencia real; se indica qué
  falta.
- **Pendiente**: sin código o sin evidencia.

Se distinguen tres tipos de evidencia:

- **Automatizada**: pruebas cmocka y de integración (§ Pruebas), con los
  comandos y resultados del § Registro.
- **Demostración manual del usuario**: ejecutada por el usuario, que informó
  del resultado (§ Demostraciones).
- **Demostración ejecutada al preparar la documentación**: comandos de
  `docs/demo.md` con sus salidas reales.

Las cifras del README no son mediciones de esta implementación. No se han
hecho benchmarks.

## Matriz R01–R15

| ID | Requisito | Estado | Evidencia | Falta |
|----|-----------|--------|-----------|-------|
| R01 | Build Meson y selección epoll/kqueue | **Parcial** | Linux: GCC, Clang, release, ASan/UBSan, TSan y cmocka del wrap, 0 avisos con `-Werror`; `auto` → epoll. Pasos de CI simulados en local con herramientas fijadas. | Compilar y probar en macOS. `io_event_kqueue.c` nunca se ha compilado. |
| R02 | API común `io_loop_*` | **Parcial** | 18 pruebas de `io_event` (epoll); el proxy y el hilo de health usan la API. | Ejecución con kqueue en macOS. |
| R03 | Sockets no bloqueantes y edge-triggered | **Parcial** | Fragmentación byte a byte, cliente lento de 40 MB con memoria acotada, escrituras parciales, desconexiones; mutación de pérdida de progreso detectada (etapa 3). | macOS/kqueue. |
| R04 | Frontends con `SO_REUSEPORT` y workers configurables | **Verificado (Linux)** | `workers` 1..64 y `0` = uno por CPU (probado: 18 workers en esta máquina). Cada worker abre su listener por frontend con `SO_REUSEPORT`; más de un worker atiende tráfico (observado por estadísticas, sin suponer reparto uniforme); routing correcto en los dos frontends con 2 workers; puerto ocupado → arranque fallido con código 1 y sin hijos. | macOS. |
| R05 | Host y cabeceras de reenvío | **Verificado (Linux)** | El backend verifica Host y la política de X-Forwarded-For, X-Real-IP y X-Forwarded-Proto (confiable y no confiable). | — |
| R06 | Exacto → wildcard → default → 502 | **Verificado (Linux)** | Unitarias de `router` e integración (incluido 502 sin default). | — |
| R07 | round_robin, weighted, least_conn | **Verificado (Linux)** | Unitarias deterministas: secuencias exactas (RR; weighted 3:1 = A A B A, 5:1:1 = a a b a c a a; 300/100 en 400), least_conn con conexiones retenidas y empates rotatorios, exclusión por salud y `max_conns`, y contabilidad entre generaciones. Integración (1 worker): weighted 30/10; least_conn con retenidas; contadores liberados al cancelar (RST) y en cierre forzado; conservación tras recarga. **Multiproceso**: weighted 3:1 cumplido dentro de cada worker (±1); least_conn equilibrado dentro de cada worker (diferencia ≤ 1); `max_conns = 1` con 2 workers → el backend llega a 2 simultáneas y cada worker respeta la suya (alcance **por worker**, documentado). | Estado compartido entre workers (descartado en esta etapa; §14.2). |
| R08 | Health checks activos TCP/HTTP y pasivos | **Verificado (Linux)** | Hilo dedicado **por worker**. Unitarias: TCP correcta/rechazada, criterio de estado HTTP, timeout, sonda retirada. Integración: backend HTTP excluido (500) y readmitido; backend TCP detenido → excluido → 503 → readmitido; exclusión pasiva y recuperación por sonda; sin exclusión por saturación, peticiones inválidas ni cancelaciones. JSON con salud por worker y resumen `up`/`down`/`mixed` (unitaria de `stats`; demo con 3 workers: cada worker detecta la caída y el pasivo cuesta hasta un 502 por worker). | Las sondas se multiplican por el número de workers (documentado). |
| R09 | Configuración TOML con tomlc99 | **Verificado (Linux)** | 8 pruebas unitarias (50 configuraciones inválidas con diagnóstico, health, algoritmos, compatibilidad de recarga) y `proxy -t`. | — |
| R10 | Recarga SIGHUP sin cortar conexiones | **Verificado (Linux)** | Maestro: lectura única, validación, **dos fases** con los mismos bytes a todos (PREPARE/PREPARED_OK/COMMIT/COMMITTED, ABORT) y plazos. Probado con 3 workers: válida → todos en generación 2; inválida y no recargable (`workers`) → nadie cambia; transferencia retenida en curso termina con la generación anterior y la nueva ruta se usa enseguida; **worker matado durante una preparación lenta** → recarga rechazada, sustituto con la generación vigente y la siguiente recarga se aplica en todos; **worker matado durante la activación** (retenido tras la decisión con un gancho solo de `proxy-testhooks`) → sin anuncio mientras falta su confirmación, los otros dos en la generación 2, anuncio de capacidad reducida (2 de 3 confirmados), sustituto en la generación 2 después del anuncio y atendiendo con el destino nuevo, recarga siguiente limpia a la 3; **retirada** de quien no confirma (plazo vencido y `COMMIT_FAILED`) con SIGTERM, SIGKILL al vencer el plazo de retirada si no puede procesarlo (SIGSTOP), reposición solo tras recogerlo y con la generación comprometida; estados `capacidad reducida` / `recuperación completa` / `recuperación agotada` en log y JSON, sin rollback; salida 1 si todos los puestos se agotan tras una activación parcial. Sin bloqueo del event loop (recarga retrasada 3 s), SIGHUP agrupados, cierre durante recarga, contabilidad conservada entre generaciones (suites de un worker). | Sin prueba dirigida: worker en retirada cuando empieza otra recarga, recargas encadenadas con puestos aún pendientes de reposición y cambio de `shutdown_timeout_ms` en plena retirada. |
| R11 | Log ring buffer 4096 × 512 B | **Verificado (Linux)** | `log.c`: ring 4096 × 512, hilo consumidor, fichero por worker, registro con pid/worker/nivel/fecha/generación. Unitarias: formato, filtro de nivel, saneado de control, truncado con marca, 8 productores concurrentes (escritos + descartados = producidos, sin intercalado), destino que no se lee → descartes sin bloquear al productor y `log_stop` con plazo, vaciado al cerrar. Integración: FIFO que nunca se lee con log de acceso → tráfico servido, `log_dropped > 0` y cierre acotado; log de acceso sin ruta, query, `Authorization` ni `Cookie`. | — |
| R12 | Pool `mmap` de slots de 16 KB con freelist | **Verificado (Linux)** | 7 pruebas unitarias; `slots_in_use=0` exigido tras cada escenario de integración y al salir. | — |
| R13 | Estadísticas JSON por socket UNIX | **Verificado (Linux)** | Maestro sirve JSON (esquema 1) con datos por worker y totales, tipos de contador, ausentes y reinicios. Unitarias: sumas, ausentes, salud mixta, escapado. Integración: totales = suma por worker; permisos 0600; ruta ocupada por un fichero o por un socket ajeno → arranque fallido y recurso intacto; clientes que no leen un JSON de ~440 KB + cliente sobrante rechazado mientras el tráfico sigue; expulsión por plazo; reinicio de worker visible (`restarts`, contadores a 0). Cliente `scripts/proxy-stats.py`. | — |
| R14 | Ciclo de vida HTTP y conexiones | **Verificado (Linux)** | Cuerpos, límites (431 extremo a extremo; 413/414/431 unitarias), timeouts: 408 de cabecera y **408 de cuerpo incompleto** con liberación verificada, keep-alive inactiva, 504 de respuesta y de connect; desconexiones del cliente (cuerpo, respuesta, RST) y del upstream (truncada, caído, anticipada); keep-alive 1.1/1.0 y pipelining; HEAD/204/304/1xx/101/100-continue; **accept ante EMFILE** sin bucle de CPU, descartando al instante y con recuperación. | Reutilización de conexiones al upstream no soportada (documentado, no es criterio). |
| R15 | Cierre ordenado | **Verificado (Linux)** | Worker: deja de aceptar, completa la activa, fuerza por plazo o segundo aviso; también con recarga en curso. Cierre del maestro **durante una retirada** (worker detenido con SIGSTOP): sin SIGTERM duplicado, un único SIGKILL (el primer plazo que vence), ninguna reposición, ningún hijo, socket retirado, código 70. Maestro: SIGTERM coordinado, plazo con SIGKILL, código 0 solo si todos salen limpios, sin hijos al terminar, socket de estadísticas borrado solo si es el suyo. Límite de reinicios → maestro sale con 1 sin hijos. Valgrind: 0 bytes y 3 fds en maestro y en **cada worker** (ver registro). | — |

## Demostraciones

### Etapa 3 — demostración manual del usuario

Informada por el usuario al iniciar la etapa 4 (no la ejecutó el asistente;
no hay log en el repositorio):

- configuraciones válidas (`proxy -t`);
- routing exacto y wildcard correcto;
- ruta default correcta;
- round robin B/A alternado en seis solicitudes;
- 502 esperado sin ruta;
- cierre con `conns=0`, `upstreams=0`, `slots_in_use=0` y `backend_active=0`.

### Etapa 4 — demo de health y recarga

Ejecutada con `scripts/demo.sh` el 2026-09-25; comandos y salidas en
`docs/demo.md` §7–§9: exclusión por sonda HTTP 500 (`B B B B B B`) y
readmisión (`A B A B A B`); caída real de B con 502 en la petición que la
descubre, exclusión pasiva (`A A A A A A`) y readmisión al rearrancar;
recarga válida (api.example.com → B, weighted `A A B A A A B A`); recarga
inválida y cambio de puerto rechazados sin interrupción; cierre con
`slots_in_use=0` y `generations_live=1`.

## Pruebas

### Unitarias (cmocka): 119 casos en 11 suites

| Suite | Casos | Novedades (etapas 4 y 5) |
|---|---|---|
| `io_event` | 18 | — |
| `buffer_pool` | 7 | — |
| `timer` | 10 | — |
| `http_parser` (+ `host`, `http_forward`) | 34 | — |
| `router` | 11 | — |
| `config` | 8 | health (tcp/http, valores por defecto), algoritmos, 11 `[pool.health]` inválidos, compatibilidad de recarga (puerto, número de frontends, max_connections; recargables aceptados) |
| `backend_pool` | 14 | corrección: salud y activas compartidas por identidad (pool, dirección), misma dirección en otro pool independiente, max_conns cuenta la generación anterior, reducción de max_conns bloquea hasta bajar del límite, least_conn cuenta la generación anterior, eliminar/reintroducir conserva la entrada (y se libera sin referencias); etapa 4: weighted (secuencias exactas, 300/100, exclusión y vuelta), least_conn (retenidas, empates), health activo (fall/rise), todos caídos, pasivo (y sin `[pool.health]` no excluye), herencia entre generaciones, referencias y generaciones vivas |
| `log` | 6 | **etapa 5**: formato y nivel, saneado de control, truncado a 512 B con marca, 8 productores × 2000 (escritos + descartados = producidos, sin intercalado, avisos de descarte), destino que nunca se lee (descarta sin bloquear y `log_stop` con plazo), vaciado al cerrar, logger instalado |
| `ipc` | 4 | **etapa 5**: ida y vuelta, carga de 5 MiB con envío/recepción alternados no bloqueantes, entrega byte a byte, magia errónea, longitud excesiva, EOF, `EMSGSIZE` |
| `stats` | 2 | **etapa 5**: sumas por worker, ausentes y `complete`, agregación por (pool, dirección), salud mixta y resumen, tipos de contador, escapado JSON; `workers_ready`/`degraded`, `reload_state` y `recovery` |
| `health` | 5 | nueva: TCP correcta y rechazada (`ECONNREFUSED`), estado HTTP 204 sano / 503 caído, timeout (vence el plazo, no la respuesta), sonda en curso de un plan retirado informa con su generación (7) y no hay más, parada con sondas en curso |

### Integración: 87 casos en 7 ficheros

Backends Python propios (`/health`, `/_set`, `/_state`, `/hold`,
`/release`, `X-Backend` en toda respuesta). Puertos libres, directorio
temporal propio, límite por prueba, solo se terminan los PID lanzados. La
coordinación usa estado observable con plazo: estadísticas JSON del socket
UNIX (desde la etapa 5; antes `SIGUSR1`), `/_state` del backend y líneas
del log; ninguna prueba se sincroniza solo con esperas fijas. Si el maestro
muriera sin recoger a sus workers, la limpieza solo mata los PID que ese
maestro registró en su log y que siguen siendo el ejecutable de la prueba.

| Fichero | Casos | Contenido |
|---|---|---|
| `test_proxy.py` | 38 | etapa 3 + **408 por cuerpo incompleto** |
| `test_balancing.py` | 4 | weighted 30/10; least_conn con retenidas; contador liberado al cancelar con RST; contadores a 0 tras cierre forzado con 3 retenidas |
| `test_health.py` | 4 | HTTP caída/recuperación; TCP detenido → 503 → rearrancado; pasivo y recuperación por sonda; sin exclusión por saturación, 400 o RST |
| `test_reload.py` | 7 | cambio de destino; transferencia larga + keep-alive; inválida; no recargables; SIGHUP agrupados + varias recargas + cierre limpio; cierre durante recarga; sonda tardía de la generación vieja |
| `test_limits.py` | 1 | EMFILE con `RLIMIT_NOFILE = 40` solo en el proxy de la prueba (omitida bajo Valgrind) |
| `test_generations.py` | 9 | corrección: max_conns = 1 ocupado tras recargar (503 hasta liberar); least_conn ve la retenida de la generación 1; reducir max_conns 3 → 1 con 3 activas (ninguna cortada, 503 con 2 y con 1 activas, 200 al bajar a 0); eliminar y reintroducir con petición activa; cancelación (RST) tras recargar descuenta en la generación 1; cierre forzado con retenidas de 3 generaciones; recarga retrasada 3 s sin bloquear; cierre durante recarga lenta con el bucle respondiendo; el binario `proxy` no contiene los ganchos de prueba (retardo de recarga y retención de la activación) y los ignora |
| `test_multiprocess.py` | 24 | **etapa 5**: worker matado durante la **activación** (determinista, ver abajo); **retirada**: plazo de confirmación vencido, `COMMIT_FAILED`, worker que no procesa SIGTERM (SIGKILL), `max_restarts` agotado tras activación parcial (estado degradado; salida 1 con un solo puesto), cierre del maestro durante una retirada; 3 workers (pids distintos, 0600, más de un worker sirve, totales = suma, cierre limpio); `workers = 0` → uno por CPU; routing en 2 frontends con 2 workers; puerto ocupado → código 1 sin hijos; `max_conns` por worker; weighted y least_conn por worker; recarga válida/inválida/no recargable coordinada; transferencia en curso durante la recarga; worker matado durante preparación lenta → rechazo y la siguiente se aplica; `kill -9` → reposición contada; límite de reinicios → maestro sale con 1; fichero y socket ajenos intactos; clientes lentos, cliente sobrante y expulsión con tráfico en marcha; log saturado; log de acceso sin secretos |

### Mutaciones (riesgo concreto de esta etapa)

| Mutación | Resultado |
|---|---|
| Sin descarte con el fd de reserva ante EMFILE | falla `test_limits` (las conexiones sobrantes quedan colgadas) |
| Reintento inmediato en bucle ante EMFILE | falla `test_limits` (el bucle acapara el proceso; ni siquiera responde a SIGUSR1) |
| Sin mutex en la cola de resultados de health | ThreadSanitizer informa "data race" en `test_health` (comprueba que TSan detecta en este entorno) |

Etapa 5 (compiladas en un build aparte, `build-mut`; fuentes restauradas y
comprobadas con `cmp`):

| Mutación | Resultado |
|---|---|
| `unlink` de la ruta del socket de estadísticas antes de `bind` | fallan `test_foreign_file_at_stats_path_is_left_intact` y `test_foreign_listening_socket_is_left_intact` |
| El productor de log espera cuando el ring está lleno (en vez de descartar) | falla `test_saturated_log_drops_without_stopping_traffic` (el tráfico se detiene: timeout) |

No se repitieron las mutaciones de etapas anteriores. Sí se repitieron 3
veces todas las suites (riesgo de intermitencia por hilos y señales):
integración 15/15, unitarias 24/24.

## Corrección de la etapa 4 (antes de la etapa 5)

### Contabilidad entre generaciones

- **Causa**: `active`, la salud y los contadores vivían en arrays propios de
  cada generación. Una recarga creaba una generación con `active = 0` (solo
  copiaba la salud), así que `least_conn` y `max_conns` de la nueva no veían
  las conexiones que seguían activas de la anterior.
- **Solución**: registro de backends por worker con identidad estable
  (pool, dirección) y referencias desde las generaciones. El estado operativo
  (activas, salud, contadores) vive en la entrada; la generación guarda su
  configuración y el estado de sus algoritmos. Cada conexión decrementa la
  misma entrada que incrementó. Detalle en `docs/architecture.md` §7.2–§7.4.

### Recarga sin bloqueo

- **Revisión del código**: `on_reload_done` ya hacía el join tras el aviso
  (correcto), pero `worker_cleanup` hacía `pthread_join` sobre un hilo de
  recarga que podía estar todavía leyendo/validando (el cierre durante una
  recarga bloqueaba el hilo principal todo ese tiempo). Además
  `getenv("PROXY_TEST_RELOAD_DELAY_MS")` estaba en el ejecutable de
  producción.
- **Solución**: `done` atómico + aviso como última acción del hilo; el loop
  solo recoge y hace join tras verlo; en el cierre el bucle sigue girando
  hasta el aviso. El retardo solo existe en `proxy-testhooks`
  (`-DPROXY_TEST_HOOKS`).

### Mutaciones de riesgo concreto

| Mutación | Resultado |
|---|---|
| El registro no comparte entradas entre generaciones (comportamiento anterior) | fallan 6 de 9 pruebas de `test_generations.py` |
| El bucle no espera el aviso y el cierre hace join bloqueante (comportamiento anterior) | falla `test_shutdown_during_slow_reload_keeps_loop_responsive` |

## Fallos encontrados y corregidos en la etapa 4

- **CI con Clang rota desde la etapa 3**: con el cmocka 1.1.8 del wrap (el
  que usa la CI), su macro `fail_msg` (`, ##__VA_ARGS__`) dispara
  `-Wgnu-zero-variadic-macro-arguments` con Clang y `-Wpedantic`. En local no
  se veía porque se usaba el cmocka 2.0.2 del sistema. Se detectó simulando
  el nuevo paso `tsan` de CI; ahora cmocka se incluye como cabecera de
  sistema (`as_system`). Se simularon además todos los jobs con el wrap
  (GCC, Clang, ASan con Clang, Valgrind, TSan, lint).
- **GCC + cmocka 1.1.8**: `-Wnull-dereference` en `test_health.c` porque
  `assert_non_null` de 1.1.8 no es *noreturn*; se usa una función que aborta.
- **Prueba intermitente**: el kernel no encola señales estándar repetidas,
  así que tres `SIGHUP` seguidos podían llegar como uno. La prueba ahora
  envía cada señal tras observar en el log el efecto de la anterior.
- **Lectura de la contabilidad**: el harness leía tras una espera fija; ahora
  el bloque termina en `accounting end` y se espera a esa marca.
- **clang-tidy**: al reactivar `concurrency-mt-unsafe` (ya hay hilos),
  `strerror` se sustituyó por `strerror_r` (`diag_strerror`); `getopt` y
  `getenv` se documentan con NOLINT porque se ejecutan antes de crear hilos.
  Falsos positivos del analizador evitados con recorridos explícitos y
  comprobaciones defensivas (sin silenciarlos).
- Errores de datos en pruebas nuevas (ruta a un pool inexistente, que la
  validación rechazó correctamente).

## Registro de ejecuciones

### Entorno

- Ubuntu 26.04 LTS sobre WSL2, kernel `6.18.33.2-microsoft-standard-WSL2`,
  x86_64, 18 CPUs lógicas.
- GCC 15.2.0, Clang 21.1.8, Meson 1.10.1, Ninja 1.13.2, cmocka 2.0.2
  (sistema) y 1.1.8 (wrap), Valgrind 3.26.0, Cppcheck 2.19.0,
  clang-format 21.1.8, clang-tidy 21.1.8 (sistema) y 21.1.6 (PyPI),
  Python 3.14.4, curl 8.18.0.
- Fecha: 2026-09-25.

### Etapa 5: retirada con plazo de workers que no confirman

Cambios de código:

- `master.c`: estado de puesto `retiring`. Al vencer el plazo de
  confirmación o recibir `COMMIT_FAILED`: un SIGTERM y un plazo de retirada
  `max(shutdown_timeout_ms viejo, nuevo) + 5000` ms; al vencer, un SIGKILL.
  La reposición solo se programa al recoger el hijo (backoff y
  `max_restarts`) y arranca con la generación comprometida. El cierre global
  no reenvía SIGTERM a un worker en retirada ni repone nada; el SIGKILL no
  se repite (`kill_sent`) y el aviso del plazo global solo aparece si mata a
  alguien. Log y JSON distinguen la activación en curso, la capacidad
  reducida, la recuperación completa y la agotada (§14.3.1).
- `stats.{c,h}`: `workers_ready`, `degraded`, `reload_state` y `recovery`
  (el objeto `master` se compone en una función aparte, `render_master`,
  para no superar el umbral de complejidad de clang-tidy).
- `worker.{c,h}`: gancho `PROXY_TEST_FAIL_COMMIT` (junto a
  `PROXY_TEST_HOLD_COMMIT`), solo con `PROXY_TEST_HOOKS`.
- `scripts/proxy-stats.py`: línea con recarga, recuperación y capacidad.

Pruebas nuevas (`ActivationFailureTest`, sincronización por líneas del log y
estadísticas, con plazo; SIGSTOP solo al pid del worker retenido de la
prueba, verificado por su línea de comandos, y SIGKILL de limpieza en
`tearDown` aunque la prueba falle):

| Prueba | Comprueba |
|---|---|
| `test_commit_deadline_retires_and_replaces_with_new_generation` | sin anuncio mientras está retenido; retirada al vencer el plazo con `plazo = shutdown + 5000`; anuncio de capacidad reducida (2 de 3); `retirado (código 0)` → `repuesto (generación 2)` → `recuperación completa`, en ese orden; sin SIGKILL; JSON `recovery = complete`, `degraded = false`; tráfico al destino nuevo; recarga a la 3 con los 3; cierre limpio |
| `test_commit_failed_retires_without_waiting_for_deadline` | con plazo de confirmación de 60 s: retirada inmediata por `COMMIT_FAILED` y la misma secuencia |
| `test_worker_that_cannot_handle_sigterm_is_killed_and_replaced` | worker detenido con SIGSTOP: SIGTERM sin efecto, `sigue vivo tras 5500 ms de retirada: SIGKILL` (un solo SIGKILL), `retirado (señal 9)`, sustituto con la generación 2, recuperación completa |
| `test_max_restarts_exhausted_after_partial_activation_is_explicit` | `max_restarts = 0`: `recuperación agotada … sigue activa con 2 de 3`, sin reposición; JSON `exhausted`, `degraded`, `workers_ready = 2`, puesto `failed`, `missing_workers = [1]`; generación 2 mantenida (sin rollback); tráfico al destino nuevo; cierre con 0 |
| `test_all_slots_exhausted_after_partial_activation_exits_1` | un solo puesto: `0 de 1 workers confirmados`, recuperación agotada, `todos los workers han fallado`, código 1, sin hijos, socket retirado |
| `test_master_shutdown_during_retirement` | cierre del maestro con un worker en retirada detenido: `ya en retirada` (sin SIGTERM duplicado), un único SIGKILL (del plazo de retirada o del de cierre, el que venza antes), `terminó (señal 9)`, ninguna línea de reposición o arranque, código 70, sin hijos, socket retirado |

```bash
meson compile -C build                                   # 0 avisos
grep -a -c PROXY_TEST_HOLD_COMMIT build/src/proxy        # 0  (proxy-testhooks: 1)
grep -a -c PROXY_TEST_FAIL_COMMIT build/src/proxy        # 0  (proxy-testhooks: 1)
PROXY_BIN=build/src/proxy PROXY_TEST_BIN=build/src/proxy-testhooks \
  python3 tests/integration/test_multiprocess.py ActivationFailureTest \
  ReloadTest.test_worker_killed_during_commit_is_replaced_with_new_generation
#   Ran 7 OK (y 3 + 3 repeticiones más de ActivationFailureTest: OK)
meson test -C build                                      # Ok: 18  Fail: 0 (multiprocess: 24 casos)
bash scripts/lint.sh format-check && bash scripts/lint.sh tidy build && bash scripts/lint.sh cppcheck
#   OK; tidy: OK (31 ficheros); OK
# ASan+UBSan y TSan sobre los caminos modificados (TIME_SCALE=2):
python3 tests/integration/test_multiprocess.py ActivationFailureTest ReloadTest SupervisionTest StatsSocketTest
#   ASan+UBSan: Ran 15 OK; test_stats OK
#   TSan: Ran 15 OK sin informes; test_stats OK
# Valgrind (TIME_SCALE=4, KEEP_LOGS=1) sobre las 7 pruebas de activación:
#   Ran 7 OK. 30 procesos arrancados (maestros, workers y sustitutos):
#   25 con "ERROR SUMMARY: 0 errors", "in use at exit: 0 bytes" y
#   "FILE DESCRIPTORS: 3 open (3 inherited)"; 5 sin resumen, todos
#   terminados con SIGKILL (3 por la propia prueba, 2 por la escalada de
#   retirada): de esos 5 no hay evidencia de Valgrind.
```

Fallos encontrados durante esta corrección:

- Bajo TSan, `test_master_shutdown_during_retirement` falló: el plazo de
  retirada y el de cierre vencieron en la misma vuelta del bucle; solo hubo
  un SIGKILL, pero el aviso del plazo de cierre decía "SIGKILL" sin enviar
  ninguno. Ahora ese aviso solo se escribe si mata a alguien (e indica
  cuántos), y la prueba acepta cualquiera de los dos plazos siempre que haya
  exactamente un SIGKILL.
- clang-tidy: `stats_render_json` superó el umbral de complejidad cognitiva
  (41 > 40) con los campos nuevos; se extrajo `render_master` (no se tocó el
  umbral).

### Etapa 5: cierre de la verificación (fallo durante la activación)

Prueba nueva, determinista:
`test_multiprocess.py::ReloadTest::test_worker_killed_during_commit_is_replaced_with_new_generation`.

- Gancho solo en `proxy-testhooks`: `PROXY_TEST_HOLD_COMMIT=<worker>:<gen>`.
  Ese worker recibe el COMMIT de esa generación (el maestro ya decidió),
  escribe `VARIANTE DE PRUEBAS: activación de la generación 2 retenida` y no
  la aplica ni la confirma. Afecta a una sola generación. El plazo de
  confirmación de la prueba (60 s escalados) no vence durante ella, así que
  la prueba decide cuándo muere el worker.
- Sincronización por eventos con plazo: línea de retención del pid del
  worker 1; estadísticas con el maestro en la generación 2, recarga en
  curso y los workers en `{0: 2, 1: 1, 2: 2}`; dos líneas "generación 2
  activada" y ninguna "recarga aplicada"/"rechazada"; `reloads_ok = 0`.
- `SIGKILL` solo al pid retenido, tomado de las estadísticas de este
  maestro. Se comprueba: el anuncio de activación parcial (texto actual,
  tras la corrección posterior de la retirada: `con capacidad reducida (2 de
  3 workers confirmados; 1 por reponer con la generación 2)`); `worker 1 repuesto (pid, generación 2)`
  **después** del anuncio; 3 workers listos en la generación 2, los otros dos
  con el mismo pid; el sustituto con `restarts = 1`, tráfico al destino
  nuevo (B) contado en **sus** estadísticas y 0 selecciones de A.
- Otra recarga (a la 3): `recarga aplicada: generación 3 (3 workers)`, sin
  retención, todos en la 3, tráfico a A, una sola generación viva, reposo.
- Cierre con código 0, socket retirado y ningún pid (retenido, sustituto,
  otros) vivo.

Cambios de código:

- `worker.c`/`worker.h`: el gancho (bajo `#ifdef PROXY_TEST_HOOKS`).
- `master.c`: un worker que muere durante la activación cuenta en el anuncio
  (antes decía `(1 workers)` sin mencionar al que faltaba).
- `test_generations.py`: la prueba de ganchos del binario de producción
  comprueba las dos variables (ausentes en `proxy`, presentes en
  `proxy-testhooks`) y que `proxy` recarga con ambas en el entorno.

```bash
meson compile -C build                                        # 0 avisos
grep -a -c PROXY_TEST_HOLD_COMMIT build/src/proxy build/src/proxy-testhooks   # 0 y 1
PROXY_BIN=build/src/proxy PROXY_TEST_BIN=build/src/proxy-testhooks \
  python3 tests/integration/test_multiprocess.py \
  ReloadTest.test_worker_killed_during_commit_is_replaced_with_new_generation
#   5 ejecuciones seguidas: OK
meson test -C build                                           # Ok: 18  Fail: 0
bash scripts/lint.sh format-check; bash scripts/lint.sh tidy build; bash scripts/lint.sh cppcheck
#   OK; tidy: OK (31 ficheros); OK
# Caminos modificados (dispatch de COMMIT en el worker, anuncio del maestro):
PROXY_BIN=build-asan/src/proxy PROXY_TEST_BIN=build-asan/src/proxy-testhooks TIME_SCALE=2 \
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  python3 tests/integration/test_multiprocess.py ReloadTest SupervisionTest   # OK (6)
  python3 tests/integration/test_generations.py                                # OK (9)
PROXY_BIN=build-tsan/... TSAN_OPTIONS="halt_on_error=1 exitcode=66" (mismas)   # OK (6), OK (9)
KEEP_LOGS=1 TIME_SCALE=4 PROXY_WRAPPER="valgrind --error-exitcode=99 ..." \
  python3 tests/integration/test_multiprocess.py ReloadTest.test_worker_killed_during_commit_is_replaced_with_new_generation
#   OK; 4 procesos con resumen (maestro, workers 0 y 2, sustituto): 0 errores,
#   0 bytes, 3 fds heredados; el worker matado con SIGKILL no emite resumen.
```

No se repitieron las campañas completas de sanitizers ni de Valgrind: el
cambio no toca hilos ni memoria compartida. La primera ejecución de la
prueba nueva falló por una aserción de la propia prueba (usaba el conjunto
de pids recogido por el arnés, que no incluía el retenido). Se sustituyó
por una comprobación directa de que ningún pid siga vivo.

### Etapa 5: comandos y resultados

```bash
meson compile -C build && meson test -C build          # 0 avisos; Ok: 18  Fail: 0
#   unitarias: 119 casos en 11 suites; integración: 80 casos en 7 ficheros
PROXY_BIN=build/src/proxy PROXY_TEST_BIN=build/src/proxy-testhooks \
  python3 tests/integration/test_multiprocess.py        # Ran 17 ... OK (3 ejecuciones seguidas)
meson test -C build-clang                               # Clang: 0 avisos; Ok: 18
meson test -C build-release                             # release: 0 avisos; Ok: 18

UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  meson test -C build-asan --timeout-multiplier 3       # ASan+UBSan: Ok: 18; 0 informes
TSAN_OPTIONS="halt_on_error=1 exitcode=66" \
  meson test -C build-tsan --timeout-multiplier 3       # TSan: Ok: 18; 0 informes
for t in buffer_pool timer router http_parser io_event config backend_pool health log ipc stats; do
  valgrind --error-exitcode=99 --leak-check=full --show-leak-kinds=all \
           --errors-for-leak-kinds=all --track-fds=yes ./build/tests/test_$t; done
#   las 11: exit 0, "in use at exit: 0 bytes", "ERROR SUMMARY: 0",
#   "FILE DESCRIPTORS: 3 open (3 inherited)"
KEEP_LOGS=1 TIME_SCALE=4 PROXY_WRAPPER="valgrind --error-exitcode=99 --leak-check=full \
  --show-leak-kinds=all --errors-for-leak-kinds=all --track-fds=yes" \
  python3 tests/integration/test_{proxy,balancing,health,reload,limits,generations,multiprocess}.py
#   38, 4, 4, 7 y 17 OK; limits omitida (skipped=1); generations: 9 OK tras
#   escalar ipc_timeout_ms en la prueba (ver abajo)
#   Valgrind sigue a los workers tras fork: 131 procesos (maestros y workers)
#   con "ERROR SUMMARY: 0 errors", "in use at exit: 0 bytes in 0 blocks" y
#   "FILE DESCRIPTORS: 3 open (3 inherited)". Maestros: 46 con código 0 y 4
#   con 1 (los arranques fallidos esperados: puerto ocupado, fichero ajeno,
#   socket ajeno, límite de reinicios).
bash scripts/lint.sh format-check                       # OK
bash scripts/lint.sh tidy build                         # tidy: OK (31 ficheros)
bash scripts/lint.sh cppcheck                           # OK
```

Demo con 3 workers ejecutada con `scripts/demo.sh` (salidas en
`docs/demo.md`): reparto entre workers, estadísticas, recarga coordinada a
la generación 2, recargas inválida y no recargable rechazadas, worker
matado con `kill -9` repuesto con la generación 2, cierre con código 0,
socket y directorio de la demo retirados.

Alcance de las herramientas:

- **ASan/LSan**: los workers terminan con `_exit`, así que LeakSanitizer
  no revisa fugas en ellos (sí los errores de memoria, que hacen salir al
  worker con código ≠ 0 y al maestro con 70). Las fugas por proceso se
  comprueban con Valgrind, que sí informa en `_exit`.
- **TSan** comprueba los hilos **dentro** de cada proceso (event loop,
  health, recarga, consumidor de log). **No** es evidencia del protocolo
  entre procesos (IPC, recarga en dos fases, supervisión): eso lo cubren
  las pruebas de integración y las unitarias de `ipc`.

Fallos encontrados y corregidos en esta etapa (además de los de la
implementación inicial):

- Un `PREPARE` que llegaba mientras terminaba el hilo de una preparación
  abortada se rechazaba ("ya hay una preparación en curso"): tras matar un
  worker durante una recarga, la siguiente fallaba. Ahora espera, sin
  bloquear, a que termine la abortada (probado).
- Valgrind: cada worker terminaba con ~7 KB "still reachable" (la
  configuración que `main` retenía en el hijo). `master_run` toma ahora la
  propiedad de la configuración y el hijo lo libera todo antes de `_exit`.
- Bajo Valgrind (`TIME_SCALE=4`) `test_slow_reload_does_not_block_serving`
  falló: el retardo artificial escalado (12 s) superaba el
  `ipc_timeout_ms` por defecto (10 s) y el maestro rechazó la recarga por
  plazo, que es el comportamiento correcto. La prueba declara ahora un
  `ipc_timeout_ms` escalado como sus demás plazos; el control no se relajó.
- Clang: `-Wformat-nonliteral` en `log.c` (faltaban atributos `format` en
  las funciones con `va_list`); el build de GCC no lo detectaba.
- clang-tidy: cadena recursiva `begin_reload → … → finish_commit →
  begin_reload` (ahora la recarga pendiente se arranca desde un temporizador
  de 0 ms), `calloc` de tamaño 0 y un falso positivo de uso tras liberar en
  `stats_close` (reescrito para que la propiedad sea explícita).

### Corrección de la etapa 4: comandos y resultados

```bash
meson setup --reconfigure build && meson compile -C build   # 0 avisos; genera proxy y proxy-testhooks
grep -c PROXY_TEST_RELOAD_DELAY_MS build/src/proxy build/src/proxy-testhooks
#   proxy: 0; proxy-testhooks: 1
meson test -C build                                  # Ok: 14  Fail: 0
#   unitarias: 107 (backend_pool 14); integración: 63 (generations 9)
PROXY_BIN=build/src/proxy PROXY_TEST_BIN=build/src/proxy-testhooks \
  python3 tests/integration/test_generations.py      # Ran 9 ... OK

meson test -C build-asan  (ASan+UBSan)               # Ok: 14; 0 informes
TSAN_OPTIONS="halt_on_error=1 exitcode=66" meson test -C build-tsan --timeout-multiplier 3
#                                                    # Ok: 14; 0 informes
valgrind ... build/tests/test_{backend_pool,config,health}
#   exit 0; 14/8/5 PASSED; 0 bytes en uso; 0 errores
KEEP_LOGS=1 TIME_SCALE=4 PROXY_WRAPPER="valgrind --error-exitcode=99 ..." \
  python3 tests/integration/test_{generations,reload,balancing,health,proxy}.py
#   9, 7, 4, 4 y 38 OK; 24 procesos proxy: 0 errores, 0 bytes,
#   3 fds heredados, "shutdown final generations_live=0 backends_live=0"
bash scripts/lint.sh format-check && bash scripts/lint.sh tidy build && bash scripts/lint.sh cppcheck
#   OK; tidy: OK (24 ficheros); OK
```

### Etapa 4: comandos y resultados (builds limpios)

```bash
meson setup build && meson compile -C build          # 0 avisos (-Werror)
meson test -C build                                  # Ok: 13  Fail: 0
#   8 suites unitarias + 5 de integración
for t in build/tests/test_*; do [ -f "$t" ] && "$t" 2>&1 | tail -1; done
#   backend_pool 10, buffer_pool 7, config 8, health 5, http_parser 34,
#   io_event 18, router 11, timer 10  ->  103 casos
for f in test_proxy test_balancing test_health test_reload test_limits; do
  PROXY_BIN=build/src/proxy python3 tests/integration/$f.py; done
#   Ran 38 OK; Ran 4 OK; Ran 4 OK; Ran 7 OK; Ran 1 OK  ->  54 casos
meson test -C build --suite integration --repeat 3   # Ok: 15  Fail: 0
meson test -C build --suite unit --repeat 3          # Ok: 24  Fail: 0

CC=clang meson setup build-clang && meson test -C build-clang        # 0 avisos; Ok: 13
meson setup build-release --buildtype=release && meson test -C build-release  # 0 avisos; Ok: 13

# ASan + UBSan (incluye las 5 integraciones contra el proxy instrumentado)
meson setup build-asan -Db_sanitize=address,undefined -Db_lundef=false
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 meson test -C build-asan
#   Ok: 13  Fail: 0; 0 informes

# ThreadSanitizer (build aparte, Clang)
CC=clang meson setup build-tsan -Db_sanitize=thread -Db_lundef=false
TSAN_OPTIONS="halt_on_error=1 exitcode=66" meson test -C build-tsan --timeout-multiplier 3
#   Ok: 13  Fail: 0; 0 informes "ThreadSanitizer"
#   (el proxy instrumentado sale con 66 ante una carrera y la prueba falla)

# Valgrind (ejecución separada, binarios sin instrumentar)
for t in buffer_pool timer router http_parser io_event config backend_pool health; do
  valgrind --error-exitcode=99 --leak-check=full --show-leak-kinds=all \
           --errors-for-leak-kinds=all --track-fds=yes ./build/tests/test_$t; done
#   las 8: exit 0, "in use at exit: 0 bytes", "ERROR SUMMARY: 0",
#   "FILE DESCRIPTORS: 3 open (3 inherited)"
for f in test_proxy test_balancing test_health test_reload test_limits; do
  KEEP_LOGS=1 TIME_SCALE=4 PROXY_WRAPPER="valgrind --error-exitcode=99 ..." \
  PROXY_BIN=build/src/proxy python3 tests/integration/$f.py; done
#   38, 4, 4 y 7 OK; limits omitida (skipped=1)
#   15 procesos proxy: "ERROR SUMMARY: 0 errors", "in use at exit: 0 bytes in
#   0 blocks", "FILE DESCRIPTORS: 3 open (3 inherited)", shutdown con
#   slots_in_use=0

bash scripts/lint.sh format-check   # OK
bash scripts/lint.sh tidy build     # tidy: OK (24 ficheros)
bash scripts/lint.sh cppcheck       # OK
```

### CI: simulación local con herramientas fijadas (no es ejecución remota)

```bash
export VENV=<scratch>/venv
CC=gcc   bash scripts/ci.sh build-test <scratch>/ci-gcc4      # cmocka 1.1.8; Ok: 13
CC=clang bash scripts/ci.sh build-test <scratch>/ci-clang4    # Ok: 13
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 CC=clang \
  bash scripts/ci.sh build-test <scratch>/ci-asan4 -Db_sanitize=address,undefined -Db_lundef=false  # Ok: 13
bash scripts/ci.sh valgrind <scratch>/ci-gcc4                 # unit Ok: 8; integration Ok: 5
CC=clang bash scripts/ci.sh tsan <scratch>/ci-tsan            # Ok: 13
bash scripts/ci.sh lint <scratch>/ci-lint4                    # format, tidy (24), cppcheck OK
```

| Pipeline | Estado |
|---|---|
| `.gitlab-ci.yml` (Linux: gcc, clang, sanitizers, valgrind, tsan, lint) | **Preparado, no ejecutado** |
| `.github/workflows/ci.yml` (Linux y macOS; tsan y valgrind solo Linux) | **Preparado, no ejecutado** |

### Etapas anteriores (referencia)

Etapa 1: `--repeat=50` de `io_event` y 6 mutaciones. Etapa 2: 7 mutaciones
(parser, router, buffer_pool, timer). Etapa 3: mutación de backpressure.

### No verificado

- macOS/kqueue: no compilado ni ejecutado.
- CI: sin ejecución remota (preparada; incluye las suites nuevas porque
  delega en `meson test`).
- TSan no demuestra ausencia de carreras en caminos no ejercitados ni cubre
  el protocolo entre procesos.
- Combinaciones de retirada sin prueba dirigida: worker en retirada cuando
  empieza otra recarga, recargas encadenadas con puestos aún pendientes de
  reposición, y cambio de `shutdown_timeout_ms` en plena retirada (el plazo
  de retirada ya fijado no cambia).
- Valgrind no emite resumen de los procesos terminados con SIGKILL (los que
  mata la prueba y los que mata la escalada de retirada): de ellos no hay
  evidencia de memoria.
- Estado compartido entre workers: no existe (decisión documentada).
- Ningún benchmark.
