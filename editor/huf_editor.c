#include "huf_editor.h"
#include "huffman.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef COLOR_RESET
#define COLOR_RESET     "\033[0m"
#define COLOR_RESULT    "\033[1;32m"
#define COLOR_ERROR     "\033[1;31m"
#define COLOR_INFO      "\033[0;90m"
#endif

/* ------------------------------------------------------------------------------------
 * SIGINT: bandera global activada SOLO por el manejador. Un manejador de señal no
 * puede llamar a pthread_mutex_lock/pthread_cond_signal ni a printf (no son
 * async-signal-safe), así que lo único que hace es poner la bandera; el REPL la
 * consulta con
 * he_sigint_pendiente() y traduce a cancelación por el flujo normal.
 * ------------------------------------------------------------------------------------ */
static volatile sig_atomic_t g_sigint = 0;

static void manejador_sigint(int sig) {
    (void)sig;
    g_sigint = 1;
}

void he_instalar_sigint(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = manejador_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;   /* sin SA_RESTART: así una syscall lenta se interrumpe y el bucle revisa la bandera */
    sigaction(SIGINT, &sa, NULL);
}

int he_sigint_pendiente(void) {
    if (g_sigint) {
        g_sigint = 0;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------------------
 * Estructura del gestor
 * ------------------------------------------------------------------------------------ */
struct HufEditor {
    /* --- Máquina de estados del trabajo --- */
    pthread_mutex_t estado_mtx;   /* protege todo lo de este bloque */
    pthread_cond_t  estado_cv;    /* se señaliza en cada transición y en progreso */
    HeEstado estado;
    int es_compresion;            /* 1=comprimir, 0=descomprimir */
    char *ruta_in;
    char *ruta_out;
    uint32_t total_bloques;
    uint32_t hechos;
    int ultimo_decimal;           /* último múltiplo de 10% ya impreso */
    /* Bandera que el motor concurrente lee entre bloques, desde hilos distintos
     * del que la escribe: debe ser ATÓMICA (no 'volatile' leída sin lock, que no
     * garantiza atomicidad entre hilos). Se accede con __atomic_* (orden
     * relaxed: solo importa la publicación del cambio 0->1, no un ordenamiento
     * con otras memorias). */
    int cancelar;                 /* 0/1, accedida con __atomic_load/store */
    int hay_hilo;                 /* 1 si 'orq' es un hilo unible pendiente de join */
    pthread_t orq;                /* hilo orquestador */

    /* --- Serialización de la salida por stdout --- */
    pthread_mutex_t salida_mtx;
};

#define ES_COMPRESION(he) ((he)->es_compresion)

/* ------------------------------------------------------------------------------------
 * Salida serializada
 * ------------------------------------------------------------------------------------ */
static void he_printf(HufEditor *he, const char *fmt, ...) {
    pthread_mutex_lock(&he->salida_mtx);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    pthread_mutex_unlock(&he->salida_mtx);
}

/* Diagnóstico a stderr con vfprintf (no fprintf con literal) para no importar el
 * símbolo fwrite, que una prueba del editor prohíbe (nm -u). */
static void he_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static const char *nombre_estado(HeEstado e) {
    switch (e) {
    case HE_IDLE:       return "ninguno";
    case HE_RUNNING:    return "en curso";
    case HE_CANCELLING: return "cancelando";
    case HE_DONE:       return "terminado";
    case HE_FAILED:     return "falló";
    case HE_CANCELLED:  return "cancelado";
    }
    return "?";
}

/* ------------------------------------------------------------------------------------
 * Callback de progreso (lo llama el hilo que ensambla/decodifica). Actualiza el
 * contador bajo estado_mtx y, al cruzar un múltiplo de 10%, imprime una línea.
 * ------------------------------------------------------------------------------------ */
static void cb_progreso(uint32_t hechos, uint32_t total, void *dato) {
    HufEditor *he = dato;
    int imprimir = 0;
    int decimal = 0;
    pthread_mutex_lock(&he->estado_mtx);
    he->hechos = hechos;
    he->total_bloques = total;
    if (total > 0) {
        int pct = (int)((uint64_t)hechos * 100 / total);
        decimal = pct / 10 * 10;
        if (decimal > he->ultimo_decimal && decimal < 100) {
            he->ultimo_decimal = decimal;
            imprimir = 1;
        }
    }
    pthread_cond_broadcast(&he->estado_cv);
    int es_comp = ES_COMPRESION(he);
    char *ruta = he->ruta_in;
    pthread_mutex_unlock(&he->estado_mtx);

    if (imprimir) {
        he_printf(he, "\n" COLOR_INFO "[huf] %d %% (%s %s)" COLOR_RESET "\n",
                  decimal, es_comp ? "comprimiendo" : "descomprimiendo", ruta ? ruta : "");
    }
}

/* ------------------------------------------------------------------------------------
 * Hilo orquestador: corre el motor y publica el estado final.
 * ------------------------------------------------------------------------------------ */
static void *orquestador(void *arg) {
    HufEditor *he = arg;

    /* Bloquear SIGINT en este hilo: lo maneja el hilo principal (REPL). */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    HufOpc opc;
    memset(&opc, 0, sizeof(opc));
    opc.nhilos = 0;                 /* automático */
    opc.tam_bloque = 0;             /* por defecto */
    opc.progreso = cb_progreso;
    opc.dato = he;
    opc.cancelar = &he->cancelar;   /* el motor lo revisa entre bloques */

    /* La exclusión frente a a/d/i NO usa locks: a/d/i (y c/x) corren siempre en
     * el MISMO hilo del REPL, y el estado ya quedó en RUNNING en lanzar() antes
     * de crear este hilo. Mientras el estado sea RUNNING/CANCELLING, a/d/i se
     * rechazan (he_inicio_edicion consulta el estado bajo estado_mtx). No hay
     * ventana porque el REPL es monohilo: entre que c/x retorna y el usuario
     * teclea a/d/i no se ejecuta nada más del REPL. */
    int r;
    if (ES_COMPRESION(he)) {
        r = huf_comprimir_concurrente(he->ruta_in, he->ruta_out, &opc);
    } else {
        r = huf_descomprimir_concurrente(he->ruta_in, he->ruta_out, &opc);
    }

    pthread_mutex_lock(&he->estado_mtx);
    HeEstado final;
    if (r == 0) {
        final = HE_DONE;
    } else if (r == 1) {
        final = HE_CANCELLED;
    } else {
        final = HE_FAILED;
    }
    he->estado = final;
    pthread_cond_broadcast(&he->estado_cv);
    int es_comp = ES_COMPRESION(he);
    char *out = he->ruta_out ? strdup(he->ruta_out) : NULL;
    pthread_mutex_unlock(&he->estado_mtx);

    if (final == HE_DONE) {
        he_printf(he, "\n" COLOR_RESULT "[huf] 100 %% — %s terminada: %s" COLOR_RESET "\n",
                  es_comp ? "compresión" : "descompresión", out ? out : "");
    } else if (final == HE_CANCELLED) {
        he_printf(he, "\n" COLOR_INFO "[huf] trabajo cancelado; sin salida." COLOR_RESET "\n");
    } else {
        he_printf(he, "\n" COLOR_ERROR "[huf] el trabajo falló." COLOR_RESET "\n");
    }
    free(out);
    return NULL;
}

/* ------------------------------------------------------------------------------------
 * Ciclo de vida
 * ------------------------------------------------------------------------------------ */
HufEditor *he_crear(void) {
    HufEditor *he = calloc(1, sizeof(HufEditor));
    if (he == NULL) {
        perror("calloc");
        return NULL;
    }
    he->estado = HE_IDLE;
    if (pthread_mutex_init(&he->estado_mtx, NULL) != 0 ||
        pthread_cond_init(&he->estado_cv, NULL) != 0 ||
        pthread_mutex_init(&he->salida_mtx, NULL) != 0) {
        he_err(COLOR_ERROR "huf: no se pudieron crear las primitivas de sincronización" COLOR_RESET "\n");
        free(he);
        return NULL;
    }
    return he;
}

/* Une el hilo orquestador si quedó pendiente. Debe llamarse SIN estado_mtx. */
static void unir_hilo(HufEditor *he) {
    pthread_mutex_lock(&he->estado_mtx);
    int unir = he->hay_hilo;
    he->hay_hilo = 0;
    pthread_mutex_unlock(&he->estado_mtx);
    if (unir) {
        pthread_join(he->orq, NULL);
    }
}

void he_destruir(HufEditor *he) {
    if (he == NULL) {
        return;
    }
    /* Cancelar y unir si hay trabajo activo (q/EOF o fin de sesión). */
    pthread_mutex_lock(&he->estado_mtx);
    if (he->estado == HE_RUNNING || he->estado == HE_CANCELLING) {
        __atomic_store_n(&he->cancelar, 1, __ATOMIC_RELAXED);
        he->estado = HE_CANCELLING;
        pthread_cond_broadcast(&he->estado_cv);
    }
    pthread_mutex_unlock(&he->estado_mtx);

    unir_hilo(he);

    free(he->ruta_in);
    free(he->ruta_out);
    pthread_mutex_destroy(&he->salida_mtx);
    pthread_cond_destroy(&he->estado_cv);
    pthread_mutex_destroy(&he->estado_mtx);
    free(he);
}

/* Arranque común de un trabajo. Retorna 0 si lanzó, -1 si no. */
static int lanzar(HufEditor *he, int es_compresion, const char *in, const char *out) {
    pthread_mutex_lock(&he->estado_mtx);
    if (he->estado == HE_RUNNING || he->estado == HE_CANCELLING) {
        pthread_mutex_unlock(&he->estado_mtx);
        he_printf(he, COLOR_ERROR "Error: ya hay un trabajo Huffman en curso (usa 't', 'w' o 'k')." COLOR_RESET "\n");
        return -1;
    }
    /* Recoger un trabajo anterior ya terminado antes de empezar otro. */
    pthread_mutex_unlock(&he->estado_mtx);
    unir_hilo(he);
    pthread_mutex_lock(&he->estado_mtx);

    free(he->ruta_in);
    free(he->ruta_out);
    he->ruta_in = strdup(in);
    he->ruta_out = strdup(out);
    if (he->ruta_in == NULL || he->ruta_out == NULL) {
        perror("strdup");
        free(he->ruta_in); free(he->ruta_out);
        he->ruta_in = he->ruta_out = NULL;
        pthread_mutex_unlock(&he->estado_mtx);
        return -1;
    }
    he->es_compresion = es_compresion;
    /* El estado pasa a RUNNING AQUÍ, antes de crear el hilo. Como a/d/i y c/x
     * corren en el mismo hilo del REPL, cuando esta función retorne al prompt el
     * estado ya es RUNNING y cualquier a/d/i posterior se rechaza: no hay ventana
     * ni hace falta ningún lock de edición. */
    he->estado = HE_RUNNING;
    __atomic_store_n(&he->cancelar, 0, __ATOMIC_RELAXED);
    he->hechos = 0;
    he->total_bloques = 0;
    he->ultimo_decimal = 0;

    int rc = pthread_create(&he->orq, NULL, orquestador, he);
    if (rc != 0) {
        he_err(COLOR_ERROR "pthread_create: %s" COLOR_RESET "\n", strerror(rc));
        he->estado = HE_IDLE;
        free(he->ruta_in); free(he->ruta_out);
        he->ruta_in = he->ruta_out = NULL;
        pthread_mutex_unlock(&he->estado_mtx);
        return -1;
    }
    he->hay_hilo = 1;
    pthread_mutex_unlock(&he->estado_mtx);

    he_printf(he, COLOR_INFO "[huf] %s en segundo plano: %s -> %s" COLOR_RESET "\n",
              es_compresion ? "compresión" : "descompresión", in, out);
    return 0;
}

int he_comprimir(HufEditor *he, const char *ruta_entrada, const char *ruta_salida) {
    return lanzar(he, 1, ruta_entrada, ruta_salida);
}

int he_descomprimir(HufEditor *he, const char *ruta_entrada, const char *ruta_salida) {
    return lanzar(he, 0, ruta_entrada, ruta_salida);
}

void he_estado_imprimir(HufEditor *he) {
    pthread_mutex_lock(&he->estado_mtx);
    HeEstado e = he->estado;
    uint32_t hechos = he->hechos, total = he->total_bloques;
    int es_comp = ES_COMPRESION(he);
    char *in = he->ruta_in ? strdup(he->ruta_in) : NULL;
    pthread_mutex_unlock(&he->estado_mtx);

    int pct = total > 0 ? (int)((uint64_t)hechos * 100 / total) : (e == HE_DONE ? 100 : 0);
    if (e == HE_IDLE) {
        he_printf(he, COLOR_INFO "[huf] estado: ninguno" COLOR_RESET "\n");
    } else if (e == HE_RUNNING || e == HE_CANCELLING) {
        he_printf(he, COLOR_INFO "[huf] estado: %s (%s %s) %d %%" COLOR_RESET "\n",
                  nombre_estado(e), es_comp ? "comprimiendo" : "descomprimiendo",
                  in ? in : "", pct);
    } else {
        he_printf(he, COLOR_INFO "[huf] estado: %s (%s %s) %d %%" COLOR_RESET "\n",
                  nombre_estado(e), es_comp ? "compresión" : "descompresión",
                  in ? in : "", pct);
    }
    free(in);
}

void he_cancelar(HufEditor *he) {
    pthread_mutex_lock(&he->estado_mtx);
    if (he->estado == HE_RUNNING) {
        __atomic_store_n(&he->cancelar, 1, __ATOMIC_RELAXED);
        he->estado = HE_CANCELLING;
        pthread_cond_broadcast(&he->estado_cv);
        pthread_mutex_unlock(&he->estado_mtx);
        he_printf(he, COLOR_INFO "[huf] cancelación solicitada." COLOR_RESET "\n");
    } else {
        HeEstado e = he->estado;
        pthread_mutex_unlock(&he->estado_mtx);
        if (e == HE_CANCELLING) {
            he_printf(he, COLOR_INFO "[huf] ya se está cancelando." COLOR_RESET "\n");
        } else {
            he_printf(he, COLOR_INFO "[huf] no hay trabajo que cancelar." COLOR_RESET "\n");
        }
    }
}

void he_esperar(HufEditor *he) {
    pthread_mutex_lock(&he->estado_mtx);
    while (he->estado == HE_RUNNING || he->estado == HE_CANCELLING) {
        pthread_cond_wait(&he->estado_cv, &he->estado_mtx);
    }
    pthread_mutex_unlock(&he->estado_mtx);
    /* Recoger el hilo terminado. */
    unir_hilo(he);
}

int he_trabajo_activo(HufEditor *he) {
    pthread_mutex_lock(&he->estado_mtx);
    int activo = (he->estado == HE_RUNNING || he->estado == HE_CANCELLING);
    pthread_mutex_unlock(&he->estado_mtx);
    return activo;
}

int he_inicio_edicion(HufEditor *he) {
    /* La exclusión con el trabajo se decide SOLO por el estado, bajo estado_mtx:
     * si hay un trabajo activo (RUNNING o CANCELLING) la edición se rechaza. No
     * se usa ningún rwlock porque el trabajo lee el archivo con su PROPIO fd y
     * a/d/i y c/x corren siempre en el mismo hilo del REPL; el estado ya es
     * RUNNING antes de que lanzar() retorne, así que no existe ventana de
     * carrera. */
    pthread_mutex_lock(&he->estado_mtx);
    int activo = (he->estado == HE_RUNNING || he->estado == HE_CANCELLING);
    pthread_mutex_unlock(&he->estado_mtx);
    return activo ? -1 : 0;
}

void he_fin_edicion(HufEditor *he) {
    /* Nada que liberar: la decisión fue instantánea bajo estado_mtx. Se conserva
     * por simetría con he_inicio_edicion y para no tocar los llamadores. */
    (void)he;
}
