---
title: "Editor de texto por líneas sobre llamadas al sistema"
subtitle: "Sistemas Operativos (SO2026B) — Universidad EAFIT — Nivel acumulativo: Parejas"
author:
  - "Miguel Angel Colorado Castaño"
  - "[Integrante 2 — nombre y código]"
date: "Octubre 2026"
lang: es
---

# 1. Resumen

Se implementó en C un editor de texto por líneas (estilo `ed`) para Unix que manipula el
archivo **únicamente** con las llamadas al sistema `open(2)`, `read(2)`, `write(2)`,
`lseek(2)`, `ftruncate(2)` y `close(2)`. El contenido se mantiene en memoria como una
**lista doblemente enlazada de líneas** y cada modificación se persiste de inmediato en
disco desplazando bytes sobre el mismo descriptor. El editor ofrece siete comandos
(`o`, `p`, `a`, `d`, `i`, `s`, `q`), funciona como programa independiente
(`editor/editor`) y está integrado al shell educativo del curso como una **nueva categoría
`editor`** con el comando `e_edit`, que ejecuta el editor en un proceso hijo.

La batería de pruebas automatizada (`editor/tests/run_tests.sh`) cubre los siete comandos,
los casos borde pedidos (línea inexistente, archivo inexistente, archivo vacío), archivos
de varios bloques, ausencia de fugas de memoria (AddressSanitizer/LeakSanitizer) y la
integración con el shell.

# 2. Análisis del shell del profesor (Paso 1)

Fuente: `https://github.com/evalenciEAFIT/SO2026B`, carpeta `shell/` (la copia del
repositorio del equipo es idéntica al original, salvo finales de línea).

## 2.1 Arquitectura

| Elemento | Archivo | Rol |
|---|---|---|
| REPL | `main.c` | Lee con `fgets`, tokeniza con `parse_line` (respeta comillas), busca el comando y llama a su `handler`. Built-ins: `help`, `clear`, `exit`. |
| Registro de comandos | `main.c` (`Command commands[]`) | Tabla estática de `{name, category, usage, description, syscalls, handler}`. La ayuda (`help`, `help <categoría>`, `help <comando>`) se genera a partir de ella. |
| Trazado | `shell.h` | Macros `LOG_SYSCALL`, `LOG_SYSCALL_RESULT`, `LOG_SYSCALL_ERROR` que imitan `strace`. |
| Categorías | `cat_datos.c`, `cat_memoria.c`, `cat_monitoreo.c`, `cat_util.c` | Un archivo por categoría; los comandos llevan prefijo (`d_`, `m_`, `p_`) según su categoría. |

Agregar un comando consiste en: escribir el handler en un `cat_*.c`, declarar su prototipo
en `shell.h`, registrar una fila en `commands[]` y, si la categoría es nueva, añadirla a
la lista de `print_help`.

## 2.2 Características relevantes de las categorías existentes

* **`datos`** (`d_create`, `d_read`, `d_info`, `d_copy`): cada comando es **atómico y sin
  estado**: hace `open → read/write → close` dentro de la misma invocación. Ningún
  descriptor ni dato sobrevive entre comandos.
* **`monitoreo`** ya usa `fork(2)` + `waitpid(2)` para aislar trabajo en un hijo
  (`p_fork`, `p_exec`).
* Ninguna categoría usa `lseek(2)` ni `ftruncate(2)`.

## 2.3 Decisión: nueva categoría `editor`

**Se creó una categoría nueva (`editor`, archivo `cat_editor.c`, comando `e_edit [archivo]`)
en lugar de integrar el editor en `datos`.** Criterios:

| Criterio | `datos` | Editor | Consecuencia |
|---|---|---|---|
| Estado | Sin estado: cada comando abre y cierra | Con estado: un `fd` abierto y una lista en memoria que viven a lo largo de muchos comandos | Meterlo en `datos` rompería el contrato de la categoría o exigiría variables globales en el shell. |
| Ciclo de vida | Termina al retornar el handler | Termina con `q` → `close()` + `exit()` | `exit()` dentro del proceso del shell mataría al shell; hace falta otro proceso. |
| Espacio de nombres | Prefijo `d_` | Comandos de una letra `o p a d i s q` | `d` y `p` chocan visualmente con los prefijos `d_` y `p_`; el editor necesita su propio prompt. |
| Syscalls | `open/read/write/close/stat` | Además `lseek` y `ftruncate` | Es un tema nuevo (acceso aleatorio y redimensionamiento), digno de categoría propia en `help`. |

Alternativas evaluadas y descartadas:

1. **`d_edit` dentro de `datos`.** Contradice el modelo sin estado de la categoría.
2. **Siete comandos sueltos en el shell** (`e_o`, `e_p`, …) **con estado global.**
   Introduce estado mutable global en el shell, complica `help` y no permite que `q`
   haga `exit()` sin cerrar el shell.
3. **Elegida: categoría `editor` con `e_edit` que lanza una sub-sesión en un proceso hijo.**
   El shell hace `fork()`; el hijo ejecuta `editor_run()` (prompt `editor>`) y termina con
   `exit()`; el padre espera con `waitpid()` y reporta el código de salida. Ventajas: el
   `exit()` del requisito se cumple literalmente, el descriptor y el heap del editor
   desaparecen con el hijo (no pueden filtrarse al shell) y se reutilizan los conceptos de
   `monitoreo`. El editor se compila desde `../editor`, así que el mismo código sirve como
   programa independiente y dentro del shell, con la traza de syscalls activada.

## 2.4 Cambios realizados al shell

| Archivo | Cambio | Motivo |
|---|---|---|
| `cat_editor.c` (nuevo) | `cmd_e_edit`: `fork` → hijo `editor_run()`; padre `waitpid` | Integración. |
| `shell.h` | Prototipo de `cmd_e_edit` | Registro. |
| `main.c` | Fila `e_edit` en `commands[]`; categoría `editor` en `help` | Registro y ayuda. |
| `main.c` | `setvbuf(stdin, NULL, _IONBF, 0)` al inicio | Con stdin con buffer, si la entrada viene de una tubería el padre lee por adelantado varias líneas; el hijo de `fork` recibe una **copia** de ese buffer y, al volver, el padre re-ejecutaría los comandos del editor. Sin buffer, `fgets` nunca lee más allá del `\n`. Verificado por la prueba "comandos del editor no se re-ejecutan en el shell". |
| `Makefile` | Compila `cat_editor.c` y las fuentes de `../editor`; la ejecución pasa a `make run`; `clean` usa `$(ARCHSALIDA)` | El objetivo `all` original ejecutaba el shell (bloquea scripts) y `clean` usaba la variable inexistente `$(TARGET)`, por lo que no borraba el binario. |

# 3. Diseño del editor

## 3.1 Módulos

| Archivo | Contenido |
|---|---|
| `lista.h/.c` | Lista doblemente enlazada (`Linea`, `Lista`) sin dependencias de E/S. |
| `editor.h/.c` | Estado `Editor`, envoltorios de syscalls con trazado, algoritmos de desplazamiento en disco, los siete comandos y el REPL `editor_run()`. |
| `main.c` | Programa independiente: `./editor [-t] [archivo]` (`-t` activa la traza). |
| `../shell/cat_editor.c` | Integración con el shell. |

```c
typedef struct Linea { char *texto; size_t len; struct Linea *prev, *next; } Linea;
typedef struct { Linea *head, *tail; size_t count; } Lista;
typedef struct { int fd; char *ruta; Lista lineas; int nl_final; } Editor;
```

## 3.2 Modelo de persistencia (write-through)

La lista es la vista en memoria; el disco es la fuente de verdad. **Cada comando que
modifica (`a`, `d`, `i`) actualiza la lista y escribe el cambio en el archivo en la misma
operación**, de modo que se mantiene el invariante *archivo = serialización de la lista*
(cada línea seguida de `\n`, excepto posiblemente la última). El indicador `nl_final`
registra si el archivo está vacío o termina en `\n`, para no corromper archivos creados por
otros programas sin salto final. El desplazamiento en bytes de la línea *n* se calcula
desde la lista: `offset(n) = Σ_{k<n} (len_k + 1)`.

## 3.3 Comandos y syscalls

| Comando | Lista | Disco |
|---|---|---|
| `o <archivo>` | Construye una lista nueva | `open(O_RDWR\|O_CREAT, 0644)`, `read` en bloques hasta EOF. Si ya había un archivo, se cierra solo **después** de que el nuevo abrió bien. |
| `p [n]` | Recorre / obtiene el nodo *n* | — (solo memoria) |
| `a <texto>` | `lista_agregar` (O(1) por `tail`) | `lseek(SEEK_END)` + `write` (antepone `\n` si `nl_final == 0`). |
| `d <n>` | `lista_eliminar` | Rango `[a,b)` de la línea; bytes de `b` a EOF se copian a `a` (`lseek`+`read`+`lseek`+`write` por bloques de 4 KiB, de adelante hacia atrás) y `ftruncate(tam − (b−a))`. |
| `i <n> <texto>` | `lista_insertar` antes del nodo *n* | Bytes de `offset(n)` a EOF se copian `len+1` posiciones adelante, **de atrás hacia adelante** para no pisar datos pendientes; luego se escribe la línea en el hueco. `n = count+1` equivale a `a`. |
| `s <palabra>` | Recorre y cuenta ocurrencias con `strstr` | — |
| `q` (o EOF) | `lista_liberar` | `close`, luego `exit(EXIT_SUCCESS)` (o `EXIT_FAILURE` si `close` falla). |

Borrar la línea 2 de `l1\nl2\nl3\n` (9 bytes):

```
antes:  l 1 \n l 2 \n l 3 \n        a = 3, b = 6
copiar: [6,9) -> [3,6)            l 1 \n l 3 \n l 3 \n
ftruncate(9 - 3 = 6):             l 1 \n l 3 \n
```

Traza real (`./editor -t`) de `i 2 medio` sobre `uno\ndos\ntres\n`:

```
[syscall] lseek(3, 0, SEEK_END) ... = 13
[syscall] lseek(3, 4, SEEK_SET) ... = 4
[syscall] read(3, buf, 9) ... = 9
[syscall] lseek(3, 10, SEEK_SET) ... = 10
[syscall] write(3, buf, 9) ... = 9
[syscall] lseek(3, 4, SEEK_SET) ... = 4
[syscall] write(3, buf, 6) ... = 6
```

# 4. Estructura de datos: por qué lista doblemente enlazada

| Operación | Lista doble (elegida) | Arreglo dinámico de punteros | Tabla hash (n → línea) |
|---|---|---|---|
| `a` (append) | **O(1)** con `tail` | O(1) amortizado, `realloc` ocasional | O(1) |
| Localizar línea *n* | O(min(n, count−n)): se recorre desde el extremo más cercano | O(1) | O(1) |
| `i` en medio, una vez localizada | **O(1)** reenlazar | O(n) `memmove` | **O(n): hay que re-indexar todas las claves posteriores** |
| `d` en medio, una vez localizada | **O(1)** desenlazar | O(n) `memmove` | O(n) re-indexar |
| `s` (subcadena) | O(tamaño del texto) | O(tamaño del texto) | O(tamaño del texto): el hash no ayuda a buscar subcadenas |
| Memoria extra por línea | 2 punteros | 1 puntero + holgura | cubetas + claves + colisiones |

**Hash descartada.** La única clave natural es el número de línea, y ese número cambia
para todas las líneas posteriores en cada `i` y cada `d`: no hay claves estables que
justifiquen el costo, y la búsqueda `s` es por subcadena, no por clave exacta.

**Lista vs. arreglo.** El arreglo gana en acceso aleatorio, pero la lista hace las
modificaciones locales sin mover memoria ni reasignar bloques, crece nodo a nodo sin
`realloc` y refleja directamente el modelo "insertar/borrar una línea desplaza a las
demás". El doble enlace permite insertar antes de un nodo y desenlazar en O(1), y recorrer
desde `tail` cuando *n* está en la segunda mitad. En la práctica el costo dominante de
`i`/`d` es el desplazamiento de bytes en disco, O(bytes después de la línea), que es
igual con cualquier estructura en memoria.

# 5. Manejo de errores

* Todo retorno de syscall se verifica (`open == -1`, `read == -1`, `lseek == (off_t)-1`,
  `ftruncate == -1`, `close == -1`) y se reporta con `perror()`; con traza activa también
  se muestra `= -1 (strerror)`.
* `read` y `write` se reintentan ante `EINTR`; `write` se repite hasta escribir todo el
  buffer (las escrituras parciales son legales). Un EOF inesperado al desplazar se reporta
  como `EIO`.
* `a` deshace el cambio en la lista y recorta el archivo a su tamaño previo con
  `ftruncate` si la escritura falla; `i` deshace el nodo insertado.
* `o` que falla conserva el archivo abierto anteriormente.
* Errores del usuario (línea inexistente, número inválido como `0`, `abc` o `2x`, comando
  desconocido, argumentos faltantes) se informan sin terminar el editor.

# 6. Restricción de E/S y gestión de memoria

* El archivo nunca se toca con `fopen/fread/fwrite/fclose`. La consola usa `printf` y
  `fgets`. Se verifica automáticamente con `nm -u editor`: el binario no importa ninguno de
  esos cuatro símbolos.
* Toda memoria (nodos, buffers de línea, ruta, buffer de lectura) se libera en `q`, al
  cambiar de archivo con `o` y en los caminos de error. Se verifica con un binario
  instrumentado (`make editor_asan`, `-fsanitize=address,undefined`) que ejecuta una sesión
  con todos los comandos: LeakSanitizer no reporta fugas. Se comprobó por separado que
  LeakSanitizer sí detecta fugas en el entorno de prueba (WSL2 Ubuntu 24.04, GCC 13.3).

# 7. Pruebas

`editor/tests/run_tests.sh` (o `make test`) compila todo y ejecuta 73 verificaciones
agrupadas en: compilación y símbolos; `o` (archivo inexistente → creado, directorio
inexistente → `ENOENT`, directorio → `EISDIR`, apertura por argv, cambio de archivo, fallo
que conserva el anterior); archivo vacío (`p`, `p 1`, `d 1`, `s`, `i 1`); `a` (líneas
vacías, espacios, archivo sin salto final); `p` (todo, *n*, inexistente, inválidos);
`d` (primera, media, última, inexistente, última sin salto final, única línea); `i`
(inicio, medio, `count+1`, fuera de rango, inválidos); archivo de 13 903 bytes con
varios bloques; `s` (varias líneas, varias ocurrencias, mayúsculas, sin resultados, sin
palabra); `q` (código de salida, persistencia, nada se ejecuta después); fugas de memoria;
integración con el shell; integración shell -> editor -> compresor Huffman (`c`, `x`, `w`, ida y vuelta con `cmp`). El contenido en disco se compara **byte a byte** con `cmp`.

Resultado en WSL2 Ubuntu 24.04 / GCC 13.3.0 (Parcial 2, con el compresor integrado): **73 PASS, 0 FAIL**. Las pruebas propias del compresor están en `tests/run_tests_huf.sh` (ver `CONCURRENCIA.md`).

# 8. Compilación y uso

```bash
cd editor && make            # binario ./editor
./editor -t notas.txt        # con traza de syscalls
make test                    # batería de pruebas
cd ../shell && make && ./eafitOS
eafitOS> help editor
eafitOS> e_edit notas.txt
```

# 9. Limitaciones y trabajo futuro

* El archivo completo se carga en memoria; `i` y `d` cuestan O(bytes posteriores) en
  disco, inherente a un archivo plano.
* Sin bloqueo de archivo: si otro proceso modifica el archivo mientras está abierto, la
  lista queda desincronizada (se detecta parcialmente: un EOF inesperado aborta el
  desplazamiento).
* `s` distingue mayúsculas de minúsculas y busca subcadenas, no palabras completas.
* Los comandos se leen con un buffer de 4096 caracteres; una línea más larga se rechaza.

# 10. Distribución del trabajo (Parejas)

| Integrante | Responsabilidades |
|---|---|
| Miguel Angel Colorado Castaño | [p. ej. lista enlazada, comandos `o`, `p`, `a`, `q`, integración con el shell] |
| [Integrante 2] | [p. ej. desplazamiento en disco, comandos `d`, `i`, `s`, pruebas, reporte] |
