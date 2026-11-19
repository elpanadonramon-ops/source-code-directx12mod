package dev.dx12mod.client;

import dev.dx12mod.Dx12Mod;

import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;

/** Puente JNI hacia dx12native.dll (C++). */
public final class Dx12Native {
	private static boolean loaded;

	private Dx12Native() {}

	/** Extrae la DLL del jar a una carpeta temporal y la carga. */
	public static synchronized boolean load() {
		if (loaded) return true;
		String res = "/natives/windows-x64/dx12native.dll";
		try (InputStream in = Dx12Native.class.getResourceAsStream(res)) {
			if (in == null) {
				Dx12Mod.LOGGER.error("No se encontró {} dentro del jar. Compila native/ primero.", res);
				return false;
			}
			Path dir = Files.createTempDirectory("dx12mod");
			Path dll = dir.resolve("dx12native.dll");
			Files.copy(in, dll, StandardCopyOption.REPLACE_EXISTING);
			dll.toFile().deleteOnExit();
			dir.toFile().deleteOnExit();
			System.load(dll.toAbsolutePath().toString());
			// Comprobar que la DLL es la del paso actual: JNI enlaza cada metodo nativo la primera vez que se llama,
			// asi que una DLL vieja solo fallaria mas tarde (en pleno arranque de Minecraft) con UnsatisfiedLinkError.
			// Estas llamadas son inofensivas (createTexture con tamano 0 devuelve 0 sin tocar la GPU).
			try {
				createTexture(0, 0, 1, 1, 0);
				stats();
				if (packedDrawSize() != PACKED_DRAW_SIZE) {
					Dx12Mod.LOGGER.error("dx12native.dll no coincide con el mod (PackedDraw {} != {}). Ejecuta native\\build.bat.", packedDrawSize(), PACKED_DRAW_SIZE);
					return false;
				}
				drawPacked(0L);
				setVsync(1);
				setInflight(2);
				perfReport();
				destroyBuffer(0L);
				textureStats(0L, 0);
				copyTexture(0L, 0L, 0, 0, 0, 0, 0, 0, 0);
			} catch (UnsatisfiedLinkError stale) {
				Dx12Mod.LOGGER.error("dx12native.dll esta DESACTUALIZADA (falta: {}). Ejecuta native\\build.bat y vuelve a lanzar.",
					stale.getMessage());
				return false;
			}
			loaded = true;
			return true;
		} catch (IOException | UnsatisfiedLinkError e) {
			Dx12Mod.LOGGER.error("Fallo cargando dx12native.dll", e);
			return false;
		}
	}

	/** Crea un ID3D12Device de prueba y devuelve un reporte de texto (GPU, feature level, shader model). */
	public static native String probe();

	// --- Nucleo persistente (paso 4) ---

	/** Crea dispositivo D3D12 + swapchain sobre la ventana (HWND). Devuelve false si falla (ver lastError). */
	public static native boolean init(long hwnd);

	public static native void shutdown();

	public static native String adapterName();

	public static native String lastError();

	// --- Paso 5: texturas reales ---

	/** Codigos de formato: 0=RGBA8, 1=RED8, 2=DEPTH32, 3=RED8I. Devuelve un handle (>0) o 0 si falla. */
	public static native long createTexture(int width, int height, int layers, int mips, int format);

	public static native void destroyTexture(long texture);

	/** Limpia un color ARGB. Si w<=0 o h<=0 limpia toda la textura; si no, solo el rectangulo. */
	public static native void clearColor(long texture, int argb, int x, int y, int w, int h);

	public static native void clearDepth(long texture, float depth, int x, int y, int w, int h);

	/** Sube pixeles desde memoria nativa (srcAddr = primer pixel; srcRowBytes = bytes entre filas de origen). */
	public static native void writeTexture(long texture, long srcAddr, int srcRowBytes, int mip, int layer,
			int destX, int destY, int width, int height);

	/** Marca que textura se copia a la ventana al final del frame. */
	public static native void presentTexture(long texture);

	/** Fin de frame: copia la textura marcada al backbuffer (o pinta r,g,b si no hay ninguna) y presenta. */
	public static native void flip(float r, float g, float b);

	/** Contadores de diagnostico (frames, copias, clears, subidas...). */
	public static native String stats();

	// --- Paso 6b: dibujo ---

	/**
	 * Crea un pipeline D3D12 a partir del HLSL. layout = [stride, n, (tipo, cantidad, normalizado, offset)*n];
	 * state = [depthFunc, depthWrite, cull, blend, srcC, dstC, srcA, dstA, colorMask, wireframe, tipoTopologia,
	 * topologia, depthBias(bits float), slopeBias(bits float), usaProfundidad]. Devuelve 0 si falla (ver lastError).
	 */
	public static native long createPipeline(String name, String vsHlsl, String psHlsl, int[] layout, int[] state);

	public static native void destroyPipeline(long pipeline);

	/** Tamano en bytes de la estructura de parametros de draw (Dx12RenderPass escribe con estos desplazamientos). */
	public static final int PACKED_DRAW_SIZE = 632;

	public static native int packedDrawSize();

	/**
	 * Dibuja leyendo los parametros de una estructura en memoria nativa (sin copiar arrays por JNI en cada draw).
	 * Desplazamientos: pso 0, color 8, depth 16, vbAddr 24, ibAddr 32, vbId 40, vbVer 48, ibId 56, ibVer 64,
	 * vbSize 72, ibSize 76, idxBytes 80, baseVertex 84, count 88, instances 92, sx 96, sy 100, sw 104, sh 108, ibOff 112,
	 * ub 120 (8 pares direccion/bytes), tex 248 (8 pares textura/esCubo), tp 376 (8 grupos de 8 enteros:
	 * baseMip, mips, minF, magF, addrU, addrV, aniso, maxLod como bits float). sw < 0 = sin recorte.
	 */
	public static native void drawPacked(long addr);

	/** VSync segun la opcion de Minecraft: 0 = sin VSync (con tearing si el sistema lo admite), >0 = VSync. */
	public static native void setVsync(int interval);

	/** Solo antes de init(): frames en vuelo (1 = CPU y GPU en serie como antes; 2 o 3 = solapados). */
	public static native void setInflight(int frames);

	// --- Paso 6e: rendimiento ---

	/** Contador de frames (lo incrementa endFrame). */
	public static long frame;

	/** Una vez por frame, tras presentar: avanza el contador y registra el informe de rendimiento cuando toca. */
	public static void endFrame() {
		frame++;
		String r = perfReport();
		if (r != null && !r.isEmpty()) dev.dx12mod.Dx12Mod.LOGGER.info("[DX12 perf] {}", r);
	}

	/** Libera la copia residente en GPU de un buffer del juego (si la hay). */
	public static native void destroyBuffer(long id);

	/** Informe de rendimiento cada 300 frames; cadena vacia el resto del tiempo. Se llama una vez por frame. */
	public static native String perfReport();

	// --- Paso 6c: diagnostico y copias ---

	/** Lee una textura de vuelta a la CPU y devuelve un resumen de su contenido (solo para depurar; es lento). */
	public static native String textureStats(long texture, int mip);

	/** Copia un rectangulo de una textura a otra del mismo formato. */
	public static native void copyTexture(long from, long to, int mip, int destX, int destY, int srcX, int srcY, int w, int h);
}
