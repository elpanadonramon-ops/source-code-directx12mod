package dev.dx12mod.client.mixin;

import com.mojang.blaze3d.platform.Window;
import dev.dx12mod.client.Dx12Mode;
import dev.dx12mod.client.Dx12Native;
import org.lwjgl.glfw.GLFW;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Redirect;

@Mixin(Window.class)
public abstract class WindowMixin {
	/** Pide a GLFW una ventana SIN contexto OpenGL (las demas pistas de OpenGL se ignoran). */
	@Redirect(method = "<init>", at = @At(value = "INVOKE", target = "Lorg/lwjgl/glfw/GLFW;glfwWindowHint(II)V"))
	private static void dx12mod$windowHint(int hint, int value) {
		if (hint == GLFW.GLFW_CLIENT_API && Dx12Mode.enabled()) {
			value = GLFW.GLFW_NO_API;
		}
		GLFW.glfwWindowHint(hint, value);
	}

	/** glfwSwapInterval necesita un contexto OpenGL; en modo DX12 se pasa la opcion de VSync de Minecraft al swapchain. */
	@Redirect(method = "updateVsync", at = @At(value = "INVOKE", target = "Lorg/lwjgl/glfw/GLFW;glfwSwapInterval(I)V"))
	private static void dx12mod$swapInterval(int interval) {
		if (Dx12Mode.enabled()) {
			Dx12Native.setVsync(Dx12Mode.flag("novsync") ? 0 : interval);
		} else {
			GLFW.glfwSwapInterval(interval);
		}
	}
}
