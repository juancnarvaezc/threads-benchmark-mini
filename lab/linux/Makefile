# =========================================================
#  Makefile de los experimentos del laboratorio (Linux)
#
#    make          compila todo
#    make limpiar  borra los ejecutables
#
#  exp3_hilos se compila con -O0 porque asi lo indica la guia
#  para el ejemplo de hilos. Los demas usan -O2, como matrix_mult.
#  En los dos casos el volatile de trabajo_cpu evita que el
#  compilador borre el trabajo del bucle.
# =========================================================

CC = gcc
CFLAGS = -O2 -Wall -pthread

todo: exp1_hardware exp3_hilos benchmark exp5_observar

exp1_hardware: exp1_hardware.c
	$(CC) $(CFLAGS) -o $@ $<

exp3_hilos: exp3_hilos.c
	$(CC) -O0 -Wall -pthread -o $@ $<

benchmark: benchmark.c comun.h
	$(CC) $(CFLAGS) -o benchmark benchmark.c

exp5_observar: exp5_observar.c comun.h
	$(CC) $(CFLAGS) -o exp5_observar exp5_observar.c

limpiar:
	rm -f exp1_hardware exp3_hilos benchmark exp5_observar

.PHONY: todo limpiar
