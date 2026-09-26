/*
 * timer.c — ver timer.h. Heap binario de mínimos ordenado por (plazo, seq).
 */
#include "timer.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <time.h>

static bool before(const timer *a, const timer *b) {
    if (a->deadline_ms != b->deadline_ms) {
        return a->deadline_ms < b->deadline_ms;
    }
    return a->seq < b->seq;
}

static void place(timer_heap *h, size_t i, timer *t) {
    h->items[i] = t;
    t->heap_index = i;
}

static void sift_up(timer_heap *h, size_t i) {
    timer *t = h->items[i];
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (!before(t, h->items[parent])) {
            break;
        }
        place(h, i, h->items[parent]);
        i = parent;
    }
    place(h, i, t);
}

static void sift_down(timer_heap *h, size_t i) {
    timer *t = h->items[i];
    for (;;) {
        size_t child = 2 * i + 1;
        if (child >= h->len) {
            break;
        }
        if (child + 1 < h->len && before(h->items[child + 1], h->items[child])) {
            child++;
        }
        if (!before(h->items[child], t)) {
            break;
        }
        place(h, i, h->items[child]);
        i = child;
    }
    place(h, i, t);
}

static void heap_remove(timer_heap *h, const timer *t) {
    size_t i = t->heap_index;
    timer *last = h->items[--h->len];
    if (last != t) {
        place(h, i, last);
        /* El sustituto puede tener que subir o bajar. */
        if (i > 0 && before(last, h->items[(i - 1) / 2])) {
            sift_up(h, i);
        } else {
            sift_down(h, i);
        }
    }
}

static void expired_unlink(timer_heap *h, timer *t) {
    if (t->prev_expired != NULL) {
        t->prev_expired->next_expired = t->next_expired;
    } else {
        h->expired_head = t->next_expired;
    }
    if (t->next_expired != NULL) {
        t->next_expired->prev_expired = t->prev_expired;
    } else {
        h->expired_tail = t->prev_expired;
    }
    t->next_expired = NULL;
    t->prev_expired = NULL;
}

static void expired_push(timer_heap *h, timer *t) {
    t->prev_expired = h->expired_tail;
    t->next_expired = NULL;
    if (h->expired_tail != NULL) {
        h->expired_tail->next_expired = t;
    } else {
        h->expired_head = t;
    }
    h->expired_tail = t;
}

void timer_heap_init(timer_heap *heap) {
    *heap = (timer_heap){0};
}

void timer_heap_destroy(timer_heap *heap) {
    free((void *)heap->items);
    *heap = (timer_heap){0};
}

void timer_init(timer *t, timer_cb cb, void *userdata) {
    *t = (timer){.cb = cb, .userdata = userdata, .state = TIMER_IDLE};
}

void timer_cancel(timer_heap *heap, timer *t) {
    if (t->state == TIMER_QUEUED) {
        heap_remove(heap, t);
    } else if (t->state == TIMER_EXPIRED) {
        expired_unlink(heap, t);
    }
    t->state = TIMER_IDLE;
}

int timer_schedule_at(timer_heap *heap, timer *t, uint64_t deadline_ms) {
    timer_cancel(heap, t);
    if (heap->len == heap->cap) {
        size_t ncap = heap->cap ? heap->cap * 2 : 16;
        timer **items = (timer **)realloc((void *)heap->items, ncap * sizeof(*items));
        if (items == NULL) {
            errno = ENOMEM;
            return -1;
        }
        heap->items = items;
        heap->cap = ncap;
    }
    t->deadline_ms = deadline_ms;
    t->seq = heap->next_seq++;
    t->state = TIMER_QUEUED;
    place(heap, heap->len++, t);
    sift_up(heap, t->heap_index);
    return 0;
}

int timer_schedule_in(timer_heap *heap, timer *t, uint64_t now_ms, uint64_t delay_ms) {
    uint64_t deadline = delay_ms > UINT64_MAX - now_ms ? UINT64_MAX : now_ms + delay_ms;
    return timer_schedule_at(heap, t, deadline);
}

bool timer_is_active(const timer *t) {
    return t->state != TIMER_IDLE;
}

size_t timer_heap_size(const timer_heap *heap) {
    return heap->len;
}

int timer_heap_next_timeout(const timer_heap *heap, uint64_t now_ms) {
    if (heap->expired_head != NULL) {
        return 0;
    }
    if (heap->len == 0) {
        return -1;
    }
    uint64_t deadline = heap->items[0]->deadline_ms;
    if (deadline <= now_ms) {
        return 0;
    }
    uint64_t diff = deadline - now_ms;
    return diff > (uint64_t)INT_MAX ? INT_MAX : (int)diff;
}

size_t timer_heap_run_expired(timer_heap *heap, uint64_t now_ms) {
    /* Fase 1: mover los vencidos a la lista, en orden de disparo. */
    while (heap->len > 0 && heap->items[0]->deadline_ms <= now_ms) {
        timer *t = heap->items[0];
        heap_remove(heap, t);
        t->state = TIMER_EXPIRED;
        expired_push(heap, t);
    }
    /* Fase 2: disparar. Un callback puede sacar a otros de la lista. */
    size_t fired = 0;
    while (heap->expired_head != NULL) {
        timer *t = heap->expired_head;
        expired_unlink(heap, t);
        t->state = TIMER_IDLE;
        fired++;
        t->cb(heap, t, t->userdata);
    }
    return fired;
}

uint64_t timer_now_ms(void) {
    struct timespec ts;
    /* CLOCK_MONOTONIC existe en Linux y macOS; no puede fallar con él. */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

int timer_loop_run_once(io_loop *loop, timer_heap *heap) {
    int timeout = timer_heap_next_timeout(heap, timer_now_ms());
    int n = io_loop_run_once(loop, timeout);
    if (n < 0) {
        return -1;
    }
    size_t fired = timer_heap_run_expired(heap, timer_now_ms());
    return n + (int)fired;
}
