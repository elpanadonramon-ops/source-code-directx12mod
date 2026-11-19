package dev.dx12mod.client;

import dev.dx12mod.Dx12Mod;
import net.fabricmc.loader.api.FabricLoader;

import java.nio.file.Files;
import java.nio.file.Path;

/**
 * Decide si el mod reemplaza OpenGL por DX12 en esta ejecucion.
 * Valvula de escape: si existe el archivo "dx12mod.disabled" en la carpeta del juego (run/), se usa OpenGL normal.
 */
public final class Dx12Mode {
	private static Boolean enabled;

	private Dx12Mode() {}

	/**
	 * Banderas de depuracion: un archivo vacio "dx12mod.<nombre>" en la carpeta del juego (run/) las activa.
	 * nocull = sin culling | flipcull = culling de la cara contraria | nodepth = sin prueba de profundidad |
	 * nomip = texturas solo con el mip 0 | notrace = sin trazas de primer dibujo.
	 */
	public static boolean flag(String name) {
		try {
			return Files.exists(FabricLoader.getInstance().getGameDir().resolve("dx12mod." + name));
		} catch (Throwable t) {
			return false;
		}
	}

	public static synchronized boolean enabled() {
		if (enabled == null) enabled = compute();
		return enabled;
	}

	private static boolean compute() {
		if (!System.getProperty("os.name", "").toLowerCase().contains("win")) {
			return false;
		}
		Path flag = FabricLoader.getInstance().getGameDir().resolve("dx12mod.disabled");
		if (Files.exists(flag)) {
			Dx12Mod.LOGGER.info("[DX12] Modo DX12 desactivado por el archivo {}", flag);
			return false;
		}
		if (!Dx12Native.load()) {
			Dx12Mod.LOGGER.warn("[DX12] No se pudo cargar dx12native.dll; se usa OpenGL");
			return false;
		}
		// Respaldo: si ningun adaptador de hardware admite DirectX 12 (por ejemplo, graficos Intel HD de 3.a generacion o Atom
		// Bay Trail), se usa OpenGL en vez de dejar el juego sin ventana. Cada adaptador valido aparece con "| FL x_y".
		String probe;
		try {
			probe = Dx12Native.probe();
		} catch (Throwable t) {
			probe = "";
		}
		if (probe == null || !probe.contains("| FL ")) {
			Dx12Mod.LOGGER.warn("[DX12] Ninguna GPU admite DirectX 12 (se necesita Windows 10 y un driver con DX12); se usa OpenGL. Sondeo:\n{}", probe);
			return false;
		}
		Dx12Mod.LOGGER.info("[DX12] Modo DX12 ACTIVADO");
		return true;
	}
}
