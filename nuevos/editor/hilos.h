#ifndef HILOS_H
#define HILOS_H

#include <stddef.h>

/**
 * ====================================================================================
 * POOL DE HILOS GENÉRICO Y REUTILIZABLE (módulo hilos_)
 * ====================================================================================
 * Pool de N trabajadores POSIX con cola de tareas. Pensado para reutilizarse en
 * las distintas fases del compresor Huffman (conteo, codificación, decodifica-
 * ción), pero sin depender de nada de Huffman: solo ejecuta funciones.
 *
 * SINCRONIZACIÓN (ver docs/CONCURRENCIA.md para el detalle y el porqué):
 *   - 1 pthread_mutex_t protege la cola de tareas, el contador de tareas en
 *     curso, la bandera de apagado y la bandera de cancelación.
 *   - pthread_cond_t "hay_tareas": los trabajadores DUERMEN bloqueados en ella
 *     cuando la cola está vacía (nada de espera activa).
 *   - pthread_cond_t "cola_vacia": permite a hilos_esperar() bloquear hasta que
 *     no queden tareas pendientes ni en curso.
 *   - Apagado ordenado: hilos_destruir() marca 'apagando' bajo el mutex, hace
 *     broadcast y UNE con pthread_join a los N trabajadores. NO se usa
 *     pthread_detach.
 *
 * CANCELACIÓN COOPERATIVA: hilos_cancelar() activa una bandera que las tareas
 * consultan con hilos_cancelado() ENTRE unidades de trabajo (p. ej. entre
 * bloques). Una tarea cancelada debe terminar pronto; el pool queda limpio.
 *
 * Toda llamada a pthread_* y a malloc verifica su retorno; ante error se limpia
 * y se propaga -1.
 */

typedef struct Pool Pool;

/* Firma de una tarea. 'arg' es el dato de la tarea; 'pool' permite consultar la
 * cancelación con hilos_cancelado(pool). */
typedef void (*HiloTarea)(void *arg, Pool *pool);

/* Crea un pool con 'n' trabajadores (n >= 1). Retorna NULL en error (mensaje ya
 * reportado). Si n == 0 se usa un valor por defecto basado en el número de CPUs. */
Pool *hilos_crear(int n);

/* Número de trabajadores del pool. */
int hilos_num(const Pool *pool);

/* Encola una tarea (func, arg). Retorna 0 en éxito, -1 si falla malloc o el pool
 * está apagando. Despierta a un trabajador dormido. */
int hilos_encolar(Pool *pool, HiloTarea func, void *arg);

/* Bloquea (en condvar, sin espera activa) hasta que no queden tareas pendientes
 * ni en curso. Útil como barrera de fin de fase. */
void hilos_esperar(Pool *pool);

/* Marca cancelación cooperativa. Las tareas la observan con hilos_cancelado().
 * No interrumpe una tarea en curso: solo evita empezar más trabajo. */
void hilos_cancelar(Pool *pool);

/* 1 si se pidió cancelación, 0 si no. Las tareas lo consultan entre bloques. */
int hilos_cancelado(Pool *pool);

/* Apagado ordenado: espera a que se vacíe la cola, marca apagado, despierta a
 * todos y los UNE con pthread_join. Libera el pool. Tras esta llamada 'pool'
 * deja de ser válido. Idempotente frente a NULL. */
void hilos_destruir(Pool *pool);

#endif /* HILOS_H */
