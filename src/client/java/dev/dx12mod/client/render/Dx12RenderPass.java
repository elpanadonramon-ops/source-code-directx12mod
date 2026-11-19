package dev.dx12mod.client.render;

import com.mojang.blaze3d.buffers.GpuBuffer;
import com.mojang.blaze3d.buffers.GpuBufferSlice;
import com.mojang.blaze3d.pipeline.RenderPipeline;
import com.mojang.blaze3d.systems.RenderPass;
import com.mojang.blaze3d.textures.AddressMode;
import com.mojang.blaze3d.textures.FilterMode;
import com.mojang.blaze3d.textures.GpuSampler;
import com.mojang.blaze3d.textures.GpuTexture;
import com.mojang.blaze3d.textures.GpuTextureView;
import com.mojang.blaze3d.vertex.VertexFormat;
import dev.dx12mod.Dx12Mod;
import dev.dx12mod.client.Dx12Mode;
import dev.dx12mod.client.Dx12Native;
import org.jspecify.annotations.Nullable;
import org.lwjgl.system.MemoryUtil;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Collection;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.Set;
import java.util.TreeMap;
import java.util.HashMap;
import java.util.Map;
import java.util.function.BiConsumer;
import java.util.function.Supplier;

/**
 * Render pass real. Recuerda el estado (pipeline, uniformes, texturas, buffers) y en cada draw se lo pasa a la DLL
 * por una estructura en memoria nativa (sin arrays JNI). Paso 7: los uniformes y texturas solo se vuelven a resolver
 * cuando cambian (marcas "dirty"), y las trazas por draw estan desactivadas salvo con el archivo run/dx12mod.trace.
 */
final class Dx12RenderPass implements RenderPass {
	private static final int CB_SLOTS = 8;
	private static final int TEX_SLOTS = 8;
	private static final int TEX_BASE = 16;
	private static boolean warnedNoPipeline;
	private static final boolean NOMIP = Dx12Mode.flag("nomip");
	private static final boolean TRACE = Dx12Mode.flag("trace");

	// Estructura de parametros de draw compartida con la DLL (ver Dx12Native.drawPacked). Un solo hilo de render la usa.
	private static final long BUF = MemoryUtil.nmemCalloc(1L, 640L);
	private static final long O_PSO = 0, O_COLOR = 8, O_DEPTH = 16, O_VBADDR = 24, O_IBADDR = 32, O_VBID = 40, O_VBVER = 48,
		O_IBID = 56, O_IBVER = 64, O_VBSIZE = 72, O_IBSIZE = 76, O_IDXBYTES = 80, O_BASEV = 84, O_COUNT = 88, O_INST = 92,
		O_SX = 96, O_SY = 100, O_SW = 104, O_SH = 108, O_IBOFF = 112, O_UB = 120, O_TEX = 248, O_TP = 376;
	/** Ultimo render pass que escribio en BUF: si cambia, las partes de uniformes/texturas se reescriben enteras. */
	private static Dx12RenderPass lastWriter;
	private boolean ubDirty = true;
	private boolean texDirty = true;

	/** Estadisticas por pipeline (solo hilo de render). */
	private static final class Stat {
		long draws, noPipeline, noData;
		int traced;
		final Map<String, Long> missingTex = new TreeMap<>();
		final Map<String, Long> missingUbo = new TreeMap<>();
	}
	private static final Map<String, Stat> STATS = new LinkedHashMap<>();
	private static final Set<Long> PROBED = new HashSet<>();
	private static int probes;
	private static ByteBuffer fanBuf;

	private static Stat stat(String name) {
		return STATS.computeIfAbsent(name, k -> new Stat());
	}

	static void logSummary() {
		for (Map.Entry<String, Stat> e : STATS.entrySet()) {
			Stat t = e.getValue();
			Dx12Mod.LOGGER.info("[DX12 trace] {}: draws={} sinPipelineNativo={} sinDatos={} texturasAusentes={} uniformesAusentes={}",
				e.getKey(), t.draws, t.noPipeline, t.noData, t.missingTex, t.missingUbo);
		}
	}

	private static ByteBuffer fanBuffer(int bytes) {
		if (fanBuf == null || fanBuf.capacity() < bytes) {
			fanBuf = ByteBuffer.allocateDirect(Math.max(bytes, 4096)).order(ByteOrder.nativeOrder());
		}
		return fanBuf;
	}

	private static String describeTex(GpuTexture t) {
		return t.getLabel() + " " + t.getWidth(0) + "x" + t.getHeight(0) + " mips=" + t.getMipLevels() + " " + t.getFormat();
	}

	private static String dumpUbo(Dx12Resources.Buffer b, long off, long len) {
		StringBuilder sb = new StringBuilder();
		int n = (int) Math.min(len / 4, 24);
		for (int i = 0; i < n; i++) {
			int pos = (int) (off + i * 4L);
			sb.append(String.format("%.3f", b.data.getFloat(pos))).append('/').append(Integer.toHexString(b.data.getInt(pos))).append(' ');
		}
		return sb.toString();
	}

	private final long color;
	private final long depth;
	private Dx12Resources.@Nullable Pipeline pipeline;
	private final Map<String, GpuBufferSlice> uniforms = new HashMap<>();
	private final Map<String, TexBinding> textures = new HashMap<>();
	private @Nullable GpuBuffer vertexBuffer;
	private @Nullable GpuBuffer indexBuffer;
	private VertexFormat.IndexType indexType = VertexFormat.IndexType.SHORT;
	private boolean scissor;
	private int sx, sy, sw, sh;

	private record TexBinding(GpuTextureView view, @Nullable GpuSampler sampler) {}

	Dx12RenderPass(@Nullable GpuTextureView colorView, @Nullable GpuTextureView depthView) {
		this.color = colorView == null ? 0L : (((Dx12Resources.Texture) colorView.texture()).handle | ((long) Math.min(255, colorView.baseMipLevel()) << 48));
		this.depth = depthView == null ? 0L : ((Dx12Resources.Texture) depthView.texture()).handle;
	}

	@Override public void pushDebugGroup(Supplier<String> label) {}
	@Override public void popDebugGroup() {}

	@Override
	public void setPipeline(RenderPipeline p) {
		Dx12Device d = Dx12Device.instance;
		this.pipeline = d == null ? null : d.pipelineFor(p);
		ubDirty = true;
		texDirty = true;
	}

	@Override
	public void bindTexture(String name, @Nullable GpuTextureView view, @Nullable GpuSampler sampler) {
		if (view == null) textures.remove(name);
		else textures.put(name, new TexBinding(view, sampler));
		texDirty = true;
	}

	@Override public void setUniform(String name, GpuBuffer buffer) { setUniform(name, buffer.slice()); }

	@Override
	public void setUniform(String name, GpuBufferSlice slice) {
		uniforms.put(name, slice);
		Dx12Resources.Pipeline p = pipeline;
		if (p != null && !ubDirty && lastWriter == this) {
			// camino rapido (p.ej. ChunkSection en cada draw del terreno): solo se reescribe esa ranura
			Integer slot = p.uboBindings.get(name);
			if (slot != null) putUbo(slot, slice);
		} else {
			ubDirty = true;
		}
	}

	@Override
	public void enableScissor(int x, int y, int w, int h) {
		scissor = true;
		sx = x; sy = y; sw = w; sh = h;
	}

	@Override public void disableScissor() { scissor = false; }

	@Override public void setVertexBuffer(int slot, GpuBuffer buffer) { if (slot == 0) vertexBuffer = buffer; }

	@Override
	public void setIndexBuffer(GpuBuffer buffer, VertexFormat.IndexType type) {
		indexBuffer = buffer;
		indexType = type;
	}

	@Override
	public void drawIndexed(int baseVertex, int firstIndex, int indexCount, int instances) {
		submit(baseVertex, firstIndex, indexCount, instances, true);
	}

	@Override
	public <T> void drawMultipleIndexed(Collection<RenderPass.Draw<T>> draws, @Nullable GpuBuffer commonIndexBuffer,
			VertexFormat.@Nullable IndexType commonType, Collection<String> dynamicUniforms, T object) {
		VertexFormat.IndexType defType = commonType == null ? VertexFormat.IndexType.SHORT : commonType;
		RenderPass.UniformUploader uploader = this::setUniform;  // una sola vez, no una por draw
		for (RenderPass.Draw<T> d : draws) {
			VertexFormat.IndexType t = d.indexType() == null ? defType : d.indexType();
			GpuBuffer ib = d.indexBuffer() == null ? commonIndexBuffer : d.indexBuffer();
			if (ib != null) setIndexBuffer(ib, t);
			setVertexBuffer(d.slot(), d.vertexBuffer());
			BiConsumer<T, RenderPass.UniformUploader> up = d.uniformUploaderConsumer();
			if (up != null) up.accept(object, uploader);
			drawIndexed(0, d.firstIndex(), d.indexCount(), 1);
		}
	}

	@Override
	public void draw(int firstVertex, int vertexCount) {
		submit(firstVertex, 0, vertexCount, 1, false);
	}

	@Override public void close() {}

	private void putUbo(int slot, GpuBufferSlice s) {
		if (slot < 0 || slot >= CB_SLOTS || !(s.buffer() instanceof Dx12Resources.Buffer b)) return;
		long a = BUF + O_UB + slot * 16L;
		long len = Math.min(s.length(), b.data.capacity() - s.offset());
		if (len <= 0) {
			MemoryUtil.memPutLong(a, 0L);
			MemoryUtil.memPutLong(a + 8, 0L);
			return;
		}
		MemoryUtil.memPutLong(a, MemoryUtil.memAddress0(b.data) + s.offset());
		MemoryUtil.memPutLong(a + 8, len);
	}

	private void resolveUbos(Dx12Resources.Pipeline p) {
		MemoryUtil.memSet(BUF + O_UB, 0, CB_SLOTS * 16L);
		for (Map.Entry<String, Integer> e : p.uboBindings.entrySet()) {
			GpuBufferSlice s = uniforms.get(e.getKey());
			if (s == null) {
				if (TRACE) stat(p.name).missingUbo.merge(e.getKey(), 1L, Long::sum);
				continue;
			}
			putUbo(e.getValue(), s);
		}
	}

	private void resolveTextures(Dx12Resources.Pipeline p) {
		MemoryUtil.memSet(BUF + O_TEX, 0, TEX_SLOTS * 16L + TEX_SLOTS * 32L);
		Stat st = TRACE ? stat(p.name) : null;
		for (Map.Entry<String, Integer> e : p.samplerBindings.entrySet()) {
			int slot = e.getValue() - TEX_BASE;
			TexBinding tb = textures.get(e.getKey());
			if (slot < 0 || slot >= TEX_SLOTS) continue;
			if (tb == null) { if (st != null) st.missingTex.merge(e.getKey() + ":sinBind", 1L, Long::sum); continue; }
			if (tb.view().isClosed()) { if (st != null) st.missingTex.merge(e.getKey() + ":vistaCerrada", 1L, Long::sum); continue; }
			GpuTexture t = tb.view().texture();
			if (!(t instanceof Dx12Resources.Texture dt) || dt.isClosed()) { if (st != null) st.missingTex.merge(e.getKey() + ":texCerrada", 1L, Long::sum); continue; }
			MemoryUtil.memPutLong(BUF + O_TEX + slot * 16L, dt.handle);
			MemoryUtil.memPutLong(BUF + O_TEX + slot * 16L + 8, ((t.usage() & GpuTexture.USAGE_CUBEMAP_COMPATIBLE) != 0 && t.getDepthOrLayers() >= 6) ? 1L : 0L);
			long o = BUF + O_TP + slot * 32L;
			MemoryUtil.memPutInt(o, NOMIP ? 0 : tb.view().baseMipLevel());
			MemoryUtil.memPutInt(o + 4, NOMIP ? 1 : tb.view().mipLevels());
			GpuSampler s = tb.sampler();
			if (s != null) {
				MemoryUtil.memPutInt(o + 8, s.getMinFilter() == FilterMode.LINEAR ? 1 : 0);
				MemoryUtil.memPutInt(o + 12, s.getMagFilter() == FilterMode.LINEAR ? 1 : 0);
				MemoryUtil.memPutInt(o + 16, s.getAddressModeU() == AddressMode.REPEAT ? 0 : 1);
				MemoryUtil.memPutInt(o + 20, s.getAddressModeV() == AddressMode.REPEAT ? 0 : 1);
				MemoryUtil.memPutInt(o + 24, Math.max(1, s.getMaxAnisotropy()));
				MemoryUtil.memPutInt(o + 28, Float.floatToRawIntBits(NOMIP ? 0.0f : (s.getMaxLod().isPresent() ? (float) s.getMaxLod().getAsDouble() : -1.0f)));
			} else {
				MemoryUtil.memPutInt(o + 16, 1);
				MemoryUtil.memPutInt(o + 20, 1);
				MemoryUtil.memPutInt(o + 24, 1);
				MemoryUtil.memPutInt(o + 28, Float.floatToRawIntBits(-1.0f));
			}
		}
	}

	private void submit(int baseVertex, int firstIndex, int count, int instances, boolean indexed) {
		Dx12Resources.Pipeline p = pipeline;
		if (p == null || p.handle == 0L || count <= 0 || color == 0L) {
			if (TRACE && p != null && p.handle == 0L && count > 0) stat(p.name).noPipeline++;
			if (p == null && !warnedNoPipeline) {
				warnedNoPipeline = true;
				Dx12Mod.LOGGER.warn("[DX12] draw sin pipeline");
			}
			return;
		}

		long vbAddr = 0, vbId = 0, vbVer = 0;
		int vbSize = 0;
		if (vertexBuffer instanceof Dx12Resources.Buffer vb) {
			vbAddr = MemoryUtil.memAddress0(vb.data);
			vbSize = vb.data.capacity();
			if (vb.wantsResident(Dx12Native.frame)) {
				vbId = vb.id;
				vbVer = vb.version;
			}
		}

		long ibAddr = 0, ibId = 0, ibVer = 0;
		int ibSize = 0;
		int ibOff = 0;
		int idxBytes = 0;
		if (indexed) {
			if (!(indexBuffer instanceof Dx12Resources.Buffer ib)) { if (TRACE) stat(p.name).noData++; return; }
			idxBytes = indexType.bytes;
			long start = (long) firstIndex * idxBytes;
			long len = (long) count * idxBytes;
			if (start < 0 || start + len > ib.data.capacity()) { if (TRACE) stat(p.name).noData++; return; }
			ibAddr = MemoryUtil.memAddress0(ib.data);   // base del buffer; la DLL aplica ibOff
			ibSize = ib.data.capacity();
			ibOff = (int) start;
			if (ib.wantsResident(Dx12Native.frame)) {
				ibId = ib.id;
				ibVer = ib.version;
			}
		}

		if (p.fan && count >= 3) {
			int tris = count - 2;
			ByteBuffer fb = fanBuffer(tris * 12);
			ByteBuffer src = indexed ? ((Dx12Resources.Buffer) indexBuffer).data : null;
			long base = (long) firstIndex * idxBytes;
			int i0 = fanIndex(src, base, idxBytes, 0);
			for (int t = 0; t < tris; t++) {
				fb.putInt(t * 12, i0);
				fb.putInt(t * 12 + 4, fanIndex(src, base, idxBytes, t + 1));
				fb.putInt(t * 12 + 8, fanIndex(src, base, idxBytes, t + 2));
			}
			ibAddr = MemoryUtil.memAddress0(fb);
			ibSize = tris * 12;
			ibOff = 0;
			ibId = 0;
			ibVer = 0;
			idxBytes = 4;
			count = tris * 3;
			indexed = true;
		}

		if (lastWriter != this) {
			lastWriter = this;
			ubDirty = true;
			texDirty = true;
		}
		if (ubDirty) { resolveUbos(p); ubDirty = false; }
		if (texDirty) { resolveTextures(p); texDirty = false; }

		if (TRACE) {
			Stat st = stat(p.name);
			st.draws++;
			trace(p, st, count, indexed, vbSize, ibSize);
		}

		long b = BUF;
		MemoryUtil.memPutLong(b + O_PSO, p.handle);
		MemoryUtil.memPutLong(b + O_COLOR, color);
		MemoryUtil.memPutLong(b + O_DEPTH, depth);
		MemoryUtil.memPutLong(b + O_VBADDR, vbAddr);
		MemoryUtil.memPutLong(b + O_IBADDR, ibAddr);
		MemoryUtil.memPutLong(b + O_VBID, vbId);
		MemoryUtil.memPutLong(b + O_VBVER, vbVer);
		MemoryUtil.memPutLong(b + O_IBID, ibId);
		MemoryUtil.memPutLong(b + O_IBVER, ibVer);
		MemoryUtil.memPutInt(b + O_VBSIZE, vbSize);
		MemoryUtil.memPutInt(b + O_IBSIZE, ibSize);
		MemoryUtil.memPutInt(b + O_IDXBYTES, idxBytes);
		MemoryUtil.memPutInt(b + O_BASEV, baseVertex);
		MemoryUtil.memPutInt(b + O_COUNT, count);
		MemoryUtil.memPutInt(b + O_INST, instances);
		MemoryUtil.memPutInt(b + O_SX, sx);
		MemoryUtil.memPutInt(b + O_SY, sy);
		MemoryUtil.memPutInt(b + O_SW, scissor ? sw : -1);
		MemoryUtil.memPutInt(b + O_SH, sh);
		MemoryUtil.memPutInt(b + O_IBOFF, ibOff);
		Dx12Native.drawPacked(b);
	}

	private static int fanIndex(@Nullable ByteBuffer src, long base, int bytes, int i) {
		if (src == null) return i;
		int pos = (int) (base + (long) i * bytes);
		return bytes == 2 ? (src.getShort(pos) & 0xFFFF) : src.getInt(pos);
	}

	/** Primeros dibujos de cada pipeline: que se enlazo y que falta. Para terreno y atlas tambien vuelca uniformes y contenido de texturas. */
	private void trace(Dx12Resources.Pipeline p, Stat st, int count, boolean indexed, int vbSize, int ibSize) {
		boolean key = p.name.contains("terrain") || p.name.contains("gui_textured") || p.name.contains("sky");
		if (st.traced >= (key ? 2 : 1)) return;
		st.traced++;
		StringBuilder sb = new StringBuilder("[DX12 trace] 1er draw ").append(p.name).append(" modo=").append(p.mode)
			.append(" n=").append(count).append(indexed ? " idx" : " sin-idx").append(" vb=").append(vbSize).append(" ib=").append(ibSize)
			.append(scissor ? " scissor" : "").append(depth != 0L ? " +depth" : " sin-depth");
		for (Map.Entry<String, Integer> e : p.uboBindings.entrySet()) {
			GpuBufferSlice s = uniforms.get(e.getKey());
			sb.append("\n    UBO ").append(e.getKey()).append("@").append(e.getValue());
			if (s == null) { sb.append(" FALTA"); continue; }
			sb.append(" off=").append(s.offset()).append(" len=").append(s.length());
			if (key && s.buffer() instanceof Dx12Resources.Buffer b && s.offset() + 96 <= b.data.capacity()) {
				sb.append(" [").append(dumpUbo(b, s.offset(), s.length())).append("]");
			}
		}
		for (Map.Entry<String, Integer> e : p.samplerBindings.entrySet()) {
			TexBinding tb = textures.get(e.getKey());
			sb.append("\n    TEX ").append(e.getKey()).append("@").append(e.getValue());
			if (tb == null) { sb.append(" FALTA"); continue; }
			GpuTexture t = tb.view().texture();
			sb.append(" ").append(describeTex(t)).append(tb.sampler() == null ? " sin-sampler" : " filtro=" + tb.sampler().getMinFilter() + "/" + tb.sampler().getMagFilter());
			if (t instanceof Dx12Resources.Texture dt && String.valueOf(t.getLabel()).toLowerCase().contains("atlas") && probes < 12 && PROBED.add(dt.handle)) {
				probes++;
				sb.append("\n      contenido ").append(Dx12Native.textureStats(dt.handle, 0));
				int last = t.getMipLevels() - 1;
				if (last > 0) sb.append("\n      contenido ").append(Dx12Native.textureStats(dt.handle, last));
			}
		}
		Dx12Mod.LOGGER.info(sb.toString());
	}
}
