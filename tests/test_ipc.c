/*
 * test_ipc.c — tramas IPC: ida y vuelta, cargas grandes, entrega
 * fragmentada, errores de protocolo y cierre.
 */
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cmocka.h>

#include "ipc.h"

#define TEST_TIMEOUT_S 20

struct got {
    size_t n;
    uint32_t type[16];
    uint64_t gen[16];
    uint64_t id[16];
    size_t len[16];
    char first[16][32];
    uint32_t big_sum;
};

static void on_msg(void *ctx, const struct ipc_msg *m) {
    struct got *g = ctx;
    if (g->n < 16) {
        g->type[g->n] = m->type;
        g->gen[g->n] = m->gen;
        g->id[g->n] = m->id;
        g->len[g->n] = m->len;
        size_t k = m->len < 31 ? m->len : 31;
        memcpy(g->first[g->n], m->data, k);
        g->first[g->n][k] = '\0';
        for (uint32_t i = 0; i < m->len; i++) {
            g->big_sum += (unsigned char)m->data[i];
        }
    }
    g->n++;
}

static int setup(void **state) {
    static int sv[2];
    alarm(TEST_TIMEOUT_S);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        return -1;
    }
    (void)fcntl(sv[0], F_SETFL, O_NONBLOCK);
    (void)fcntl(sv[1], F_SETFL, O_NONBLOCK);
    *state = sv;
    return 0;
}

static int teardown(void **state) {
    int *sv = *state;
    close(sv[0]);
    close(sv[1]);
    alarm(0);
    return 0;
}

static void test_roundtrip(void **state) {
    const int *sv = *state;
    struct ipc_chan a;
    struct ipc_chan b;
    ipc_init(&a, sv[0]);
    ipc_init(&b, sv[1]);
    assert_int_equal(ipc_queue(&a, IPC_READY, 1, 0, "123", 3), 0);
    assert_int_equal(ipc_queue(&a, IPC_STATS, 2, 99, "worker x=1\n", 11), 0);
    assert_int_equal(ipc_queue(&a, IPC_COMMIT, 7, 0, NULL, 0), 0);
    assert_int_equal(ipc_flush(&a), 0);
    assert_false(ipc_has_output(&a));
    struct got g = {0};
    assert_int_equal(ipc_read(&b, on_msg, &g), 0);
    assert_int_equal(g.n, 3);
    assert_int_equal(g.type[0], IPC_READY);
    assert_string_equal(g.first[0], "123");
    assert_int_equal(g.gen[1], 2);
    assert_int_equal(g.id[1], 99);
    assert_string_equal(g.first[1], "worker x=1\n");
    assert_int_equal(g.type[2], IPC_COMMIT);
    assert_int_equal(g.len[2], 0);
    ipc_free(&a);
    ipc_free(&b);
}

static void test_large_payload_nonblocking(void **state) {
    const int *sv = *state;
    struct ipc_chan a;
    struct ipc_chan b;
    ipc_init(&a, sv[0]);
    ipc_init(&b, sv[1]);
    size_t len = 5u << 20;
    char *data = malloc(len);
    assert_non_null(data);
    if (data == NULL) {
        return;
    }
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        data[i] = (char)(i * 31u);
        sum += (unsigned char)data[i];
    }
    assert_int_equal(ipc_queue(&a, IPC_PREPARE, 3, 0, data, len), 0);
    struct got g = {0};
    /* Envío y recepción alternados: el socket no admite 5 MiB de golpe. */
    int rounds = 0;
    while (g.n == 0 && rounds++ < 100000) {
        assert_true(ipc_flush(&a) >= 0);
        assert_int_equal(ipc_read(&b, on_msg, &g), 0);
    }
    assert_int_equal(g.n, 1);
    assert_int_equal(g.len[0], len);
    assert_int_equal(g.big_sum, sum);
    free(data);
    ipc_free(&a);
    ipc_free(&b);
}

static void test_fragmented_delivery(void **state) {
    int *sv = *state;
    struct ipc_chan a;
    struct ipc_chan b;
    ipc_init(&a, -1);
    ipc_init(&b, sv[1]);
    assert_int_equal(ipc_queue(&a, IPC_PREPARED_ERR, 4, 5, "motivo", 6), 0);
    struct got g = {0};
    for (size_t i = 0; i < a.out_len; i++) {
        assert_int_equal(write(sv[0], a.out + i, 1), 1);
        assert_int_equal(ipc_read(&b, on_msg, &g), 0);
        assert_int_equal(g.n, i + 1 == a.out_len ? 1 : 0); /* solo al completarse */
    }
    assert_string_equal(g.first[0], "motivo");
    ipc_free(&a);
    ipc_free(&b);
}

static void test_protocol_errors_and_eof(void **state) {
    int *sv = *state;
    struct ipc_chan b;
    ipc_init(&b, sv[1]);
    struct got g = {0};
    unsigned char bad[32] = {0};
    bad[0] = 'X'; /* magia incorrecta */
    assert_int_equal(write(sv[0], bad, sizeof(bad)), (ssize_t)sizeof(bad));
    assert_int_equal(ipc_read(&b, on_msg, &g), -1);
    ipc_free(&b);

    int sv2[2];
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, sv2), 0);
    (void)fcntl(sv2[1], F_SETFL, O_NONBLOCK);
    ipc_init(&b, sv2[1]);
    struct ipc_chan a;
    ipc_init(&a, -1);
    assert_int_equal(ipc_queue(&a, IPC_STATS, 0, 0, "x", 1), 0);
    /* Longitud excesiva en la cabecera. */
    a.out[8] = (char)0xff;
    a.out[9] = (char)0xff;
    a.out[10] = (char)0xff;
    a.out[11] = (char)0x7f;
    assert_int_equal(write(sv2[0], a.out, IPC_HEADER), (ssize_t)IPC_HEADER);
    assert_int_equal(ipc_read(&b, on_msg, &g), -1);
    ipc_free(&a);
    ipc_free(&b);
    close(sv2[0]);
    close(sv2[1]);

    /* EOF del otro extremo. */
    int sv3[2];
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, sv3), 0);
    (void)fcntl(sv3[1], F_SETFL, O_NONBLOCK);
    ipc_init(&b, sv3[1]);
    close(sv3[0]);
    assert_int_equal(ipc_read(&b, on_msg, &g), -1);
    ipc_free(&b);
    close(sv3[1]);

    errno = 0;
    ipc_init(&a, -1);
    assert_int_equal(ipc_queue(&a, IPC_STATS, 0, 0, NULL, IPC_MAX_PAYLOAD + 1), -1);
    assert_int_equal(errno, EMSGSIZE);
    ipc_free(&a);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_roundtrip, setup, teardown),
        cmocka_unit_test_setup_teardown(test_large_payload_nonblocking, setup, teardown),
        cmocka_unit_test_setup_teardown(test_fragmented_delivery, setup, teardown),
        cmocka_unit_test_setup_teardown(test_protocol_errors_and_eof, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
