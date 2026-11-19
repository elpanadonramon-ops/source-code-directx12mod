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
import dev.dx12mod.client.Dx12Mode;
import dev.dx12mod.client.Dx12Native;
import dev.dx12mod.client.shader.ShaderTranslator;
import org.jspecify.annotations.Nullable;
import org.lwjgl.glfw.GLFWNativeWin32;

import com.mojang.blaze3d.pipeline.BlendFunction;
import com.mojang.blaze3d.platform.DestFactor;
import com.mojang.blaze3d.platform.SourceFactor;
import com.mojang.blaze3d.vertex.VertexFormat;
import com.mojang.blaze3d.vertex.VertexFormatElement;

import java.nio.ByteBuffer;
import java.util.HashMap;
import java.util.Map;
import java.util.List;
import java.util.OptionalDouble;
import java.util.function.Supplier;

/**
 * Paso 5: GpuDevice propio con texturas reales. Crea el swapchain DX12 sobre la ventana de Minecraft;
 * recursos y comandos son stubs (ver Dx12Resources). No toca OpenGL en absoluto.
 */
public final class Dx12Device implements GpuDevice {
	private final CommandEncoder encoder = new Dx12CommandEncoder();
	private final String adapter;
	private int pipelines;
	private int shadersOk;
	private int shadersFailed;
	private int nativeOk;
	private int nativeFailed;
	private boolean translatorBroken;
	private final ShaderSource defaultShaderSource;
	private final Map<RenderPipeline, Dx12Resources.Pipeline> compiled = new HashMap<>();
	static Dx12Device instance;
	private static final boolean NO_CULL = Dx12Mode.flag("nocull");
	private static final boolean FLIP_CULL = Dx12Mode.flag("flipcull");
	private static final boolean NO_DEPTH = Dx12Mode.flag("nodepth");

	public Dx12Device(long glfwWindow, ShaderSource defaultShaderSource) {
		this.defaultShaderSource = defaultShaderSource;
		instance = this;
		long hwnd = GLFWNativeWin32.glfwGetWin32Window(glfwWindow);
		if (hwnd == 0L) {
			throw new IllegalStateException("No se pudo obtener el HWND de la ventana");
		}
		Dx12Native.setInflight(Dx12Mode.flag("serial") ? 1 : (Dx12Mode.flag("triple") ? 3 : 2));
		if (!Dx12Native.init(hwnd)) {
			throw new IllegalStateException("Fallo iniciando DX12: " + Dx12Native.lastError());
		}
		this.adapter = Dx12Native.adapterName();
		Dx12Mod.LOGGER.info("[DX12] Dispositivo DirectX 12 creado en: {}", adapter);
		StringBuilder flags = new StringBuilder();
		for (String f : new String[] {"nocull", "flipcull", "nodepth", "nomip", "trace", "novsync", "serial", "triple"}) {
			if (Dx12Mode.flag(f)) flags.append(f).append(' ');
		}
		if (flags.length() > 0) Dx12Mod.LOGGER.info("[DX12] Banderas de depuracion activas: {}", flags.toString().trim());
	}

	@Override public CommandEncoder createCommandEncoder() { return encoder; }

	@Override
	public GpuSampler createSampler(AddressMode u, AddressMode v, FilterMode min, FilterMode mag, int aniso, OptionalDouble lod) {
		return new Dx12Resources.Sampler(u, v, min, mag, aniso, lod);
	}

	@Override
	public GpuTexture createTexture(@Nullable Supplier<String> label, int usage, TextureFormat format, int w, int h, int layers, int mips) {
		return new Dx12Resources.Texture(usage, label == null ? null : label.get(), format, w, h, layers, mips);
	}

	@Override
	public GpuTexture createTexture(@Nullable String label, int usage, TextureFormat format, int w, int h, int layers, int mips) {
		return new Dx12Resources.Texture(usage, label, format, w, h, layers, mips);
	}

	@Override public GpuTextureView createTextureView(GpuTexture texture) {
		return new Dx12Resources.TextureView(texture, 0, texture.getMipLevels());
	}

	@Override public GpuTextureView createTextureView(GpuTexture texture, int baseMip, int mips) {
		return new Dx12Resources.TextureView(texture, baseMip, mips);
	}

	@Override
	public GpuBuffer createBuffer(@Nullable Supplier<String> label, int usage, long size) {
		return new Dx12Resources.Buffer(usage, size);
	}

	@Override
	public GpuBuffer createBuffer(@Nullable Supplier<String> label, int usage, ByteBuffer data) {
		Dx12Resources.Buffer buffer = new Dx12Resources.Buffer(usage, data.remaining());
		buffer.data.put(data.duplicate());
		return buffer;
	}

	@Override
	public CompiledRenderPipeline precompilePipeline(RenderPipeline pipeline, @Nullable ShaderSource shaderSource) {
		Dx12Resources.Pipeline existing = compiled.get(pipeline);
		if (existing != null) return existing;
		pipelines++;
		ShaderSource source = shaderSource != null ? shaderSource : defaultShaderSource;
		Dx12Resources.Pipeline result = new Dx12Resources.Pipeline(0L, pipeline.getLocation().toString(), Map.of(), Map.of(), pipeline.getVertexFormatMode());
		if (translatorBroken) {
			Dx12Mod.LOGGER.info("[DX12] pipeline #{}: {} (sin traducir)", pipelines, pipeline);
		} else {
			try {
				long t0 = System.nanoTime();
				ShaderTranslator.Result r = ShaderTranslator.translate(pipeline, source);
				translateNs += System.nanoTime() - t0;
				shadersOk++;
				String name = pipeline.getLocation().toString();
				LayoutFix fx = fixUnalignedFloats(pipeline, vertexLayout(pipeline), r.vertexHlsl());
				long t1 = System.nanoTime();
				long handle = Dx12Native.createPipeline(name, fx.hlsl(), r.fragmentHlsl(), fx.lay(), pipelineState(pipeline));
				nativeNs += System.nanoTime() - t1;
				if (handle == 0L) {
					nativeFailed++;
					Dx12Mod.LOGGER.warn("[DX12] pipeline #{}: {} -> D3D12 FALLO: {}", pipelines, name, Dx12Native.lastError());
				} else {
					nativeOk++;
					result = new Dx12Resources.Pipeline(handle, name, r.uboBindings(), r.samplerBindings(), pipeline.getVertexFormatMode());
					Dx12Mod.LOGGER.info("[DX12] pipeline #{}: {} -> listo", pipelines, name);
					if (pipelines % 10 == 0) {
						Dx12Mod.LOGGER.info("[DX12] arranque acumulado: traduccion GLSL->HLSL {} ms, compilacion/creacion D3D12 {} ms", translateNs / 1_000_000L, nativeNs / 1_000_000L);
					}
				}
			} catch (ShaderTranslator.TranslateException e) {
				shadersFailed++;
				Dx12Mod.LOGGER.warn("[DX12] pipeline #{}: {} -> FALLO: {}", pipelines, pipeline, e.getMessage());
			} catch (Throwable t) {
				// Dependencias ausentes, DLL de shaderc/spvc que no carga, API distinta...: no se rompe el juego.
				translatorBroken = true;
				Dx12Mod.LOGGER.error("[DX12] El traductor de shaders no esta disponible; se desactiva", t);
			}
		}
		compiled.put(pipeline, result);
		return result;
	}

	Dx12Resources.Pipeline pipelineFor(RenderPipeline pipeline) {
		return (Dx12Resources.Pipeline) precompilePipeline(pipeline, null);
	}

	private static long translateNs, nativeNs;  // tiempos acumulados de arranque (diagnostico)

	private record LayoutFix(String hlsl, int[] lay) {}

	/**
	 * El formato de lineas de Minecraft 1.21.11 mide 23 bytes y deja LineWidth (float) en el byte 19, desalineado, justo
	 * pegado a la normal (3 bytes en 16..18). D3D12 rechaza ese layout (lines, lines_translucent y secondary_block_outline
	 * fallaban: hitboxes y contorno de bloques). Solucion: leer esos atributos byte a byte (R8_UINT / R8_SNORM, sin
	 * restricciones de alineacion ni solapes) y reconstruirlos en el vertex shader: el float con asfloat() y la normal
	 * con float3(). Los bytes sueltos usan slots (TEXCOORDn) nuevos al final; los originales no se renumeran.
	 */
	private static LayoutFix fixUnalignedFloats(RenderPipeline pipeline, int[] lay, String vsHlsl) {
		VertexFormat f = pipeline.getVertexFormat();
		java.util.List<VertexFormatElement> els = f.getElements();
		java.util.List<String> names = f.getElementAttributeNames();
		int k = -1;
		for (int i = 0; i < els.size() && i < names.size(); i++) {
			VertexFormatElement e = els.get(i);
			if (e.type().ordinal() == 0 && e.count() == 1 && f.getOffset(e) % 4 != 0) { k = i; break; }
		}
		if (k < 0) return new LayoutFix(vsHlsl, lay);

		java.util.List<int[]> entries = new java.util.ArrayList<>();  // {tipo, count, normalizado, offset}
		for (int i = 0; i < els.size(); i++) {
			entries.add(new int[] { lay[2 + i * 4], lay[3 + i * 4], lay[4 + i * 4], lay[5 + i * 4] });
		}
		String out = vsHlsl;

		// 1) LineWidth: 4 bytes sueltos -> float
		String wn = names.get(k);
		int wOff = f.getOffset(els.get(k));
		String wDecl = "\\bfloat\\s+" + wn + "\\s*:\\s*TEXCOORD" + k + "\\s*;";
		String wAsg = "\\b" + wn + "\\s*=\\s*stage_input\\." + wn + "\\s*;";
		if (!java.util.regex.Pattern.compile(wDecl).matcher(out).find() || !java.util.regex.Pattern.compile(wAsg).matcher(out).find()) {
			Dx12Mod.LOGGER.warn("[DX12] {}: '{}' desalineado pero no se pudo reescribir el HLSL", pipeline.getLocation(), wn);
			return new LayoutFix(vsHlsl, lay);
		}
		int next = els.size();
		int[] ws = { k, next, next + 1, next + 2 };
		next += 3;
		entries.set(k, new int[] { 1, 1, 0, wOff });
		for (int b = 1; b < 4; b++) entries.add(new int[] { 1, 1, 0, wOff + b });
		StringBuilder decl = new StringBuilder();
		for (int b = 0; b < 4; b++) decl.append("uint ").append(wn).append("_b").append(b).append(" : TEXCOORD").append(ws[b]).append("; ");
		String s = "stage_input." + wn;
		String asg = wn + " = asfloat(" + s + "_b0 | (" + s + "_b1 << 8) | (" + s + "_b2 << 16) | (" + s + "_b3 << 24));";
		out = out.replaceAll(wDecl, java.util.regex.Matcher.quoteReplacement(decl.toString().trim()));
		out = out.replaceAll(wAsg, java.util.regex.Matcher.quoteReplacement(asg));

		// 2) Normal (3 bytes con signo justo antes): sus 4 bytes de lectura se solaparian con LineWidth -> 3 bytes sueltos
		if (k > 0) {
			VertexFormatElement ne = els.get(k - 1);
			if (ne.type().ordinal() == 2 && ne.count() == 3) {
				String nn = names.get(k - 1);
				int nj = k - 1;
				int nOff = f.getOffset(ne);
				String nDecl = "\\bfloat3\\s+" + nn + "\\s*:\\s*TEXCOORD" + nj + "\\s*;";
				String nAsg = "\\b" + nn + "\\s*=\\s*stage_input\\." + nn + "\\s*;";
				if (java.util.regex.Pattern.compile(nDecl).matcher(out).find() && java.util.regex.Pattern.compile(nAsg).matcher(out).find()) {
					int[] ns = { nj, next, next + 1 };
					next += 2;
					entries.set(nj, new int[] { 2, 1, 1, nOff });
					entries.add(new int[] { 2, 1, 1, nOff + 1 });
					entries.add(new int[] { 2, 1, 1, nOff + 2 });
					String nd = "float " + nn + "_x : TEXCOORD" + ns[0] + "; float " + nn + "_y : TEXCOORD" + ns[1] + "; float " + nn + "_z : TEXCOORD" + ns[2] + ";";
					String ns2 = "stage_input." + nn;
					String na = nn + " = float3(" + ns2 + "_x, " + ns2 + "_y, " + ns2 + "_z);";
					out = out.replaceAll(nDecl, java.util.regex.Matcher.quoteReplacement(nd));
					out = out.replaceAll(nAsg, java.util.regex.Matcher.quoteReplacement(na));
				} else {
					Dx12Mod.LOGGER.warn("[DX12] {}: no se pudo reescribir la normal '{}'; se deja como estaba", pipeline.getLocation(), nn);
				}
			}
		}

		int[] fixed = new int[2 + entries.size() * 4];
		fixed[0] = lay[0];
		fixed[1] = entries.size();
		for (int i = 0; i < entries.size(); i++) {
			int[] en = entries.get(i);
			fixed[2 + i * 4] = en[0];
			fixed[3 + i * 4] = en[1];
			fixed[4 + i * 4] = en[2];
			fixed[5 + i * 4] = en[3];
		}
		Dx12Mod.LOGGER.info("[DX12] {}: layout de lineas partido en bytes sueltos ({} atributos)", pipeline.getLocation(), entries.size());
		return new LayoutFix(out, fixed);
	}

	private static int[] vertexLayout(RenderPipeline pipeline) {
		VertexFormat f = pipeline.getVertexFormat();
		java.util.List<VertexFormatElement> els = f.getElements();
		int[] a = new int[2 + els.size() * 4];
		a[0] = f.getVertexSize();
		a[1] = els.size();
		for (int i = 0; i < els.size(); i++) {
			VertexFormatElement e = els.get(i);
			a[2 + i * 4] = e.type().ordinal();
			a[3 + i * 4] = e.count();
			// GL normaliza color y normal; posicion/UV/genericos enteros no se normalizan
			a[4 + i * 4] = (e.usage() == VertexFormatElement.Usage.COLOR || e.usage() == VertexFormatElement.Usage.NORMAL) ? 1 : 0;
			a[5 + i * 4] = f.getOffset(e);
		}
		return a;
	}

	private static int[] pipelineState(RenderPipeline p) {
		int[] st = new int[15];
		st[0] = switch (p.getDepthTestFunction()) {
			case NO_DEPTH_TEST -> 0;
			case EQUAL_DEPTH_TEST -> 1;
			case LEQUAL_DEPTH_TEST -> 2;
			case LESS_DEPTH_TEST -> 3;
			case GREATER_DEPTH_TEST -> 4;
		};
		st[1] = p.isWriteDepth() ? 1 : 0;
		st[2] = !p.isCull() || NO_CULL ? 0 : (FLIP_CULL ? 2 : 1);  // 0 ninguno, 1 traseras, 2 delanteras
		var blend = p.getBlendFunction();
		if (blend.isPresent()) {
			BlendFunction b = blend.get();
			st[3] = 1;
			st[4] = src(b.sourceColor(), false);
			st[5] = dst(b.destColor(), false);
			st[6] = src(b.sourceAlpha(), true);
			st[7] = dst(b.destAlpha(), true);
		}
		st[8] = (p.isWriteColor() ? 7 : 0) | (p.isWriteAlpha() ? 8 : 0);
		st[9] = p.getPolygonMode() == com.mojang.blaze3d.platform.PolygonMode.WIREFRAME ? 1 : 0;
		// tipo de topologia D3D12: 1 punto, 2 linea, 3 triangulo; topologia D3D: 1 puntos, 2 lineas, 3 tira de lineas, 4 triangulos, 5 tira de triangulos
		switch (p.getVertexFormatMode()) {
			case POINTS -> { st[10] = 1; st[11] = 1; }
			case DEBUG_LINES -> { st[10] = 2; st[11] = 2; }
			case DEBUG_LINE_STRIP -> { st[10] = 2; st[11] = 3; }
			case TRIANGLE_STRIP -> { st[10] = 3; st[11] = 5; }
			default -> { st[10] = 3; st[11] = 4; } // TRIANGLES, QUADS y LINES (ambos con indices de triangulos), TRIANGLE_FAN (no soportado)
		}
		st[12] = Float.floatToRawIntBits(p.getDepthBiasConstant());
		st[13] = Float.floatToRawIntBits(p.getDepthBiasScaleFactor());
		if (NO_DEPTH) st[0] = 0;
		st[14] = st[0] == 0 ? 0 : 1;
		return st;
	}

	// valores de D3D12_BLEND
	private static int src(SourceFactor f, boolean alpha) {
		return switch (f) {
			case ZERO -> 1;
			case ONE -> 2;
			case SRC_COLOR -> alpha ? 5 : 3;
			case ONE_MINUS_SRC_COLOR -> alpha ? 6 : 4;
			case SRC_ALPHA -> 5;
			case ONE_MINUS_SRC_ALPHA -> 6;
			case DST_ALPHA -> 7;
			case ONE_MINUS_DST_ALPHA -> 8;
			case DST_COLOR -> alpha ? 7 : 9;
			case ONE_MINUS_DST_COLOR -> alpha ? 8 : 10;
			case SRC_ALPHA_SATURATE -> 11;
			case CONSTANT_COLOR, CONSTANT_ALPHA -> 14;
			case ONE_MINUS_CONSTANT_COLOR, ONE_MINUS_CONSTANT_ALPHA -> 15;
		};
	}

	private static int dst(DestFactor f, boolean alpha) {
		return switch (f) {
			case ZERO -> 1;
			case ONE -> 2;
			case SRC_COLOR -> alpha ? 5 : 3;
			case ONE_MINUS_SRC_COLOR -> alpha ? 6 : 4;
			case SRC_ALPHA -> 5;
			case ONE_MINUS_SRC_ALPHA -> 6;
			case DST_ALPHA -> 7;
			case ONE_MINUS_DST_ALPHA -> 8;
			case DST_COLOR -> alpha ? 7 : 9;
			case ONE_MINUS_DST_COLOR -> alpha ? 8 : 10;
			case CONSTANT_COLOR, CONSTANT_ALPHA -> 14;
			case ONE_MINUS_CONSTANT_COLOR, ONE_MINUS_CONSTANT_ALPHA -> 15;
		};
	}

	@Override
	public void clearPipelineCache() {
		for (Dx12Resources.Pipeline p : compiled.values()) if (p.handle != 0L) Dx12Native.destroyPipeline(p.handle);
		compiled.clear();
	}

	@Override public String getImplementationInformation() { return "DirectX 12 (" + adapter + ")"; }
	@Override public List<String> getLastDebugMessages() { return List.of(); }
	@Override public boolean isDebuggingEnabled() { return false; }
	@Override public String getVendor() { return "Microsoft"; }
	@Override public String getBackendName() { return "DirectX 12"; }
	@Override public String getVersion() { return "D3D12"; }
	@Override public String getRenderer() { return adapter; }
	@Override public int getMaxTextureSize() { return 16384; }
	@Override public int getUniformOffsetAlignment() { return 256; }
	@Override public List<String> getEnabledExtensions() { return List.of(); }
	@Override public int getMaxSupportedAnisotropy() { return 1; }

	@Override
	public void close() {
		Dx12Mod.LOGGER.info("[DX12] Resumen: {}", Dx12Native.stats());
		Dx12Mod.LOGGER.info("[DX12] Shaders: {} traducidos, {} con fallo de traduccion; pipelines D3D12: {} OK, {} con fallo (HLSL en dx12mod/shaders)",
			shadersOk, shadersFailed, nativeOk, nativeFailed);
		Dx12RenderPass.logSummary();
		Dx12Native.shutdown();
	}
}
