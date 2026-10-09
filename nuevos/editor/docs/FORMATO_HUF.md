# Formato de archivo `.huf` (versión "HUF1")

Formato producido por el módulo `huf_` del editor. Todos los enteros son
**little-endian**. La E/S se hace solo con syscalls POSIX
(`open`/`pread`/`pwrite`/`read`/`write`/`ftruncate`/`close`/`fstat`).

## Disposición general

```
+----------------------+  offset 0
| ENCABEZADO FIJO      |  276 bytes
+----------------------+  offset 276
| TABLA DE BLOQUES     |  nbloques * 32 bytes
+----------------------+
| FLUJOS DE BITS       |  concatenados, uno por bloque, alineados a byte
+----------------------+
```

## Encabezado fijo (276 bytes)

| offset | tamaño | tipo | campo |
|--------|--------|------|-------|
| 0  | 4   | char[4] | magic `"HUF1"` |
| 4  | 8   | u64 | tamaño original del archivo en bytes |
| 12 | 4   | u32 | tamaño de bloque usado al comprimir |
| 16 | 4   | u32 | número de bloques `nbloques` |
| 20 | 256 | u8[256] | longitud de código canónico por símbolo (0 = símbolo ausente) |

El descompresor reconstruye los códigos canónicos a partir de las 256
longitudes, con el acuerdo: ordenar los símbolos presentes por `(longitud,
símbolo)` ascendente y numerarlos consecutivamente, desplazando un bit al pasar
a la siguiente longitud. Por eso **no** se serializa el árbol.

## Tabla de bloques (`nbloques` entradas de 32 bytes)

Una entrada por bloque, en orden de índice de bloque (0, 1, 2, …):

| offset | tamaño | tipo | campo |
|--------|--------|------|-------|
| +0  | 8 | u64 | offset en el `.huf` donde empieza el flujo de bits del bloque |
| +8  | 8 | u64 | longitud comprimida del bloque en **bytes** (incluye relleno) |
| +16 | 8 | u64 | longitud **original** del bloque en bytes |
| +24 | 4 | u32 | CRC32 del bloque original (polinomio reflejado `0xEDB88320`) |
| +28 | 4 | u32 | número de **bits útiles** del flujo (`nbits`) |

La tabla permite localizar y verificar cada bloque **sin leer los anteriores**,
lo que habilita la descompresión concurrente (cada hilo decodifica un bloque y
escribe su resultado en `índice * tamaño_de_bloque`, destinos disjuntos).

### Por qué la tabla se reserva y se completa al final

El tamaño comprimido de un bloque solo se conoce **después** de codificarlo. Al
comprimir:

1. Se escribe el encabezado fijo y se reserva el espacio de la tabla
   (`ftruncate` hasta donde empiezan los flujos).
2. Se codifica cada bloque y se escribe su flujo con `pwrite` en el offset
   acumulado, anotando en memoria su entrada de tabla.
3. Al terminar, se escribe la tabla completa con `pwrite` en su offset
   reservado (276).

En la versión concurrente (etapas 3+) la salida va a `nombre.huf.tmp` y se
renombra con `rename()` **atómico** solo si todo terminó bien; ante error o
cancelación se hace `unlink` del temporal, de modo que la salida final nunca
queda a medias.

## Flujos de bits y relleno

Cada bloque se codifica de forma independiente con la **misma** tabla de códigos
(derivada del histograma de todo el archivo). Los bits se emiten MSB primero
dentro de cada código. El flujo de cada bloque se alinea a byte: el último byte
se rellena con ceros en los bits bajos no usados.

El relleno **no** se interpreta como datos porque la tabla guarda `nbits`, el
número exacto de bits útiles; el decodificador lee exactamente esa cantidad y
produce exactamente `longitud original` bytes.

## Casos límite

- **Archivo vacío (0 bytes):** `nbloques = 0`, tabla vacía, sin flujos.
  Descomprimir produce un archivo vacío.
- **Un único símbolo distinto:** su longitud de código es 1 (nunca 0).
- **Tamaño no múltiplo del bloque:** el último bloque es más corto; su longitud
  original se registra en la tabla.
- **Frecuencias patológicas (p. ej. Fibonacci):** un árbol de Huffman pleno
  sobre 256 símbolos puede alcanzar longitudes de hasta 255 bits. Como los
  códigos se guardan en `uint32_t`, el módulo aplica *length-limiting* a 32 bits
  (recorte de Kraft): el código deja de ser óptimo pero sigue siendo un código
  prefijo decodificable. 32 es el máximo soportado y documentado.
