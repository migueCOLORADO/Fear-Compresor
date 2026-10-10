#ifndef EDITOR_H
#define EDITOR_H

#include "lista.h"
#include "huf_editor.h"   /* gestor del trabajo Huffman en segundo plano (aditivo) */

/**
 * ====================================================================================
 * EDITOR DE TEXTO DE LÍNEA (estilo ed) SOBRE LLAMADAS AL SISTEMA
 * ====================================================================================
 * El archivo se manipula EXCLUSIVAMENTE con open(2), read(2), write(2), lseek(2),
 * ftruncate(2) y close(2). No se usa fopen/fread/fwrite/fclose para el archivo.
 * La consola (comandos del usuario y mensajes) sí usa stdio (printf/fgets).
 *
 * Modelo: la lista doblemente enlazada es la vista en memoria del archivo, y cada
 * comando que modifica (a, d, i) se persiste INMEDIATAMENTE en disco sobre el mismo
 * descriptor, desplazando bytes con lseek/read/write y ajustando el tamaño con
 * ftruncate. Así el archivo en disco y la lista siempre coinciden.
 *
 * Comandos:
 *   o <archivo>      open(O_RDWR|O_CREAT) y carga en la lista
 *   p [n]            imprime la línea n, o todo el archivo
 *   a <texto>        agrega una línea al final
 *   d <n>            borra la línea n
 *   i <n> <texto>    inserta texto como nueva línea n, desplazando el resto
 *   s <palabra>      busca y reporta las líneas donde aparece
 *   q                close() + liberar la lista + exit()
 */

/* Colores ANSI (mismos valores que shell.h del shell educativo). */
#ifndef COLOR_RESET
#define COLOR_RESET     "\033[0m"
#define COLOR_PROMPT    "\033[1;36m"
#define COLOR_SYSCALL   "\033[1;35m"
#define COLOR_PARAM     "\033[0;33m"
#define COLOR_RESULT    "\033[1;32m"
#define COLOR_ERROR     "\033[1;31m"
#define COLOR_INFO      "\033[0;90m"
#define COLOR_TITLE     "\033[1;97m"
#endif

typedef struct {
    int fd;            /* Descriptor del archivo abierto, -1 si no hay archivo */
    char *ruta;        /* Copia malloc'd de la ruta abierta (NULL si no hay) */
    Lista lineas;      /* Contenido del archivo, una línea por nodo */
    int nl_final;      /* 1 si el archivo está vacío o termina en '\n' */
    HufEditor *huf;    /* Gestor del trabajo Huffman en segundo plano (NULL si no se creó) */
} Editor;

/* Si es distinto de 0, cada syscall sobre el archivo se traza estilo strace. */
extern int ed_traza;

void editor_init(Editor *ed);

/* Cada comando retorna 0 en éxito y -1 en error (ya reportado al usuario). */
int ed_abrir(Editor *ed, const char *ruta);
int ed_imprimir(Editor *ed, const char *arg_n);
int ed_agregar(Editor *ed, const char *texto);
int ed_borrar(Editor *ed, const char *arg_n);
int ed_insertar(Editor *ed, const char *args);
int ed_buscar(Editor *ed, const char *palabra);

/* Cierra el descriptor y libera toda la memoria. No termina el proceso. */
int ed_cerrar(Editor *ed);

/* Interpreta una línea de comando. Retorna 1 si el usuario pidió salir (q). */
int ed_ejecutar(Editor *ed, char *linea);

/* REPL del editor. Si ruta != NULL la abre primero. Termina SIEMPRE con exit():
 * EXIT_SUCCESS si close() fue exitoso, EXIT_FAILURE si no. Nunca retorna. */
void editor_run(const char *ruta) __attribute__((noreturn));

#endif /* EDITOR_H */
