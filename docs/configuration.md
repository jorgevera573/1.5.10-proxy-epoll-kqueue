# Configuración (TOML)

El proxy lee un fichero TOML con tomlc99 (`third_party/tomlc99`, fijado por
commit). Toda clave desconocida es un error, para detectar erratas. Los
diagnósticos tienen la forma `origen: sección.clave: motivo`.

```bash
./build/src/proxy -t -c proxy.toml   # solo valida; no abre sockets
./build/src/proxy -c proxy.toml      # ejecuta
```

Códigos de salida (del maestro): `0` cierre limpio de todos los workers,
`1` configuración o arranque inválidos (o todos los workers agotaron sus
reinicios), `2` uso incorrecto, `70` algún worker terminó mal (recursos
prestados al salir, señal o plazo de cierre vencido).

## `[server]`

| Clave | Tipo | Rango | Por defecto | Notas |
|---|---|---|---|---|
| `workers` | entero | 0..64 | 1 | Procesos worker. `0` = uno por CPU en línea (`sysconf(_SC_NPROCESSORS_ONLN)`, máximo 64). **No recargable.** |
| `max_connections` | entero | 1..32768 | 1024 | Conexiones de cliente simultáneas **por worker**. El pool de buffers de cada worker se dimensiona a 4 slots de 16 KB por conexión. **No recargable.** |
| `shutdown_timeout_ms` | entero | 0..3600000 | 10000 | Plazo del cierre ordenado de cada worker antes de forzar. El maestro manda SIGKILL a los que sigan vivos a los `shutdown_timeout_ms + 5000` ms. También fija el plazo de **retirada** de un worker que no confirma una activación: `max(valor viejo, valor nuevo) + 5000` ms tras su SIGTERM, y después SIGKILL. |
| `ipc_timeout_ms` | entero | 100..600000 | 10000 | Plazo del maestro para: que cada worker avise de que está listo (READY); la fase de preparación de una recarga (valor vigente); la confirmación de la activación (valor de la configuración **nueva**). No acota toda la recuperación tras un fallo: la retirada, `restart_backoff_ms` y el arranque se suman (`docs/architecture.md` §14.3.1). |
| `restart_backoff_ms` | entero | 10..60000 | 500 | Espera antes de reponer un worker caído; se duplica en cada caída seguida (máx. 30 s) y vuelve al valor inicial cuando el worker queda listo. |
| `max_restarts` | entero | 0..1000 | 5 | Reposiciones permitidas por worker dentro de `restart_window_ms` (la salida de un worker retirado también cuenta). Superado, ese worker queda **fallido** (sin reponer); si fallan todos, el maestro termina con código 1. Tras una activación parcial la generación nueva se mantiene: `recuperación agotada` y `degraded` en las estadísticas. |
| `restart_window_ms` | entero | 1000..3600000 | 60000 | Ventana de `max_restarts`. |

**Alcance por worker.** Cada worker tiene su propio bucle, sus conexiones,
su registro de backends y sus sondas de salud. Por eso `max_connections`,
`max_conns` de un backend, los cursores de `round_robin`/`weighted` y las
cuentas de `least_conn` son **de cada worker**: con N workers un backend con
`max_conns = 1` puede recibir hasta N conexiones simultáneas, y las sondas
activas se multiplican por N (ver `docs/architecture.md` §14).

## `[limits]`

| Clave | Rango | Por defecto | Efecto |
|---|---|---|---|
| `max_header_bytes` | 1024..16384 | 16384 | Cabecera de petición mayor → `431`. Máximo = un slot. |
| `max_request_line` | 64..`max_header_bytes` | 8192 | Línea de petición mayor → `414`. |
| `max_headers` | 1..100 | 100 | Más campos → `431`. |
| `max_body_bytes` | 0..2^62 | 0 (sin límite) | Cuerpo de petición mayor → `413` (Content-Length al analizar la cabecera; chunked en streaming). |
| `max_requests_per_connection` | 1..10000000 | 1000 | Al llegar al límite la respuesta lleva `Connection: close`. |

## `[timeouts]` (milisegundos, 1..3600000)

| Clave | Por defecto | Cuándo corre | Al vencer |
|---|---|---|---|
| `client_header_ms` | 10000 | Desde el primer byte de la petición (plazo absoluto) o desde la aceptación hasta la primera petición | `408` si hay bytes; cierre silencioso si no |
| `client_idle_ms` | 60000 | Keep-alive sin bytes de la siguiente petición | cierre silencioso |
| `upstream_connect_ms` | 3000 | `connect` no bloqueante en curso | `504` (se contabiliza como fallo del backend) |
| `upstream_response_ms` | 30000 | Petición enviada entera, sin cabecera de respuesta | `504` |
| `io_idle_ms` | 30000 | Transferencia de cuerpos sin progreso | sin respuesta empezada: `504` si el lento es el upstream, `408` si es el cliente; con respuesta empezada: cierre (no se inserta otra respuesta) |
| `close_ms` | 2000 | Cierre: vaciado de la salida y espera del EOF del cliente tras `shutdown(SHUT_WR)` | cierre forzado |

## `[[frontend]]` (1..16)

| Clave | Tipo | Notas |
|---|---|---|
| `listen` | cadena | `"IPv4:puerto"` o `"[IPv6]:puerto"`. **Solo IP literal.** Puertos 1..65535; repetidos se rechazan. **No recargable** (ni el número ni el orden de frontends). |
| `trusted_proxies` | lista de cadenas | IP literales (sin CIDR ni nombres), hasta 64. Solo los pares de esta lista pueden aportar `X-Forwarded-For`, `X-Real-IP` y `X-Forwarded-Proto`. |

Cada worker abre su propio listener por frontend con `SO_REUSEADDR` y
`SO_REUSEPORT`; el núcleo reparte las conexiones nuevas entre ellos (el
reparto no es necesariamente uniforme). Si un puerto está ocupado por otro
proceso sin `SO_REUSEPORT`, el arranque falla limpiamente (código 1).

## `[[pool]]` (1..256)

| Clave | Tipo | Notas |
|---|---|---|
| `name` | cadena | `[A-Za-z0-9_-]`, 1..64, único. |
| `algorithm` | cadena | `"round_robin"` (por defecto), `"weighted"` o `"least_conn"` (ver abajo). |
| `[[pool.backend]]` | tablas (1..64) | También vale `backend = [ { address = "..." } ]`. |
| `[pool.health]` | tabla | Opcional: health checks activos y pasivos (ver abajo). |

`[[pool.backend]]`:

| Clave | Rango | Por defecto | Notas |
|---|---|---|---|
| `address` | — | obligatoria | `"IP:puerto"`. **Solo IP literal**: un nombre (p. ej. `localhost`) se rechaza porque la resolución DNS no está implementada. Repetida en el pool → error. |
| `weight` | 1..1000 | 1 | Solo lo usa `weighted`. |
| `max_conns` | 0..100000 | 0 (sin límite) | Un backend en su máximo no es elegible; con todos así → `503`. |

### Algoritmos

Solo se eligen backends **elegibles**: sanos y por debajo de `max_conns`. Si
no hay ninguno → `503`.

| Algoritmo | Selección | Empates |
|---|---|---|
| `round_robin` | el siguiente elegible desde un cursor que avanza tras cada elección | — |
| `weighted` | *smooth weighted round robin* (el de nginx): cada elegible suma su `weight` a su contador; se elige el mayor y se le resta la suma de pesos de los elegibles. En cada ciclo de `Σ weight` elecciones cada backend sale `weight` veces, intercalado (3:1 → A A B A) | menor índice |
| `least_conn` | el elegible con menos conexiones upstream activas **de la generación vigente** | el primero recorriendo desde el cursor de round robin (los empates rotan) |

### `[pool.health]`

Sin esta tabla no hay sondas ni exclusión pasiva: los backends del pool son
siempre sanos (solo cuenta `max_conns`), porque nada podría readmitir uno
excluido.

| Clave | Rango | Por defecto | Notas |
|---|---|---|---|
| `type` | `"tcp"` \| `"http"` | obligatoria | tcp: el `connect` termina bien. http: además la línea de estado llega y su código está en el rango esperado |
| `interval_ms` | 100..3600000 | 2000 | Entre inicios de sonda |
| `timeout_ms` | 10..60000 | 1000 | Debe ser menor que `interval_ms` |
| `fall` | 1..100 | 3 | Sondas fallidas consecutivas para excluir |
| `rise` | 1..100 | 2 | Sondas correctas consecutivas para readmitir |
| `passive_fall` | 0..100 | 3 | Fallos **reales** de conexión consecutivos para excluir; 0 lo desactiva |
| `path` | `/…` sin espacios | `"/health"` | Solo http: `GET <path> HTTP/1.1`, `Connection: close` |
| `host` | host válido | dirección del backend | Solo http: cabecera `Host` de la sonda |
| `expect_status_min` / `_max` | 100..599 | 200 / 399 | Solo http: estados considerados sanos |

Health pasivo: cuentan `ECONNREFUSED`, `ETIMEDOUT` (incluido el vencimiento
de `upstream_connect_ms`), `EHOSTUNREACH`, `ENETUNREACH`, `ECONNRESET`,
`ECONNABORTED`, `ENETDOWN` y `EHOSTDOWN` al conectar. **No cuentan**: errores
o cancelaciones del cliente, saturación local (`max_conns`, sin slots, sin
fds, sin puertos efímeros), timeouts de respuesta ni códigos HTTP del
backend. Una conexión correcta reinicia la cuenta. Un backend excluido (por
sondas o en pasivo) solo vuelve cuando `rise` sondas seguidas son correctas.
La petición que descubre el fallo pasivo recibe `502`: no se reintenta.

Límite: 1024 backends con sondas en total.

## `[[route]]` y `[routing]`

| Clave | Notas |
|---|---|
| `[[route]].host` | Dominio exacto o `*.sufijo`. Se normaliza (minúsculas, sin punto final). Sin puerto. Duplicados tras normalizar → error. |
| `[[route]].pool` | Nombre de un `[[pool]]` existente. |
| `[routing].default_pool` | Opcional. Sin ella, un Host sin ruta recibe `502`. |

Precedencia: exacto → wildcard con el sufijo más largo → `default_pool` →
`502`. Las rutas son comunes a todos los frontends.

Debe existir al menos una `[[route]]` o `default_pool`.

## `[log]`

| Clave | Tipo | Por defecto | Notas |
|---|---|---|---|
| `dir` | cadena (ruta absoluta) | `""` | Directorio de logs. Cada worker escribe en `<dir>/worker-<i>.log` (se crea con modo 0640, `O_APPEND`). Vacío: los workers escriben en stderr. El maestro escribe siempre en stderr. |
| `level` | `"debug"`, `"info"`, `"warn"`, `"error"` | `"info"` | Nivel mínimo. |
| `access` | booleano | `false` | Una línea por petición: `access client=… host=… method=… status=… backend=… ms=…`. Nunca incluye la ruta, la query, cuerpos ni cabeceras (ni credenciales ni cookies). |

Cada registro: `ISO-8601-UTC pid=P worker=N level=L gen=G mensaje` (en el
maestro, `role=master`). Máximo 512 bytes; los más largos se truncan con
` [truncado]`. Los caracteres de control se sustituyen por `?`.

El log de cada worker es **asíncrono**: los hilos producen en un ring de
4096 registros y un hilo consumidor escribe. Si el ring está lleno el
registro se **descarta** y se cuenta (`log_dropped`); el tráfico nunca espera
al disco. Al cerrar, el consumidor vacía lo pendiente con un plazo de 2 s; si
el destino no avanza, lo abandona. **No recargable.**

## `[stats]`

| Clave | Tipo | Por defecto | Notas |
|---|---|---|---|
| `socket` | cadena (ruta absoluta, < 104 bytes) | `""` (desactivado) | Socket UNIX de estadísticas del maestro. |
| `mode` | entero (p. ej. `0o600`) | `0o600` | Permisos del socket; debe incluir lectura y escritura del propietario. Se crea con `umask 0177` y después se aplica `mode`. |
| `timeout_ms` | 50..60000 | 2000 | Plazo total por cliente. La recogida de los workers dura como mucho la mitad; los que no respondan figuran como ausentes. |
| `max_clients` | 1..256 | 16 | Clientes simultáneos; los sobrantes se cierran sin datos (`stats_rejected`). |

Si la ruta ya existe (fichero, socket de otro proceso o socket huérfano) el
maestro **no la borra** y el arranque falla con código 1: hay que retirarla a
mano. Al terminar se borra solo si sigue siendo el mismo socket que creó
(mismo dispositivo e inodo). Protocolo y esquema JSON:
`docs/architecture.md` §14.6; cliente: `scripts/proxy-stats.py`. **No
recargable.**

## Restricciones de esta versión

- **IP literales**: sin DNS en `listen`, `address` ni `trusted_proxies`.
- **Estado por worker**: límites, balanceo y salud no se comparten entre
  workers (ver arriba).
- **Recarga parcial**: ver la sección siguiente.
- **Sin TLS**: ninguna clave activa HTTPS.

## Recarga (`SIGHUP`)

```bash
kill -HUP <pid-del-maestro>   # vuelve a leer el mismo fichero que -c
```

- El **maestro** lee el fichero una vez, lo valida y comprueba la
  compatibilidad; después envía **exactamente esos bytes** a todos los
  workers (preparación y activación en dos fases, `docs/architecture.md`
  §14.3). Cada worker analiza el texto en un hilo auxiliar: su event loop no
  bloquea.
- **Válida y compatible** → `recarga aplicada: generación N (K workers)` en el
  log del maestro, cuando todos los workers vivos la han activado. Si un
  worker muere o no confirma durante la activación (plazo vencido o
  `COMMIT_FAILED`), la recarga **no se deshace**: se anuncia `recarga
  aplicada: generación N con capacidad reducida (K de W workers
  confirmados; M por reponer con la generación N)`, el que no confirmó se
  retira (SIGTERM y, si no termina a tiempo, SIGKILL) y los sustitutos
  arrancan con la generación N. Después: `recuperación completa` o
  `recuperación agotada` (ver `docs/architecture.md` §14.3.1). Las peticiones en curso
  terminan con la configuración anterior; las nuevas, incluida la siguiente
  de una conexión keep-alive, usan la nueva.
- **Inválida**, o que falle en algún worker → `recarga rechazada:
  <diagnóstico>; se mantiene la generación N`. Ningún worker cambia.
- **No recargable** → rechazada con la lista de diferencias:
  `[server].workers`, `[server].max_connections`, los `[[frontend]]`
  (número, orden y `listen`), `[log]` y `[stats]`. Para esos cambios hay que
  reiniciar. (`ipc_timeout_ms`, `restart_backoff_ms`, `max_restarts` y
  `restart_window_ms` sí se recargan: el maestro los aplica al activar.)
- **Recargables**: todo lo demás (`[[pool]]`, algoritmos, pesos,
  `max_conns`, `[pool.health]`, `[[route]]`, `[routing]`, `[limits]`,
  `[timeouts]`, `trusted_proxies`, `shutdown_timeout_ms`).
- **Estado entre recargas**: un backend lógico se identifica por (nombre del
  pool, dirección). Conserva sus conexiones activas, su salud y sus
  contadores. `least_conn` y `max_conns` de la configuración nueva cuentan
  las conexiones que siguen activas desde configuraciones anteriores.
- **Bajar `max_conns`** por debajo de las activas no corta nada: no se
  asignan nuevas a ese backend hasta que baje del límite.
- **Quitar y volver a poner** un backend: si aún tiene peticiones de una
  configuración anterior, al reintroducirlo recupera esas activas y su
  salud; si no, empieza de cero.
- Límites y contadores son **por pool/backend**: la misma dirección en dos
  pools tiene dos `max_conns` y dos contadores independientes.
- Todo lo anterior es por worker: cada worker conserva el estado de sus
  propios backends.
- Varios `SIGHUP` durante una recarga (o mientras arranca o se repone un
  worker) se agrupan en una recarga más al terminar. Un `SIGTERM` durante
  una recarga la descarta.

El análisis no bloquea el event loop de los workers: se sigue atendiendo con
la configuración vigente mientras dura (probado con una recarga retrasada 3 s).

Retardo solo para pruebas: `PROXY_TEST_RELOAD_DELAY_MS` existe únicamente en
el ejecutable de pruebas `build/src/proxy-testhooks`. El ejecutable `proxy`
ignora esa variable (no contiene el código).

## Ejemplos

- `proxy.toml`: demo con 3 workers, dos frontends (7777, 7778), backends
  3000 y 3001, rutas exacta, wildcard y por defecto, y `[stats]` (la demo
  sustituye la ruta del socket por una propia).
- `examples/proxy-noroute.toml`: sin ruta por defecto (7779), para ver el
  `502`.
- `examples/proxy-reload.toml`: recarga válida (api.example.com → B; pool
  por defecto weighted 3:1).
- `examples/proxy-invalid.toml`: recarga inválida (pool inexistente).
- `examples/proxy-port-change.toml`: recarga no permitida (cambia un puerto).
