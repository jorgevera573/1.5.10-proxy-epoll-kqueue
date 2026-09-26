/*
 * diag.h — mensajes de diagnóstico (nivel INFO, sin generación).
 *
 * Se encaminan por log.h: en un worker van al ring buffer y los escribe el
 * hilo consumidor; en el maestro (sin hilos) se escriben de forma síncrona en
 * stderr con el mismo formato.
 */
#ifndef DIAG_H
#define DIAG_H

#include <stddef.h>

#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
void diag(const char *fmt, ...);

/*
 * Texto de un errno, reentrante (strerror_r XSI), apto para cualquier hilo.
 * Devuelve `buf`.
 */
const char *diag_strerror(int err, char *buf, size_t len);

#endif /* DIAG_H */
