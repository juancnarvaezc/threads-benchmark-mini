/*
===========================================================
 windows_threads.c

 Comparacion de paralelizacion en Windows.
 Equivalente a linux_threads.c.

 La logica experimental es identica, solo cambian las APIs:

   pthread_create        -> CreateThread
   pthread_join          -> WaitForSingleObject
   fork                  -> CreateProcess (mismo ejecutable)
   mmap(MAP_SHARED)      -> CreateFileMapping + MapViewOfFile
   sched_setaffinity     -> SetProcessAffinityMask
   clock_gettime         -> QueryPerformanceCounter
   /proc/stat            -> ZwQuerySystemInformation (ntdll)

===========================================================
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <windows.h>

#define N 2000

#define SEMILLA_A 12345
#define SEMILLA_B 67890

#define UMBRAL_ACTIVO 70.0

#define PAUSAR 1

/* El PID evita que dos instancias del programa se pisen. */
#define PREFIJO_A "MatrizA_"
#define PREFIJO_B "MatrizB_"
#define PREFIJO_C "MatrizC_"

typedef struct {
    int inicio;
    int fin;
} Rango;

typedef struct {
    int inicio;
    int fin;
} DatosHilo;

/* CreateProcess no hereda memoria como fork(), asi que A, B y C
   tienen que estar en memoria compartida para que las lean los hijos.
   En Linux solo C necesita mmap(MAP_SHARED). */
static int *A;
static int *B;
static int64_t *C;

static HANDLE memoria_compartida_A;
static HANDLE memoria_compartida_B;
static HANDLE memoria_compartida_C;

static char NOMBRE_A[64];
static char NOMBRE_B[64];
static char NOMBRE_C[64];

/* num_trabajadores = 0 -> automatico: tantos como nucleos logicos.
   El usuario puede fijarlo con la opcion 7 del menu. */
static int nucleos_logicos = 1;
static int num_trabajadores = 0;

static int obtener_trabajadores(void) {
    if (num_trabajadores > 0)
        return num_trabajadores;

    return nucleos_logicos;
}

static void construir_nombre(char *destino, const char *prefijo,
                             unsigned long pid) {
    sprintf(destino, "%s%lu", prefijo, pid);
}

static unsigned int generar_numero(unsigned int *estado) {
    *estado = (*estado * 1103515245u) + 12345u;
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

static DWORD WINAPI tarea_hilo(LPVOID parametro) {
    DatosHilo *datos = (DatosHilo *)parametro;

    multiplicar_filas(datos->inicio, datos->fin);
    return 0;
}

static void ejecutar_secuencial(void) {
    multiplicar_filas(0, N);
}

static void ejecutar_hilos(int trabajadores) {
    HANDLE *hilos = malloc((size_t)trabajadores * sizeof(HANDLE));
    DatosHilo *datos = malloc((size_t)trabajadores * sizeof(DatosHilo));
    Rango *rangos = malloc((size_t)trabajadores * sizeof(Rango));

    if (hilos == NULL || datos == NULL || rangos == NULL) {
        printf("Error reservando memoria\n");
        exit(EXIT_FAILURE);
    }

    repartir_filas(N, trabajadores, rangos);

    for (int i = 0; i < trabajadores; i++) {
        datos[i].inicio = rangos[i].inicio;
        datos[i].fin = rangos[i].fin;

        hilos[i] = CreateThread(NULL, 0, tarea_hilo, &datos[i], 0, NULL);

        if (hilos[i] == NULL) {
            printf("Error creando hilo %d\n", i);
            exit(EXIT_FAILURE);
        }
    }

    for (int i = 0; i < trabajadores; i++) {
        WaitForSingleObject(hilos[i], INFINITE);
        CloseHandle(hilos[i]);
    }

    free(hilos);
    free(datos);
    free(rangos);
}

/* El hijo de CreateProcess no recibe A, B y C: reconstruye los nombres
   (llevan el PID del padre) y mapea las tres matrices por su cuenta. */
static int ejecutar_worker_proceso(int inicio, int fin, unsigned long pid) {
    char nombre_A[64], nombre_B[64], nombre_C[64];

    construir_nombre(nombre_A, PREFIJO_A, pid);
    construir_nombre(nombre_B, PREFIJO_B, pid);
    construir_nombre(nombre_C, PREFIJO_C, pid);

    HANDLE mapa_A = OpenFileMappingA(FILE_MAP_READ, FALSE, nombre_A);
    HANDLE mapa_B = OpenFileMappingA(FILE_MAP_READ, FALSE, nombre_B);
    HANDLE mapa_C = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, nombre_C);

    if (mapa_A == NULL || mapa_B == NULL || mapa_C == NULL) {
        printf("worker: no se encontro la memoria compartida del padre.\n");
        return 1;
    }

    A = (int *)MapViewOfFile(mapa_A, FILE_MAP_READ, 0, 0,
                              N * N * sizeof(int));
    B = (int *)MapViewOfFile(mapa_B, FILE_MAP_READ, 0, 0,
                              N * N * sizeof(int));
    C = (int64_t *)MapViewOfFile(mapa_C, FILE_MAP_ALL_ACCESS, 0, 0,
                                  N * N * sizeof(int64_t));

    if (A == NULL || B == NULL || C == NULL) {
        printf("worker: error mapeando A, B o C\n");
        return 1;
    }

    multiplicar_filas(inicio, fin);

    UnmapViewOfFile(A);
    UnmapViewOfFile(B);
    UnmapViewOfFile(C);

    CloseHandle(mapa_A);
    CloseHandle(mapa_B);
    CloseHandle(mapa_C);

    return 0;
}

static void ejecutar_procesos(int trabajadores) {
    HANDLE *procesos = malloc((size_t)trabajadores * sizeof(HANDLE));
    Rango *rangos = malloc((size_t)trabajadores * sizeof(Rango));

    if (procesos == NULL || rangos == NULL) {
        printf("Error reservando memoria\n");
        exit(EXIT_FAILURE);
    }

    repartir_filas(N, trabajadores, rangos);

    /* fork() no existe: hay que volver a lanzar este mismo ejecutable.
       El buffer cubre una ruta de Windows completa, no un char[100]. */
    char ejecutable[MAX_PATH];
    GetModuleFileNameA(NULL, ejecutable, MAX_PATH);

    unsigned long pid_padre = (unsigned long)GetCurrentProcessId();

    for (int i = 0; i < trabajadores; i++) {
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;

        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        ZeroMemory(&pi, sizeof(pi));

        char argumentos[MAX_PATH + 128];

        sprintf(argumentos, "\"%s\" worker %d %d %lu", ejecutable,
                rangos[i].inicio, rangos[i].fin, pid_padre);

        if (CreateProcessA(NULL, argumentos, NULL, NULL, FALSE, 0, NULL, NULL,
                           &si, &pi) == FALSE) {
            printf("Error creando proceso %d (codigo %lu)\n", i,
                   (unsigned long)GetLastError());
            exit(EXIT_FAILURE);
        }

        procesos[i] = pi.hProcess;
        CloseHandle(pi.hThread);
    }

    for (int i = 0; i < trabajadores; i++) {
        WaitForSingleObject(procesos[i], INFINITE);

        /* waitpid() no avisa si el hijo fallo, GetExitCodeProcess si. */
        DWORD codigo = 0;
        GetExitCodeProcess(procesos[i], &codigo);

        if (codigo != 0)
            printf("Aviso: el proceso %d termino con codigo %lu\n", i,
                   (unsigned long)codigo);

        CloseHandle(procesos[i]);
    }

    free(procesos);
    free(rangos);
}

static void reservar_memoria(void) {
    size_t bytes_ab = (size_t)N * N * sizeof(int);
    size_t bytes_c = (size_t)N * N * sizeof(int64_t);

    memoria_compartida_A = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL,
                                              PAGE_READWRITE, 0, bytes_ab,
                                              NOMBRE_A);
    memoria_compartida_B = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL,
                                              PAGE_READWRITE, 0, bytes_ab,
                                              NOMBRE_B);
    memoria_compartida_C = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL,
                                              PAGE_READWRITE, 0, bytes_c,
                                              NOMBRE_C);

    if (memoria_compartida_A == NULL || memoria_compartida_B == NULL
        || memoria_compartida_C == NULL) {
        printf("Error creando memoria compartida (codigo %lu)\n",
               (unsigned long)GetLastError());
        exit(EXIT_FAILURE);
    }

    A = (int *)MapViewOfFile(memoria_compartida_A, FILE_MAP_ALL_ACCESS, 0, 0,
                             bytes_ab);
    B = (int *)MapViewOfFile(memoria_compartida_B, FILE_MAP_ALL_ACCESS, 0, 0,
                             bytes_ab);
    C = (int64_t *)MapViewOfFile(memoria_compartida_C, FILE_MAP_ALL_ACCESS,
                                 0, 0, bytes_c);

    if (A == NULL || B == NULL || C == NULL) {
        printf("Error creando vistas compartidas\n");
        exit(EXIT_FAILURE);
    }
}

/* Equivale a munmap() en Linux. */
static void liberar_memoria(void) {
    if (A != NULL) UnmapViewOfFile(A);
    if (B != NULL) UnmapViewOfFile(B);
    if (C != NULL) UnmapViewOfFile(C);

    if (memoria_compartida_A != NULL) CloseHandle(memoria_compartida_A);
    if (memoria_compartida_B != NULL) CloseHandle(memoria_compartida_B);
    if (memoria_compartida_C != NULL) CloseHandle(memoria_compartida_C);

    A = NULL;
    B = NULL;
    C = NULL;
}

static LARGE_INTEGER frecuencia_reloj;

static void inicializar_reloj(void) {
    QueryPerformanceFrequency(&frecuencia_reloj);
}

static double obtener_tiempo(void) {
    LARGE_INTEGER contador;
    QueryPerformanceCounter(&contador);

    return (double)contador.QuadPart / (double)frecuencia_reloj.QuadPart;
}

/* Equivale a sched_setaffinity(0, ...) de Linux. Como la mascara se aplica
   al proceso completo, los hijos la heredan igual que con fork(). */
static void fijar_afinidad(int un_nucleo, DWORD original) {
    DWORD mascara = original;

    if (un_nucleo) {
        /* El primer procesador permitido originalmente. */
        for (DWORD i = 0; i < 32; i++) {
            if (original & (1UL << i)) {
                mascara = 1UL << i;
                break;
            }
        }
    }

    if (SetProcessAffinityMask(GetCurrentProcess(), mascara) == FALSE)
        printf("Error fijando afinidad (codigo %lu)\n",
               (unsigned long)GetLastError());
}

typedef struct {
    unsigned long long usuario, nice, sistema, idle;
    unsigned long long iowait, irq, softirq, steal;
} CPUInfo;

/* Este MinGW no trae stdatomic.h: volatile LONG + Interlocked. */
typedef struct {
    volatile LONG parar;
    HANDLE hilo;
    int max_activos;
    int nucleos;
} Monitor;

/* SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION: un registro por nucleo logico
   mas uno con el total del sistema. Se usa ntdll porque pdh.h no viene
   en el MinGW y GetSystemTimes() solo da totales del sistema. */
#define CLASE_PROCESSOR_PERFORMANCE 8

typedef struct {
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER DpcTime;
    LARGE_INTEGER InterruptTime;
    LARGE_INTEGER IrqlTime;
} ProcessorPerformance;

typedef LONG (WINAPI *ConsultarSistema)(ULONG clase, PVOID buffer,
                                        ULONG longitud, PULONG retorno);

static ConsultarSistema consultar_sistema = NULL;

/* Buffer de leer_cpu(), solo lo necesita el hilo del monitor. */
static ProcessorPerformance *info_cpu = NULL;
static ULONG bytes_info_cpu = 0;

static int iniciar_lector_cpu(void) {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");

    if (ntdll == NULL)
        return -1;

    consultar_sistema = (ConsultarSistema)(void *)GetProcAddress(
        ntdll, "ZwQuerySystemInformation");

    return consultar_sistema == NULL ? -1 : 0;
}

static int leer_cpu(CPUInfo *datos, int nucleos) {
    if (consultar_sistema == NULL || info_cpu == NULL)
        return -1;

    ULONG necesario = 0;
    consultar_sistema(CLASE_PROCESSOR_PERFORMANCE, NULL, 0, &necesario);

    if (necesario == 0 || necesario > bytes_info_cpu)
        return -1;

    consultar_sistema(CLASE_PROCESSOR_PERFORMANCE, info_cpu, bytes_info_cpu,
                      &necesario);

    int disponibles = (int)(necesario / sizeof(ProcessorPerformance)) - 1;

    for (int i = 0; i < nucleos; i++) {
        if (i >= disponibles) {
            datos[i].usuario = datos[i].nice = datos[i].sistema = 0;
            datos[i].idle = datos[i].iowait = datos[i].irq = 0;
            datos[i].softirq = datos[i].steal = 0;
            continue;
        }

        /* En Windows KernelTime YA incluye el idle, y en /proc/stat la
           columna "system" NO lo incluye, por eso se separa aqui. */
        datos[i].usuario = (unsigned long long)info_cpu[i].UserTime.QuadPart;
        datos[i].sistema = (unsigned long long)
            (info_cpu[i].KernelTime.QuadPart - info_cpu[i].IdleTime.QuadPart);
        datos[i].idle = (unsigned long long)info_cpu[i].IdleTime.QuadPart;
        datos[i].irq = (unsigned long long)info_cpu[i].InterruptTime.QuadPart;
        datos[i].softirq = (unsigned long long)info_cpu[i].DpcTime.QuadPart;

        /* Windows no distingue nice, iowait ni steal. */
        datos[i].nice = datos[i].iowait = datos[i].steal = 0;
    }

    return 0;
}

static double calcular_uso(CPUInfo anterior, CPUInfo actual) {
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

static DWORD WINAPI correr_monitor(LPVOID arg) {
    Monitor *monitor = (Monitor *)arg;
    int n = monitor->nucleos;

    CPUInfo *anterior = malloc((size_t)n * sizeof(CPUInfo));
    CPUInfo *actual = malloc((size_t)n * sizeof(CPUInfo));

    if (anterior == NULL || actual == NULL)
        return 0;

    leer_cpu(anterior, n);

    int segundo = 0;

    while (InterlockedCompareExchange(&monitor->parar, 0, 0) == 0) {
        Sleep(1000); /* sleep() en Linux, pero Sleep va en milisegundos */

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

        memcpy(anterior, actual, (size_t)n * sizeof(CPUInfo));
    }

    free(anterior);
    free(actual);

    return 0;
}

static void monitor_iniciar(Monitor *monitor) {
    monitor->max_activos = 0;
    monitor->nucleos = nucleos_logicos;

    bytes_info_cpu = (ULONG)((size_t)(monitor->nucleos + 2)
                             * sizeof(ProcessorPerformance));

    info_cpu = (ProcessorPerformance *)malloc(bytes_info_cpu);

    InterlockedExchange(&monitor->parar, 0);

    monitor->hilo = CreateThread(NULL, 0, correr_monitor, monitor, 0, NULL);
}

static void monitor_detener(Monitor *monitor) {
    InterlockedExchange(&monitor->parar, 1);

    if (monitor->hilo != NULL) {
        WaitForSingleObject(monitor->hilo, INFINITE);
        CloseHandle(monitor->hilo);
        monitor->hilo = NULL;
    }

    if (info_cpu != NULL) {
        free(info_cpu);
        info_cpu = NULL;
        bytes_info_cpu = 0;
    }
}

static void esperar_enter(void) {
#if PAUSAR

    printf("Pulsa ENTER para empezar...");
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

static Resultado medir(Modo modo, int un_nucleo, const char *nombre,
                       DWORD original) {
    Resultado resultado;

    int trabajadores = (modo == SECUENCIAL) ? 1 : obtener_trabajadores();

    fijar_afinidad(un_nucleo, original);

    printf("\n>>> %s (%d trabajadores)\n", nombre, trabajadores);

    esperar_enter();

    /* C esta en memoria compartida, los hijos empiezan a escribir
       sobre la matriz ya vacia. */
    memset(C, 0, (size_t)N * N * sizeof(int64_t));

    Monitor monitor;
    monitor_iniciar(&monitor);

    double inicio = obtener_tiempo();

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

    double fin = obtener_tiempo();
    double tiempo = fin - inicio;

    monitor_detener(&monitor);

    uint64_t suma = checksum();

    printf("Tiempo: %.3f s | Checksum: %" PRIu64 "\n", tiempo, suma);

    strcpy(resultado.nombre, nombre);
    resultado.trabajadores = trabajadores;
    resultado.tiempo = tiempo;
    resultado.activos = monitor.max_activos;
    resultado.checksum = suma;

    return resultado;
}

static void mostrar_resumen(Resultado resultados[], int cantidad) {
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

static void mostrar_menu(void) {
    printf("\n--- MENU ---\n");

    printf("1) Secuencial\n");
    printf("2) Hilos\n");
    printf("3) Procesos\n");
    printf("4) Hilos en 1 nucleo\n");
    printf("5) Procesos en 1 nucleo\n");
    printf("6) Todos y resumen\n");
    printf("7) Cambiar numero de trabajadores\n");
    printf("0) Salir\n");

    /* En Linux los trabajadores son siempre los nucleos del sistema,
       aqui el usuario puede fijarlos con la opcion 7. */
    if (num_trabajadores > 0)
        printf("Trabajadores: %d (fijado)\n", num_trabajadores);
    else
        printf("Trabajadores: %d (automatico)\n", nucleos_logicos);

    printf("Opcion: ");
    fflush(stdout);
}

static void cambiar_trabajadores(void) {
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

static void ejecutar_todos(DWORD original) {
    Resultado resultados[5];

    resultados[0] = medir(SECUENCIAL, 0, "Secuencial", original);
    resultados[1] = medir(HILOS, 0, "Hilos", original);
    resultados[2] = medir(PROCESOS, 0, "Procesos", original);
    resultados[3] = medir(HILOS, 1, "Hilos en 1 nucleo", original);
    resultados[4] = medir(PROCESOS, 1, "Procesos en 1 nucleo", original);

    mostrar_resumen(resultados, 5);
}

int main(int argc, char **argv) {
    /* El hijo de CreateProcess es otra copia de este ejecutable, asi que se
       identifica por argumentos. Esta comprobacion va PRIMERO, antes de
       reservar memoria, para que el hijo no cree matrices ni abra el menu. */
    if (argc >= 5 && strcmp(argv[1], "worker") == 0)
        return ejecutar_worker_proceso(atoi(argv[2]), atoi(argv[3]),
                                       strtoul(argv[4], NULL, 10));

    SYSTEM_INFO informacion;
    GetSystemInfo(&informacion);
    nucleos_logicos = (int)informacion.dwNumberOfProcessors;

    if (argc > 1) {
        printf("Uso: %s | %s worker inicio fin pid\n", argv[0], argv[0]);
        return EXIT_FAILURE;
    }

    /* Guardamos la afinidad original, como sched_getaffinity() en Linux. */
    DWORD afinidad_proceso = 0;
    DWORD afinidad_sistema = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &afinidad_proceso,
                           &afinidad_sistema);

    inicializar_reloj();

    if (iniciar_lector_cpu() != 0)
        printf("Aviso: sin monitor de CPU por nucleo (se seguira midiendo "
               "el tiempo)\n\n");

    unsigned long pid = (unsigned long)GetCurrentProcessId();

    construir_nombre(NOMBRE_A, PREFIJO_A, pid);
    construir_nombre(NOMBRE_B, PREFIJO_B, pid);
    construir_nombre(NOMBRE_C, PREFIJO_C, pid);

    reservar_memoria();

    /* Las matrices se generan UNA SOLA VEZ, para que todos los
       experimentos multipliquen los mismos datos y los procesos
       hijos las encuentren ya escritas en la memoria compartida. */
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
                fijar_afinidad(0, afinidad_proceso);
                liberar_memoria();
                return 0;

            case 1:
                medir(SECUENCIAL, 0, "Secuencial", afinidad_proceso);
                break;

            case 2:
                medir(HILOS, 0, "Hilos", afinidad_proceso);
                break;

            case 3:
                medir(PROCESOS, 0, "Procesos", afinidad_proceso);
                break;

            case 4:
                medir(HILOS, 1, "Hilos en 1 nucleo", afinidad_proceso);
                break;

            case 5:
                medir(PROCESOS, 1, "Procesos en 1 nucleo", afinidad_proceso);
                break;

            case 6:
                ejecutar_todos(afinidad_proceso);
                break;

            case 7:
                cambiar_trabajadores();
                break;

            default:
                printf("Opcion invalida.\n");
                break;
        }
    }

    fijar_afinidad(0, afinidad_proceso);
    liberar_memoria();

    return 0;
}