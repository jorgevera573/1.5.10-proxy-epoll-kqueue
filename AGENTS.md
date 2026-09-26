# Instrucciones del proyecto

## Objetivo
Implementar el proxy inverso HTTP L7 descrito en README.md.
Usar C11, Meson, epoll en Linux y kqueue en macOS.
Los criterios verificables están en docs/requirements.md.

## Forma de trabajo
- Revisar README.md y docs/requirements.md antes de modificar código.
- Implementar por etapas compilables y comprobables.
- No declarar una función terminada sin pruebas que la ejerciten.
- Registrar decisiones, limitaciones y comandos reproducibles.
- No inventar resultados de tests, compatibilidad ni benchmarks.
- No presentar las cifras originales del README como mediciones propias.
- No realizar commits ni push salvo instrucción del usuario.

## Arquitectura y seguridad
- Separar eventos, conexiones, HTTP, rutas, configuración y backends.
- Mantener una API común para epoll y kqueue.
- Usar sockets no bloqueantes y gestionar EAGAIN/EWOULDBLOCK y EINTR.
- Con eventos edge-triggered, drenar operaciones hasta EAGAIN.
- Manejar lecturas y escrituras parciales, límites y backpressure.
- Definir la propiedad y duración de cada recurso.
- Cerrar descriptores y liberar memoria en rutas normales y de error.
- No ejecutar parsing, asignaciones ni logging inseguro en handlers de señales.
- Documentar la sincronización entre procesos e hilos.
- Validar entradas HTTP y TOML; rechazar formatos no soportados explícitamente.
- No asumir que una llamada recv contiene una petición HTTP completa.

## Dependencias y calidad
- Usar dependencias con versiones o commits fijados.
- Conservar licencias de dependencias incorporadas.
- Compilar con advertencias estrictas.
- Incorporar tests unitarios cmocka y pruebas de integración.
- Incorporar comprobaciones de formato, análisis estático y CI.
- Usar sanitizers y Valgrind en ejecuciones separadas.
- Probar Linux y macOS mediante entornos reales o CI.
- Documentar cualquier plataforma pendiente de verificación.

## Rendimiento
- Medir builds release y comparar acceso directo frente al proxy.
- Registrar hardware, entorno, workers, comando, duración y errores.
- Informar latencias y throughput observados.
- Tratar 50.000 peticiones/s como objetivo sujeto a medición.
