CC = gcc
EDITOR_DIR = ../editor
CFLAGS = -Wall -Wextra -std=gnu99 -g -D_GNU_SOURCE -I$(EDITOR_DIR)
# El editor incorpora el compresor Huffman concurrente (hilos POSIX): -pthread.
LDFLAGS = -pthread
ARCHSALIDA = eafitOS
SRCS = main.c cat_datos.c cat_memoria.c cat_monitoreo.c cat_util.c cat_editor.c
OBJS = $(SRCS:.c=.o)
# Fuentes del editor (categoría 'editor'): se compilan aquí con nombre propio
# para no mezclar sus .o con los del Makefile de ../editor. Se agregan los
# módulos nuevos del compresor Huffman: huffman, hilos y el orquestador.
EDITOR_OBJS = editor_core.o editor_lista.o editor_huffman.o editor_hilos.o editor_huf_editor.o

all: $(ARCHSALIDA)

$(ARCHSALIDA): $(OBJS) $(EDITOR_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(ARCHSALIDA) $(OBJS) $(EDITOR_OBJS)

# Antes 'all' ejecutaba el shell tras compilar; eso bloquea 'make' en scripts
# de prueba, así que la ejecución quedó en su propio objetivo.
run: $(ARCHSALIDA)
	./$(ARCHSALIDA)

%.o: %.c shell.h
	$(CC) $(CFLAGS) -c $< -o $@

EDITOR_HDRS = $(EDITOR_DIR)/editor.h $(EDITOR_DIR)/lista.h $(EDITOR_DIR)/huffman.h \
              $(EDITOR_DIR)/hilos.h $(EDITOR_DIR)/huf_editor.h

cat_editor.o: cat_editor.c shell.h $(EDITOR_HDRS)
	$(CC) $(CFLAGS) -c $< -o $@

editor_core.o: $(EDITOR_DIR)/editor.c $(EDITOR_HDRS)
	$(CC) $(CFLAGS) -c $< -o $@

editor_lista.o: $(EDITOR_DIR)/lista.c $(EDITOR_DIR)/lista.h
	$(CC) $(CFLAGS) -c $< -o $@

editor_huffman.o: $(EDITOR_DIR)/huffman.c $(EDITOR_DIR)/huffman.h $(EDITOR_DIR)/hilos.h
	$(CC) $(CFLAGS) -c $< -o $@

editor_hilos.o: $(EDITOR_DIR)/hilos.c $(EDITOR_DIR)/hilos.h
	$(CC) $(CFLAGS) -c $< -o $@

editor_huf_editor.o: $(EDITOR_DIR)/huf_editor.c $(EDITOR_DIR)/huf_editor.h $(EDITOR_DIR)/huffman.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(ARCHSALIDA) $(OBJS) $(EDITOR_OBJS)

.PHONY: all run clean
