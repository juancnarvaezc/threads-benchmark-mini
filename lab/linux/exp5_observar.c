/*
===========================================================
 exp5_observar.c

 EXPERIMENTO 5: observacion del scheduler y del uso de CPU.

 Crea N hilos que se quedan trabajando hasta que se pulse ENTER.
 Imprime el PID del proceso y el TID (identificador) de cada hilo,
 para poder observarlos desde fuera con las herramientas que
 sugiere el laboratorio:

    htop                  (tecla H para mostrar los hilos)
    ps -eLf               (un hilo por linea)
    pidstat -t -p PID 1   (uso de CPU por hilo)
    lscpu -e              (topologia de procesadores logicos)

 Uso:
    ./exp5_observar 8

 Compilar:
    gcc -O2 -Wall -pthread exp5_observar.c -o exp5_observar
===========================================================
*/

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/syscall.h>

#include "comun.h"

/* El hilo principal pone esta bandera a 0 cuando hay que parar. */
static volatile int seguir = 1;

typedef struct {
    int id;
} DatoHilo;

static void *rutina(void *arg) {
    DatoHilo *dato = (DatoHilo *)arg;

    /* Cada hilo tiene su propio identificador, el TID. Es el numero
       que aparece por hilo en ps -eLf o en htop. */
    long tid = (long)syscall(SYS_gettid);

    printf("Hilo %d trabajando (TID %ld)\n", dato->id, tid);

    /* Se trabaja en trozos y entre trozo y trozo se revisa la
       bandera. Los hilos siguen a tope de CPU mientras se observan
       con htop o pidstat, pero paran en cuanto se pide. */
    while (seguir)
        trabajo_cpu(20000000ULL);

    return NULL;
}

int main(int argc, char *argv[]) {
    int n = 4;

    if (argc > 1) {
        n = atoi(argv[1]);

        if (n < 1)
            n = 1;
    }

    long nucleos = sysconf(_SC_NPROCESSORS_ONLN);

    printf("== EXPERIMENTO 5: observacion del scheduler ==\n\n");
    printf("Procesadores logicos: %ld\n", nucleos);
    printf("PID del proceso: %d\n", (int)getpid());
    printf("Creando %d hilos...\n\n", n);

    pthread_t *hilos = malloc(n * sizeof(pthread_t));
    DatoHilo *datos = malloc(n * sizeof(DatoHilo));

    if (hilos == NULL || datos == NULL) {
        printf("Error reservando memoria.\n");
        return EXIT_FAILURE;
    }

    for (int i = 0; i < n; i++) {
        datos[i].id = i;

        if (pthread_create(&hilos[i], NULL, rutina, &datos[i]) != 0) {
            printf("Error creando el hilo %d.\n", i);
            return EXIT_FAILURE;
        }
    }

    printf("\nAbre otra terminal y observa (cambia %d por el PID):\n",
           (int)getpid());
    printf("  htop                  (tecla H para ver los hilos)\n");
    printf("  ps -eLf | grep %d\n", (int)getpid());
    printf("  pidstat -t -p %d 1\n", (int)getpid());
    printf("  lscpu -e\n\n");
    printf("Pulsa ENTER aqui para detener los hilos...");

    char buffer[8];

    if (fgets(buffer, sizeof(buffer), stdin) == NULL)
        clearerr(stdin);

    seguir = 0;

    for (int i = 0; i < n; i++)
        pthread_join(hilos[i], NULL);

    printf("Hilos detenidos. Fin.\n");

    free(hilos);
    free(datos);
    return 0;
}
