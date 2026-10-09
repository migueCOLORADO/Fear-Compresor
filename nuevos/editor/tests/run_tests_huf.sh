#!/usr/bin/env bash
# =====================================================================================
# Pruebas del compresor/descompresor Huffman concurrente (módulo huf_).
# Uso (desde editor/):  bash tests/run_tests_huf.sh   o   make test-huf
#
# Variables de entorno:
#   HUF_CLI   binario del CLI de prueba a usar (por defecto ./huf_cli).
#             make test-asan / make test-tsan lo apuntan a los binarios
#             instrumentados con AddressSanitizer/UBSan o ThreadSanitizer.
#
# Secciones: corrección (ida y vuelta con cmp/md5), carreras con la edición,
# cancelación (k/q/EOF/SIGINT), errores, estrés y rendimiento (tabla de speedup).
# Cada caso reporta [PASS]/[FAIL] como run_tests.sh.
# =====================================================================================
set -u

DIR_ED="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$DIR_ED/tests/tmp_huf"
CLI="${HUF_CLI:-$DIR_ED/huf_cli}"
EDITOR="$DIR_ED/editor"

PASS=0
FAIL=0
ok()   { PASS=$((PASS + 1)); printf '  [PASS] %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  [FAIL] %s\n' "$1"; }
seccion() { printf '\n== %s ==\n' "$1"; }

# md5 de un archivo (admite md5sum o, si no, el CLI 'crc' como huella alternativa).
huella() {
    if command -v md5sum >/dev/null 2>&1; then
        md5sum "$1" | cut -d' ' -f1
    else
        "$CLI" crc "$1"
    fi
}

# Comparación byte a byte. Usa cmp si existe; si no, compara tamaños + huella.
igual() {
    if command -v cmp >/dev/null 2>&1; then
        cmp -s "$1" "$2"
    else
        [ "$(wc -c < "$1")" = "$(wc -c < "$2")" ] && [ "$(huella "$1")" = "$(huella "$2")" ]
    fi
}

if [ ! -x "$CLI" ]; then
    printf 'No existe el CLI de prueba: %s (ejecuta make huf_cli)\n' "$CLI" >&2
    exit 1
fi

rm -rf "$TMP"; mkdir -p "$TMP"; cd "$TMP" || exit 1

# Generadores de contenido ---------------------------------------------------------
gen_texto()     { yes "la rapida zorra marron salta sobre el perro perezoso 0123456789" | head -c "$1" > "$2"; }
gen_aleatorio() { head -c "$1" /dev/urandom > "$2"; }
gen_unbyte()    { head -c "$1" /dev/zero | tr '\0' 'Q' > "$2"; }
gen_ceros()     { head -c "$1" /dev/zero > "$2"; }     # binario con muchos 0x00

# -------------------------------------------------------------------------------------
seccion "1. Corrección: ida y vuelta (c -> x -> cmp/md5) con N y bloques variados"
# Tamaños representativos (el spec incluye 50 MB; aquí se acota para el entorno de
# pruebas, pero se cubren los bordes 0/1/4095/4096/4097 y archivos de 1 MB).
TAMS="0 1 4095 4096 4097 1048576"
for tipo in texto aleatorio unbyte ceros; do
    for sz in $TAMS; do
        f="in_${tipo}_${sz}.bin"
        case $tipo in
            texto)     gen_texto "$sz" "$f" ;;
            aleatorio) gen_aleatorio "$sz" "$f" ;;
            unbyte)    gen_unbyte "$sz" "$f" ;;
            ceros)     gen_ceros "$sz" "$f" ;;
        esac
        h0=$(huella "$f")
        malo=0
        for N in 1 2 4 8; do
            B=$(( (RANDOM % 3072) + 1024 ))   # 1-4 KiB: fuerza muchos bloques
            "$CLI" C "$f" c.huf "$N" "$B" >/dev/null 2>&1 || { malo=1; break; }
            "$CLI" X c.huf x.out "$N" >/dev/null 2>&1 || { malo=1; break; }
            igual "$f" x.out || { malo=1; break; }
            [ "$(huella x.out)" = "$h0" ] || { malo=1; break; }
            rm -f c.huf x.out
        done
        if [ "$malo" -eq 0 ]; then ok "ida y vuelta $tipo $sz B (N=1,2,4,8, bloques 1-4 KiB)"
        else bad "ida y vuelta $tipo $sz B"; fi
        rm -f "$f" c.huf x.out
    done
done

# Igualdad del histograma paralelo vs secuencial (longitudes canónicas del header).
gen_texto 500000 ht.bin
"$CLI" c ht.bin seq.huf 1024 >/dev/null 2>&1
dd if=seq.huf bs=1 skip=20 count=256 of=seq.longs 2>/dev/null
malo=0
for N in 1 2 4 8; do
    "$CLI" C ht.bin par.huf "$N" 1024 >/dev/null 2>&1
    dd if=par.huf bs=1 skip=20 count=256 of=par.longs 2>/dev/null
    igual seq.longs par.longs || { malo=1; break; }
    rm -f par.huf par.longs
done
[ "$malo" -eq 0 ] && ok "histograma paralelo idéntico al secuencial" || bad "histograma paralelo != secuencial"
rm -f ht.bin seq.huf seq.longs par.huf par.longs

# Editores con worker lento (HUF_LENTO) para mantener vivo el trabajo cuando
# llegan los comandos. Hay dos: el normal y el instrumentado con ThreadSanitizer.
# Las secciones 2 y 3 se ejecutan con AMBOS: así las carreras y la cancelación se
# validan también bajo TSan (etiqueta "[tsan]").
ED_LENTO="$DIR_ED/editor_lento"
ED_LENTO_TSAN="$DIR_ED/editor_lento_tsan"

base_intacto() { [ "$(huella "$1")" = "$2" ]; }

# ¿La salida contiene un reporte de ThreadSanitizer? (solo relevante en modo tsan)
hay_tsan() { echo "$1" | grep -q "WARNING: ThreadSanitizer"; }

# Sección 2 parametrizada: $1 = binario del editor, $2 = etiqueta (""/"[tsan] ").
seccion2_carreras() {
    local ED="$1" et="$2"
    if [ ! -x "$ED" ]; then bad "${et}no está el editor lento ($ED)"; return; fi
    gen_texto 4000000 race.txt
    local h0 out; h0=$(huella race.txt)
    out=$(printf 'o race.txt\nc\na intruso\nd 1\ni 1 intruso\np 1\ns zorra\nk\nw\nq\n' \
          | TSAN_OPTIONS=halt_on_error=0 timeout 180 "$ED" 2>&1 | sed 's/\x1b\[[0-9;]*m//g')
    if echo "$out" | grep -q "no se puede modificar el archivo"; then ok "${et}a/d/i se rechazan durante el trabajo"
    else bad "${et}a/d/i NO se rechazaron durante el trabajo"; fi
    if echo "$out" | grep -qE "^ *1 "; then ok "${et}p responde durante el trabajo"; else bad "${et}p no respondió"; fi
    if [ "$(huella race.txt)" = "$h0" ]; then ok "${et}el archivo original queda intacto"; else bad "${et}el original cambió"; fi
    if [ -n "$et" ]; then
        hay_tsan "$out" && bad "${et}ThreadSanitizer reportó una carrera" || ok "${et}sin reportes de ThreadSanitizer"
    fi
    rm -f race.txt race.txt.huf race.txt.huf.tmp
}

# Sección 3 parametrizada: $1 = binario del editor, $2 = etiqueta.
seccion3_cancelacion() {
    local ED="$1" et="$2"
    if [ ! -x "$ED" ]; then bad "${et}no está el editor lento ($ED)"; return; fi
    local h0 rc out tsan_malo=0

    # k a mitad de trabajo
    gen_texto 6000000 can.txt; h0=$(huella can.txt)
    out=$(printf 'o can.txt\nc\nk\nw\nt\nq\n' | TSAN_OPTIONS=halt_on_error=0 timeout 180 "$ED" 2>&1)
    hay_tsan "$out" && tsan_malo=1
    [ ! -e can.txt.huf.tmp ] && base_intacto can.txt "$h0" && ok "${et}k: sin .tmp y original intacto" || bad "${et}k dejó basura o cambió el original"
    rm -f can.txt can.txt.huf can.txt.huf.tmp

    # q a mitad de trabajo (cancelación cooperativa + join antes de salir)
    gen_texto 6000000 can.txt; h0=$(huella can.txt)
    out=$(printf 'o can.txt\nc\nq\n' | TSAN_OPTIONS=halt_on_error=0 timeout 180 "$ED" 2>&1); rc=$?
    hay_tsan "$out" && tsan_malo=1
    [ ! -e can.txt.huf.tmp ] && base_intacto can.txt "$h0" && ok "${et}q con trabajo activo: sin .tmp, original intacto (rc=$rc)" || bad "${et}q dejó basura o cambió el original"
    rm -f can.txt can.txt.huf can.txt.huf.tmp

    # EOF a mitad (equivale a q): el script termina su entrada estando el trabajo activo
    gen_texto 6000000 can.txt; h0=$(huella can.txt)
    out=$(printf 'o can.txt\nc\n' | TSAN_OPTIONS=halt_on_error=0 timeout 180 "$ED" 2>&1)
    hay_tsan "$out" && tsan_malo=1
    [ ! -e can.txt.huf.tmp ] && base_intacto can.txt "$h0" && ok "${et}EOF con trabajo activo: sin .tmp, original intacto" || bad "${et}EOF dejó basura o cambió el original"
    rm -f can.txt can.txt.huf can.txt.huf.tmp

    # SIGINT a mitad: se envía al proceso del editor mientras trabaja
    gen_texto 6000000 can.txt; h0=$(huella can.txt)
    setsid bash -c '
        ( printf "o can.txt\nc\n"; sleep 2; printf "w\nq\n"; sleep 1 ) | TSAN_OPTIONS=halt_on_error=0 "$1" > sig.out 2>&1 &
        pid=$!
        sleep 1
        kill -INT "$pid" 2>/dev/null
        wait "$pid"
    ' _ "$ED" >/dev/null 2>&1
    hay_tsan "$(cat sig.out 2>/dev/null)" && tsan_malo=1
    [ ! -e can.txt.huf.tmp ] && base_intacto can.txt "$h0" && ok "${et}SIGINT con trabajo activo: sin .tmp, original intacto" || bad "${et}SIGINT dejó basura o cambió el original"
    rm -f can.txt can.txt.huf can.txt.huf.tmp sig.out

    if [ -n "$et" ]; then
        [ "$tsan_malo" -eq 0 ] && ok "${et}sin reportes de ThreadSanitizer en la cancelación" || bad "${et}ThreadSanitizer reportó en la cancelación"
    fi
}

# -------------------------------------------------------------------------------------
seccion "2. Carreras con la edición (a/d/i rechazados; p/s responden; original intacto)"
seccion2_carreras "$ED_LENTO" ""
seccion2_carreras "$ED_LENTO_TSAN" "[tsan] "

# -------------------------------------------------------------------------------------
seccion "3. Cancelación (k / q / EOF / SIGINT): sin .tmp, original intacto"
seccion3_cancelacion "$ED_LENTO" ""
seccion3_cancelacion "$ED_LENTO_TSAN" "[tsan] "

# -------------------------------------------------------------------------------------
seccion "3b. Progreso monótono (el porcentaje de 't' nunca decrece; 0-100%)"
# Extrae todos los enteros que preceden a '%' en la salida (líneas de progreso y
# respuestas de 't') y verifica que la secuencia no decrece y termina en 100.
monotono() {   # monotono "<descripción>" "<salida sin ANSI>"
    local desc="$1" out="$2"
    local prev=-1 v fin=-1 malo=0
    while read -r v; do
        [ -z "$v" ] && continue
        if [ "$v" -lt "$prev" ]; then malo=1; fi
        prev="$v"; fin="$v"
    done <<EOF
$(echo "$out" | grep -oE '[0-9]+ *%' | grep -oE '[0-9]+')
EOF
    if [ "$malo" -ne 0 ]; then bad "$desc (el porcentaje decreció)"; printf '%s\n' "$out" | grep -oE '[0-9]+ *%' | tr '\n' ' ' | sed 's/^/        serie: /'; echo
    elif [ "$fin" -ne 100 ]; then bad "$desc (no terminó en 100%, fin=$fin)"
    else ok "$desc"; fi
}

if [ -x "$ED_LENTO" ]; then
    # Compresión: dos fases (conteo 0-50%, codificación 50-100%). Se intercalan
    # muchos 't' para muestrear el avance mientras el trabajo corre.
    gen_texto 3000000 prog.txt
    out=$(printf 'o prog.txt\nc\nt\nt\nt\nt\nt\nt\nt\nt\nw\nt\nq\n' \
          | timeout 180 "$ED_LENTO" 2>&1 | sed 's/\x1b\[[0-9;]*m//g')
    monotono "compresión: progreso global monótono 0->100% sin bajar en el cambio de fase" "$out"
    rm -f prog.txt prog.txt.huf prog.txt.huf.tmp

    # Descompresión: una sola fase 0-100%.
    gen_texto 3000000 prog2.txt
    "$CLI" C prog2.txt prog2.huf 0 4096 >/dev/null 2>&1
    out=$(printf 'o prog2.huf\nx prog2.rt\nt\nt\nt\nt\nt\nw\nt\nq\n' \
          | timeout 180 "$ED_LENTO" 2>&1 | sed 's/\x1b\[[0-9;]*m//g')
    monotono "descompresión: progreso monótono 0->100%" "$out"
    rm -f prog2.txt prog2.huf prog2.rt prog2.rt.tmp
else
    bad "no está editor_lento para la prueba de progreso monótono"
fi

# -------------------------------------------------------------------------------------
seccion "4. Errores (.huf corrupto/truncado, overwrite, archivo inexistente)"
gen_texto 60000 e.bin
"$CLI" C e.bin good.huf 4 1024 >/dev/null 2>&1

# magic incorrecto
cp good.huf badmagic.huf; printf 'XXXX' | dd of=badmagic.huf bs=1 seek=0 count=4 conv=notrunc 2>/dev/null
"$CLI" X badmagic.huf bm.out >/dev/null 2>&1; [ $? -ne 0 ] && [ ! -e bm.out ] && ok "magic incorrecto rechazado" || bad "magic incorrecto no rechazado"

# truncado
head -c $(( $(wc -c < good.huf) / 2 )) good.huf > trunc.huf
"$CLI" X trunc.huf tr.out >/dev/null 2>&1; [ $? -ne 0 ] && [ ! -e tr.out ] && ok "archivo truncado rechazado" || bad "truncado no rechazado"

# byte alterado en datos (lo caza el CRC32)
cp good.huf alt.huf; sz=$(wc -c < alt.huf); printf '\xAA' | dd of=alt.huf bs=1 seek=$((sz-5)) count=1 conv=notrunc 2>/dev/null
"$CLI" X alt.huf al.out >/dev/null 2>&1; [ $? -ne 0 ] && ok "byte alterado detectado por CRC32" || bad "byte alterado no detectado"

# descomprimir sobre un archivo existente: no debe sobrescribir en silencio
echo PREEXISTENTE > ya.out
"$CLI" X good.huf ya.out >/dev/null 2>&1; r=$?
[ "$r" -ne 0 ] && [ "$(cat ya.out)" = "PREEXISTENTE" ] && ok "no sobrescribe un archivo existente" || bad "sobrescribió un archivo existente"

# archivo de entrada inexistente
"$CLI" C no_existe_12345.bin x.huf 2 1024 >/dev/null 2>&1; [ $? -ne 0 ] && ok "entrada inexistente -> error" || bad "entrada inexistente no dio error"

# c sin archivo abierto en el editor
out=$(printf 'c\nq\n' | timeout 20 "$EDITOR" 2>&1 | sed 's/\x1b\[[0-9;]*m//g')
echo "$out" | grep -q "no hay archivo abierto" && ok "c sin archivo abierto -> error claro" || bad "c sin archivo abierto no avisó"

# dos c seguidos: el segundo se rechaza (ya hay trabajo) — con editor lento
if [ -x "$ED_LENTO" ]; then
    gen_texto 4000000 dos.txt
    out=$(printf 'o dos.txt\nc\nc\nw\nq\n' | timeout 120 "$ED_LENTO" 2>&1 | sed 's/\x1b\[[0-9;]*m//g')
    echo "$out" | grep -q "ya hay un trabajo Huffman en curso" && ok "dos c seguidos: el segundo se rechaza" || bad "dos c seguidos no se rechazó"
    rm -f dos.txt dos.txt.huf dos.txt.huf.tmp
fi
rm -f e.bin good.huf badmagic.huf trunc.huf alt.huf *.out

# -------------------------------------------------------------------------------------
seccion "5. Estrés: repeticiones de ida y vuelta con N y bloque aleatorios"
REPS="${HUF_STRESS_REPS:-40}"   # el spec pide 200; configurable para el entorno
malo=0
for i in $(seq 1 "$REPS"); do
    sz=$(( (RANDOM % 200000) + 1 ))
    case $((RANDOM % 3)) in
        0) gen_texto "$sz" s.bin ;;
        1) gen_aleatorio "$sz" s.bin ;;
        2) gen_unbyte "$sz" s.bin ;;
    esac
    N=$(( (RANDOM % 8) + 1 )); B=$(( (RANDOM % 3072) + 1024 ))
    "$CLI" C s.bin s.huf "$N" "$B" >/dev/null 2>&1 && "$CLI" X s.huf s.out "$N" >/dev/null 2>&1
    igual s.bin s.out || { malo=1; echo "    fallo en rep $i (sz=$sz N=$N B=$B)"; break; }
    rm -f s.bin s.huf s.out
done
[ "$malo" -eq 0 ] && ok "$REPS repeticiones de ida y vuelta sin fallos" || bad "falló una repetición de estrés"
rm -f s.bin s.huf s.out

# -------------------------------------------------------------------------------------
seccion "6. Rendimiento: secuencial vs paralelo (tabla de speedup)"
# El spec pide >=100 MB; aquí se usa un tamaño configurable (por defecto 20 MB)
# para que la tabla salga rápido en el entorno de pruebas.
PERF_MB="${HUF_PERF_MB:-20}"
gen_texto $((PERF_MB * 1024 * 1024)) perf.bin
printf '    %-10s %-12s %-10s\n' "modo" "tiempo(s)" "speedup"
t_seq=""
mide() {  # mide <etiqueta> <comando...>
    local etiqueta="$1"; shift
    local ini fin dt
    ini=$(date +%s.%N)
    "$@" >/dev/null 2>&1
    fin=$(date +%s.%N)
    dt=$(awk "BEGIN{printf \"%.3f\", $fin-$ini}")
    echo "$dt"
}
# Secuencial (etapa 1)
t_seq=$(mide seq "$CLI" c perf.bin perf.huf 65536)
printf '    %-10s %-12s %-10s\n' "secuencial" "$t_seq" "1.00x"
for N in 1 2 4 8; do
    t=$(mide "par$N" "$CLI" C perf.bin perf.huf "$N" 65536)
    sp=$(awk "BEGIN{ if ($t>0) printf \"%.2fx\", $t_seq/$t; else printf \"-\" }")
    printf '    %-10s %-12s %-10s\n' "paralelo N=$N" "$t" "$sp"
done
# Verificar que la última salida paralela se descomprime bien.
"$CLI" X perf.huf perf.out 4 >/dev/null 2>&1
igual perf.bin perf.out && ok "la salida de la medición de rendimiento es correcta" || bad "la salida de rendimiento no coincide"
rm -f perf.bin perf.huf perf.out

# -------------------------------------------------------------------------------------
printf '\n==================== RESUMEN HUF: %d PASS, %d FAIL ====================\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
