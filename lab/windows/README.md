# Laboratorio de Procesos e Hilos en Windows (C)

Implementación en C de los experimentos del documento
`../Laboratorio_de_Procesos_e_Hilos_en_Windows_y_Linux.md`.
**Solo la parte de Windows.** Es la versión equivalente a `../linux/laboratorios.c`:
misma lógica y mismo menú, solo cambian las APIs del sistema operativo.

La carga de trabajo es una **multiplicación de matrices** (secuencial, con hilos
y con procesos), adaptada de `matrix_mult/windows_threads.c`.

## Compilar y ejecutar

```powershell
mingw32-make
.\laboratorios.exe
```

A mano (MinGW):

```powershell
gcc -O2 -Wall laboratorios.c -o laboratorios.exe
```

No necesita librerías extra: el monitor de CPU usa `ZwQuerySystemInformation`
de `ntdll.dll`, resuelto en runtime con `GetProcAddress`.

## Menú

Cada opción del menú principal es un laboratorio. Los que ejecutan código abren
un submenú para configurar la corrida (`0) Volver` regresa).

```
1) Lab 1 - Identificacion del hardware
2) Lab 2 - Proceso frente a hilo
3) Lab 3 - Creacion de hilos
4) Lab 4 - Concurrencia y paralelismo
5) Lab 5 - Scheduler y uso de CPU
6) Lab 6 - Procesos vs hilos
7) Lab 7 - Medicion del tiempo (repeticiones)
0) Salir
```

## Qué laboratorio alimenta cada tabla

| Lab | Exp. | Sección | Opción del menú | Qué produce |
| --- | ---- | ------- | --------------- | ----------- |
| 1 | 1 | §7.1 | Lab 1 | Salida de `Get-CimInstance Win32_Processor` / `Win32_ComputerSystem` y `%NUMBER_OF_PROCESSORS%` |
| 2 | 2 | §8 | Lab 2 → 1/2/3 o 4 | Tiempo de secuencial vs hilos vs procesos |
| 3 | 3 | §9.2 | Lab 3 → 1 | Ejemplo de `CreateThread` / espera |
| 4 | 4 | §10, §10.1 | Lab 4 → 1/2/3 | Hilos a N trabajadores, 1 núcleo y resumen de la tabla §10 |
| 5 | 5 | §11.1 | Lab 5 → 1 | Monitor por núcleo en vivo para observar con el Administrador de tareas |
| 6 | 6 | §12 | Lab 6 → 1 | Hilos vs procesos: tiempo, núcleos activos y speedup |
| 7 | 7 | §13 | Lab 7 → 1 | Barrido 1, 2, 4, 8, 16 × R repeticiones → tabla §13 |

## Ajustar la carga

`N` es el tamaño de la matriz (N×N), **2000 por defecto**, y se cambia en
runtime desde **Lab 7 → 3) Cambiar N**. Objetivo: que la versión secuencial
tarde unos pocos segundos. Para probar rápido, baja `N` (p. ej. 300).

`R` (repeticiones del barrido) también es configurable: **Lab 7 → 2**, 3 por
defecto.

Además, en los submenús "Cambiar trabajadores" se puede fijar cuántos hilos o
procesos se usan (0 = automático = núcleos lógicos). Se permiten más
trabajadores que núcleos lógicos para responder la pregunta 8.

## Cómo leer los resultados

- **El trabajo total se reparte** entre los hilos/procesos: con 4 hilos cada uno
  hace 1/4 de las filas. Por eso el tiempo baja si hay núcleos libres.
- **Checksum**: `medir` limpia `C`, ejecuta y suma toda la matriz. Si todas las
  corridas dan el mismo checksum, el reparto fue correcto (no se solapan rangos
  ni se pierden filas).
- **Speedup**: tiempo base / tiempo de la corrida. Cercano al número de núcleos
  en una máquina libre.
- **Núcleos activos**: cuántos procesadores lógicos superaron el umbral de uso
  (70 %) mientras corría el experimento.
- **1 núcleo (Lab 4)**: los hilos heredan la afinidad del proceso fijada a un
  solo procesador lógico con `SetProcessAffinityMask`. Si el tiempo se queda
  como el secuencial, hay concurrencia pero no paralelismo.
- **Pausa**: los submenús tienen una opción para pausar antes de cada corrida y
  abrir el Administrador de tareas. Está **apagada por defecto** para que el
  barrido corra solo.

## Flujo sugerido para el informe

1. Lab 1 → copiar la salida a la tabla §7.1.
2. Lab 3 → 1 → evidencia de crear y esperar hilos.
3. Lab 2 → 4 → tiempos de secuencial, hilos y procesos.
4. Lab 4 → 3 → resumen con speedup y núcleos activos (§10).
5. Lab 5 → 1 → capturas del Administrador de tareas / Monitor de recursos (§11.1).
6. Lab 6 → 1 → tabla §12.
7. Lab 7 → 1 → tabla §13.

## Bibliotecas usadas

| Librería | Tipo | Instrucciones usadas |
| -------- | ---- | -------------------- |
| `windows.h` | Hilos | `CreateThread`, `WaitForSingleObject`, `CloseHandle`, `HANDLE` |
| `windows.h` | Procesos | `CreateProcessA`, `STARTUPINFOA`, `PROCESS_INFORMATION`, `GetCurrentProcessId` |
| `windows.h` | Memoria compartida entre procesos | `CreateFileMappingA`, `MapViewOfFile`, `UnmapViewOfFile`, `FILE_MAP_ALL_ACCESS` |
| `windows.h` | Afinidad de CPU | `SetProcessAffinityMask`, `GetProcessAffinityMask`, `GetCurrentProcess` |
| `windows.h` | Medición del tiempo | `QueryPerformanceCounter`, `QueryPerformanceFrequency`, `LARGE_INTEGER` |
| `windows.h` | Detección de hardware y API dinámica | `GetSystemInfo`, `GetModuleHandleA`, `GetProcAddress` |
| `windows.h` | Sincronización entre hilos | `InterlockedExchange`, `InterlockedCompareExchange`, `volatile LONG`, `Sleep` |
| `ntdll.dll` (resuelta en runtime) | Monitor de CPU por núcleo | `ZwQuerySystemInformation` |
| `inttypes.h` | Impresión de enteros de 64 bits | `PRIu64` |
| `stdint.h` | Enteros de ancho fijo | `int64_t`, `uint64_t` |
| `string.h` | Cadenas y memoria | `memset`, `memcpy`, `strcpy`, `sprintf` |
| `stdio.h` | Entrada/salida | `printf`, `fgets`, `fflush`, `system` |
| `stdlib.h` | Memoria y utilidades | `malloc`, `free`, `atoi`, `exit` |

El Lab 1 no usa las APIs de C para el hardware: delega en PowerShell
(`Get-CimInstance`) y en `%NUMBER_OF_PROCESSORS%` a través de `system()`.

## Diferencias con la versión Linux

| Concepto | Linux (`../linux`) | Windows (aquí) |
| --- | --- | --- |
| Crear hilo | `pthread_create()` | `CreateThread()` |
| Esperar hilo | `pthread_join()` | `WaitForSingleObject()` |
| Crear proceso | `fork()` | `CreateProcess()` del mismo `.exe` |
| Memoria compartida | `mmap(MAP_SHARED)` solo para C | `CreateFileMapping()` para **A, B y C** |
| Afinidad | `sched_setaffinity()` | `SetProcessAffinityMask()` |
| Tiempo | `clock_gettime()` | `QueryPerformanceCounter()` |
| CPU por núcleo | `/proc/stat` | `ZwQuerySystemInformation()` |
| Núcleos | `sysconf()` | `GetSystemInfo()` |
| Parar el monitor | `atomic_bool` | `InterlockedExchange()` |

En Windows no hay `fork()`, así que el hijo es **otra copia del ejecutable** y
**A, B y C** tienen que estar en memoria compartida. Por eso el proceso hijo se
lanza como `laboratorios.exe worker inicio fin pid N` (recibe también `N`
porque arranca con el valor por defecto).

## Notas

- Mismos `N`, semillas y algoritmo que la versión Linux: para el mismo `N` el
  `checksum` coincide entre ambos sistemas (evidencia de la comparación §16).
- La matriz C se usa `int64_t` porque los resultados no caben en un `int` de
  32 bits.
- Los objetos de memoria compartida llevan el PID en el nombre, para que dos
  instancias del programa no se pisen entre sí.
- `SetProcessAffinityMask` trabaja con una máscara de hasta 32 procesadores
  lógicos; en equipos con más hay que usar grupos de procesadores.
- El ejecutable `laboratorios.exe` está en el `.gitignore`.
