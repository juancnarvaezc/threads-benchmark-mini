"""
Laboratorio de Sistemas Operativos: procesos, hilos y planificación de CPU.
Versión Windows/Python, con compatibilidad básica para Linux.

Instalación:
    py -m pip install psutil matplotlib

Ejecución:
    py experimento_matrices_windows.py

El programa registra caracterización del equipo, mediciones individuales,
resúmenes y gráficas. Las capturas de Administrador de tareas/Monitor de
recursos y el análisis técnico se realizan por separado.

Notas metodológicas:
- Las matrices se generan antes de iniciar el cronómetro.
- El tiempo incluye la creación/gestión de trabajadores y la reconstrucción de C.
- En procesos se incluye el costo de crear el Pool y transferir las matrices.
- La utilización global de CPU es del sistema; CPU del proceso se estima sumando
  el proceso principal y los procesos hijos visibles durante el muestreo.
- En CPython estándar, el GIL limita el paralelismo de hilos Python para esta
  carga CPU-bound. Los procesos son la variante que puede usar varios núcleos.
"""

import csv
import json
import multiprocessing as mp
import os
import platform
import statistics
import subprocess
import threading
import time
from collections import defaultdict
from datetime import datetime
from pathlib import Path

import matplotlib.pyplot as plt
import psutil


# ---------------------------- CONFIGURACIÓN ----------------------------

# Tamaño práctico inicial para que el lote completo sea ejecutable en un equipo
# de escritorio. Aumenta N si las mediciones resultan demasiado cortas.
N = 1000
REPETICIONES = 3
TRABAJADORES_CANDIDATOS = [1, 2, 4, 8, 16]
SEMILLA_A = 12345
SEMILLA_B = 67890
UMBRAL_ACTIVO = 70.0
INTERVALO_MUESTREO = 0.20

CARPETA_RESULTADOS = Path(__file__).resolve().parent / "resultados_windows_python"
ARCHIVO_RAW = CARPETA_RESULTADOS / "mediciones_individuales.csv"
ARCHIVO_RESUMEN = CARPETA_RESULTADOS / "resumen_promedios.csv"
ARCHIVO_HARDWARE_JSON = CARPETA_RESULTADOS / "caracterizacion_hardware.json"
ARCHIVO_HARDWARE_TXT = CARPETA_RESULTADOS / "caracterizacion_hardware.txt"
ARCHIVO_EVIDENCIAS = CARPETA_RESULTADOS / "guia_evidencias_windows.txt"
GRAFICA_TIEMPOS = CARPETA_RESULTADOS / "tiempo_promedio.png"
GRAFICA_CPU = CARPETA_RESULTADOS / "uso_cpu_promedio.png"

MODOS = {
    "secuencial": "Secuencial",
    "hilos": "Hilos",
    "procesos": "Procesos",
    "hilos_afinidad": "Hilos en 1 procesador lógico",
    "procesos_afinidad": "Procesos en 1 procesador lógico",
}

COLUMNAS_RAW = [
    "fecha_hora", "lote", "n_matriz", "modo", "nombre_modo",
    "trabajadores", "afinidad_restringida", "afinidad_aplicada",
    "repeticion", "tiempo_segundos", "cpu_sistema_promedio_pct",
    "cpu_promedio_por_logico_pct", "cpu_max_logicos_activos",
    "cpu_proceso_y_hijos_promedio_pct", "pid_principal", "checksum",
    "checksum_correcto",
]


# ---------------------------- MULTIPLICACIÓN ----------------------------

def generar_matriz(n, semilla):
    """Genera una matriz reproducible con una semilla independiente."""
    import random
    generador = random.Random(semilla)
    return [[generador.randint(1, 8000) for _ in range(n)] for _ in range(n)]


def multiplicar_filas(A, B, fila_ini, fila_fin):
    """Calcula el bloque de filas [fila_ini, fila_fin)."""
    n = len(A)
    bloque = []
    for i in range(fila_ini, fila_fin):
        fila = [0] * n
        for k in range(n):
            aik = A[i][k]
            fila_b = B[k]
            for j in range(n):
                fila[j] += aik * fila_b[j]
        bloque.append(fila)
    return bloque


def repartir_filas(n, trabajadores):
    """Distribuye todas las filas entre trabajadores sin perder filas."""
    trabajadores = max(1, min(trabajadores, n))
    cociente, resto = divmod(n, trabajadores)
    rangos = []
    inicio = 0
    for indice in range(trabajadores):
        tamano = cociente + (1 if indice < resto else 0)
        fin = inicio + tamano
        rangos.append((inicio, fin))
        inicio = fin
    return rangos


def unir_bloques(bloques):
    return [fila for bloque in bloques for fila in bloque]


def checksum(C):
    return sum(sum(fila) for fila in C)


# ---------------------------- HARDWARE ----------------------------

def _consultar_hardware_windows():
    """Consulta modelo, sockets, núcleos y LP mediante CIM/PowerShell."""
    comando = r"""
$cpu = @(Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores, NumberOfLogicalProcessors, SocketDesignation)
$cs = Get-CimInstance Win32_ComputerSystem | Select-Object NumberOfProcessors, NumberOfLogicalProcessors
$numa = @(Get-CimInstance Win32_NumaNode -ErrorAction SilentlyContinue).Count
[pscustomobject]@{ Processors = $cpu; ComputerSystem = $cs; NumaNodes = $numa } | ConvertTo-Json -Depth 4 -Compress
"""
    ejecutable = "powershell.exe" if os.name == "nt" else "powershell"
    resultado = subprocess.run(
        [ejecutable, "-NoProfile", "-NonInteractive", "-Command", comando],
        capture_output=True, text=True, timeout=20, check=True,
    )
    datos = json.loads(resultado.stdout.strip())
    procesadores = datos.get("Processors") or []
    if isinstance(procesadores, dict):
        procesadores = [procesadores]
    sistema = datos.get("ComputerSystem") or {}
    modelos = sorted({str(p.get("Name", "")).strip() for p in procesadores if p.get("Name")})
    nucleos = sum(int(p.get("NumberOfCores") or 0) for p in procesadores)
    logicos = sum(int(p.get("NumberOfLogicalProcessors") or 0) for p in procesadores)
    designaciones = sorted({str(p.get("SocketDesignation", "")).strip() for p in procesadores if p.get("SocketDesignation")})
    return {
        "modelo_cpu": "; ".join(modelos) or platform.processor() or "No disponible",
        "sockets_cim": int(sistema.get("NumberOfProcessors") or len(procesadores) or 0),
        "designaciones_socket": designaciones,
        "nucleos_fisicos_cim": nucleos or None,
        "procesadores_logicos_cim": logicos or int(sistema.get("NumberOfLogicalProcessors") or 0) or None,
        "nodos_numa": int(datos.get("NumaNodes") or 0),
        "fuente_topologia": "Windows CIM (Win32_Processor / Win32_ComputerSystem)",
    }


def obtener_caracterizacion_hardware():
    """Recolecta hardware y entorno; usa CIM en Windows y psutil como respaldo."""
    logicos_psutil = psutil.cpu_count(logical=True)
    fisicos_psutil = psutil.cpu_count(logical=False)
    info = {
        "fecha_hora": datetime.now().isoformat(timespec="seconds"),
        "sistema_operativo": platform.platform(),
        "python": platform.python_version(),
        "arquitectura": platform.machine(),
        "nucleos_fisicos_psutil": fisicos_psutil,
        "procesadores_logicos_psutil": logicos_psutil,
        "ram_instalada_gib": round(psutil.virtual_memory().total / (1024 ** 3), 2),
        "frecuencia_cpu_mhz": None,
        "modelo_cpu": platform.processor() or "No disponible",
        "sockets_cim": None,
        "designaciones_socket": [],
        "nucleos_fisicos_cim": None,
        "procesadores_logicos_cim": None,
        "nodos_numa": None,
        "fuente_topologia": "psutil; CIM no consultado",
        "advertencia_cim": "",
    }
    frecuencia = psutil.cpu_freq()
    if frecuencia:
        info["frecuencia_cpu_mhz"] = round(frecuencia.current, 1)

    if os.name == "nt":
        try:
            info.update(_consultar_hardware_windows())
        except (OSError, subprocess.SubprocessError, json.JSONDecodeError, ValueError) as exc:
            info["advertencia_cim"] = f"No fue posible consultar CIM: {exc}"
            info["fuente_topologia"] = "psutil (respaldo; verificar con PowerShell)"
    return info


def guardar_caracterizacion(info):
    CARPETA_RESULTADOS.mkdir(parents=True, exist_ok=True)
    ARCHIVO_HARDWARE_JSON.write_text(
        json.dumps(info, indent=2, ensure_ascii=False), encoding="utf-8"
    )
    etiquetas = [
        ("Fecha y hora", info.get("fecha_hora")),
        ("Sistema operativo", info.get("sistema_operativo")),
        ("Python", info.get("python")),
        ("Arquitectura", info.get("arquitectura")),
        ("Modelo de CPU", info.get("modelo_cpu")),
        ("Sockets / procesadores físicos (CIM)", info.get("sockets_cim")),
        ("Designación(es) de socket", ", ".join(info.get("designaciones_socket") or []) or "No disponible"),
        ("Núcleos físicos (CIM)", info.get("nucleos_fisicos_cim")),
        ("Procesadores lógicos (CIM)", info.get("procesadores_logicos_cim")),
        ("Nodos NUMA", info.get("nodos_numa")),
        ("Núcleos físicos (psutil)", info.get("nucleos_fisicos_psutil")),
        ("Procesadores lógicos (psutil)", info.get("procesadores_logicos_psutil")),
        ("RAM instalada (GiB)", info.get("ram_instalada_gib")),
        ("Frecuencia actual reportada (MHz)", info.get("frecuencia_cpu_mhz")),
        ("Fuente", info.get("fuente_topologia")),
        ("Advertencia", info.get("advertencia_cim") or "Ninguna"),
    ]
    texto = "CARACTERIZACIÓN DEL EQUIPO\n" + "=" * 29 + "\n"
    texto += "\n".join(f"{nombre}: {valor if valor is not None else 'No disponible'}" for nombre, valor in etiquetas)
    texto += (
        "\n\nComando de verificación en PowerShell:\n"
        "Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores, NumberOfLogicalProcessors\n"
        "Get-CimInstance Win32_ComputerSystem | Select-Object NumberOfProcessors, NumberOfLogicalProcessors\n"
        "\nInterpreta estos datos en el informe e indica si la máquina es física o virtual.\n"
    )
    ARCHIVO_HARDWARE_TXT.write_text(texto, encoding="utf-8")
    print(texto)
    print(f"Registro de hardware: {ARCHIVO_HARDWARE_TXT}")


def obtener_trabajadores(logicos):
    """Usa 1/2/4/8 hasta los LP disponibles y permite 16 si hay más de 8."""
    logicos = logicos or 1
    valores = [n for n in (1, 2, 4, 8) if n == 1 or n <= logicos]
    if logicos > 8:
        valores.append(16)
    if not valores:
        valores = [1]
    return valores


# ---------------------------- MONITOR DE CPU ----------------------------
class MonitorCPU:
    """Muestrea uso global por LP y CPU del árbol del proceso cada 0.2 s."""
    def __init__(self, intervalo=INTERVALO_MUESTREO):
        self.intervalo = intervalo
        self.detener_evento = threading.Event()
        self.hilo = threading.Thread(target=self._muestrear, daemon=True)
        self.usos_por_muestra = []
        self.cpu_arbol_por_muestra = []
        self.max_logicos_activos = 0
        self.proceso_raiz = psutil.Process(os.getpid())
        self._procesos_vistos = {self.proceso_raiz.pid: self.proceso_raiz}
        psutil.cpu_percent(interval=None, percpu=True)
        self._preparar_contadores()

    def _preparar_contadores(self):
        try:
            self.proceso_raiz.cpu_percent(interval=None)
        except psutil.Error:
            pass
        try:
            hijos = self.proceso_raiz.children(recursive=True)
        except psutil.Error:
            hijos = []
        for hijo in hijos:
            self._procesos_vistos[hijo.pid] = hijo
            try:
                hijo.cpu_percent(interval=None)
            except psutil.Error:
                pass

    def _muestrear_una_vez(self):
        usos = psutil.cpu_percent(interval=None, percpu=True)
        if usos:
            self.usos_por_muestra.append(usos)
            activos = sum(valor >= UMBRAL_ACTIVO for valor in usos)
            self.max_logicos_activos = max(self.max_logicos_activos, activos)
        try:
            hijos = self.proceso_raiz.children(recursive=True)
        except psutil.Error:
            hijos = []
        for hijo in hijos:
            if hijo.pid not in self._procesos_vistos:
                self._procesos_vistos[hijo.pid] = hijo
                try:
                    hijo.cpu_percent(interval=None)
                except psutil.Error:
                    pass
        total = 0.0
        for pid, proceso in list(self._procesos_vistos.items()):
            try:
                total += proceso.cpu_percent(interval=None)
            except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
                self._procesos_vistos.pop(pid, None)
        self.cpu_arbol_por_muestra.append(total)

    def _muestrear(self):
        while not self.detener_evento.wait(self.intervalo):
            self._muestrear_una_vez()
        self._muestrear_una_vez()

    def iniciar(self):
        self.hilo.start()

    def detener(self):
        self.detener_evento.set()
        self.hilo.join()

    @staticmethod
    def _promedio(valores):
        return statistics.mean(valores) if valores else 0.0

    def promedio_por_logico(self):
        medias = [statistics.mean(m) for m in self.usos_por_muestra if m]
        return self._promedio(medias)

    def promedio_sistema(self):
        totales = [sum(m) / len(m) for m in self.usos_por_muestra if m]
        return self._promedio(totales)

    def promedio_arbol_proceso(self):
        return self._promedio(self.cpu_arbol_por_muestra)


# ---------------------------- TRABAJO CON HILOS Y PROCESOS ----------------------------
def ejecutar_secuencial(A, B, trabajadores):
    return multiplicar_filas(A, B, 0, len(A))


def ejecutar_hilos(A, B, trabajadores):
    rangos = repartir_filas(len(A), trabajadores)
    bloques = [None] * len(rangos)

    def tarea(indice, inicio, fin):
        bloques[indice] = multiplicar_filas(A, B, inicio, fin)

    hilos = [
        threading.Thread(target=tarea, args=(i, ini, fin))
        for i, (ini, fin) in enumerate(rangos)
    ]
    for hilo in hilos:
        hilo.start()
    for hilo in hilos:
        hilo.join()
    return unir_bloques(bloques)


_A_PROCESO = None
_B_PROCESO = None
_AFINIDAD_PROCESO = None


def inicializar_proceso(A, B, afinidad):
    """Inicializa entradas compartidas por referencia dentro del worker."""
    global _A_PROCESO, _B_PROCESO, _AFINIDAD_PROCESO
    _A_PROCESO = A
    _B_PROCESO = B
    _AFINIDAD_PROCESO = afinidad
    if afinidad:
        try:
            psutil.Process().cpu_affinity(afinidad)
        except (psutil.Error, OSError):
            # La afinidad del padre ya se aplica antes de crear el Pool y en Windows
            # los procesos hijos la heredan. No lanzar aquí: multiprocessing podría
            # reintentar indefinidamente la creación del worker.
            pass


def tarea_proceso(rango):
    ini, fin = rango
    return multiplicar_filas(_A_PROCESO, _B_PROCESO, ini, fin)


def ejecutar_procesos(A, B, trabajadores, afinidad_workers):
    rangos = repartir_filas(len(A), trabajadores)
    with mp.Pool(
        processes=trabajadores,
        initializer=inicializar_proceso,
        initargs=(A, B, afinidad_workers),
    ) as pool:
        bloques = pool.map(tarea_proceso, rangos)
    return unir_bloques(bloques)


def obtener_funcion(modo):
    if modo == "secuencial":
        return ejecutar_secuencial
    if modo in ("hilos", "hilos_afinidad"):
        return ejecutar_hilos
    if modo in ("procesos", "procesos_afinidad"):
        return ejecutar_procesos
    raise ValueError(f"Modo desconocido: {modo}")


def aplicar_afinidad(restringir, afinidad_original):
    proceso = psutil.Process()
    afinidad_objetivo = [afinidad_original[0]] if restringir else afinidad_original
    proceso.cpu_affinity(afinidad_objetivo)
    return afinidad_objetivo


# ---------------------------- REGISTRO Y MEDICIÓN ----------------------------
def guardar_medicion(registro):
    CARPETA_RESULTADOS.mkdir(parents=True, exist_ok=True)
    existe = ARCHIVO_RAW.exists()
    with ARCHIVO_RAW.open("a", newline="", encoding="utf-8-sig") as archivo:
        escritor = csv.DictWriter(archivo, fieldnames=COLUMNAS_RAW)
        if not existe:
            escritor.writeheader()
        escritor.writerow(registro)


def ejecutar_una_medicion(modo, trabajadores, repeticion, lote,
                          afinidad_original, checksum_referencia=None):
    restringir = modo in ("hilos_afinidad", "procesos_afinidad")
    funcion = obtener_funcion(modo)
    A = generar_matriz(N, SEMILLA_A)
    B = generar_matriz(N, SEMILLA_B)
    proceso_actual = psutil.Process()
    afinidad_aplicada = False
    afinidad_efectiva = None

    try:
        afinidad_efectiva = aplicar_afinidad(restringir, afinidad_original)
        afinidad_aplicada = True
        print(
            f"\n{MODOS[modo]} | N={N} | trabajadores={trabajadores} "
            f"| repetición={repeticion}/{REPETICIONES}"
        )
        monitor = MonitorCPU()
        monitor.iniciar()
        inicio = time.perf_counter()
        try:
            if modo in ("procesos", "procesos_afinidad"):
                C = funcion(A, B, trabajadores, afinidad_efectiva)
            else:
                C = funcion(A, B, trabajadores)
            tiempo = time.perf_counter() - inicio
        finally:
            monitor.detener()

        suma = checksum(C)
        correcto = True if checksum_referencia is None else suma == checksum_referencia
        registro = {
            "fecha_hora": datetime.now().isoformat(timespec="seconds"),
            "lote": lote,
            "n_matriz": N,
            "modo": modo,
            "nombre_modo": MODOS[modo],
            "trabajadores": trabajadores,
            "afinidad_restringida": restringir,
            "afinidad_aplicada": afinidad_aplicada,
            "repeticion": repeticion,
            "tiempo_segundos": round(tiempo, 6),
            "cpu_sistema_promedio_pct": round(monitor.promedio_sistema(), 3),
            "cpu_promedio_por_logico_pct": round(monitor.promedio_por_logico(), 3),
            "cpu_max_logicos_activos": monitor.max_logicos_activos,
            "cpu_proceso_y_hijos_promedio_pct": round(monitor.promedio_arbol_proceso(), 3),
            "pid_principal": os.getpid(),
            "checksum": suma,
            "checksum_correcto": correcto,
        }
        guardar_medicion(registro)
        print(
            f"Tiempo: {tiempo:.3f} s | Checksum: {suma} | "
            f"CPU sistema: {registro['cpu_sistema_promedio_pct']:.1f}% | "
            f"CPU proceso+workers aprox.: {registro['cpu_proceso_y_hijos_promedio_pct']:.1f}% | "
            f"Máx. LP activos (umbral {UMBRAL_ACTIVO:.0f}%): {monitor.max_logicos_activos}"
        )
        if not correcto:
            print("ADVERTENCIA: checksum distinto de la referencia secuencial.")
        return registro, suma
    finally:
        try:
            proceso_actual.cpu_affinity(afinidad_original)
        except (psutil.Error, OSError):
            pass


# ---------------------------- RESUMEN, TABLAS Y GRÁFICAS ----------------------------
def leer_mediciones():
    if not ARCHIVO_RAW.exists():
        return []
    with ARCHIVO_RAW.open("r", newline="", encoding="utf-8-sig") as archivo:
        return list(csv.DictReader(archivo))


def calcular_resumen(registros):
    grupos = defaultdict(list)
    for r in registros:
        clave = (r["lote"], r["n_matriz"], r["modo"], r["trabajadores"], r["afinidad_restringida"])
        grupos[clave].append(r)

    resumen = []
    for (lote, n_matriz, modo, trabajadores, afinidad), filas in sorted(
        grupos.items(), key=lambda x: (x[0][0], int(x[0][1]), x[0][2], int(x[0][3]))
    ):
        tiempos = [float(f["tiempo_segundos"]) for f in filas]
        cpu_sistema = [float(f["cpu_sistema_promedio_pct"]) for f in filas]
        cpu_proceso = [float(f["cpu_proceso_y_hijos_promedio_pct"]) for f in filas]
        checksums = {f["checksum"] for f in filas}
        correctos = all(f["checksum_correcto"].strip().lower() in ("true", "1", "sí", "si") for f in filas)
        resumen.append({
            "lote": lote,
            "n_matriz": int(n_matriz),
            "modo": modo,
            "nombre_modo": MODOS.get(modo, modo),
            "trabajadores": int(trabajadores),
            "afinidad_restringida": afinidad,
            "repeticiones": len(filas),
            "tiempo_promedio_s": statistics.mean(tiempos),
            "tiempo_min_s": min(tiempos),
            "tiempo_max_s": max(tiempos),
            "desviacion_estandar_s": statistics.stdev(tiempos) if len(tiempos) > 1 else 0.0,
            "cpu_sistema_promedio_pct": statistics.mean(cpu_sistema),
            "cpu_proceso_y_hijos_promedio_pct": statistics.mean(cpu_proceso),
            "checksum_consistente": len(checksums) == 1 and correctos,
            "checksum": next(iter(checksums)) if len(checksums) == 1 else "INCONSISTENTE",
        })

    bases = defaultdict(list)
    for fila in resumen:
        if fila["modo"] == "secuencial":
            bases[(fila["lote"], fila["n_matriz"])].append(fila["tiempo_promedio_s"])
    for fila in resumen:
        referencias = bases.get((fila["lote"], fila["n_matriz"]), [])
        base = statistics.mean(referencias) if referencias else None
        fila["speedup_vs_secuencial"] = (
            base / fila["tiempo_promedio_s"]
            if base and fila["tiempo_promedio_s"] > 0 else ""
        )
    return resumen


def guardar_resumen(resumen):
    if not resumen:
        return
    CARPETA_RESULTADOS.mkdir(parents=True, exist_ok=True)
    columnas = list(resumen[0].keys())
    with ARCHIVO_RESUMEN.open("w", newline="", encoding="utf-8-sig") as archivo:
        escritor = csv.DictWriter(archivo, fieldnames=columnas)
        escritor.writeheader()
        for fila in resumen:
            fila = fila.copy()
            for clave, valor in list(fila.items()):
                if isinstance(valor, float):
                    fila[clave] = round(valor, 4)
            escritor.writerow(fila)


def imprimir_tabla(resumen):
    print("\n=== TABLA RESUMEN ===")
    encabezado = f"{'Lote':<16} {'Modalidad':<31} {'Trab.':>5} {'Rep.':>4} {'Prom.(s)':>10} {'CPU sist.%':>10} {'CPU proc.%':>10} {'Speedup':>9} {'Checksum':>9}"
    print(encabezado)
    print("-" * len(encabezado))
    for f in resumen:
        sp = f["speedup_vs_secuencial"]
        sp_txt = f"{sp:.2f}x" if isinstance(sp, (int, float)) else "N/D"
        ok = "OK" if f["checksum_consistente"] else "REVISAR"
        print(
            f"{f['lote']:<16} {f['nombre_modo']:<31} "
            f"{f['trabajadores']:>5} {f['repeticiones']:>4} "
            f"{f['tiempo_promedio_s']:>10.3f} "
            f"{f['cpu_sistema_promedio_pct']:>9.1f}% "
            f"{f['cpu_proceso_y_hijos_promedio_pct']:>9.1f}% "
            f"{sp_txt:>9} {ok:>9}"
        )
    print(f"\nCSV individual: {ARCHIVO_RAW}\nCSV resumen:    {ARCHIVO_RESUMEN}")


def crear_graficas(resumen, trabajadores_prueba):
    if not resumen:
        print("No hay resultados para generar gráficas.")
        return
    # Las gráficas combinan los lotes disponibles: promedio por modalidad y número de trabajadores.
    plt.figure(figsize=(11, 6))
    for modo, nombre in MODOS.items():
        puntos = defaultdict(list)
        for fila in resumen:
            if fila["modo"] == modo and fila["n_matriz"] == N:
                puntos[fila["trabajadores"]].append(fila["tiempo_promedio_s"])
        xs = sorted(puntos)
        if xs:
            ys = [statistics.mean(puntos[x]) for x in xs]
            plt.plot(xs, ys, marker="o", label=nombre)
    plt.title(f"Tiempo promedio de multiplicación de matrices (N={N})")
    plt.xlabel("Número de trabajadores")
    plt.ylabel("Tiempo promedio (segundos)")
    plt.xticks(trabajadores_prueba)
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(GRAFICA_TIEMPOS, dpi=160)
    plt.close()

    plt.figure(figsize=(11, 6))
    for modo, nombre in MODOS.items():
        puntos = defaultdict(list)
        for fila in resumen:
            if fila["modo"] == modo and fila["n_matriz"] == N:
                puntos[fila["trabajadores"]].append(fila["cpu_sistema_promedio_pct"])
        xs = sorted(puntos)
        if xs:
            ys = [statistics.mean(puntos[x]) for x in xs]
            plt.plot(xs, ys, marker="o", label=nombre)
    plt.title(f"Utilización promedio de CPU del sistema (N={N})")
    plt.xlabel("Número de trabajadores")
    plt.ylabel("Uso promedio por procesador lógico (%)")
    plt.xticks(trabajadores_prueba)
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(GRAFICA_CPU, dpi=160)
    plt.close()
    print(f"Gráfica de tiempos: {GRAFICA_TIEMPOS}")
    print(f"Gráfica de CPU:     {GRAFICA_CPU}")


def actualizar_salidas(trabajadores_prueba):
    registros = leer_mediciones()
    if not registros:
        print("Todavía no existen mediciones guardadas.")
        return
    resumen = calcular_resumen(registros)
    guardar_resumen(resumen)
    imprimir_tabla(resumen)
    crear_graficas(resumen, trabajadores_prueba)


# ---------------------------- PLAN DE EXPERIMENTOS ----------------------------
def escribir_guia_evidencias(trabajadores):
    texto = f"""EVIDENCIAS WINDOWS PARA COMPLETAR APARTE
=========================================

1. Adjunta caracterizacion_hardware.txt y caracterizacion_hardware.json.
2. En PowerShell verifica la topologia con:
   Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores, NumberOfLogicalProcessors
   Get-CimInstance Win32_ComputerSystem | Select-Object NumberOfProcessors, NumberOfLogicalProcessors
3. Toma capturas del Administrador de tareas > Rendimiento > CPU (modelo, núcleos,
   procesadores lógicos y utilización) y de Detalles durante varias configuraciones.
4. Si está disponible, captura Monitor de recursos mostrando el proceso del laboratorio.
5. Guarda las capturas con nombres que indiquen modo y trabajadores (por ejemplo,
   hilos_4.png o procesos_8.png). El programa no toma capturas de pantalla.
6. Las mediciones se guardan en mediciones_individuales.csv; el resumen y las gráficas
   se regeneran desde el menú. El CPU del sistema no equivale al CPU exclusivo del programa.
7. Configuraciones elegidas según los LP detectados: {trabajadores}.
8. En CPython estándar, los hilos para esta carga CPU-bound están limitados por el GIL;
   interpreta el resultado de hilos con esa limitación.
9. La comparación Windows/Linux y las respuestas/análisis del informe quedan a cargo del estudiante.
"""
    ARCHIVO_EVIDENCIAS.write_text(texto, encoding="utf-8")


def ejecutar_lote_completo(afinidad_original, trabajadores_prueba):
    lote = datetime.now().strftime("%Y%m%d_%H%M%S")
    print(f"\nLote: {lote} | Matrices: {N}x{N} | Repeticiones/configuración: {REPETICIONES}")
    print(f"Trabajadores probados: {trabajadores_prueba}")
    print("La generación de matrices no se incluye en el tiempo medido.")
    checksum_referencia = None
    plan = [("secuencial", 1, rep) for rep in range(1, REPETICIONES + 1)]
    for modo in ("hilos", "procesos", "hilos_afinidad", "procesos_afinidad"):
        for trabajadores in trabajadores_prueba:
            for rep in range(1, REPETICIONES + 1):
                plan.append((modo, trabajadores, rep))

    for numero, (modo, trabajadores, rep) in enumerate(plan, start=1):
        print(f"\n--- Prueba {numero}/{len(plan)} ---")
        _, suma = ejecutar_una_medicion(
            modo, trabajadores, rep, lote, afinidad_original,
            checksum_referencia=checksum_referencia,
        )
        if modo == "secuencial" and checksum_referencia is None:
            checksum_referencia = suma
        elif modo == "secuencial" and suma != checksum_referencia:
            print("ADVERTENCIA: el checksum secuencial cambió entre repeticiones.")
    actualizar_salidas(trabajadores_prueba)


def mostrar_menu():
    print("\n=== EXPERIMENTO WINDOWS + PYTHON ===")
    print("1) Ejecutar conjunto completo de pruebas")
    print("2) Recalcular resumen y gráficas desde los CSV")
    print("0) Salir")
    return input("Opción: ").strip()


def main():
    CARPETA_RESULTADOS.mkdir(parents=True, exist_ok=True)
    proceso = psutil.Process()
    try:
        afinidad_original = proceso.cpu_affinity()
    except (psutil.Error, OSError):
        afinidad_original = list(range(psutil.cpu_count(logical=True) or 1))
    info_hardware = obtener_caracterizacion_hardware()
    guardar_caracterizacion(info_hardware)
    logical = info_hardware.get("procesadores_logicos_cim") or info_hardware.get("procesadores_logicos_psutil") or 1
    trabajadores_prueba = obtener_trabajadores(int(logical))
    escribir_guia_evidencias(trabajadores_prueba)
    print(f"Tamaño de matriz: {N}x{N}; repeticiones: {REPETICIONES}")
    print(f"Trabajadores disponibles para las pruebas: {trabajadores_prueba}")
    print(f"Afinidad disponible para el proceso: {afinidad_original}")
    if int(logical) < 8:
        print("Nota: se omiten configuraciones con más trabajadores que procesadores lógicos detectados.")
    print("Nota: CPython estándar limita el paralelismo de los hilos Python en cargas CPU-bound (GIL).")
    print(f"Guía de evidencias: {ARCHIVO_EVIDENCIAS}")

    try:
        while True:
            opcion = mostrar_menu()
            if opcion == "0":
                break
            if opcion == "1":
                confirmar = input(
                    f"Se ejecutarán {REPETICIONES} repeticiones por configuración. "
                    f"N={N}; el lote puede tardar. ¿Continuar? (s/n): "
                ).strip().lower()
                if confirmar in ("s", "si", "sí"):
                    ejecutar_lote_completo(afinidad_original, trabajadores_prueba)
            elif opcion == "2":
                actualizar_salidas(trabajadores_prueba)
            else:
                print("Opción no válida.")
    finally:
        try:
            proceso.cpu_affinity(afinidad_original)
        except (psutil.Error, OSError):
            pass


if __name__ == "__main__":
    mp.freeze_support()
    main()
