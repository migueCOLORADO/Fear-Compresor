CC      = gcc
CFLAGS  = -Wall -Wextra -Werror -std=gnu99 -g -D_GNU_SOURCE
# El módulo Huffman concurrente usa hilos POSIX: -pthread aplica a toda la
# compilación y enlazado del editor.
LDFLAGS = -pthread
TARGET  = editor
SRCS    = main.c editor.c lista.c huffman.c hilos.c huf_editor.c
OBJS    = $(SRCS:.c=.o)
HDRS    = editor.h lista.h huffman.h hilos.h huf_editor.h

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c $< -o $@

# Binario instrumentado con AddressSanitizer/UBSan/LeakSanitizer (lo usa
# tests/run_tests.sh para verificar que 'q' no deja fugas de memoria). No forma
# parte de 'all'.
editor_asan: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer $(LDFLAGS) -o $@ $(SRCS)

# Binario instrumentado con ThreadSanitizer para cazar carreras de datos entre
# los hilos del compresor. No se puede combinar con AddressSanitizer: por eso es
# un binario aparte.
editor_tsan: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -fsanitize=thread -fno-omit-frame-pointer $(LDFLAGS) -o $@ $(SRCS)

# CLI de prueba del módulo huf_ (lo usa tests/run_tests_huf.sh). No es parte del
# editor ni del shell.
huf_cli: huffman.c hilos.c tests/huf_cli.c $(HDRS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ huffman.c hilos.c tests/huf_cli.c

# Editor con worker artificialmente lento (-DHUF_LENTO): mantiene vivo el trabajo
# para las pruebas de carrera y cancelación. SOLO para pruebas.
editor_lento: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -DHUF_LENTO=5000 $(LDFLAGS) -o $@ $(SRCS)

# Igual que editor_lento pero instrumentado con ThreadSanitizer: las secciones de
# carrera y cancelación de la suite se ejecutan también con este binario.
editor_lento_tsan: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -DHUF_LENTO=5000 -fsanitize=thread -fno-omit-frame-pointer $(LDFLAGS) -o $@ $(SRCS)

huf_cli_asan: huffman.c hilos.c tests/huf_cli.c $(HDRS)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer $(LDFLAGS) -o $@ huffman.c hilos.c tests/huf_cli.c

huf_cli_tsan: huffman.c hilos.c tests/huf_cli.c $(HDRS)
	$(CC) $(CFLAGS) -fsanitize=thread -fno-omit-frame-pointer $(LDFLAGS) -o $@ huffman.c hilos.c tests/huf_cli.c

test: $(TARGET)
	bash tests/run_tests.sh

# Pruebas del módulo Huffman (corrección, concurrencia, robustez). Se construye
# también editor_lento_tsan para repetir las secciones de carrera y cancelación
# bajo ThreadSanitizer.
test-huf: huf_cli editor editor_lento editor_lento_tsan
	bash tests/run_tests_huf.sh

# Suite Huffman bajo AddressSanitizer/UBSan/LeakSanitizer. HUF_CLI absoluto
# porque el script se mueve a tests/tmp_huf antes de ejecutar el CLI.
test-asan: huf_cli_asan editor editor_lento editor_lento_tsan
	HUF_CLI=$(CURDIR)/huf_cli_asan bash tests/run_tests_huf.sh

# Suite Huffman bajo ThreadSanitizer.
test-tsan: huf_cli_tsan editor editor_lento editor_lento_tsan
	HUF_CLI=$(CURDIR)/huf_cli_tsan bash tests/run_tests_huf.sh

clean:
	rm -f $(TARGET) editor_asan editor_tsan editor_lento editor_lento_tsan huf_cli huf_cli_asan huf_cli_tsan $(OBJS)
	rm -rf tests/tmp tests/tmp_huf

.PHONY: all clean test test-huf test-asan test-tsan
