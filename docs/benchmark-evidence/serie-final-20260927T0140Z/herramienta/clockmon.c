/*
 * clockmon.c — detecta saltos de CLOCK_REALTIME comparándolo con
 * CLOCK_MONOTONIC.
 *
 * Cada ~1 ms lee MONOTONIC, REALTIME y MONOTONIC otra vez (la lectura de
 * REALTIME queda acotada entre las dos monotónicas). Entre dos muestras
 * consecutivas compara el avance del reloj de pared con el monotónico:
 *   - avance de pared negativo            -> "BACKWARD" (siempre se registra)
 *   - |avance_pared - avance_mono| > 500 us + 10 % del intervalo
 *                                         -> "STEP" (supera cualquier slew de
 *                                            chrony: máx. 83.333 ppm ≈ 8,3 %)
 * Al final imprime un resumen: muestras, eventos, desfase total acumulado
 * entre relojes y el mayor intervalo sin muestrear.
 *
 * Uso: clockmon SEGUNDOS
 */
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int64_t ns(clockid_t id) {
    struct timespec t;
    clock_gettime(id, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static void iso(int64_t rt_ns, char *buf, size_t len) {
    time_t s = (time_t)(rt_ns / 1000000000);
    struct tm tm;
    gmtime_r(&s, &tm);
    size_t n = strftime(buf, len, "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(buf + n, len - n, ".%06" PRId64 "Z", (rt_ns % 1000000000) / 1000);
}

int main(int argc, char **argv) {
    double secs = argc > 1 ? atof(argv[1]) : 60;
    int64_t m0 = ns(CLOCK_MONOTONIC), r0 = ns(CLOCK_REALTIME);
    int64_t prev_m = m0, prev_r = r0;
    int64_t end = m0 + (int64_t)(secs * 1e9);
    uint64_t samples = 0, steps = 0, backward = 0;
    int64_t max_gap = 0;
    char when[64];
    struct timespec tick = {0, 1000000};
    for (;;) {
        clock_nanosleep(CLOCK_MONOTONIC, 0, &tick, NULL);
        int64_t ma = ns(CLOCK_MONOTONIC);
        int64_t r = ns(CLOCK_REALTIME);
        int64_t mb = ns(CLOCK_MONOTONIC);
        int64_t m = ma + (mb - ma) / 2;
        int64_t dm = m - prev_m, dr = r - prev_r, diff = dr - dm;
        samples++;
        if (dm > max_gap) max_gap = dm;
        int64_t tol = 500000 + dm / 10;
        if (dr < 0 || diff > tol || diff < -tol) {
            iso(r, when, sizeof(when));
            if (dr < 0) backward++;
            steps++;
            printf("%s %s d_realtime_us=%" PRId64 " d_monotonic_us=%" PRId64
                   " realtime_minus_monotonic_us=%" PRId64 "\n",
                   when, dr < 0 ? "BACKWARD" : "STEP", dr / 1000, dm / 1000, diff / 1000);
            fflush(stdout);
        }
        prev_m = m;
        prev_r = r;
        if (m >= end) break;
    }
    int64_t total = (prev_r - r0) - (prev_m - m0);
    iso(prev_r, when, sizeof(when));
    printf("%s SUMMARY seconds=%.1f samples=%" PRIu64 " events=%" PRIu64 " backward=%" PRIu64
           " drift_realtime_minus_monotonic_us=%" PRId64 " max_sample_gap_us=%" PRId64 "\n",
           when, (prev_m - m0) / 1e9, samples, steps, backward, total / 1000, max_gap / 1000);
    return 0;
}
