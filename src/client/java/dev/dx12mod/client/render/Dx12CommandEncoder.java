package dev.dx12mod.client.render;

import com.mojang.blaze3d.buffers.GpuBuffer;
import com.mojang.blaze3d.buffers.GpuBufferSlice;
import com.mojang.blaze3d.buffers.GpuFence;
import com.mojang.blaze3d.platform.NativeImage;
import com.mojang.blaze3d.systems.CommandEncoder;
import com.mojang.blaze3d.systems.GpuQuery;
import com.mojang.blaze3d.systems.RenderPass;
import com.mojang.blaze3d.textures.GpuTexture;
import com.mojang.blaze3d.textures.GpuTextureView;
import dev.dx12mod.Dx12Mod;
import dev.dx12mod.client.Dx12Native;
import org.lwjgl.system.MemoryUtil;

import java.nio.ByteBuffer;
import java.util.OptionalDouble;
import java.util.OptionalInt;
import java.util.function.Supplier;

/**
 * Paso 5: clears, subida de texturas y presentTexture ya van a la GPU (D3D12) de verdad.
 * Los buffers siguen en memoria Java; el dibujo (render pass) todavia se ignora (paso 6).
 */
final class Dx12CommandEncoder implements CommandEncoder {
	private boolean loggedPresent;

	private static long h(GpuTexture texture) {
		return ((Dx12Resources.Texture) texture).handle;
	}

	private static Dx12Resources.Texture t(GpuTexture texture) {
		return (Dx12Resources.Texture) texture;
	}

	@Override
	public RenderPass createRenderPass(Supplier<String> label, GpuTextureView color, OptionalInt clearColor) {
		if (clearColor.isPresent()) {
			Dx12Native.clearColor(h(color.texture()) | ((long) Math.min(255, color.baseMipLevel()) << 48), clearColor.getAsInt(), 0, 0, 0, 0);
		}
		return new Dx12RenderPass(color, null);
	}

	@Override
	public RenderPass createRenderPass(Supplier<String> label, GpuTextureView color, OptionalInt clearColor,
			GpuTextureView depth, OptionalDouble clearDepth) {
		if (clearColor.isPresent()) {
			Dx12Native.clearColor(h(color.texture()) | ((long) Math.min(255, color.baseMipLevel()) << 48), clearColor.getAsInt(), 0, 0, 0, 0);
		}
		if (depth != null && clearDepth.isPresent()) {
			Dx12Native.clearDepth(h(depth.texture()), (float) clearDepth.getAsDouble(), 0, 0, 0, 0);
		}
		return new Dx12RenderPass(color, depth);
	}

	@Override
	public void clearColorTexture(GpuTexture texture, int color) {
		Dx12Native.clearColor(h(texture), color, 0, 0, 0, 0);
	}

	@Override
	public void clearColorAndDepthTextures(GpuTexture color, int clearColor, GpuTexture depth, double clearDepth) {
		Dx12Native.clearColor(h(color), clearColor, 0, 0, 0, 0);
		Dx12Native.clearDepth(h(depth), (float) clearDepth, 0, 0, 0, 0);
	}

	@Override
	public void clearColorAndDepthTextures(GpuTexture color, int clearColor, GpuTexture depth, double clearDepth,
			int x, int y, int w, int hh) {
		// OJO: el origen en OpenGL es abajo-izquierda y en D3D12 arriba-izquierda; si se ve invertido, se corrige en el paso 6.
		Dx12Native.clearColor(h(color), clearColor, x, y, w, hh);
		Dx12Native.clearDepth(h(depth), (float) clearDepth, x, y, w, hh);
	}

	@Override
	public void clearDepthTexture(GpuTexture depth, double clearDepth) {
		Dx12Native.clearDepth(h(depth), (float) clearDepth, 0, 0, 0, 0);
	}

	@Override
	public void writeToBuffer(GpuBufferSlice slice, ByteBuffer data) {
		Dx12Resources.Buffer target = (Dx12Resources.Buffer) slice.buffer();
		target.touch();
		ByteBuffer dst = target.data.duplicate();
		dst.position((int) slice.offset());
		dst.put(data.duplicate());
	}

	@Override
	public GpuBuffer.MappedView mapBuffer(GpuBuffer buffer, boolean read, boolean write) {
		if (write) ((Dx12Resources.Buffer) buffer).touch();
		return new Dx12Resources.MappedView((Dx12Resources.Buffer) buffer, 0, buffer.size());
	}

	@Override
	public GpuBuffer.MappedView mapBuffer(GpuBufferSlice slice, boolean read, boolean write) {
		if (write) ((Dx12Resources.Buffer) slice.buffer()).touch();
		return new Dx12Resources.MappedView((Dx12Resources.Buffer) slice.buffer(), slice.offset(), slice.length());
	}

	@Override
	public void copyToBuffer(GpuBufferSlice from, GpuBufferSlice to) {
		ByteBuffer src = ((Dx12Resources.Buffer) from.buffer()).data.duplicate();
		src.position((int) from.offset());
		src.limit((int) (from.offset() + from.length()));
		Dx12Resources.Buffer target = (Dx12Resources.Buffer) to.buffer();
		target.touch();
		ByteBuffer dst = target.data.duplicate();
		dst.position((int) to.offset());
		dst.put(src);
	}

	@Override
	public void writeToTexture(GpuTexture texture, NativeImage image) {
		Dx12Resources.Texture tex = t(texture);
		Dx12Native.writeTexture(tex.handle, image.getPointer(), image.getWidth() * tex.bpp, 0, 0,
			0, 0, image.getWidth(), image.getHeight());
	}

	@Override
	public void writeToTexture(GpuTexture texture, NativeImage image, int mip, int layer,
			int destX, int destY, int width, int height, int srcX, int srcY) {
		Dx12Resources.Texture tex = t(texture);
		long addr = image.getPointer() + ((long) srcY * image.getWidth() + srcX) * tex.bpp;
		Dx12Native.writeTexture(tex.handle, addr, image.getWidth() * tex.bpp, mip, layer, destX, destY, width, height);
	}

	@Override
	public void writeToTexture(GpuTexture texture, ByteBuffer data, NativeImage.Format format,
			int mip, int layer, int destX, int destY, int width, int height) {
		Dx12Resources.Texture tex = t(texture);
		ByteBuffer tmp = null;
		long addr;
		if (data.isDirect()) {
			addr = MemoryUtil.memAddress(data);
		} else {
			tmp = MemoryUtil.memAlloc(data.remaining());
			tmp.put(data.duplicate());
			tmp.flip();
			addr = MemoryUtil.memAddress(tmp);
		}
		try {
			Dx12Native.writeTexture(tex.handle, addr, width * tex.bpp, mip, layer, destX, destY, width, height);
		} finally {
			if (tmp != null) MemoryUtil.memFree(tmp);
		}
	}

	// Lecturas de textura (capturas de pantalla): todavia no soportadas; no se llama al callback.
	@Override public void copyTextureToBuffer(GpuTexture texture, GpuBuffer buffer, long offset, Runnable done, int mip) {}
	@Override public void copyTextureToBuffer(GpuTexture texture, GpuBuffer buffer, long offset, Runnable done, int mip,
			int x, int y, int w, int hh) {}
	@Override
	public void copyTextureToTexture(GpuTexture from, GpuTexture to, int mip,
			int destX, int destY, int srcX, int srcY, int w, int hh) {
		Dx12Native.copyTexture(h(from), h(to), mip, destX, destY, srcX, srcY, w, hh);
	}

	@Override
	public void presentTexture(GpuTextureView view) {
		GpuTexture tex = view.texture();
		if (!loggedPresent) {
			loggedPresent = true;
			Dx12Mod.LOGGER.info("[DX12] presentTexture recibido: {}x{} {}", tex.getWidth(0), tex.getHeight(0), tex.getFormat());
		}
		Dx12Native.presentTexture(h(tex));
	}

	@Override public GpuFence createFence() { return new Dx12Resources.Fence(); }
	@Override public GpuQuery timerQueryBegin() { return new Dx12Resources.Query(); }
	@Override public void timerQueryEnd(GpuQuery query) {}
}
