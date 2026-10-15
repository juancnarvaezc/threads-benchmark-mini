/*
===========================================================
 comun.h

 Funciones comunes a los experimentos:
  - la carga CPU-bound del laboratorio (seccion 8.1)
  - medir tiempo de pared (reloj monotono)
  - leer el tiempo de CPU que lleva consumido el proceso

 Lo incluyen los demas programas con #include "comun.h".
===========================================================
*/

#ifndef COMUN_H
#define COMUN_H

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/resource.h>

/* Iteraciones TOTALES de la carga. Es el trabajo total del
   experimento: cuando hay varios hilos o procesos, este numero se
   reparte entre ellos. Asi todos hacen juntos lo mismo que hace el
   secuencial y los tiempos se pueden comparar.

   Se puede cambiar por linea de comandos, por ejemplo:
      ./benchmark 50000000
   Ajustalo para que la version secuencial tarde unos segundos. */
#define ITERACIONES_DEFECTO 1000000000ULL

/* Reparto del trabajo: cada trabajador recibe su parte y el ultimo
   se lleva ademas el resto, para no perder iteraciones. */
static inline unsigned long long parte_de(unsigned long long total,
                                          int trabajadores, int indice) {
    unsigned long long base = total / (unsigned long long)trabajadores;

    if (indice == trabajadores - 1)
        return total - base * (unsigned long long)(trabajadores - 1);

    return base;
}

/* ------------------------------------------------------------
   Carga CPU-bound del laboratorio (seccion 8.1).

   El acumulador es LOCAL a cada hilo o proceso a proposito: si
   fuera un volatile global compartido, todos los hilos
   escribirian la misma variable, se pelearian por la cache y el
   paralelismo se arruinaria. El volatile local basta para que el
   compilador no borre el trabajo, y se devuelve el valor para
   asegurar que se calcule de verdad.
   ------------------------------------------------------------ */
static inline unsigned long long trabajo_cpu(unsigned long long iteraciones) {
    volatile unsigned long long acumulado = 0;

    for (unsigned long long i = 0; i < iteraciones; ++i) {
        acumulado += (i % 97) * (i % 53);
        acumulado ^= (i << 3);
    }

    return acumulado;
}

/* Tiempo de pared en segundos: el que mediria un cronometro. */
static inline double ahora(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1000000000.0;
}

/* Tiempo de CPU (usuario + sistema) consumido hasta el momento.
   Con incluir_hijos = 1 suma ademas el de los procesos hijos ya
   terminados, que es lo que hace falta en el experimento de fork(). */
static inline double tiempo_cpu(int incluir_hijos) {
    struct rusage uso;

    getrusage(incluir_hijos ? RUSAGE_CHILDREN : RUSAGE_SELF, &uso);

    double usuario = uso.ru_utime.tv_sec + uso.ru_utime.tv_usec / 1000000.0;
    double sistema = uso.ru_stime.tv_sec + uso.ru_stime.tv_usec / 1000000.0;

    return usuario + sistema;
}

#endif /* COMUN_H */
