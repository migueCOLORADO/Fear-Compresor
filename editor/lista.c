#include "lista.h"
#include <stdlib.h>
#include <string.h>

void lista_init(Lista *l) {
    l->head = NULL;
    l->tail = NULL;
    l->count = 0;
}

/* Crea un nodo suelto con una copia del texto. NULL si malloc falla. */
static Linea *nodo_nuevo(const char *texto, size_t len) {
    Linea *nodo = malloc(sizeof(Linea));
    if (nodo == NULL) {
        return NULL;
    }
    nodo->texto = malloc(len + 1);
    if (nodo->texto == NULL) {
        free(nodo);
        return NULL;
    }
    if (len > 0) {
        memcpy(nodo->texto, texto, len);
    }
    nodo->texto[len] = '\0';
    nodo->len = len;
    nodo->prev = NULL;
    nodo->next = NULL;
    return nodo;
}

Linea *lista_obtener(const Lista *l, size_t n) {
    if (n == 0 || n > l->count) {
        return NULL;
    }
    Linea *actual;
    if (n <= l->count / 2 + 1) {
        /* Más cerca del inicio: avanzar con next */
        actual = l->head;
        for (size_t i = 1; i < n; i++) {
            actual = actual->next;
        }
    } else {
        /* Más cerca del final: retroceder con prev */
        actual = l->tail;
        for (size_t i = l->count; i > n; i--) {
            actual = actual->prev;
        }
    }
    return actual;
}

int lista_insertar(Lista *l, size_t pos, const char *texto, size_t len) {
    if (pos == 0 || pos > l->count + 1) {
        return -1;
    }
    Linea *nodo = nodo_nuevo(texto, len);
    if (nodo == NULL) {
        return -1;
    }

    if (pos == l->count + 1) {
        /* Enlazar al final */
        nodo->prev = l->tail;
        if (l->tail != NULL) {
            l->tail->next = nodo;
        } else {
            l->head = nodo;      /* Lista vacía: el nodo es head y tail */
        }
        l->tail = nodo;
    } else {
        /* Enlazar ANTES de la línea que hoy ocupa 'pos': esa línea y todas
         * las siguientes quedan desplazadas una posición hacia abajo. */
        Linea *sig = lista_obtener(l, pos);
        nodo->next = sig;
        nodo->prev = sig->prev;
        if (sig->prev != NULL) {
            sig->prev->next = nodo;
        } else {
            l->head = nodo;
        }
        sig->prev = nodo;
    }
    l->count++;
    return 0;
}

int lista_agregar(Lista *l, const char *texto, size_t len) {
    return lista_insertar(l, l->count + 1, texto, len);
}

int lista_eliminar(Lista *l, size_t n) {
    Linea *nodo = lista_obtener(l, n);
    if (nodo == NULL) {
        return -1;
    }
    if (nodo->prev != NULL) {
        nodo->prev->next = nodo->next;
    } else {
        l->head = nodo->next;
    }
    if (nodo->next != NULL) {
        nodo->next->prev = nodo->prev;
    } else {
        l->tail = nodo->prev;
    }
    free(nodo->texto);
    free(nodo);
    l->count--;
    return 0;
}

size_t lista_offset(const Lista *l, size_t n) {
    size_t off = 0;
    size_t i = 1;
    for (Linea *actual = l->head; actual != NULL && i < n; actual = actual->next, i++) {
        off += actual->len + 1;  /* contenido + '\n' */
    }
    return off;
}

void lista_liberar(Lista *l) {
    Linea *actual = l->head;
    while (actual != NULL) {
        Linea *sig = actual->next;
        free(actual->texto);
        free(actual);
        actual = sig;
    }
    lista_init(l);
}
