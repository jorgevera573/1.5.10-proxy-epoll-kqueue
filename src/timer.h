/*
 * timer.h — temporizadores de un solo disparo sobre un min-heap, en
 * milisegundos de reloj monotónico (CLOCK_MONOTONIC).
 *
 * Los nodos `timer` son intrusivos: los embebe el propietario (p. ej. la
 * conexión) y el heap solo guarda punteros; programar no reserva memoria salvo
 * al crecer el array del heap. Un timer debe cancelarse antes de liberar la
 * memoria que lo contiene.
 *
 * Todas las funciones reciben `now_ms` explícito: la lógica es determinista y
 * se prueba sin esperas reales. timer_now_ms() da el valor real.
 *
 * Ámbito: un timer_heap por event loop, usado solo desde su hilo.
 *
 * Orden: por plazo; a igual plazo, por orden de programación (FIFO).
 *
 * Disparo: timer_heap_run_expired extrae primero todos los vencidos a `now_ms`
 * a una lista y después invoca sus callbacks en orden. Un callback puede
 * cancelar o reprogramar cualquier timer, incluido él mismo:
 *   - cancelar un timer vencido que aún no ha disparado evita su disparo;
 *   - reprogramar (aunque sea con plazo <= now_ms) lo devuelve al heap y no
 *     dispara otra vez en la misma llamada, lo que impide bucles infinitos.
 */
#ifndef TIMER_H
#define TIMER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "io_event.h"

typedef struct timer_heap timer_heap;
typedef struct timer timer;
typedef void (*timer_cb)(timer_heap *heap, timer *t, void *userdata);

enum timer_state { TIMER_IDLE = 0, TIMER_QUEUED, TIMER_EXPIRED };

/* Campos internos: usar solo mediante la API. */
struct timer {
    uint64_t deadline_ms;
    uint64_t seq;
    size_t heap_index;
    timer *next_expired;
    timer *prev_expired;
    timer_cb cb;
    void *userdata;
    enum timer_state state;
};

struct timer_heap {
    timer **items;
    size_t len;
    size_t cap;
    uint64_t next_seq;
    timer *expired_head; /* lista de vencidos pendientes de disparar */
    timer *expired_tail;
};

void timer_heap_init(timer_heap *heap);
/* Libera el array. No toca los timers; quedan en estado indefinido. */
void timer_heap_destroy(timer_heap *heap);

void timer_init(timer *t, timer_cb cb, void *userdata);

/*
 * Programa (o reprograma, si ya estaba activo) para vencer en `deadline_ms`.
 * Devuelve 0 o -1 con errno = ENOMEM (el timer queda sin programar).
 */
int timer_schedule_at(timer_heap *heap, timer *t, uint64_t deadline_ms);
/* Igual, con plazo relativo `now_ms + delay_ms` (satura en UINT64_MAX). */
int timer_schedule_in(timer_heap *heap, timer *t, uint64_t now_ms, uint64_t delay_ms);

/* Cancela; idempotente (no hace nada si el timer no está activo). */
void timer_cancel(timer_heap *heap, timer *t);

/* true si está programado o vencido pendiente de disparar. */
bool timer_is_active(const timer *t);

size_t timer_heap_size(const timer_heap *heap);

/*
 * Timeout en ms para la espera del event loop: -1 sin timers, 0 si hay alguno
 * vencido y el tiempo restante en otro caso, saturado a INT_MAX.
 */
int timer_heap_next_timeout(const timer_heap *heap, uint64_t now_ms);

/* Dispara los timers con plazo <= now_ms. Devuelve cuántos dispararon. */
size_t timer_heap_run_expired(timer_heap *heap, uint64_t now_ms);

/* Milisegundos de CLOCK_MONOTONIC. */
uint64_t timer_now_ms(void);

/*
 * Una iteración del event loop con temporizadores: espera en `loop` como
 * máximo hasta el próximo vencimiento (sin límite si no hay timers),
 * despacha la E/S y dispara los timers vencidos. Devuelve el número de
 * callbacks de E/S más timers ejecutados, o -1 con errno si falla la espera.
 */
int timer_loop_run_once(io_loop *loop, timer_heap *heap);

#endif /* TIMER_H */
