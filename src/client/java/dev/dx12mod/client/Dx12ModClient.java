package dev.dx12mod.client;

import dev.dx12mod.Dx12Mod;
import net.fabricmc.api.ClientModInitializer;

public class Dx12ModClient implements ClientModInitializer {
	@Override
	public void onInitializeClient() {
		if (!System.getProperty("os.name", "").toLowerCase().contains("win")) {
			Dx12Mod.LOGGER.warn("DirectX 12 solo existe en Windows; el mod no hace nada.");
			return;
		}
		if (Dx12Native.load()) {
			Dx12Mod.LOGGER.info("[DX12 probe]\n{}", Dx12Native.probe());
		}
	}
}
