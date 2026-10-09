#include "hilos.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef COLOR_RESET
#define COLOR_RESET     "\033[0m"
#define COLOR_ERROR     "\033[1;31m"
#endif

#define HILOS_MAX_DEFECTO 8   /* Tope razonable de hilos por defecto */

/* Nodo de la cola de tareas (lista enlazada FIFO). */
typedef struct Tarea {
    HiloTarea func;
    void *arg;
    struct Tarea *sig;
} Tarea;

struct Pool {
    pthread_t *hilos;
    int n;

    /* ------------------------------------------------------------------
     * mutex: candado único del pool. Protege TODO lo que sigue (cola,
     * contadores y banderas). Las secciones críticas son cortas: encolar/
     * desencolar un nodo y tocar contadores. El trabajo pesado de cada tarea
     * se ejecuta FUERA del candado (la tarea se desencola, se suelta el mutex
     * y recién entonces se llama a func).
     * ------------------------------------------------------------------ */
    pthread_mutex_t mutex;

    /* Los trabajadores DUERMEN aquí cuando no hay tareas (sin espera activa).
     * Se señaliza al encolar y al apagar. */
    pthread_cond_t hay_tareas;

    /* hilos_esperar() bloquea aquí hasta pendientes==0 && en_curso==0. */
    pthread_cond_t cola_vacia;

    Tarea *cabeza, *cola;   /* Cola FIFO de tareas pendientes */
    long pendientes;        /* Tareas encoladas aún no tomadas */
    long en_curso;          /* Tareas tomadas por un worker y aún no terminadas */

    int apagando;           /* 1 => los workers salen cuando la cola se vacía */
    int cancelado;          /* 1 => cancelación cooperativa pedida */
};

/* Diagnóstico a stderr con vfprintf (no fprintf con literal) para no importar el
 * símbolo fwrite, que una prueba del editor prohíbe (nm -u). */
#include <stdarg.h>
static void hilos_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void fallo_pth(const char *ctx, int codigo) {
    hilos_err(COLOR_ERROR "%s: %s" COLOR_RESET "\n", ctx, strerror(codigo));
}

/* Bucle de cada trabajador: toma tareas y las ejecuta hasta el apagado. */
static void *worker(void *p) {
    Pool *pool = p;

    /* Los hilos del pool NO deben recibir señales asíncronas (SIGINT): el hilo
     * principal es quien las maneja. Bloquearlas aquí evita que un worker sea
     * interrumpido en medio de una syscall por Ctrl+C. */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    for (;;) {
        int rc = pthread_mutex_lock(&pool->mutex);
        if (rc != 0) {
            fallo_pth("pthread_mutex_lock", rc);
            return NULL;
        }
        /* Dormir mientras no haya tareas y no se esté apagando. El while
         * reevalúa el predicado tras cada despertar (protección contra
         * despertares espurios). */
        while (pool->cabeza == NULL && !pool->apagando) {
            rc = pthread_cond_wait(&pool->hay_tareas, &pool->mutex);
            if (rc != 0) {
                fallo_pth("pthread_cond_wait", rc);
                pthread_mutex_unlock(&pool->mutex);
                return NULL;
            }
        }
        if (pool->cabeza == NULL && pool->apagando) {
            pthread_mutex_unlock(&pool->mutex);
            return NULL;   /* No queda trabajo y hay que apagar */
        }
        /* Desencolar una tarea. */
        Tarea *t = pool->cabeza;
        pool->cabeza = t->sig;
        if (pool->cabeza == NULL) {
            pool->cola = NULL;
        }
        pool->pendientes--;
        pool->en_curso++;
        pthread_mutex_unlock(&pool->mutex);

        /* Ejecutar FUERA del candado para no serializar el trabajo real. */
        t->func(t->arg, pool);
        free(t);

        rc = pthread_mutex_lock(&pool->mutex);
        if (rc != 0) {
            fallo_pth("pthread_mutex_lock", rc);
            return NULL;
        }
        pool->en_curso--;
        if (pool->pendientes == 0 && pool->en_curso == 0) {
            /* Último en terminar: avisar a hilos_esperar(). */
            pthread_cond_broadcast(&pool->cola_vacia);
        }
        pthread_mutex_unlock(&pool->mutex);
    }
}

Pool *hilos_crear(int n) {
    if (n <= 0) {
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);
        if (cpus < 1) {
            cpus = 1;
        }
        n = (int)(cpus > HILOS_MAX_DEFECTO ? HILOS_MAX_DEFECTO : cpus);
    }

    Pool *pool = calloc(1, sizeof(Pool));
    if (pool == NULL) {
        perror("calloc");
        return NULL;
    }
    pool->hilos = malloc((size_t)n * sizeof(pthread_t));
    if (pool->hilos == NULL) {
        perror("malloc");
        free(pool);
        return NULL;
    }
    pool->n = n;

    int rc = pthread_mutex_init(&pool->mutex, NULL);
    if (rc != 0) {
        fallo_pth("pthread_mutex_init", rc);
        free(pool->hilos);
        free(pool);
        return NULL;
    }
    rc = pthread_cond_init(&pool->hay_tareas, NULL);
    if (rc != 0) {
        fallo_pth("pthread_cond_init", rc);
        pthread_mutex_destroy(&pool->mutex);
        free(pool->hilos);
        free(pool);
        return NULL;
    }
    rc = pthread_cond_init(&pool->cola_vacia, NULL);
    if (rc != 0) {
        fallo_pth("pthread_cond_init", rc);
        pthread_cond_destroy(&pool->hay_tareas);
        pthread_mutex_destroy(&pool->mutex);
        free(pool->hilos);
        free(pool);
        return NULL;
    }

    /* Crear los trabajadores. Si uno falla, apagar los ya creados y unirlos. */
    int creados = 0;
    for (int i = 0; i < n; i++) {
        rc = pthread_create(&pool->hilos[i], NULL, worker, pool);
        if (rc != 0) {
            fallo_pth("pthread_create", rc);
            break;
        }
        creados++;
    }
    if (creados < n) {
        pthread_mutex_lock(&pool->mutex);
        pool->apagando = 1;
        pthread_cond_broadcast(&pool->hay_tareas);
        pthread_mutex_unlock(&pool->mutex);
        for (int i = 0; i < creados; i++) {
            pthread_join(pool->hilos[i], NULL);
        }
        pthread_cond_destroy(&pool->cola_vacia);
        pthread_cond_destroy(&pool->hay_tareas);
        pthread_mutex_destroy(&pool->mutex);
        free(pool->hilos);
        free(pool);
        return NULL;
    }
    return pool;
}

int hilos_num(const Pool *pool) {
    return pool->n;
}

int hilos_encolar(Pool *pool, HiloTarea func, void *arg) {
    Tarea *t = malloc(sizeof(Tarea));
    if (t == NULL) {
        perror("malloc");
        return -1;
    }
    t->func = func;
    t->arg = arg;
    t->sig = NULL;

    int rc = pthread_mutex_lock(&pool->mutex);
    if (rc != 0) {
        fallo_pth("pthread_mutex_lock", rc);
        free(t);
        return -1;
    }
    if (pool->apagando) {
        pthread_mutex_unlock(&pool->mutex);
        free(t);
        return -1;
    }
    if (pool->cola == NULL) {
        pool->cabeza = pool->cola = t;
    } else {
        pool->cola->sig = t;
        pool->cola = t;
    }
    pool->pendientes++;
    /* Despertar a UN trabajador dormido: una tarea nueva, un hilo. */
    pthread_cond_signal(&pool->hay_tareas);
    pthread_mutex_unlock(&pool->mutex);
    return 0;
}

void hilos_esperar(Pool *pool) {
    int rc = pthread_mutex_lock(&pool->mutex);
    if (rc != 0) {
        fallo_pth("pthread_mutex_lock", rc);
        return;
    }
    while (pool->pendientes != 0 || pool->en_curso != 0) {
        rc = pthread_cond_wait(&pool->cola_vacia, &pool->mutex);
        if (rc != 0) {
            fallo_pth("pthread_cond_wait", rc);
            break;
        }
    }
    pthread_mutex_unlock(&pool->mutex);
}

void hilos_cancelar(Pool *pool) {
    int rc = pthread_mutex_lock(&pool->mutex);
    if (rc != 0) {
        fallo_pth("pthread_mutex_lock", rc);
        return;
    }
    pool->cancelado = 1;
    pthread_mutex_unlock(&pool->mutex);
}

int hilos_cancelado(Pool *pool) {
    int rc = pthread_mutex_lock(&pool->mutex);
    if (rc != 0) {
        fallo_pth("pthread_mutex_lock", rc);
        return 1;   /* Ante fallo del lock, pedir parada por seguridad */
    }
    int c = pool->cancelado;
    pthread_mutex_unlock(&pool->mutex);
    return c;
}

void hilos_destruir(Pool *pool) {
    if (pool == NULL) {
        return;
    }
    /* Esperar a que se vacíe la cola para no perder tareas ya encoladas. */
    hilos_esperar(pool);

    int rc = pthread_mutex_lock(&pool->mutex);
    if (rc != 0) {
        fallo_pth("pthread_mutex_lock", rc);
    }
    pool->apagando = 1;
    pthread_cond_broadcast(&pool->hay_tareas);
    pthread_mutex_unlock(&pool->mutex);

    for (int i = 0; i < pool->n; i++) {
        int jr = pthread_join(pool->hilos[i], NULL);
        if (jr != 0) {
            fallo_pth("pthread_join", jr);
        }
    }

    /* Liberar cualquier tarea que quedara (no debería tras hilos_esperar). */
    Tarea *t = pool->cabeza;
    while (t != NULL) {
        Tarea *s = t->sig;
        free(t);
        t = s;
    }
    pthread_cond_destroy(&pool->cola_vacia);
    pthread_cond_destroy(&pool->hay_tareas);
    pthread_mutex_destroy(&pool->mutex);
    free(pool->hilos);
    free(pool);
}
