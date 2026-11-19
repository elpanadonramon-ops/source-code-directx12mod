# Lineas, intento 2 (SIN PROBAR) - solo cambia Dx12Device.java

Extrae encima del proyecto y ejecuta:   gradlew runClient   (no hace falta recompilar la DLL: build.bat no cambia nada)
Ahora el vertice de lineas se lee byte a byte (R8_UINT/R8_SNORM) y el shader reconstruye LineWidth y la normal.
Esperado en el log: "layout de lineas partido en bytes sueltos (9 atributos)" y "pipeline ... lines -> listo".
Si sigue fallando, pasame esas lineas del log (incluyen layoutReal y las variantes).
