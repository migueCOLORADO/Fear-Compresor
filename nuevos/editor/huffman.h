#ifndef HUFFMAN_H
#define HUFFMAN_H

#include <stddef.h>
#include <stdint.h>

/**
 * ====================================================================================
 * COMPRESOR / DESCOMPRESOR HUFFMAN (módulo huf_) — NÚCLEO SECUENCIAL (Etapa 1)
 * ====================================================================================
 * Módulo INDEPENDIENTE: no depende de Editor ni de Lista. Opera sobre BYTES
 * (alfabeto de 256 símbolos), nunca sobre cadenas C, así que funciona con datos
 * binarios, con bytes 0x00 incrustados y con archivos vacíos.
 *
 * E/S SOLO con syscalls POSIX (open/pread/pwrite/read/write/lseek/ftruncate/
 * close/fstat), con manejo de EINTR y de E/S parciales en bucle. No se usa
 * fopen/fread/fwrite/fclose.
 *
 * Esta etapa es SECUENCIAL (un solo hilo): es la línea base de corrección y de
 * rendimiento. La API de bajo nivel (huf_contar/huf_construir/huf_codificar_
 * bloque/huf_decodificar_bloque) ya está pensada para llamarse desde varios
 * hilos en una etapa posterior: es reentrante, sin variables globales mutables,
 * con todo el estado pasado explícitamente.
 *
 * ------------------------------------------------------------------------------------
 * CÓDIGOS CANÓNICOS
 * ------------------------------------------------------------------------------------
 * El árbol se construye con un min-heap propio y desempate DETERMINISTA
 * (frecuencia y, a igualdad, símbolo menor primero), de modo que un mismo
 * histograma produce siempre las mismas longitudes de código. A partir de las
 * longitudes se asignan códigos CANÓNICOS, ordenando los símbolos por
 * (longitud, símbolo) y numerándolos consecutivamente. Gracias a esto el
 * encabezado solo necesita guardar 256 longitudes (1 byte c/u): el descompresor
 * reconstruye los mismos códigos con el mismo acuerdo.
 *
 * Longitud máxima de código soportada: 32 bits (los códigos se guardan en
 * uint32_t). Con 256 símbolos la longitud teórica máxima de un árbol de Huffman
 * degenerado (frecuencias tipo Fibonacci) es 255 bits, que NO cabe en 32. Para
 * acotarla se aplica un "length-limiting": tras construir el árbol, si alguna
 * longitud supera HUF_MAX_LONG (= 32) se recalcula limitando la profundidad con
 * el algoritmo de recorte de Kraft (ver huffman.c). Esto puede alejarse del
 * óptimo entrópico en entradas patológicas, pero garantiza que todo código cabe
 * en 32 bits y que la desigualdad de Kraft se mantiene (código decodificable).
 *
 * ------------------------------------------------------------------------------------
 * FORMATO .huf  (little-endian; ver también docs/FORMATO_HUF.md)
 * ------------------------------------------------------------------------------------
 *   offset  tamaño          campo
 *   0       4               magic "HUF1"
 *   4       8               tamaño original en bytes            (u64)
 *   12      4               tamaño de bloque                    (u32)
 *   16      4               número de bloques nbloques          (u32)
 *   20      256             longitudes de código por símbolo    (u8 x256; 0 = ausente)
 *   276     nbloques*32     TABLA DE BLOQUES (una entrada por bloque)
 *   ...     ...             flujos de bits de los bloques, concatenados
 *
 *   Cada entrada de la TABLA DE BLOQUES (32 bytes, little-endian):
 *     +0   u64  offset en el archivo .huf donde empieza el flujo del bloque
 *     +8   u64  longitud comprimida del bloque en BYTES (relleno a byte incluido)
 *     +16  u64  longitud original del bloque en BYTES
 *     +24  u32  CRC32 del bloque original (polinomio reflejado 0xEDB88320)
 *     +28  u32  número de bits útiles del flujo del bloque (nbits)
 *
 * La tabla permite localizar y verificar cada bloque sin leer los anteriores
 * (útil para la descompresión concurrente posterior). Como el tamaño comprimido
 * de un bloque solo se conoce tras codificarlo, al comprimir se RESERVA el
 * espacio de la tabla al inicio (ftruncate/seek) y se completa al final con
 * pwrite en los offsets correspondientes.
 *
 * RELLENO FINAL: cada bloque se escribe alineado a byte. El bit writer vuelca el
 * acumulador y, si quedan bits sueltos, emite un byte final con los bits bajos
 * usados y el resto en 0. El descompresor NO se guía por ese relleno: usa 'nbits'
 * de la tabla para saber exactamente cuántos bits leer, de modo que los bits de
 * relleno nunca se interpretan como símbolos.
 */

#define HUF_NSIMB       256        /* Alfabeto: un símbolo por valor de byte */
#define HUF_MAX_LONG    32         /* Longitud máxima de código (cabe en uint32_t) */
#define HUF_TAM_BLOQUE  (64 * 1024) /* Tamaño de bloque por defecto al comprimir */

/* Si es distinto de 0, el módulo traza sus syscalls estilo strace (como el
 * editor). El editor lo enlaza con su propia variable ed_traza. */
extern int huf_traza;

/**
 * ------------------------------------------------------------------------------------
 * API DE BAJO NIVEL (reentrante, sin estado global) — base para la concurrencia
 * ------------------------------------------------------------------------------------
 */

/* Tabla de códigos canónicos derivada de un histograma. 'longs[s]' es la
 * longitud en bits del código del símbolo s (0 si el símbolo no aparece) y
 * 'codigo[s]' su valor (bits significativos = longs[s], MSB primero). */
typedef struct {
    uint8_t  longs[HUF_NSIMB];
    uint32_t codigo[HUF_NSIMB];
} HufCodigos;

/* Acumula en hist[256] las frecuencias de los n bytes de 'in'. NO limpia hist:
 * el llamador la inicializa a 0 (así varios hilos pueden sumar histogramas
 * parciales sobre buffers separados y luego combinarlos). */
void huf_contar(const uint8_t *in, size_t n, uint64_t hist[HUF_NSIMB]);

/* Construye la tabla de códigos canónicos a partir del histograma. Retorna 0 en
 * éxito, -1 si falla malloc (mensaje ya reportado). Para histograma vacío deja
 * todas las longitudes en 0 y retorna 0. Reentrante: no toca estado global. */
int huf_construir(const uint64_t hist[HUF_NSIMB], HufCodigos *out);

/* Codifica in[0..n) en el buffer out (capacidad 'cap' bytes) usando los códigos
 * 'c'. Escribe en *nbits el número de bits útiles emitidos. Retorna 0 en éxito,
 * -1 si out no tiene capacidad suficiente (nunca desborda). El último byte queda
 * rellenado con ceros; el llamador persiste *nbits para la decodificación. */
int huf_codificar_bloque(const HufCodigos *c, const uint8_t *in, size_t n,
                          uint8_t *out, size_t cap, size_t *nbits);

/* Decodifica 'nbits' bits de in[] produciendo exactamente 'n_orig' bytes en out
 * (capacidad >= n_orig). Usa las longitudes 'longs' para reconstruir el mismo
 * árbol canónico que el compresor. Retorna 0 en éxito, -1 si el flujo es
 * inconsistente (código inválido o se agotan los bits antes de n_orig). */
int huf_decodificar_bloque(const uint8_t longs[HUF_NSIMB], const uint8_t *in,
                           size_t nbits, uint8_t *out, size_t n_orig);

/* CRC32 (polinomio reflejado 0xEDB88320, igual que zlib/PNG). 'crc' inicial debe
 * ser 0 para un cálculo nuevo; permite acumular por trozos. Implementación propia. */
uint32_t huf_crc32(uint32_t crc, const uint8_t *in, size_t n);

/**
 * ------------------------------------------------------------------------------------
 * API DE ARCHIVO SECUENCIAL (línea base de corrección y rendimiento)
 * ------------------------------------------------------------------------------------
 * Un solo hilo. Retornan 0 en éxito, -1 en error (ya reportado). No dejan fugas
 * ni descriptores abiertos en ninguna ruta. 'tam_bloque' 0 => HUF_TAM_BLOQUE.
 */
int huf_comprimir_archivo(const char *entrada, const char *salida, uint32_t tam_bloque);
int huf_descomprimir_archivo(const char *entrada, const char *salida);

/**
 * ------------------------------------------------------------------------------------
 * API DE ARCHIVO CONCURRENTE (etapas 2-4)
 * ------------------------------------------------------------------------------------
 * Usan un pool de 'nhilos' trabajadores (0 => automático según CPUs). La salida
 * va a '<salida>.tmp' y se renombra atómicamente con rename() solo si todo
 * terminó bien; ante error o cancelación se hace unlink del temporal, de modo
 * que la salida final nunca queda a medias.
 *
 * 'progreso' (puede ser NULL) recibe avisos de avance; 'cancelar' (puede ser
 * NULL) es una bandera que, si apunta a un entero distinto de 0, aborta el
 * trabajo de forma cooperativa entre bloques. Se usan en la integración con el
 * editor (segundo plano).
 */

/* Callback de progreso: (bloques_hechos, bloques_totales, dato_usuario). Se
 * llama desde el hilo que ensambla/decodifica; debe ser breve y no bloquear. */
typedef void (*HufProgreso)(uint32_t hechos, uint32_t total, void *dato);

typedef struct {
    int          nhilos;       /* 0 => automático */
    uint32_t     tam_bloque;   /* 0 => HUF_TAM_BLOQUE */
    HufProgreso  progreso;     /* NULL => sin avisos */
    void        *dato;         /* se pasa tal cual a 'progreso' */
    /* NULL o puntero a una bandera de cancelación. El motor la lee ENTRE bloques
     * desde varios hilos, de forma ATÓMICA (__atomic_load_n, orden relaxed): el
     * dueño la escribe también de forma atómica. No es 'volatile' porque volatile
     * no garantiza atomicidad entre hilos; la atomicidad la dan las primitivas
     * __atomic_*. */
    const int   *cancelar;
} HufOpc;

/* Comprime/descomprime en paralelo. Retornan 0 en éxito, -1 en error, 1 si se
 * canceló (en ambos casos la salida final no se crea / se deja intacta). */
int huf_comprimir_concurrente(const char *entrada, const char *salida, const HufOpc *opc);
int huf_descomprimir_concurrente(const char *entrada, const char *salida, const HufOpc *opc);

#endif /* HUFFMAN_H */
