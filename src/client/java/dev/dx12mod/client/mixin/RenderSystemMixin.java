package dev.dx12mod.client.mixin;

import com.mojang.blaze3d.shaders.ShaderSource;
import com.mojang.blaze3d.systems.GpuDevice;
import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.blaze3d.systems.SamplerCache;
import dev.dx12mod.client.Dx12Mode;
import dev.dx12mod.client.Dx12Native;
import dev.dx12mod.client.render.Dx12Device;
import dev.dx12mod.client.render.Dx12DeviceProxy;
import net.minecraft.client.renderer.DynamicUniforms;
import org.lwjgl.glfw.GLFW;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.Redirect;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(RenderSystem.class)
public abstract class RenderSystemMixin {
	@Shadow
	private static GpuDevice DEVICE;
	@Shadow
	private static String apiDescription;
	@Shadow
	private static DynamicUniforms dynamicUniforms;
	@Shadow
	private static SamplerCache samplerCache;

	/**
	 * Modo DX12: no se crea el GlDevice (necesita un contexto OpenGL que ya no existe).
	 * Se crea nuestro Dx12Device y se repite el resto de initRenderer.
	 */
	@Inject(method = "initRenderer", at = @At("HEAD"), cancellable = true)
	private static void dx12mod$initDx12(long window, int glDebug, boolean sync, ShaderSource shaderSource,
			boolean debugLabels, CallbackInfo ci) {
		if (!Dx12Mode.enabled()) {
			return;
		}
		DEVICE = new Dx12Device(window, shaderSource);
		apiDescription = DEVICE.getImplementationInformation();
		dynamicUniforms = new DynamicUniforms();
		samplerCache.initialize();
		ci.cancel();
	}

	/** Modo OpenGL (valvula de escape): se mantiene el proxy del paso 3. */
	@Inject(method = "initRenderer", at = @At("RETURN"))
	private static void dx12mod$wrapGl(CallbackInfo ci) {
		if (!(DEVICE instanceof Dx12Device) && !(DEVICE instanceof Dx12DeviceProxy)) {
			DEVICE = new Dx12DeviceProxy(DEVICE);
		}
	}

	/** En vez de glfwSwapBuffers (que exige contexto OpenGL), presentamos con el swapchain DX12.
	 * Si el juego llamo a presentTexture este frame se copia esa textura; si no, se pinta el color pulsante de diagnostico. */
	@Redirect(method = "flipFrame", at = @At(value = "INVOKE", target = "Lorg/lwjgl/glfw/GLFW;glfwSwapBuffers(J)V"))
	private static void dx12mod$swap(long window) {
		if (Dx12Mode.enabled()) {
			float t = System.nanoTime() / 1_000_000_000f;
			float p = 0.5f + 0.5f * (float) Math.sin(t * 1.5f);
			Dx12Native.flip(0.05f, 0.15f + 0.25f * p, 0.35f + 0.2f * p);
			Dx12Native.endFrame();
		} else {
			GLFW.glfwSwapBuffers(window);
		}
	}
}
