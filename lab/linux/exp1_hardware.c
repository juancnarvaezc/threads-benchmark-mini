/*
===========================================================
 exp1_hardware.c

 EXPERIMENTO 1: identificacion del hardware de ejecucion.

 Antes de crear hilos hay que saber con cuantos procesadores
 logicos se puede contar. Este programa imprime:

  - procesadores logicos que ve el sistema (sysconf)
  - en cuales puede realmente ejecutarse este proceso (afinidad)
  - sockets, nucleos fisicos e hilos por nucleo segun /proc/cpuinfo
  - nodos NUMA activos del sistema

 Complementa los comandos del laboratorio (lscpu, lscpu -e, nproc),
 que exp1_hardware.sh guarda como evidencia.

 Compilar:
    gcc -O2 -Wall exp1_hardware.c -o exp1_hardware
===========================================================
*/

#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>

/* Tope de procesadores logicos que sabemos guardar. De sobra. */
#define MAX_CPUS 1024

/* Lo que nos interesa de cada procesador logico. */
typedef struct {
    int procesador;  /* campo "processor"                    */
    int socket;      /* campo "physical id"; -1 si no aparece */
    int nucleo;      /* campo "core id";     -1 si no aparece */
} CPUInfo;

/* Un socket es una CPU fisica instalada en la placa. Contamos
   cuantos valores distintos de "physical id" hay. */
static int contar_sockets(const CPUInfo cpus[], int total) {
    int sockets = 0;

    for (int i = 0; i < total; i++) {
        int repetido = 0;

        for (int j = 0; j < i; j++) {
            if (cpus[j].socket == cpus[i].socket) {
                repetido = 1;
                break;
            }
        }

        if (!repetido)
            sockets++;
    }

    return sockets;
}

/* Un nucleo fisico es un par (socket, nucleo): con SMT varios
   procesadores logicos comparten el mismo par. */
static int contar_nucleos_fisicos(const CPUInfo cpus[], int total) {
    int nucleos = 0;

    for (int i = 0; i < total; i++) {
        int repetido = 0;

        for (int j = 0; j < i; j++) {
            if (cpus[j].socket == cpus[i].socket
                && cpus[j].nucleo == cpus[i].nucleo) {
                repetido = 1;
                break;
            }
        }

        if (!repetido)
            nucleos++;
    }

    return nucleos;
}

/* Lee /proc/cpuinfo y resume la topologia. */
static void leer_cpuinfo(void) {
    static CPUInfo cpus[MAX_CPUS];
    int total = 0;
    char linea[256];
    char modelo[128] = "(no encontrado)";
    int nucleos_por_socket = -1;  /* campo "cpu cores" */
    int hermanos_por_socket = -1; /* campo "siblings"   */
    CPUInfo actual;
    int hay_actual = 0;

    FILE *archivo = fopen("/proc/cpuinfo", "r");

    if (archivo == NULL) {
        perror("fopen /proc/cpuinfo");
        return;
    }

    memset(&actual, 0, sizeof(actual));

    while (fgets(linea, sizeof(linea), archivo) != NULL) {
        int valor;

        /* Cada bloque de /proc/cpuinfo empieza con "processor".
           Cuando aparece uno nuevo, guardamos el anterior. */
        if (sscanf(linea, "processor : %d", &valor) == 1) {
            if (hay_actual && total < MAX_CPUS)
                cpus[total++] = actual;

            memset(&actual, 0, sizeof(actual));
            actual.procesador = valor;
            actual.socket = -1;
            actual.nucleo = -1;
            hay_actual = 1;
        } else if (sscanf(linea, "physical id : %d", &valor) == 1) {
            actual.socket = valor;
        } else if (sscanf(linea, "core id : %d", &valor) == 1) {
            actual.nucleo = valor;
        } else if (modelo[0] == '(') {
            /* Solo interesa el "model name" de la primera CPU. */
            sscanf(linea, "model name : %127[^\n]", modelo);
        }

        /* Estos dos campos son iguales en todas las CPUs: el
           primero que aparezca vale. */
        if (sscanf(linea, "cpu cores : %d", &valor) == 1
            && nucleos_por_socket < 0)
            nucleos_por_socket = valor;

        if (sscanf(linea, "siblings : %d", &valor) == 1
            && hermanos_por_socket < 0)
            hermanos_por_socket = valor;
    }

    /* Falta el ultimo bloque, que no tiene "processor" detras. */
    if (hay_actual && total < MAX_CPUS)
        cpus[total++] = actual;

    fclose(archivo);

    printf("\n-- Segun /proc/cpuinfo --\n");
    printf("Modelo de CPU: %s\n", modelo);
    printf("Procesadores logicos: %d\n", total);

    if (total == 0) {
        printf("No se encontro informacion en /proc/cpuinfo.\n");
        return;
    }

    /* Si el sistema no informa la topologia (pasa en algunas ARM
       o maquinas virtuales), avisamos en vez de inventar numeros. */
    int completos = 1;

    for (int i = 0; i < total; i++) {
        if (cpus[i].socket < 0 || cpus[i].nucleo < 0) {
            completos = 0;
            break;
        }
    }

    if (!completos) {
        printf("Este sistema no informa sockets/nucleos en /proc/cpuinfo.\n");
        printf("Usa lscpu para ver la topologia completa.\n");
        return;
    }

    int sockets = contar_sockets(cpus, total);
    int nucleos = contar_nucleos_fisicos(cpus, total);

    printf("Sockets (CPUs fisicas): %d\n", sockets);
    printf("Nucleos fisicos: %d (%d por socket)\n", nucleos,
           nucleos / sockets);
    printf("Hilos por nucleo (SMT): %d\n", total / nucleos);

    if (nucleos_por_socket > 0)
        printf("El kernel informa \"cpu cores\": %d\n", nucleos_por_socket);

    if (hermanos_por_socket > 0)
        printf("El kernel informa \"siblings\": %d\n", hermanos_por_socket);
}

/* Los nodos NUMA se listan en /sys. El archivo "online" dice cuales
   hay activos, por ejemplo "0" o "0-1". */
static void leer_numa(void) {
    FILE *archivo = fopen("/sys/devices/system/node/online", "r");
    char texto[64] = "(no se pudo leer)";

    if (archivo != NULL) {
        if (fgets(texto, sizeof(texto), archivo) == NULL)
            strcpy(texto, "(vacio)");

        fclose(archivo);
    }

    printf("\nNodos NUMA activos: %s", texto);

    if (strchr(texto, '\n') == NULL)
        printf("\n");
}

int main(void) {
    printf("== EXPERIMENTO 1: hardware de ejecucion ==\n\n");

    long en_linea = sysconf(_SC_NPROCESSORS_ONLN);
    long configurados = sysconf(_SC_NPROCESSORS_CONF);

    printf("-- Segun sysconf --\n");
    printf("Procesadores logicos en linea:      %ld\n", en_linea);
    printf("Procesadores logicos configurados:  %ld\n", configurados);

    /* La afinidad dice en que procesadores logicos puede ejecutarse
       este proceso. Normalmente son todos, pero puede estar
       limitada, por ejemplo si se arranca con taskset. */
    cpu_set_t mascara;

    if (sched_getaffinity(0, sizeof(mascara), &mascara) == 0) {
        printf("Permitidos para este proceso:      %d (",
               CPU_COUNT(&mascara));

        for (int i = 0; i < CPU_SETSIZE; i++) {
            if (CPU_ISSET(i, &mascara))
                printf("%d ", i);
        }

        printf(")\n");
    }

    leer_cpuinfo();
    leer_numa();

    printf("\nEjecuta tambien lscpu, lscpu -e y nproc (exp1_hardware.sh\n");
    printf("los guarda como evidencia para el informe).\n");

    return 0;
}
