# Experimentos del laboratorio en Linux (C)

Implementación en C de los experimentos del documento
`../Laboratorio_de_Procesos_e_Hilos_en_Windows_y_Linux.md`.
**Solo la parte de Linux y C.**

## Compilar

```bash
make
```

También se puede compilar a mano:

```bash
gcc -O2 -Wall -pthread benchmark.c -o benchmark
gcc -O0 -Wall -pthread exp3_hilos.c -o exp3_hilos
```

`exp3_hilos` usa `-O0` porque así lo compila la guía; los demás usan
`-O2` (como `matrix_mult`). En ambos casos el `volatile` de la carga
evita que el compilador borre el trabajo.

## Qué programa usar para cada experimento

| Exp. | Programa | Qué produce |
| ---- | -------- | ----------- |
| 1. Hardware | `./exp1_hardware` y `sh exp1_hardware.sh` | Datos para la tabla §7.2 + evidencia en `evidencias/exp1_hardware.txt` |
| 2. Proceso vs hilo | `./benchmark` → opciones 1, 2, 3 | Tiempo y %CPU de las tres formas |
| 3. Crear hilos | `./exp3_hilos` | El ejemplo de la guía (4 hilos) |
| 4. Concurrencia y paralelismo | `./benchmark` → opciones 6 y 4 | Tabla §10 (barrido) y prueba de 1 núcleo |
| 5. Scheduler y CPU | `./exp5_observar 8` | Proceso con hilos vivos para observar con `htop`, `ps -eLf`, `pidstat` |
| 6. Procesos vs hilos | `./benchmark` → opción 5 | Costo de crear N hilos vs N procesos + tiempos con la carga |
| 7. Medición del tiempo | `./benchmark` → opción 6 | Tabla §13 con R1, R2, R3, promedio, %CPU y speedup |

## Cómo funciona `benchmark`

```
--- MENU ---
1) Secuencial
2) Hilos
3) Procesos
4) Hilos en 1 nucleo (concurrencia)
5) Hilos vs Procesos (experimento 6)
6) Barrido de hilos con repeticiones (experimentos 4 y 7)
7) Cambiar trabajadores
8) Cambiar iteraciones y repeticiones
9) Activar pausa antes de cada prueba (para htop)
0) Salir
```

Puntos clave para interpretar bien:

- **El trabajo total se reparte** entre los hilos o procesos. Con 4
  hilos cada uno hace 1/4 de las iteraciones; entre todos hacen lo
  mismo que el secuencial. Por eso el tiempo baja si hay núcleos
  libres (sin reparto, el tiempo no bajaría nunca).
- **%CPU** = tiempo de CPU (usuario + sistema) / tiempo de pared.
  4 hilos a tope dan ≈ 400%. Los hijos de `fork` se cuentan con
  `RUSAGE_CHILDREN`.
- **Opción 4 (1 núcleo)**: los hilos heredan la afinidad del proceso
  fijada a un solo procesador lógico con `sched_setaffinity`. Si el
  tiempo se queda como el secuencial y el %CPU ≈ 100, hay
  concurrencia pero no paralelismo.
- **Opción 7**: se permiten más trabajadores que núcleos lógicos,
  para responder qué pasa cuando los hilos superan los
  procesadores lógicos (pregunta 8).
- **Opción 9**: pausa antes de cada prueba para abrir `htop` y ver
  la utilización en vivo. Desactivada por defecto para que las
  tablas se generen seguidas.

## Ajustar la carga

El valor por defecto son 1000 millones de iteraciones. Si el
secuencial tarda demasiado (o muy poco), cámbialo:

```bash
./benchmark 50000000        # desde la linea de comandos
# o dentro del menu, opcion 8
```

Objetivo: que la versión secuencial tarde **unos pocos segundos**,
para que los tiempos se midan bien.

## Flujo sugerido para el informe

1. `sh exp1_hardware.sh` y `./exp1_hardware` → tabla §7.2 y evidencia.
2. `./exp3_hilos` → evidencia del experimento 3 (crear y esperar hilos).
3. `./benchmark` → opción 6 → copiar la tabla (§10 y §13).
4. `./benchmark` → opción 4 → evidencia de concurrencia en 1 núcleo.
5. `./benchmark` → opción 5 → datos para la tabla §12.
6. `./exp5_observar 8` → capturas de `htop` y `pidstat` (§11.2).

## Notas

- El acumulador de `trabajo_cpu` es local a cada hilo a propósito:
  un `volatile` global compartido haría que todos los hilos pelearan
  por la misma línea de caché y el paralelismo desaparecería.
- El `Resultado` que imprime `benchmark` solo existe para que el
  compilador no pueda descartar el trabajo. **No** es comparable entre
  configuraciones: cambia con el reparto porque cada trabajador
  reinicia su contador local.
- Los procesos hijos devuelven su resultado por memoria compartida
  (`mmap MAP_SHARED`): es la excusa para ver que un hijo **no** ve la
  memoria del padre sin un mecanismo así.
- Los ejecutables y `evidencias/` están en el `.gitignore`.
