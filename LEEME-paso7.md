# Paso 7: rendimiento (SIN COMPILAR NI PROBAR: no tengo Windows/D3D12/Minecraft aqui)

Copia estos 5 archivos ENCIMA de tu proyecto y vuelve a compilar la DLL:

    native\src\dx12_core.cpp
    src\client\java\dev\dx12mod\client\Dx12Native.java
    src\client\java\dev\dx12mod\client\render\Dx12RenderPass.java
    src\client\java\dev\dx12mod\client\render\Dx12Device.java
    src\client\java\dev\dx12mod\client\mixin\WindowMixin.java

    cd native && build.bat && cd .. && gradlew runClient

Antes de probar: en Minecraft > Opciones > Video, pon "Velocidad de fotogramas" en ilimitado y VSync desactivado.
Ahora el mod RESPETA la opcion de VSync (antes ignoraba la de Minecraft y hacia siempre Present(1)).
Sin VSync usa tearing de DXGI (necesita Windows 10+ y ventana normal o sin bordes).

## Que cambia
1. Frames en vuelo (2): la CPU graba el frame N+1 mientras la GPU dibuja el N. Antes, tras cada Present, se esperaba a que la GPU
   terminara TODO (CPU y GPU en serie: tiempo de frame = CPU + GPU; ahora ~ max(CPU, GPU)). Cada frame en vuelo tiene su propio
   allocator, anillo de subida (48 MB), heap de SRV y anillos de RTV/DSV.
2. Cache de estado: ya no se repiten PSO, firma raiz, viewport, scissor, topologia, vertex/index buffer, blend factor ni los
   8 CBV si no cambian. Antes cada draw hacia ~35 llamadas D3D12; ahora normalmente 4-6 en el terreno.
3. Tablas de descriptores reutilizadas: las 8 SRV (antes: 8 CreateShaderResourceView por draw) y los 8 samplers (antes: 8 tablas
   raiz por draw) se crean una vez por combinacion de texturas y se reutilizan. Firma raiz: 8 CBV + 1 tabla SRV + 1 tabla de samplers.
4. RTV/DSV: solo se crean y fijan cuando cambia el destino (antes: uno por draw).
5. Uniformes deduplicados: si el contenido de un UBO es igual al del draw anterior, no se copia otra vez al anillo.
6. Parametros del draw por memoria nativa compartida (drawPacked): sin 3 copias de arrays JNI por draw. En Java los uniformes y
   texturas solo se vuelven a resolver si cambiaron (marcas dirty), sin buscar por nombre en cada draw.
7. Trazas por draw DESACTIVADAS por defecto (antes: un HashMap + cadenas en cada draw). Para volver a verlas: archivo vacio run\dx12mod.trace
8. Shaders compilados con D3DCOMPILE_OPTIMIZATION_LEVEL3 (el arranque tarda algo mas; la GPU hace menos trabajo).
9. Swapchain de 3 buffers con espera de latencia (maximo 2 frames en cola) y VSync/tearing segun la opcion de Minecraft.

## Banderas (archivo vacio en run\)
    dx12mod.novsync   fuerza sin VSync aunque Minecraft lo tenga activado
    dx12mod.serial    1 frame en vuelo (comportamiento de antes; sirve para comprobar si los frames en vuelo dan algun problema)
    dx12mod.triple    3 frames en vuelo
    dx12mod.trace     trazas de diagnostico (lento)
    (siguen: nocull, flipcull, nodepth, nomip)

## Como leer la linea [DX12 perf] (cada 300 frames)
    NNN FPS (X ms/frame) = DLL a + espera GPU/latencia b + Present c + Minecraft/Java d ms
- DLL: tiempo de CPU dentro de la DLL (draws + fin de frame).
- espera GPU/latencia: la CPU esperando a la GPU. Si es grande, el cuello es la GPU (resolucion, sombreado).
- Present: tiempo dentro de Present (con VSync activado incluye esperar al refresco).
- Minecraft/Java: todo lo demas (logica del juego, construccion de chunks, Java del mod).
Pasame varias lineas [DX12 perf] (en un mundo y quieto) y se donde seguir.

## Limites honestos
No puedo garantizar 350-400 FPS: depende de tu CPU, de una GTX 1060 3GB y de la resolucion/distancia de render. Con la espera a la GPU
eliminada y la CPU de draw mucho mas barata, lo que quede sera sobre todo Minecraft/Java o la GPU.
Si el juego no arranca o se ve raro: prueba primero con dx12mod.serial; si sigue igual, copiame el log completo.
