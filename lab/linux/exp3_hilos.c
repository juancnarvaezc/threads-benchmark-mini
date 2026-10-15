/*
===========================================================
 exp3_hilos.c

 EXPERIMENTO 3: creacion de hilos en C con POSIX (seccion 9.1).

 Es el ejemplo del laboratorio casi tal cual: se crean 4 hilos,
 cada uno imprime su numero y termina. Aqui solo se ve como nace
 un hilo y como se espera su final; la carga pesada y las
 mediciones van en benchmark.c.

 Compilar como indica la guia:
    gcc -O0 -Wall -pthread exp3_hilos.c -o exp3_hilos
===========================================================
*/

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define N_HILOS 4

/* Codigo que ejecuta cada hilo. Recibe su numero por argumento. */
void *trabajo(void *arg) {
    int id = *(int *)arg;

    printf("Hilo %d ejecutando...\n", id);

    /* Aqui iria la carga CPU-bound, por ejemplo:
       trabajo_cpu(ITERACIONES_DEFECTO);   con #include "comun.h" */

    return NULL;
}

int main(void) {
    pthread_t hilos[N_HILOS];
    int ids[N_HILOS];

    for (int i = 0; i < N_HILOS; ++i) {
        ids[i] = i;

        /* pthread_create crea el hilo, pero no lo obliga a
           ejecutarse en ese instante: a partir de aqui decide
           el scheduler cuando le toca. */
        if (pthread_create(&hilos[i], NULL, trabajo, &ids[i]) != 0) {
            perror("pthread_create");
            exit(EXIT_FAILURE);
        }
    }

    /* pthread_join hace que el hilo principal ESPERE a que el
    otro termine; no lo ejecuta, solo espera. */
    for (int i = 0; i < N_HILOS; ++i)
        pthread_join(hilos[i], NULL);

    printf("Todos los hilos terminaron.\n");
    return 0;
}
