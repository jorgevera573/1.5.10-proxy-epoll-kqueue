# Requisitos y criterios de aceptación

## Alcance
Proxy inverso HTTP L7 en C11 para Linux y macOS.
La base del enunciado es el README.md original, del commit 0fc621f
(`git show 0fc621f:README.md`); el README.md actual describe la
implementación.
Las cifras de rendimiento y número de tests del enunciado son
referencias: requieren verificación propia.

## Requisitos funcionales

| ID | Requisito | Evidencia de aceptación |
|----|-----------|------------------------|
| R01 | Build Meson y selección epoll/kqueue según plataforma | Compilación y tests en Linux y macOS |
| R02 | API común io_loop_create/add/mod/del/run/stop | Tests de registro, modificación, retirada y parada |
| R03 | Sockets no bloqueantes y eventos edge-triggered | Pruebas de fragmentación, escrituras parciales y clientes lentos |
| R04 | Múltiples frontends con SO_REUSEPORT y workers configurables | Solicitudes correctas por cada puerto y ejecución multiproceso |
| R05 | Interpretar HTTP Host e insertar cabeceras de reenvío | Backend de prueba verifica Host y política de X-Forwarded-For, X-Forwarded-Proto y X-Real-IP |
| R06 | Enrutar exacto, wildcard, default y finalmente 502 | Tests de precedencia y ausencia de coincidencias |
| R07 | Round robin, weighted y least_conn | Tests deterministas de selección, pesos y conexiones activas |
| R08 | Health checks activos TCP/HTTP y pasivos | Backend caído queda excluido y puede recuperarse |
| R09 | Configuración TOML con tomlc99 | Configuraciones válidas aceptadas e inválidas rechazadas con diagnóstico |
| R10 | Recarga SIGHUP sin cortar conexiones | Conexión en curso termina; nuevas solicitudes usan nuevas rutas; configuración inválida conserva la anterior |
| R11 | Logging mediante ring buffer 4096 x 512 bytes e hilo consumidor | Pruebas de concurrencia y política documentada de saturación |
| R12 | Pool de buffers mmap con slots de 16 KB y freelist | Tests de reserva, devolución y agotamiento |
| R13 | Estadísticas JSON mediante socket UNIX | JSON válido con uptime, contadores y estado de backends |
| R14 | Gestión del ciclo de vida HTTP y conexiones | Tests de cuerpos, límites, timeouts, desconexión y reutilización soportada |
| R15 | Cierre ordenado | SIGTERM/SIGINT permiten liberar recursos y retirar el socket UNIX propio |

## Diseño exigido
- Separar io_event, listener, connection, http_parser, router,
  backend_pool, health, config, log, buffer_pool y stats.
- Router con hash djb2 para dominios exactos.
- Recarga mediante notificación segura de señales, como self-pipe.
- Publicar la nueva configuración conservando referencias a la anterior
  mientras existan conexiones que la utilizan.
- Definir propagación de recargas y estadísticas entre workers.
- Definir sincronización de sondas, estado de backends y logging.
- Evitar crear hilos antes de fork sin una estrategia segura explícita.
- Definir límites de memoria y backpressure para clientes lentos.

## Decisiones que deben documentarse antes de implementar
- Alcance HTTP: framing de mensajes, Content-Length, chunked,
  keep-alive y tratamiento explícito de pipelining.
- Tratamiento de Host con puerto, mayúsculas y cabeceras duplicadas.
- Rechazo de mensajes ambiguos o malformados.
- Política de confianza de cabeceras de reenvío recibidas.
- TLS: el puerto 443 por sí solo no implica soporte HTTPS.
- Política para cambios de puertos y workers durante una recarga.
- Resolución de nombres de upstream sin bloquear el event loop.
- Comportamiento ante saturación, timeouts y falta de backends sanos.

## Verificación y entrega
- Tests unitarios cmocka y pruebas de integración automatizadas.
- Servicios de prueba identificables y limpieza de procesos al terminar.
- Formato, análisis estático y CI.
- AddressSanitizer/UndefinedBehaviorSanitizer y Valgrind por separado.
- Medición release con wrk, calentamiento y ejecuciones repetidas.
- Comparación del backend directo con acceso mediante el proxy.
- Registrar entorno, workers, concurrencia, latencias y errores.
- Objetivo de rendimiento: superar 50.000 peticiones/s.
- Informar el resultado real aunque no alcance el objetivo.
- README final reproducible y guion de demostración.
- Matriz requisito -> prueba -> resultado; indicar pendientes.
