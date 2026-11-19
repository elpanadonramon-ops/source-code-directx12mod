# DX12 Mod - Paso 5: texturas reales en la GPU

Copia estos archivos ENCIMA de tu proyecto (sobrescribe los del paso 4):
- `native/src/dx12_core.cpp`
- `src/client/java/dev/dx12mod/client/Dx12Native.java`
- `src/client/java/dev/dx12mod/client/mixin/RenderSystemMixin.java`
- `src/client/java/dev/dx12mod/client/render/Dx12CommandEncoder.java`
- `src/client/java/dev/dx12mod/client/render/Dx12Device.java`
- `src/client/java/dev/dx12mod/client/render/Dx12Resources.java`

Pasos:
1. `cd native && build.bat`
2. `gradlew runClient`
3. Al cerrar el juego, busca en el log `[DX12] presentTexture recibido` y `[DX12] Resumen`.

Que deberia verse: ya no el azul pulsante, sino lo que Minecraft limpia en su render target
(negro en los menus, color de niebla/cielo dentro de un mundo). Todavia no se dibuja geometria ni GUI (paso 6: shaders).
Si el log dice "frames sin textura" muy alto, el juego no esta llamando a presentTexture y hay que presentar de otra forma.
