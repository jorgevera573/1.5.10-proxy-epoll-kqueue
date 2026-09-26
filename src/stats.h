/*
 * stats.h — instantáneas de los workers y documento JSON agregado.
 *
 * Cada worker responde a IPC_STATS_REQ con un texto de líneas "clave=valor":
 *   worker index=0 pid=… generation=… uptime_ms=… <contadores>
 *   backend pool=… addr=… algo=… weight=… max_conns=… active=… selected=…
 *           failures=… health=up|down-active|down-passive
 * El maestro lo interpreta y compone el JSON (stats_render_json).
 *
 * Tipos de contador (STATS_COUNTERS): "cumulative" crece desde el arranque
 * de ESE proceso worker; "instant" es el valor en el momento de su
 * instantánea. Al reponer un worker sus acumulativos vuelven a 0 (su pid,
 * uptime y "restarts" permiten detectarlo). Los totales suman una sola vez
 * la respuesta de cada worker a ESTA petición; los que no respondieron no
 * suman y aparecen en "missing_workers" con "complete": false. Las
 * instantáneas se toman en momentos distintos: el agregado no es una
 * fotografía simultánea.
 */
#ifndef STATS_H
#define STATS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct stats_counter_def {
    const char *name;
    bool cumulative;
};

extern const struct stats_counter_def STATS_COUNTERS[];
extern const size_t STATS_NCOUNTERS;

struct stats_master_info {
    long pid;
    uint64_t uptime_ms;
    uint64_t generation;
    uint32_t workers_configured;
    bool reload_in_progress;
    const char *reload_state; /* "idle", "preparing", "committing" */
    const char *recovery;     /* "none", "pending", "complete", "exhausted" */
    uint64_t recovery_generation;
    uint32_t workers_ready; /* slots en estado ready */
    bool draining;
    uint64_t reloads_ok;
    uint64_t reloads_failed;
    uint64_t worker_restarts;
    uint64_t stats_rejected;
    uint64_t stats_timeouts;
};

struct stats_worker_input {
    int index;
    long pid;          /* 0 si no hay proceso */
    const char *state; /* "starting", "ready", "restarting", "failed" */
    uint32_t restarts;
    const char *text; /* NULL si no respondió */
    size_t len;
};

/* JSON terminado en '\n' (malloc) o NULL sin memoria. */
char *stats_render_json(const struct stats_master_info *m, const struct stats_worker_input *w,
                        size_t n, size_t *out_len);

#endif /* STATS_H */
