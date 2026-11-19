# Paso 6c: diagnostico del mundo negro + arreglos (SIN PROBAR: no pude compilar ni ejecutar aqui)

Copia estos archivos ENCIMA de tu proyecto, luego `cd native && build.bat`, `cd ..`, `gradlew runClient`.

Cambios
- Cielo: TRIANGLE_FAN ahora se convierte a lista de triangulos (el disco del cielo y el amanecer estaban rotos).
- animate_sprite_blit (y posiblemente otros): el VS ya no exporta varyings que el fragment no declara (fallaba el PSO por empaquetado).
- copyTextureToTexture ya copia de verdad (antes era un no-op).
- Si un PSO falla, el log anade "variantes: sinBlend/sinLayout/sinCull=OK|FALLO" para ver que parte lo invalida.
- Trazas "[DX12 trace]": primer draw de cada pipeline (uniformes y texturas enlazados o FALTA), volcado de UBOs de terreno
  y contenido (pixeles no nulos) de los atlas. Al cerrar, resumen por pipeline (draws, texturas/uniformes ausentes).

Banderas (archivo vacio en la carpeta run/, p.ej. run/dx12mod.nocull) para probar sin recompilar:
  nocull, flipcull, nodepth, nomip, notrace.
Prueba cada una por separado entrando a un mundo; di cual hace aparecer el terreno/botones.

Pasame: el log completo, y run/dx12mod/shaders/minecraft_pipeline_lines.*.hlsl si lines sigue fallando.
Pendiente: clouds (texel buffers), lines.

## Paso 6d
- Causa del terreno negro: el atlas de bloques tenia el mip 0 con contenido y los mips 1-4 vacios. El juego los genera dibujando
  (animate_sprite_blit/interpolate) en niveles de mip del atlas y el mod ignoraba el nivel de mip del destino. Ahora el RTV,
  el viewport y los clears respetan el mip del GpuTextureView.

## Paso 6e: rendimiento (SIN PROBAR)
- Buffers residentes en GPU: un buffer de vertices/indices que se usa en 2 frames distintos sin cambiar se sube UNA vez a
  memoria de video (heap DEFAULT) y se dibuja desde ahi, sin copiarlo al anillo en cada draw ni escanear sus indices.
  Buffers que cambian cada frame (GUI, entidades) siguen por el anillo como antes.
- Cada escritura/map/copia de un buffer incrementa su version; la DLL re-sube si la version cambia.
- Informe "[DX12 perf]" cada 300 frames: draws/frame, draws con buffer residente, ms de CPU en draw, ms esperando a la GPU,
  MB copiados al anillo por frame, submits por frame, VRAM residente.
- Compara el informe antes/despues: pasame las lineas "[DX12 perf]" y las FPS que ves en F3.

## Paso 6f: tirones al cargar chunks (SIN PROBAR)
- Los buffers residentes en GPU se reciclan: al cerrarse un buffer su recurso va a un pool por clases de tamano y se reutiliza,
  en vez de crear un CreateCommittedResource nuevo por cada chunk (caro y causa de los tirones).
- El informe [DX12 perf] ahora incluye: creados/reciclados, tiempo en buffers residentes (ms/frame), PEOR frame (ms)
  y cuantos frames tardaron mas de 10 ms.
