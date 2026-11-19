# Diagnostico de lineas (SIN PROBAR): por que fallan lines / secondary_block_outline

Extrae el zip ENCIMA del proyecto (sobrescribe native\src\dx12_core.cpp; incluye pasos 8 y 9), luego:
    cd native && build.bat && cd .. && gradlew runClient
Abre un mundo y cierra. Pasame las lineas "[DX12] pipeline ... lines -> D3D12 FALLO" del log (ahora son mas largas).
