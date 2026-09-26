# Demostración

Proxy con **3 workers** (procesos), dos frontends y dos backends
identificables. Las salidas que se muestran se obtuvieron al ejecutar estos
comandos en el entorno de desarrollo (Ubuntu 26.04 sobre WSL2, 2026-09-25,
etapa 5). Los PID y marcas de tiempo cambian en cada ejecución; el reparto
de conexiones entre workers lo decide el núcleo (`SO_REUSEPORT`), así que las
secuencias de backends de conexiones nuevas también pueden variar.

Requisitos: Meson, Ninja, un compilador C11 y `python3` (≥ 3.9) para los
backends. Puertos libres: 3000, 3001, 7777, 7778 (y 7779 para la variante
sin ruta).

## 1. Compilar y validar la configuración

```bash
meson setup build
meson compile -C build
./build/src/proxy -t -c proxy.toml
```

```
proxy.toml: configuración válida (2 frontends, 3 pools, 2 rutas, con ruta por defecto, 3 workers)
```

`-t` no abre ningún socket.

Configuración de `proxy.toml`:

| Frontend | Confianza en X-Forwarded-* |
|---|---|
| `127.0.0.1:7777` | ninguna |
| `127.0.0.1:7778` | `127.0.0.1` |

| Host | Pool | Backends |
|---|---|---|
| `api.example.com` (exacto) | `a` | A (3000) |
| `*.example.com` (wildcard) | `b` | B (3001) |
| cualquier otro (`default_pool`) | `ab` | A y B en round robin |

## 2. Levantar backends y proxy

```bash
bash scripts/demo.sh start
```

```
demo: backend-A en marcha (pid …)
demo: backend-B en marcha (pid …)
…/proxy.toml: configuración válida (2 frontends, 3 pools, 2 rutas, con ruta por defecto, 3 workers)
demo: proxy en marcha (pid …)
demo: logs en /tmp/proxy-demo-<uid>
```

```bash
bash scripts/demo.sh workers
```

```
maestro pid=263760
worker 0 arrancado pid=263762
worker 1 arrancado pid=263763
worker 2 arrancado pid=263764
```

El script guarda un `.pid` (el del **maestro**) y un `.log` por proceso en
`${TMPDIR:-/tmp}/proxy-demo-<uid>` (se puede cambiar con `DEMO_DIR`). Al
copiar `proxy.toml` sustituye la ruta de `[stats].socket` por
`$DEMO_DIR/stats.sock`, privada de la demo. El maestro y los workers escriben
en el mismo `proxy.log` (`[log].dir` vacío = stderr); cada línea indica
`role=master` o `worker=N`. Equivalente manual, en tres terminales (con la
ruta del socket de `proxy.toml`, `/tmp/proxy-stats.sock`):

```bash
python3 tests/integration/backend.py --port 3000 --name A --verbose
python3 tests/integration/backend.py --port 3001 --name B --verbose
./build/src/proxy -c proxy.toml
```

Para la variante sin ruta por defecto (puerto 7779):

```bash
bash scripts/demo.sh start-noroute
```

## 3. Observar el routing

Cada backend responde en `/echo` un JSON con su nombre (`backend`) y las
cabeceras que recibió.

```bash
for h in api.example.com www.example.com a.b.example.com otro.org; do
  printf '%-18s -> ' "$h"
  curl -s -H "Host: $h" http://127.0.0.1:7777/echo \
    | python3 -c 'import sys,json; print(json.load(sys.stdin)["backend"])'
done
```

```
api.example.com    -> A      (exacto)
www.example.com    -> B      (wildcard)
a.b.example.com    -> B      (wildcard, varias etiquetas)
otro.org           -> A      (por defecto; A o B según el turno del worker)
```

El mismo resultado se obtiene en el puerto 7778.

## 4. Observar el round robin

Cada worker tiene su propio cursor. Con conexiones nuevas (cada `curl`
abre una) las peticiones caen en workers distintos, así que la secuencia
global no alterna estrictamente:

```bash
for i in 1 2 3 4 5 6 7 8; do
  curl -s http://127.0.0.1:7778/echo \
    | python3 -c 'import sys,json; print(json.load(sys.stdin)["backend"], end=" ")'
done; echo
```

```
A B B A B A A B
```

Dentro de **una** conexión keep-alive (un solo worker) la alternancia es
exacta:

```bash
curl -s $(for i in 1 2 3 4 5 6; do printf 'http://127.0.0.1:7778/echo '; done) \
  | python3 -c 'import sys,re; print(" ".join(re.findall(r"\"backend\": \"(\w)\"", sys.stdin.read())))'
```

```
A B A B A B
```

## 5. Cabeceras de reenvío

En el frontend 7778 el par `127.0.0.1` es de confianza y se conserva su
`X-Forwarded-For`:

```bash
curl -s -H 'Host: api.example.com' -H 'X-Forwarded-For: 203.0.113.7' \
  http://127.0.0.1:7778/echo \
  | python3 -c 'import sys,json; [print(k+":", v) for k, v in json.load(sys.stdin)["headers"]]'
```

```
Host: api.example.com
User-Agent: curl/8.18.0
Accept: */*
X-Forwarded-For: 203.0.113.7, 127.0.0.1
X-Real-IP: 127.0.0.1
X-Forwarded-Proto: http
Connection: close
```

En el 7777 no hay confianza y el valor del cliente se descarta:

```bash
curl -s -H 'Host: api.example.com' -H 'X-Forwarded-For: 203.0.113.7' \
  http://127.0.0.1:7777/echo \
  | python3 -c 'import sys,json; [print(k+":", v) for k, v in json.load(sys.stdin)["headers"] if k.startswith("X-")]'
```

```
X-Forwarded-For: 127.0.0.1
X-Real-IP: 127.0.0.1
X-Forwarded-Proto: http
```

`Connection: close` hacia el backend es deliberado: la primera versión no
reutiliza conexiones con el upstream (el cliente sí puede usar keep-alive).

## 6. Errores

```bash
# Sin ruta (proxy de start-noroute): 502
curl -s -o /dev/null -w '%{http_code}\n' -H 'Host: otro.org' http://127.0.0.1:7779/
# WebSocket: rechazado explícitamente
curl -s -i -H 'Connection: Upgrade' -H 'Upgrade: websocket' \
  -H 'Host: api.example.com' http://127.0.0.1:7777/ | head -1
```

```
502
HTTP/1.1 501 Not Implemented
```

Otros casos (cubiertos por las pruebas de integración, no incluidos en la
captura anterior): `curl -s -i http://127.0.0.1:7777/garbage` (respuesta inválida
del backend → 502), `/switch` (101 → 502), `/slow?delay_ms=40000` (→ 504 a
los 30 s con `proxy.toml`). La lista completa de rutas del backend está en la
cabecera de `tests/integration/backend.py`.

## 7. Health checks: caída y recuperación

`proxy.toml` sondea el pool por defecto (`ab`) con `GET /health` cada
segundo (`fall = 2`, `rise = 2`, `passive_fall = 1`). Estas salidas se
capturaron el 2026-09-25 con los comandos siguientes.

Función auxiliar para ver qué backend atiende seis peticiones seguidas:

```bash
rr() { for i in 1 2 3 4 5 6; do
  curl -s -o /dev/null -D - http://127.0.0.1:7777/echo | tr -d '\r' \
    | awk -F': ' '/^X-Backend/{printf "%s ", $2}'; done; echo; }
rr                                   # p. ej. B A A B A B (un worker por conexión)
```

Cada worker sondea por su cuenta: **las sondas se multiplican por 3** y cada
worker registra su propio cambio de estado.

**Sonda HTTP fallida** (A responde 500 en `/health`):

```bash
bash scripts/demo.sh health A 500
sleep 3; grep "health:" /tmp/proxy-demo-$(id -u)/proxy.log | tail -1
rr
```

```
… pid=263764 worker=2 level=WARN gen=1 health: generación 1 pool=ab backend=127.0.0.1:3000 CAÍDO (activo: estado HTTP 500)
… pid=263763 worker=1 level=WARN gen=1 health: generación 1 pool=ab backend=127.0.0.1:3000 CAÍDO (activo: estado HTTP 500)
… pid=263762 worker=0 level=WARN gen=1 health: generación 1 pool=ab backend=127.0.0.1:3000 CAÍDO (activo: estado HTTP 500)
B B B B B B
```

```bash
bash scripts/demo.sh health A 200
sleep 3; grep "health:" /tmp/proxy-demo-$(id -u)/proxy.log | tail -3; rr
```

```
… worker=1 … health: generación 1 pool=ab backend=127.0.0.1:3000 SANO (activo: sonda correcta)
… worker=0 … health: generación 1 pool=ab backend=127.0.0.1:3000 SANO (activo: sonda correcta)
… worker=2 … health: generación 1 pool=ab backend=127.0.0.1:3000 SANO (activo: sonda correcta)
A B A A A B
```

**Caída real** (se detiene el proceso B): en **cada worker** la primera
petición que intenta conectar con B recibe 502 y lo excluye en pasivo; con 3
workers puede haber hasta tres 502 (el pasivo es por worker):

```bash
bash scripts/demo.sh backend-stop B
for i in $(seq 1 12); do curl -s -o /dev/null -w "%{http_code} " http://127.0.0.1:7777/echo; done; echo
grep "health:" /tmp/proxy-demo-$(id -u)/proxy.log | tail -3; rr
bash scripts/demo.sh stats | grep "ab/"
```

```
200 502 502 200 502 200 200 200 200 200 200 200
… worker=2 … health: generación 1 pool=ab backend=127.0.0.1:3001 CAÍDO (pasivo: fallo de conexión)
… worker=1 … health: generación 1 pool=ab backend=127.0.0.1:3001 CAÍDO (pasivo: fallo de conexión)
… worker=0 … health: generación 1 pool=ab backend=127.0.0.1:3001 CAÍDO (pasivo: fallo de conexión)
A A A A A A
  ab/127.0.0.1:3000 (round_robin) activas=0 elegido=30 fallos=0 salud=up {'up': 3, 'down-active': 0, 'down-passive': 0}
  ab/127.0.0.1:3001 (round_robin) activas=0 elegido=21 fallos=3 salud=down {'up': 0, 'down-active': 0, 'down-passive': 3}
```

Las estadísticas muestran la salud de cada worker (`health_by_worker`) y un
resumen: `up`, `down` o `mixed` (workers en desacuerdo durante un
intervalo).

```bash
bash scripts/demo.sh backend-start B
sleep 3; rr
```

```
… worker=1 … health: generación 1 pool=ab backend=127.0.0.1:3001 SANO (activo: sonda correcta)
… worker=2 … health: generación 1 pool=ab backend=127.0.0.1:3001 SANO (activo: sonda correcta)
… worker=0 … health: generación 1 pool=ab backend=127.0.0.1:3001 SANO (activo: sonda correcta)
B B A B A A
```

Con todos los backends de un pool caídos el proxy responde **503** (probado
en `tests/integration/test_health.py`).

## 8. Recarga coordinada (SIGHUP al maestro)

`scripts/demo.sh reload FICHERO` copia el fichero sobre la configuración
activa de la demo (`$DEMO_DIR/proxy.toml`, no la del repositorio, con la
misma sustitución del socket) y envía `SIGHUP` al **maestro**. El maestro lee
el fichero una vez, lo valida y lo envía a los 3 workers en dos fases
(preparar → activar); solo informa `recarga aplicada` cuando los 3 han
activado la generación nueva.

**Válida**: `examples/proxy-reload.toml` lleva `api.example.com` a B y hace
el pool por defecto `weighted` 3:1.

```bash
curl -s -D - -o /dev/null -H 'Host: api.example.com' http://127.0.0.1:7777/echo | grep X-Backend
bash scripts/demo.sh reload examples/proxy-reload.toml
curl -s -D - -o /dev/null -H 'Host: api.example.com' http://127.0.0.1:7777/echo | grep X-Backend
for i in $(seq 1 40); do curl -s -D - -o /dev/null http://127.0.0.1:7777/echo \
  | tr -d '\r' | awk -F': ' '/^X-Backend/{printf "%s ", $2}'; done; echo
```

```
X-Backend: A
… role=master level=INFO gen=1 recarga iniciada (generación actual 1)
… role=master level=INFO gen=2 recarga aplicada: generación 2 (3 workers)
X-Backend: B
A A A B A B A A A A A A A B A B A A A A B A A A A B B A A A A B A B A A A B A A
```

30 A y 10 B en 40 conexiones nuevas: cada worker aplica 3:1 sobre lo que él
atiende. En una sola conexión keep-alive (un worker):
`A A A B A A A B`.

**Inválida** (pool inexistente): se rechaza y todo sigue igual.

```bash
bash scripts/demo.sh reload examples/proxy-invalid.toml
```

```
… role=master level=INFO gen=2 recarga iniciada (generación actual 2)
… role=master level=WARN gen=2 recarga rechazada: …/proxy.toml: [[route]] #2 (host "*.example.com"): el pool "no-existe" no existe; se mantiene la generación 2
```

**No recargable** (cambia el puerto 7778): se rechaza sin interrumpir el
servicio.

```bash
bash scripts/demo.sh reload examples/proxy-port-change.toml
curl -s -o /dev/null -w "7778 sigue: %{http_code}\n" http://127.0.0.1:7778/echo
```

```
… role=master level=WARN gen=2 recarga rechazada: [[frontend]] #2 listen 127.0.0.1:7778 -> 127.0.0.1:7780; no son recargables (requieren reiniciar el proceso); se mantiene la generación 2
7778 sigue: 200
```

Que una transferencia larga iniciada antes de la recarga termine completa
con el destino anterior, y que la siguiente petición de la misma conexión
keep-alive use el nuevo, se comprueba en `tests/integration/test_reload.py`
y `test_multiprocess.py`. Qué pasa si un worker muere durante la
preparación o la activación: `docs/architecture.md` §14.3.

## 9. Estadísticas (socket UNIX)

```bash
bash scripts/demo.sh stats          # resumen legible
bash scripts/demo.sh stats --json   # documento JSON completo
# equivalente directo:
python3 scripts/proxy-stats.py /tmp/proxy-demo-$(id -u)/stats.sock
```

```
maestro pid=263760 uptime=11.8s generación=2 workers=3 recarga_en_curso=False reinicios=0
completo=True sin_respuesta=[]
  worker 0 pid=263762 estado=ready gen=2 conns=0 peticiones=24 aceptadas=25 5xx=2 log_descartados=0
  worker 1 pid=263763 estado=ready gen=2 conns=0 peticiones=43 aceptadas=31 5xx=1 log_descartados=0
  worker 2 pid=263764 estado=ready gen=2 conns=0 peticiones=46 aceptadas=46 5xx=1 log_descartados=0
totales: peticiones=113 conns=0 slots=0/12288 log_descartados=0
  a/127.0.0.1:3000 (round_robin) activas=0 elegido=4 fallos=0 salud=up {'up': 3, 'down-active': 0, 'down-passive': 0}
  b/127.0.0.1:3001 (round_robin) activas=0 elegido=3 fallos=0 salud=up {'up': 3, 'down-active': 0, 'down-passive': 0}
  ab/127.0.0.1:3000 (weighted) activas=0 elegido=70 fallos=0 salud=up {'up': 3, 'down-active': 0, 'down-passive': 0}
  ab/127.0.0.1:3001 (weighted) activas=0 elegido=36 fallos=3 salud=up {'up': 3, 'down-active': 0, 'down-passive': 0}
```

Nota: esta captura es anterior a la corrección de la retirada de workers.
Desde entonces el resumen añade una segunda línea con el estado de la
recarga y de la recuperación, y el objeto `master` del JSON los campos
`workers_ready`, `degraded`, `reload_state` y `recovery`
(`docs/architecture.md` §14.6). Captura real de esa línea, con 2 workers
recién arrancados:

```
recarga=idle recuperación=none(gen 0) listos=2/2 degradado=False
```

`peticiones` y `aceptadas` (conexiones) no tienen por qué coincidir: una
conexión keep-alive lleva varias peticiones y una conexión puede cerrarse
sin que se contabilice ninguna. Extractos del JSON (esquema 1):

```
{"schema": 1, "complete": true, "missing_workers": []}
master: {"pid": 263760, "uptime_ms": 11900, "generation": 2, "workers_configured": 3, "reload_in_progress": false, "draining": false, "reloads_ok": 1, "reloads_failed": 2, "worker_restarts": 0, "stats_rejected": 0, "stats_timeouts": 0}
workers[0]: {"index": 0, "pid": 263762, "state": "ready", "restarts": 0, "responded": true, "generation": 2, …}
totals.backends[2]: {"pool": "ab", "addr": "127.0.0.1:3000", "algorithm": "weighted", "active": 0, "selected": 70, "failures": 0, "health_by_worker": {"up": 3, "down-active": 0, "down-passive": 0}, "health_summary": "up"}
```

El socket se crea con permisos `srw-------` (0600). Esquema, tipos de
contador (acumulado/instantáneo) y protocolo: `docs/architecture.md` §14.6.

**Worker caído y repuesto** (se mata uno con `kill -9`):

```bash
kill -9 <pid del worker 0>     # p. ej. el de "bash scripts/demo.sh workers"
sleep 1.5; grep -E "terminó inesperadamente|se repondrá|repuesto" /tmp/proxy-demo-$(id -u)/proxy.log
bash scripts/demo.sh stats | head -5
```

```
… role=master level=WARN gen=2 worker 0 (pid 263762) terminó inesperadamente (señal 9)
… role=master level=INFO gen=2 worker 0 se repondrá en 500 ms
… role=master level=INFO gen=2 worker 0 repuesto (pid 264359, generación 2)
maestro pid=263760 uptime=13.6s generación=2 workers=3 recarga_en_curso=False reinicios=1
completo=True sin_respuesta=[]
  worker 0 pid=264359 estado=ready gen=2 conns=0 peticiones=0 aceptadas=0 5xx=0 log_descartados=0
  worker 1 pid=263763 estado=ready gen=2 conns=0 peticiones=43 aceptadas=31 5xx=1 log_descartados=0
  worker 2 pid=263764 estado=ready gen=2 conns=0 peticiones=46 aceptadas=46 5xx=1 log_descartados=0
```

El sustituto nace con la generación vigente (2) y sus contadores
acumulados empiezan en 0.

## 10. Detener

```bash
bash scripts/demo.sh stop
```

```
demo: proxy-noroute detenido (pid …)
demo: proxy detenido (pid …)
demo: backend-A detenido (pid …)
demo: backend-B detenido (pid …)
demo: resumen de cierre del proxy:
… (proxy-noroute: 1 worker, código 0)
… role=master … worker 0 (pid 263762) terminó inesperadamente (señal 9)
… pid=263763 worker=1 … shutdown conns=0 upstreams=0 slots_in_use=0 slots_total=4096 backend_active=0 generations_live=1 backends_live=4
… pid=264359 worker=0 … shutdown conns=0 upstreams=0 slots_in_use=0 slots_total=4096 backend_active=0 generations_live=1 backends_live=4
… pid=263764 worker=2 … shutdown conns=0 upstreams=0 slots_in_use=0 slots_total=4096 backend_active=0 generations_live=1 backends_live=4
… role=master … worker 1 (pid 263763) terminó (código 0)
… role=master … worker 2 (pid 263764) terminó (código 0)
… role=master … worker 0 (pid 264359) terminó (código 0)
… role=master level=INFO gen=- maestro: fin (código 0)
```

`stop` envía SIGTERM solo a los PID que registró `start` (el maestro, tras
comprobar que su línea de comandos es la esperada), espera y borra su
directorio. El maestro coordina el cierre: reenvía SIGTERM a sus workers,
cada uno deja de aceptar, termina sus peticiones activas dentro de
`shutdown_timeout_ms` y comprueba al salir que no quedan slots, conexiones
ni upstreams prestados (si quedaran, sale con 70 y el maestro también). El
maestro borra su socket de estadísticas y termina con 0 si todos los workers
salieron con 0.

Con el proxy en primer plano, `Ctrl+C` llega solo al maestro (los workers
están en su propio grupo de procesos) y produce el mismo cierre coordinado;
un segundo `Ctrl+C` lo fuerza.
