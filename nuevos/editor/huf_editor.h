#ifndef HUF_EDITOR_H
#define HUF_EDITOR_H

/**
 * ====================================================================================
 * ORQUESTADOR DEL TRABAJO HUFFMAN EN SEGUNDO PLANO (integración con el editor)
 * ====================================================================================
 * Puente entre el REPL del editor y el motor concurrente de huffman.c. Vive en
 * archivos nuevos para que editor.c solo reciba cambios mínimos y aditivos.
 *
 * Un solo trabajo (compresión o descompresión) puede estar activo a la vez. El
 * REPL lanza c/x y vuelve DE INMEDIATO al prompt: un hilo ORQUESTADOR (distinto
 * del REPL) corre las fases del motor y publica el estado. Los comandos t/k/w
 * consultan/cancelan/esperan ese trabajo.
 *
 * SINCRONIZACIÓN (detalle y justificación en docs/CONCURRENCIA.md):
 *   - Un mutex 'estado_mtx' + condvar 'estado_cv' protegen la máquina de estados
 *     del trabajo. Toda transición y toda consulta (t, k, w, q) pasan por ahí.
 *   - La carrera con la edición se arbitra SOLO por el estado (sin rwlock): a/d/i
 *     consultan el estado bajo estado_mtx y, si hay un trabajo activo (RUNNING o
 *     CANCELLING), se rechazan. No hace falta un lock de archivo porque el
 *     trabajo lee con su PROPIO fd y a/d/i, c/x corren todos en el mismo hilo del
 *     REPL; el estado ya es RUNNING antes de que c/x retorne, así que no hay
 *     ventana en la que un worker lea bytes que a/d/i estén desplazando.
 *   - Un mutex 'salida_mtx' serializa TODA la salida por stdout (prompt del REPL
 *     y líneas de progreso del orquestador) para que no se entremezclen.
 *   - La bandera de cancelación es ATÓMICA (__atomic_*), no 'volatile': el motor
 *     la lee entre bloques desde varios hilos.
 *   - Una bandera 'volatile sig_atomic_t' recibe SIGINT; el manejador SOLO la
 *     activa (no llama a funciones no async-signal-safe). La cancelación real la
 *     hace el flujo normal del REPL/orquestador.
 *
 * ORDEN DE ADQUISICIÓN DE LOCKS (para evitar interbloqueos): hay dos mutex y
 * nunca se anidan uno dentro del otro. 'salida_mtx' es una hoja (se toma y se
 * suelta sin tomar ningún otro lock dentro) y 'estado_mtx' igual: cuando se
 * necesita imprimir, primero se suelta estado_mtx y luego se llama a la salida.
 * Al no anidarse, no puede haber ciclo ni inversión de orden. Ver
 * CONCURRENCIA.md §orden de locks.
 */

/* Estados observables del trabajo. */
typedef enum {
    HE_IDLE = 0,     /* No hay trabajo */
    HE_RUNNING,      /* Trabajo en curso */
    HE_CANCELLING,   /* Se pidió cancelación; aún no termina */
    HE_DONE,         /* Terminó bien */
    HE_FAILED,       /* Terminó con error */
    HE_CANCELLED     /* Terminó por cancelación */
} HeEstado;

typedef struct HufEditor HufEditor;

/* Crea el gestor (una sola instancia por sesión de editor). NULL en error. */
HufEditor *he_crear(void);

/* Destruye el gestor: si hay trabajo, lo cancela y UNE el hilo orquestador
 * (pthread_join); libera todo. Idempotente frente a NULL. */
void he_destruir(HufEditor *he);

/* Lanza en segundo plano la compresión de 'ruta_entrada' hacia 'ruta_salida'.
 * Retorna 0 si arrancó, -1 si no (p. ej. ya hay un trabajo). Mensajes ya
 * emitidos de forma serializada. No bloquea el REPL. */
int he_comprimir(HufEditor *he, const char *ruta_entrada, const char *ruta_salida);

/* Igual para descompresión. 'ruta_salida' nunca debe ser el archivo abierto en
 * el editor (lo valida el llamador). */
int he_descomprimir(HufEditor *he, const char *ruta_entrada, const char *ruta_salida);

/* Comando 't': imprime el estado del trabajo con porcentaje (serializado). */
void he_estado_imprimir(HufEditor *he);

/* Comando 'k': pide cancelación cooperativa si hay trabajo. No bloquea. */
void he_cancelar(HufEditor *he);

/* Comando 'w': BLOQUEA (en condvar) hasta que el trabajo activo termine. Si no
 * hay trabajo, retorna de inmediato. Tras terminar, recoge el hilo (join). */
void he_esperar(HufEditor *he);

/* ¿Hay un trabajo activo (RUNNING o CANCELLING)? Lo consulta el guardia de a/d/i. */
int he_trabajo_activo(HufEditor *he);

/* Guardia de edición para a/d/i. Retorna 0 si NO hay trabajo activo (la edición
 * puede proceder) y -1 si hay un trabajo activo (la edición se rechaza). La
 * decisión se toma leyendo el estado bajo estado_mtx; no usa ningún lock de
 * archivo. he_fin_edicion no hace nada (se conserva por simetría con los
 * llamadores). */
int he_inicio_edicion(HufEditor *he);
void he_fin_edicion(HufEditor *he);

/* Instala el manejador de SIGINT (sigaction) que solo activa la bandera. Lo
 * llama el editor al arrancar (ejecutable propio) y cat_editor.c en el hijo. */
void he_instalar_sigint(void);

/* ¿Se recibió SIGINT desde la última consulta? El REPL lo revisa para traducirlo
 * a cancelación cooperativa del trabajo. */
int he_sigint_pendiente(void);

#endif /* HUF_EDITOR_H */
