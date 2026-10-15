# =========================================================
#  Makefile del laboratorio (Linux)
#
#    make          compila laboratorios
#    make limpiar  borra el ejecutable
#
#  Un solo programa, laboratorios.c, con una multiplicacion de
#  matrices como carga (adaptado de matrix_mult/linux_threads.c).
# =========================================================

CC = gcc
CFLAGS = -O2 -Wall -pthread

todo: laboratorios

laboratorios: laboratorios.c
	$(CC) $(CFLAGS) -o $@ $<

limpiar:
	rm -f laboratorios

.PHONY: todo limpiar
