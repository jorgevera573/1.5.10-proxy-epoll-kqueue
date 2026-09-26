/*
 * main.c — punto de entrada.
 *
 *   proxy -c proxy.toml       ejecuta el maestro y sus workers
 *   proxy -t -c proxy.toml    valida la configuración sin abrir sockets
 *
 * Códigos de salida: 0 correcto; 1 configuración o arranque inválidos;
 * 2 uso incorrecto; 70 algún worker terminó con recursos prestados o por
 * señal durante el cierre.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <stdlib.h>

#include "config.h"
#include "diag.h"
#include "master.h"

static void usage(FILE *f) {
    (void)fprintf(f, "uso: proxy -c FICHERO.toml [-t]\n"
                     "  -c FICHERO  configuración TOML\n"
                     "  -t          solo validar la configuración (no abre sockets)\n"
                     "  -h          esta ayuda\n");
}

int main(int argc, char **argv) {
    const char *path = NULL;
    int check_only = 0;
    int opt;
    /* Antes de crear ningún hilo: getopt no es reentrante, aquí no importa. */
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    while ((opt = getopt(argc, argv, "c:th")) != -1) {
        switch (opt) {
        case 'c':
            path = optarg;
            break;
        case 't':
            check_only = 1;
            break;
        case 'h':
            usage(stdout);
            return 0;
        default:
            usage(stderr);
            return 2;
        }
    }
    if (path == NULL || optind != argc) {
        usage(stderr);
        return 2;
    }

    char err[CONFIG_ERR_LEN];
    size_t len = 0;
    char *text = config_read_text(path, &len, err, sizeof(err));
    struct config *cfg = text != NULL ? config_load_string(text, path, err, sizeof(err)) : NULL;
    if (cfg == NULL) {
        diag("proxy: configuración inválida: %s\n", err);
        free(text);
        return 1;
    }
    if (check_only) {
        (void)printf("%s: configuración válida (%zu frontends, %zu pools, %zu rutas%s, "
                     "%u workers)\n",
                     path, cfg->nfrontends, cfg->npools, cfg->nroutes,
                     cfg->has_default ? ", con ruta por defecto" : ", sin ruta por defecto",
                     config_effective_workers(cfg));
        config_unref(cfg);
        free(text);
        return 0;
    }
    /* master_run toma la propiedad de cfg y de text: así un worker (hijo del
     * maestro) puede soltar todo lo heredado antes de _exit. */
    return master_run(cfg, text, len, path);
}
