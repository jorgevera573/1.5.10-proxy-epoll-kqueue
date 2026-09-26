/*
 * stats.c — ver stats.h.
 */
#include "stats.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const struct stats_counter_def STATS_COUNTERS[] = {
    {"conns", false},          {"upstreams", false},
    {"slots_in_use", false},   {"slots_total", false},
    {"backend_active", false}, {"generations_live", false},
    {"backends_live", false},  {"draining", false},
    {"reload_running", false}, {"accepted", true},
    {"rejected", true},        {"accept_emfile", true},
    {"requests", true},        {"proxy_4xx", true},
    {"proxy_5xx", true},       {"upstream_reusable", true},
    {"reloads_ok", true},      {"reloads_failed", true},
    {"health_results", true},  {"health_stale", true},
    {"health_dropped", true},  {"log_written", true},
    {"log_dropped", true},     {"log_write_errors", true},
    {"log_truncated", true},
};
const size_t STATS_NCOUNTERS = sizeof(STATS_COUNTERS) / sizeof(STATS_COUNTERS[0]);

#define MAX_KV 64

/* ------------------------------------------------------------------------- */
/* Constructor de cadenas                                                    */
/* ------------------------------------------------------------------------- */

struct sb {
    char *p;
    size_t len;
    size_t cap;
    bool oom;
};

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
static void sbf(struct sb *b, const char *fmt, ...) {
    if (b->oom) {
        return;
    }
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(b->p ? b->p + b->len : NULL, b->p ? b->cap - b->len : 0, fmt, ap);
        va_end(ap);
        if (n < 0) {
            b->oom = true;
            return;
        }
        if (b->p != NULL && (size_t)n < b->cap - b->len) {
            b->len += (size_t)n;
            return;
        }
        size_t ncap = b->cap ? b->cap * 2 : 4096;
        while (ncap < b->len + (size_t)n + 1) {
            ncap *= 2;
        }
        char *np = realloc(b->p, ncap);
        if (np == NULL) {
            b->oom = true;
            return;
        }
        b->p = np;
        b->cap = ncap;
    }
}

/* Cadena JSON escapada. */
static void sb_str(struct sb *b, const char *s, size_t len) {
    sbf(b, "\"");
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            sbf(b, "\\%c", c);
        } else if (c < 0x20) {
            sbf(b, "\\u%04x", c);
        } else {
            sbf(b, "%c", c);
        }
    }
    sbf(b, "\"");
}

/* ------------------------------------------------------------------------- */
/* Interpretación del texto de un worker                                     */
/* ------------------------------------------------------------------------- */

struct kv {
    const char *k;
    size_t klen;
    const char *v;
    size_t vlen;
};

struct line {
    struct kv kv[MAX_KV];
    size_t n;
};

static void parse_line(const char *s, size_t len, struct line *out) {
    out->n = 0;
    size_t i = 0;
    while (i < len && out->n < MAX_KV) {
        while (i < len && s[i] == ' ') {
            i++;
        }
        size_t ks = i;
        while (i < len && s[i] != '=' && s[i] != ' ') {
            i++;
        }
        if (i >= len || s[i] != '=') {
            continue; /* palabra sin '=' (etiqueta de línea) */
        }
        size_t ke = i++;
        size_t vs = i;
        while (i < len && s[i] != ' ') {
            i++;
        }
        out->kv[out->n++] = (struct kv){s + ks, ke - ks, s + vs, i - vs};
    }
}

static const struct kv *find(const struct line *l, const char *key) {
    size_t kl = strlen(key);
    for (size_t i = 0; i < l->n; i++) {
        if (l->kv[i].klen == kl && memcmp(l->kv[i].k, key, kl) == 0) {
            return &l->kv[i];
        }
    }
    return NULL;
}

static unsigned long long num(const struct line *l, const char *key) {
    const struct kv *kv = find(l, key);
    unsigned long long v = 0;
    for (size_t i = 0; kv != NULL && i < kv->vlen; i++) {
        if (kv->v[i] < '0' || kv->v[i] > '9') {
            return 0;
        }
        v = v * 10 + (unsigned long long)(kv->v[i] - '0');
    }
    return v;
}

static void str_field(struct sb *b, const struct line *l, const char *key) {
    const struct kv *kv = find(l, key);
    if (kv != NULL) {
        sb_str(b, kv->v, kv->vlen);
    } else {
        sbf(b, "null");
    }
}

/* Recorre las líneas de un texto; cb(tipo, línea). */
typedef void (*line_cb)(void *ctx, bool is_worker, const struct line *l);

static void each_line(const char *text, size_t len, line_cb cb, void *ctx) {
    size_t i = 0;
    while (i < len) {
        size_t s = i;
        while (i < len && text[i] != '\n') {
            i++;
        }
        size_t e = i++;
        static const char W[] = "worker ";
        static const char B[] = "backend ";
        struct line l;
        if (e - s > sizeof(W) - 1 && memcmp(text + s, W, sizeof(W) - 1) == 0) {
            parse_line(text + s, e - s, &l);
            cb(ctx, true, &l);
        } else if (e - s > sizeof(B) - 1 && memcmp(text + s, B, sizeof(B) - 1) == 0) {
            parse_line(text + s, e - s, &l);
            cb(ctx, false, &l);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Agregación                                                                */
/* ------------------------------------------------------------------------- */

struct agg_backend {
    char key[256]; /* pool\taddr */
    char pool[80];
    char addr[80];
    char algo[24];
    unsigned long long active;
    unsigned long long selected;
    unsigned long long failures;
    unsigned up;
    unsigned down_active;
    unsigned down_passive;
};

struct agg {
    unsigned long long counters[64];
    unsigned long long gen_min;
    unsigned long long gen_max;
    unsigned responded;
    struct agg_backend *backends;
    size_t nbackends;
    size_t cap;
    bool oom;
};

static void copy_kv(char *dst, size_t cap, const struct line *l, const char *key) {
    const struct kv *kv = find(l, key);
    size_t n = kv ? (kv->vlen < cap - 1 ? kv->vlen : cap - 1) : 0;
    if (n > 0) {
        memcpy(dst, kv->v, n);
    }
    dst[n] = '\0';
}

static void agg_line(void *ctx, bool is_worker, const struct line *l) {
    struct agg *a = ctx;
    if (is_worker) {
        for (size_t i = 0; i < STATS_NCOUNTERS; i++) {
            a->counters[i] += num(l, STATS_COUNTERS[i].name);
        }
        unsigned long long g = num(l, "generation");
        if (a->responded == 0 || g < a->gen_min) {
            a->gen_min = g;
        }
        if (g > a->gen_max) {
            a->gen_max = g;
        }
        a->responded++;
        return;
    }
    char pool[80];
    char addr[80];
    copy_kv(pool, sizeof(pool), l, "pool");
    copy_kv(addr, sizeof(addr), l, "addr");
    char key[256];
    (void)snprintf(key, sizeof(key), "%s\t%s", pool, addr);
    struct agg_backend *e = NULL;
    for (size_t i = 0; i < a->nbackends; i++) {
        if (strcmp(a->backends[i].key, key) == 0) {
            e = &a->backends[i];
        }
    }
    if (e == NULL) {
        if (a->nbackends == a->cap) {
            size_t nc = a->cap ? a->cap * 2 : 16;
            struct agg_backend *nb = realloc(a->backends, nc * sizeof(*nb));
            if (nb == NULL) {
                a->oom = true;
                return;
            }
            a->backends = nb;
            a->cap = nc;
        }
        e = &a->backends[a->nbackends++];
        memset(e, 0, sizeof(*e));
        memcpy(e->key, key, sizeof(key));
        memcpy(e->pool, pool, sizeof(pool));
        memcpy(e->addr, addr, sizeof(addr));
        copy_kv(e->algo, sizeof(e->algo), l, "algo");
    }
    e->active += num(l, "active");
    e->selected += num(l, "selected");
    e->failures += num(l, "failures");
    const struct kv *h = find(l, "health");
    if (h != NULL && h->vlen == 2 && memcmp(h->v, "up", 2) == 0) {
        e->up++;
    } else if (h != NULL && h->vlen == 11 && memcmp(h->v, "down-active", 11) == 0) {
        e->down_active++;
    } else {
        e->down_passive++;
    }
}

/* ------------------------------------------------------------------------- */
/* Worker individual                                                         */
/* ------------------------------------------------------------------------- */

struct wctx {
    struct sb *b;
    bool first_backend;
};

static void worker_line(void *ctx, bool is_worker, const struct line *l) {
    struct wctx *w = ctx;
    struct sb *b = w->b;
    if (is_worker) {
        sbf(b, "\"generation\":%llu,\"uptime_ms\":%llu,\"counters\":{", num(l, "generation"),
            num(l, "uptime_ms"));
        for (size_t i = 0; i < STATS_NCOUNTERS; i++) {
            sbf(b, "%s\"%s\":%llu", i ? "," : "", STATS_COUNTERS[i].name,
                num(l, STATS_COUNTERS[i].name));
        }
        sbf(b, "},\"backends\":[");
        return;
    }
    sbf(b, "%s{\"pool\":", w->first_backend ? "" : ",");
    w->first_backend = false;
    str_field(b, l, "pool");
    sbf(b, ",\"addr\":");
    str_field(b, l, "addr");
    sbf(b, ",\"algorithm\":");
    str_field(b, l, "algo");
    sbf(b,
        ",\"weight\":%llu,\"max_conns\":%llu,\"active\":%llu,\"selected\":%llu,"
        "\"failures\":%llu,\"health\":",
        num(l, "weight"), num(l, "max_conns"), num(l, "active"), num(l, "selected"),
        num(l, "failures"));
    str_field(b, l, "health");
    sbf(b, "}");
}

/* Objeto "master": estado del maestro, de la recarga y de la recuperación. */
static void render_master(struct sb *b, const struct stats_master_info *m) {
    sbf(b,
        "\"master\":{\"pid\":%ld,\"uptime_ms\":%llu,\"generation\":%llu,"
        "\"workers_configured\":%u,\"workers_ready\":%u,\"degraded\":%s,\"listener_model\":\"%s\","
        "\"reload_in_progress\":%s,\"reload_state\":\"%s\","
        "\"recovery\":{\"state\":\"%s\",\"generation\":%llu},\"draining\":%s,"
        "\"reloads_ok\":%llu,\"reloads_failed\":%llu,\"worker_restarts\":%llu,"
        "\"stats_rejected\":%llu,\"stats_timeouts\":%llu},",
        m->pid, (unsigned long long)m->uptime_ms, (unsigned long long)m->generation,
        m->workers_configured, m->workers_ready,
        m->workers_ready < m->workers_configured ? "true" : "false",
        m->listener_model != NULL ? m->listener_model : "per_worker_reuseport",
        m->reload_in_progress ? "true" : "false",
        m->reload_state != NULL ? m->reload_state : "idle",
        m->recovery != NULL ? m->recovery : "none", (unsigned long long)m->recovery_generation,
        m->draining ? "true" : "false", (unsigned long long)m->reloads_ok,
        (unsigned long long)m->reloads_failed, (unsigned long long)m->worker_restarts,
        (unsigned long long)m->stats_rejected, (unsigned long long)m->stats_timeouts);
}

char *stats_render_json(const struct stats_master_info *m, const struct stats_worker_input *w,
                        size_t n, size_t *out_len) {
    struct sb b = {0};
    struct agg a = {0};
    size_t missing = 0;
    for (size_t i = 0; i < n; i++) {
        if (w[i].text != NULL) {
            each_line(w[i].text, w[i].len, agg_line, &a);
        } else {
            missing++;
        }
    }
    sbf(&b, "{\"schema\":1,\"complete\":%s,", missing == 0 ? "true" : "false");
    sbf(&b, "\"note\":\"Cada worker toma su instantanea en un momento distinto: el agregado no "
            "es una fotografia simultanea. Los acumulativos cuentan desde el arranque de cada "
            "proceso worker.\",");
    render_master(&b, m);
    sbf(&b, "\"counter_kinds\":{");
    for (size_t i = 0; i < STATS_NCOUNTERS; i++) {
        sbf(&b, "%s\"%s\":\"%s\"", i ? "," : "", STATS_COUNTERS[i].name,
            STATS_COUNTERS[i].cumulative ? "cumulative" : "instant");
    }
    sbf(&b, "},\"workers\":[");
    for (size_t i = 0; i < n; i++) {
        sbf(&b, "%s{\"index\":%d,\"pid\":%ld,\"state\":", i ? "," : "", w[i].index, w[i].pid);
        sb_str(&b, w[i].state, strlen(w[i].state));
        sbf(&b, ",\"restarts\":%u,\"responded\":%s", w[i].restarts,
            w[i].text != NULL ? "true" : "false");
        if (w[i].text != NULL) {
            sbf(&b, ",");
            struct wctx wc = {.b = &b, .first_backend = true};
            each_line(w[i].text, w[i].len, worker_line, &wc);
            sbf(&b, "]");
        }
        sbf(&b, "}");
    }
    sbf(&b, "],\"missing_workers\":[");
    bool first = true;
    for (size_t i = 0; i < n; i++) {
        if (w[i].text == NULL) {
            sbf(&b, "%s%d", first ? "" : ",", w[i].index);
            first = false;
        }
    }
    sbf(&b,
        "],\"totals\":{\"workers_responded\":%u,\"generation_min\":%llu,"
        "\"generation_max\":%llu,\"counters\":{",
        a.responded, a.gen_min, a.gen_max);
    for (size_t i = 0; i < STATS_NCOUNTERS; i++) {
        sbf(&b, "%s\"%s\":%llu", i ? "," : "", STATS_COUNTERS[i].name, a.counters[i]);
    }
    sbf(&b, "},\"backends\":[");
    for (size_t i = 0; i < a.nbackends; i++) {
        const struct agg_backend *e = &a.backends[i];
        unsigned total = e->up + e->down_active + e->down_passive;
        const char *summary = e->up == total ? "up" : (e->up == 0 ? "down" : "mixed");
        sbf(&b, "%s{\"pool\":", i ? "," : "");
        sb_str(&b, e->pool, strlen(e->pool));
        sbf(&b, ",\"addr\":");
        sb_str(&b, e->addr, strlen(e->addr));
        sbf(&b, ",\"algorithm\":");
        sb_str(&b, e->algo, strlen(e->algo));
        sbf(&b,
            ",\"active\":%llu,\"selected\":%llu,\"failures\":%llu,"
            "\"health_by_worker\":{\"up\":%u,\"down-active\":%u,\"down-passive\":%u},"
            "\"health_summary\":\"%s\"}",
            e->active, e->selected, e->failures, e->up, e->down_active, e->down_passive, summary);
    }
    sbf(&b, "]}}\n");
    free(a.backends);
    if (b.oom || a.oom) {
        free(b.p);
        return NULL;
    }
    *out_len = b.len;
    return b.p;
}
