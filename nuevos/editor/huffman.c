#include "huffman.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Colores ANSI (mismos valores que editor.h/shell.h). Se redeclaran aquí para
 * que el módulo no dependa de la cabecera del editor; coinciden en valor. */
#ifndef COLOR_RESET
#define COLOR_RESET     "\033[0m"
#define COLOR_SYSCALL   "\033[1;35m"
#define COLOR_RESULT    "\033[1;32m"
#define COLOR_ERROR     "\033[1;31m"
#endif

int huf_traza = 0;

/* Diagnóstico a stderr. Se usa vfprintf (no fprintf con cadena literal) a
 * propósito: GCC convierte fprintf(stderr,"literal") en una llamada a fwrite, y
 * una de las pruebas del editor verifica con nm -u que el binario NO importe
 * fopen/fread/fwrite/fclose. Centralizando aquí la salida se evita ese símbolo.
 * No es E/S de archivo (la del .huf va siempre por syscalls POSIX). */
#include <stdarg.h>
static void huf_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

#define HUF_MAGIA_STR   "HUF1"                        /* 4 bytes de identificación */
#define HUF_CAB_FIJA    (4 + 8 + 4 + 4 + HUF_NSIMB)  /* magic+orig+bloque+nbloques+longs = 276 */
#define HUF_ENTRADA_TAB 32                            /* bytes por entrada de la tabla de bloques */

/**
 * ====================================================================================
 * TRAZADO Y REPORTE DE ERRORES (estilo strace, como el editor)
 * ====================================================================================
 * Va por stderr: en la etapa concurrente el trabajo correrá en segundo plano y
 * escribir por stdout se mezclaría con el prompt del editor.
 */
#define HTRAZA(nombre, fmt, ...) \
    do { if (huf_traza) huf_err(COLOR_SYSCALL "[huf] " nombre COLOR_RESET "(" fmt ") ... ", ##__VA_ARGS__); } while (0)
#define HTRAZA_OK(r) \
    do { if (huf_traza) huf_err("= " COLOR_RESULT "%ld" COLOR_RESET "\n", (long)(r)); } while (0)

/* Reporta el fallo de una syscall conservando errno (igual estilo que el editor). */
static void hfallo(const char *ctx) {
    int err = errno;
    if (huf_traza) {
        huf_err("= " COLOR_ERROR "-1 (%s)" COLOR_RESET "\n", strerror(err));
    }
    errno = err;
    perror(ctx);
}

/**
 * ====================================================================================
 * ENVOLTORIOS DE SYSCALLS POSIX (offset explícito: aptos para hilos)
 * ====================================================================================
 * pread/pwrite para no depender del cursor del descriptor. Reintentan ante EINTR
 * y completan E/S parciales en bucle.
 */
static int sys_open2(const char *ruta, int flags, mode_t modo) {
    HTRAZA("open", "\"%s\", 0x%x, 0%o", ruta, (unsigned)flags, (unsigned)modo);
    int fd;
    do {
        fd = open(ruta, flags, modo);
    } while (fd == -1 && errno == EINTR);
    if (fd == -1) {
        hfallo("open");
        return -1;
    }
    HTRAZA_OK(fd);
    return fd;
}

static int sys_close(int fd) {
    HTRAZA("close", "%d", fd);
    if (close(fd) == -1) {
        hfallo("close");
        return -1;
    }
    HTRAZA_OK(0);
    return 0;
}

/* pread completo desde 'off'. Retorna 0 si leyó n bytes, 1 si encontró EOF antes
 * (último bloque corto; *leidos = cuántos), -1 en error. */
static int pread_todo(int fd, void *buf, size_t n, off_t off, size_t *leidos) {
    size_t hecho = 0;
    char *p = buf;
    while (hecho < n) {
        HTRAZA("pread", "%d, buf, %zu, %ld", fd, n - hecho, (long)(off + (off_t)hecho));
        ssize_t r = pread(fd, p + hecho, n - hecho, off + (off_t)hecho);
        if (r == -1) {
            if (errno == EINTR) {
                continue;
            }
            hfallo("pread");
            return -1;
        }
        HTRAZA_OK(r);
        if (r == 0) {
            *leidos = hecho;
            return 1;
        }
        hecho += (size_t)r;
    }
    *leidos = hecho;
    return 0;
}

/* pwrite completo en 'off'. 0 en éxito, -1 en error. */
static int pwrite_todo(int fd, const void *buf, size_t n, off_t off) {
    size_t hecho = 0;
    const char *p = buf;
    while (hecho < n) {
        HTRAZA("pwrite", "%d, buf, %zu, %ld", fd, n - hecho, (long)(off + (off_t)hecho));
        ssize_t w = pwrite(fd, p + hecho, n - hecho, off + (off_t)hecho);
        if (w == -1) {
            if (errno == EINTR) {
                continue;
            }
            hfallo("pwrite");
            return -1;
        }
        HTRAZA_OK(w);
        hecho += (size_t)w;
    }
    return 0;
}

static off_t sys_tamano(int fd) {
    struct stat st;
    HTRAZA("fstat", "%d, &st", fd);
    if (fstat(fd, &st) == -1) {
        hfallo("fstat");
        return -1;
    }
    HTRAZA_OK(st.st_size);
    return st.st_size;
}

static int sys_ftruncate(int fd, off_t largo) {
    HTRAZA("ftruncate", "%d, %ld", fd, (long)largo);
    if (ftruncate(fd, largo) == -1) {
        hfallo("ftruncate");
        return -1;
    }
    HTRAZA_OK(0);
    return 0;
}

/* Serialización little-endian independiente de la máquina. */
static void le_poner32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}
static void le_poner64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}
static uint32_t le_leer32(const uint8_t *p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        v |= (uint32_t)p[i] << (8 * i);
    }
    return v;
}
static uint64_t le_leer64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (uint64_t)p[i] << (8 * i);
    }
    return v;
}

/**
 * ====================================================================================
 * CRC32 (implementación propia, polinomio reflejado 0xEDB88320)
 * ====================================================================================
 * Mismo CRC que zlib/PNG. Se calcula bit a bit (sin tabla) para que la
 * implementación sea evidentemente propia y verificable. 'crc' inicial = 0.
 */
uint32_t huf_crc32(uint32_t crc, const uint8_t *in, size_t n) {
    crc = ~crc;                         /* Pre-condicionamiento estándar */
    for (size_t i = 0; i < n; i++) {
        crc ^= in[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mascara = -(crc & 1u);          /* 0xFFFFFFFF si el bit bajo es 1 */
            crc = (crc >> 1) ^ (0xEDB88320u & mascara);
        }
    }
    return ~crc;                        /* Post-condicionamiento estándar */
}

/**
 * ====================================================================================
 * API DE BAJO NIVEL: HISTOGRAMA
 * ====================================================================================
 */
void huf_contar(const uint8_t *in, size_t n, uint64_t hist[HUF_NSIMB]) {
    for (size_t i = 0; i < n; i++) {
        hist[in[i]]++;
    }
}

/**
 * ====================================================================================
 * NÚCLEO HUFFMAN: min-heap propio, árbol, longitudes y códigos canónicos
 * ====================================================================================
 */

/* Nodo del árbol (solo durante la construcción). El campo 'simbolo_min' guarda
 * el menor símbolo del subárbol: sirve de desempate DETERMINISTA cuando dos
 * nodos tienen la misma frecuencia, para que un histograma dado produzca siempre
 * exactamente las mismas longitudes (independiente del orden de inserción). */
typedef struct NodoHuf {
    uint64_t freq;
    int simbolo;              /* 0..255 en hojas, -1 en internos */
    int simbolo_min;          /* menor símbolo del subárbol (desempate) */
    struct NodoHuf *izq, *der;
} NodoHuf;

/* ¿Es 'a' estrictamente "menor" que 'b' para el heap? Orden: menor frecuencia
 * primero y, a igualdad, menor simbolo_min primero (desempate determinista). */
static int nodo_menor(const NodoHuf *a, const NodoHuf *b) {
    if (a->freq != b->freq) {
        return a->freq < b->freq;
    }
    return a->simbolo_min < b->simbolo_min;
}

typedef struct {
    NodoHuf **v;
    size_t n, cap;
} MinHeap;

static int heap_init(MinHeap *h, size_t cap) {
    h->v = malloc(cap * sizeof(NodoHuf *));
    if (h->v == NULL) {
        perror("malloc");
        return -1;
    }
    h->n = 0;
    h->cap = cap;
    return 0;
}

static void heap_push(MinHeap *h, NodoHuf *x) {
    size_t i = h->n++;
    h->v[i] = x;
    while (i > 0) {
        size_t padre = (i - 1) / 2;
        if (!nodo_menor(h->v[i], h->v[padre])) {
            break;
        }
        NodoHuf *t = h->v[padre]; h->v[padre] = h->v[i]; h->v[i] = t;
        i = padre;
    }
}

static NodoHuf *heap_pop(MinHeap *h) {
    NodoHuf *top = h->v[0];
    h->v[0] = h->v[--h->n];
    size_t i = 0;
    while (1) {
        size_t izq = 2 * i + 1, der = 2 * i + 2, min = i;
        if (izq < h->n && nodo_menor(h->v[izq], h->v[min])) min = izq;
        if (der < h->n && nodo_menor(h->v[der], h->v[min])) min = der;
        if (min == i) {
            break;
        }
        NodoHuf *t = h->v[min]; h->v[min] = h->v[i]; h->v[i] = t;
        i = min;
    }
    return top;
}

static void arbol_liberar(NodoHuf *n) {
    if (n == NULL) {
        return;
    }
    arbol_liberar(n->izq);
    arbol_liberar(n->der);
    free(n);
}

/* Asigna longitudes (profundidad) a cada símbolo recorriendo el árbol. Un árbol
 * de una sola hoja produce profundidad 0; se corrige a 1 fuera de aquí. */
static void recorrer_longitudes(const NodoHuf *n, int prof, uint8_t longs[HUF_NSIMB]) {
    if (n->simbolo >= 0) {
        longs[n->simbolo] = (uint8_t)(prof > 255 ? 255 : prof);
        return;
    }
    recorrer_longitudes(n->izq, prof + 1, longs);
    recorrer_longitudes(n->der, prof + 1, longs);
}

/**
 * Length-limiting por el método de Kraft: si alguna longitud supera HUF_MAX_LONG,
 * se recorta la profundidad del árbol a HUF_MAX_LONG y se repara la desigualdad
 * de Kraft (sum 2^-len <= 1) subiendo símbolos de niveles poco profundos. Esto
 * mantiene el código decodificable (prefijo) aunque deje de ser óptimo en
 * entradas patológicas (p.ej. frecuencias de Fibonacci, que sin recorte darían
 * longitudes de hasta 255 bits). Trabaja sobre el arreglo de longitudes 'longs'
 * con 'nsimb' símbolos presentes.
 */
static void limitar_longitudes(uint8_t longs[HUF_NSIMB]) {
    int excede = 0;
    for (int s = 0; s < HUF_NSIMB; s++) {
        if (longs[s] > HUF_MAX_LONG) {
            excede = 1;
            break;
        }
    }
    if (!excede) {
        return;
    }
    /* 1) Capar todas las longitudes a HUF_MAX_LONG. */
    for (int s = 0; s < HUF_NSIMB; s++) {
        if (longs[s] > HUF_MAX_LONG) {
            longs[s] = HUF_MAX_LONG;
        }
    }
    /* 2) Reparar Kraft. Suma de Kraft en unidades de 2^-HUF_MAX_LONG. */
    uint64_t total = (uint64_t)1 << HUF_MAX_LONG;   /* "1.0" en esas unidades */
    uint64_t suma = 0;
    for (int s = 0; s < HUF_NSIMB; s++) {
        if (longs[s] > 0) {
            suma += (uint64_t)1 << (HUF_MAX_LONG - longs[s]);
        }
    }
    /* Mientras la suma supere 1, alargar el código más corto disponible (sube la
     * profundidad de un símbolo poco profundo, reduciendo su aporte a Kraft). */
    while (suma > total) {
        int mejor = -1;
        for (int s = 0; s < HUF_NSIMB; s++) {
            if (longs[s] > 0 && longs[s] < HUF_MAX_LONG) {
                if (mejor == -1 || longs[s] < longs[mejor]) {
                    mejor = s;
                }
            }
        }
        if (mejor == -1) {
            break;   /* No debería ocurrir con <=256 símbolos y 32 bits */
        }
        suma -= (uint64_t)1 << (HUF_MAX_LONG - longs[mejor]);
        longs[mejor]++;
        suma += (uint64_t)1 << (HUF_MAX_LONG - longs[mejor]);
    }
}

/* Códigos canónicos a partir de las longitudes: por longitud creciente y, a
 * igualdad, por símbolo creciente, se numeran consecutivamente (desplazando al
 * cambiar de longitud). El descompresor usa el mismo acuerdo. */
static void construir_codigos_canonicos(const uint8_t longs[HUF_NSIMB], uint32_t codigo[HUF_NSIMB]) {
    uint32_t cod = 0;
    for (int s = 0; s < HUF_NSIMB; s++) {
        codigo[s] = 0;
    }
    for (int L = 1; L <= HUF_MAX_LONG; L++) {
        for (int s = 0; s < HUF_NSIMB; s++) {
            if (longs[s] == L) {
                codigo[s] = cod++;
            }
        }
        cod <<= 1;
    }
}

int huf_construir(const uint64_t hist[HUF_NSIMB], HufCodigos *out) {
    memset(out->longs, 0, sizeof(out->longs));
    memset(out->codigo, 0, sizeof(out->codigo));

    MinHeap h;
    if (heap_init(&h, HUF_NSIMB) == -1) {
        return -1;
    }
    for (int s = 0; s < HUF_NSIMB; s++) {
        if (hist[s] > 0) {
            NodoHuf *n = malloc(sizeof(NodoHuf));
            if (n == NULL) {
                perror("malloc");
                for (size_t i = 0; i < h.n; i++) {
                    arbol_liberar(h.v[i]);
                }
                free(h.v);
                return -1;
            }
            n->freq = hist[s];
            n->simbolo = s;
            n->simbolo_min = s;
            n->izq = n->der = NULL;
            heap_push(&h, n);
        }
    }
    if (h.n == 0) {           /* Archivo vacío: nada que codificar */
        free(h.v);
        return 0;
    }
    if (h.n == 1) {           /* Un único símbolo distinto: longitud 1 (nunca 0) */
        NodoHuf *unico = heap_pop(&h);
        out->longs[unico->simbolo] = 1;
        arbol_liberar(unico);
        free(h.v);
        construir_codigos_canonicos(out->longs, out->codigo);
        return 0;
    }
    while (h.n > 1) {
        NodoHuf *a = heap_pop(&h);
        NodoHuf *b = heap_pop(&h);
        NodoHuf *p = malloc(sizeof(NodoHuf));
        if (p == NULL) {
            perror("malloc");
            arbol_liberar(a);
            arbol_liberar(b);
            for (size_t i = 0; i < h.n; i++) {
                arbol_liberar(h.v[i]);
            }
            free(h.v);
            return -1;
        }
        p->freq = a->freq + b->freq;
        p->simbolo = -1;
        p->simbolo_min = a->simbolo_min < b->simbolo_min ? a->simbolo_min : b->simbolo_min;
        p->izq = a;
        p->der = b;
        heap_push(&h, p);
    }
    NodoHuf *raiz = heap_pop(&h);
    free(h.v);
    recorrer_longitudes(raiz, 0, out->longs);
    arbol_liberar(raiz);

    limitar_longitudes(out->longs);
    construir_codigos_canonicos(out->longs, out->codigo);
    return 0;
}

/**
 * ====================================================================================
 * BIT WRITER / BIT READER (acumulador de 64 bits)
 * ====================================================================================
 * El writer acumula bits en 'acc' (MSB primero dentro de cada código) y vuelca
 * bytes completos a 'out'. Al terminar, 'bw_terminar' emite el último byte
 * parcial (bits bajos en 0: relleno). El número EXACTO de bits útiles se guarda
 * aparte (nbits) para que el reader no confunda el relleno con símbolos.
 */
typedef struct {
    uint8_t *out;
    size_t   cap;
    size_t   pos;        /* byte actual en out */
    uint64_t acc;        /* acumulador: bits pendientes alineados a la izquierda */
    int      llenos;     /* bits válidos actualmente en acc (desde el MSB) */
    size_t   nbits;      /* total de bits emitidos */
    int      desborde;   /* 1 si se intentó escribir más allá de cap */
} BitWriter;

static void bw_init(BitWriter *w, uint8_t *out, size_t cap) {
    w->out = out;
    w->cap = cap;
    w->pos = 0;
    w->acc = 0;
    w->llenos = 0;
    w->nbits = 0;
    w->desborde = 0;
}

/* Vuelca a 'out' todos los bytes completos disponibles en el acumulador. */
static void bw_volcar(BitWriter *w) {
    while (w->llenos >= 8) {
        if (w->pos >= w->cap) {
            w->desborde = 1;
            return;
        }
        w->out[w->pos++] = (uint8_t)(w->acc >> 56);  /* 8 bits más altos */
        w->acc <<= 8;
        w->llenos -= 8;
    }
}

/* Escribe los 'nb' bits bajos de 'cod' (MSB primero). nb en 1..32. */
static void bw_poner(BitWriter *w, uint32_t cod, int nb) {
    /* Alinear los nb bits al tope del hueco libre del acumulador. Como llenos
     * nunca supera 7 tras bw_volcar y nb<=32, caben de sobra en 64 bits. */
    w->acc |= ((uint64_t)(cod & (nb == 32 ? 0xFFFFFFFFu : ((1u << nb) - 1)))) << (64 - w->llenos - nb);
    w->llenos += nb;
    w->nbits += (size_t)nb;
    bw_volcar(w);
}

/* Cierra el flujo: emite el byte parcial final (relleno con ceros) si queda. */
static void bw_terminar(BitWriter *w) {
    if (w->llenos > 0) {
        if (w->pos >= w->cap) {
            w->desborde = 1;
            return;
        }
        w->out[w->pos++] = (uint8_t)(w->acc >> 56);
        w->acc = 0;
        w->llenos = 0;
    }
}

int huf_codificar_bloque(const HufCodigos *c, const uint8_t *in, size_t n,
                         uint8_t *out, size_t cap, size_t *nbits) {
    BitWriter w;
    bw_init(&w, out, cap);
    for (size_t i = 0; i < n; i++) {
        uint8_t s = in[i];
        int L = c->longs[s];
        /* Un símbolo presente en el bloque siempre tiene longitud > 0 porque la
         * tabla se construyó desde el histograma de ESTE conjunto de datos. */
        bw_poner(&w, c->codigo[s], L);
        if (w.desborde) {
            return -1;
        }
    }
    bw_terminar(&w);
    if (w.desborde) {
        return -1;
    }
    *nbits = w.nbits;
    return 0;
}

/**
 * ------------------------------------------------------------------------------------
 * BIT READER + decodificación con tabla canónica
 * ------------------------------------------------------------------------------------
 * Se decodifica por el método canónico sin reconstruir el árbol de punteros:
 * para cada longitud L se conoce el primer código y el primer índice; al leer
 * bits se compara el código acumulado contra los límites por longitud. Esto es
 * O(L) por símbolo y no asigna memoria dinámica (apto para hilos).
 */
int huf_decodificar_bloque(const uint8_t longs[HUF_NSIMB], const uint8_t *in,
                           size_t nbits, uint8_t *out, size_t n_orig) {
    if (n_orig == 0) {
        return 0;
    }
    /* Preparar tablas canónicas por longitud (igual acuerdo que el compresor). */
    int cuenta[HUF_MAX_LONG + 1];
    uint32_t primer_cod[HUF_MAX_LONG + 1];   /* primer código de cada longitud */
    int primer_idx[HUF_MAX_LONG + 1];        /* índice en 'orden' del primero */
    uint8_t orden[HUF_NSIMB];                /* símbolos por (longitud, símbolo) */

    memset(cuenta, 0, sizeof(cuenta));
    for (int s = 0; s < HUF_NSIMB; s++) {
        if (longs[s] > 0) {
            cuenta[longs[s]]++;
        }
    }
    int idx = 0;
    uint32_t cod = 0;
    for (int L = 1; L <= HUF_MAX_LONG; L++) {
        primer_cod[L] = cod;
        primer_idx[L] = idx;
        for (int s = 0; s < HUF_NSIMB; s++) {
            if (longs[s] == L) {
                orden[idx++] = (uint8_t)s;
            }
        }
        cod = (cod + (uint32_t)cuenta[L]) << 1;
    }

    size_t escritos = 0;
    size_t bit = 0;
    uint32_t acc = 0;
    int acclen = 0;
    while (escritos < n_orig) {
        /* Avanzar un bit y probar si el código acumulado cierra en alguna longitud. */
        if (bit >= nbits) {
            huf_err(COLOR_ERROR "huf: flujo agotado antes de decodificar todo" COLOR_RESET "\n");
            return -1;
        }
        int byte = in[bit >> 3];
        int b = (byte >> (7 - (bit & 7))) & 1;
        bit++;
        acc = (acc << 1) | (uint32_t)b;
        acclen++;
        if (acclen > HUF_MAX_LONG) {
            huf_err(COLOR_ERROR "huf: código inválido (excede longitud máxima)" COLOR_RESET "\n");
            return -1;
        }
        int L = acclen;
        if (cuenta[L] > 0) {
            uint32_t desplazado = acc - primer_cod[L];
            if (desplazado < (uint32_t)cuenta[L]) {
                out[escritos++] = orden[primer_idx[L] + (int)desplazado];
                acc = 0;
                acclen = 0;
            }
        }
    }
    return 0;
}

/**
 * ====================================================================================
 * API DE ARCHIVO SECUENCIAL (línea base)
 * ====================================================================================
 */

/* Lee todo el archivo abierto en 'fd' (tamaño 'tam') a un buffer malloc'd.
 * Retorna el buffer (que el llamador libera) o NULL en error. tam==0 => buffer
 * de 1 byte (no NULL) para simplificar el llamador. */
static uint8_t *leer_todo(int fd, off_t tam) {
    uint8_t *buf = malloc(tam > 0 ? (size_t)tam : 1);
    if (buf == NULL) {
        perror("malloc");
        return NULL;
    }
    if (tam > 0) {
        size_t leidos;
        int r = pread_todo(fd, buf, (size_t)tam, 0, &leidos);
        if (r == -1 || (r == 1 && leidos != (size_t)tam)) {
            if (r == 1) {
                huf_err(COLOR_ERROR "huf: el archivo encogió durante la lectura" COLOR_RESET "\n");
            }
            free(buf);
            return NULL;
        }
    }
    return buf;
}

int huf_comprimir_archivo(const char *entrada, const char *salida, uint32_t tam_bloque) {
    if (tam_bloque == 0) {
        tam_bloque = HUF_TAM_BLOQUE;
    }
    int fde = -1, fds = -1, rc = -1;
    uint8_t *datos = NULL, *comp = NULL, *tabla = NULL;
    uint8_t cab[HUF_CAB_FIJA];

    fde = sys_open2(entrada, O_RDONLY, 0);
    if (fde == -1) {
        return -1;
    }
    off_t tam = sys_tamano(fde);
    if (tam == -1) {
        goto fin;
    }
    datos = leer_todo(fde, tam);
    if (datos == NULL) {
        goto fin;
    }

    uint64_t total = (uint64_t)tam;
    uint32_t nbloques = total == 0 ? 0 : (uint32_t)((total + tam_bloque - 1) / tam_bloque);

    /* Histograma global sobre todo el archivo (en la etapa concurrente serán
     * histogramas parciales combinados). */
    uint64_t hist[HUF_NSIMB];
    memset(hist, 0, sizeof(hist));
    huf_contar(datos, (size_t)total, hist);

    HufCodigos cods;
    if (huf_construir(hist, &cods) == -1) {
        goto fin;
    }

    /* Encabezado fijo. */
    memcpy(cab, HUF_MAGIA_STR, 4);
    le_poner64(cab + 4, total);
    le_poner32(cab + 12, tam_bloque);
    le_poner32(cab + 16, nbloques);
    for (int s = 0; s < HUF_NSIMB; s++) {
        cab[20 + s] = cods.longs[s];
    }

    size_t tam_tabla = (size_t)nbloques * HUF_ENTRADA_TAB;
    off_t off_datos = (off_t)HUF_CAB_FIJA + (off_t)tam_tabla;

    tabla = malloc(tam_tabla > 0 ? tam_tabla : 1);
    if (tabla == NULL) {
        perror("malloc");
        goto fin;
    }

    fds = sys_open2(salida, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fds == -1) {
        goto fin;
    }
    /* Escribir el encabezado fijo ya; la tabla se completa al final. Reservar su
     * espacio truncando el archivo hasta off_datos. */
    if (pwrite_todo(fds, cab, HUF_CAB_FIJA, 0) == -1) {
        goto fin;
    }
    if (sys_ftruncate(fds, off_datos) == -1) {
        goto fin;
    }

    /* Codificar bloque a bloque, escribiendo cada flujo en off_datos acumulado.
     * Cota del buffer comprimido: cada símbolo ocupa como máximo HUF_MAX_LONG
     * bits = 4 bytes (datos incompresibles o códigos limitados a 32 bits), más
     * un byte de relleno final. Reservar esa cota garantiza que
     * huf_codificar_bloque nunca desborde. */
    size_t cap_comp = (size_t)tam_bloque * (HUF_MAX_LONG / 8) + 8;
    comp = malloc(cap_comp);
    if (comp == NULL) {
        perror("malloc");
        goto fin;
    }
    off_t cursor = off_datos;
    for (uint32_t k = 0; k < nbloques; k++) {
        uint64_t ini = (uint64_t)k * tam_bloque;
        size_t n = (size_t)(ini + tam_bloque <= total ? tam_bloque : total - ini);
        const uint8_t *bloque = datos + ini;

        size_t nbits = 0;
        if (huf_codificar_bloque(&cods, bloque, n, comp, cap_comp, &nbits) == -1) {
            huf_err(COLOR_ERROR "huf: no se pudo codificar el bloque %u" COLOR_RESET "\n", k);
            goto fin;
        }
        size_t nbytes = (nbits + 7) / 8;
        if (pwrite_todo(fds, comp, nbytes, cursor) == -1) {
            goto fin;
        }
        uint32_t crc = huf_crc32(0, bloque, n);
        uint8_t *e = tabla + (size_t)k * HUF_ENTRADA_TAB;
        le_poner64(e + 0, (uint64_t)cursor);
        le_poner64(e + 8, (uint64_t)nbytes);
        le_poner64(e + 16, (uint64_t)n);
        le_poner32(e + 24, crc);
        le_poner32(e + 28, (uint32_t)nbits);
        cursor += (off_t)nbytes;
    }

    /* Completar la tabla de bloques (reservada tras el encabezado). */
    if (tam_tabla > 0 && pwrite_todo(fds, tabla, tam_tabla, (off_t)HUF_CAB_FIJA) == -1) {
        goto fin;
    }
    /* Ajustar el tamaño final exacto (por si el archivo tenía basura previa). */
    if (sys_ftruncate(fds, cursor) == -1) {
        goto fin;
    }
    rc = 0;

fin:
    free(datos);
    free(comp);
    free(tabla);
    if (fde != -1 && sys_close(fde) == -1) {
        rc = -1;
    }
    if (fds != -1 && sys_close(fds) == -1) {
        rc = -1;
    }
    return rc;
}

int huf_descomprimir_archivo(const char *entrada, const char *salida) {
    int fde = -1, fds = -1, rc = -1;
    uint8_t *tabla = NULL, *comp = NULL, *plano = NULL;
    uint8_t cab[HUF_CAB_FIJA];

    fde = sys_open2(entrada, O_RDONLY, 0);
    if (fde == -1) {
        return -1;
    }
    size_t leidos;
    int r = pread_todo(fde, cab, HUF_CAB_FIJA, 0, &leidos);
    if (r == -1) {
        goto fin;
    }
    if (r == 1 || memcmp(cab, HUF_MAGIA_STR, 4) != 0) {
        huf_err(COLOR_ERROR "huf: '%s' no es un archivo HUF1 válido" COLOR_RESET "\n", entrada);
        goto fin;
    }
    uint64_t total = le_leer64(cab + 4);
    uint32_t tam_bloque = le_leer32(cab + 12);
    uint32_t nbloques = le_leer32(cab + 16);
    uint8_t longs[HUF_NSIMB];
    for (int s = 0; s < HUF_NSIMB; s++) {
        longs[s] = cab[20 + s];
    }
    (void)tam_bloque;

    size_t tam_tabla = (size_t)nbloques * HUF_ENTRADA_TAB;
    tabla = malloc(tam_tabla > 0 ? tam_tabla : 1);
    if (tabla == NULL) {
        perror("malloc");
        goto fin;
    }
    if (tam_tabla > 0) {
        r = pread_todo(fde, tabla, tam_tabla, (off_t)HUF_CAB_FIJA, &leidos);
        if (r != 0) {
            huf_err(COLOR_ERROR "huf: tabla de bloques truncada" COLOR_RESET "\n");
            goto fin;
        }
    }

    fds = sys_open2(salida, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fds == -1) {
        goto fin;
    }

    uint64_t escritos_total = 0;
    for (uint32_t k = 0; k < nbloques; k++) {
        const uint8_t *e = tabla + (size_t)k * HUF_ENTRADA_TAB;
        uint64_t off_c = le_leer64(e + 0);
        uint64_t len_c = le_leer64(e + 8);
        uint64_t len_o = le_leer64(e + 16);
        uint32_t crc = le_leer32(e + 24);
        uint32_t nbits = le_leer32(e + 28);

        comp = malloc(len_c > 0 ? (size_t)len_c : 1);
        plano = malloc(len_o > 0 ? (size_t)len_o : 1);
        if (comp == NULL || plano == NULL) {
            perror("malloc");
            goto fin;
        }
        if (len_c > 0) {
            r = pread_todo(fde, comp, (size_t)len_c, (off_t)off_c, &leidos);
            if (r != 0) {
                huf_err(COLOR_ERROR "huf: bloque %u truncado" COLOR_RESET "\n", k);
                goto fin;
            }
        }
        if (huf_decodificar_bloque(longs, comp, (size_t)nbits, plano, (size_t)len_o) == -1) {
            goto fin;
        }
        if (huf_crc32(0, plano, (size_t)len_o) != crc) {
            huf_err(COLOR_ERROR "huf: CRC32 no coincide en el bloque %u (datos corruptos)" COLOR_RESET "\n", k);
            goto fin;
        }
        if (pwrite_todo(fds, plano, (size_t)len_o, (off_t)escritos_total) == -1) {
            goto fin;
        }
        escritos_total += len_o;
        free(comp); comp = NULL;
        free(plano); plano = NULL;
    }

    if (escritos_total != total) {
        huf_err(COLOR_ERROR "huf: tamaño reconstruido (%llu) != esperado (%llu)" COLOR_RESET "\n",
                (unsigned long long)escritos_total, (unsigned long long)total);
        goto fin;
    }
    if (sys_ftruncate(fds, (off_t)total) == -1) {
        goto fin;
    }
    rc = 0;

fin:
    free(tabla);
    free(comp);
    free(plano);
    if (fde != -1 && sys_close(fde) == -1) {
        rc = -1;
    }
    if (fds != -1 && sys_close(fds) == -1) {
        rc = -1;
    }
    return rc;
}

/**
 * ====================================================================================
 * MOTOR CONCURRENTE (etapas 2-4): pool de hilos + fases paralelas
 * ====================================================================================
 * Diseño y justificación completos en docs/CONCURRENCIA.md. Aquí, en resumen:
 *
 *  FASE 1 (conteo):  cada tarea = un bloque. El worker lee su bloque con pread()
 *                    sobre un fd propio del trabajo y cuenta en un histograma
 *                    LOCAL de la tarea (sin ningún lock por byte). El coordinador
 *                    suma los histogramas parciales DESPUÉS del join (un solo
 *                    hilo: sin lock). Documentado: cero contención durante el
 *                    conteo porque no hay dato compartido mientras se cuenta.
 *
 *  CONSTRUCCIÓN:     el coordinador construye el árbol UNA vez (huf_construir).
 *
 *  FASE 2 (codif.):  cada tarea = un bloque. El worker SOLO LEE la tabla de
 *                    códigos (inmutable tras construirse: lectura concurrente sin
 *                    lock), codifica a un buffer propio y lo entrega al
 *                    ensamblador. Un hilo ensamblador escribe en ORDEN con
 *                    pwrite y completa la tabla de bloques.
 *
 *  VENTANA POR ÍNDICE: el bloque i solo arranca si i < siguiente_a_escribir + W
 *                    (W >= nhilos). Garantiza memoria acotada SIN interbloqueo
 *                    (ver comentario en comp_tarea).
 */

#include "hilos.h"
#include <pthread.h>

/* Retardo artificial para pruebas: con -DHUF_LENTO=K cada bloque cuyo índice
 * módulo 7 sea 0 duerme K microsegundos, forzando que los bloques terminen
 * fuera de orden y ejerciten el ensamblador. SOLO se activa en la compilación
 * de pruebas; en producción HUF_LENTO no está definido y esto no genera código. */
#ifdef HUF_LENTO
#include <time.h>
static void huf_retardo_prueba(uint32_t k) {
    if (k % 7 == 0) {
        struct timespec ts = { 0, (long)(HUF_LENTO) * 1000L };
        nanosleep(&ts, NULL);
    }
}
#else
static void huf_retardo_prueba(uint32_t k) { (void)k; }
#endif

/* ¿El llamador pidió cancelación por su bandera externa (opc->cancelar)? El
 * motor la consulta entre bloques y, si está activa, propaga la cancelación al
 * pool con hilos_cancelar() para que todos los hilos salgan ordenadamente. */
static int huf_cancel_externa(const HufOpc *opc) {
    /* Lectura ATÓMICA de la bandera (orden relaxed): la escribe otro hilo. */
    return opc != NULL && opc->cancelar != NULL &&
           __atomic_load_n(opc->cancelar, __ATOMIC_RELAXED) != 0;
}

/* ------------------------------------------------------------------------------------
 * FASE 1: conteo concurrente de frecuencias
 * ------------------------------------------------------------------------------------ */

/* Contexto compartido de la fase de conteo. El único dato realmente compartido
 * es la bandera de cancelación (vía el pool) y el fd (que se usa con pread, sin
 * cursor compartido). Los histogramas son por-tarea: no se comparten. */
typedef struct {
    int fd;                   /* fd propio del trabajo (solo pread, offset explícito) */
    uint64_t total;           /* tamaño del archivo */
    uint32_t tam_bloque;
    uint32_t nbloques;
    uint64_t (*parciales)[HUF_NSIMB];  /* nbloques histogramas, uno por tarea */
    int error;                /* 1 si alguna tarea falló (bajo nada: ver abajo) */
    pthread_mutex_t mtx;      /* protege 'error' y el contador de progreso */
    uint32_t hechos;          /* bloques completados (progreso) */
    /* Progreso GLOBAL y MONÓTONO: el conteo es la 1ª mitad de la compresión, así
     * que se reporta (prog_base + bloques_contados) sobre prog_total. Para la
     * descompresión (una sola fase) prog_base=0 y prog_total=nbloques. */
    uint32_t prog_base;
    uint32_t prog_total;
    const HufOpc *opc;
} CtxConteo;

/* Argumento de una tarea de conteo: su contexto y su índice de bloque. */
typedef struct {
    CtxConteo *ctx;
    uint32_t idx;
} TareaConteo;

static void conteo_tarea(void *arg, Pool *pool) {
    TareaConteo *tc = arg;
    CtxConteo *ctx = tc->ctx;
    uint32_t k = tc->idx;

    /* Cancelación cooperativa: se revisa ANTES de hacer trabajo pesado, tanto la
     * interna del pool como la bandera externa del llamador (comando 'k'). */
    if (huf_cancel_externa(ctx->opc)) {
        hilos_cancelar(pool);
    }
    if (hilos_cancelado(pool)) {
        return;
    }

    uint64_t ini = (uint64_t)k * ctx->tam_bloque;
    size_t n = (size_t)(ini + ctx->tam_bloque <= ctx->total ? ctx->tam_bloque : ctx->total - ini);

    uint8_t *buf = malloc(n > 0 ? n : 1);
    if (buf == NULL) {
        perror("malloc");
        int rc = pthread_mutex_lock(&ctx->mtx);
        if (rc == 0) { ctx->error = 1; pthread_mutex_unlock(&ctx->mtx); }
        hilos_cancelar(pool);
        return;
    }
    size_t leidos;
    int r = pread_todo(ctx->fd, buf, n, (off_t)ini, &leidos);
    if (r == -1 || (r == 1 && leidos != n)) {
        free(buf);
        int rc = pthread_mutex_lock(&ctx->mtx);
        if (rc == 0) { ctx->error = 1; pthread_mutex_unlock(&ctx->mtx); }
        hilos_cancelar(pool);
        return;
    }
    /* Conteo en el histograma LOCAL de ESTA tarea: sin lock, sin contención. */
    huf_contar(buf, n, ctx->parciales[k]);
    free(buf);

    /* Progreso: una sección crítica mínima por bloque. Se reporta en unidades
     * GLOBALES (prog_base + hechos) sobre prog_total para que el porcentaje sea
     * monótono a través de las dos fases de la compresión. */
    int rc = pthread_mutex_lock(&ctx->mtx);
    if (rc == 0) {
        ctx->hechos++;
        uint32_t h = ctx->hechos;
        pthread_mutex_unlock(&ctx->mtx);
        if (ctx->opc != NULL && ctx->opc->progreso != NULL) {
            ctx->opc->progreso(ctx->prog_base + h, ctx->prog_total, ctx->opc->dato);
        }
    }
}

/* Cuenta frecuencias en paralelo. Rellena hist[256]. Retorna 0 ok, -1 error,
 * 1 cancelado. Reutilizable: el conteo paralelo debe dar EXACTAMENTE el mismo
 * histograma que huf_contar secuencial. */
static int contar_concurrente(int fd, uint64_t total, uint32_t tam_bloque,
                              uint32_t nbloques, Pool *pool, const HufOpc *opc,
                              uint32_t prog_base, uint32_t prog_total,
                              uint64_t hist[HUF_NSIMB]) {
    memset(hist, 0, HUF_NSIMB * sizeof(uint64_t));
    if (nbloques == 0) {
        return 0;
    }
    CtxConteo ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.fd = fd;
    ctx.total = total;
    ctx.tam_bloque = tam_bloque;
    ctx.nbloques = nbloques;
    ctx.prog_base = prog_base;
    ctx.prog_total = prog_total;
    ctx.opc = opc;
    int rc = pthread_mutex_init(&ctx.mtx, NULL);
    if (rc != 0) {
        huf_err(COLOR_ERROR "pthread_mutex_init: %s" COLOR_RESET "\n", strerror(rc));
        return -1;
    }
    ctx.parciales = calloc(nbloques, sizeof(*ctx.parciales));
    TareaConteo *tareas = calloc(nbloques, sizeof(TareaConteo));
    if (ctx.parciales == NULL || tareas == NULL) {
        perror("calloc");
        free(ctx.parciales);
        free(tareas);
        pthread_mutex_destroy(&ctx.mtx);
        return -1;
    }

    int ret = 0;
    for (uint32_t k = 0; k < nbloques; k++) {
        tareas[k].ctx = &ctx;
        tareas[k].idx = k;
        if (hilos_encolar(pool, conteo_tarea, &tareas[k]) == -1) {
            ret = -1;
            hilos_cancelar(pool);
            break;
        }
    }
    /* Barrera: esperar a que TODAS las tareas de conteo terminen. */
    hilos_esperar(pool);

    if (hilos_cancelado(pool)) {
        ret = ret == 0 ? 1 : ret;
    }
    if (ctx.error) {
        ret = -1;
    }
    if (ret == 0) {
        /* Reducción en el COORDINADOR (un solo hilo): sumar los histogramas
         * parciales. No hace falta lock porque todas las tareas ya terminaron
         * (hilos_esperar fue la barrera) y nadie más toca 'parciales'. Esto
         * evita por completo la contención que habría si cada tarea sumara al
         * histograma global bajo un lock. */
        for (uint32_t k = 0; k < nbloques; k++) {
            for (int s = 0; s < HUF_NSIMB; s++) {
                hist[s] += ctx.parciales[k][s];
            }
        }
    }
    free(ctx.parciales);
    free(tareas);
    pthread_mutex_destroy(&ctx.mtx);
    return ret;
}

/* ------------------------------------------------------------------------------------
 * FASE 2: codificación concurrente + ensamblado ordenado
 * ------------------------------------------------------------------------------------ */

/* Un bloque codificado listo para escribir. */
typedef struct {
    uint8_t *datos;    /* buffer malloc'd con el flujo de bits (NULL si no listo) */
    size_t   nbytes;   /* bytes del flujo */
    size_t   nbits;    /* bits útiles */
    size_t   n_orig;   /* bytes originales del bloque */
    uint32_t crc;      /* CRC32 del bloque original */
    int      listo;    /* 1 cuando el worker lo depositó */
} BloqueComp;

typedef struct {
    int fd_in;                 /* fd de entrada (pread, offset explícito) */
    int fd_out;                /* fd del temporal de salida (pwrite) */
    uint64_t total;
    uint32_t tam_bloque;
    uint32_t nbloques;
    const HufCodigos *cods;    /* SOLO LECTURA tras construirse (sin lock) */
    uint8_t *tabla;            /* tabla de bloques en memoria (la llena el escritor) */
    off_t off_datos;           /* donde empiezan los flujos en el .huf */

    /* ------------------------------------------------------------------
     * Sincronización del ensamblado. Un único mutex protege: el reparto de
     * índices (sig_asignar), el estado de cada BloqueComp (listo/datos), el
     * cursor del escritor (sig_escribir), el error y el progreso.
     * ------------------------------------------------------------------ */
    pthread_mutex_t mtx;
    pthread_cond_t  cond_listo;     /* el escritor espera a que el bloque que le toca esté listo */
    pthread_cond_t  cond_ventana;   /* los workers esperan a entrar en la ventana por índice */

    uint32_t sig_asignar;      /* próximo índice de bloque a tomar por un worker */
    uint32_t sig_escribir;     /* próximo índice que el escritor debe persistir */
    uint32_t ventana;          /* W: bloques en vuelo permitidos (>= nhilos) */
    BloqueComp *bloques;       /* array de nbloques entradas */
    off_t cursor_escr;         /* offset actual de escritura */
    /* Progreso GLOBAL/MONÓTONO: la codificación es la 2ª mitad de la compresión,
     * se reporta (prog_base + bloques_escritos) sobre prog_total (= 2*nbloques,
     * con prog_base = nbloques). */
    uint32_t prog_base;
    uint32_t prog_total;

    int error;                 /* 1 si alguna etapa falló */
    const HufOpc *opc;
} CtxComp;

/* Reporta error y despierta a todos para que salgan ordenadamente. Debe
 * llamarse CON el mutex tomado. */
static void comp_marcar_error(CtxComp *ctx) {
    ctx->error = 1;
    pthread_cond_broadcast(&ctx->cond_listo);
    pthread_cond_broadcast(&ctx->cond_ventana);
}

/* Tarea de codificación: toma índices de bloque, codifica y deposita. Varias de
 * estas corren en paralelo (una por worker). */
static void comp_tarea(void *arg, Pool *pool) {
    CtxComp *ctx = arg;

    for (;;) {
        /* Propagar la cancelación externa (comando 'k') a la del pool y despertar
         * a los que esperan en las condvars (incluido el ensamblador) para que
         * salgan sin quedarse bloqueados. */
        if (huf_cancel_externa(ctx->opc)) {
            hilos_cancelar(pool);
            pthread_mutex_lock(&ctx->mtx);
            pthread_cond_broadcast(&ctx->cond_listo);
            pthread_cond_broadcast(&ctx->cond_ventana);
            pthread_mutex_unlock(&ctx->mtx);
        }
        int rc = pthread_mutex_lock(&ctx->mtx);
        if (rc != 0) {
            huf_err(COLOR_ERROR "pthread_mutex_lock: %s" COLOR_RESET "\n", strerror(rc));
            return;
        }
        /* Tomar el siguiente índice, respetando la VENTANA POR ÍNDICE:
         * el bloque i solo puede empezar si i < sig_escribir + ventana.
         * POR QUÉ no hay interbloqueo: con ventana W >= nhilos, el bloque que el
         * escritor necesita (sig_escribir) siempre cae dentro de [sig_escribir,
         * sig_escribir+W), así que SIEMPRE puede ser tomado por algún worker; el
         * escritor nunca espera por un bloque que ningún worker tiene permiso de
         * procesar. Los bloques posteriores quedan fuera de la ventana y no
         * acaparan memoria ni cupos. */
        while (!ctx->error && !hilos_cancelado(pool) &&
               ctx->sig_asignar < ctx->nbloques &&
               ctx->sig_asignar >= ctx->sig_escribir + ctx->ventana) {
            rc = pthread_cond_wait(&ctx->cond_ventana, &ctx->mtx);
            if (rc != 0) {
                huf_err(COLOR_ERROR "pthread_cond_wait: %s" COLOR_RESET "\n", strerror(rc));
                pthread_mutex_unlock(&ctx->mtx);
                return;
            }
        }
        if (ctx->error || hilos_cancelado(pool) || ctx->sig_asignar >= ctx->nbloques) {
            pthread_mutex_unlock(&ctx->mtx);
            return;
        }
        uint32_t k = ctx->sig_asignar++;
        pthread_mutex_unlock(&ctx->mtx);

        /* --- Trabajo pesado FUERA del lock --- */
        uint64_t ini = (uint64_t)k * ctx->tam_bloque;
        size_t n = (size_t)(ini + ctx->tam_bloque <= ctx->total ? ctx->tam_bloque : ctx->total - ini);
        uint8_t *orig = malloc(n > 0 ? n : 1);
        size_t cap = n * (HUF_MAX_LONG / 8) + 8;
        uint8_t *comp = malloc(cap);
        if (orig == NULL || comp == NULL) {
            perror("malloc");
            free(orig); free(comp);
            pthread_mutex_lock(&ctx->mtx);
            comp_marcar_error(ctx);
            pthread_mutex_unlock(&ctx->mtx);
            hilos_cancelar(pool);
            return;
        }
        size_t leidos;
        int r = pread_todo(ctx->fd_in, orig, n, (off_t)ini, &leidos);
        if (r == -1 || (r == 1 && leidos != n)) {
            free(orig); free(comp);
            pthread_mutex_lock(&ctx->mtx);
            comp_marcar_error(ctx);
            pthread_mutex_unlock(&ctx->mtx);
            hilos_cancelar(pool);
            return;
        }
        huf_retardo_prueba(k);
        size_t nbits = 0;
        if (huf_codificar_bloque(ctx->cods, orig, n, comp, cap, &nbits) == -1) {
            free(orig); free(comp);
            pthread_mutex_lock(&ctx->mtx);
            comp_marcar_error(ctx);
            pthread_mutex_unlock(&ctx->mtx);
            hilos_cancelar(pool);
            return;
        }
        uint32_t crc = huf_crc32(0, orig, n);
        free(orig);

        /* --- Depositar el bloque y, si soy el escritor en turno, drenar --- */
        pthread_mutex_lock(&ctx->mtx);
        ctx->bloques[k].datos = comp;
        ctx->bloques[k].nbytes = (nbits + 7) / 8;
        ctx->bloques[k].nbits = nbits;
        ctx->bloques[k].n_orig = n;
        ctx->bloques[k].crc = crc;
        ctx->bloques[k].listo = 1;
        /* Avisar al que esté esperando este bloque como "siguiente a escribir". */
        pthread_cond_broadcast(&ctx->cond_listo);
        pthread_mutex_unlock(&ctx->mtx);
    }
}

/* Hilo ensamblador: escribe los bloques EN ORDEN. Bloquea en cond_listo hasta
 * que el bloque sig_escribir esté depositado. Corre en el pool como una tarea
 * más (una sola instancia). */
static void comp_ensamblador(void *arg, Pool *pool) {
    CtxComp *ctx = arg;

    for (;;) {
        /* Traducir la cancelación externa a la del pool antes de (re)evaluar. */
        if (huf_cancel_externa(ctx->opc)) {
            hilos_cancelar(pool);
        }
        int rc = pthread_mutex_lock(&ctx->mtx);
        if (rc != 0) {
            huf_err(COLOR_ERROR "pthread_mutex_lock: %s" COLOR_RESET "\n", strerror(rc));
            return;
        }
        if (ctx->sig_escribir >= ctx->nbloques) {
            pthread_mutex_unlock(&ctx->mtx);
            return;   /* Todo escrito */
        }
        /* Esperar (sin espera activa) a que el bloque que toca esté listo. */
        while (!ctx->error && !hilos_cancelado(pool) &&
               !ctx->bloques[ctx->sig_escribir].listo) {
            rc = pthread_cond_wait(&ctx->cond_listo, &ctx->mtx);
            if (rc != 0) {
                huf_err(COLOR_ERROR "pthread_cond_wait: %s" COLOR_RESET "\n", strerror(rc));
                pthread_mutex_unlock(&ctx->mtx);
                return;
            }
        }
        if (ctx->error || hilos_cancelado(pool)) {
            pthread_mutex_unlock(&ctx->mtx);
            return;
        }
        uint32_t k = ctx->sig_escribir;
        BloqueComp *b = &ctx->bloques[k];
        off_t off = ctx->cursor_escr;
        /* Datos para escribir fuera del lock. */
        uint8_t *datos = b->datos;
        size_t nbytes = b->nbytes;
        size_t nbits = b->nbits;
        size_t n_orig = b->n_orig;
        uint32_t crc = b->crc;
        pthread_mutex_unlock(&ctx->mtx);

        if (pwrite_todo(ctx->fd_out, datos, nbytes, off) == -1) {
            pthread_mutex_lock(&ctx->mtx);
            comp_marcar_error(ctx);
            pthread_mutex_unlock(&ctx->mtx);
            hilos_cancelar(pool);
            return;
        }
        /* Rellenar la entrada de la tabla de este bloque. */
        uint8_t *e = ctx->tabla + (size_t)k * HUF_ENTRADA_TAB;
        le_poner64(e + 0, (uint64_t)off);
        le_poner64(e + 8, (uint64_t)nbytes);
        le_poner64(e + 16, (uint64_t)n_orig);
        le_poner32(e + 24, crc);
        le_poner32(e + 28, (uint32_t)nbits);

        pthread_mutex_lock(&ctx->mtx);
        free(b->datos);
        b->datos = NULL;
        b->listo = 0;
        ctx->cursor_escr += (off_t)nbytes;
        ctx->sig_escribir++;
        uint32_t hechos = ctx->sig_escribir;
        /* Avanzó la ventana: despertar a workers que esperaban entrar. */
        pthread_cond_broadcast(&ctx->cond_ventana);
        pthread_mutex_unlock(&ctx->mtx);

        if (ctx->opc != NULL && ctx->opc->progreso != NULL) {
            ctx->opc->progreso(ctx->prog_base + hechos, ctx->prog_total, ctx->opc->dato);
        }
    }
}

int huf_comprimir_concurrente(const char *entrada, const char *salida, const HufOpc *opc) {
    HufOpc vacia; memset(&vacia, 0, sizeof(vacia));
    if (opc == NULL) {
        opc = &vacia;
    }
    uint32_t tam_bloque = opc->tam_bloque ? opc->tam_bloque : HUF_TAM_BLOQUE;

    int fde = -1, fds = -1, rc = -1;
    char *tmp = NULL;
    uint8_t *tabla = NULL;
    Pool *pool = NULL;
    CtxComp ctx;
    int ctx_init = 0;
    uint8_t cab[HUF_CAB_FIJA];

    fde = sys_open2(entrada, O_RDONLY, 0);
    if (fde == -1) {
        return -1;
    }
    off_t tam = sys_tamano(fde);
    if (tam == -1) {
        goto fin;
    }
    uint64_t total = (uint64_t)tam;
    uint32_t nbloques = total == 0 ? 0 : (uint32_t)((total + tam_bloque - 1) / tam_bloque);

    pool = hilos_crear(opc->nhilos);
    if (pool == NULL) {
        goto fin;
    }

    /* FASE 1: histograma concurrente. El progreso de compresión es GLOBAL y
     * MONÓTONO sobre las dos fases con PONDERACIÓN 50/50: el conteo son las
     * primeras 'nbloques' unidades de un total de 2*nbloques; la codificación son
     * las segundas 'nbloques'. Así 't' nunca muestra un porcentaje que baje. */
    uint64_t hist[HUF_NSIMB];
    int cr = contar_concurrente(fde, total, tam_bloque, nbloques, pool, opc,
                                0, 2 * nbloques, hist);
    if (cr != 0) {
        rc = cr;   /* -1 error, 1 cancelado */
        goto fin;
    }

    HufCodigos cods;
    if (huf_construir(hist, &cods) == -1) {
        goto fin;
    }

    /* Encabezado fijo. */
    memcpy(cab, HUF_MAGIA_STR, 4);
    le_poner64(cab + 4, total);
    le_poner32(cab + 12, tam_bloque);
    le_poner32(cab + 16, nbloques);
    for (int s = 0; s < HUF_NSIMB; s++) {
        cab[20 + s] = cods.longs[s];
    }
    size_t tam_tabla = (size_t)nbloques * HUF_ENTRADA_TAB;
    off_t off_datos = (off_t)HUF_CAB_FIJA + (off_t)tam_tabla;

    tabla = calloc(tam_tabla > 0 ? tam_tabla : 1, 1);
    if (tabla == NULL) {
        perror("calloc");
        goto fin;
    }

    /* Salida a temporal en el MISMO directorio para que rename() sea atómico. */
    if (asprintf(&tmp, "%s.tmp", salida) == -1) {
        perror("asprintf");
        tmp = NULL;
        goto fin;
    }
    fds = sys_open2(tmp, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fds == -1) {
        goto fin;
    }
    if (pwrite_todo(fds, cab, HUF_CAB_FIJA, 0) == -1) {
        goto fin;
    }
    if (sys_ftruncate(fds, off_datos) == -1) {
        goto fin;
    }

    /* FASE 2: codificación paralela + ensamblado ordenado. */
    memset(&ctx, 0, sizeof(ctx));
    ctx.fd_in = fde;
    ctx.fd_out = fds;
    ctx.total = total;
    ctx.tam_bloque = tam_bloque;
    ctx.nbloques = nbloques;
    ctx.cods = &cods;
    ctx.tabla = tabla;
    ctx.off_datos = off_datos;
    ctx.cursor_escr = off_datos;
    ctx.prog_base = nbloques;      /* 2ª mitad del progreso global de compresión */
    ctx.prog_total = 2 * nbloques;
    ctx.opc = opc;
    /* W = 2*nhilos (>= nhilos): holgura para que haya bloques listos por delante
     * del escritor sin acumular memoria sin límite. */
    ctx.ventana = (uint32_t)(2 * hilos_num(pool));
    if (ctx.ventana == 0) {
        ctx.ventana = 1;
    }
    if (pthread_mutex_init(&ctx.mtx, NULL) != 0 ||
        pthread_cond_init(&ctx.cond_listo, NULL) != 0 ||
        pthread_cond_init(&ctx.cond_ventana, NULL) != 0) {
        huf_err(COLOR_ERROR "huf: no se pudieron crear primitivas de sincronización" COLOR_RESET "\n");
        goto fin;
    }
    ctx_init = 1;
    ctx.bloques = calloc(nbloques > 0 ? nbloques : 1, sizeof(BloqueComp));
    if (ctx.bloques == NULL) {
        perror("calloc");
        goto fin;
    }

    if (nbloques > 0) {
        /* Encolar N tareas codificadoras (cada una toma índices en bucle hasta
         * agotar). El ENSAMBLADOR lo ejecuta ESTE hilo (el orquestador) de forma
         * inline: así no compite por una ranura del pool. Si corriera como una
         * tarea más del pool y los N workers tomaran tareas codificadoras, el
         * ensamblador quedaría en la cola sin hilo que lo ejecute, y como la
         * ventana solo avanza cuando el ensamblador escribe, se produciría un
         * interbloqueo. Ejecutarlo aquí lo evita por completo. */
        int ok = 1;
        for (int i = 0; i < hilos_num(pool); i++) {
            if (hilos_encolar(pool, comp_tarea, &ctx) == -1) {
                ok = 0;
                break;
            }
        }
        if (!ok) {
            pthread_mutex_lock(&ctx.mtx);
            comp_marcar_error(&ctx);
            pthread_mutex_unlock(&ctx.mtx);
            hilos_cancelar(pool);
        } else {
            /* Drenar en orden desde el hilo orquestador. */
            comp_ensamblador(&ctx, pool);
        }
        /* Esperar a que las tareas codificadoras terminen (barrera). */
        hilos_esperar(pool);
    }

    if (hilos_cancelado(pool) && !ctx.error) {
        rc = 1;   /* cancelado */
        goto fin;
    }
    if (ctx.error) {
        rc = -1;
        goto fin;
    }

    /* Completar la tabla y ajustar tamaño. */
    if (tam_tabla > 0 && pwrite_todo(fds, tabla, tam_tabla, (off_t)HUF_CAB_FIJA) == -1) {
        goto fin;
    }
    if (sys_ftruncate(fds, ctx.cursor_escr) == -1) {
        goto fin;
    }
    if (sys_close(fds) == -1) {
        fds = -1;
        goto fin;
    }
    fds = -1;
    /* rename() atómico: la salida final aparece completa o no aparece. */
    HTRAZA("rename", "\"%s\", \"%s\"", tmp, salida);
    if (rename(tmp, salida) == -1) {
        hfallo("rename");
        goto fin;
    }
    HTRAZA_OK(0);
    free(tmp);
    tmp = NULL;
    rc = 0;

fin:
    if (ctx_init) {
        if (ctx.bloques != NULL) {
            for (uint32_t k = 0; k < nbloques; k++) {
                free(ctx.bloques[k].datos);
            }
            free(ctx.bloques);
        }
        pthread_cond_destroy(&ctx.cond_ventana);
        pthread_cond_destroy(&ctx.cond_listo);
        pthread_mutex_destroy(&ctx.mtx);
    }
    if (pool != NULL) {
        hilos_destruir(pool);   /* join de TODOS los hilos (éxito/error/cancelación) */
    }
    free(tabla);
    if (fds != -1 && sys_close(fds) == -1) {
        rc = (rc == 1) ? 1 : -1;
    }
    if (fde != -1 && sys_close(fde) == -1) {
        rc = (rc == 1) ? 1 : -1;
    }
    /* Si no se logró la salida final, borrar el temporal: nunca queda a medias. */
    if (tmp != NULL) {
        HTRAZA("unlink", "\"%s\"", tmp);
        if (unlink(tmp) == -1 && errno != ENOENT) {
            hfallo("unlink");
        } else {
            HTRAZA_OK(0);
        }
        free(tmp);
    }
    return rc;
}

/* ------------------------------------------------------------------------------------
 * ETAPA 4: descompresión concurrente
 * ------------------------------------------------------------------------------------
 * Cada tarea = un bloque: pread del flujo comprimido, decodifica, verifica CRC32
 * y escribe el resultado con pwrite en el offset (indice * tam_bloque) del
 * archivo de salida. Los destinos de escritura son DISJUNTOS (bloque i ocupa
 * [i*tam_bloque, i*tam_bloque + len_orig_i), y len_orig_i <= tam_bloque para
 * todo bloque salvo que es exactamente tam_bloque en los no finales), así que
 * NO hace falta ordenar ni bloquear la escritura entre hilos. Se incluye una
 * aserción de rango para respaldar ese argumento.
 */
typedef struct {
    int fd_in;
    int fd_out;
    uint64_t total;
    uint32_t tam_bloque;
    uint32_t nbloques;
    uint8_t longs[HUF_NSIMB];
    const uint8_t *tabla;      /* solo lectura */
    off_t tam_archivo_in;      /* para validar rangos */
    int error;
    pthread_mutex_t mtx;       /* protege 'error' y el progreso */
    uint32_t hechos;
    const HufOpc *opc;
} CtxDesc;

typedef struct {
    CtxDesc *ctx;
    uint32_t idx;
} TareaDesc;

static void desc_fallar(CtxDesc *ctx, Pool *pool) {
    int rc = pthread_mutex_lock(&ctx->mtx);
    if (rc == 0) { ctx->error = 1; pthread_mutex_unlock(&ctx->mtx); }
    hilos_cancelar(pool);
}

static void desc_tarea(void *arg, Pool *pool) {
    TareaDesc *td = arg;
    CtxDesc *ctx = td->ctx;
    uint32_t k = td->idx;

    if (huf_cancel_externa(ctx->opc)) {
        hilos_cancelar(pool);
    }
    if (hilos_cancelado(pool)) {
        return;
    }
    const uint8_t *e = ctx->tabla + (size_t)k * HUF_ENTRADA_TAB;
    uint64_t off_c = le_leer64(e + 0);
    uint64_t len_c = le_leer64(e + 8);
    uint64_t len_o = le_leer64(e + 16);
    uint32_t crc = le_leer32(e + 24);
    uint32_t nbits = le_leer32(e + 28);

    /* Validación de rango del bloque comprimido (defensa ante .huf corrupto). */
    if (len_o > ctx->tam_bloque ||
        off_c > (uint64_t)ctx->tam_archivo_in ||
        len_c > (uint64_t)ctx->tam_archivo_in - off_c) {
        huf_err(COLOR_ERROR "huf: bloque %u fuera de rango (archivo corrupto)" COLOR_RESET "\n", k);
        desc_fallar(ctx, pool);
        return;
    }
    /* Aserción del argumento de destinos disjuntos: el bloque k se escribe en
     * [k*tam_bloque, k*tam_bloque+len_o) y len_o <= tam_bloque, de modo que no
     * se solapa con el bloque k+1 que empieza en (k+1)*tam_bloque. */
    uint64_t destino = (uint64_t)k * ctx->tam_bloque;
    if (destino + len_o > ctx->total || (k + 1 < ctx->nbloques && len_o > ctx->tam_bloque)) {
        huf_err(COLOR_ERROR "huf: destino del bloque %u inconsistente" COLOR_RESET "\n", k);
        desc_fallar(ctx, pool);
        return;
    }

    uint8_t *comp = malloc(len_c > 0 ? (size_t)len_c : 1);
    uint8_t *plano = malloc(len_o > 0 ? (size_t)len_o : 1);
    if (comp == NULL || plano == NULL) {
        perror("malloc");
        free(comp); free(plano);
        desc_fallar(ctx, pool);
        return;
    }
    if (len_c > 0) {
        size_t leidos;
        int r = pread_todo(ctx->fd_in, comp, (size_t)len_c, (off_t)off_c, &leidos);
        if (r != 0) {
            huf_err(COLOR_ERROR "huf: bloque %u truncado" COLOR_RESET "\n", k);
            free(comp); free(plano);
            desc_fallar(ctx, pool);
            return;
        }
    }
    if (huf_decodificar_bloque(ctx->longs, comp, (size_t)nbits, plano, (size_t)len_o) == -1) {
        free(comp); free(plano);
        desc_fallar(ctx, pool);
        return;
    }
    if (huf_crc32(0, plano, (size_t)len_o) != crc) {
        huf_err(COLOR_ERROR "huf: CRC32 no coincide en el bloque %u" COLOR_RESET "\n", k);
        free(comp); free(plano);
        desc_fallar(ctx, pool);
        return;
    }
    /* Escritura en destino disjunto: sin lock, sin ordenar. */
    if (len_o > 0 && pwrite_todo(ctx->fd_out, plano, (size_t)len_o, (off_t)destino) == -1) {
        free(comp); free(plano);
        desc_fallar(ctx, pool);
        return;
    }
    free(comp);
    free(plano);

    int rc = pthread_mutex_lock(&ctx->mtx);
    if (rc == 0) {
        ctx->hechos++;
        uint32_t h = ctx->hechos;
        pthread_mutex_unlock(&ctx->mtx);
        if (ctx->opc != NULL && ctx->opc->progreso != NULL) {
            ctx->opc->progreso(h, ctx->nbloques, ctx->opc->dato);
        }
    }
}

int huf_descomprimir_concurrente(const char *entrada, const char *salida, const HufOpc *opc) {
    HufOpc vacia; memset(&vacia, 0, sizeof(vacia));
    if (opc == NULL) {
        opc = &vacia;
    }
    int fde = -1, fds = -1, rc = -1;
    char *tmp = NULL;
    uint8_t *tabla = NULL;
    TareaDesc *tareas = NULL;
    Pool *pool = NULL;
    CtxDesc ctx;
    int ctx_init = 0;
    int reserva_salida = 0;   /* 1 si creamos el archivo de salida con O_EXCL */
    uint8_t cab[HUF_CAB_FIJA];

    fde = sys_open2(entrada, O_RDONLY, 0);
    if (fde == -1) {
        return -1;
    }
    off_t tam_in = sys_tamano(fde);
    if (tam_in == -1) {
        goto fin;
    }
    size_t leidos;
    int r = pread_todo(fde, cab, HUF_CAB_FIJA, 0, &leidos);
    if (r == -1) {
        goto fin;
    }
    if (r == 1 || memcmp(cab, HUF_MAGIA_STR, 4) != 0) {
        huf_err(COLOR_ERROR "huf: '%s' no es un archivo HUF1 válido" COLOR_RESET "\n", entrada);
        goto fin;
    }
    uint64_t total = le_leer64(cab + 4);
    uint32_t tam_bloque = le_leer32(cab + 12);
    uint32_t nbloques = le_leer32(cab + 16);

    /* Validación de coherencia del encabezado antes de confiar en la tabla. */
    if (tam_bloque == 0 && nbloques != 0) {
        huf_err(COLOR_ERROR "huf: encabezado inconsistente (tam_bloque=0)" COLOR_RESET "\n");
        goto fin;
    }
    if (nbloques != 0) {
        uint64_t esperados = (total + tam_bloque - 1) / tam_bloque;
        if ((uint64_t)nbloques != esperados) {
            huf_err(COLOR_ERROR "huf: nbloques (%u) no concuerda con el tamaño original" COLOR_RESET "\n", nbloques);
            goto fin;
        }
    } else if (total != 0) {
        huf_err(COLOR_ERROR "huf: nbloques=0 pero tamaño original != 0" COLOR_RESET "\n");
        goto fin;
    }

    memset(&ctx, 0, sizeof(ctx));
    for (int s = 0; s < HUF_NSIMB; s++) {
        ctx.longs[s] = cab[20 + s];
    }
    size_t tam_tabla = (size_t)nbloques * HUF_ENTRADA_TAB;
    /* La tabla debe caber tras el encabezado dentro del archivo. */
    if ((uint64_t)HUF_CAB_FIJA + tam_tabla > (uint64_t)tam_in) {
        huf_err(COLOR_ERROR "huf: tabla de bloques fuera del archivo (truncado)" COLOR_RESET "\n");
        goto fin;
    }
    tabla = malloc(tam_tabla > 0 ? tam_tabla : 1);
    if (tabla == NULL) {
        perror("malloc");
        goto fin;
    }
    if (tam_tabla > 0) {
        r = pread_todo(fde, tabla, tam_tabla, (off_t)HUF_CAB_FIJA, &leidos);
        if (r != 0) {
            huf_err(COLOR_ERROR "huf: tabla de bloques truncada" COLOR_RESET "\n");
            goto fin;
        }
    }
    /* Validar que la suma de longitudes originales == tamaño original. */
    uint64_t suma_orig = 0;
    for (uint32_t k = 0; k < nbloques; k++) {
        suma_orig += le_leer64(tabla + (size_t)k * HUF_ENTRADA_TAB + 16);
    }
    if (suma_orig != total) {
        huf_err(COLOR_ERROR "huf: suma de bloques (%llu) != tamaño original (%llu)" COLOR_RESET "\n",
                (unsigned long long)suma_orig, (unsigned long long)total);
        goto fin;
    }

    /* NUNCA sobrescribir en silencio: reservar el nombre de salida con O_EXCL.
     * Si ya existe, open falla con EEXIST y se informa sin tocar el archivo
     * previo. El archivo (vacío) que creamos aquí se sustituye por rename() al
     * final; si algo falla antes, se borra en la limpieza (reserva_salida). */
    int probe = sys_open2(salida, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (probe == -1) {
        goto fin;
    }
    reserva_salida = 1;
    if (sys_close(probe) == -1) {
        goto fin;
    }
    if (asprintf(&tmp, "%s.tmp", salida) == -1) {
        perror("asprintf");
        tmp = NULL;
        goto fin;
    }
    fds = sys_open2(tmp, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fds == -1) {
        goto fin;
    }
    if (total > 0 && sys_ftruncate(fds, (off_t)total) == -1) {
        goto fin;
    }

    pool = hilos_crear(opc->nhilos);
    if (pool == NULL) {
        goto fin;
    }
    ctx.fd_in = fde;
    ctx.fd_out = fds;
    ctx.total = total;
    ctx.tam_bloque = tam_bloque;
    ctx.nbloques = nbloques;
    ctx.tabla = tabla;
    ctx.tam_archivo_in = tam_in;
    ctx.opc = opc;
    if (pthread_mutex_init(&ctx.mtx, NULL) != 0) {
        huf_err(COLOR_ERROR "huf: no se pudo crear el mutex" COLOR_RESET "\n");
        goto fin;
    }
    ctx_init = 1;

    tareas = calloc(nbloques > 0 ? nbloques : 1, sizeof(TareaDesc));
    if (tareas == NULL) {
        perror("calloc");
        goto fin;
    }
    int ok = 1;
    for (uint32_t k = 0; k < nbloques; k++) {
        tareas[k].ctx = &ctx;
        tareas[k].idx = k;
        if (hilos_encolar(pool, desc_tarea, &tareas[k]) == -1) {
            ok = 0;
            hilos_cancelar(pool);
            break;
        }
    }
    hilos_esperar(pool);

    if (!ok || ctx.error) {
        rc = -1;
        goto fin;
    }
    if (hilos_cancelado(pool)) {
        rc = 1;
        goto fin;
    }
    if (sys_close(fds) == -1) {
        fds = -1;
        goto fin;
    }
    fds = -1;
    HTRAZA("rename", "\"%s\", \"%s\"", tmp, salida);
    if (rename(tmp, salida) == -1) {
        hfallo("rename");
        goto fin;
    }
    HTRAZA_OK(0);
    reserva_salida = 0;   /* rename sustituyó el archivo reservado */
    free(tmp);
    tmp = NULL;
    rc = 0;

fin:
    if (ctx_init) {
        pthread_mutex_destroy(&ctx.mtx);
    }
    if (pool != NULL) {
        hilos_destruir(pool);
    }
    free(tabla);
    free(tareas);
    if (fds != -1 && sys_close(fds) == -1) {
        rc = (rc == 1) ? 1 : -1;
    }
    if (fde != -1 && sys_close(fde) == -1) {
        rc = (rc == 1) ? 1 : -1;
    }
    if (tmp != NULL) {
        HTRAZA("unlink", "\"%s\"", tmp);
        if (unlink(tmp) == -1 && errno != ENOENT) {
            hfallo("unlink");
        } else {
            HTRAZA_OK(0);
        }
        free(tmp);
    }
    /* Si reservamos el nombre de salida con O_EXCL pero no llegamos a renombrar,
     * borrar ese archivo vacío para no dejar basura ni una salida a medias. */
    if (reserva_salida) {
        HTRAZA("unlink", "\"%s\"", salida);
        if (unlink(salida) == -1 && errno != ENOENT) {
            hfallo("unlink");
        } else {
            HTRAZA_OK(0);
        }
    }
    return rc;
}
