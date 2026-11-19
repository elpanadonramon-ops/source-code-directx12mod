# Arreglo de lineas (SIN PROBAR): hitboxes (F3+B) y contorno de bloques

Extrae encima del proyecto (sobrescribe native\src\dx12_core.cpp y
src\client\java\dev\dx12mod\client\render\Dx12Device.java; incluye pasos 8, 9 y el diagnostico), luego:
    cd native && build.bat && cd .. && gradlew runClient

Causa: en 1.21.11 el vertice de lineas mide 23 bytes y LineWidth (float) queda en el byte 19; D3D12 no puede leer un
float desalineado. Ahora ese atributo se lee como 4 bytes y el vertex shader lo reconstruye con asfloat().
En el log deben aparecer: "esta en el byte 19 (desalineado); se lee como 4 bytes" y "pipeline ... lines -> listo".
Prueba: mira un bloque (contorno) y F3+B (hitboxes).
