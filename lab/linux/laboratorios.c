/*
===========================================================
 laboratorios.c

 Laboratorio de procesos e hilos en Linux. La carga es una
 multiplicacion de matrices (secuencial, hilos y procesos) y
 cada opcion del menu principal es uno de los experimentos del
 documento lab/Laboratorio_...md.

 Adaptado de matrix_mult/linux_threads.c.

 Compilar:
    gcc -O2 -Wall -pthread laboratorios.c -o laboratorios
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

#define SEMILLA_A 12345
#define SEMILLA_B 67890

/* Un nucleo cuenta como "activo" si supera este uso. */
#define UMBRAL_ACTIVO 70.0

#define N_DEFECTO 2000
#define MAX_REPETICIONES 10

/* N es configurable en runtime (Lab 7); empieza en 2000. */
static int N = N_DEFECTO;

static int *A;
static int *B;

/* C va con mmap porque todos los hijos de fork() deben escribir en la misma
   matriz. int64_t porque el resultado no cabe en un int de 32 bits. */
static int64_t *C;

/* num_trabajadores = 0 -> automatico: tantos como nucleos logicos. */
static int nucleos_logicos = 1;
static int num_trabajadores = 0;

/* Repeticiones por configuracion en el barrido del Lab 7. */
static int repeticiones = 3;

/* Pausa antes de cada corrida para abrir htop. Apagada por defecto. */
static int pausar = 0;

/* Afinidad con la que arranco el proceso, para volver a ella despues de la
   prueba de un solo nucleo. */
static cpu_set_t afinidad_original;

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

int reservar_matrices(void) {
    A = malloc((size_t)N * N * sizeof(int));
    B = malloc((size_t)N * N * sizeof(int));

    C = mmap(NULL, (size_t)N * N * sizeof(int64_t), PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANONYMOUS, -1, 0);

    if (A == NULL || B == NULL || C == MAP_FAILED)
        return 0;

    return 1;
}

void liberar_matrices(void) {
    if (C != NULL && C != MAP_FAILED)
        munmap(C, (size_t)N * N * sizeof(int64_t));

    free(A);
    free(B);

    A = NULL;
    B = NULL;
    C = NULL;
}

/* Las matrices se generan con semillas fijas: todas las corridas multiplican
   exactamente los mismos datos. */
void regenerar_matrices(void) {
    generar_matriz(A, N, SEMILLA_A);
    generar_matriz(B, N, SEMILLA_B);
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

/* ---------------- Monitor de uso de CPU por nucleo ---------------- */

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
    /* idle e iowait cuentan como reposo, igual que en la version de Windows. */
    unsigned long long idle_anterior = anterior.idle + anterior.iowait;
    unsigned long long idle_actual = actual.idle + actual.iowait;

    unsigned long long activo_anterior = anterior.usuario + anterior.nice
        + anterior.sistema + anterior.irq + anterior.softirq + anterior.steal;
    unsigned long long activo_actual = actual.usuario + actual.nice
        + actual.sistema + actual.irq + actual.softirq + actual.steal;

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

/* ---------------- Formas de ejecutar la multiplicacion ---------------- */

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

void fijar_afinidad(int un_nucleo) {
    cpu_set_t conjunto;
    CPU_ZERO(&conjunto);

    if (un_nucleo) {
        /* El primer procesador permitido originalmente. */
        for (int i = 0; i < CPU_SETSIZE; i++) {
            if (CPU_ISSET(i, &afinidad_original)) {
                CPU_SET(i, &conjunto);
                break;
            }
        }
    } else {
        conjunto = afinidad_original;
    }

    if (sched_setaffinity(0, sizeof(cpu_set_t), &conjunto) != 0)
        perror("sched_setaffinity");
}

void esperar_enter(void) {
    if (!pausar)
        return;

    printf("Abre htop y pulsa ENTER...");
    fflush(stdout);

    char buffer[8];

    if (fgets(buffer, sizeof(buffer), stdin) == NULL)
        clearerr(stdin);
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

Resultado medir(Modo modo, int un_nucleo, const char *nombre) {
    Resultado resultado;

    int trabajadores = (modo == SECUENCIAL) ? 1 : obtener_trabajadores();

    fijar_afinidad(un_nucleo);

    printf("\n>>> %s (%d trabajadores)\n", nombre, trabajadores);

    esperar_enter();

    /* Limpiamos C antes de cada experimento. */
    memset(C, 0, (size_t)N * N * sizeof(int64_t));

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

void ejecutar_todos(void) {
    Resultado resultados[5];

    resultados[0] = medir(SECUENCIAL, 0, "Secuencial");
    resultados[1] = medir(HILOS, 0, "Hilos");
    resultados[2] = medir(PROCESOS, 0, "Procesos");
    resultados[3] = medir(HILOS, 1, "Hilos en 1 nucleo");
    resultados[4] = medir(PROCESOS, 1, "Procesos en 1 nucleo");

    mostrar_resumen(resultados, 5);
}

/* ---------------- Opciones de configuracion ---------------- */

void cambiar_trabajadores(void) {
    char entrada[32];

    /* Se permite pasar de los nucleos logicos para ver que ocurre cuando hay
       mas hilos que procesadores logicos (pregunta 8 del analisis). */
    int maximo = nucleos_logicos * 4;

    printf("\nNucleos logicos del sistema: %d\n", nucleos_logicos);
    printf("Trabajadores actuales: %d\n", obtener_trabajadores());
    printf("Nuevos trabajadores (1-%d, 0 = automatico): ", maximo);
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

    if (valor < 1 || valor > maximo) {
        printf("Valor invalido.\n");
        return;
    }

    num_trabajadores = valor;
    printf("Trabajadores fijados en %d\n", valor);
}

void cambiar_n(void) {
    char entrada[32];

    printf("\nN actual: %d\n", N);
    printf("Nuevo N (200-4000): ");
    fflush(stdout);

    if (fgets(entrada, sizeof(entrada), stdin) == NULL) {
        clearerr(stdin);
        return;
    }

    int valor = atoi(entrada);

    if (valor < 200 || valor > 4000) {
        printf("Valor invalido.\n");
        return;
    }

    liberar_matrices();
    N = valor;

    if (!reservar_matrices()) {
        printf("Error reservando memoria.\n");
        exit(EXIT_FAILURE);
    }

    regenerar_matrices();
    printf("N fijado en %d\n", N);
}

/* ---------------- Laboratorios ---------------- */

void lab1_hardware(void) {
    printf("\n== Lab 1: identificacion del hardware ==\n");
    printf("Esta salida es la evidencia de la tabla 7.2.\n\n");
    fflush(stdout);

    /* fflush para que el titulo salga antes que la salida de los comandos. */
    printf("--- lscpu ---\n");
    fflush(stdout);
    system("lscpu");

    printf("\n--- lscpu -e ---\n");
    fflush(stdout);
    system("lscpu -e");

    printf("\n--- nproc ---\n");
    fflush(stdout);
    system("nproc");

    printf("\nPulsa ENTER para volver...");
    fflush(stdout);

    char buffer[8];

    if (fgets(buffer, sizeof(buffer), stdin) == NULL)
        clearerr(stdin);
}

/* Ejemplo de la seccion 9.1: crear hilos y esperarlos. */
void *tarea_simple(void *arg) {
    int id = *(int *)arg;

    printf("Hilo %d ejecutando...\n", id);
    return NULL;
}

void lab3_ejemplo(int cantidad) {
    pthread_t *hilos = malloc(cantidad * sizeof(pthread_t));
    int *ids = malloc(cantidad * sizeof(int));

    if (hilos == NULL || ids == NULL) {
        printf("Error reservando memoria.\n");
        return;
    }

    for (int i = 0; i < cantidad; i++) {
        ids[i] = i;
        pthread_create(&hilos[i], NULL, tarea_simple, &ids[i]);
    }

    for (int i = 0; i < cantidad; i++)
        pthread_join(hilos[i], NULL);

    printf("Todos los hilos terminaron.\n");

    free(hilos);
    free(ids);
}

void lab7_barrido(void) {
    int configs[8];
    int n_configs = 0;

    /* Configuraciones del laboratorio: 1, 2, 4, 8 y 16 hilos, hasta los
       procesadores logicos que existan. */
    for (int v = 1; v <= 16; v *= 2) {
        if (v <= nucleos_logicos)
            configs[n_configs++] = v;
    }

    /* Si el equipo no tiene potencia de dos exacta (por ejemplo 6), se
       agrega igual para no dejarlo fuera. */
    if (configs[n_configs - 1] != nucleos_logicos)
        configs[n_configs++] = nucleos_logicos;

    double tiempos[8][MAX_REPETICIONES];
    double promedios[8];
    double activos[8];

    printf("\n== Lab 7: barrido de hilos, %d repeticiones ==\n", repeticiones);

    int guardado = num_trabajadores;

    for (int c = 0; c < n_configs; c++) {
        double suma = 0.0;
        double suma_activos = 0.0;
        char nombre[32];

        num_trabajadores = configs[c];
        snprintf(nombre, sizeof(nombre), "Hilos %d", configs[c]);

        for (int r = 0; r < repeticiones; r++) {
            Resultado res = medir(HILOS, 0, nombre);

            tiempos[c][r] = res.tiempo;
            suma += res.tiempo;
            suma_activos += res.activos;
        }

        promedios[c] = suma / repeticiones;
        activos[c] = suma_activos / repeticiones;
    }

    num_trabajadores = guardado;

    /* Tabla lista para el informe (seccion 13). */
    printf("\n%-8s", "Hilos");

    for (int r = 0; r < repeticiones; r++) {
        char col[8];

        snprintf(col, sizeof(col), "R%d", r + 1);
        printf(" %7s", col);
    }

    printf(" %9s %8s %9s\n", "Promedio", "Activos", "Speedup");
    printf("---------------------------------------------------------------\n");

    for (int c = 0; c < n_configs; c++) {
        printf("%-8d", configs[c]);

        for (int r = 0; r < repeticiones; r++)
            printf(" %7.2f", tiempos[c][r]);

        printf(" %9.2f %8.1f %8.2fx\n", promedios[c], activos[c],
               promedios[0] / promedios[c]);
    }
}

/* ---------------- Submenus de cada laboratorio ---------------- */

void menu_lab2(void) {
    char entrada[32];

    while (1) {
        printf("\n== Lab 2: proceso frente a hilo ==\n");
        printf("1) Secuencial\n");
        printf("2) Hilos\n");
        printf("3) Procesos\n");
        printf("4) Las tres y comparar\n");
        printf("5) Cambiar trabajadores\n");
        printf("6) %s pausa antes de cada corrida\n",
               pausar ? "Desactivar" : "Activar");
        printf("0) Volver\n");
        printf("Opcion: ");
        fflush(stdout);

        if (fgets(entrada, sizeof(entrada), stdin) == NULL)
            return;

        switch (atoi(entrada)) {
            case 0:
                return;

            case 1:
                medir(SECUENCIAL, 0, "Secuencial");
                break;

            case 2:
                medir(HILOS, 0, "Hilos");
                break;

            case 3:
                medir(PROCESOS, 0, "Procesos");
                break;

            case 4: {
                Resultado resultados[3];

                resultados[0] = medir(SECUENCIAL, 0, "Secuencial");
                resultados[1] = medir(HILOS, 0, "Hilos");
                resultados[2] = medir(PROCESOS, 0, "Procesos");

                mostrar_resumen(resultados, 3);
                break;
            }

            case 5:
                cambiar_trabajadores();
                break;

            case 6:
                pausar = !pausar;
                break;

            default:
                printf("Opcion invalida.\n");
                break;
        }
    }
}

void menu_lab3(void) {
    char entrada[32];
    int cantidad = 4;

    while (1) {
        printf("\n== Lab 3: creacion de hilos ==\n");
        printf("1) Ejecutar el ejemplo (%d hilos)\n", cantidad);
        printf("2) Cambiar numero de hilos\n");
        printf("0) Volver\n");
        printf("Opcion: ");
        fflush(stdout);

        if (fgets(entrada, sizeof(entrada), stdin) == NULL)
            return;

        switch (atoi(entrada)) {
            case 0:
                return;

            case 1:
                lab3_ejemplo(cantidad);
                break;

            case 2:
                printf("Numero de hilos (1-64): ");
                fflush(stdout);

                if (fgets(entrada, sizeof(entrada), stdin) != NULL) {
                    int valor = atoi(entrada);

                    if (valor >= 1 && valor <= 64)
                        cantidad = valor;
                    else
                        printf("Valor invalido.\n");
                }
                break;

            default:
                printf("Opcion invalida.\n");
                break;
        }
    }
}

void menu_lab4(void) {
    char entrada[32];

    while (1) {
        printf("\n== Lab 4: concurrencia y paralelismo ==\n");
        printf("1) Hilos con %d trabajadores\n", obtener_trabajadores());
        printf("2) Hilos en 1 nucleo\n");
        printf("3) Todos los casos y resumen (tabla 10)\n");
        printf("4) Cambiar trabajadores\n");
        printf("5) %s pausa antes de cada corrida\n",
               pausar ? "Desactivar" : "Activar");
        printf("0) Volver\n");
        printf("Opcion: ");
        fflush(stdout);

        if (fgets(entrada, sizeof(entrada), stdin) == NULL)
            return;

        switch (atoi(entrada)) {
            case 0:
                return;

            case 1:
                medir(HILOS, 0, "Hilos");
                break;

            case 2:
                medir(HILOS, 1, "Hilos en 1 nucleo");
                break;

            case 3:
                ejecutar_todos();
                break;

            case 4:
                cambiar_trabajadores();
                break;

            case 5:
                pausar = !pausar;
                break;

            default:
                printf("Opcion invalida.\n");
                break;
        }
    }
}

void menu_lab5(void) {
    char entrada[32];

    while (1) {
        printf("\n== Lab 5: scheduler y uso de CPU ==\n");
        printf("1) Ejecutar con %d hilos y pausa (observar)\n",
               obtener_trabajadores());
        printf("2) Cambiar trabajadores\n");
        printf("0) Volver\n");
        printf("Opcion: ");
        fflush(stdout);

        if (fgets(entrada, sizeof(entrada), stdin) == NULL)
            return;

        switch (atoi(entrada)) {
            case 0:
                return;

            case 1: {
                printf("\nObserva el proceso %d en otra terminal:\n",
                       (int)getpid());
                printf("  htop                  (tecla H para ver hilos)\n");
                printf("  ps -eLf | grep %d\n", (int)getpid());
                printf("  pidstat -t -p %d 1\n", (int)getpid());
                printf("  lscpu -e\n");

                /* Esta corrida siempre pausa, para poder abrir htop. */
                int antes = pausar;
                pausar = 1;

                medir(HILOS, 0, "Hilos (observacion)");

                pausar = antes;
                break;
            }

            case 2:
                cambiar_trabajadores();
                break;

            default:
                printf("Opcion invalida.\n");
                break;
        }
    }
}

void menu_lab6(void) {
    char entrada[32];

    while (1) {
        printf("\n== Lab 6: procesos vs hilos ==\n");
        printf("1) Ejecutar %d hilos y %d procesos y comparar\n",
               obtener_trabajadores(), obtener_trabajadores());
        printf("2) Cambiar trabajadores\n");
        printf("0) Volver\n");
        printf("Opcion: ");
        fflush(stdout);

        if (fgets(entrada, sizeof(entrada), stdin) == NULL)
            return;

        switch (atoi(entrada)) {
            case 0:
                return;

            case 1: {
                Resultado resultados[2];

                resultados[0] = medir(HILOS, 0, "Hilos");
                resultados[1] = medir(PROCESOS, 0, "Procesos");

                mostrar_resumen(resultados, 2);
                break;
            }

            case 2:
                cambiar_trabajadores();
                break;

            default:
                printf("Opcion invalida.\n");
                break;
        }
    }
}

void menu_lab7(void) {
    char entrada[32];

    while (1) {
        printf("\n== Lab 7: medicion del tiempo ==\n");
        printf("1) Barrido 1,2,4,8,16 hilos x %d repeticiones (tabla 13)\n",
               repeticiones);
        printf("2) Cambiar repeticiones (%d)\n", repeticiones);
        printf("3) Cambiar N (actual %d)\n", N);
        printf("4) %s pausa antes de cada corrida\n",
               pausar ? "Desactivar" : "Activar");
        printf("0) Volver\n");
        printf("Opcion: ");
        fflush(stdout);

        if (fgets(entrada, sizeof(entrada), stdin) == NULL)
            return;

        switch (atoi(entrada)) {
            case 0:
                return;

            case 1:
                lab7_barrido();
                break;

            case 2:
                printf("Repeticiones (1-%d): ", MAX_REPETICIONES);
                fflush(stdout);

                if (fgets(entrada, sizeof(entrada), stdin) != NULL) {
                    int valor = atoi(entrada);

                    if (valor >= 1 && valor <= MAX_REPETICIONES)
                        repeticiones = valor;
                    else
                        printf("Valor invalido.\n");
                }
                break;

            case 3:
                cambiar_n();
                break;

            case 4:
                pausar = !pausar;
                break;

            default:
                printf("Opcion invalida.\n");
                break;
        }
    }
}

int main(void) {
    nucleos_logicos = (int)sysconf(_SC_NPROCESSORS_ONLN);

    if (sched_getaffinity(0, sizeof(cpu_set_t), &afinidad_original) != 0) {
        perror("sched_getaffinity");
        return EXIT_FAILURE;
    }

    if (!reservar_matrices()) {
        printf("Error reservando memoria.\n");
        return EXIT_FAILURE;
    }

    regenerar_matrices();

    printf("== LABORATORIOS: Procesos e Hilos (Linux) ==\n");
    printf("%d procesadores logicos | N=%d\n", nucleos_logicos, N);

    char entrada[32];

    while (1) {
        printf("\n--- MENU ---\n");
        printf("1) Lab 1 - Identificacion del hardware\n");
        printf("2) Lab 2 - Proceso frente a hilo\n");
        printf("3) Lab 3 - Creacion de hilos\n");
        printf("4) Lab 4 - Concurrencia y paralelismo\n");
        printf("5) Lab 5 - Scheduler y uso de CPU\n");
        printf("6) Lab 6 - Procesos vs hilos\n");
        printf("7) Lab 7 - Medicion del tiempo (repeticiones)\n");
        printf("0) Salir\n");
        printf("Opcion: ");
        fflush(stdout);

        if (fgets(entrada, sizeof(entrada), stdin) == NULL)
            break;

        switch (atoi(entrada)) {
            case 0:
                fijar_afinidad(0);
                liberar_matrices();
                return 0;

            case 1:
                lab1_hardware();
                break;

            case 2:
                menu_lab2();
                break;

            case 3:
                menu_lab3();
                break;

            case 4:
                menu_lab4();
                break;

            case 5:
                menu_lab5();
                break;

            case 6:
                menu_lab6();
                break;

            case 7:
                menu_lab7();
                break;

            default:
                printf("Opcion invalida.\n");
                break;
        }
    }

    fijar_afinidad(0);
    liberar_matrices();

    return 0;
}
