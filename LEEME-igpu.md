# Paso 11 (SIN PROBAR): compatibilidad con GPUs integradas (Celeron / Atom)

Extrae encima del proyecto (sobrescribe dx12_core.cpp, Dx12Mode.java y Dx12Device.java; incluye todo lo anterior, tambien
el arreglo de lineas fix2) y ejecuta:
    cd native && build.bat && cd .. && gradlew runClient     (o gradlew build para el jar)

1) Perfil de bajo consumo automatico si la GPU es integrada (UMA) o tiene menos de 1.5 GB de VRAM:
   anillo de subida 16 MB (RAM <= 4 GB) o 24 MB (en vez de 48 MB por frame), pool 48 MB (en vez de 192), paginas de heap de
   16 MB con tope de 192/320 MB (en vez de 64 MB y 768 MB).  Variable de entorno DX12MOD_LOWMEM=1 lo fuerza, =0 lo desactiva.
   El informe [DX12 perf] muestra "perfil=normal|bajo consumo anillo=N MB".
2) Si ninguna GPU admite DirectX 12 se usa OpenGL en vez de fallar (log: "Ninguna GPU admite DirectX 12").
3) En el log aparece cada 10 pipelines "arranque acumulado: traduccion ... ms, D3D12 ... ms" para ver donde se va el arranque.
