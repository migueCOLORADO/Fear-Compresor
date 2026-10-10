#include "editor.h"
#include <stdio.h>
#include <string.h>

/**
 * Punto de entrada del editor como programa independiente.
 *
 * Uso: ./editor [-t] [archivo]
 *   -t       traza cada syscall sobre el archivo (estilo strace del shell)
 *   archivo  se abre al iniciar, equivalente a escribir 'o archivo'
 *
 * editor_run() nunca retorna: el comando 'q' (o EOF) hace close() y exit().
 */
int main(int argc, char **argv) {
    const char *ruta = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0) {
            ed_traza = 1;
        } else if (ruta == NULL) {
            ruta = argv[i];
        } else {
            printf("Uso: %s [-t] [archivo]\n", argv[0]);
            return 2;
        }
    }
    editor_run(ruta);
}
