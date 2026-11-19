package dev.dx12mod.client.render;

import com.mojang.blaze3d.buffers.GpuBuffer;
import com.mojang.blaze3d.buffers.GpuFence;
import com.mojang.blaze3d.pipeline.CompiledRenderPipeline;
import com.mojang.blaze3d.systems.GpuQuery;
import com.mojang.blaze3d.textures.AddressMode;
import com.mojang.blaze3d.textures.FilterMode;
import com.mojang.blaze3d.textures.GpuSampler;
import com.mojang.blaze3d.textures.GpuTexture;
import com.mojang.blaze3d.textures.GpuTextureView;
import com.mojang.blaze3d.textures.TextureFormat;
import dev.dx12mod.client.Dx12Native;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.OptionalDouble;
import java.util.OptionalLong;

/**
 * Paso 5: las texturas ya son recursos D3D12 reales (via Dx12Native). Los buffers siguen en memoria Java
 * (para que el juego pueda escribir/leer sin romperse); vistas y samplers solo recuerdan sus parametros.
 */
final class Dx12Resources {
	private Dx12Resources() {}

	static final class Buffer extends GpuBuffer {
		private static long nextId = 1;

		final ByteBuffer data;
		/** Identificador para la copia residente en la GPU. */
		final long id = nextId++;
		/** Sube cada vez que el contenido cambia; la DLL vuelve a subir el buffer si cambia. */
		long version = 1;
		private long frameSeen = -1;
		private int framesStable;
		private boolean resident;
		private boolean sentToNative;
		private boolean closed;

		Buffer(int usage, long size) {
			super(usage, size);
			this.data = ByteBuffer.allocateDirect((int) Math.min(size, Integer.MAX_VALUE)).order(ByteOrder.nativeOrder());
		}

		/** Llamar siempre que el contenido pueda haber cambiado (escritura, map, copia). */
		void touch() {
			version++;
			resident = false;
			framesStable = 0;
			frameSeen = -1;
		}

		/** Un buffer pasa a ser residente en GPU cuando se usa en 2 frames distintos sin cambiar (terreno si, GUI/entidades no). */
		boolean wantsResident(long frame) {
			if (resident) { sentToNative = true; return true; }
			if (frameSeen != frame) {
				frameSeen = frame;
				framesStable++;
			}
			if (framesStable >= 2 && data.capacity() >= 256) {
				resident = true;
				sentToNative = true;
			}
			return resident;
		}

		@Override public boolean isClosed() { return closed; }

		@Override
		public void close() {
			if (!closed) {
				closed = true;
				if (sentToNative) Dx12Native.destroyBuffer(id);
			}
		}
	}

	static final class MappedView implements GpuBuffer.MappedView {
		private final ByteBuffer view;

		MappedView(Buffer buffer, long offset, long length) {
			ByteBuffer d = buffer.data.duplicate();
			d.position((int) offset);
			d.limit((int) (offset + length));
			this.view = d.slice().order(ByteOrder.nativeOrder());
		}

		@Override public ByteBuffer data() { return view; }
		@Override public void close() {}
	}

	static final class Texture extends GpuTexture {
		/** Handle de la textura D3D12 en la DLL nativa. */
		final long handle;
		/** Bytes por pixel (4 para RGBA8/DEPTH32, 1 para RED8/RED8I). */
		final int bpp;
		private boolean closed;

		Texture(int usage, String label, TextureFormat format, int w, int h, int layers, int mips) {
			super(usage, label, format, w, h, layers, mips);
			int code = switch (format.name()) {
				case "RED8" -> 1;
				case "DEPTH32" -> 2;
				case "RED8I" -> 3;
				default -> 0; // RGBA8
			};
			this.bpp = (code == 1 || code == 3) ? 1 : 4;
			this.handle = Dx12Native.createTexture(w, h, layers, mips, code);
			if (handle == 0L) {
				throw new IllegalStateException("createTexture " + format + " " + w + "x" + h + " fallo: " + Dx12Native.lastError());
			}
		}

		@Override
		public void close() {
			if (!closed) {
				closed = true;
				Dx12Native.destroyTexture(handle);
			}
		}

		@Override public boolean isClosed() { return closed; }
	}

	static final class TextureView extends GpuTextureView {
		private boolean closed;

		TextureView(GpuTexture texture, int baseMip, int mips) {
			super(texture, baseMip, mips);
		}

		@Override public void close() { closed = true; }
		@Override public boolean isClosed() { return closed; }
	}

	static final class Sampler extends GpuSampler {
		private final AddressMode u, v;
		private final FilterMode min, mag;
		private final int aniso;
		private final OptionalDouble lod;

		Sampler(AddressMode u, AddressMode v, FilterMode min, FilterMode mag, int aniso, OptionalDouble lod) {
			this.u = u; this.v = v; this.min = min; this.mag = mag; this.aniso = aniso; this.lod = lod;
		}

		@Override public AddressMode getAddressModeU() { return u; }
		@Override public AddressMode getAddressModeV() { return v; }
		@Override public FilterMode getMinFilter() { return min; }
		@Override public FilterMode getMagFilter() { return mag; }
		@Override public int getMaxAnisotropy() { return aniso; }
		@Override public OptionalDouble getMaxLod() { return lod; }
		@Override public void close() {}
	}

	static final class Pipeline implements CompiledRenderPipeline {
		/** Handle nativo; 0 = no se pudo crear (el juego sigue, pero no se dibuja con este pipeline). */
		final long handle;
		final String name;
		final java.util.Map<String, Integer> uboBindings;
		final java.util.Map<String, Integer> samplerBindings;
		final com.mojang.blaze3d.vertex.VertexFormat.Mode mode;
		/** D3D12 no tiene abanicos de triangulos: se convierten a lista de triangulos en cada draw. */
		final boolean fan;

		Pipeline(long handle, String name, java.util.Map<String, Integer> ubo, java.util.Map<String, Integer> samplers,
				com.mojang.blaze3d.vertex.VertexFormat.Mode mode) {
			this.handle = handle;
			this.name = name;
			this.uboBindings = ubo;
			this.samplerBindings = samplers;
			this.mode = mode;
			this.fan = mode.name().equals("TRIANGLE_FAN");
		}

		@Override public boolean isValid() { return true; }
	}

	/** Fence que siempre esta completada (no hay trabajo de GPU real todavia). */
	static final class Fence implements GpuFence {
		@Override public void close() {}
		@Override public boolean awaitCompletion(long timeout) { return true; }
	}

	static final class Query implements GpuQuery {
		@Override public OptionalLong getValue() { return OptionalLong.empty(); }
		@Override public void close() {}
	}
}
