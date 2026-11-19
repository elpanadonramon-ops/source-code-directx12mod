package dev.dx12mod.client.shader;

import com.mojang.blaze3d.pipeline.RenderPipeline;
import com.mojang.blaze3d.preprocessor.GlslPreprocessor;
import com.mojang.blaze3d.shaders.ShaderSource;
import com.mojang.blaze3d.shaders.ShaderType;
import com.mojang.blaze3d.shaders.UniformType;
import net.fabricmc.loader.api.FabricLoader;
import org.lwjgl.PointerBuffer;
import org.lwjgl.system.MemoryStack;
import org.lwjgl.util.shaderc.Shaderc;
import org.lwjgl.util.spvc.Spvc;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.IntBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.function.Function;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Paso 6a: traduce los shaders de un RenderPipeline de Minecraft: GLSL 330 -> SPIR-V (shaderc) -> HLSL SM 5.0 (SPIRV-Cross).
 *
 * El juego entrega el GLSL ya preprocesado (con #moj_import resueltos) y cada pipeline declara sus uniformes,
 * samplers y formato de vertices. Con eso se reescriben las declaraciones del GLSL para que tengan
 * binding/location EXPLICITOS y deterministas (todos los pipelines usan la misma convencion):
 *
 *   UBO      (std140)        -> binding 0..15   (en el orden de pipeline.getUniforms())
 *   sampler2D                -> binding 16..31  (en el orden de pipeline.getSamplers())
 *   texel buffer (samplerBuffer) -> binding 32+
 *   entrada de vertice       -> location = indice del atributo en el VertexFormat
 *   varying VS->FS           -> location por nombre (el mismo en ambas etapas)
 *   salida del fragment      -> location 0..
 *
 * El HLSL generado se vuelca a <juego>/dx12mod/shaders/ para poder inspeccionarlo.
 */
public final class ShaderTranslator {
	private ShaderTranslator() {}

	public static final class TranslateException extends Exception {
		public TranslateException(String message) {
			super(message);
		}
	}

	/** HLSL de cada etapa y el binding de cada uniforme/sampler (nombre GLSL -> numero de registro). */
	public record Result(String vertexHlsl, String fragmentHlsl, Map<String, Integer> uboBindings, Map<String, Integer> samplerBindings) {}

	private static final int SAMPLER_BASE = 16;
	private static final int TEXEL_BASE = 32;

	private static final Pattern VERSION = Pattern.compile("(?m)^[ \\t]*#version[ \\t]+\\d+(?:[ \\t]+\\w+)?");
	private static final Pattern UBO = Pattern.compile("layout\\s*\\(\\s*std140\\s*\\)\\s*uniform\\s+(\\w+)");
	private static final Pattern SAMPLER = Pattern.compile(
		"(?m)^([ \\t]*)uniform[ \\t]+(sampler2D|sampler2DArray|samplerCube|isamplerBuffer|usamplerBuffer|samplerBuffer)[ \\t]+(\\w+)[ \\t]*;");
	private static final Pattern IN_OUT = Pattern.compile(
		"(?m)^([ \\t]*)((?:flat|smooth|noperspective)[ \\t]+)?(in|out)[ \\t]+(\\w+)[ \\t]+(\\w+)[ \\t]*;");

	public static Result translate(RenderPipeline pipeline, ShaderSource source) throws TranslateException {
		String name = pipeline.getLocation().toString().replace(':', '_').replace('/', '_');

		Layout layout = new Layout(pipeline);
		String vsRaw = fetch(source, pipeline, ShaderType.VERTEX);
		String fsRaw = fetch(source, pipeline, ShaderType.FRAGMENT);
		// Varyings que el fragment no declara: el VS no debe exportarlos, o FXC empaqueta distinto y D3D12 rechaza el PSO.
		java.util.Set<String> fsInputs = new java.util.HashSet<>();
		Matcher fm = IN_OUT.matcher(fsRaw);
		while (fm.find()) {
			if (fm.group(3).equals("in")) fsInputs.add(fm.group(5));
		}
		String vsGlsl = rewrite(vsRaw, ShaderType.VERTEX, pipeline, layout, fsInputs);
		String fsGlsl = rewrite(fsRaw, ShaderType.FRAGMENT, pipeline, layout, null);
		if (!layout.unlisted.isEmpty()) {
			dump(name + ".notes.txt", "Declarados en el GLSL pero no listados en el pipeline (se asumen sin usar): " + layout.unlisted);
		}
		dump(name + ".vs.glsl", vsGlsl);
		dump(name + ".fs.glsl", fsGlsl);

		try {
			String vsHlsl = glslToHlsl(vsGlsl, Shaderc.shaderc_glsl_vertex_shader, name + ".vsh");
			dump(name + ".vs.hlsl", vsHlsl);
			String fsHlsl = glslToHlsl(fsGlsl, Shaderc.shaderc_glsl_fragment_shader, name + ".fsh");
			dump(name + ".fs.hlsl", fsHlsl);
			return new Result(vsHlsl, fsHlsl, Map.copyOf(layout.uboBinding), Map.copyOf(layout.samplerBinding));
		} catch (TranslateException e) {
			dump(name + ".error.txt", e.getMessage());
			throw e;
		}
	}

	// ------------------------------------------------------------------ GLSL

	private static String fetch(ShaderSource source, RenderPipeline pipeline, ShaderType type) throws TranslateException {
		var id = type == ShaderType.VERTEX ? pipeline.getVertexShader() : pipeline.getFragmentShader();
		String glsl = source.get(id, type);
		if (glsl == null) {
			throw new TranslateException("No se encontro el shader " + id + " (" + type.getName() + ")");
		}
		return GlslPreprocessor.injectDefines(glsl, pipeline.getShaderDefines());
	}

	/** Bindings y locations compartidos entre las dos etapas de un pipeline. */
	private static final class Layout {
		final Map<String, Integer> uboBinding = new HashMap<>();
		final Map<String, Integer> texelBinding = new HashMap<>();
		final Map<String, Integer> samplerBinding = new HashMap<>();
		final Map<String, Integer> varyingLocation = new HashMap<>();
		/** Recursos que el GLSL declara pero el pipeline no lista (GL los ignora si no se usan). */
		final java.util.Set<String> unlisted = new java.util.TreeSet<>();
		private final int listedUbos;
		private final int listedSamplers;
		private final int listedTexels;
		private int extraUbos;
		private int extraSamplers;
		private int extraTexels;

		int uboBinding(String id) throws TranslateException {
			Integer b = uboBinding.get(id);
			if (b != null) {
				return b;
			}
			unlisted.add(id);
			int n = listedUbos + extraUbos++;
			if (n >= SAMPLER_BASE) {
				throw new TranslateException("Demasiados bloques uniformes (" + id + ")");
			}
			uboBinding.put(id, n);
			return n;
		}

		int samplerBinding(String id) throws TranslateException {
			Integer b = samplerBinding.get(id);
			if (b != null) {
				return b;
			}
			unlisted.add(id);
			int n = SAMPLER_BASE + listedSamplers + extraSamplers++;
			if (n >= TEXEL_BASE) {
				throw new TranslateException("Demasiados samplers (" + id + ")");
			}
			samplerBinding.put(id, n);
			return n;
		}

		int texelBinding(String id) {
			Integer b = texelBinding.get(id);
			if (b != null) {
				return b;
			}
			unlisted.add(id);
			int n = TEXEL_BASE + listedTexels + extraTexels++;
			texelBinding.put(id, n);
			return n;
		}

		Layout(RenderPipeline pipeline) {
			int ubo = 0;
			int texel = 0;
			for (RenderPipeline.UniformDescription u : pipeline.getUniforms()) {
				if (u.type() == UniformType.UNIFORM_BUFFER) {
					uboBinding.put(u.name(), ubo++);
				} else {
					texelBinding.put(u.name(), TEXEL_BASE + texel++);
				}
			}
			List<String> samplers = pipeline.getSamplers();
			for (int i = 0; i < samplers.size(); i++) {
				samplerBinding.put(samplers.get(i), SAMPLER_BASE + i);
			}
			this.listedUbos = ubo;
			this.listedTexels = texel;
			this.listedSamplers = samplers.size();
		}
	}

	private static String rewrite(String glsl, ShaderType stage, RenderPipeline pipeline, Layout layout,
			java.util.Set<String> fsInputs) throws TranslateException {
		String s = glsl;

		// Vulkan GLSL pide 450+; el resto del codigo de Mojang es compatible.
		Matcher v = VERSION.matcher(s);
		if (v.find()) {
			s = s.substring(0, v.start()) + "#version 450" + s.substring(v.end());
		}
		s = s.replaceAll("\\bgl_VertexID\\b", "gl_VertexIndex").replaceAll("\\bgl_InstanceID\\b", "gl_InstanceIndex");
		// 'sampler' es palabra reservada en Vulkan GLSL: algunos shaders la usan como nombre de parametro.
		s = s.replaceAll("\\bsampler\\b", "mc_sampler");
		// HLSL no tiene tamano de punto por vertice.
		s = s.replaceAll("gl_PointSize\\s*=[^;]*;", "");

		// Bloques uniformes
		s = replaceAll(UBO, s, m -> {
			try {
				return "layout(std140, binding = " + layout.uboBinding(m.group(1)) + ") uniform " + m.group(1);
			} catch (TranslateException e) {
				throw new Unchecked(e.getMessage());
			}
		});

		// Samplers y texel buffers
		s = replaceAll(SAMPLER, s, m -> {
			String type = m.group(2);
			String id = m.group(3);
			int b;
			try {
				b = type.endsWith("Buffer") ? layout.texelBinding(id) : layout.samplerBinding(id);
			} catch (TranslateException e) {
				throw new Unchecked(e.getMessage());
			}
			return m.group(1) + "layout(binding = " + b + ") uniform " + type + " " + id + ";";
		});

		// Entradas/salidas
		final int[] fragmentOut = {0};
		List<String> attributes = pipeline.getVertexFormat().getElementAttributeNames();
		s = replaceAll(IN_OUT, s, m -> {
			String indent = m.group(1);
			String qualifier = m.group(2) == null ? "" : m.group(2);
			boolean isIn = m.group(3).equals("in");
			String type = m.group(4);
			String id = m.group(5);
			int location;
			if (stage == ShaderType.VERTEX && isIn) {
				location = attributes.indexOf(id);
				if (location < 0) {
					throw new Unchecked("El atributo '" + id + "' no esta en el VertexFormat del pipeline " + attributes);
				}
			} else if (stage == ShaderType.VERTEX) {
				if (fsInputs != null && !fsInputs.contains(id)) {
					return indent + type + " " + id + ";";  // variable privada: no es salida del VS
				}
				location = layout.varyingLocation.computeIfAbsent(id, k -> layout.varyingLocation.size());
			} else if (isIn) {
				Integer loc = layout.varyingLocation.get(id);
				if (loc == null) {
					// El vertex shader no lo produce: en GL valdria 0. Pasa a ser una variable global a cero.
					return indent + type + " " + id + " = " + type + "(0);";
				}
				location = loc;
			} else {
				location = fragmentOut[0]++;
			}
			return indent + "layout(location = " + location + ") " + qualifier + m.group(3) + " " + type + " " + id + ";";
		});
		return s;
	}

	/** Excepcion interna para poder lanzar errores desde las lambdas de replaceAll. */
	private static final class Unchecked extends RuntimeException {
		Unchecked(String message) {
			super(message);
		}
	}

	private static String replaceAll(Pattern pattern, String input, Function<Matcher, String> fn) throws TranslateException {
		Matcher m = pattern.matcher(input);
		StringBuilder out = new StringBuilder();
		try {
			while (m.find()) {
				m.appendReplacement(out, Matcher.quoteReplacement(fn.apply(m)));
			}
		} catch (Unchecked e) {
			throw new TranslateException(e.getMessage());
		}
		m.appendTail(out);
		return out.toString();
	}

	// ------------------------------------------------------------------ GLSL -> SPIR-V -> HLSL

	private static String glslToHlsl(String glsl, int kind, String fileName) throws TranslateException {
		IntBuffer spirv = glslToSpirv(glsl, kind, fileName);
		return spirvToHlsl(spirv, kind == Shaderc.shaderc_glsl_vertex_shader, fileName);
	}

	private static IntBuffer glslToSpirv(String glsl, int kind, String fileName) throws TranslateException {
		long compiler = Shaderc.shaderc_compiler_initialize();
		long options = Shaderc.shaderc_compile_options_initialize();
		try {
			Shaderc.shaderc_compile_options_set_source_language(options, Shaderc.shaderc_source_language_glsl);
			Shaderc.shaderc_compile_options_set_target_env(options, Shaderc.shaderc_target_env_vulkan, Shaderc.shaderc_env_version_vulkan_1_0);

			long result = Shaderc.shaderc_compile_into_spv(compiler, glsl, kind, fileName, "main", options);
			try {
				if (Shaderc.shaderc_result_get_compilation_status(result) != Shaderc.shaderc_compilation_status_success) {
					throw new TranslateException("shaderc (" + fileName + "): " + Shaderc.shaderc_result_get_error_message(result));
				}
				ByteBuffer bytes = Shaderc.shaderc_result_get_bytes(result);
				if (bytes == null) {
					throw new TranslateException("shaderc (" + fileName + "): resultado vacio");
				}
				// Copia propia: el resultado de shaderc se libera en el finally.
				ByteBuffer copy = ByteBuffer.allocateDirect(bytes.remaining()).order(ByteOrder.nativeOrder());
				copy.put(bytes);
				copy.flip();
				return copy.asIntBuffer();
			} finally {
				Shaderc.shaderc_result_release(result);
			}
		} finally {
			Shaderc.shaderc_compile_options_release(options);
			Shaderc.shaderc_compiler_release(compiler);
		}
	}

	private static String spirvToHlsl(IntBuffer spirv, boolean vertex, String fileName) throws TranslateException {
		try (MemoryStack stack = MemoryStack.stackPush()) {
			PointerBuffer pContext = stack.mallocPointer(1);
			if (Spvc.spvc_context_create(pContext) != Spvc.SPVC_SUCCESS) {
				throw new TranslateException("SPIRV-Cross: no se pudo crear el contexto");
			}
			long context = pContext.get(0);
			try {
				PointerBuffer pIr = stack.mallocPointer(1);
				check(context, Spvc.spvc_context_parse_spirv(context, spirv, spirv.remaining(), pIr), "parse_spirv", fileName);

				PointerBuffer pCompiler = stack.mallocPointer(1);
				check(context, Spvc.spvc_context_create_compiler(context, Spvc.SPVC_BACKEND_HLSL, pIr.get(0),
					Spvc.SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, pCompiler), "create_compiler", fileName);
				long compiler = pCompiler.get(0);

				PointerBuffer pOptions = stack.mallocPointer(1);
				check(context, Spvc.spvc_compiler_create_compiler_options(compiler, pOptions), "create_options", fileName);
				long options = pOptions.get(0);
				check(context, Spvc.spvc_compiler_options_set_uint(options, Spvc.SPVC_COMPILER_OPTION_HLSL_SHADER_MODEL, 50),
					"set_shader_model", fileName);
				if (vertex) {
					// Convencion del mod: filas de textura con el origen de OpenGL (fila 0 = abajo) -> invertir Y y pasar z de [-1,1] a [0,1].
					check(context, Spvc.spvc_compiler_options_set_bool(options, Spvc.SPVC_COMPILER_OPTION_FLIP_VERTEX_Y, true), "flip_y", fileName);
					check(context, Spvc.spvc_compiler_options_set_bool(options, Spvc.SPVC_COMPILER_OPTION_FIXUP_DEPTH_CONVENTION, true), "fixup_depth", fileName);
				}
				check(context, Spvc.spvc_compiler_install_compiler_options(compiler, options), "install_options", fileName);

				PointerBuffer pSource = stack.mallocPointer(1);
				check(context, Spvc.spvc_compiler_compile(compiler, pSource), "compile", fileName);
				// SM 5.0 solo admite samplers s0..s15: los samplers del GLSL usan bindings 16.. -> se renumeran a s0..
				Matcher sm = Pattern.compile("register\\(s(\\d+)\\)").matcher(pSource.getStringUTF8(0));
				StringBuilder out = new StringBuilder();
				while (sm.find()) {
					int n = Integer.parseInt(sm.group(1));
					sm.appendReplacement(out, "register(s" + (n >= SAMPLER_BASE ? n - SAMPLER_BASE : n) + ")");
				}
				sm.appendTail(out);
				return out.toString();
			} finally {
				Spvc.spvc_context_destroy(context);
			}
		}
	}

	private static void check(long context, int result, String step, String fileName) throws TranslateException {
		if (result != Spvc.SPVC_SUCCESS) {
			throw new TranslateException("SPIRV-Cross " + step + " (" + fileName + "): " + Spvc.spvc_context_get_last_error_string(context));
		}
	}

	// ------------------------------------------------------------------ volcado a disco

	private static Path dumpDir;

	private static void dump(String fileName, String content) {
		try {
			if (dumpDir == null) {
				dumpDir = FabricLoader.getInstance().getGameDir().resolve("dx12mod").resolve("shaders");
				Files.createDirectories(dumpDir);
			}
			Files.writeString(dumpDir.resolve(fileName), content, StandardCharsets.UTF_8);
		} catch (IOException ignored) {
			// el volcado es solo para depurar
		}
	}
}
