#!/usr/bin/env bash
# =====================================================================================
# Pruebas del editor de líneas (comandos o, p, a, d, i, s, q) y de su integración con
# el shell educativo. Uso (desde editor/):  bash tests/run_tests.sh   o   make test
#
# Cada caso alimenta comandos por stdin (como lo haría un usuario) y verifica:
#   - la salida del editor (mensajes, errores esperados), y/o
#   - el contenido EXACTO del archivo en disco, byte a byte (cmp).
# =====================================================================================
set -u

DIR_ED="$(cd "$(dirname "$0")/.." && pwd)"
DIR_SHELL="$DIR_ED/../shell"
TMP="$DIR_ED/tests/tmp"
ED="$DIR_ED/editor"
ED_ASAN="$DIR_ED/editor_asan"

PASS=0
FAIL=0

ok()   { PASS=$((PASS + 1)); printf '  [PASS] %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  [FAIL] %s\n' "$1"; }
seccion() { printf '\n== %s ==\n' "$1"; }

# Ejecuta el editor con la entrada $1 (secuencia con \n) y deja la salida sin
# colores ANSI en $OUT y el código de salida en $RC. Argumentos extra -> editor.
correr() {
    local entrada="$1"; shift
    OUT="$(printf "$entrada" | "$ED" "$@" 2>&1 | sed 's/\x1b\[[0-9;]*m//g'; exit "${PIPESTATUS[1]}")"
    RC=$?
}

contiene() {   # contiene "<descripción>" "<texto esperado en la salida>"
    if grep -qF -- "$2" <<<"$OUT"; then ok "$1"; else bad "$1 (no aparece: '$2')"; printf '%s\n' "$OUT" | sed 's/^/        | /'; fi
}

no_contiene() {
    if grep -qF -- "$2" <<<"$OUT"; then bad "$1 (aparece: '$2')"; else ok "$1"; fi
}

archivo_es() { # archivo_es "<descripción>" <archivo> "<contenido esperado (printf)>"
    if cmp -s "$2" <(printf "$3"); then ok "$1"; else
        bad "$1"; printf '        esperado: '; printf "$3" | od -c | head -3; printf '        obtenido: '; od -c "$2" | head -3
    fi
}

rc_es() {
    if [ "$RC" -eq "$2" ]; then ok "$1"; else bad "$1 (código $RC, esperado $2)"; fi
}

# -------------------------------------------------------------------------------------
seccion "Compilación"
make -C "$DIR_ED" clean >/dev/null
if make -C "$DIR_ED" all editor_asan >/dev/null 2>&1; then ok "make all (editor, -Werror) y editor_asan"; else bad "make all"; exit 1; fi
if make -C "$DIR_SHELL" clean all >/dev/null 2>&1; then ok "make all del shell con la categoría editor"; else bad "make shell"; fi
if nm -u "$ED" | grep -qE '\b(fopen|fread|fwrite|fclose)\b'; then
    bad "el binario NO debe importar fopen/fread/fwrite/fclose"; nm -u "$ED" | grep -E 'fopen|fread|fwrite|fclose'
else
    ok "el binario no importa fopen/fread/fwrite/fclose (nm -u)"
fi

rm -rf "$TMP"; mkdir -p "$TMP"; cd "$TMP" || exit 1

# -------------------------------------------------------------------------------------
seccion "o: abrir / crear"
correr 'o nuevo.txt\nq\n'
contiene "archivo inexistente se crea con O_CREAT" "Archivo 'nuevo.txt' abierto (fd 3): 0 línea(s), 0 byte(s)."
[ -f nuevo.txt ] && ok "nuevo.txt existe en disco" || bad "nuevo.txt no fue creado"
rc_es "q termina con exit(0)" 0

correr 'o no_existe_dir/x.txt\np\nq\n'
contiene "ruta en directorio inexistente -> perror(open)" "open: No such file or directory"
contiene "sin archivo abierto, p falla limpio" "no hay archivo abierto"

mkdir -p undir
correr 'o undir\nq\n'
contiene "abrir un directorio -> perror(open) EISDIR" "open: Is a directory"

correr 'o\nq\n'
contiene "o sin argumento muestra uso" "Uso: o <archivo>"

printf 'uno\ndos\n' > existente.txt
correr '' existente.txt
contiene "archivo pasado por argv se abre al iniciar" "Archivo 'existente.txt' abierto (fd 3): 2 línea(s), 8 byte(s)."
rc_es "EOF (Ctrl+D) equivale a q" 0

printf 'A\n' > a.txt; printf 'B1\nB2\n' > b.txt
correr 'o a.txt\no b.txt\np\nq\n' -t
contiene "cambiar de archivo cierra el anterior" "[syscall] close(3) ... = 0"
contiene "el nuevo archivo queda cargado" "     2  B2"
correr 'o b.txt\no no_existe_dir/z\np 1\nq\n'
contiene "si el nuevo open falla se conserva el actual" "     1  B1"

# -------------------------------------------------------------------------------------
seccion "Archivo vacío"
: > vacio.txt
correr 'o vacio.txt\np\np 1\nd 1\ns algo\nq\n'
contiene "p en archivo vacío" "(archivo vacío)"
contiene "p 1 en archivo vacío -> línea inexistente" "Error: la línea 1 no existe."
contiene "d 1 en archivo vacío -> línea inexistente" "El archivo tiene 0 línea(s)."
contiene "s en archivo vacío" "'algo' no aparece en el archivo."
archivo_es "archivo vacío sigue vacío" vacio.txt ''

correr 'o vacio.txt\ni 1 primera\nq\n'
archivo_es "i 1 en archivo vacío equivale a append" vacio.txt 'primera\n'

# -------------------------------------------------------------------------------------
seccion "a: append"
correr 'o ap.txt\na linea uno\na linea dos\na\na   con espacios  \nq\n'
contiene "a reporta número de línea" "Línea 4 agregada."
archivo_es "a escribe al final (incluye línea vacía y espacios)" ap.txt 'linea uno\nlinea dos\n\n  con espacios  \n'

printf 'x\ny' > sin_nl.txt
correr 'o sin_nl.txt\na z\nq\n'
archivo_es "a en archivo sin salto final no pega el texto" sin_nl.txt 'x\ny\nz\n'

# -------------------------------------------------------------------------------------
seccion "p: imprimir"
printf 'alfa\nbeta\ngamma\n' > p.txt
correr 'o p.txt\np\np 2\np 4\np 0\np abc\np 2x\nq\n'
contiene "p imprime todo (línea 1)" "     1  alfa"
contiene "p imprime todo (línea 3)" "     3  gamma"
contiene "p 2" "     2  beta"
contiene "p 4 -> línea inexistente" "Error: la línea 4 no existe."
contiene "p 0 -> inválido" "número de línea inválido '0'"
contiene "p abc -> inválido" "número de línea inválido 'abc'"
contiene "p 2x -> inválido" "número de línea inválido '2x'"

# -------------------------------------------------------------------------------------
seccion "d: borrar (lseek + ftruncate)"
printf 'l1\nl2\nl3\nl4\nl5\n' > d.txt
correr 'o d.txt\nd 3\nd 1\nd 3\nd 9\nd\nq\n' -t
contiene "d usa ftruncate" "[syscall] ftruncate(3,"
contiene "d 9 -> línea inexistente" "Error: la línea 9 no existe."
contiene "d sin n -> inválido" "número de línea inválido ''"
archivo_es "d medio, primera y última" d.txt 'l2\nl4\n'

printf 'x\ny' > d_sin_nl.txt
correr 'o d_sin_nl.txt\nd 2\nq\n'
archivo_es "d de la última línea sin salto final no deja línea vacía" d_sin_nl.txt 'x'
printf 'solo' > d_unica.txt
correr 'o d_unica.txt\nd 1\na nueva\nq\n'
archivo_es "d de la única línea y luego a" d_unica.txt 'nueva\n'

# -------------------------------------------------------------------------------------
seccion "i: inserción arbitraria"
printf 'uno\ndos\ntres\n' > i.txt
correr 'o i.txt\ni 1 cero\ni 3 uno y medio\ni 6 final\ni 9 no\ni 0 no\ni x no\nq\n'
contiene "i 9 -> fuera de rango" "no se puede insertar en la línea 9."
contiene "i 0 -> inválido" "número de línea inválido '0 no'"
contiene "i x -> inválido" "número de línea inválido 'x no'"
archivo_es "i al inicio, en medio y en count+1" i.txt 'cero\nuno\nuno y medio\ndos\ntres\nfinal\n'

printf 'x\ny' > i_sin_nl.txt
correr 'o i_sin_nl.txt\ni 2 medio\nq\n'
archivo_es "i antes de una última línea sin salto final" i_sin_nl.txt 'x\nmedio\ny'

# Archivo de más de un bloque (4096 B): el desplazamiento recorre varios bloques.
seq 1 3000 > grande.txt
{ echo "INSERTADA"; seq 1 3000; } > esperado_i.txt
correr 'o grande.txt\ni 1 INSERTADA\nq\n'
if cmp -s grande.txt esperado_i.txt; then ok "i 1 en archivo de $(wc -c < esperado_i.txt) bytes (multi-bloque)"; else bad "i en archivo grande"; fi
correr 'o grande.txt\nd 1\nd 1500\nq\n'
seq 1 3000 | sed '1500d' > esperado_d.txt
if cmp -s grande.txt esperado_d.txt; then ok "d 1 y d 1500 en archivo grande (multi-bloque)"; else bad "d en archivo grande"; fi

# -------------------------------------------------------------------------------------
seccion "s: búsqueda"
printf 'el gato\nel perro y el gato\nnada\nGATO\n' > s.txt
correr 'o s.txt\ns gato\ns el\ns zzz\ns\nq\n'
contiene "s reporta línea 1" "     1  el gato  (1)"
contiene "s reporta línea 2" "     2  el perro y el gato  (1)"
contiene "s es sensible a mayúsculas (línea 4 no)" "'gato': 2 ocurrencia(s) en 2 línea(s)."
contiene "s cuenta varias ocurrencias por línea" "'el': 3 ocurrencia(s) en 2 línea(s)."
contiene "s sin resultados" "'zzz' no aparece en el archivo."
contiene "s sin palabra -> uso" "Uso: s <palabra>"

# -------------------------------------------------------------------------------------
seccion "q, comandos inválidos y persistencia"
# 'z' en vez de 'x': 'x' ahora es el comando de descompresión Huffman (etapa 5),
# así que ya no sirve como ejemplo de comando inexistente.
correr 'o q.txt\na persistente\nz\npp\nq\na nunca\n'
contiene "comando desconocido" "comando 'z' no reconocido"
contiene "comando pegado (pp) no se confunde con p" "comando 'pp' no reconocido"
no_contiene "nada se ejecuta después de q" "nunca"
archivo_es "lo escrito persiste tras q" q.txt 'persistente\n'
correr 'o q.txt\np\nq\n'
contiene "al reabrir se lee lo persistido" "     1  persistente"

# -------------------------------------------------------------------------------------
seccion "Fugas de memoria (AddressSanitizer / LeakSanitizer)"
seq 1 500 > asan.txt
SAL="$(printf 'o asan.txt\na x\ni 2 y\nd 10\ns 1\np\no asan.txt\np 3\nd 999\nq\n' \
      | ASAN_OPTIONS=detect_leaks=1 "$ED_ASAN" -t 2>&1 >/dev/null)"; RC=$?
if [ "$RC" -eq 0 ] && ! grep -q "Sanitizer" <<<"$SAL"; then ok "sesión completa sin fugas ni errores de memoria"; else bad "ASan reportó problemas"; printf '%s\n' "$SAL" | head -30; fi

# -------------------------------------------------------------------------------------
seccion "Integración con el shell educativo (e_edit)"
OUT="$(printf 'help editor\ne_edit sh.txt\na desde el shell\ni 1 primera\np\nq\nd_read sh.txt\nexit\n' \
      | "$DIR_SHELL/eafitOS" 2>&1 | sed 's/\x1b\[[0-9;]*m//g')"
contiene "help editor lista e_edit" "e_edit     -> Abre el editor"
contiene "e_edit hace fork y el hijo corre el editor" "[syscall] fork() ... = 0 (hijo"
contiene "el padre espera con waitpid" "terminó con código 0. De vuelta en el shell."
contiene "el shell continúa tras q (d_read lee el archivo)" "--- Contenido del Archivo ---"
archivo_es "archivo editado desde el shell" sh.txt 'primera\ndesde el shell\n'
no_contiene "comandos del editor no se re-ejecutan en el shell" "Comando 'a' no encontrado"

# Ctrl+C: SIGINT a todo el grupo de procesos (como la terminal). Debe morir solo el
# editor; el shell sigue vivo y lo ya escrito queda en disco. El productor de la
# entrada ignora SIGINT para poder seguir enviando comandos.
setsid bash -c '
    ( trap "" INT; printf "e_edit sigint.txt\na antes\n"; sleep 1
      kill -INT -- -$$; sleep 0.5; printf "hora\nexit\n" ) | "$1" > sigint.out 2>&1
' _ "$DIR_SHELL/eafitOS"
OUT="$(sed 's/\x1b\[[0-9;]*m//g' sigint.out)"
contiene "Ctrl+C termina solo al editor" "terminado por la señal 2"
no_contiene "el shell sigue aceptando comandos tras Ctrl+C" "no encontrado"
contiene "el shell sigue vivo tras Ctrl+C" "Saliendo del shell educativo"
archivo_es "lo escrito antes de Ctrl+C persiste" sigint.txt 'antes\n'

# -------------------------------------------------------------------------------------
printf '\n==================== RESUMEN: %d PASS, %d FAIL ====================\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
