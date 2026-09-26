/*
 * log.h — logging asíncrono con ring buffer acotado y un hilo consumidor.
 *
 * Formato de cada registro (una línea, como máximo LOG_RECORD bytes con el
 * '\n'):
 *   2026-09-25T10:11:12.345Z pid=1234 worker=0 level=INFO gen=2 mensaje
 *   (en el maestro: "role=master" en lugar de "worker=N"; "gen=-" si no aplica)
 * Los caracteres de control del mensaje se sustituyen por '?', de modo que un
 * valor externo no puede inyectar líneas. Un mensaje más largo se trunca y
 * termina en " [truncado]".
 *
 * Sincronización: ring de `capacity` ranuras de LOG_RECORD bytes protegido
 * por un mutex. El productor formatea FUERA del mutex y dentro solo copia una
 * ranura (sección crítica acotada) y avisa con una variable de condición. El
 * consumidor saca lotes dentro del mutex y escribe FUERA de él.
 *
 * Saturación: con el ring lleno el mensaje nuevo se DESCARTA y se cuenta
 * (`dropped`); el productor nunca espera a la escritura. El consumidor emite
 * periódicamente "log: N mensajes descartados".
 *
 * Errores de escritura: se cuentan (`write_errors`) y el registro se pierde;
 * no se reintenta indefinidamente. Con un destino que no admite datos
 * (EAGAIN) el consumidor espera con poll() en tramos de 100 ms comprobando si
 * debe abandonar.
 *
 * Cierre: log_stop(timeout) pide al consumidor que vacíe el ring y espera
 * como máximo `timeout`. Si vence (destino bloqueado), marca abandono: el
 * consumidor deja de escribir en cuanto vuelve de poll/write y cuenta lo
 * pendiente como descartado. Si aun así no termina (un write() bloqueante en
 * un descriptor heredado, como stderr hacia un pipe lleno), log_stop devuelve
 * false sin liberar el logger: el proceso termina igualmente.
 *
 * Uso por proceso: log_install() fija el logger del proceso; plog()/diag() lo
 * usan desde cualquier hilo. Sin logger instalado (el maestro, que no crea
 * hilos) plog() escribe de forma síncrona en stderr con el mismo formato.
 */
#ifndef LOG_H
#define LOG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"

#define LOG_RING_CAP 4096
#define LOG_RECORD   512

typedef struct logger logger;

struct log_counters {
    uint64_t written;      /* registros escritos completos */
    uint64_t dropped;      /* descartados: ring lleno o abandono al cerrar */
    uint64_t write_errors; /* write() fallidos */
    uint64_t truncated;    /* mensajes recortados a LOG_RECORD */
};

/*
 * Arranca un logger que escribe en `fd` (si own_fd, lo cierra al terminar)
 * con `capacity` ranuras. `worker` < 0 significa el maestro. NULL con errno.
 */
logger *log_start(int fd, bool own_fd, enum log_level min, size_t capacity, int worker);

/* Abre `path` (O_APPEND, 0640) o usa stderr si path es NULL o vacío. */
logger *log_open(const char *path, enum log_level min, int worker, char *err, size_t errlen);

void log_install(logger *lg); /* NULL: modo síncrono */
logger *log_installed(void);
/* Papel para el modo síncrono: -1 maestro, >= 0 worker. */
void log_set_role(int worker);

/* Registra (no bloquea salvo el mutex de copia). gen = 0: "gen=-". */
#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
void plog(enum log_level level, uint64_t gen, const char *fmt, ...);

__attribute__((format(printf, 3, 0))) void plogv(enum log_level level, uint64_t gen,
                                                 const char *fmt, va_list ap);

#if defined(__GNUC__)
__attribute__((format(printf, 4, 5)))
#endif
void log_msg(logger *lg, enum log_level level, uint64_t gen, const char *fmt, ...);

void log_get_counters(logger *lg, struct log_counters *out);

/* Vacía y detiene con plazo. true si el consumidor terminó y se liberó. */
bool log_stop(logger *lg, uint32_t timeout_ms);

#endif /* LOG_H */
