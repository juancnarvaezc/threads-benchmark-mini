# threads-benchmark-mini

Benchmark para medir el rendimiento de una multiplicación de matrices grandes
usando hilos y procesos en varios procesadores lógicos, con una versión
equivalente para Linux y otra para Windows.

La idea es comparar tres formas de hacer el mismo trabajo:

-   **Secuencial**: un solo hilo o proceso.
-   **Hilos**: `pthread_create` / `CreateThread`.
-   **Procesos**: `fork` / `CreateProcess`.

Y además forzar la ejecución a un solo núcleo, para comprobar que la ganancia
de la paralelización viene realmente de usar varios núcleos y no de otra
cosa.

------------------------------------------------------------------------

# Archivos

| Archivo              | Sistema  | Notas                                  |
| -------------------- | -------- | -------------------------------------- |
| `linux_threads.c`    | Linux    | pthread, fork, /proc/stat              |
| `windows_threads.c`  | Windows  | WinAPI, CreateProcess, ntdll           |

Los dos programas son equivalentes línea a línea en cuanto a lógica: mismos
experimentos, mismo menú, mismos mensajes y mismo checksum esperado. Solo
cambian las llamadas al sistema.

------------------------------------------------------------------------

# Compilación

Linux:

```bash
gcc linux_threads.c -O2 -pthread -o threads
```

Windows (MinGW):

```powershell
gcc windows_threads.c -O2 -o windows_threads.exe
```

Windows no necesita librerías extra: el monitor de CPU usa
`ZwQuerySystemInformation` de `ntdll.dll`, que se resuelve en tiempo de
ejecución con `GetProcAddress`, porque el MinGW.org no trae `pdh.h`.

------------------------------------------------------------------------

# Uso

Al arrancar muestra los núcleos lógicos disponibles y el tamaño de la matriz:

    16 nucleos logicos | N=2000

    --- MENU ---
    1) Secuencial
    2) Hilos
    3) Procesos
    4) Hilos en 1 nucleo
    5) Procesos en 1 nucleo
    6) Todos y resumen
    7) Cambiar numero de trabajadores
    0) Salir
    Trabajadores: 16 (automatico)
    Opcion:

La opción **7** permite fijar cuántos procesadores lógicos usar en los
experimentos paralelos (0 vuelve al modo automático). El valor se muestra en
cada ejecución:

    >>> Hilos (4 trabajadores)

    Tiempo: 8.412 s | Checksum: 127994057854419456

El experimento 6 ejecuta los cinco casos seguidos y muestra el resumen con el
speedup respecto al secuencial, el número de núcleos activos que se detectaron
y la comprobación de que todos los checksums coinciden:

    Experimento               Trab     Tiempo   Speedup   Activos
    -------------------------------------------------------------
    Secuencial                   1    33.51s     1.00x         4
    Hilos                       16     4.64s     7.22x        15
    Procesos                    16     4.76s     7.04x        15
    Hilos en 1 nucleo           16    34.25s     0.98x         1
    Procesos en 1 nucleo        16    35.55s     0.94x         1

    Checksum: igual en todos

------------------------------------------------------------------------

# Qué se mide

-   **Tiempo**: `clock_gettime(CLOCK_MONOTONIC)` en Linux y
    `QueryPerformanceCounter()` en Windows.
-   **Núcleos activos**: un hilo auxiliar muestrea el uso de cada núcleo cada
    segundo (`/proc/stat` en Linux, `ZwQuerySystemInformation` en Windows) y
    cuenta los que superan el `UMBRAL_ACTIVO` (70 %). El máximo se guarda en
    la columna "Activos".

La monitorización por líneas es en vivo, durante la ejecución:

    t= 1s | 100 100  90  90 100 100 ... | activos: 15

------------------------------------------------------------------------

# Diferencias entre los dos sistemas

| Concepto            | Linux                          | Windows                                  |
| ------------------- | ------------------------------ | ---------------------------------------- |
| Crear hilo          | `pthread_create()`             | `CreateThread()`                         |
| Esperar hilo        | `pthread_join()`               | `WaitForSingleObject()`                  |
| Crear proceso       | `fork()`                       | `CreateProcess()` del mismo ejecutable    |
| Memoria compartida  | `mmap(MAP_SHARED)`             | `CreateFileMapping()` + `MapViewOfFile()`|
| Afinidad            | `sched_setaffinity()`          | `SetProcessAffinityMask()`               |
| Tiempo              | `clock_gettime()`              | `QueryPerformanceCounter()`              |
| CPU por núcleo      | `/proc/stat`                   | `ZwQuerySystemInformation()`             |
| Núcleos             | `sysconf()`                    | `GetSystemInfo()`                        |
| Parar el monitor    | `atomic_bool`                  | `InterlockedExchange()`                  |

Lo más relevante: en Linux los hijos de `fork()` heredan la memoria del
padre, así que solo C necesita estar compartida. En Windows no hay `fork()`, el
hijo es otra copia del ejecutable y **A, B y C** tienen que estar en memoria
compartida para que pueda leerlas.

------------------------------------------------------------------------

# Notas

-   `N` (tamaño de las matrices) es el mismo en los dos archivos. Si se cambia
    en uno hay que cambiarlo en el otro, porque el checksum depende de `N`.
-   La matriz C se usa `int64_t` porque los resultados no caben en un `int` de
    32 bits.
-   Las matrices se generan una sola vez con semillas fijas, por lo que
    cualquier ejecución da el mismo checksum.
-   `PAUSAR` está activado: el programa espera a que pulses ENTER antes de
    cada experimento, para que puedas abrir `htop` o el Administrador de
    tareas.
-   En Windows los objetos de memoria compartida llevan el PID en el nombre,
    para que dos instancias del programa no se pisen entre sí.