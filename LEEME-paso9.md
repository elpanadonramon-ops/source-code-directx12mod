# Paso 9 (SIN PROBAR): cache de shaders en disco (arranque mucho mas rapido desde la 2a vez)

Extrae este zip ENCIMA de tu proyecto (sobrescribe native\src\dx12_core.cpp), luego:

    cd native && build.bat && cd .. && gradlew runClient

Incluye lo del paso 8 (buffers desde heaps grandes).

Cambio: el bytecode de cada shader compilado se guarda en run\dx12mod_cache\ y se reutiliza.
- 1a ejecucion: igual de lenta que antes (compila y guarda).
- 2a ejecucion en adelante: lee de disco; no vuelve a compilar.
- Para forzar recompilar: borra la carpeta run\dx12mod_cache.
El informe [DX12 perf] anade al final: shaders de cache / compilados / ms compilando.

Prueba: abre el juego DOS veces y compara el tiempo hasta el menu (en el log, de "Reloading ResourceManager" a "pipeline #90").
