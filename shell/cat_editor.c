#include "shell.h"
#include "editor.h"
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/**
 * ====================================================================================
 * COMANDO: e_edit [archivo]
 * ====================================================================================
 * Categoría nueva: "editor". Lanza el editor de líneas (../editor) como una
 * sub-sesión interactiva con su propio prompt 'editor>' y sus propios comandos
 * (o, p, a, d, i, s, q).
 *
 * ¿Por qué en un proceso hijo?
 * - El comando 'q' del editor debe hacer close() + exit(). Si el editor corriera
 *   dentro del proceso del shell, ese exit() mataría también al shell.
 * - Con fork(2) el editor vive en su propio espacio de direcciones: su descriptor
 *   de archivo, su lista enlazada y su heap desaparecen al terminar el hijo, sin
 *   poder dejar fugas ni descriptores abiertos dentro del shell.
 * - El padre se bloquea en waitpid(2) y reporta el código de salida del editor
 *   (0 = close() exitoso).
 *
 * Syscalls: fork(2), waitpid(2) en el shell; open, read, write, lseek, ftruncate,
 * close y exit en el editor (trazadas igual que el resto del shell).
 */
int cmd_e_edit(int argc, char **argv) {
    if (argc > 2) {
        fprintf(stderr, COLOR_ERROR "Uso: e_edit [archivo]\n" COLOR_RESET);
        return 1;
    }

    /* Ctrl+C envía SIGINT a todo el grupo de procesos del terminal (shell y
     * editor). El padre lo ignora mientras espera, para que la interrupción
     * termine solo al editor; cada comando del editor ya quedó persistido en
     * disco, así que no se pierde nada. El hijo restaura la acción original. */
    struct sigaction ignorar, previa;
    memset(&ignorar, 0, sizeof(ignorar));
    ignorar.sa_handler = SIG_IGN;
    sigemptyset(&ignorar.sa_mask);
    sigaction(SIGINT, &ignorar, &previa);

    /* Vaciar stdout ANTES de fork: si no, el hijo heredaría una copia del buffer
     * pendiente y el texto se imprimiría dos veces. */
    LOG_SYSCALL("fork", "");
    fflush(stdout);
    pid_t pid = fork();
    if (pid == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        perror("fork");
        sigaction(SIGINT, &previa, NULL);
        return 1;
    }

    if (pid == 0) {
        /* Proceso hijo: ejecuta el editor. editor_run() nunca retorna. */
        sigaction(SIGINT, &previa, NULL);
        printf("= " COLOR_RESULT "0" COLOR_RESET " (hijo, pid %d)\n", getpid());
        ed_traza = 1;
        editor_run(argc == 2 ? argv[1] : NULL);
    }

    /* Proceso padre: esperar a que el usuario salga del editor con 'q'. */
    int status;
    pid_t esperado;
    do {
        esperado = waitpid(pid, &status, 0);
    } while (esperado == -1 && errno == EINTR);
    sigaction(SIGINT, &previa, NULL);
    LOG_SYSCALL("waitpid", "%d, &status, 0", pid);
    if (esperado == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        perror("waitpid");
        return 1;
    }
    LOG_SYSCALL_RESULT(esperado);

    if (WIFEXITED(status)) {
        printf(COLOR_INFO "Editor (pid %d) terminó con código %d. De vuelta en el shell.\n" COLOR_RESET,
               pid, WEXITSTATUS(status));
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        printf(COLOR_ERROR "Editor (pid %d) terminado por la señal %d.\n" COLOR_RESET, pid, WTERMSIG(status));
    }
    return 1;
}
