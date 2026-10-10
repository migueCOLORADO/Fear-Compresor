#ifndef LISTA_H
#define LISTA_H

#include <stddef.h>

/**
 * ====================================================================================
 * LISTA DOBLEMENTE ENLAZADA DE LÍNEAS
 * ====================================================================================
 * Representación en memoria del archivo abierto en el editor. Cada nodo es una
 * línea del archivo SIN el carácter '\n' final. El texto se guarda en un buffer
 * reservado con malloc() y terminado en '\0' (para poder usar strstr en búsquedas),
 * pero la longitud real se lleva en 'len' para no recorrer la cadena cada vez.
 *
 * Numeración: las líneas se numeran desde 1 (como en ed/vi), nunca desde 0.
 */
typedef struct Linea {
    char *texto;            /* Buffer malloc'd con el contenido de la línea ('\0' al final) */
    size_t len;             /* Bytes de la línea, sin contar '\n' ni '\0' */
    struct Linea *prev;     /* Línea anterior (NULL si es la primera) */
    struct Linea *next;     /* Línea siguiente (NULL si es la última) */
} Linea;

typedef struct {
    Linea *head;            /* Primera línea del archivo */
    Linea *tail;            /* Última línea: permite append en O(1) */
    size_t count;           /* Número de líneas: permite validar 'n' en O(1) */
} Lista;

/* Deja la lista vacía (no libera nada: usar lista_liberar antes si tenía nodos). */
void lista_init(Lista *l);

/* Inserta una copia de texto[0..len) en la posición pos (1..count+1).
 * pos == count+1 equivale a append. Retorna 0 en éxito, -1 si pos es inválida
 * o malloc falla (en ese caso la lista queda intacta). */
int lista_insertar(Lista *l, size_t pos, const char *texto, size_t len);

/* Append al final: atajo de lista_insertar(l, count+1, ...). O(1). */
int lista_agregar(Lista *l, const char *texto, size_t len);

/* Retorna el nodo de la línea n (1..count) o NULL si no existe.
 * Recorre desde el extremo más cercano (head o tail): O(min(n, count-n)). */
Linea *lista_obtener(const Lista *l, size_t n);

/* Desenlaza y libera la línea n. Retorna 0 en éxito, -1 si n no existe. */
int lista_eliminar(Lista *l, size_t n);

/* Offset en bytes (dentro del archivo) donde empieza la línea n (1..count+1).
 * Toda línea anterior a n ocupa len + 1 bytes (contenido + '\n'). */
size_t lista_offset(const Lista *l, size_t n);

/* Libera todos los nodos y sus buffers. Deja la lista vacía y reutilizable. */
void lista_liberar(Lista *l);

#endif /* LISTA_H */
