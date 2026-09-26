/*
 * diag.c — ver diag.h.
 */
#include "diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "log.h"

void diag(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    /* Asíncrono si el proceso tiene logger (workers); síncrono si no. */
    plogv(LOG_INFO, 0, fmt, ap);
    va_end(ap);
}

const char *diag_strerror(int err, char *buf, size_t len) {
    /* Con _POSIX_C_SOURCE y sin _GNU_SOURCE, strerror_r es la variante XSI
     * (devuelve int) tanto en glibc como en macOS. */
    if (strerror_r(err, buf, len) != 0) {
        (void)snprintf(buf, len, "errno %d", err);
    }
    return buf;
}
