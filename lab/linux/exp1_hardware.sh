#!/bin/sh
# =========================================================
#  exp1_hardware.sh
#
#  EXPERIMENTO 1 (Linux): identificacion del hardware.
#
#  Ejecuta los comandos que pide el laboratorio (lscpu,
#  lscpu -e y nproc) y guarda la salida como evidencia en
#  evidencias/exp1_hardware.txt para pegarla en el informe.
#
#  Uso:  sh exp1_hardware.sh
# =========================================================

SALIDA="evidencias/exp1_hardware.txt"
mkdir -p evidencias

# Ejecuta un comando si existe; si no, deja nota en la evidencia.
ejecutar() {
    if command -v "$1" >/dev/null 2>&1; then
        "$@"
    else
        echo "($1 no esta instalado en este sistema)"
    fi
}

{
    echo "== Fecha =="
    date
    echo
    echo "== lscpu =="
    ejecutar lscpu
    echo
    echo "== lscpu -e (topologia por procesador logico) =="
    ejecutar lscpu -e
    echo
    echo "== nproc =="
    ejecutar nproc
} | tee "$SALIDA"

echo
echo "Evidencia guardada en $SALIDA"
