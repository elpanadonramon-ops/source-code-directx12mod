# Paso 8 (SIN PROBAR): buffers residentes desde heaps grandes

Extrae este zip ENCIMA de tu proyecto (sobrescribe native\src\dx12_core.cpp), luego:

    cd native && build.bat && cd .. && gradlew runClient

Cambio: los buffers residentes de GPU se cortan de paginas de 64 MB (CreatePlacedResource) en vez de crear uno
committed por cada chunk. Si falla o se pasa de 768 MB de paginas, vuelve al metodo de antes.
El informe [DX12 perf] anade al final: colocados, crear ms/frame, buffersRes ms/frame, heaps MB.

Prueba: entra a un mundo y vuela rapido por terreno nuevo; pasame las lineas [DX12 perf].
