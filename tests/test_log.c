/*
 * test_log.c — formato, niveles, truncado, concurrencia, saturación sin
 * bloqueo y cierre con plazo ante un destino bloqueado.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>

#include "log.h"
#include "timer.h"

#define TEST_TIMEOUT_S 20

static int setup(void **state) {
    (void)state;
    alarm(TEST_TIMEOUT_S);
    return 0;
}

static int teardown(void **state) {
    (void)state;
    alarm(0);
    return 0;
}

static int temp_file(char *path, size_t len) {
    (void)snprintf(path, len, "/tmp/test_log_XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) {
        fail_msg("%s", "mkstemp");
    }
    return fd;
}

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fail_msg("%s", "fopen");
        return NULL;
    }
    char *buf = malloc(8u << 20);
    if (buf == NULL) {
        (void)fclose(f);
        fail_msg("%s", "malloc");
        return NULL;
    }
    *len = fread(buf, 1, (8u << 20) - 1, f);
    buf[*len] = '\0';
    (void)fclose(f);
    return buf;
}

static size_t count_lines(const char *buf, size_t len) {
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        n += buf[i] == '\n';
    }
    return n;
}

static void test_format_levels_and_sanitizing(void **state) {
    (void)state;
    char path[64];
    int fd = temp_file(path, sizeof(path));
    logger *lg = log_start(fd, true, LOG_INFO, 16, 3);
    assert_non_null(lg);
    log_msg(lg, LOG_DEBUG, 1, "no debe salir");
    log_msg(lg, LOG_INFO, 7, "hola %d", 1);
    log_msg(lg, LOG_WARN, 0, "a\nb\tc\r\n"); /* intento de inyectar líneas */
    assert_true(log_stop(lg, 2000));
    size_t len;
    char *buf = slurp(path, &len);
    assert_int_equal(count_lines(buf, len), 2);
    assert_non_null(strstr(buf, " worker=3 level=INFO gen=7 hola 1\n"));
    assert_non_null(strstr(buf, " level=WARN gen=- a?b?c\n"));
    assert_null(strstr(buf, "no debe salir"));
    assert_non_null(strstr(buf, " pid="));
    assert_true(buf[4] == '-' && buf[10] == 'T'); /* marca de tiempo ISO 8601 */
    free(buf);
    unlink(path);
}

static void test_truncation(void **state) {
    (void)state;
    char path[64];
    int fd = temp_file(path, sizeof(path));
    logger *lg = log_start(fd, true, LOG_INFO, 16, 0);
    char big[3000];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    log_msg(lg, LOG_INFO, 0, "%s", big);
    struct log_counters c;
    log_get_counters(lg, &c);
    assert_int_equal(c.truncated, 1);
    assert_true(log_stop(lg, 2000));
    size_t len;
    char *buf = slurp(path, &len);
    assert_int_equal(len, LOG_RECORD);
    assert_int_equal(count_lines(buf, len), 1);
    assert_memory_equal(buf + len - 12, " [truncado]\n", 12);
    free(buf);
    unlink(path);
}

struct prod {
    logger *lg;
    int id;
    int n;
};

static void *producer(void *arg) {
    struct prod *p = arg;
    for (int i = 0; i < p->n; i++) {
        log_msg(p->lg, LOG_INFO, 0, "hilo=%d n=%d fin", p->id, i);
    }
    return NULL;
}

static void test_concurrent_producers(void **state) {
    (void)state;
    enum { THREADS = 8, PER = 2000 };
    char path[64];
    int fd = temp_file(path, sizeof(path));
    logger *lg = log_start(fd, true, LOG_INFO, LOG_RING_CAP, 0);
    pthread_t th[THREADS];
    struct prod p[THREADS];
    for (int i = 0; i < THREADS; i++) {
        p[i] = (struct prod){lg, i, PER};
        assert_int_equal(pthread_create(&th[i], NULL, producer, &p[i]), 0);
    }
    for (int i = 0; i < THREADS; i++) {
        pthread_join(th[i], NULL);
    }
    struct log_counters before;
    log_get_counters(lg, &before);
    assert_true(log_stop(lg, 5000)); /* vacía lo pendiente */
    size_t len;
    char *buf = slurp(path, &len);
    size_t lines = count_lines(buf, len);
    /* Líneas de los productores (las demás son avisos de descarte). Cada
     * mensaje producido se escribió completo o se contó como descartado, y
     * no hay intercalado: cada línea es un registro completo. */
    size_t produced = 0;
    size_t notices = 0;
    for (char *l = buf; *l != '\0';) {
        char *nl = strchr(l, '\n');
        assert_non_null(nl);
        *nl = '\0';
        if (strstr(l, " level=INFO gen=- hilo=") != NULL) {
            assert_true(strcmp(l + strlen(l) - 4, " fin") == 0);
            produced++;
        } else {
            assert_non_null(strstr(l, " level=WARN gen=- log: "));
            assert_non_null(strstr(l, " mensajes descartados (ring lleno)"));
            notices++;
        }
        l = nl + 1;
    }
    assert_int_equal(produced + notices, lines);
    assert_int_equal(produced + before.dropped, (size_t)THREADS * PER);
    assert_true(before.dropped == 0 || notices > 0);
    free(buf);
    unlink(path);
}

static void test_saturation_never_blocks_and_stop_has_deadline(void **state) {
    (void)state;
    int p[2];
    assert_int_equal(pipe(p), 0);
    /* Destino que nunca se lee: se llena y el consumidor queda esperando. */
    assert_int_equal(fcntl(p[1], F_SETFL, O_NONBLOCK), 0);
    logger *lg = log_start(p[1], true, LOG_INFO, 256, 0);
    uint64_t t0 = timer_now_ms();
    for (int i = 0; i < 20000; i++) {
        log_msg(lg, LOG_INFO, 0, "mensaje de relleno %d con algo de texto para ocupar sitio", i);
    }
    uint64_t produce_ms = timer_now_ms() - t0;
    struct log_counters c;
    log_get_counters(lg, &c);
    assert_true(c.dropped > 0);     /* política acotada: se descarta */
    assert_true(produce_ms < 2000); /* el productor no espera a la escritura */
    t0 = timer_now_ms();
    bool freed = log_stop(lg, 300); /* plazo aunque el destino no avance */
    uint64_t stop_ms = timer_now_ms() - t0;
    assert_true(freed);
    assert_true(stop_ms < 1500);
    close(p[0]);
}

static void test_stop_drains_pending(void **state) {
    (void)state;
    char path[64];
    int fd = temp_file(path, sizeof(path));
    logger *lg = log_start(fd, true, LOG_INFO, LOG_RING_CAP, 0);
    for (int i = 0; i < 1000; i++) {
        log_msg(lg, LOG_INFO, 0, "linea %d", i);
    }
    assert_true(log_stop(lg, 5000));
    size_t len;
    char *buf = slurp(path, &len);
    assert_int_equal(count_lines(buf, len), 1000);
    free(buf);
    unlink(path);
}

static void test_installed_logger_and_sync_mode(void **state) {
    (void)state;
    char path[64];
    int fd = temp_file(path, sizeof(path));
    logger *lg = log_start(fd, true, LOG_INFO, 16, 5);
    log_install(lg);
    assert_ptr_equal(log_installed(), lg);
    plog(LOG_ERROR, 9, "via plog");
    assert_true(log_stop(lg, 2000));
    assert_null(log_installed()); /* log_stop lo desinstala */
    size_t len;
    char *buf = slurp(path, &len);
    assert_non_null(strstr(buf, "worker=5 level=ERROR gen=9 via plog\n"));
    free(buf);
    unlink(path);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_format_levels_and_sanitizing, setup, teardown),
        cmocka_unit_test_setup_teardown(test_truncation, setup, teardown),
        cmocka_unit_test_setup_teardown(test_concurrent_producers, setup, teardown),
        cmocka_unit_test_setup_teardown(test_saturation_never_blocks_and_stop_has_deadline, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_stop_drains_pending, setup, teardown),
        cmocka_unit_test_setup_teardown(test_installed_logger_and_sync_mode, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
