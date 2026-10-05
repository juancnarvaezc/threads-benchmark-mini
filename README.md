# threads-benchmark-mini
Benchmark para probar el rendimiento de multiplicaciones de mátrices grandes usando hilos en múltiples procesadores lógicos.

>[!warning]
> El ejecutable fue compilado en usando gcc en Linux. **No correrá en Windows** sin que se compile nuevamente en una máquina con Windows.
> Para ello, se dispone del comando `gcc threads.c -O2 -o threads`, lo que genera compila el código y devuelve el ejecutable.

>[!note]
> El código cuenta con múltiples optimizaciones realizadas con IA en cuanto a la algoritmia con la que se llenan y se realizan los cálculos en las mátrices, así como en algunas comparaciones. El propósito de optimizar el código con IA es que la diferencia de tiempos de ejecución entre los métodos empleados sean por los métodos en sí mismos.
