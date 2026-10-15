/*
===========================================================
 benchmark.c

 EXPERIMENTOS 2, 4, 6 y 7 del laboratorio.

 Ejecuta la misma carga CPU-bound (comun.h) de tres formas:
   - secuencial (un solo hilo)
   - varios hilos     (pthread_create + pthread_join)
   - varios procesos  (fork + waitpid)

 El trabajo total se REPARTE entre los trabajadores, asi que con
 mas hilos el tiempo deberia bajar mientras haya procesadores
 logicos libres: eso es lo que hay que medir e interpretar.

 El tiempo es de pared (CLOCK_MONOTONIC) y el %CPU es el consumo
 de CPU (usuario + sistema) dividido por el tiempo de pared, el
 mismo criterio que muestra top para un proceso.

 Compilar:
    gcc -O2 -Wall -pthread benchmark.c -o benchmark

 Uso:
    ./benchmark              (menu)
    ./benchmark 50000000     (cambia las iteraciones de la carga)
===========================================================
*/

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sched.h>

#include "comun.h"

/* Trabajadores maximos que se pueden fijar a mano. Puede ser mayor
   que los nucleos logicos a proposito: sirve para responder que
   pasa cuando hay mas hilos que procesadores logicos (pregunta 8
   del analisis). */
#define MAX_TRABAJADORES 64

/* Repeticiones maximas de la tabla del experimento 7. */
#define MAX_REPETICIONES 10

/* Espacio para las configuraciones del barrido (1,2,4,8,16...). */
#define CONFIGS_BARRIDO 8

static long nucleos_logicos = 1;

/* 0 = automatico: tantos trabajadores como nucleos logicos. */
static int num_trabajadores = 0;

static unsigned long long iteraciones = ITERACIONES_DEFECTO;
static int repeticiones = 3;

/* 1 = pausa antes de cada prueba, para mirarlo con htop. */
static int pausar = 0;

/* Afinidad con la que arranco el proceso, para poder volver a ella
   despues de la prueba de un solo nucleo. */
static cpu_set_t afinidad_original;

/* Trabajadores actuales: fijados por el usuario o automaticos. */
static int obtener_trabajadores(void) {
    return (num_trabajadores > 0) ? num_trabajadores
                                   : (int)nucleos_logicos;
}

/* Pausa para poder abrir htop o top en otra terminal. */
static void esperar_enter(void) {
    char buffer[8];

    printf("Abre htop/top en otra terminal y pulsa ENTER...");
    fflush(stdout);

    if (fgets(buffer, sizeof(buffer), stdin) == NULL)
        clearerr(stdin);
}

/* ----------------------------------------------------------------
   Ejecucion con HILOS
   ---------------------------------------------------------------- */

typedef struct {
    unsigned long long iteraciones; /* parte del trabajo que le toca */
    unsigned long long resultado;   /* lo que calculo */
} DatoHilo;

static void *rutina_hilo(void *arg) {
    DatoHilo *dato = (DatoHilo *)arg;

    dato->resultado = trabajo_cpu(dato->iteraciones);
    return NULL;
}

static unsigned long long ejecutar_hilos(int trabajadores) {
    pthread_t *hilos = malloc(trabajadores * sizeof(pthread_t));
    DatoHilo *datos = malloc(trabajadores * sizeof(DatoHilo));
    unsigned long long suma = 0;

    if (hilos == NULL || datos == NULL) {
        printf("Error reservando memoria.\n");
        exit(EXIT_FAILURE);
    }

    /* Se reparte el trabajo total entre todos los hilos. */
    for (int i = 0; i < trabajadores; i++) {
        datos[i].iteraciones = parte_de(iteraciones, trabajadores, i);
        datos[i].resultado = 0;

        if (pthread_create(&hilos[i], NULL, rutina_hilo, &datos[i]) != 0) {
            printf("Error creando el hilo %d.\n", i);
            exit(EXIT_FAILURE);
        }
    }

    /* El hilo principal espera a que todos terminen. */
    for (int i = 0; i < trabajadores; i++)
        pthread_join(hilos[i], NULL);

    for (int i = 0; i < trabajadores; i++)
        suma += datos[i].resultado;

    free(hilos);
    free(datos);
    return suma;
}

/* ----------------------------------------------------------------
   Ejecucion con PROCESOS
   ---------------------------------------------------------------- */

static unsigned long long ejecutar_procesos(int trabajadores) {
    /* Los resultados van en memoria compartida (mmap): cada hijo es
       una copia del proceso y lo que escriba en sus variables
       normales el padre no lo ve. Los hilos no necesitan esto
       porque comparten la memoria del proceso. */
    unsigned long long *resultados = mmap(NULL,
        trabajadores * sizeof(unsigned long long),
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);

    pid_t *procesos = malloc(trabajadores * sizeof(pid_t));
    unsigned long long suma = 0;

    if (resultados == MAP_FAILED || procesos == NULL) {
        printf("Error reservando memoria.\n");
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < trabajadores; i++) {
        pid_t pid = fork();

        if (pid < 0) {
            perror("fork");
            exit(EXIT_FAILURE);
        }

        if (pid == 0) {
            /* Codigo del HIJO: hace su parte del trabajo y termina.
               fork() le da una copia del proceso, asi que ya ve
               "iteraciones" sin hacer nada mas. */
            resultados[i] = trabajo_cpu(
                parte_de(iteraciones, trabajadores, i));
            _exit(0);
        }

        procesos[i] = pid;
    }

    /* Codigo del PADRE: espera a todos los hijos. */
    for (int i = 0; i < trabajadores; i++)
        waitpid(procesos[i], NULL, 0);

    for (int i = 0; i < trabajadores; i++)
        suma += resultados[i];

    munmap(resultados, trabajadores * sizeof(unsigned long long));
    free(procesos);
    return suma;
}

/* ----------------------------------------------------------------
   Costo de CREAR hilos o procesos (sin trabajo)
   ---------------------------------------------------------------- */

/* Hilo que no hace nada: solo sirve para medir cuanto cuesta
   crearlo y unirlo. */
static void *rutina_nada(void *arg) {
    (void)arg;
    return NULL;
}

static double crear_hilos(int n) {
    pthread_t *hilos = malloc(n * sizeof(pthread_t));
    double t_inicio = ahora();

    for (int i = 0; i < n; i++) {
        if (pthread_create(&hilos[i], NULL, rutina_nada, NULL) != 0) {
            printf("Error creando el hilo %d.\n", i);
            exit(EXIT_FAILURE);
        }
    }

    for (int i = 0; i < n; i++)
        pthread_join(hilos[i], NULL);

    double t_fin = ahora();

    free(hilos);
    return t_fin - t_inicio;
}

static double crear_procesos(int n) {
    pid_t *procesos = malloc(n * sizeof(pid_t));
    double t_inicio = ahora();

    for (int i = 0; i < n; i++) {
        pid_t pid = fork();

        if (pid < 0) {
            perror("fork");
            exit(EXIT_FAILURE);
        }

        if (pid == 0)
            _exit(0); /* Hijo que no hace nada. */

        procesos[i] = pid;
    }

    for (int i = 0; i < n; i++)
        waitpid(procesos[i], NULL, 0);

    double t_fin = ahora();

    free(procesos);
    return t_fin - t_inicio;
}

/* ----------------------------------------------------------------
   Mediciones
   ---------------------------------------------------------------- */

typedef enum {
    SECUENCIAL,
    HILOS,
    PROCESOS
} Modo;

typedef struct {
    int trabajadores;
    double tiempo;       /* tiempo de pared en segundos */
    double cpu;          /* %CPU = tiempo de CPU / tiempo de pared */
    unsigned long long resultado;
} Resultado;

/* Con un_nucleo = 1 se restringe el proceso a un solo procesador
   logico (los hilos nuevos lo heredan). Sirve para demostrar
   concurrencia sin paralelismo. */
static void fijar_afinidad(int un_nucleo) {
    cpu_set_t conjunto;

    CPU_ZERO(&conjunto);

    if (un_nucleo) {
        /* El primer procesador logico que el proceso tuviera
           permitido desde el principio. */
        for (int i = 0; i < CPU_SETSIZE; i++) {
            if (CPU_ISSET(i, &afinidad_original)) {
                CPU_SET(i, &conjunto);
                break;
            }
        }
    } else {
        conjunto = afinidad_original;
    }

    if (sched_setaffinity(0, sizeof(conjunto), &conjunto) != 0)
        perror("sched_setaffinity");
}

/* Ejecuta una configuracion y devuelve tiempos y %CPU. */
static Resultado correr(Modo modo, int trabajadores, int un_nucleo,
                        const char *nombre) {
    Resultado r;
    int con_hijos = (modo == PROCESOS) ? 1 : 0;
    unsigned long long resultado = 0;

    fijar_afinidad(un_nucleo);

    printf("\n>>> %s (%d trabajadores, %llu iteraciones)\n",
           nombre, trabajadores, iteraciones);

    if (pausar)
        esperar_enter();

    double cpu_antes = tiempo_cpu(con_hijos);
    double t_inicio = ahora();

    if (modo == SECUENCIAL) {
        resultado = trabajo_cpu(iteraciones);
    } else if (modo == HILOS) {
        resultado = ejecutar_hilos(trabajadores);
    } else {
        resultado = ejecutar_procesos(trabajadores);
    }

    double t_fin = ahora();
    double cpu_despues = tiempo_cpu(con_hijos);

    /* Volvemos a la afinidad normal. */
    fijar_afinidad(0);

    r.trabajadores = trabajadores;
    r.tiempo = t_fin - t_inicio;
    r.resultado = resultado;

    /* %CPU promedio: 4 hilos a tope deberian rondar el 400%.
       Los hilos cuentan dentro del propio proceso (RUSAGE_SELF)
       y los hijos aparte (RUSAGE_CHILDREN). */
    r.cpu = (r.tiempo > 0.0)
        ? 100.0 * (cpu_despues - cpu_antes) / r.tiempo
        : 0.0;

    printf("Tiempo: %.3f s | CPU: %.0f%% | Resultado: %llu\n",
           r.tiempo, r.cpu, resultado);

    return r;
}

/* ----------------------------------------------------------------
   EXPERIMENTO 6: N hilos vs N procesos
   ---------------------------------------------------------------- */

static void hilos_vs_procesos(void) {
    int n = obtener_trabajadores();

    printf("\n== EXPERIMENTO 6: %d hilos vs %d procesos ==\n", n, n);

    /* Primero medimos solo el costo de crear y esperar, sin carga:
       es la diferencia de fabrica entre un hilo y un proceso. */
    double t_hilos = 0.0;
    double t_procesos = 0.0;

    for (int i = 0; i < 5; i++) {
        t_hilos += crear_hilos(n);
        t_procesos += crear_procesos(n);
    }

    printf("\nSolo crear y esperar, sin carga (promedio de 5):\n");
    printf("  Crear %d hilos:    %8.2f ms\n", n,
           1000.0 * t_hilos / 5.0);
    printf("  Crear %d procesos: %8.2f ms\n", n,
           1000.0 * t_procesos / 5.0);

    /* Ahora con la carga completa: mismo trabajo repartido. */
    Resultado con_hilos = correr(HILOS, n, 0, "Hilos");
    Resultado con_procesos = correr(PROCESOS, n, 0, "Procesos");

    printf("\n%-12s %10s %8s\n", "Forma", "Tiempo", "CPU");
    printf("-------------------------------\n");
    printf("%-12s %9.3fs %7.0f%%\n", "Hilos", con_hilos.tiempo,
           con_hilos.cpu);
    printf("%-12s %9.3fs %7.0f%%\n", "Procesos", con_procesos.tiempo,
           con_procesos.cpu);
}

/* ----------------------------------------------------------------
   EXPERIMENTOS 4 y 7: barrido de hilos con repeticiones
   ---------------------------------------------------------------- */

static void barrido(void) {
    int configs[CONFIGS_BARRIDO];
    int n_configs = 0;

    /* Configuraciones de la guia: 1, 2, 4, 8 y 16 hilos, limitadas
       a los nucleos logicos que existan (si hay menos, se usan las
       disponibles y se explica la limitacion). */
    for (int v = 1; v <= 16 && n_configs < CONFIGS_BARRIDO; v *= 2) {
        if (v <= nucleos_logicos)
            configs[n_configs++] = v;
    }

    /* Si el equipo no tiene una potencia de dos exacta (por ejemplo
       6 nucleos), lo agregamos igual para no dejarlo fuera. */
    if (n_configs == 0 || configs[n_configs - 1] != nucleos_logicos)
        configs[n_configs++] = (int)nucleos_logicos;

    double tiempos[CONFIGS_BARRIDO][MAX_REPETICIONES];
    double promedios[CONFIGS_BARRIDO];
    double cpus[CONFIGS_BARRIDO];

    printf("\n== EXPERIMENTOS 4 y 7: barrido de hilos ==\n");
    printf("Carga: %llu iteraciones repartidas | %d repeticiones\n",
           iteraciones, repeticiones);

    for (int c = 0; c < n_configs; c++) {
        int n = configs[c];
        double suma_tiempo = 0.0;
        double suma_cpu = 0.0;

        for (int rep = 0; rep < repeticiones; rep++) {
            char nombre[40];

            snprintf(nombre, sizeof(nombre), "Hilos %d (repeticion %d)",
                     n, rep + 1);

            Resultado r = correr(HILOS, n, 0, nombre);

            tiempos[c][rep] = r.tiempo;
            suma_tiempo += r.tiempo;
            suma_cpu += r.cpu;
        }

        promedios[c] = suma_tiempo / repeticiones;
        cpus[c] = suma_cpu / repeticiones;
    }

    /* Tabla final, lista para copiar al informe. */
    printf("\n-- TABLA PARA EL INFORME --\n");

    printf("%-8s", "Hilos");

    for (int rep = 0; rep < repeticiones; rep++) {
        char columna[8];

        snprintf(columna, sizeof(columna), "R%d", rep + 1);
        printf(" %9s", columna);
    }

    printf(" %10s %8s %10s\n", "Promedio", "CPU", "Speedup");

    for (int i = 0; i < 64; i++)
        printf("-");

    printf("\n");

    for (int c = 0; c < n_configs; c++) {
        printf("%-8d", configs[c]);

        for (int rep = 0; rep < repeticiones; rep++)
            printf(" %9.3f", tiempos[c][rep]);

        /* Speedup: cuanto mas rapido es esta fila frente a 1 hilo. */
        printf(" %10.3f %7.0f%% %9.2fx\n", promedios[c], cpus[c],
               promedios[0] / promedios[c]);
    }

    printf("\nSpeedup = tiempo de 1 hilo / tiempo de la fila.\n");
    printf("Observa si el tiempo baja mientras hay nucleos libres y\n");
    printf("si el %%CPU sube hasta el tope de procesadores logicos.\n");
}

/* ----------------------------------------------------------------
   Menu
   ---------------------------------------------------------------- */

static void mostrar_menu(void) {
    printf("\n--- MENU ---\n");
    printf("1) Secuencial\n");
    printf("2) Hilos\n");
    printf("3) Procesos\n");
    printf("4) Hilos en 1 nucleo (concurrencia)\n");
    printf("5) Hilos vs Procesos (experimento 6)\n");
    printf("6) Barrido de hilos con repeticiones (experimentos 4 y 7)\n");
    printf("7) Cambiar trabajadores\n");
    printf("8) Cambiar iteraciones y repeticiones\n");
    printf("9) %s pausa antes de cada prueba (para htop)\n",
           pausar ? "Desactivar" : "Activar");
    printf("0) Salir\n");

    if (num_trabajadores > 0)
        printf("Trabajadores: %d (fijado)\n", num_trabajadores);
    else
        printf("Trabajadores: %ld (automatico)\n", nucleos_logicos);

    printf("Opcion: ");
    fflush(stdout);
}

static void cambiar_trabajadores(void) {
    char entrada[32];

    printf("\nNucleos logicos del sistema: %ld\n", nucleos_logicos);
    printf("Trabajadores actuales: %d\n", obtener_trabajadores());
    printf("Se permite pasar de %ld para ver que pasa con mas hilos\n",
           nucleos_logicos);
    printf("que procesadores logicos (pregunta 8 del analisis).\n");
    printf("Nuevos trabajadores (1-%d, 0 = automatico): ",
           MAX_TRABAJADORES);

    if (fgets(entrada, sizeof(entrada), stdin) == NULL)
        return;

    int valor = atoi(entrada);

    if (valor == 0) {
        num_trabajadores = 0;
        printf("Automatico: %ld trabajadores\n", nucleos_logicos);
        return;
    }

    if (valor < 1 || valor > MAX_TRABAJADORES) {
        printf("Valor invalido.\n");
        return;
    }

    num_trabajadores = valor;
    printf("Trabajadores fijados en %d\n", valor);
}

static void cambiar_parametros(void) {
    char entrada[32];

    printf("\nIteraciones totales de la carga (actual %llu): ",
           iteraciones);

    if (fgets(entrada, sizeof(entrada), stdin) == NULL)
        return;

    unsigned long long valor = strtoull(entrada, NULL, 10);

    if (valor > 0) {
        iteraciones = valor;
        printf("Iteraciones fijadas en %llu\n", iteraciones);
    } else {
        printf("Valor invalido, se quedan en %llu\n", iteraciones);
    }

    printf("Repeticiones por configuracion (1-%d, actual %d): ",
           MAX_REPETICIONES, repeticiones);

    if (fgets(entrada, sizeof(entrada), stdin) == NULL)
        return;

    int reps = atoi(entrada);

    if (reps >= 1 && reps <= MAX_REPETICIONES) {
        repeticiones = reps;
        printf("Repeticiones fijadas en %d\n", repeticiones);
    } else {
        printf("Valor invalido, se quedan en %d\n", repeticiones);
    }
}

int main(int argc, char *argv[]) {
    /* Iteraciones por linea de comandos: ./benchmark 50000000 */
    if (argc > 1) {
        unsigned long long valor = strtoull(argv[1], NULL, 10);

        if (valor > 0)
            iteraciones = valor;
    }

    nucleos_logicos = sysconf(_SC_NPROCESSORS_ONLN);

    if (sched_getaffinity(0, sizeof(afinidad_original), &afinidad_original)
        != 0) {
        perror("sched_getaffinity");
        return EXIT_FAILURE;
    }

    printf("== Laboratorio: procesos e hilos en Linux ==\n");
    printf("Procesadores logicos: %ld | carga: %llu iteraciones\n",
           nucleos_logicos, iteraciones);
    printf("Ajusta las iteraciones (argumento u opcion 8) para que la\n");
    printf("version secuencial tarde unos pocos segundos.\n");

    char entrada[32];

    while (1) {
        mostrar_menu();

        if (fgets(entrada, sizeof(entrada), stdin) == NULL)
            break;

        int opcion = atoi(entrada);

        switch (opcion) {
            case 0:
                return 0;

            case 1:
                correr(SECUENCIAL, 1, 0, "Secuencial");
                break;

            case 2:
                correr(HILOS, obtener_trabajadores(), 0, "Hilos");
                break;

            case 3:
                correr(PROCESOS, obtener_trabajadores(), 0, "Procesos");
                break;

            case 4: {
                Resultado r = correr(HILOS, obtener_trabajadores(), 1,
                                     "Hilos en 1 nucleo");
                printf("\nCon %d hilos en un solo procesador logico: si el\n",
                       r.trabajadores);
                printf("tiempo se parece al secuencial y la CPU se queda\n");
                printf("cerca del 100%%, hay concurrencia (se turnan) pero\n");
                printf("no paralelismo real.\n");
                break;
            }

            case 5:
                hilos_vs_procesos();
                break;

            case 6:
                barrido();
                break;

            case 7:
                cambiar_trabajadores();
                break;

            case 8:
                cambiar_parametros();
                break;

            case 9:
                pausar = !pausar;
                printf("Pausa %sactivada.\n", pausar ? "" : "des");
                break;

            default:
                printf("Opcion invalida.\n");
                break;
        }
    }

    return 0;
}
