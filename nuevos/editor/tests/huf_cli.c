/* =====================================================================================
 * Pequeño CLI de prueba del módulo huf_ (NO forma parte del editor ni del shell).
 * Lo usan tests/run_tests_huf.sh para ejercitar la API de archivo.
 *
 * Uso:
 *   huf_cli c <entrada> <salida> [tam_bloque]   comprime (secuencial, etapa 1)
 *   huf_cli x <entrada> <salida>                descomprime (secuencial)
 *   huf_cli crc <archivo>                       imprime el CRC32 del archivo
 * ===================================================================================== */
#include "../huffman.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int imprimir_crc(const char *ruta) {
    int fd = open(ruta, O_RDONLY);
    if (fd == -1) {
        perror("open");
        return 1;
    }
    uint32_t crc = 0;
    uint8_t buf[65536];
    for (;;) {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("read");
            close(fd);
            return 1;
        }
        if (r == 0) {
            break;
        }
        crc = huf_crc32(crc, buf, (size_t)r);
    }
    close(fd);
    printf("%08x\n", crc);
    return 0;
}

int main(int argc, char **argv) {
    if (getenv("HUF_TRAZA") != NULL) {
        huf_traza = 1;
    }
    /* Secuencial (etapa 1): c/x */
    if (argc >= 4 && strcmp(argv[1], "c") == 0) {
        uint32_t tb = argc >= 5 ? (uint32_t)strtoul(argv[4], NULL, 10) : 0;
        return huf_comprimir_archivo(argv[2], argv[3], tb) == 0 ? 0 : 1;
    }
    if (argc == 4 && strcmp(argv[1], "x") == 0) {
        return huf_descomprimir_archivo(argv[2], argv[3]) == 0 ? 0 : 1;
    }
    /* Concurrente (etapas 2-4): C <in> <out> [nhilos] [tam_bloque]
     *                           X <in> <out> [nhilos] */
    if (argc >= 4 && strcmp(argv[1], "C") == 0) {
        HufOpc opc; memset(&opc, 0, sizeof(opc));
        opc.nhilos = argc >= 5 ? (int)strtol(argv[4], NULL, 10) : 0;
        opc.tam_bloque = argc >= 6 ? (uint32_t)strtoul(argv[5], NULL, 10) : 0;
        int r = huf_comprimir_concurrente(argv[2], argv[3], &opc);
        return r == 0 ? 0 : (r == 1 ? 3 : 1);   /* 3 = cancelado */
    }
    if (argc >= 4 && strcmp(argv[1], "X") == 0) {
        HufOpc opc; memset(&opc, 0, sizeof(opc));
        opc.nhilos = argc >= 5 ? (int)strtol(argv[4], NULL, 10) : 0;
        int r = huf_descomprimir_concurrente(argv[2], argv[3], &opc);
        return r == 0 ? 0 : (r == 1 ? 3 : 1);
    }
    if (argc == 3 && strcmp(argv[1], "crc") == 0) {
        return imprimir_crc(argv[2]);
    }
    fprintf(stderr,
            "uso: %s c <in> <out> [tam_bloque]        (comprimir secuencial)\n"
            "     %s x <in> <out>                      (descomprimir secuencial)\n"
            "     %s C <in> <out> [nhilos] [tam_bloque](comprimir concurrente)\n"
            "     %s X <in> <out> [nhilos]             (descomprimir concurrente)\n"
            "     %s crc <archivo>\n",
            argv[0], argv[0], argv[0], argv[0], argv[0]);
    return 2;
}
