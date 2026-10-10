# Fear-Compresor | Sistemas Operativos

Parcial 2 de Sistemas Operativos SO2026B: **compresor/descompresor Huffman concurrente integrado al editor de texto** del Parcial 1. El editor por líneas corre dentro del shell educativo `eafitOS` y ahora puede comprimir y descomprimir el archivo abierto en segundo plano, con un pool de hilos POSIX, sin bloquear el prompt.

## Índice
1. [Categorías preexistentes del shell](#categorías-preexistentes-del-shell)
   1. [Datos y archivos (`datos`)](#datos-y-archivos-datos)
   2. [Memoria (`memoria`)](#memoria-memoria)
   3. [Monitoreo y procesos (`monitoreo`)](#monitoreo-y-procesos-monitoreo)
   4. [Utilidades (`utilidades`)](#utilidades-utilidades)
   5. [Ayuda interactiva](#ayuda-interactiva)
2. [Parte 1 de la entrega: Integración con el shell](#parte-1-de-la-entrega-integración-con-el-shell)
   1. [Pasos de ejecución](#pasos-de-ejecución)
3. [Parte 2 de la entrega: Editor de texto](#parte-2-de-la-entrega-editor-de-texto)
   1. [Pasos de ejecución](#pasos-de-ejecución-1)
4. [Parte 3 de la entrega: Compresor Huffman concurrente](#parte-3-de-la-entrega-compresor-huffman-concurrente)
   1. [Comandos en el editor](#comandos-en-el-editor)
   2. [Fases del algoritmo](#fases-del-algoritmo)
   3. [Sincronización](#sincronización)
   4. [Carrera con la edición](#carrera-con-la-edición)
   5. [Formato del archivo `.huf`](#formato-del-archivo-huf)
   6. [Pasos de ejecución](#pasos-de-ejecución-2)
5. [Parte 4 de la entrega: Pruebas](#parte-4-de-la-entrega-pruebas)
   1. [Pasos de ejecución](#pasos-de-ejecución-3)
6. [Rendimiento: secuencial vs concurrente](#rendimiento-secuencial-vs-concurrente)
7. [Estructura del repositorio](#estructura-del-repositorio)
8. [Autores](#autores)
9. [Información del curso](#información-del-curso)

## Categorías preexistentes del shell

El shell educativo (`shell/`, binario `eafitOS`) clasifica sus comandos por categoría. Cada comando muestra en magenta, al estilo de `strace`, la llamada al sistema que ejecuta, sus parámetros y el valor que retorna el kernel (macros `LOG_SYSCALL*` de `shell/shell.h`). Estas cuatro categorías vienen del shell del profesor; el editor (Parte 1) agrega la quinta, `editor`.

### Datos y archivos (`datos`)

Archivo: `shell/cat_datos.c`. Cada comando es atómico: abre, opera y cierra en la misma invocación.

| Comando | Acción | Syscalls |
| --- | --- | --- |
| `d_create <archivo> "<texto>"` | Crea o vacía el archivo y escribe el texto | `open(O_WRONLY\|O_CREAT\|O_TRUNC, 0644)`, `write`, `close` |
| `d_read <archivo>` | Lee hasta 1023 bytes (buffer de 1024) y los imprime | `open(O_RDONLY)`, `read`, `close` |
| `d_info <archivo>` | Muestra inodo, permisos en octal, UID del dueño, tamaño, tamaño de bloque y fecha de modificación | `stat` |
| `d_copy <origen> <destino>` | Copia en bloques de 512 bytes hasta EOF | `open` ×2, `read`/`write` en bucle, `close` ×2 |

```
eafitOS> d_create mi_archivo.txt "Hola SO"
[syscall] open("mi_archivo.txt", O_WRONLY|O_CREAT|O_TRUNC, 0644) ... = 3
[syscall] write(3, "Hola SO", 7) ... = 7
[syscall] close(3) ... = 0
Archivo 'mi_archivo.txt' creado exitosamente con 7 bytes.
```

### Memoria (`memoria`)

Archivo: `shell/cat_memoria.c`.

| Comando | Acción | Syscalls |
| --- | --- | --- |
| `m_sbrk <incremento_bytes>` | Lee el program break (`sbrk(0)`), lo mueve según el incremento (puede ser negativo) y lo vuelve a leer para mostrar el cambio del heap | `sbrk` / `brk` |
| `m_mmap <tamaño_bytes>` | Mapea memoria anónima y privada, escribe un patrón en ella y la libera | `mmap(NULL, n, PROT_READ\|PROT_WRITE, MAP_PRIVATE\|MAP_ANONYMOUS, -1, 0)`, `munmap` |
| `m_info` | Filtra `/proc/self/status`: `VmPeak`, `VmSize`, `VmLck`, `VmHWM`, `VmRSS`, `VmData`, `VmStk`, `VmExe`, `VmLib`, `VmPTE` | Lectura de `/proc/self/status` |

### Monitoreo y procesos (`monitoreo`)

Archivo: `shell/cat_monitoreo.c`.

| Comando | Acción | Syscalls |
| --- | --- | --- |
| `p_fork` | El hijo imprime su PID y el de su padre, duerme 2 s y sale con `exit(42)`; el padre espera y muestra el código con `WIFEXITED`/`WEXITSTATUS` | `fork`, `getpid`, `getppid`, `waitpid` |
| `p_exec <comando> [args...]` | El hijo reemplaza su imagen con el programa; si `execvp` falla sale con `127` | `fork`, `execvp`, `waitpid` |
| `p_kill <pid> <numero_señal>` | Envía la señal al proceso | `kill` |
| `p_monitor` | Tiempos de CPU de usuario y de sistema, RSS máximo, fallos de página y cambios de contexto voluntarios e involuntarios del shell | `getrusage(RUSAGE_SELF)` |

### Utilidades (`utilidades`)

Archivo: `shell/cat_util.c`.

| Comando | Acción | Syscalls |
| --- | --- | --- |
| `saludar` | Saluda al usuario actual por su nombre | `getuid` + `getpwuid(3)` |
| `despedir` | Despide al usuario actual por su nombre | `getuid` + `getpwuid(3)` |
| `hora` | Hora del sistema `HH:MM:SS` | `time` + `localtime(3)`/`strftime(3)` |
| `fecha` | Fecha del sistema `YYYY-MM-DD` | `time` + `localtime(3)`/`strftime(3)` |

### Ayuda interactiva

* `help`: bienvenida y lista de categorías.
* `help <categoria>`: comandos de `datos`, `memoria`, `monitoreo`, `utilidades` o `editor`.
* `help <comando>`: descripción, uso y syscalls de un comando.
* `clear` limpia la pantalla; `exit` (o `Ctrl+D`) cierra el shell.
* Un comando desconocido sugiere ejecutarlo como binario externo con `p_exec`.

## Parte 1 de la entrega: Integración con el shell

El shell educativo del curso (`shell/`) organiza sus comandos en categorías (`datos`, `memoria`, `monitoreo`, `utilidades`), cada una en su propio archivo `cat_*.c` y registrada en la tabla `commands[]` de `main.c`. El editor se integró como una **categoría nueva, `editor`**, con un único comando:

```
eafitOS> e_edit [archivo]
```

### Por qué una categoría nueva y no `datos`

| Criterio | `datos` | Editor |
| --- | --- | --- |
| Estado | Cada comando hace `open → read/write → close` y termina | Mantiene un `fd` abierto y una lista en memoria durante toda la sesión |
| Ciclo de vida | Termina al retornar el handler | Termina con `q` → `close()` + `exit()` |
| Nombres | Prefijo `d_` | Comandos de una letra (`o p a d i s q`); `d` y `p` chocarían con los prefijos `d_` y `p_` |
| Syscalls | `open`, `read`, `write`, `close`, `stat` | Además `lseek` y `ftruncate` |

### Funcionamiento

**Proceso hijo**

`e_edit` hace `fork()`. El hijo ejecuta el editor con su propio prompt `editor>`; el padre espera con `waitpid()` y reporta el código de salida. Así el `exit()` que exige el comando `q` termina solo al editor, y su descriptor y su heap desaparecen con el hijo sin afectar al shell.

```c
pid_t pid = fork();
if (pid == 0) {
    ed_traza = 1;
    editor_run(argc == 2 ? argv[1] : NULL);   /* nunca retorna: q -> exit() */
}
waitpid(pid, &status, 0);
```

**Ctrl+C**

Mientras el editor corre, el shell ignora `SIGINT`; el hijo restaura la acción original. Sin un trabajo Huffman activo, una interrupción termina solo al editor, y como cada comando ya quedó persistido en disco, no se pierde nada. Con un trabajo de compresión o descompresión en curso, `Ctrl+C` cancela ese trabajo y el editor sigue abierto (ver [Parte 3](#parte-3-de-la-entrega-compresor-huffman-concurrente)).

**stdin sin buffer**

Al inicio del shell se ejecuta `setvbuf(stdin, NULL, _IONBF, 0)`. Si la entrada viene de una tubería o un script, el buffer de stdio podría guardar líneas leídas por adelantado; el hijo recibe una copia de ese buffer y, al volver, el padre re-ejecutaría los comandos del editor. Sin buffer, `fgets` nunca lee más allá del `\n`.

### Cambios al shell

| Archivo | Cambio |
| --- | --- |
| `shell/cat_editor.c` | Nuevo: comando `e_edit` |
| `shell/shell.h` | Prototipo de `cmd_e_edit` |
| `shell/main.c` | Registro de `e_edit` (su descripción y syscalls incluyen el compresor), categoría `editor` en `help`, `setvbuf` de stdin |
| `shell/Makefile` | Compila `cat_editor.c` y las fuentes de `../editor` (editor, lista y los módulos del compresor `huffman.c`, `hilos.c`, `huf_editor.c`), enlaza con `-pthread`; `all` ya no ejecuta el binario (eso pasa a `make run`); `clean` borraba `$(TARGET)`, que no existía |

### Pasos de ejecución

1. Compilar el shell: `cd shell && make`.
2. Ejecutar: `./eafitOS` (o `make run`).
3. Escribir `help editor` para ver la categoría.
4. Escribir `e_edit notas.txt` para abrir el editor; `q` regresa al shell.

## Parte 2 de la entrega: Editor de texto

Editor estilo `ed` escrito en C. El archivo se manipula **únicamente** con `open`, `read`, `write`, `lseek`, `ftruncate` y `close`; no se usa `fopen`/`fread`/`fwrite`/`fclose`. La consola sí usa `printf` y `fgets`.

### Comandos

| Comando | Acción | Syscalls sobre el archivo |
| --- | --- | --- |
| `o <archivo>` | Abre o crea el archivo y lo carga en memoria | `open(O_RDWR\|O_CREAT)`, `read` |
| `p [n]` | Imprime la línea `n`, o todo el archivo | — |
| `a <texto>` | Agrega una línea al final | `lseek(SEEK_END)`, `write` |
| `d <n>` | Borra la línea `n` | `lseek`, `read`, `write`, `ftruncate` |
| `i <n> <texto>` | Inserta en la línea `n`, desplazando el resto | `lseek`, `read`, `write` |
| `s <palabra>` | Reporta las líneas donde aparece y cuántas veces | — |
| `c`, `x`, `t`, `k`, `w` | Compresor Huffman en segundo plano (ver [Parte 3](#parte-3-de-la-entrega-compresor-huffman-concurrente)) | `open`, `fstat`, `pread`, `pwrite`, `ftruncate`, `rename`, `unlink` |
| `q` | Cierra, libera la lista y termina | `close`, `exit` |
| `h` | Ayuda | — |

### Estructura de datos

Lista doblemente enlazada de líneas. Cada nodo guarda un buffer reservado con `malloc`, su longitud y los punteros `prev` / `next`:

```c
typedef struct Linea {
    char *texto;
    size_t len;
    struct Linea *prev, *next;
} Linea;
```

| Variable | Uso |
| --- | --- |
| `fd` | Descriptor del archivo abierto (`-1` si no hay) |
| `lineas` | Lista con `head`, `tail` (append en O(1)) y `count` (validar `n` en O(1)) |
| `nl_final` | Indica si el archivo está vacío o termina en `\n`, para no pegar texto a la última línea |

**¿Por qué no una tabla hash?** La única clave natural es el número de línea, y ese número cambia para todas las líneas siguientes en cada `i` y cada `d`. No hay claves estables que la justifiquen, y la búsqueda `s` es por subcadena, no por clave exacta.

**¿Por qué no un arreglo?** Insertar o borrar en medio obliga a mover memoria (`memmove`) y a reasignar (`realloc`). En la lista, una vez ubicado el nodo, insertar o desenlazar es O(1), y la búsqueda de la línea `n` empieza desde el extremo más cercano (`head` o `tail`).

### Funcionamiento

**Persistencia inmediata**

Cada comando que modifica (`a`, `d`, `i`) actualiza la lista y escribe el cambio en disco en la misma operación. El desplazamiento en bytes de la línea `n` se calcula desde la lista:

offset(n) = suma de (len + 1) de las líneas 1 .. n-1

**Borrar (`d`)**

Los bytes que siguen a la línea se copian hacia atrás en bloques de 4 KiB, de adelante hacia atrás, y luego se recorta el archivo:

```
antes:     l 1 \n l 2 \n l 3 \n        (borrar línea 2: bytes [3, 6))
copiar:    [6, 9) -> [3, 6)
ftruncate: l 1 \n l 3 \n
```

**Insertar (`i`)**

Los bytes desde la línea `n` se copian `len + 1` posiciones hacia adelante, **de atrás hacia adelante** para no sobrescribir datos que aún no se han leído, y luego se escribe la línea en el hueco. Traza real de `i 2 medio` sobre `uno\ndos\ntres\n`:

```
[syscall] lseek(3, 0, SEEK_END) ... = 13
[syscall] lseek(3, 4, SEEK_SET) ... = 4
[syscall] read(3, buf, 9) ... = 9
[syscall] lseek(3, 10, SEEK_SET) ... = 10
[syscall] write(3, buf, 9) ... = 9
[syscall] lseek(3, 4, SEEK_SET) ... = 4
[syscall] write(3, buf, 6) ... = 6
```

**Manejo de errores**

Cada retorno de syscall se verifica y los fallos se reportan con `perror()`. `read` y `write` se reintentan ante `EINTR` y `write` se repite hasta escribir todo el buffer. Si `a` falla al escribir, se deshace en la lista y el archivo vuelve a su tamaño anterior con `ftruncate`. Si `o` falla, se conserva el archivo que estaba abierto. Los errores del usuario (línea inexistente, número inválido, comando desconocido) no terminan el editor.

### Pasos de ejecución

1. Compilar: `cd editor && make` (genera `./editor`; `make clean` borra binarios y temporales).
2. Ejecutar: `./editor notas.txt`, o `./editor -t notas.txt` para ver la traza de cada syscall.
3. Escribir comandos, por ejemplo `a hola`, `i 1 primera`, `p`, `s hola`, `d 2`.
4. Salir con `q` (o `Ctrl+D`).

## Parte 3 de la entrega: Compresor Huffman concurrente

El compresor vive **dentro del editor** como una segunda capacidad: mientras `editor>` sigue aceptando comandos, un hilo orquestador y un pool de hilos trabajadores comprimen o descomprimen el archivo abierto. Solo usa `pthread` (mutex, variables de condición) y syscalls POSIX; no hay librerías de compresión ni de sincronización de alto nivel.

| Archivo | Contenido |
| --- | --- |
| `editor/huffman.c`, `editor/huffman.h` | Motor: histograma, árbol y códigos canónicos, codificación/decodificación por bloque, CRC32, versiones secuencial y concurrente |
| `editor/hilos.c`, `editor/hilos.h` | Pool de hilos genérico con cola de tareas, barrera de fin de fase y cancelación cooperativa |
| `editor/huf_editor.c`, `editor/huf_editor.h` | Orquestador en segundo plano: máquina de estados del trabajo, progreso, cancelación, `SIGINT` |
| `editor/editor.c` | Comandos `c`, `x`, `t`, `k`, `w` y guardia de `a`/`d`/`i` durante un trabajo |
| `editor/docs/CONCURRENCIA.md` | Protocolo de concurrencia completo: hilos, recursos, orden de locks, diagramas, decisiones |
| `editor/docs/FORMATO_HUF.md` | Especificación del formato `.huf` |

### Comandos en el editor

| Comando | Acción |
| --- | --- |
| `c [salida]` | Comprime el archivo abierto en segundo plano; por defecto escribe `<ruta>.huf` |
| `x [salida]` | Descomprime el archivo abierto (debe terminar en `.huf`); por defecto quita la extensión. La salida no puede ser el archivo abierto y nunca sobrescribe un archivo existente (`O_EXCL`) |
| `t` | Estado del trabajo y porcentaje de avance |
| `k` | Cancela el trabajo en curso (cooperativo, entre bloques) |
| `w` | Bloquea hasta que el trabajo termine (espera en variable de condición) |

`c` y `x` regresan de inmediato al prompt. El progreso se imprime en cada múltiplo de 10 % (`[huf] 30 % (comprimiendo notas.txt)`) y nunca retrocede. Solo puede haber un trabajo activo a la vez. `q`, EOF y `Ctrl+C` cancelan el trabajo, unen todos los hilos con `pthread_join` y borran el temporal.

```
editor> o notas.txt
editor> c
[huf] compresión en segundo plano: notas.txt -> notas.txt.huf
editor> t
editor> w
editor> o notas.txt.huf
editor> x notas.rt
editor> w
```

### Fases del algoritmo

El archivo se divide en bloques (64 KiB por defecto). El pool tiene tantos hilos como CPUs en línea, con un tope de 8.

1. **Conteo de frecuencias.** Cada tarea lee su bloque con `pread` y cuenta en su **propio** histograma parcial (`parciales[k][256]`): no hay dato compartido durante el conteo. Tras la barrera (`hilos_esperar`), el orquestador suma los parciales.
2. **Árbol y códigos.** Un solo hilo construye el árbol con un min-heap de desempate determinista y deriva códigos canónicos (longitud máxima 32 bits, con recorte de Kraft para entradas patológicas). La tabla queda inmutable y los workers solo la leen.
3. **Codificación en paralelo.** Cada worker codifica un bloque en su propio buffer, con CRC32 del bloque original.
4. **Ensamblado en orden.** El orquestador hace de ensamblador: espera en `cond_listo` el bloque `sig_escribir`, lo escribe con `pwrite` en el offset acumulado y avanza. Una ventana `W = 2·N` limita cuántos bloques pueden ir por delante, sin interbloqueo porque el bloque esperado siempre está dentro de la ventana.
5. **Cierre atómico.** Se escribe la tabla de bloques, y `<salida>.tmp` se renombra con `rename` solo si todo salió bien; ante error o cancelación se hace `unlink` del temporal.

La **descompresión** reparte un bloque por tarea: cada worker localiza su bloque en la tabla, lo decodifica, verifica el CRC32 y lo escribe con `pwrite` en `k · tam_bloque`. Los destinos son disjuntos, así que no hace falta lock ni orden de escritura.

### Sincronización

| Recurso | Protección |
| --- | --- |
| Cola de tareas del pool | `pthread_mutex` + condvar `hay_tareas` (workers duermen) y `cola_vacia` (barrera) |
| Histogramas parciales | Ninguna: una fila por tarea, reducción en un solo hilo tras la barrera |
| Bloques codificados y cursor de escritura | `mtx` del contexto + condvars `cond_listo` y `cond_ventana` |
| Estado del trabajo (editor) | `estado_mtx` + `estado_cv` |
| Salida por pantalla | `salida_mtx` (prompt y progreso no se mezclan) |
| Cancelación | Bandera atómica (`__atomic_load_n`/`__atomic_store_n`) |
| `SIGINT` | `volatile sig_atomic_t`; el manejador solo la activa |

No hay espera activa: todo punto de espera bloquea en una variable de condición o en `pthread_join`. `estado_mtx` y `salida_mtx` nunca se anidan, y los mutex del motor son hojas, por lo que no hay ciclos de locks. El detalle, los diagramas de hilos y estados y las preguntas de sustentación están en [editor/docs/CONCURRENCIA.md](editor/docs/CONCURRENCIA.md).

### Carrera con la edición

Mientras un trabajo está `RUNNING` o `CANCELLING`, `a`, `d` e `i` se **rechazan** sin tocar el archivo:

```
Error: hay un trabajo Huffman en curso; no se puede modificar el archivo.
Usa 'w' para esperar, 'k' para cancelar o 't' para ver el estado.
```

El estado pasa a `RUNNING` antes de crear el hilo orquestador, y el REPL es monohilo, así que ninguna edición puede colarse entre el arranque del trabajo y la primera lectura de los workers. El trabajo lee con su propio descriptor, nunca con el del editor. `p` y `s` siguen respondiendo porque solo leen la lista en memoria.

### Formato del archivo `.huf`

Enteros little-endian. Especificación completa en [editor/docs/FORMATO_HUF.md](editor/docs/FORMATO_HUF.md).

| Sección | Tamaño | Contenido |
| --- | --- | --- |
| Encabezado | 276 bytes | Magic `HUF1`, tamaño original (u64), tamaño de bloque (u32), número de bloques (u32), 256 longitudes de código (u8) |
| Tabla de bloques | `nbloques × 32` bytes | Por bloque: offset del flujo, longitud comprimida, longitud original, CRC32, bits útiles |
| Flujos de bits | variable | Un flujo por bloque, alineado a byte |

Solo se guardan las longitudes: el descompresor reconstruye los mismos códigos canónicos. La tabla permite decodificar cada bloque sin leer los anteriores.

### Pasos de ejecución

1. Compilar el shell (incluye editor y compresor): `cd shell && make`.
2. Ejecutar `./eafitOS` y abrir el editor: `e_edit notas.txt`.
3. Escribir `h` para ver los comandos; `c` comprime, `t` muestra el avance, `w` espera.
4. Abrir el comprimido con `o notas.txt.huf` y descomprimir con `x notas.rt`; luego `w`.
5. Salir con `q` y comparar: `cmp notas.txt notas.rt` (sin salida si son idénticos).

También se puede usar el editor independiente: `cd editor && make && ./editor notas.txt`.

## Parte 4 de la entrega: Pruebas

Hay dos baterías, ambas en `editor/tests/`. El contenido en disco se compara **byte a byte** con `cmp`/`md5sum`.

**`run_tests.sh` (`make test`)**: editor y shell.

- Los 7 comandos del editor, con sus errores de uso y casos borde (línea inexistente, `ENOENT`, `EISDIR`, archivo vacío, archivo sin `\n` final, archivo de más de 4 KiB).
- Ausencia de `fopen`/`fread`/`fwrite`/`fclose` en el binario (`nm -u`) y fugas con AddressSanitizer/LeakSanitizer.
- Integración con el shell (`e_edit`, `waitpid`, `Ctrl+C`).
- Integración shell → editor → compresor: `e_edit`, `h`, `c`, `w`, `x`, ida y vuelta idéntica con `cmp`, sin temporales.

**`run_tests_huf.sh` (`make test-huf`)**: compresor.

- Corrección: ida y vuelta con distintos números de hilos y tamaños de bloque, datos de texto, aleatorios, un solo símbolo y ceros; histograma paralelo idéntico al secuencial.
- Carreras con la edición: `a`/`d`/`i` rechazados durante el trabajo, `p`/`s` responden, original intacto.
- Cancelación con `k`, `q`, EOF y `SIGINT`: sin `.tmp`, original intacto.
- Progreso monótono de 0 a 100 %.
- Errores: `.huf` corrupto o truncado, salida existente, entrada inexistente.
- Estrés con parámetros aleatorios y una tabla de rendimiento.
- Las secciones de carreras y cancelación se repiten con un editor compilado con ThreadSanitizer.

Resultados en WSL2 Ubuntu 24.04 (kernel 6.18.40.1) / GCC 13.3.0:

| Suite | Comando | Resultado |
| --- | --- | --- |
| Editor + shell + integración | `make test` | **73 PASS, 0 FAIL** |
| Compresor | `make test-huf` | **52 PASS, 0 FAIL** |
| Compresor con AddressSanitizer/UBSan/LeakSanitizer | `make test-asan` | **52 PASS, 0 FAIL** |
| Compresor con ThreadSanitizer | `make test-tsan` | **52 PASS, 0 FAIL** |

ThreadSanitizer de GCC 13 aborta al arrancar en kernels con alta entropía de ASLR (`FATAL: ThreadSanitizer: unexpected memory mapping`). Por eso `run_tests_huf.sh` se re-ejecuta a sí mismo con `setarch -R` (ASLR desactivado), y un `FATAL` de TSan cuenta como fallo.

El reporte técnico del editor está en [editor/docs/REPORTE_TECNICO.md](editor/docs/REPORTE_TECNICO.md).

### Pasos de ejecución

1. Entrar al editor: `cd editor`.
2. Editor, shell e integración: `make test`.
3. Compresor: `make test-huf`; con sanitizers: `make test-asan` y `make test-tsan`.
4. Limpiar binarios y temporales: `make clean`.

## Rendimiento: secuencial vs concurrente

Compresión de una entrada de 64 MiB (texto repetido y bytes aleatorios) con el CLI de pruebas `editor/huf_cli` (`make huf_cli`), bloque de 64 KiB, 12 CPUs lógicas, mediana de 3 corridas:

| Modo | Comando | Tiempo (s) | Speedup |
| --- | --- | --- | --- |
| Secuencial | `./huf_cli c in.bin o.huf` | 2.060 | 1.00x |
| Pool de 1 hilo | `./huf_cli C in.bin o.huf 1` | 2.029 | 1.02x |
| Pool automático (8 hilos) | `./huf_cli C in.bin o.huf 0` | 0.489 | 4.21x |

La ida y vuelta (`C` + `X` concurrente y `c` + `x` secuencial) produjo el mismo SHA-256 que la entrada. El detalle de las corridas está en [editor/docs/CONCURRENCIA.md](editor/docs/CONCURRENCIA.md#10-evidencia-secuencial-vs-concurrente).

## Estructura del repositorio

| Ruta | Contenido |
| --- | --- |
| `shell/` | Shell educativo `eafitOS`: categorías `datos`, `memoria`, `monitoreo`, `utilidades` y `editor` |
| `editor/` | Editor por líneas (`main.c`, `editor.c`, `lista.c`) y compresor (`huffman.c`, `hilos.c`, `huf_editor.c`), con su `Makefile` |
| `editor/tests/` | `run_tests.sh`, `run_tests_huf.sh` y el CLI de pruebas `huf_cli.c` |
| `editor/docs/` | `REPORTE_TECNICO.md`, `CONCURRENCIA.md`, `FORMATO_HUF.md` |

## Autores

**Miguel Ángel Colorado Castaño** <br>
**Sebastián Andrés Ibarra Prada**

## Información del curso
**Curso:** Sistemas Operativos - SO2026B <br>
**Profesor:** Edison Valencia Diaz <br>
**Universidad:** Universidad EAFIT <br>
**Entrega:** Parcial 2, Concurrencia y Sincronización. Alternativa 1: Compresor de Archivos Huffman Concurrente Integrado al Editor <br>
**Ciclo:** 2026-2
