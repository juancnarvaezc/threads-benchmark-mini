/*
===========================================================
 linux_threads.c

 Comparacion de paralelizacion en Linux (secuencial / hilos /
 procesos) sobre una multiplicacion de matrices grandes.
 Equivalente a windows_threads.c.

===========================================================
*/

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sched.h>
#include <time.h>
#include <stdatomic.h>

#define N 2000

#define SEMILLA_A 12345
#define SEMILLA_B 67890

#define UMBRAL_ACTIVO 70.0

#define PAUSAR 1

static int *A;
static int *B;

/* C va con mmap porque todos los hijos de fork() deben escribir en la misma
   matriz. int64_t porque el resultado no cabe en un int de 32 bits. */
static int64_t *C;

/* num_trabajadores = 0 -> automatico: tantos como nucleos logicos.
   El usuario puede fijarlo con la opcion 7 del menu. */
static int nucleos_logicos = 1;
static int num_trabajadores = 0;

static int obtener_trabajadores(void) {
    if (num_trabajadores > 0)
        return num_trabajadores;

    return nucleos_logicos;
}

static unsigned int generar_numero(unsigned int *estado) {
    *estado = (*estado * 1103515245u + 12345u);
    return *estado;
}

void generar_matriz(int *M, int n, unsigned int semilla) {
    unsigned int estado = semilla;

    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++)
            M[i * n + j] = (generar_numero(&estado) % 8000) + 1;
    }
}

void multiplicar_filas(int fila_inicio, int fila_fin) {
    for (int i = fila_inicio; i < fila_fin; i++) {
        for (int j = 0; j < N; j++) {
            int64_t suma = 0;

            for (int k = 0; k < N; k++)
                suma += (int64_t)A[i * N + k] * B[k * N + j];

            C[i * N + j] = suma;
        }
    }
}

typedef struct {
    int inicio;
    int fin;
} Rango;

void repartir_filas(int n, int trabajadores, Rango *rangos) {
    int paso = n / trabajadores;

    for (int i = 0; i < trabajadores; i++) {
        rangos[i].inicio = i * paso;
        rangos[i].fin = (i == trabajadores - 1) ? n : (i + 1) * paso;
    }
}

uint64_t checksum(void) {
    uint64_t suma = 0;

    for (int i = 0; i < N * N; i++)
        suma += (uint64_t)C[i];

    return suma;
}

typedef struct {
    unsigned long long usuario, nice, sistema, idle;
    unsigned long long iowait, irq, softirq, steal;
} CPUInfo;

typedef struct {
    atomic_bool parar;
    pthread_t hilo;
    int max_activos;
    int nucleos;
} Monitor;

int leer_cpu(CPUInfo *datos, int nucleos) {
    FILE *archivo = fopen("/proc/stat", "r");

    if (archivo == NULL)
        return -1;

    char linea[512];

    /* La primera linea "cpu" es el promedio total, se ignora. */
    fgets(linea, sizeof(linea), archivo);

    for (int i = 0; i < nucleos; i++) {
        if (fgets(linea, sizeof(linea), archivo) == NULL)
            break;

        char nombre[32];

        sscanf(linea, "%31s %llu %llu %llu %llu %llu %llu %llu %llu", nombre,
               &datos[i].usuario, &datos[i].nice, &datos[i].sistema,
               &datos[i].idle, &datos[i].iowait, &datos[i].irq,
               &datos[i].softirq, &datos[i].steal);
    }

    fclose(archivo);

    return 0;
}

double calcular_uso(CPUInfo anterior, CPUInfo actual) {
    /* idle e iowait cuentan como reposo, el mismo criterio que se usa en
       la version de Windows para poder comparar los dos sistemas. */
    unsigned long long idle_anterior = anterior.idle + anterior.iowait;
    unsigned long long idle_actual = actual.idle + actual.iowait;

    unsigned long long activo_anterior = anterior.usuario + anterior.nice
        + anterior.sistema + anterior.irq + anterior.softirq
        + anterior.steal;
    unsigned long long activo_actual = actual.usuario + actual.nice
        + actual.sistema + actual.irq + actual.softirq
        + actual.steal;

    unsigned long long total_anterior = idle_anterior + activo_anterior;
    unsigned long long total_actual = idle_actual + activo_actual;

    unsigned long long diferencia_total = total_actual - total_anterior;
    unsigned long long diferencia_idle = idle_actual - idle_anterior;

    if (diferencia_total == 0)
        return 0.0;

    return 100.0 * (double)(diferencia_total - diferencia_idle)
        / (double)diferencia_total;
}

void *correr_monitor(void *arg) {
    Monitor *monitor = (Monitor *)arg;
    int n = monitor->nucleos;

    CPUInfo *anterior = malloc(n * sizeof(CPUInfo));
    CPUInfo *actual = malloc(n * sizeof(CPUInfo));

    if (anterior == NULL || actual == NULL)
        return NULL;

    leer_cpu(anterior, n);

    int segundo = 0;

    while (!atomic_load(&monitor->parar)) {
        sleep(1);
        leer_cpu(actual, n);
        segundo++;

        int activos = 0;

        printf("t=%2ds | ", segundo);

        for (int i = 0; i < n; i++) {
            double uso = calcular_uso(anterior[i], actual[i]);

            printf("%3.0f ", uso);

            if (uso >= UMBRAL_ACTIVO)
                activos++;
        }

        printf("| activos: %d\n", activos);
        fflush(stdout);

        if (activos > monitor->max_activos)
            monitor->max_activos = activos;

        memcpy(anterior, actual, n * sizeof(CPUInfo));
    }

    free(anterior);
    free(actual);

    return NULL;
}

void monitor_iniciar(Monitor *monitor) {
    monitor->max_activos = 0;
    monitor->nucleos = nucleos_logicos;

    atomic_init(&monitor->parar, 0);

    pthread_create(&monitor->hilo, NULL, correr_monitor, monitor);
}

void monitor_detener(Monitor *monitor) {
    atomic_store(&monitor->parar, 1);
    pthread_join(monitor->hilo, NULL);
}

void ejecutar_secuencial(void) {
    multiplicar_filas(0, N);
}

typedef struct {
    int inicio;
    int fin;
} DatosHilo;

void *tarea_hilo(void *arg) {
    DatosHilo *datos = (DatosHilo *)arg;

    multiplicar_filas(datos->inicio, datos->fin);
    return NULL;
}

void ejecutar_hilos(int trabajadores) {
    pthread_t *hilos = malloc(trabajadores * sizeof(pthread_t));
    DatosHilo *datos = malloc(trabajadores * sizeof(DatosHilo));
    Rango *rangos = malloc(trabajadores * sizeof(Rango));

    repartir_filas(N, trabajadores, rangos);

    for (int i = 0; i < trabajadores; i++) {
        datos[i].inicio = rangos[i].inicio;
        datos[i].fin = rangos[i].fin;

        pthread_create(&hilos[i], NULL, tarea_hilo, &datos[i]);
    }

    for (int i = 0; i < trabajadores; i++)
        pthread_join(hilos[i], NULL);

    free(hilos);
    free(datos);
    free(rangos);
}

void ejecutar_procesos(int trabajadores) {
    pid_t *procesos = malloc(trabajadores * sizeof(pid_t));
    Rango *rangos = malloc(trabajadores * sizeof(Rango));

    repartir_filas(N, trabajadores, rangos);

    /* fork() copia la memoria del padre, asi que los hijos ya ven A y B y
       solo necesitan C, que es la unica matriz compartida. */

    for (int i = 0; i < trabajadores; i++) {
        pid_t pid = fork();

        if (pid == 0) {
            multiplicar_filas(rangos[i].inicio, rangos[i].fin);
            _exit(0);
        }

        if (pid < 0) {
            perror("fork");
            exit(EXIT_FAILURE);
        }

        procesos[i] = pid;
    }

    for (int i = 0; i < trabajadores; i++)
        waitpid(procesos[i], NULL, 0);

    free(procesos);
    free(rangos);
}

void fijar_afinidad(int un_nucleo, cpu_set_t *originales) {
    cpu_set_t conjunto;
    CPU_ZERO(&conjunto);

    if (un_nucleo) {
        /* El primer procesador permitido originalmente. */
        for (int i = 0; i < CPU_SETSIZE; i++) {
            if (CPU_ISSET(i, originales)) {
                CPU_SET(i, &conjunto);
                break;
            }
        }
    } else {
        conjunto = *originales;
    }

    if (sched_setaffinity(0, sizeof(cpu_set_t), &conjunto) != 0)
        perror("sched_setaffinity");
}

void esperar_enter(void) {
#if PAUSAR

    printf("Abre htop y pulsa ENTER...");
    fflush(stdout);

    char buffer[8];

    if (fgets(buffer, sizeof(buffer), stdin) == NULL)
        clearerr(stdin);

#endif
}

typedef struct {
    char nombre[64];
    int trabajadores;
    double tiempo;
    int activos;
    uint64_t checksum;
} Resultado;

typedef enum {
    SECUENCIAL,
    HILOS,
    PROCESOS
} Modo;

Resultado medir(Modo modo, int un_nucleo, const char *nombre,
                cpu_set_t *originales) {
    Resultado resultado;

    int trabajadores = (modo == SECUENCIAL) ? 1 : obtener_trabajadores();

    fijar_afinidad(un_nucleo, originales);

    printf("\n>>> %s (%d trabajadores)\n", nombre, trabajadores);

    esperar_enter();

    /* Limpiamos C antes de cada experimento. */
    memset(C, 0, N * N * sizeof(int64_t));

    Monitor monitor;
    monitor_iniciar(&monitor);

    struct timespec inicio;
    struct timespec fin;

    clock_gettime(CLOCK_MONOTONIC, &inicio);

    switch (modo) {
        case SECUENCIAL:
            ejecutar_secuencial();
            break;

        case HILOS:
            ejecutar_hilos(trabajadores);
            break;

        case PROCESOS:
            ejecutar_procesos(trabajadores);
            break;
    }

    clock_gettime(CLOCK_MONOTONIC, &fin);

    double tiempo = (fin.tv_sec - inicio.tv_sec)
        + (fin.tv_nsec - inicio.tv_nsec) / 1000000000.0;

    monitor_detener(&monitor);

    uint64_t suma = checksum();

    printf("Tiempo: %.3f s | Checksum: %llu\n", tiempo,
           (unsigned long long)suma);

    strcpy(resultado.nombre, nombre);
    resultado.trabajadores = trabajadores;
    resultado.tiempo = tiempo;
    resultado.activos = monitor.max_activos;
    resultado.checksum = suma;

    return resultado;
}

void mostrar_resumen(Resultado resultados[], int cantidad) {
    double base = resultados[0].tiempo;

    printf("\n%-24s %5s %10s %9s %9s\n", "Experimento", "Trab", "Tiempo",
           "Speedup", "Activos");

    printf("--------------------------------------------------------"
           "-------------\n");

    for (int i = 0; i < cantidad; i++) {
        printf("%-24s %5d %8.2fs %8.2fx %9d\n", resultados[i].nombre,
               resultados[i].trabajadores, resultados[i].tiempo,
               base / resultados[i].tiempo, resultados[i].activos);
    }

    int iguales = 1;

    for (int i = 1; i < cantidad; i++) {
        if (resultados[i].checksum != resultados[0].checksum) {
            iguales = 0;
            break;
        }
    }

    if (iguales)
        printf("\nChecksum: igual en todos\n");
    else
        printf("\nChecksum: DIFERENTE, revisar\n");
}

void mostrar_menu(void) {
    printf("\n--- MENU ---\n");

    printf("1) Secuencial\n");
    printf("2) Hilos\n");
    printf("3) Procesos\n");
    printf("4) Hilos en 1 nucleo\n");
    printf("5) Procesos en 1 nucleo\n");
    printf("6) Todos y resumen\n");
    printf("7) Cambiar numero de trabajadores\n");
    printf("0) Salir\n");

    /* Por defecto se usan todos los nucleos logicos, pero el usuario
       puede fijarlos con la opcion 7. */
    if (num_trabajadores > 0)
        printf("Trabajadores: %d (fijado)\n", num_trabajadores);
    else
        printf("Trabajadores: %d (automatico)\n", nucleos_logicos);

    printf("Opcion: ");
    fflush(stdout);
}

void cambiar_trabajadores(void) {
    char entrada[32];

    printf("\nNucleos logicos del sistema: %d\n", nucleos_logicos);
    printf("Trabajadores actuales: %d\n", obtener_trabajadores());
    printf("Nuevos trabajadores (1-%d, 0 = automatico): ", nucleos_logicos);
    fflush(stdout);

    if (fgets(entrada, sizeof(entrada), stdin) == NULL) {
        clearerr(stdin);
        return;
    }

    int valor = atoi(entrada);

    if (valor == 0) {
        num_trabajadores = 0;
        printf("Automatico: %d trabajadores\n", nucleos_logicos);
        return;
    }

    if (valor < 1 || valor > nucleos_logicos) {
        printf("Valor invalido.\n");
        return;
    }

    num_trabajadores = valor;
    printf("Trabajadores fijados en %d\n", valor);
}

void ejecutar_todos(cpu_set_t *originales) {
    Resultado resultados[5];

    resultados[0] = medir(SECUENCIAL, 0, "Secuencial", originales);
    resultados[1] = medir(HILOS, 0, "Hilos", originales);
    resultados[2] = medir(PROCESOS, 0, "Procesos", originales);
    resultados[3] = medir(HILOS, 1, "Hilos en 1 nucleo", originales);
    resultados[4] = medir(PROCESOS, 1, "Procesos en 1 nucleo", originales);

    mostrar_resumen(resultados, 5);
}

int main(void) {
    nucleos_logicos = (int)sysconf(_SC_NPROCESSORS_ONLN);

    cpu_set_t afinidad_original;

    if (sched_getaffinity(0, sizeof(cpu_set_t), &afinidad_original) != 0) {
        perror("sched_getaffinity");
        return EXIT_FAILURE;
    }

    A = malloc(N * N * sizeof(int));
    B = malloc(N * N * sizeof(int));

    C = mmap(NULL, N * N * sizeof(int64_t), PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANONYMOUS, -1, 0);

    if (A == NULL || B == NULL || C == MAP_FAILED) {
        printf("Error reservando memoria.\n");
        return EXIT_FAILURE;
    }

    /* Las matrices se generan UNA SOLA VEZ, para que todos los experimentos
       multipliquen exactamente los mismos datos. */
    generar_matriz(A, N, SEMILLA_A);
    generar_matriz(B, N, SEMILLA_B);

    printf("%d nucleos logicos | N=%d\n", nucleos_logicos, N);

    char entrada[32];

    while (1) {
        mostrar_menu();

        if (fgets(entrada, sizeof(entrada), stdin) == NULL)
            break;

        int opcion = atoi(entrada);

        switch (opcion) {
            case 0:
                fijar_afinidad(0, &afinidad_original);
                munmap(C, N * N * sizeof(int64_t));
                free(A);
                free(B);
                return 0;

            case 1:
                medir(SECUENCIAL, 0, "Secuencial", &afinidad_original);
                break;

            case 2:
                medir(HILOS, 0, "Hilos", &afinidad_original);
                break;

            case 3:
                medir(PROCESOS, 0, "Procesos", &afinidad_original);
                break;

            case 4:
                medir(HILOS, 1, "Hilos en 1 nucleo", &afinidad_original);
                break;

            case 5:
                medir(PROCESOS, 1, "Procesos en 1 nucleo",
                      &afinidad_original);
                break;

            case 6:
                ejecutar_todos(&afinidad_original);
                break;

            case 7:
                cambiar_trabajadores();
                break;

            default:
                printf("Opcion invalida.\n");
                break;
        }
    }

    fijar_afinidad(0, &afinidad_original);
    munmap(C, N * N * sizeof(int64_t));
    free(A);
    free(B);

    return 0;
}