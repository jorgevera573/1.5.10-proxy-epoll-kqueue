/*
 * test_buffer_pool.c — reserva, devolución, reutilización, agotamiento y
 * rechazo de devoluciones inválidas o duplicadas sin corromper la freelist.
 */
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>

#include "buffer_pool.h"

#define TEST_TIMEOUT_S 5
#define NSLOTS         8

static int setup(void **state) {
    alarm(TEST_TIMEOUT_S);
    buffer_pool *pool = buffer_pool_create(NSLOTS);
    if (pool == NULL) {
        return -1;
    }
    *state = pool;
    return 0;
}

static int teardown(void **state) {
    buffer_pool_destroy(*state);
    alarm(0);
    return 0;
}

static void test_create_validates_capacity(void **state) {
    (void)state;
    errno = 0;
    assert_null(buffer_pool_create(0));
    assert_int_equal(errno, EINVAL);
    errno = 0;
    assert_null(buffer_pool_create(BUFFER_POOL_MAX_SLOTS + 1));
    assert_int_equal(errno, EINVAL);

    buffer_pool *one = buffer_pool_create(1);
    assert_non_null(one);
    assert_int_equal(buffer_pool_capacity(one), 1);
    buffer_pool_destroy(one);
    buffer_pool_destroy(NULL);
}

static void test_acquire_distinct_writable_slots(void **state) {
    buffer_pool *pool = *state;
    unsigned char *slots[NSLOTS];

    assert_int_equal(buffer_pool_available(pool), NSLOTS);
    for (size_t i = 0; i < NSLOTS; i++) {
        slots[i] = buffer_pool_acquire(pool);
        assert_non_null(slots[i]);
        /* Escribe el slot entero con un patrón propio. */
        memset(slots[i], (int)(0xA0 + i), BUFFER_POOL_SLOT_SIZE);
    }
    assert_int_equal(buffer_pool_available(pool), 0);

    /* Sin solapamientos: cada slot conserva su patrón y la distancia entre
     * slots es múltiplo exacto de 16 KB. */
    for (size_t i = 0; i < NSLOTS; i++) {
        for (size_t k = 0; k < BUFFER_POOL_SLOT_SIZE; k += 1024) {
            assert_int_equal(slots[i][k], 0xA0 + i);
        }
        assert_int_equal(slots[i][BUFFER_POOL_SLOT_SIZE - 1], 0xA0 + i);
        uintptr_t d = (uintptr_t)slots[i] - (uintptr_t)slots[0];
        assert_int_equal(d % BUFFER_POOL_SLOT_SIZE, 0);
        for (size_t j = 0; j < i; j++) {
            assert_ptr_not_equal(slots[i], slots[j]);
        }
    }
    for (size_t i = 0; i < NSLOTS; i++) {
        assert_int_equal(buffer_pool_release(pool, slots[i]), 0);
    }
    assert_int_equal(buffer_pool_available(pool), NSLOTS);
}

static void test_exhaustion_and_recovery(void **state) {
    buffer_pool *pool = *state;
    void *slots[NSLOTS];
    for (size_t i = 0; i < NSLOTS; i++) {
        slots[i] = buffer_pool_acquire(pool);
        assert_non_null(slots[i]);
    }
    errno = 0;
    assert_null(buffer_pool_acquire(pool));
    assert_int_equal(errno, ENOBUFS);

    /* Una devolución permite exactamente una reserva más. */
    assert_int_equal(buffer_pool_release(pool, slots[3]), 0);
    void *again = buffer_pool_acquire(pool);
    assert_ptr_equal(again, slots[3]);
    assert_null(buffer_pool_acquire(pool));

    for (size_t i = 0; i < NSLOTS; i++) {
        assert_int_equal(buffer_pool_release(pool, slots[i]), 0);
    }
}

static void test_reuse_is_lifo(void **state) {
    buffer_pool *pool = *state;
    void *a = buffer_pool_acquire(pool);
    void *b = buffer_pool_acquire(pool);
    assert_int_equal(buffer_pool_release(pool, a), 0);
    assert_int_equal(buffer_pool_release(pool, b), 0);
    /* El último devuelto es el primero en volver a salir. */
    assert_ptr_equal(buffer_pool_acquire(pool), b);
    assert_ptr_equal(buffer_pool_acquire(pool), a);
    assert_int_equal(buffer_pool_release(pool, a), 0);
    assert_int_equal(buffer_pool_release(pool, b), 0);
}

static void test_double_release_is_rejected(void **state) {
    buffer_pool *pool = *state;
    void *a = buffer_pool_acquire(pool);
    assert_int_equal(buffer_pool_release(pool, a), 0);
    size_t avail = buffer_pool_available(pool);

    errno = 0;
    assert_int_equal(buffer_pool_release(pool, a), -1);
    assert_int_equal(errno, EALREADY);
    assert_int_equal(buffer_pool_available(pool), avail);

    /* La freelist sigue íntegra: NSLOTS reservas, todas distintas. */
    void *slots[NSLOTS];
    for (size_t i = 0; i < NSLOTS; i++) {
        slots[i] = buffer_pool_acquire(pool);
        assert_non_null(slots[i]);
        for (size_t j = 0; j < i; j++) {
            assert_ptr_not_equal(slots[i], slots[j]);
        }
    }
    assert_null(buffer_pool_acquire(pool));
    for (size_t i = 0; i < NSLOTS; i++) {
        assert_int_equal(buffer_pool_release(pool, slots[i]), 0);
    }
}

static void test_invalid_release_is_rejected(void **state) {
    buffer_pool *pool = *state;
    unsigned char *a = buffer_pool_acquire(pool);
    size_t avail = buffer_pool_available(pool);
    unsigned char outside[16];

    void *bad[] = {
        NULL,
        a + 1,                         /* dentro de un slot */
        a + BUFFER_POOL_SLOT_SIZE / 2, /* mitad de un slot */
        outside,                       /* otra memoria */
        /* más allá de la arena (aritmética entera: evita UB de punteros) */
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        (void *)((uintptr_t)a + (NSLOTS + 4) * BUFFER_POOL_SLOT_SIZE),
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        errno = 0;
        assert_int_equal(buffer_pool_release(pool, bad[i]), -1);
        assert_int_equal(errno, EINVAL);
        assert_int_equal(buffer_pool_available(pool), avail);
    }
    assert_int_equal(buffer_pool_release(pool, a), 0);
}

static void test_release_of_other_pool_slot_is_rejected(void **state) {
    buffer_pool *pool = *state;
    buffer_pool *other = buffer_pool_create(2);
    assert_non_null(other);
    void *foreign = buffer_pool_acquire(other);
    errno = 0;
    assert_int_equal(buffer_pool_release(pool, foreign), -1);
    assert_int_equal(errno, EINVAL);
    assert_int_equal(buffer_pool_available(pool), NSLOTS);
    assert_int_equal(buffer_pool_release(other, foreign), 0);
    buffer_pool_destroy(other);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_create_validates_capacity, setup, teardown),
        cmocka_unit_test_setup_teardown(test_acquire_distinct_writable_slots, setup, teardown),
        cmocka_unit_test_setup_teardown(test_exhaustion_and_recovery, setup, teardown),
        cmocka_unit_test_setup_teardown(test_reuse_is_lifo, setup, teardown),
        cmocka_unit_test_setup_teardown(test_double_release_is_rejected, setup, teardown),
        cmocka_unit_test_setup_teardown(test_invalid_release_is_rejected, setup, teardown),
        cmocka_unit_test_setup_teardown(test_release_of_other_pool_slot_is_rejected, setup,
                                        teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
