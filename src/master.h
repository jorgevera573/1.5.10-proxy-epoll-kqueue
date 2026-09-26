/*
 * master.h — proceso maestro: crea y supervisa los workers, coordina la
 * recarga en dos fases y sirve las estadísticas por socket UNIX.
 *
 * El maestro NO crea hilos en ningún momento: así cualquier fork (arranque o
 * reposición de un worker) es seguro. Tampoco atiende tráfico HTTP. Su bucle
 * de eventos (io_event + timer) atiende: señales (self-pipe), canales IPC con
 * los workers y clientes de estadísticas. Sus mensajes de log se escriben de
 * forma síncrona en stderr (log.h, modo sin logger).
 *
 * Ver docs/architecture.md §14 para el protocolo y el comportamiento ante
 * fallos.
 */
#ifndef MASTER_H
#define MASTER_H

#include <stddef.h>

#include "config.h"

/*
 * Ejecuta el maestro con la configuración `cfg` (toma la propiedad de la
 * referencia de quien llama),
 * cuyo texto original es `text` (toma su propiedad; se reenvía a los workers
 * en las recargas) y cuya ruta es `path` (se relee con SIGHUP).
 * Códigos de salida: 0 cierre limpio de todos los workers; 1 fallo de
 * arranque o todos los workers fallidos; 70 algún worker terminó mal durante
 * el cierre.
 */
int master_run(struct config *cfg, char *text, size_t text_len, const char *path);

#endif /* MASTER_H */
