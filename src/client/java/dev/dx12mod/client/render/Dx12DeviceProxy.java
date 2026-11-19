package dev.dx12mod.client.render;

import com.mojang.blaze3d.buffers.GpuBuffer;
import com.mojang.blaze3d.pipeline.CompiledRenderPipeline;
import com.mojang.blaze3d.pipeline.RenderPipeline;
import com.mojang.blaze3d.shaders.ShaderSource;
import com.mojang.blaze3d.systems.CommandEncoder;
import com.mojang.blaze3d.systems.GpuDevice;
import com.mojang.blaze3d.textures.AddressMode;
import com.mojang.blaze3d.textures.FilterMode;
import com.mojang.blaze3d.textures.GpuSampler;
import com.mojang.blaze3d.textures.GpuTexture;
import com.mojang.blaze3d.textures.GpuTextureView;
import com.mojang.blaze3d.textures.TextureFormat;
import dev.dx12mod.Dx12Mod;
import org.jspecify.annotations.Nullable;

import java.nio.ByteBuffer;
import java.util.List;
import java.util.OptionalDouble;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Supplier;

/**
 * Paso 3: GpuDevice que reenvia TODO al dispositivo OpenGL real y cuenta/loguea las llamadas.
 * Sirve para (1) probar que el mixin engancha bien y (2) saber que usa Minecraft de verdad.
 * En pasos siguientes iremos reemplazando metodos por implementaciones DX12.
 */
public final class Dx12DeviceProxy implements GpuDevice {
	private final GpuDevice real;

	private final AtomicInteger textures = new AtomicInteger();
	private final AtomicInteger textureViews = new AtomicInteger();
	private final AtomicInteger buffers = new AtomicInteger();
	private final AtomicInteger samplers = new AtomicInteger();
	private final AtomicInteger pipelines = new AtomicInteger();

	public Dx12DeviceProxy(GpuDevice real) {
		this.real = real;
		Dx12Mod.LOGGER.info("[DX12] GpuDevice interceptado. Backend real: {} | {} | {}",
			real.getBackendName(), real.getRenderer(), real.getVersion());
	}

	private static boolean shouldLog(int n) {
		return n <= 5 || (n & (n - 1)) == 0; // primeras 5 y luego potencias de 2
	}

	@Override
	public CommandEncoder createCommandEncoder() {
		return real.createCommandEncoder();
	}

	@Override
	public GpuSampler createSampler(AddressMode u, AddressMode v, FilterMode min, FilterMode mag, int aniso, OptionalDouble lod) {
		int n = samplers.incrementAndGet();
		if (shouldLog(n)) Dx12Mod.LOGGER.info("[DX12] createSampler #{} {}/{} {}/{} aniso={}", n, u, v, min, mag, aniso);
		return real.createSampler(u, v, min, mag, aniso, lod);
	}

	@Override
	public GpuTexture createTexture(@Nullable Supplier<String> label, @GpuTexture.Usage int usage, TextureFormat format, int w, int h, int layers, int mips) {
		int n = textures.incrementAndGet();
		if (shouldLog(n)) Dx12Mod.LOGGER.info("[DX12] createTexture #{} {}x{} {} usage={} layers={} mips={}", n, w, h, format, usage, layers, mips);
		return real.createTexture(label, usage, format, w, h, layers, mips);
	}

	@Override
	public GpuTexture createTexture(@Nullable String label, @GpuTexture.Usage int usage, TextureFormat format, int w, int h, int layers, int mips) {
		int n = textures.incrementAndGet();
		if (shouldLog(n)) Dx12Mod.LOGGER.info("[DX12] createTexture #{} '{}' {}x{} {} usage={} layers={} mips={}", n, label, w, h, format, usage, layers, mips);
		return real.createTexture(label, usage, format, w, h, layers, mips);
	}

	@Override
	public GpuTextureView createTextureView(GpuTexture texture) {
		textureViews.incrementAndGet();
		return real.createTextureView(texture);
	}

	@Override
	public GpuTextureView createTextureView(GpuTexture texture, int baseMip, int mipCount) {
		textureViews.incrementAndGet();
		return real.createTextureView(texture, baseMip, mipCount);
	}

	@Override
	public GpuBuffer createBuffer(@Nullable Supplier<String> label, @GpuBuffer.Usage int usage, long size) {
		int n = buffers.incrementAndGet();
		if (shouldLog(n)) Dx12Mod.LOGGER.info("[DX12] createBuffer #{} size={} usage={}", n, size, usage);
		return real.createBuffer(label, usage, size);
	}

	@Override
	public GpuBuffer createBuffer(@Nullable Supplier<String> label, @GpuBuffer.Usage int usage, ByteBuffer data) {
		int n = buffers.incrementAndGet();
		if (shouldLog(n)) Dx12Mod.LOGGER.info("[DX12] createBuffer #{} initialData={} bytes usage={}", n, data.remaining(), usage);
		return real.createBuffer(label, usage, data);
	}

	@Override
	public CompiledRenderPipeline precompilePipeline(RenderPipeline pipeline, @Nullable ShaderSource shaderSource) {
		int n = pipelines.incrementAndGet();
		if (shouldLog(n)) Dx12Mod.LOGGER.info("[DX12] precompilePipeline #{} {}", n, pipeline);
		return real.precompilePipeline(pipeline, shaderSource);
	}

	@Override
	public void close() {
		Dx12Mod.LOGGER.info("[DX12] Resumen: texturas={} vistas={} buffers={} samplers={} pipelines={}",
			textures.get(), textureViews.get(), buffers.get(), samplers.get(), pipelines.get());
		real.close();
	}

	// --- reenvio directo ---
	@Override public String getImplementationInformation() { return real.getImplementationInformation(); }
	@Override public List<String> getLastDebugMessages() { return real.getLastDebugMessages(); }
	@Override public boolean isDebuggingEnabled() { return real.isDebuggingEnabled(); }
	@Override public String getVendor() { return real.getVendor(); }
	@Override public String getBackendName() { return real.getBackendName(); }
	@Override public String getVersion() { return real.getVersion(); }
	@Override public String getRenderer() { return real.getRenderer(); }
	@Override public int getMaxTextureSize() { return real.getMaxTextureSize(); }
	@Override public int getUniformOffsetAlignment() { return real.getUniformOffsetAlignment(); }
	@Override public void clearPipelineCache() { real.clearPipelineCache(); }
	@Override public List<String> getEnabledExtensions() { return real.getEnabledExtensions(); }
	@Override public int getMaxSupportedAnisotropy() { return real.getMaxSupportedAnisotropy(); }
}
