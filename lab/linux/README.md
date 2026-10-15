# Laboratorio de Procesos e Hilos en Linux (C)

Implementación en C de los experimentos del documento
`../Laboratorio_de_Procesos_e_Hilos_en_Windows_y_Linux.md`.
**Solo la parte de Linux y C.** El port a Windows se hará después.

La carga de trabajo es una **multiplicación de matrices** (secuencial, con hilos
y con procesos), adaptada de `matrix_mult/linux_threads.c`.

## Compilar y ejecutar

```bash
make
./laboratorios
```

A mano:

```bash
gcc -O2 -Wall -pthread laboratorios.c -o laboratorios
```

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
| 1 | 1 | §7.2 | Lab 1 | Salida de `lscpu`, `lscpu -e` y `nproc` → tabla §7.2 |
| 2 | 2 | §8 | Lab 2 → 1/2/3 o 4 | Tiempo de secuencial vs hilos vs procesos |
| 3 | 3 | §9.1 | Lab 3 → 1 | Ejemplo de `pthread_create` / `pthread_join` |
| 4 | 4 | §10, §10.1 | Lab 4 → 1/2/3 | Hilos a N trabajadores, 1 núcleo y resumen de la tabla §10 |
| 5 | 5 | §11.2 | Lab 5 → 1 | Monitor por núcleo en vivo para observar con `htop`/`pidstat` |
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
  (70 %) mientras corría el experimento. Lo mide el monitor de `/proc/stat`.
- **1 núcleo (Lab 4)**: los hilos heredan la afinidad del proceso fijada a un
  solo procesador lógico con `sched_setaffinity`. Si el tiempo se queda como el
  secuencial, hay concurrencia pero no paralelismo.
- **Pausa**: los submenús tienen una opción para pausar antes de cada corrida y
  abrir `htop`. Está **apagada por defecto** para que el barrido corra solo.

## Flujo sugerido para el informe

1. Lab 1 → copiar la salida a la tabla §7.2.
2. Lab 3 → 1 → evidencia de crear y esperar hilos.
3. Lab 2 → 4 → tiempos de secuencial, hilos y procesos.
4. Lab 4 → 3 → resumen con speedup y núcleos activos (§10).
5. Lab 5 → 1 → capturas de `htop` y `pidstat` (§11.2).
6. Lab 6 → 1 → tabla §12.
7. Lab 7 → 1 → tabla §13.

## Notas

- El acumulador de cada hilo es local a propósito: un `volatile` global
  compartido haría que todos pelearan por la misma línea de caché y el
  paralelismo desaparecería.
- Los procesos hijos devuelven el resultado por memoria compartida
  (`mmap MAP_SHARED`) porque un hijo **no** ve la memoria del padre sin un
  mecanismo así.
- El ejecutable `laboratorios` está en el `.gitignore`.
