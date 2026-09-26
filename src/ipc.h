/*
 * ipc.h — mensajes enmarcados entre el maestro y cada worker sobre un
 * socketpair(AF_UNIX, SOCK_STREAM) no bloqueante.
 *
 * Trama: cabecera de 32 bytes en little-endian
 *   magic u32 = 0x31595850 ("PXY1") | tipo u32 | longitud u32 | reservado u32
 *   generación u64 | id u64
 * seguida de `longitud` bytes de carga (<= IPC_MAX_PAYLOAD).
 * Una cabecera con magia incorrecta o longitud excesiva es un error de
 * protocolo: el canal se da por roto (el maestro repone el worker).
 *
 * Cada extremo mantiene buffers de entrada y salida propios; ipc_queue()
 * añade y ipc_flush() envía lo posible sin bloquear. Solo lo usa el hilo del
 * event loop de cada proceso.
 */
#ifndef IPC_H
#define IPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IPC_MAGIC       0x31595850u
#define IPC_HEADER      32u
#define IPC_MAX_PAYLOAD (8u << 20) /* configuración (<= 4 MiB) o estadísticas */

enum ipc_type {
    IPC_READY = 1,     /* W->M: listo con su generación */
    IPC_PREPARE,       /* M->W: configuración candidata (bytes del fichero) */
    IPC_PREPARED_OK,   /* W->M: candidata construida */
    IPC_PREPARED_ERR,  /* W->M: rechazada (texto con el motivo) */
    IPC_COMMIT,        /* M->W: activar la candidata */
    IPC_COMMITTED,     /* W->M: activada */
    IPC_COMMIT_FAILED, /* W->M: no había candidata para esa generación */
    IPC_ABORT,         /* M->W: descartar la candidata */
    IPC_STATS_REQ,     /* M->W: pide instantánea (id de petición) */
    IPC_STATS,         /* W->M: instantánea en texto clave=valor */
};

struct ipc_msg {
    uint32_t type;
    uint64_t gen;
    uint64_t id;
    const char *data; /* válido solo durante el callback */
    uint32_t len;
};

struct ipc_chan {
    int fd;
    char *in;
    size_t in_len;
    size_t in_cap;
    char *out;
    size_t out_off;
    size_t out_len;
    size_t out_cap;
};

void ipc_init(struct ipc_chan *ch, int fd);
/* Libera los buffers (no cierra el fd). */
void ipc_free(struct ipc_chan *ch);

/* Añade un mensaje a la salida. 0 o -1 (ENOMEM, EMSGSIZE). */
int ipc_queue(struct ipc_chan *ch, uint32_t type, uint64_t gen, uint64_t id, const void *data,
              size_t len);
/* Envía lo posible: 0 todo enviado, 1 queda pendiente (EAGAIN), -1 error. */
int ipc_flush(struct ipc_chan *ch);
bool ipc_has_output(const struct ipc_chan *ch);

typedef void (*ipc_handler)(void *ctx, const struct ipc_msg *msg);
/*
 * Lee hasta EAGAIN y entrega cada mensaje completo a `cb`. Devuelve 0, o -1
 * si el otro extremo cerró o hubo un error de E/S o de protocolo.
 */
int ipc_read(struct ipc_chan *ch, ipc_handler cb, void *ctx);

#endif /* IPC_H */
