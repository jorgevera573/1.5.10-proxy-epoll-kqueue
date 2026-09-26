/*
 * http_forward.h — construcción de la cabecera de petición que se envía al
 * upstream a partir de una petición ya analizada por http_parser.
 *
 * Separado del parser: http_parser no modifica nada; este módulo solo lee la
 * cabecera original (buf + http_request) y escribe una nueva en `out`.
 *
 * Política (docs/architecture.md §6):
 *   - Línea de petición: método y versión originales; absolute-form se
 *     convierte en origin-form ("http://h/p?q" -> "/p?q"; vacío -> "/").
 *   - Host: valor original; en absolute-form, la autoridad del target.
 *     HTTP/1.0 sin Host: no se añade.
 *   - Se eliminan hop-by-hop: Connection, Keep-Alive, Proxy-Connection, TE,
 *     Upgrade y toda cabecera nombrada en Connection.
 *   - Cliente no confiable (por defecto): se eliminan X-Forwarded-For,
 *     X-Forwarded-Proto, X-Real-IP y Forwarded, y se generan con la IP del
 *     par y proto "http".
 *   - Par confiable (trusted_proxies): X-Forwarded-For recibido se conserva y
 *     se le añade la IP del par; X-Real-IP y X-Forwarded-Proto recibidos se
 *     conservan si existen. Forwarded se elimina siempre (no se genera).
 *   - Content-Length / Transfer-Encoding se conservan: el cuerpo se reenvía
 *     sin transformar.
 *   - Expect se elimina: el proxy responde "100 Continue" al cliente.
 *   - Siempre "Connection: close": primera versión sin reutilización de la
 *     conexión upstream (independiente del keep-alive del cliente).
 */
#ifndef HTTP_FORWARD_H
#define HTTP_FORWARD_H

#include <stdbool.h>
#include <stddef.h>

#include "http_parser.h"

struct http_forward_params {
    const char *client_ip; /* texto de inet_ntop; solo [0-9a-fA-F:.] */
    bool peer_trusted;
};

/*
 * Escribe la cabecera en out[0..cap). Devuelve 0 y *out_len, o -1 con errno:
 * EINVAL (client_ip vacío o con caracteres no permitidos), ENOBUFS (no cabe).
 */
int http_forward_build(const char *buf, const struct http_request *req,
                       const struct http_forward_params *params, char *out, size_t cap,
                       size_t *out_len);

/*
 * Cabecera de respuesta hacia el cliente a partir de la del upstream:
 * "HTTP/1.1 <código> <motivo>", sin hop-by-hop (Connection, Keep-Alive,
 * Proxy-Connection, TE, Upgrade) ni cabeceras nombradas en Connection;
 * Content-Length y Transfer-Encoding se conservan (el cuerpo se reenvía tal
 * cual). El proxy añade su propia decisión: "Connection: close" si no
 * mantendrá la conexión del cliente, o "Connection: keep-alive" para un
 * cliente HTTP/1.0 que sí la mantiene.
 * Devuelve 0 o -1 con errno = ENOBUFS.
 */
int http_forward_response(const char *buf, const struct http_response *resp, bool keep_alive,
                          int client_minor, char *out, size_t cap, size_t *out_len);

#endif /* HTTP_FORWARD_H */
