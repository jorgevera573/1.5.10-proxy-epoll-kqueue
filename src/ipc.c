/*
 * ipc.c — ver ipc.h.
 */
#include "ipc.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

static void put32(unsigned char *p, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        p[i] = (unsigned char)(v >> (8 * i));
    }
}

static void put64(unsigned char *p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        p[i] = (unsigned char)(v >> (8 * i));
    }
}

static uint32_t get32(const unsigned char *p) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

static uint64_t get64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

void ipc_init(struct ipc_chan *ch, int fd) {
    memset(ch, 0, sizeof(*ch));
    ch->fd = fd;
}

void ipc_free(struct ipc_chan *ch) {
    free(ch->in);
    free(ch->out);
    ch->in = ch->out = NULL;
    ch->in_len = ch->in_cap = ch->out_off = ch->out_len = ch->out_cap = 0;
}

static int reserve(char **buf, size_t *cap, size_t need) {
    if (need <= *cap) {
        return 0;
    }
    size_t n = *cap ? *cap : 4096;
    while (n < need) {
        n *= 2;
    }
    char *b = realloc(*buf, n);
    if (b == NULL) {
        return -1;
    }
    *buf = b;
    *cap = n;
    return 0;
}

int ipc_queue(struct ipc_chan *ch, uint32_t type, uint64_t gen, uint64_t id, const void *data,
              size_t len) {
    if (len > IPC_MAX_PAYLOAD) {
        errno = EMSGSIZE;
        return -1;
    }
    /* Compactar lo ya enviado antes de crecer. */
    if (ch->out_off > 0) {
        memmove(ch->out, ch->out + ch->out_off, ch->out_len - ch->out_off);
        ch->out_len -= ch->out_off;
        ch->out_off = 0;
    }
    if (reserve(&ch->out, &ch->out_cap, ch->out_len + IPC_HEADER + len) < 0) {
        errno = ENOMEM;
        return -1;
    }
    unsigned char *h = (unsigned char *)ch->out + ch->out_len;
    put32(h, IPC_MAGIC);
    put32(h + 4, type);
    put32(h + 8, (uint32_t)len);
    put32(h + 12, 0);
    put64(h + 16, gen);
    put64(h + 24, id);
    if (len > 0) {
        memcpy(h + IPC_HEADER, data, len);
    }
    ch->out_len += IPC_HEADER + len;
    return 0;
}

bool ipc_has_output(const struct ipc_chan *ch) {
    return ch->out_len > ch->out_off;
}

int ipc_flush(struct ipc_chan *ch) {
    while (ch->out_off < ch->out_len) {
        ssize_t n = send(ch->fd, ch->out + ch->out_off, ch->out_len - ch->out_off, SEND_FLAGS);
        if (n > 0) {
            ch->out_off += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 1;
        } else {
            return -1;
        }
    }
    ch->out_off = ch->out_len = 0;
    return 0;
}

/* Entrega los mensajes completos del buffer; -1 si la cabecera es inválida. */
static int dispatch(struct ipc_chan *ch, ipc_handler cb, void *ctx) {
    size_t off = 0;
    while (ch->in_len - off >= IPC_HEADER) {
        const unsigned char *h = (const unsigned char *)ch->in + off;
        uint32_t len = get32(h + 8);
        if (get32(h) != IPC_MAGIC || len > IPC_MAX_PAYLOAD) {
            return -1;
        }
        if (ch->in_len - off < IPC_HEADER + (size_t)len) {
            break;
        }
        struct ipc_msg m = {.type = get32(h + 4),
                            .gen = get64(h + 16),
                            .id = get64(h + 24),
                            .data = ch->in + off + IPC_HEADER,
                            .len = len};
        off += IPC_HEADER + len;
        cb(ctx, &m);
    }
    if (off > 0) {
        memmove(ch->in, ch->in + off, ch->in_len - off);
        ch->in_len -= off;
    }
    return 0;
}

int ipc_read(struct ipc_chan *ch, ipc_handler cb, void *ctx) {
    for (;;) {
        if (reserve(&ch->in, &ch->in_cap, ch->in_len + 65536) < 0) {
            return -1;
        }
        ssize_t n = recv(ch->fd, ch->in + ch->in_len, ch->in_cap - ch->in_len, 0);
        if (n > 0) {
            ch->in_len += (size_t)n;
            if (dispatch(ch, cb, ctx) < 0) {
                return -1;
            }
            continue;
        }
        if (n == 0) {
            return -1; /* el otro extremo cerró */
        }
        if (errno == EINTR) {
            continue;
        }
        return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
    }
}
