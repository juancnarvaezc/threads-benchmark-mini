# =========================================================
#  Makefile del laboratorio (Windows, MinGW)
#
#    mingw32-make          compila laboratorios.exe
#    mingw32-make limpiar  borra el ejecutable
#
#  Un solo programa, laboratorios.c, con una multiplicacion de
#  matrices como carga (adaptado de matrix_mult/windows_threads.c).
# =========================================================

CC = gcc
CFLAGS = -O2 -Wall

todo: laboratorios.exe

laboratorios.exe: laboratorios.c
	$(CC) $(CFLAGS) -o $@ $<

limpiar:
	-del /Q laboratorios.exe 2>NUL

.PHONY: todo limpiar
