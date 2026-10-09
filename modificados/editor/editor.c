#include "editor.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>   /* signal/raise para preservar Ctrl+C sin trabajo (aditivo) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define TAM_BLOQUE 4096   /* Tamaño de bloque para read/write al desplazar bytes */
#define MAX_LINEA  4096   /* Máximo de caracteres por comando leído de consola */

int ed_traza = 0;

/**
 * ====================================================================================
 * TRAZADO DE SYSCALLS (estilo strace, igual que el shell educativo)
 * ====================================================================================
 * Solo imprime si ed_traza != 0. Va por stdout para que quede intercalado en orden
 * con el resto de mensajes del editor.
 */
#define TRAZA(nombre, fmt, ...) \
    do { if (ed_traza) printf(COLOR_SYSCALL "[syscall] " nombre COLOR_RESET "(" fmt ") ... ", ##__VA_ARGS__); } while (0)
#define TRAZA_OK(r) \
    do { if (ed_traza) printf("= " COLOR_RESULT "%ld" COLOR_RESET "\n", (long)(r)); } while (0)

/* Reporta el fallo de una syscall: cierra la línea de traza (si aplica) y llama
 * a perror(). Conserva errno porque printf podría modificarlo. */
static void fallo(const char *ctx) {
    int err = errno;
    if (ed_traza) {
        printf("= " COLOR_ERROR "-1 (%s)" COLOR_RESET "\n", strerror(err));
    }
    fflush(stdout);
    errno = err;
    perror(ctx);
}

/**
 * ====================================================================================
 * ENVOLTORIOS DE SYSCALLS: cada retorno se verifica
 * ====================================================================================
 */
static int sys_open(const char *ruta) {
    TRAZA("open", "\"%s\", O_RDWR|O_CREAT, 0644", ruta);
    int fd = open(ruta, O_RDWR | O_CREAT, 0644);
    if (fd == -1) {
        fallo("open");
        return -1;
    }
    TRAZA_OK(fd);
    return fd;
}

static off_t sys_lseek(int fd, off_t off, int whence) {
    TRAZA("lseek", "%d, %ld, %s", fd, (long)off,
          whence == SEEK_SET ? "SEEK_SET" : whence == SEEK_END ? "SEEK_END" : "SEEK_CUR");
    off_t r = lseek(fd, off, whence);
    if (r == (off_t)-1) {
        fallo("lseek");
        return -1;
    }
    TRAZA_OK(r);
    return r;
}

/* read(2) que reintenta ante EINTR. Retorna bytes leídos (0 = EOF) o -1. */
static ssize_t sys_read(int fd, void *buf, size_t n) {
    ssize_t r;
    TRAZA("read", "%d, buf, %zu", fd, n);
    do {
        r = read(fd, buf, n);
    } while (r == -1 && errno == EINTR);
    if (r == -1) {
        fallo("read");
        return -1;
    }
    TRAZA_OK(r);
    return r;
}

/* write(2) completo: repite hasta escribir los n bytes (write puede ser parcial). */
static int sys_write_todo(int fd, const char *buf, size_t n) {
    size_t hecho = 0;
    while (hecho < n) {
        TRAZA("write", "%d, buf, %zu", fd, n - hecho);
        ssize_t w = write(fd, buf + hecho, n - hecho);
        if (w == -1) {
            if (errno == EINTR) {
                continue;
            }
            fallo("write");
            return -1;
        }
        TRAZA_OK(w);
        hecho += (size_t)w;
    }
    return 0;
}

/* Lee exactamente n bytes desde 'off'. Se usa al desplazar bytes del archivo. */
static int leer_en(int fd, off_t off, char *buf, size_t n) {
    if (sys_lseek(fd, off, SEEK_SET) == -1) {
        return -1;
    }
    size_t hecho = 0;
    while (hecho < n) {
        ssize_t r = sys_read(fd, buf + hecho, n - hecho);
        if (r == -1) {
            return -1;
        }
        if (r == 0) {          /* EOF inesperado: alguien truncó el archivo por fuera */
            errno = EIO;
            fallo("read");
            return -1;
        }
        hecho += (size_t)r;
    }
    return 0;
}

static int escribir_en(int fd, off_t off, const char *buf, size_t n) {
    if (sys_lseek(fd, off, SEEK_SET) == -1) {
        return -1;
    }
    return sys_write_todo(fd, buf, n);
}

static int sys_ftruncate(int fd, off_t largo) {
    TRAZA("ftruncate", "%d, %ld", fd, (long)largo);
    if (ftruncate(fd, largo) == -1) {
        fallo("ftruncate");
        return -1;
    }
    TRAZA_OK(0);
    return 0;
}

/* Tamaño actual del archivo: lseek(fd, 0, SEEK_END) devuelve el offset final. */
static off_t tamano_archivo(int fd) {
    return sys_lseek(fd, 0, SEEK_END);
}

/**
 * ====================================================================================
 * DESPLAZAMIENTO DE BYTES EN DISCO
 * ====================================================================================
 * Borrar o insertar una línea en medio del archivo obliga a mover todo lo que viene
 * después. No existe una syscall de "insertar bytes": hay que copiar por bloques.
 */

/* Mueve [desde, tam) hacia 'hasta' (hasta < desde) y luego trunca el sobrante.
 * Se copia de adelante hacia atrás: como el destino está antes que el origen,
 * ningún bloque pisa datos que aún no se han leído. */
static int desplazar_izquierda(int fd, off_t desde, off_t hasta, off_t tam) {
    char bloque[TAM_BLOQUE];
    off_t pos = desde;
    while (pos < tam) {
        size_t n = (size_t)(tam - pos) < sizeof(bloque) ? (size_t)(tam - pos) : sizeof(bloque);
        if (leer_en(fd, pos, bloque, n) == -1 || escribir_en(fd, hasta + (pos - desde), bloque, n) == -1) {
            return -1;
        }
        pos += (off_t)n;
    }
    return sys_ftruncate(fd, tam - (desde - hasta));
}

/* Mueve [desde, tam) 'delta' bytes hacia el final, abriendo un hueco en 'desde'.
 * Se copia de atrás hacia adelante: el destino está después del origen, así que
 * empezar por el último bloque evita sobrescribir bytes pendientes. El archivo
 * crece solo al escribir más allá del final (no hace falta ftruncate). */
static int desplazar_derecha(int fd, off_t desde, off_t delta, off_t tam) {
    char bloque[TAM_BLOQUE];
    off_t fin = tam;
    while (fin > desde) {
        size_t n = (size_t)(fin - desde) < sizeof(bloque) ? (size_t)(fin - desde) : sizeof(bloque);
        off_t ini = fin - (off_t)n;
        if (leer_en(fd, ini, bloque, n) == -1 || escribir_en(fd, ini + delta, bloque, n) == -1) {
            return -1;
        }
        fin = ini;
    }
    return 0;
}

/**
 * ====================================================================================
 * UTILIDADES DE PARSEO
 * ====================================================================================
 */
static const char *saltar_espacios(const char *s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

/* Parsea un número de línea >= 1. Si resto == NULL, solo se admiten espacios
 * después del número; si no, *resto apunta a lo que sigue al número. */
static int parsear_n(const char *s, size_t *n, const char **resto) {
    s = saltar_espacios(s);
    if (*s < '0' || *s > '9') {
        return -1;
    }
    char *fin;
    errno = 0;
    unsigned long long v = strtoull(s, &fin, 10);
    if (errno == ERANGE || v == 0) {
        return -1;
    }
    if (resto != NULL) {
        if (*fin != '\0' && *fin != ' ' && *fin != '\t') {
            return -1;
        }
        *resto = fin;
    } else if (*saltar_espacios(fin) != '\0') {
        return -1;
    }
    *n = (size_t)v;
    return 0;
}

static int requiere_archivo(const Editor *ed) {
    if (ed->fd == -1) {
        printf(COLOR_ERROR "Error: no hay archivo abierto. Usa 'o <archivo>' primero." COLOR_RESET "\n");
        return -1;
    }
    return 0;
}

/* Guardia de carrera para a/d/i (aditivo): rechaza modificar el archivo mientras
 * un trabajo Huffman esté activo. he_inicio_edicion consulta el estado del
 * gestor bajo su mutex: si hay un trabajo RUNNING/CANCELLING, retorna -1 y la
 * edición se rechaza SIN tocar nada. Como el REPL es de un solo hilo y un trabajo
 * solo puede arrancar desde este mismo hilo (comandos c/x), y el estado ya es
 * RUNNING cuando c/x retorna al prompt, ningún trabajo puede empezar a leer entre
 * este chequeo y la modificación que sigue; por eso no queda ventana de carrera
 * y no hace falta ningún lock de archivo. */
static int ed_edicion_permitida(Editor *ed) {
    if (ed->huf == NULL) {
        return 0;   /* Sin gestor (no debería pasar en el editor): permitir */
    }
    if (he_inicio_edicion(ed->huf) == -1) {
        printf(COLOR_ERROR "Error: hay un trabajo Huffman en curso; no se puede modificar el archivo."
               COLOR_RESET "\n");
        printf(COLOR_INFO "Usa 'w' para esperar, 'k' para cancelar o 't' para ver el estado." COLOR_RESET "\n");
        return -1;
    }
    he_fin_edicion(ed->huf);
    return 0;
}

static int linea_existe(const Editor *ed, size_t n) {
    if (n > ed->lineas.count) {
        printf(COLOR_ERROR "Error: la línea %zu no existe." COLOR_RESET "\n", n);
        printf(COLOR_INFO "El archivo tiene %zu línea(s)." COLOR_RESET "\n", ed->lineas.count);
        return 0;
    }
    return 1;
}

/**
 * ====================================================================================
 * COMANDOS DEL EDITOR
 * ====================================================================================
 */
void editor_init(Editor *ed) {
    ed->fd = -1;
    ed->ruta = NULL;
    lista_init(&ed->lineas);
    ed->nl_final = 1;
    ed->huf = NULL;
}

/* Divide buf[0..tam) en líneas y las agrega a l. */
static int cargar_lineas(Lista *l, const char *buf, size_t tam) {
    size_t ini = 0;
    for (size_t i = 0; i < tam; i++) {
        if (buf[i] == '\n') {
            if (lista_agregar(l, buf + ini, i - ini) == -1) {
                return -1;
            }
            ini = i + 1;
        }
    }
    if (ini < tam) {   /* Última línea sin '\n' final */
        return lista_agregar(l, buf + ini, tam - ini);
    }
    return 0;
}

/* o <archivo>: abre (o crea) y carga el archivo completo en la lista.
 * Si ya había otro abierto, solo se cierra cuando el nuevo abrió bien. */
int ed_abrir(Editor *ed, const char *ruta) {
    int fd = sys_open(ruta);
    if (fd == -1) {
        return -1;
    }

    /* Leer todo el archivo con read(2) en bloques, creciendo el buffer. */
    size_t cap = TAM_BLOQUE, tam = 0;
    char *buf = malloc(cap);
    Lista nueva;
    lista_init(&nueva);
    char *ruta_copia = strdup(ruta);
    if (buf == NULL || ruta_copia == NULL) {
        perror("malloc");
        goto error;
    }
    while (1) {
        if (tam == cap) {
            char *mas = realloc(buf, cap * 2);
            if (mas == NULL) {
                perror("realloc");
                goto error;
            }
            buf = mas;
            cap *= 2;
        }
        ssize_t r = sys_read(fd, buf + tam, cap - tam);
        if (r == -1) {
            goto error;
        }
        if (r == 0) {
            break;    /* EOF */
        }
        tam += (size_t)r;
    }
    if (cargar_lineas(&nueva, buf, tam) == -1) {
        perror("malloc");
        goto error;
    }

    if (ed->fd != -1) {
        ed_cerrar(ed);   /* Cerrar el archivo anterior antes de reemplazarlo */
    }
    ed->fd = fd;
    ed->ruta = ruta_copia;
    ed->lineas = nueva;
    ed->nl_final = (tam == 0 || buf[tam - 1] == '\n');
    free(buf);

    printf(COLOR_RESULT "Archivo '%s' abierto (fd %d): %zu línea(s), %zu byte(s)." COLOR_RESET "\n",
           ruta, fd, ed->lineas.count, tam);
    return 0;

error:
    free(buf);
    free(ruta_copia);
    lista_liberar(&nueva);
    TRAZA("close", "%d", fd);
    if (close(fd) == -1) {
        fallo("close");
    } else {
        TRAZA_OK(0);
    }
    return -1;
}

/* p [n]: imprime una línea o todas, numeradas. Solo lee la lista (no toca disco). */
int ed_imprimir(Editor *ed, const char *arg) {
    if (requiere_archivo(ed) == -1) {
        return -1;
    }
    if (*saltar_espacios(arg) == '\0') {
        if (ed->lineas.count == 0) {
            printf(COLOR_INFO "(archivo vacío)" COLOR_RESET "\n");
            return 0;
        }
        size_t i = 1;
        for (Linea *ln = ed->lineas.head; ln != NULL; ln = ln->next, i++) {
            printf(COLOR_PARAM "%6zu" COLOR_RESET "  %s\n", i, ln->texto);
        }
        return 0;
    }
    size_t n;
    if (parsear_n(arg, &n, NULL) == -1) {
        printf(COLOR_ERROR "Error: número de línea inválido '%s'. Uso: p [n]" COLOR_RESET "\n", arg);
        return -1;
    }
    if (!linea_existe(ed, n)) {
        return -1;
    }
    printf(COLOR_PARAM "%6zu" COLOR_RESET "  %s\n", n, lista_obtener(&ed->lineas, n)->texto);
    return 0;
}

/* a <texto>: agrega al final. Si el archivo no termina en '\n', primero se
 * escribe uno para no pegar el texto a la última línea existente. */
int ed_agregar(Editor *ed, const char *texto) {
    if (requiere_archivo(ed) == -1) {
        return -1;
    }
    if (ed_edicion_permitida(ed) == -1) {   /* guardia de carrera con el trabajo Huffman (aditivo) */
        return -1;
    }
    size_t len = strlen(texto);
    size_t pre = ed->nl_final ? 0 : 1;
    char *datos = malloc(pre + len + 1);
    if (datos == NULL) {
        perror("malloc");
        return -1;
    }
    datos[0] = '\n';                       /* Se sobrescribe si pre == 0 */
    memcpy(datos + pre, texto, len);
    datos[pre + len] = '\n';

    /* Primero la lista (puede fallar malloc), luego el disco; si el disco
     * falla, se deshace la lista y se trunca lo que se haya escrito. */
    if (lista_agregar(&ed->lineas, texto, len) == -1) {
        perror("malloc");
        free(datos);
        return -1;
    }
    off_t fin = tamano_archivo(ed->fd);
    if (fin == -1 || sys_write_todo(ed->fd, datos, pre + len + 1) == -1) {
        lista_eliminar(&ed->lineas, ed->lineas.count);
        if (fin != -1) {
            sys_ftruncate(ed->fd, fin);
        }
        free(datos);
        return -1;
    }
    free(datos);
    ed->nl_final = 1;
    printf(COLOR_RESULT "Línea %zu agregada." COLOR_RESET "\n", ed->lineas.count);
    return 0;
}

/* d <n>: borra la línea n. En disco: desplaza hacia atrás todos los bytes que
 * siguen a la línea (lseek + read + lseek + write) y recorta con ftruncate. */
int ed_borrar(Editor *ed, const char *arg) {
    if (requiere_archivo(ed) == -1) {
        return -1;
    }
    if (ed_edicion_permitida(ed) == -1) {   /* guardia de carrera con el trabajo Huffman (aditivo) */
        return -1;
    }
    size_t n;
    if (parsear_n(arg, &n, NULL) == -1) {
        printf(COLOR_ERROR "Error: número de línea inválido '%s'. Uso: d <n>" COLOR_RESET "\n", arg);
        return -1;
    }
    if (!linea_existe(ed, n)) {
        return -1;
    }

    Linea *ln = lista_obtener(&ed->lineas, n);
    off_t ini = (off_t)lista_offset(&ed->lineas, n);
    off_t a, b;   /* Rango de bytes [a, b) a eliminar del archivo */
    if (n < ed->lineas.count || ed->nl_final) {
        a = ini;                      /* Caso normal: contenido + su '\n' */
        b = ini + (off_t)ln->len + 1;
    } else if (n > 1) {
        a = ini - 1;                  /* Última línea sin '\n': se quita el '\n' */
        b = ini + (off_t)ln->len;     /* de la anterior para no dejar línea vacía */
    } else {
        a = 0;                        /* Única línea, sin '\n' */
        b = (off_t)ln->len;
    }

    off_t tam = tamano_archivo(ed->fd);
    if (tam == -1 || desplazar_izquierda(ed->fd, b, a, tam) == -1) {
        printf(COLOR_ERROR "Error: no se pudo borrar la línea %zu en disco." COLOR_RESET "\n", n);
        return -1;
    }
    lista_eliminar(&ed->lineas, n);
    if (ed->lineas.count == 0) {
        ed->nl_final = 1;
    }
    printf(COLOR_RESULT "Línea %zu borrada (%ld byte(s))." COLOR_RESET "\n", n, (long)(b - a));
    return 0;
}

/* i <n> <texto>: inserta texto como nueva línea n; la antigua línea n y las
 * siguientes bajan una posición. En disco se abre un hueco de len+1 bytes. */
int ed_insertar(Editor *ed, const char *args) {
    if (requiere_archivo(ed) == -1) {
        return -1;
    }
    if (ed_edicion_permitida(ed) == -1) {   /* guardia de carrera con el trabajo Huffman (aditivo) */
        return -1;
    }
    size_t n;
    const char *resto;
    if (parsear_n(args, &n, &resto) == -1) {
        printf(COLOR_ERROR "Error: número de línea inválido '%s'. Uso: i <n> <texto>" COLOR_RESET "\n", args);
        return -1;
    }
    if (*resto == ' ' || *resto == '\t') {
        resto++;   /* Un solo separador: los espacios extra son parte del texto */
    }
    if (n > ed->lineas.count + 1) {
        printf(COLOR_ERROR "Error: no se puede insertar en la línea %zu." COLOR_RESET "\n", n);
        printf(COLOR_INFO "Rango válido: 1..%zu (n = líneas+1 equivale a 'a')." COLOR_RESET "\n",
               ed->lineas.count + 1);
        return -1;
    }
    if (n == ed->lineas.count + 1) {
        return ed_agregar(ed, resto);
    }

    size_t len = strlen(resto);
    char *datos = malloc(len + 1);
    if (datos == NULL) {
        perror("malloc");
        return -1;
    }
    memcpy(datos, resto, len);
    datos[len] = '\n';

    off_t ini = (off_t)lista_offset(&ed->lineas, n);
    if (lista_insertar(&ed->lineas, n, resto, len) == -1) {
        perror("malloc");
        free(datos);
        return -1;
    }
    off_t tam = tamano_archivo(ed->fd);
    if (tam == -1 || desplazar_derecha(ed->fd, ini, (off_t)len + 1, tam) == -1 ||
        escribir_en(ed->fd, ini, datos, len + 1) == -1) {
        lista_eliminar(&ed->lineas, n);
        free(datos);
        printf(COLOR_ERROR "Error: no se pudo insertar la línea %zu en disco." COLOR_RESET "\n", n);
        return -1;
    }
    free(datos);
    printf(COLOR_RESULT "Línea %zu insertada." COLOR_RESET "\n", n);
    return 0;
}

/* s <palabra>: búsqueda de subcadena (sensible a mayúsculas) en cada línea. */
int ed_buscar(Editor *ed, const char *palabra) {
    if (requiere_archivo(ed) == -1) {
        return -1;
    }
    if (*palabra == '\0') {
        printf(COLOR_ERROR "Error: falta la palabra. Uso: s <palabra>" COLOR_RESET "\n");
        return -1;
    }
    size_t plen = strlen(palabra), lineas = 0, total = 0, i = 1;
    for (Linea *ln = ed->lineas.head; ln != NULL; ln = ln->next, i++) {
        size_t k = 0;
        for (const char *p = strstr(ln->texto, palabra); p != NULL; p = strstr(p + plen, palabra)) {
            k++;
        }
        if (k > 0) {
            printf(COLOR_PARAM "%6zu" COLOR_RESET "  %s  " COLOR_INFO "(%zu)" COLOR_RESET "\n", i, ln->texto, k);
            lineas++;
            total += k;
        }
    }
    if (lineas == 0) {
        printf(COLOR_INFO "'%s' no aparece en el archivo." COLOR_RESET "\n", palabra);
    } else {
        printf(COLOR_RESULT "'%s': %zu ocurrencia(s) en %zu línea(s)." COLOR_RESET "\n", palabra, total, lineas);
    }
    return 0;
}

/* c [salida]: comprime en segundo plano el archivo abierto hacia 'salida'
 * (por defecto <ruta>.huf). Despacho nuevo (aditivo); la lógica vive en el
 * gestor huf_editor. */
static int ed_comprimir(Editor *ed, const char *arg) {
    if (requiere_archivo(ed) == -1) {
        return -1;
    }
    const char *a = saltar_espacios(arg);
    char *salida = NULL;
    if (*a == '\0') {
        /* Por defecto: <ruta>.huf */
        size_t n = strlen(ed->ruta);
        salida = malloc(n + 5);
        if (salida == NULL) {
            perror("malloc");
            return -1;
        }
        memcpy(salida, ed->ruta, n);
        memcpy(salida + n, ".huf", 5);
    } else {
        salida = strdup(a);
        if (salida == NULL) {
            perror("strdup");
            return -1;
        }
    }
    int rc = he_comprimir(ed->huf, ed->ruta, salida);
    free(salida);
    return rc;
}

/* x [salida]: descomprime en segundo plano el archivo abierto (debe terminar en
 * .huf) hacia 'salida' (por defecto, sin la extensión). La salida NUNCA puede ser
 * el archivo abierto en el editor. Despacho nuevo (aditivo). */
static int ed_descomprimir(Editor *ed, const char *arg) {
    if (requiere_archivo(ed) == -1) {
        return -1;
    }
    size_t rl = strlen(ed->ruta);
    const char *a = saltar_espacios(arg);
    char *salida = NULL;
    if (*a == '\0') {
        if (rl < 4 || strcmp(ed->ruta + rl - 4, ".huf") != 0) {
            printf(COLOR_ERROR "Error: el archivo abierto no termina en '.huf'; indica la salida: x <salida>."
                   COLOR_RESET "\n");
            return -1;
        }
        salida = malloc(rl - 3);   /* rl-4 chars + '\0' */
        if (salida == NULL) {
            perror("malloc");
            return -1;
        }
        memcpy(salida, ed->ruta, rl - 4);
        salida[rl - 4] = '\0';
    } else {
        salida = strdup(a);
        if (salida == NULL) {
            perror("strdup");
            return -1;
        }
    }
    if (strcmp(salida, ed->ruta) == 0) {
        printf(COLOR_ERROR "Error: la salida no puede ser el archivo abierto en el editor." COLOR_RESET "\n");
        free(salida);
        return -1;
    }
    int rc = he_descomprimir(ed->huf, ed->ruta, salida);
    free(salida);
    return rc;
}

int ed_cerrar(Editor *ed) {
    int rc = 0;
    if (ed->fd != -1) {
        TRAZA("close", "%d", ed->fd);
        if (close(ed->fd) == -1) {
            fallo("close");
            rc = -1;
        } else {
            TRAZA_OK(0);
        }
    }
    lista_liberar(&ed->lineas);
    free(ed->ruta);
    HufEditor *huf = ed->huf;   /* editor_init lo pondría en NULL; se conserva (aditivo) */
    editor_init(ed);
    ed->huf = huf;
    return rc;
}

static void ayuda(void) {
    printf(COLOR_TITLE "Comandos del editor:" COLOR_RESET "\n");
    printf("  " COLOR_PROMPT "o <archivo>" COLOR_RESET "     abre o crea el archivo (open O_RDWR|O_CREAT)\n");
    printf("  " COLOR_PROMPT "p [n]" COLOR_RESET "           imprime la línea n, o todo el archivo\n");
    printf("  " COLOR_PROMPT "a <texto>" COLOR_RESET "       agrega una línea al final\n");
    printf("  " COLOR_PROMPT "d <n>" COLOR_RESET "           borra la línea n (lseek + ftruncate)\n");
    printf("  " COLOR_PROMPT "i <n> <texto>" COLOR_RESET "   inserta en la línea n desplazando el resto\n");
    printf("  " COLOR_PROMPT "s <palabra>" COLOR_RESET "     busca la palabra y reporta las líneas\n");
    printf("  " COLOR_PROMPT "c [salida]" COLOR_RESET "      comprime (Huffman, 2º plano) -> <ruta>.huf\n");
    printf("  " COLOR_PROMPT "x [salida]" COLOR_RESET "      descomprime un .huf (2º plano) -> sin extensión\n");
    printf("  " COLOR_PROMPT "t" COLOR_RESET "               estado del trabajo Huffman (con %%)\n");
    printf("  " COLOR_PROMPT "k" COLOR_RESET "               cancela el trabajo Huffman en curso\n");
    printf("  " COLOR_PROMPT "w" COLOR_RESET "               espera a que el trabajo Huffman termine\n");
    printf("  " COLOR_PROMPT "q" COLOR_RESET "               cierra el archivo y sale\n");
    printf("  " COLOR_PROMPT "h" COLOR_RESET "               muestra esta ayuda\n");
}

int ed_ejecutar(Editor *ed, char *linea) {
    /* Quitar fin de línea (\n o \r\n) */
    size_t len = strlen(linea);
    while (len > 0 && (linea[len - 1] == '\n' || linea[len - 1] == '\r')) {
        linea[--len] = '\0';
    }
    char *cmd = (char *)saltar_espacios(linea);
    if (*cmd == '\0') {
        return 0;
    }
    char *arg = cmd + 1;
    if (*arg != '\0' && *arg != ' ' && *arg != '\t') {
        printf(COLOR_ERROR "Error: comando '%s' no reconocido. Escribe 'h' para ver la ayuda." COLOR_RESET "\n", cmd);
        return 0;
    }
    if (*arg != '\0') {
        arg++;   /* Saltar el separador */
    }

    switch (*cmd) {
    case 'o': {
        /* La ruta puede tener espacios internos; se recortan los de los extremos */
        char *ruta = (char *)saltar_espacios(arg);
        size_t rl = strlen(ruta);
        while (rl > 0 && (ruta[rl - 1] == ' ' || ruta[rl - 1] == '\t')) {
            ruta[--rl] = '\0';
        }
        if (rl == 0) {
            printf(COLOR_ERROR "Error: falta el archivo. Uso: o <archivo>" COLOR_RESET "\n");
        } else {
            ed_abrir(ed, ruta);
        }
        break;
    }
    case 'p': ed_imprimir(ed, arg); break;
    case 'a': ed_agregar(ed, arg); break;
    case 'd': ed_borrar(ed, arg); break;
    case 'i': ed_insertar(ed, arg); break;
    case 's': {
        char *pal = (char *)saltar_espacios(arg);
        size_t pl = strlen(pal);
        while (pl > 0 && (pal[pl - 1] == ' ' || pal[pl - 1] == '\t')) {
            pal[--pl] = '\0';
        }
        ed_buscar(ed, pal);
        break;
    }
    case 'c': ed_comprimir(ed, arg); break;     /* Huffman en segundo plano (aditivo) */
    case 'x': ed_descomprimir(ed, arg); break;  /* Huffman en segundo plano (aditivo) */
    case 't': if (ed->huf) he_estado_imprimir(ed->huf); break;  /* estado del trabajo */
    case 'k': if (ed->huf) he_cancelar(ed->huf); break;         /* cancelar el trabajo */
    case 'w': if (ed->huf) he_esperar(ed->huf); break;          /* esperar al trabajo */
    case 'q': return 1;
    case 'h': case '?': ayuda(); break;
    default:
        printf(COLOR_ERROR "Error: comando '%c' no reconocido. Escribe 'h' para ver la ayuda." COLOR_RESET "\n", *cmd);
    }
    return 0;
}

void editor_run(const char *ruta) {
    Editor ed;
    char linea[MAX_LINEA];
    int interactivo = isatty(STDIN_FILENO);

    editor_init(&ed);
    ed.huf = he_crear();                 /* gestor del trabajo Huffman en 2º plano (aditivo) */
    he_instalar_sigint();                /* SIGINT solo activa una bandera (aditivo) */
    printf(COLOR_TITLE "--- Editor de líneas (syscalls: open/read/write/lseek/ftruncate/close) ---" COLOR_RESET "\n");
    printf(COLOR_INFO "Escribe 'h' para ver los comandos, 'q' para salir." COLOR_RESET "\n");
    if (ruta != NULL) {
        ed_abrir(&ed, ruta);
    }

    while (1) {
        if (interactivo) {
            printf(COLOR_PROMPT "editor> " COLOR_RESET);
        }
        fflush(stdout);
        errno = 0;
        if (fgets(linea, sizeof(linea), stdin) == NULL) {
            /* SIGINT (Ctrl+C) interrumpe fgets sin SA_RESTART: si hay señal
             * pendiente, se traduce a cancelación del trabajo y se sigue en el
             * REPL (aditivo). Un EOF real (sin señal) termina como 'q'. */
            if (errno == EINTR && he_sigint_pendiente()) {
                clearerr(stdin);
                if (ed.huf != NULL && he_trabajo_activo(ed.huf)) {
                    /* Hay trabajo: Ctrl+C lo cancela de forma cooperativa y el
                     * REPL sigue vivo (aditivo). */
                    he_cancelar(ed.huf);
                    continue;
                }
                /* Sin trabajo activo: se conserva el comportamiento original
                 * (Ctrl+C termina el editor por la señal). Se restablece la
                 * disposición por defecto y se re-eleva la señal para morir con
                 * WIFSIGNALED(SIGINT), como esperan las pruebas existentes. */
                he_destruir(ed.huf);
                ed.huf = NULL;
                ed_cerrar(&ed);
                signal(SIGINT, SIG_DFL);
                raise(SIGINT);
            }
            break;   /* EOF (Ctrl+D o fin del script): se trata como 'q' */
        }
        if (strchr(linea, '\n') == NULL && strlen(linea) == sizeof(linea) - 1) {
            /* Línea más larga que el buffer: descartar el resto para no
             * interpretarlo como otro comando. */
            int c;
            while ((c = getchar()) != '\n' && c != EOF) {
            }
            printf(COLOR_ERROR "Error: comando demasiado largo (máx %d caracteres)." COLOR_RESET "\n", MAX_LINEA - 2);
            continue;
        }
        if (!interactivo) {
            /* Eco del comando cuando la entrada viene de un script/tubería */
            printf(COLOR_PROMPT "editor> " COLOR_RESET "%s", linea);
            if (strchr(linea, '\n') == NULL) {
                printf("\n");
            }
        }
        if (ed_ejecutar(&ed, linea)) {
            break;
        }
    }

    /* q/EOF con un trabajo activo: cancelación cooperativa + join de todos los
     * hilos + borrado del temporal, ANTES de cerrar y salir (aditivo). */
    he_destruir(ed.huf);
    ed.huf = NULL;

    int rc = ed_cerrar(&ed);
    printf(COLOR_INFO "Editor cerrado." COLOR_RESET "\n");
    fflush(stdout);
    exit(rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
