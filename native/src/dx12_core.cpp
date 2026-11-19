// Paso 7 (rendimiento): frames en vuelo (la CPU graba el frame N+1 mientras la GPU dibuja el N), cache de estado entre draws,
// tablas de descriptores reutilizadas, uniformes deduplicados, VSync/tearing segun la opcion de Minecraft y parametros de draw
// compartidos por memoria nativa. Modelo: una lista de comandos siempre abierta; se ejecuta al final del frame (flip) o antes
// si se acumula demasiado trabajo (en ese caso se espera a la GPU, como antes).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <jni.h>
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
const UINT kFrames = 3;      // backbuffers del swapchain
const UINT kMaxInflight = 3; // frames en vuelo como maximo
const DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
const UINT kRtvRing = 4096;
const UINT kDsvRing = 1024;
const size_t kMaxPendingUpload = 128u * 1024u * 1024u;

// --- Paso 6: dibujo ---
const UINT kCbSlots = 8;    // uniformes b0..b7 (CBV raiz); el traductor numera los UBO desde 0
const UINT kTexSlots = 8;   // texturas t16..t23 y samplers s16..s23; el traductor numera los samplers desde 16
const UINT kTexBase = 16;
const UINT kSrvRing = 16384;  // por frame en vuelo
const UINT kSampMax = 512;     // samplers individuales
const UINT kSampTables = 256;  // tablas de 8 samplers (el heap visible admite 2048 descriptores)
size_t kUploadRing = 48u * 1024u * 1024u;  // por frame en vuelo (paso 11: se reduce en GPUs integradas / poca VRAM)
size_t gPoolMax = 192u * 1024u * 1024u;     // tope del pool de buffers reciclables
size_t gHeapPage = 64u * 1024u * 1024u;     // tamano de cada pagina de heap para buffers residentes
size_t gHeapBudget = 768u * 1024u * 1024u;  // tope total de paginas de heap
bool gLowMem = false;                       // perfil de bajo consumo (GPU integrada o poca VRAM)
const size_t kShadowMax = 512;  // uniformes de hasta este tamano se deduplican entre draws
const size_t kZeroCb = 256;  // primeros bytes del anillo: bloque de ceros para uniformes sin asignar

// Codigos de formato (deben coincidir con Dx12Resources.Texture en Java)
enum : int { FMT_RGBA8 = 0, FMT_RED8 = 1, FMT_DEPTH32 = 2, FMT_RED8I = 3 };

struct Tex {
	ComPtr<ID3D12Resource> res;
	UINT w = 0, h = 0, layers = 1, mips = 1;
	DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
	UINT bpp = 4;
	bool depth = false;
	D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};

struct Pso {
	ComPtr<ID3D12PipelineState> withDepth;  // solo si el pipeline usa profundidad
	ComPtr<ID3D12PipelineState> noDepth;
	D3D_PRIMITIVE_TOPOLOGY topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	UINT stride = 0;
	bool wantsDepth = false;
};

struct TexBind {
	Tex* t = nullptr;
	int baseMip = 0, mipCount = 0;
	bool cube = false;
	int minF = 0, magF = 0, addrU = 1, addrV = 1, aniso = 1;
	float maxLod = -1.0f;
};

static inline long long qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static inline double qpcMs(long long ticks) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return ticks * 1000.0 / (double)f.QuadPart; }

// Buffer residente en la GPU (heap DEFAULT): vertices/indices estaticos se suben una vez en vez de copiarse en cada draw.
struct GBuf {
	ComPtr<ID3D12Resource> res;
	size_t size = 0;  // bytes validos del buffer del juego
	size_t cap = 0;   // capacidad real del recurso (clase de tamano)
	jlong version = -1;
	D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
	bool failed = false;
	bool placed = false;  // paso 8: vive dentro de un heap grande (su hueco no se puede devolver al sistema)
};

struct PooledBuf {
	ComPtr<ID3D12Resource> res;
	size_t cap = 0;
	D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
	bool placed = false;
};

// paso 8: paginas de memoria de video de las que se cortan los buffers residentes (CreatePlacedResource es mucho mas barato
// que CreateCommittedResource, que pide memoria nueva al sistema de video en cada llamada).
struct HeapPage {
	ComPtr<ID3D12Heap> heap;
	size_t size = 0, used = 0;
};

// --- paso 7: rendimiento ---
struct K16 { uint64_t w[16]; bool operator==(const K16& o) const { return std::memcmp(w, o.w, sizeof(w)) == 0; } };
struct K8 { uint64_t w[8]; bool operator==(const K8& o) const { return std::memcmp(w, o.w, sizeof(w)) == 0; } };
struct KHash {
	template <class K> size_t operator()(const K& k) const {
		uint64_t h = 1469598103934665603ull;
		for (uint64_t v : k.w) { h ^= v; h *= 1099511628211ull; h ^= h >> 29; }
		return (size_t)h;
	}
};

// Lo que ya esta fijado en la lista de comandos actual: evita repetir llamadas D3D12 identicas entre draws.
struct StateCache {
	bool rootBound = false;
	D3D12_GPU_VIRTUAL_ADDRESS cb[kCbSlots] = {};
	UINT64 srvGpu = 0, sampGpu = 0;
	ID3D12PipelineState* pso = nullptr;
	bool targetsValid = false;
	jlong colorKey = 0, depthKey = 0;
	D3D12_CPU_DESCRIPTOR_HANDLE rtv{}, dsv{};
	bool hasDsv = false;
	bool srValid = false;
	D3D12_RECT sr{};
	D3D_PRIMITIVE_TOPOLOGY topo = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
	D3D12_GPU_VIRTUAL_ADDRESS vbLoc = 0;
	UINT vbSize = 0, vbStride = 0;
	D3D12_GPU_VIRTUAL_ADDRESS ibLoc = 0;
	UINT ibSize = 0;
	DXGI_FORMAT ibFmt = DXGI_FORMAT_UNKNOWN;
	bool blendSet = false;
	bool srvKeyValid = false, sampKeyValid = false;
	K16 srvKey{};
	UINT64 srvKeyGpu = 0;
	K8 sampKey{};
	UINT64 sampKeyGpu = 0;
	// Copia en CPU de los uniformes ya subidos al anillo (el anillo es write-combined: leerlo seria lentisimo).
	UINT shadowLen[kCbSlots] = {};
	D3D12_GPU_VIRTUAL_ADDRESS cbAddr[kCbSlots] = {};
	unsigned char shadow[kCbSlots][kShadowMax];
};

// Todo lo que pertenece a UN frame en vuelo: asi la CPU graba el frame N+1 mientras la GPU aun dibuja el N.
struct FrameCtx {
	ComPtr<ID3D12CommandAllocator> alloc;
	ComPtr<ID3D12Resource> ring;
	unsigned char* ringPtr = nullptr;
	ComPtr<ID3D12DescriptorHeap> srvHeap, rtvRing, dsvRing;
	UINT64 fence = 0;
	std::vector<ComPtr<ID3D12Resource>> trash;
	std::vector<PooledBuf> gbufRetire;
	std::vector<ComPtr<ID3D12PipelineState>> psoTrash;
};

struct Core {
	bool ready = false;
	HWND hwnd{};
	UINT width = 0, height = 0;
	ComPtr<IDXGIFactory6> factory;
	ComPtr<ID3D12Device> dev;
	ComPtr<ID3D12CommandQueue> queue;
	ComPtr<IDXGISwapChain3> swap;
	ComPtr<ID3D12DescriptorHeap> rtvHeap;      // backbuffers
	ComPtr<ID3D12DescriptorHeap> clearRtvHeap; // anillo de RTVs transitorios
	ComPtr<ID3D12DescriptorHeap> dsvHeap;      // anillo de DSVs transitorios
	UINT rtvSize = 0, dsvSize = 0;
	UINT rtvNext = 0, dsvNext = 0;
	ComPtr<ID3D12Resource> backbuf[kFrames];
	ComPtr<ID3D12CommandAllocator> alloc;
	ComPtr<ID3D12GraphicsCommandList> list;
	ComPtr<ID3D12Fence> fence;
	UINT64 fenceValue = 0;
	HANDLE fenceEvent{};
	std::string adapterName;
	std::string lastError;

	std::unordered_map<jlong, Tex> tex;
	jlong nextTex = 1;
	std::vector<ComPtr<ID3D12Resource>> trash; // recursos a liberar cuando la GPU termine
	size_t pendingUpload = 0;
	jlong pendingPresent = 0;

	UINT64 nFlips = 0, nPresentCopies = 0, nFallbackFrames = 0, nClears = 0, nUploads = 0, nUploadBytes = 0;

	// --- paso 6: dibujo ---
	ComPtr<ID3D12RootSignature> rootSig;
	ComPtr<ID3D12DescriptorHeap> srvHeap;   // visible al shader: SRVs de cada draw
	ComPtr<ID3D12DescriptorHeap> sampHeap;  // visible al shader: samplers deduplicados
	UINT srvInc = 0, sampInc = 0, srvNext = 0, sampCount = 0;
	std::unordered_map<unsigned long long, UINT> sampCache;
	ComPtr<ID3D12Resource> ring;            // memoria de subida por frame (vertices, indices, uniformes)
	unsigned char* ringPtr = nullptr;
	size_t ringOff = 0;
	std::unordered_map<jlong, Pso> pso;
	jlong nextPso = 1;
	std::vector<ComPtr<ID3D12PipelineState>> psoTrash;
	ComPtr<ID3D12PipelineState> presentPso;
	UINT64 nDraws = 0, nSkipped = 0;
	std::vector<std::string> skipReasons;

	std::unordered_map<jlong, GBuf> gbuf;
	// contadores de rendimiento (se reinician en cada informe)
	// reciclaje de buffers residentes: crear recursos committed en cada chunk nuevo era caro
	std::unordered_map<size_t, std::vector<PooledBuf>> gbufPool;
	std::vector<PooledBuf> gbufRetire;  // retirados este frame; pasan al pool tras esperar a la GPU
	size_t gbufPoolBytes = 0;
	std::vector<HeapPage> bufHeaps;   // paso 8
	size_t bufHeapBytes = 0;
	UINT64 pfPlaced = 0;
	long long pfCreateTime = 0;
	UINT64 shHits = 0, shMiss = 0;   // paso 9: cache de shaders en disco (acumulados, no se reinician)
	long long shCompileTime = 0;
	long long pfDraw = 0, pfWait = 0, pfResTime = 0, pfLast = 0, pfMaxFrame = 0;
	UINT64 pfResCreated = 0, pfResReused = 0, pfSlow = 0;
	UINT64 pfDraws = 0, pfResDraws = 0, pfRingBytes = 0, pfResUploads = 0, pfResBytes = 0, pfSubmits = 0, pfFrames = 0;

	// --- paso 7 ---
	UINT nInflight = 2, cur = 0;
	FrameCtx fr[kMaxInflight];
	StateCache sc;
	std::unordered_map<K16, UINT64, KHash> srvTables;   // por frame: combinacion de texturas -> tabla en el heap SRV
	std::unordered_map<K8, UINT64, KHash> sampTables;   // persistente: 8 samplers -> tabla en el heap de samplers
	ComPtr<ID3D12DescriptorHeap> sampStage;             // samplers individuales (heap sin visibilidad de shader)
	UINT sampTableCount = 0;
	UINT64 sampTable0 = 0;
	std::vector<Tex*> texIdx;                           // id -> textura (acceso O(1) en cada draw)
	Pso* lastPso = nullptr;
	jlong lastPsoId = 0;
	int vsync = 1;
	bool tearing = false;
	UINT swapFlags = 0;
	HANDLE latencyWait = nullptr;
	long long pfPresent = 0, pfNative = 0, pfFlip = 0, pfStart = 0;

	std::mutex mtx;
};
Core g;

std::string narrow(const wchar_t* w) {
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	std::string s(n > 0 ? n - 1 : 0, '\0');
	if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
	return s;
}

void fail(const char* what, HRESULT hr) {
	char buf[320];
	if (g.dev && (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)) {
		snprintf(buf, sizeof(buf), "%s fallo (HRESULT 0x%08X, razon de device removed 0x%08X)", what, (unsigned)hr,
			(unsigned)g.dev->GetDeviceRemovedReason());
	} else {
		snprintf(buf, sizeof(buf), "%s fallo (HRESULT 0x%08X)", what, (unsigned)hr);
	}
	g.lastError = buf;
}

#define TRY(expr, what) do { HRESULT _hr = (expr); if (FAILED(_hr)) { fail(what, _hr); return false; } } while (0)

inline Tex* findTex(jlong id) {
	if (id <= 0 || (size_t)id >= g.texIdx.size()) return nullptr;
	return g.texIdx[(size_t)id];
}

void waitFence(UINT64 v) {
	if (!g.fence || g.fence->GetCompletedValue() >= v) return;
	const long long t0 = qpc();
	g.fence->SetEventOnCompletion(v, g.fenceEvent);
	WaitForSingleObject(g.fenceEvent, INFINITE);
	g.pfWait += qpc() - t0;
}

// Espera a que la GPU termine TODO lo enviado hasta ahora (redimensionar, cerrar, vaciados a mitad de frame).
void waitGpu() {
	if (!g.queue || !g.fence) return;
	g.queue->Signal(g.fence.Get(), ++g.fenceValue);
	waitFence(g.fenceValue);
}

void barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
	D3D12_RESOURCE_BARRIER b{};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = r;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = from;
	b.Transition.StateAfter = to;
	g.list->ResourceBarrier(1, &b);
}

void transition(Tex& t, D3D12_RESOURCE_STATES s) {
	if (t.state == s) return;
	barrier(t.res.Get(), t.state, s);
	t.state = s;
}

// El frame "actual" vive en los miembros g.alloc/g.ring/g.srvHeap...; al cambiar de frame se guardan y se cargan los del siguiente.
void saveFrame() {
	FrameCtx& f = g.fr[g.cur];
	f.alloc = std::move(g.alloc);
	f.ring = std::move(g.ring);
	f.ringPtr = g.ringPtr;
	f.srvHeap = std::move(g.srvHeap);
	f.rtvRing = std::move(g.clearRtvHeap);
	f.dsvRing = std::move(g.dsvHeap);
	f.trash.swap(g.trash);
	f.gbufRetire.swap(g.gbufRetire);
	f.psoTrash.swap(g.psoTrash);
	g.ringPtr = nullptr;
}

void loadFrame(UINT i) {
	FrameCtx& f = g.fr[i];
	g.alloc = std::move(f.alloc);
	g.ring = std::move(f.ring);
	g.ringPtr = f.ringPtr;
	g.srvHeap = std::move(f.srvHeap);
	g.clearRtvHeap = std::move(f.rtvRing);
	g.dsvHeap = std::move(f.dsvRing);
	g.trash.swap(f.trash);
	g.gbufRetire.swap(f.gbufRetire);
	g.psoTrash.swap(f.psoTrash);
	g.cur = i;
}

void resetStateCache() {
	StateCache& c = g.sc;
	c.rootBound = false;
	std::memset(c.cb, 0, sizeof(c.cb));
	c.srvGpu = c.sampGpu = 0;
	c.pso = nullptr;
	c.targetsValid = false;
	c.srValid = false;
	c.topo = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
	c.vbLoc = 0; c.vbSize = c.vbStride = 0;
	c.ibLoc = 0; c.ibSize = 0; c.ibFmt = DXGI_FORMAT_UNKNOWN;
	c.blendSet = false;
	c.srvKeyValid = c.sampKeyValid = false;
	std::memset(c.shadowLen, 0, sizeof(c.shadowLen));
	std::memset(c.cbAddr, 0, sizeof(c.cbAddr));
	g.srvTables.clear();
}

// Libera lo que el frame ACTUAL (ya terminado en la GPU) tenia pendiente de destruir.
void releaseFrameResources() {
	g.trash.clear();
	const size_t kPoolMax = gPoolMax;
	for (auto& r : g.gbufRetire) {
		if (r.placed || g.gbufPoolBytes + r.cap <= kPoolMax) {  // los colocados no se sueltan: su hueco en el heap no se recupera
			g.gbufPoolBytes += r.cap;
			g.gbufPool[r.cap].push_back(std::move(r));
		}  // si no cabe, se libera al destruirse
	}
	g.gbufRetire.clear();
	g.psoTrash.clear();
	g.pendingUpload = 0;
}

// Cierra y ejecuta la lista.
//  doPresent=true : presenta y PASA AL SIGUIENTE frame en vuelo (solo espera si la GPU aun no termino el de hace nInflight frames).
//  doPresent=false: vaciado a mitad de frame; espera a que la GPU termine todo y sigue en el mismo frame.
bool submit(bool doPresent) {
	g.pfSubmits++;
	if (FAILED(g.list->Close())) { g.lastError = "ID3D12GraphicsCommandList::Close fallo"; g.ready = false; return false; }
	ID3D12CommandList* lists[] = { g.list.Get() };
	g.queue->ExecuteCommandLists(1, lists);
	if (doPresent) {
		const long long t0 = qpc();
		const UINT interval = g.vsync ? 1u : 0u;
		const UINT flags = (!g.vsync && g.tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
		HRESULT hr = g.swap->Present(interval, flags);
		g.pfPresent += qpc() - t0;
		if (FAILED(hr)) { fail("Present", hr); g.ready = false; }
		const UINT64 fv = ++g.fenceValue;
		g.queue->Signal(g.fence.Get(), fv);
		g.fr[g.cur].fence = fv;
		saveFrame();
		loadFrame((g.cur + 1) % g.nInflight);
		waitFence(g.fr[g.cur].fence);  // el frame que vamos a reutilizar tiene que haber terminado
	} else {
		waitGpu();
	}
	releaseFrameResources();
	g.rtvNext = 0;
	g.dsvNext = 0;
	g.srvNext = 0;
	g.ringOff = kZeroCb;
	if (FAILED(g.alloc->Reset()) || FAILED(g.list->Reset(g.alloc.Get(), nullptr))) {
		g.lastError = "No se pudo reiniciar la lista de comandos";
		g.ready = false;
		return false;
	}
	resetStateCache();
	return g.ready;
}

bool createRtvs() {
	D3D12_CPU_DESCRIPTOR_HANDLE h = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
	for (UINT i = 0; i < kFrames; ++i) {
		TRY(g.swap->GetBuffer(i, IID_PPV_ARGS(&g.backbuf[i])), "GetBuffer");
		g.dev->CreateRenderTargetView(g.backbuf[i].Get(), nullptr, h);
		h.ptr += g.rtvSize;
	}
	return true;
}

bool getClientSize(UINT& w, UINT& h) {
	RECT rc{};
	if (!GetClientRect(g.hwnd, &rc)) return false;
	w = (UINT)(rc.right - rc.left);
	h = (UINT)(rc.bottom - rc.top);
	return true;
}

bool resizeSwapchain(UINT w, UINT h) {
	waitGpu();
	for (auto& b : g.backbuf) b.Reset();
	TRY(g.swap->ResizeBuffers(kFrames, w, h, DXGI_FORMAT_UNKNOWN, g.swapFlags), "ResizeBuffers");
	g.width = w;
	g.height = h;
	return createRtvs();
}

// ---------- dibujo: raiz, descriptores, anillos ----------

void skip(const std::string& why) {
	g.nSkipped++;
	for (auto& r : g.skipReasons) if (r == why) return;
	if (g.skipReasons.size() < 8) g.skipReasons.push_back(why);
}

// paso 9: cache de bytecode en disco (carpeta dx12mod_cache junto al juego). Compilar ~200 shaders con OPTIMIZATION_LEVEL3 es lo que
// mas tarda al arrancar; la segunda vez que abres el juego se lee de disco en vez de compilar.
static uint64_t fnv64(const void* p, size_t n, uint64_t h) {
	const unsigned char* b = (const unsigned char*)p;
	for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
	return h;
}

static std::string shaderCachePath(const std::string& src, const char* target) {
	static const char kSalt[] = "O3|v1";  // cambia si cambian las opciones de compilacion
	uint64_t a = fnv64(kSalt, sizeof(kSalt), 1469598103934665603ull);
	a = fnv64(target, std::strlen(target), a);
	a = fnv64(src.data(), src.size(), a);
	uint64_t b = fnv64(src.data(), src.size(), 0x9E3779B97F4A7C15ull);
	b = fnv64(target, std::strlen(target), b) ^ ((uint64_t)src.size() * 0x100000001B3ull);
	char buf[128];
	snprintf(buf, sizeof(buf), "dx12mod_cache\\%016llx%016llx.bin", (unsigned long long)a, (unsigned long long)b);
	return buf;
}

static ComPtr<ID3DBlob> loadCachedShader(const std::string& path) {
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return nullptr;
	ComPtr<ID3DBlob> blob;
	std::fseek(f, 0, SEEK_END);
	const long sz = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	if (sz > 16 && sz < 64L * 1024 * 1024 && SUCCEEDED(D3DCreateBlob((SIZE_T)sz, &blob))) {
		unsigned char* d = (unsigned char*)blob->GetBufferPointer();
		if (std::fread(d, 1, (size_t)sz, f) != (size_t)sz || std::memcmp(d, "DXBC", 4) != 0) blob.Reset();  // truncado o corrupto
	}
	std::fclose(f);
	return blob;
}

static void saveCachedShader(const std::string& path, ID3DBlob* code) {
	CreateDirectoryA("dx12mod_cache", nullptr);
	const std::string tmp = path + ".tmp";
	FILE* f = std::fopen(tmp.c_str(), "wb");
	if (!f) return;
	const size_t n = code->GetBufferSize();
	const bool ok = std::fwrite(code->GetBufferPointer(), 1, n, f) == n;
	std::fclose(f);
	if (!ok || !MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileA(tmp.c_str());
}

ComPtr<ID3DBlob> compileHlsl(const std::string& src, const char* target, const std::string& name) {
	const std::string cpath = shaderCachePath(src, target);
	if (ComPtr<ID3DBlob> hit = loadCachedShader(cpath)) {
		g.shHits++;
		return hit;
	}
	const long long t0 = qpc();
	ComPtr<ID3DBlob> code, err;
	HRESULT hr = D3DCompile(src.data(), src.size(), name.c_str(), nullptr, nullptr, "main", target,
		D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
	g.shCompileTime += qpc() - t0;
	g.shMiss++;
	if (FAILED(hr)) {
		std::string msg = err ? std::string((const char*)err->GetBufferPointer(), err->GetBufferSize()) : std::string("sin mensaje");
		g.lastError = std::string("HLSL ") + target + " (" + name + "): " + msg;
		return nullptr;
	}
	saveCachedShader(cpath, code.Get());
	return code;
}

// Devuelve el indice del sampler en el heap de samplers individuales (se crea la primera vez que se pide).
UINT getSampler(int minF, int magF, int au, int av, int aniso, float maxLod) {
	unsigned fb = 0;
	std::memcpy(&fb, &maxLod, sizeof(fb));
	const unsigned long long key = ((unsigned long long)fb << 24) | ((unsigned long long)(aniso & 0xFF) << 12) |
		((unsigned long long)(au & 3) << 8) | ((unsigned long long)(av & 3) << 4) |
		((unsigned long long)(minF & 1) << 1) | (unsigned long long)(magF & 1);
	auto it = g.sampCache.find(key);
	if (it != g.sampCache.end()) return it->second;
	if (g.sampCount >= kSampMax) return 0;

	D3D12_SAMPLER_DESC sd{};
	const bool an = aniso > 1 && minF && magF;
	sd.Filter = an ? D3D12_FILTER_ANISOTROPIC : (D3D12_FILTER)(((minF ? 1 : 0) << 4) | ((magF ? 1 : 0) << 2) | (minF ? 1 : 0));
	sd.AddressU = au ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sd.AddressV = av ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sd.AddressW = sd.AddressU;
	sd.MipLODBias = 0.0f;
	sd.MaxAnisotropy = an ? (UINT)std::min(aniso, 16) : 1u;
	sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	sd.MinLOD = 0.0f;
	sd.MaxLOD = maxLod >= 0.0f ? maxLod : D3D12_FLOAT32_MAX;
	D3D12_CPU_DESCRIPTOR_HANDLE h = g.sampStage->GetCPUDescriptorHandleForHeapStart();
	h.ptr += (SIZE_T)g.sampCount * g.sampInc;
	g.dev->CreateSampler(&sd, h);
	const UINT idx = g.sampCount++;
	g.sampCache.emplace(key, idx);
	return idx;
}

// Una tabla = 8 samplers contiguos en el heap visible al shader. Se crea una vez por combinacion y se reutiliza siempre.
UINT64 addSampTable(const K8& key, const UINT idx[8]) {
	if (g.sampTableCount >= kSampTables) return g.sampTable0;  // heap lleno (no deberia pasar): sampler por defecto
	D3D12_CPU_DESCRIPTOR_HANDLE dst = g.sampHeap->GetCPUDescriptorHandleForHeapStart();
	dst.ptr += (SIZE_T)g.sampTableCount * kTexSlots * g.sampInc;
	const D3D12_CPU_DESCRIPTOR_HANDLE stage0 = g.sampStage->GetCPUDescriptorHandleForHeapStart();
	for (UINT i = 0; i < kTexSlots; ++i) {
		D3D12_CPU_DESCRIPTOR_HANDLE src = stage0;
		src.ptr += (SIZE_T)idx[i] * g.sampInc;
		g.dev->CopyDescriptorsSimple(1, dst, src, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
		dst.ptr += g.sampInc;
	}
	D3D12_GPU_DESCRIPTOR_HANDLE gpu = g.sampHeap->GetGPUDescriptorHandleForHeapStart();
	gpu.ptr += (UINT64)g.sampTableCount * kTexSlots * g.sampInc;
	g.sampTableCount++;
	g.sampTables.emplace(key, gpu.ptr);
	return gpu.ptr;
}

bool createRootSig() {
	D3D12_DESCRIPTOR_RANGE srvRange{};
	srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srvRange.NumDescriptors = kTexSlots;
	srvRange.BaseShaderRegister = kTexBase;
	srvRange.RegisterSpace = 0;
	srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	// UNA sola tabla con los 8 samplers (SM5 solo tiene s0..s15; el traductor renumera s16.. -> s0..)
	D3D12_DESCRIPTOR_RANGE sampRange{};
	sampRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
	sampRange.NumDescriptors = kTexSlots;
	sampRange.BaseShaderRegister = 0;
	sampRange.RegisterSpace = 0;
	sampRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	D3D12_ROOT_PARAMETER params[kCbSlots + 2]{};
	for (UINT i = 0; i < kCbSlots; ++i) {
		params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		params[i].Descriptor.ShaderRegister = i;
		params[i].Descriptor.RegisterSpace = 0;
		params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	}
	params[kCbSlots].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[kCbSlots].DescriptorTable.NumDescriptorRanges = 1;
	params[kCbSlots].DescriptorTable.pDescriptorRanges = &srvRange;
	params[kCbSlots].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[kCbSlots + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[kCbSlots + 1].DescriptorTable.NumDescriptorRanges = 1;
	params[kCbSlots + 1].DescriptorTable.pDescriptorRanges = &sampRange;
	params[kCbSlots + 1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	D3D12_ROOT_SIGNATURE_DESC rs{};
	rs.NumParameters = (UINT)(sizeof(params) / sizeof(params[0]));
	rs.pParameters = params;
	rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	ComPtr<ID3DBlob> blob, err;
	HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
	if (FAILED(hr)) {
		g.lastError = std::string("D3D12SerializeRootSignature: ") + (err ? (const char*)err->GetBufferPointer() : "?");
		return false;
	}
	TRY(g.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.rootSig)), "CreateRootSignature");
	return true;
}

void defaultBlend(D3D12_RENDER_TARGET_BLEND_DESC& rt) {
	rt.BlendEnable = FALSE;
	rt.LogicOpEnable = FALSE;
	rt.SrcBlend = D3D12_BLEND_ONE;
	rt.DestBlend = D3D12_BLEND_ZERO;
	rt.BlendOp = D3D12_BLEND_OP_ADD;
	rt.SrcBlendAlpha = D3D12_BLEND_ONE;
	rt.DestBlendAlpha = D3D12_BLEND_ZERO;
	rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	rt.LogicOp = D3D12_LOGIC_OP_NOOP;
	rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
}

// Triangulo a pantalla completa que copia una textura al backbuffer VOLTEANDO verticalmente:
// el mod guarda todo con la convencion de OpenGL (fila 0 = abajo); la ventana tiene la fila 0 arriba.
const char* kPresentVs = R"(
struct VOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VOut main(uint id : SV_VertexID) {
	VOut o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
	o.uv = float2(uv.x, 1.0 - uv.y);
	return o;
}
)";
const char* kPresentPs = R"(
Texture2D<float4> Src : register(t16);
SamplerState Smp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
	return float4(Src.Sample(Smp, uv).rgb, 1.0);
}
)";

bool createPresentPso() {
	ComPtr<ID3DBlob> vs = compileHlsl(kPresentVs, "vs_5_0", "present.vs");
	if (!vs) return false;
	ComPtr<ID3DBlob> ps = compileHlsl(kPresentPs, "ps_5_0", "present.ps");
	if (!ps) return false;
	D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
	d.pRootSignature = g.rootSig.Get();
	d.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
	d.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
	defaultBlend(d.BlendState.RenderTarget[0]);
	d.SampleMask = UINT_MAX;
	d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	d.RasterizerState.DepthClipEnable = TRUE;
	d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	d.NumRenderTargets = 1;
	d.RTVFormats[0] = kFormat;
	d.SampleDesc.Count = 1;
	TRY(g.dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&g.presentPso)), "CreateGraphicsPipelineState(present)");
	return true;
}

// Recursos que existen una vez por frame en vuelo: allocator, anillo de subida, heap SRV y anillos de RTV/DSV.
bool createFrames() {
	g.srvInc = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	for (UINT i = 0; i < g.nInflight; ++i) {
		FrameCtx& f = g.fr[i];
		TRY(g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc)), "CreateCommandAllocator");

		D3D12_DESCRIPTOR_HEAP_DESC hd{};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		hd.NumDescriptors = kSrvRing;
		hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		TRY(g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&f.srvHeap)), "CreateDescriptorHeap(SRV)");
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		hd.NumDescriptors = kRtvRing;
		hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		TRY(g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&f.rtvRing)), "CreateDescriptorHeap(RTV anillo)");
		D3D12_DESCRIPTOR_HEAP_DESC dd{};
		dd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		dd.NumDescriptors = kDsvRing;
		TRY(g.dev->CreateDescriptorHeap(&dd, IID_PPV_ARGS(&f.dsvRing)), "CreateDescriptorHeap(DSV anillo)");

		D3D12_HEAP_PROPERTIES hp{};
		hp.Type = D3D12_HEAP_TYPE_UPLOAD;
		D3D12_RESOURCE_DESC bd{};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = kUploadRing;
		bd.Height = 1;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		TRY(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
			IID_PPV_ARGS(&f.ring)), "CreateCommittedResource(anillo de subida)");
		D3D12_RANGE noRead{ 0, 0 };
		TRY(f.ring->Map(0, &noRead, (void**)&f.ringPtr), "Map(anillo de subida)");
		std::memset(f.ringPtr, 0, kZeroCb);
	}
	loadFrame(0);
	g.ringOff = kZeroCb;
	return true;
}

bool createDrawResources() {
	if (!createRootSig()) return false;

	D3D12_DESCRIPTOR_HEAP_DESC hd{};
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
	hd.NumDescriptors = kSampMax;
	hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	TRY(g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.sampStage)), "CreateDescriptorHeap(SAMPLER individuales)");
	hd.NumDescriptors = kSampTables * kTexSlots;
	hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	TRY(g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.sampHeap)), "CreateDescriptorHeap(SAMPLER tablas)");
	g.sampInc = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

	getSampler(0, 0, 1, 1, 1, -1.0f);  // indice 0: point + clamp (para ranuras sin textura)
	K8 zero{};
	const UINT idx0[8] = {};
	g.sampTable0 = addSampTable(zero, idx0);
	return createPresentPso();
}

void makeSrv(D3D12_CPU_DESCRIPTOR_HANDLE dst, const TexBind& b) {
	D3D12_SHADER_RESOURCE_VIEW_DESC d{};
	d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	if (!b.t) {  // SRV nulo
		d.Format = kFormat;
		d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		d.Texture2D.MipLevels = 1;
		g.dev->CreateShaderResourceView(nullptr, &d, dst);
		return;
	}
	const Tex& t = *b.t;
	const DXGI_FORMAT f = (t.fmt == DXGI_FORMAT_R32_TYPELESS) ? DXGI_FORMAT_R32_FLOAT : t.fmt;
	const UINT base = (UINT)std::min(std::max(b.baseMip, 0), (int)t.mips - 1);
	UINT cnt = b.mipCount > 0 ? (UINT)b.mipCount : (t.mips - base);
	cnt = std::min(cnt, t.mips - base);
	d.Format = f;
	if (b.cube && t.layers >= 6) {
		d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
		d.TextureCube.MostDetailedMip = base;
		d.TextureCube.MipLevels = cnt;
	} else if (t.layers > 1) {
		d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
		d.Texture2DArray.MostDetailedMip = base;
		d.Texture2DArray.MipLevels = cnt;
		d.Texture2DArray.FirstArraySlice = 0;
		d.Texture2DArray.ArraySize = t.layers;
	} else {
		d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		d.Texture2D.MostDetailedMip = base;
		d.Texture2D.MipLevels = cnt;
	}
	g.dev->CreateShaderResourceView(t.res.Get(), &d, dst);
}

// Heaps + firma raiz: una vez por lista de comandos. Despues de fijar la firma raiz los parametros quedan sin definir.
void ensureRoot() {
	StateCache& c = g.sc;
	if (c.rootBound) return;
	ID3D12DescriptorHeap* heaps[] = { g.srvHeap.Get(), g.sampHeap.Get() };
	g.list->SetDescriptorHeaps(2, heaps);
	g.list->SetGraphicsRootSignature(g.rootSig.Get());
	c.rootBound = true;
	std::memset(c.cb, 0, sizeof(c.cb));
	c.srvGpu = c.sampGpu = 0;
}

bool init(HWND hwnd) {
	g.hwnd = hwnd;

	UINT factoryFlags = 0;
	if (GetEnvironmentVariableA("DX12MOD_DEBUG", nullptr, 0) > 0) {
		ComPtr<ID3D12Debug> dbg;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
			dbg->EnableDebugLayer();
			factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
		}
	}
	TRY(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&g.factory)), "CreateDXGIFactory2");

	ComPtr<IDXGIAdapter1> adapter;
	for (UINT i = 0; g.factory->EnumAdapterByGpuPreference(
			i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++i) {
		DXGI_ADAPTER_DESC1 d{};
		adapter->GetDesc1(&d);
		if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
			SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr))) {
			g.adapterName = narrow(d.Description);
			break;
		}
		adapter.Reset();
	}
	if (!adapter) { g.lastError = "No hay GPU compatible con D3D12"; return false; }

	TRY(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.dev)), "D3D12CreateDevice");

	// paso 11: perfil de memoria. Una GPU integrada (UMA) comparte la RAM del sistema y suele ir con 4-8 GB en total; las
	// reservas de una tarjeta dedicada (48 MB de anillo por frame, 192 MB de pool, hasta 768 MB de heaps) se reducen.
	// DX12MOD_LOWMEM=1 fuerza el perfil bajo, DX12MOD_LOWMEM=0 lo desactiva.
	{
		DXGI_ADAPTER_DESC1 ad{};
		adapter->GetDesc1(&ad);
		D3D12_FEATURE_DATA_ARCHITECTURE arch{};
		const bool uma = SUCCEEDED(g.dev->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &arch, sizeof(arch))) && arch.UMA;
		MEMORYSTATUSEX ms{};
		ms.dwLength = sizeof(ms);
		GlobalMemoryStatusEx(&ms);
		const size_t ramMB = (size_t)(ms.ullTotalPhys >> 20);
		const size_t vramMB = (size_t)(ad.DedicatedVideoMemory >> 20);
		bool low = uma || vramMB < 1536;
		char ev[8] = {};
		if (GetEnvironmentVariableA("DX12MOD_LOWMEM", ev, sizeof(ev)) > 0) low = (ev[0] != '0');
		gLowMem = low;
		if (low) {
			kUploadRing = (ramMB <= 4096 ? 16u : 24u) * 1024u * 1024u;
			gHeapPage = 16u * 1024u * 1024u;
			gPoolMax = 48u * 1024u * 1024u;
			gHeapBudget = (ramMB <= 4096 ? 192u : 320u) * 1024u * 1024u;
		}
	}

	D3D12_COMMAND_QUEUE_DESC qd{};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	TRY(g.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue)), "CreateCommandQueue");

	UINT w = 0, h = 0;
	if (!getClientSize(w, h)) { g.lastError = "GetClientRect fallo (HWND invalido?)"; return false; }
	if (w == 0) w = 1;
	if (h == 0) h = 1;

	// Tearing (VSync desactivado de verdad) y objeto de espera de latencia: se prueba con ellos y, si falla, sin ellos.
	BOOL allowTearing = FALSE;
	{
		ComPtr<IDXGIFactory5> f5;
		if (SUCCEEDED(g.factory.As(&f5))) {
			if (FAILED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing)))) allowTearing = FALSE;
		}
	}
	ComPtr<IDXGISwapChain1> sc1;
	UINT flagsTry[2] = { (allowTearing ? (UINT)DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u) | (UINT)DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT, 0u };
	HRESULT shr = E_FAIL;
	for (int t = 0; t < 2; ++t) {
		DXGI_SWAP_CHAIN_DESC1 sd{};
		sd.Width = w;
		sd.Height = h;
		sd.Format = kFormat;
		sd.SampleDesc.Count = 1;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.BufferCount = kFrames;
		sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		sd.Flags = flagsTry[t];
		shr = g.factory->CreateSwapChainForHwnd(g.queue.Get(), hwnd, &sd, nullptr, nullptr, &sc1);
		if (SUCCEEDED(shr)) { g.swapFlags = flagsTry[t]; break; }
	}
	TRY(shr, "CreateSwapChainForHwnd");
	g.tearing = (g.swapFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
	g.factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
	TRY(sc1.As(&g.swap), "QueryInterface IDXGISwapChain3");
	if (g.swapFlags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
		ComPtr<IDXGISwapChain2> sc2;
		if (SUCCEEDED(g.swap.As(&sc2))) {
			sc2->SetMaximumFrameLatency(2);
			g.latencyWait = sc2->GetFrameLatencyWaitableObject();
		}
	}
	g.width = w;
	g.height = h;

	D3D12_DESCRIPTOR_HEAP_DESC hd{};
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	hd.NumDescriptors = kFrames;
	TRY(g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.rtvHeap)), "CreateDescriptorHeap(RTV backbuffers)");
	g.rtvSize = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	g.dsvSize = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	if (!createRtvs()) return false;

	if (g.nInflight < 1) g.nInflight = 1;
	if (g.nInflight > kMaxInflight) g.nInflight = kMaxInflight;
	if (!createFrames()) return false;  // deja el frame 0 cargado en g.alloc / g.ring / g.srvHeap ...

	TRY(g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc.Get(), nullptr, IID_PPV_ARGS(&g.list)), "CreateCommandList");
	// La lista se queda ABIERTA: todo se graba en ella hasta el siguiente submit().

	TRY(g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)), "CreateFence");
	g.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

	if (!createDrawResources()) return false;
	resetStateCache();

	g.ready = true;
	return true;
}

// ---------- texturas ----------

jlong createTexture(int w, int h, int layers, int mips, int fmtCode) {
	if (!g.ready || w <= 0 || h <= 0) return 0;
	Tex t;
	t.w = (UINT)w;
	t.h = (UINT)h;
	t.layers = (UINT)std::max(layers, 1);
	t.mips = (UINT)std::max(mips, 1);
	DXGI_FORMAT resFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
	switch (fmtCode) {
		case FMT_RED8:    resFmt = DXGI_FORMAT_R8_UNORM; t.bpp = 1; break;
		case FMT_RED8I:   resFmt = DXGI_FORMAT_R8_SINT;  t.bpp = 1; break;
		case FMT_DEPTH32: resFmt = DXGI_FORMAT_R32_TYPELESS; t.bpp = 4; t.depth = true; break;
		default:          resFmt = DXGI_FORMAT_R8G8B8A8_UNORM; t.bpp = 4; break;
	}
	t.fmt = resFmt;

	D3D12_HEAP_PROPERTIES hp{};
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC rd{};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	rd.Width = t.w;
	rd.Height = t.h;
	rd.DepthOrArraySize = (UINT16)t.layers;
	rd.MipLevels = (UINT16)t.mips;
	rd.Format = resFmt;
	rd.SampleDesc.Count = 1;
	rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	rd.Flags = t.depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	HRESULT hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON,
		nullptr, IID_PPV_ARGS(&t.res));
	if (FAILED(hr)) { fail("CreateCommittedResource(textura)", hr); return 0; }

	jlong id = g.nextTex++;
	auto ins = g.tex.emplace(id, std::move(t));
	if ((size_t)id >= g.texIdx.size()) g.texIdx.resize(std::max<size_t>((size_t)id + 1, g.texIdx.size() * 2), nullptr);
	g.texIdx[(size_t)id] = &ins.first->second;
	return id;
}

void destroyTexture(jlong id) {
	auto it = g.tex.find(id);
	if (it == g.tex.end()) return;
	g.trash.push_back(it->second.res); // la lista abierta aun puede referenciarla
	if ((size_t)id < g.texIdx.size()) g.texIdx[(size_t)id] = nullptr;
	g.tex.erase(it);
	if (g.pendingPresent == id) g.pendingPresent = 0;
}

// El mip del destino viaja en los bits altos del handle (handle | mip << 48).
static inline int mipOf(jlong h) { return (int)((h >> 48) & 0xFF); }
static inline jlong idOf(jlong h) { return h & ((1LL << 48) - 1); }
static void makeRtv(Tex& t, int mip, D3D12_CPU_DESCRIPTOR_HANDLE cpu) {
	if (mip <= 0 || (UINT)mip >= t.mips) {
		g.dev->CreateRenderTargetView(t.res.Get(), nullptr, cpu);
		return;
	}
	D3D12_RENDER_TARGET_VIEW_DESC rv{};
	rv.Format = kFormat;
	rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	rv.Texture2D.MipSlice = (UINT)mip;
	g.dev->CreateRenderTargetView(t.res.Get(), &rv, cpu);
}

void clearColor(jlong hid, int argb, int x, int y, int w, int h) {
	if (!g.ready) return;
	const int cmip = mipOf(hid);
	const jlong id = idOf(hid);
	auto it = g.tex.find(id);
	if (it == g.tex.end() || it->second.depth) return;
	if (g.rtvNext >= kRtvRing && !submit(false)) return;
	Tex& t = it->second;
	transition(t, D3D12_RESOURCE_STATE_RENDER_TARGET);
	D3D12_CPU_DESCRIPTOR_HANDLE cpu = g.clearRtvHeap->GetCPUDescriptorHandleForHeapStart();
	cpu.ptr += (SIZE_T)g.rtvNext++ * g.rtvSize;
	makeRtv(t, cmip, cpu);
	const unsigned u = (unsigned)argb;
	const float c[4] = { ((u >> 16) & 255) / 255.0f, ((u >> 8) & 255) / 255.0f, (u & 255) / 255.0f, ((u >> 24) & 255) / 255.0f };
	if (w > 0 && h > 0) {
		D3D12_RECT rc{ x, y, x + w, y + h };
		g.list->ClearRenderTargetView(cpu, c, 1, &rc);
	} else {
		g.list->ClearRenderTargetView(cpu, c, 0, nullptr);
	}
	g.nClears++;
}

void clearDepth(jlong id, float d, int x, int y, int w, int h) {
	if (!g.ready) return;
	auto it = g.tex.find(id);
	if (it == g.tex.end() || !it->second.depth) return;
	if (g.dsvNext >= kDsvRing && !submit(false)) return;
	Tex& t = it->second;
	transition(t, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	D3D12_CPU_DESCRIPTOR_HANDLE cpu = g.dsvHeap->GetCPUDescriptorHandleForHeapStart();
	cpu.ptr += (SIZE_T)g.dsvNext++ * g.dsvSize;
	D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
	dv.Format = DXGI_FORMAT_D32_FLOAT;
	dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	g.dev->CreateDepthStencilView(t.res.Get(), &dv, cpu);
	if (w > 0 && h > 0) {
		D3D12_RECT rc{ x, y, x + w, y + h };
		g.list->ClearDepthStencilView(cpu, D3D12_CLEAR_FLAG_DEPTH, d, 0, 1, &rc);
	} else {
		g.list->ClearDepthStencilView(cpu, D3D12_CLEAR_FLAG_DEPTH, d, 0, 0, nullptr);
	}
	g.nClears++;
}

// Copia w*h pixeles (bpp de la textura) desde memoria del proceso a la textura.
// src apunta al primer pixel; srcRowBytes es la distancia entre filas de origen.
void writeTexture(jlong id, const unsigned char* src, int srcRowBytes, int mip, int layer, int dx, int dy, int w, int h) {
	if (!g.ready || !src) return;
	auto it = g.tex.find(id);
	if (it == g.tex.end() || it->second.depth) return;
	Tex& t = it->second;
	if (mip < 0 || (UINT)mip >= t.mips || layer < 0 || (UINT)layer >= t.layers) return;
	const int mw = (int)std::max(1u, t.w >> mip), mh = (int)std::max(1u, t.h >> mip);
	if (dx < 0 || dy < 0 || dx >= mw || dy >= mh) return;
	w = std::min(w, mw - dx);
	h = std::min(h, mh - dy);
	if (w <= 0 || h <= 0) return;

	const UINT rowBytes = (UINT)w * t.bpp;
	const UINT pitch = (rowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
	const UINT64 size = (UINT64)pitch * (UINT)h;

	D3D12_HEAP_PROPERTIES hp{};
	hp.Type = D3D12_HEAP_TYPE_UPLOAD;
	D3D12_RESOURCE_DESC bd{};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = size;
	bd.Height = 1;
	bd.DepthOrArraySize = 1;
	bd.MipLevels = 1;
	bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ComPtr<ID3D12Resource> up;
	HRESULT hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr, IID_PPV_ARGS(&up));
	if (FAILED(hr)) { fail("CreateCommittedResource(upload)", hr); return; }

	unsigned char* dst = nullptr;
	D3D12_RANGE noRead{ 0, 0 };
	if (FAILED(up->Map(0, &noRead, (void**)&dst))) return;
	for (int r = 0; r < h; ++r) {
		std::memcpy(dst + (size_t)r * pitch, src + (size_t)r * (size_t)srcRowBytes, rowBytes);
	}
	up->Unmap(0, nullptr);

	transition(t, D3D12_RESOURCE_STATE_COPY_DEST);
	D3D12_TEXTURE_COPY_LOCATION dl{}, sl{};
	dl.pResource = t.res.Get();
	dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dl.SubresourceIndex = (UINT)mip + (UINT)layer * t.mips;
	sl.pResource = up.Get();
	sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	sl.PlacedFootprint.Offset = 0;
	sl.PlacedFootprint.Footprint.Format = t.fmt;
	sl.PlacedFootprint.Footprint.Width = (UINT)w;
	sl.PlacedFootprint.Footprint.Height = (UINT)h;
	sl.PlacedFootprint.Footprint.Depth = 1;
	sl.PlacedFootprint.Footprint.RowPitch = pitch;
	g.list->CopyTextureRegion(&dl, (UINT)dx, (UINT)dy, 0, &sl, nullptr);

	g.trash.push_back(up);
	g.pendingUpload += (size_t)size;
	g.nUploads++;
	g.nUploadBytes += size;
	if (g.pendingUpload > kMaxPendingUpload) submit(false);
}

// ---------- pipelines y dibujo ----------

// type: 0 FLOAT, 1 UBYTE, 2 BYTE, 3 USHORT, 4 SHORT, 5 UINT, 6 INT (orden de VertexFormatElement.Type)
DXGI_FORMAT vtxFormat(int type, int count, bool norm) {
	switch (type) {
		case 0:
			return count <= 1 ? DXGI_FORMAT_R32_FLOAT : count == 2 ? DXGI_FORMAT_R32G32_FLOAT
				: count == 3 ? DXGI_FORMAT_R32G32B32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
		case 1:
			if (count <= 1) return norm ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8_UINT;
			if (count == 2) return norm ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8G8_UINT;
			return norm ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UINT;
		case 2:
			if (count <= 1) return norm ? DXGI_FORMAT_R8_SNORM : DXGI_FORMAT_R8_SINT;
			if (count == 2) return norm ? DXGI_FORMAT_R8G8_SNORM : DXGI_FORMAT_R8G8_SINT;
			return norm ? DXGI_FORMAT_R8G8B8A8_SNORM : DXGI_FORMAT_R8G8B8A8_SINT;
		case 3:
			if (count <= 1) return norm ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R16_UINT;
			if (count == 2) return norm ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R16G16_UINT;
			return norm ? DXGI_FORMAT_R16G16B16A16_UNORM : DXGI_FORMAT_R16G16B16A16_UINT;
		case 4:
			if (count <= 1) return norm ? DXGI_FORMAT_R16_SNORM : DXGI_FORMAT_R16_SINT;
			if (count == 2) return norm ? DXGI_FORMAT_R16G16_SNORM : DXGI_FORMAT_R16G16_SINT;
			return norm ? DXGI_FORMAT_R16G16B16A16_SNORM : DXGI_FORMAT_R16G16B16A16_SINT;
		case 5:
			return count <= 1 ? DXGI_FORMAT_R32_UINT : count == 2 ? DXGI_FORMAT_R32G32_UINT
				: count == 3 ? DXGI_FORMAT_R32G32B32_UINT : DXGI_FORMAT_R32G32B32A32_UINT;
		case 6:
			return count <= 1 ? DXGI_FORMAT_R32_SINT : count == 2 ? DXGI_FORMAT_R32G32_SINT
				: count == 3 ? DXGI_FORMAT_R32G32B32_SINT : DXGI_FORMAT_R32G32B32A32_SINT;
		case 7:  // paso 10: un float de 32 bits en un desplazamiento no alineado se lee como 4 bytes y el shader lo reconstruye
			return DXGI_FORMAT_R8G8B8A8_UINT;
		default: return DXGI_FORMAT_UNKNOWN;
	}
}

D3D12_COMPARISON_FUNC depthFunc(int f) {
	switch (f) {
		case 1: return D3D12_COMPARISON_FUNC_EQUAL;
		case 2: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
		case 3: return D3D12_COMPARISON_FUNC_LESS;
		case 4: return D3D12_COMPARISON_FUNC_GREATER;
		default: return D3D12_COMPARISON_FUNC_ALWAYS;
	}
}

// Diagnostico: firmas de entrada/salida y recursos de un shader (se anexa al error si falla la creacion del PSO).
std::string describeShader(ID3DBlob* blob, const char* tag) {
	ComPtr<ID3D11ShaderReflection> r;
	if (FAILED(D3DReflect(blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D11ShaderReflection), (void**)&r))) return std::string(" | ") + tag + ": sin reflexion";
	D3D11_SHADER_DESC d{};
	r->GetDesc(&d);
	char buf[160];
	std::string s = std::string(" | ") + tag + " in[";
	for (UINT i = 0; i < d.InputParameters; ++i) {
		D3D11_SIGNATURE_PARAMETER_DESC p{};
		r->GetInputParameterDesc(i, &p);
		snprintf(buf, sizeof(buf), "%s%u:m%x:t%d:sv%d ", p.SemanticName, p.SemanticIndex, (unsigned)p.Mask, (int)p.ComponentType, (int)p.SystemValueType);
		s += buf;
	}
	s += "] out[";
	for (UINT i = 0; i < d.OutputParameters; ++i) {
		D3D11_SIGNATURE_PARAMETER_DESC p{};
		r->GetOutputParameterDesc(i, &p);
		snprintf(buf, sizeof(buf), "%s%u:m%x:t%d:sv%d ", p.SemanticName, p.SemanticIndex, (unsigned)p.Mask, (int)p.ComponentType, (int)p.SystemValueType);
		s += buf;
	}
	s += "] res[";
	for (UINT i = 0; i < d.BoundResources; ++i) {
		D3D11_SHADER_INPUT_BIND_DESC b{};
		r->GetResourceBindingDesc(i, &b);
		snprintf(buf, sizeof(buf), "%s:type%d@%u ", b.Name, (int)b.Type, b.BindPoint);
		s += buf;
	}
	return s + "]";
}

// lay = [stride, n, (type, count, normalizado, offset) * n]; la semantica de cada elemento es TEXCOORD<indice>
// (asi numera el traductor las entradas de vertice).
// st  = [0 depthFunc, 1 depthWrite, 2 cull, 3 blend, 4 srcC, 5 dstC, 6 srcA, 7 dstA, 8 colorMask, 9 wireframe,
//        10 tipoTopologia, 11 topologia, 12 depthBias (bits float), 13 slopeBias (bits float), 14 usaProfundidad]
jlong createPipeline(const std::string& name, const std::string& vsSrc, const std::string& psSrc,
		const std::vector<jint>& lay, const std::vector<jint>& st) {
	if (!g.ready) { g.lastError = "DX12 no esta listo"; return 0; }
	if (lay.size() < 2 || st.size() < 15) { g.lastError = "createPipeline: arrays incompletos"; return 0; }
	const int n = lay[1];
	if (n < 0 || lay.size() < (size_t)(2 + n * 4)) { g.lastError = "createPipeline: layout incompleto"; return 0; }

	ComPtr<ID3DBlob> vs = compileHlsl(vsSrc, "vs_5_0", name + ".vs");
	if (!vs) return 0;
	ComPtr<ID3DBlob> ps = compileHlsl(psSrc, "ps_5_0", name + ".fs");
	if (!ps) return 0;

	std::vector<D3D12_INPUT_ELEMENT_DESC> elems;
	for (int i = 0; i < n; ++i) {
		const DXGI_FORMAT f = vtxFormat(lay[2 + i * 4], lay[3 + i * 4], lay[4 + i * 4] != 0);
		if (f == DXGI_FORMAT_UNKNOWN) { g.lastError = "createPipeline: formato de vertice no soportado (" + name + ")"; return 0; }
		D3D12_INPUT_ELEMENT_DESC e{};
		e.SemanticName = "TEXCOORD";
		e.SemanticIndex = (UINT)i;
		e.Format = f;
		e.InputSlot = 0;
		e.AlignedByteOffset = (UINT)lay[5 + i * 4];
		e.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
		e.InstanceDataStepRate = 0;
		elems.push_back(e);
	}

	float biasF = 0.0f, slopeF = 0.0f;
	{
		jint b0 = st[12], b1 = st[13];
		std::memcpy(&biasF, &b0, sizeof(float));
		std::memcpy(&slopeF, &b1, sizeof(float));
	}

	// diagnostico (paso 9b): layout "sintetico" generado desde la firma de entrada del VS (siempre valido para ese shader) y volcado del layout real
	std::vector<std::string> synthNames;
	std::vector<D3D12_INPUT_ELEMENT_DESC> synthElems;
	{
		ComPtr<ID3D11ShaderReflection> rf;
		if (SUCCEEDED(D3DReflect(vs->GetBufferPointer(), vs->GetBufferSize(), __uuidof(ID3D11ShaderReflection), (void**)&rf))) {
			D3D11_SHADER_DESC sd{};
			rf->GetDesc(&sd);
			synthNames.reserve(sd.InputParameters);
			UINT off = 0;
			for (UINT i = 0; i < sd.InputParameters; ++i) {
				D3D11_SIGNATURE_PARAMETER_DESC sp{};
				rf->GetInputParameterDesc(i, &sp);
				if (sp.SystemValueType != D3D_NAME_UNDEFINED) continue;
				int comps = 1;
				for (int b = 0; b < 4; ++b) if (sp.Mask & (1u << b)) comps = b + 1;
				DXGI_FORMAT f;
				if (sp.ComponentType == D3D_REGISTER_COMPONENT_UINT32)
					f = comps == 1 ? DXGI_FORMAT_R32_UINT : comps == 2 ? DXGI_FORMAT_R32G32_UINT : comps == 3 ? DXGI_FORMAT_R32G32B32_UINT : DXGI_FORMAT_R32G32B32A32_UINT;
				else if (sp.ComponentType == D3D_REGISTER_COMPONENT_SINT32)
					f = comps == 1 ? DXGI_FORMAT_R32_SINT : comps == 2 ? DXGI_FORMAT_R32G32_SINT : comps == 3 ? DXGI_FORMAT_R32G32B32_SINT : DXGI_FORMAT_R32G32B32A32_SINT;
				else
					f = comps == 1 ? DXGI_FORMAT_R32_FLOAT : comps == 2 ? DXGI_FORMAT_R32G32_FLOAT : comps == 3 ? DXGI_FORMAT_R32G32B32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
				synthNames.push_back(sp.SemanticName);
				D3D12_INPUT_ELEMENT_DESC e{};
				e.SemanticName = synthNames.back().c_str();
				e.SemanticIndex = sp.SemanticIndex;
				e.Format = f;
				e.AlignedByteOffset = off;
				e.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
				synthElems.push_back(e);
				off += 16;
			}
		}
	}
	auto layoutDump = [&]() {
		std::string d = " | layoutReal(stride=" + std::to_string(lay[0]) + "):";
		char b[160];
		for (int i = 0; i < n; ++i) {
			const int t = lay[2 + i * 4], c = lay[3 + i * 4], nm = lay[4 + i * 4], o = lay[5 + i * 4];
			const int cs = (t == 1 || t == 2 || t == 7) ? 1 : (t == 3 || t == 4) ? 2 : 4;
			snprintf(b, sizeof(b), " [%d]tipo%d x%d norm%d off%d fmt%d%s", i, t, c, nm, o, (int)elems[i].Format, (o % cs) ? " DESALINEADO" : "");
			d += b;
		}
		return d;
	};

	Pso out;
	out.stride = (UINT)lay[0];
	out.topo = (D3D_PRIMITIVE_TOPOLOGY)st[11];
	out.wantsDepth = st[14] != 0;

	// Variantes solo para diagnostico: si el PSO falla, se prueba quitando una parte cada vez para saber cual lo invalida.
	struct Variant { bool bias = true, blend = true, layout = true, cull = true, synth = false; };
	auto build = [&](bool withDepth, const Variant& v, ComPtr<ID3D12PipelineState>& dst, bool report) -> bool {
		D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
		d.pRootSignature = g.rootSig.Get();
		d.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
		d.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
		D3D12_RENDER_TARGET_BLEND_DESC& rt = d.BlendState.RenderTarget[0];
		defaultBlend(rt);
		if (st[3] && v.blend) {
			rt.BlendEnable = TRUE;
			rt.SrcBlend = (D3D12_BLEND)st[4];
			rt.DestBlend = (D3D12_BLEND)st[5];
			rt.SrcBlendAlpha = (D3D12_BLEND)st[6];
			rt.DestBlendAlpha = (D3D12_BLEND)st[7];
		}
		rt.RenderTargetWriteMask = (UINT8)st[8];
		d.SampleMask = UINT_MAX;
		d.RasterizerState.FillMode = st[9] ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
		d.RasterizerState.CullMode = !v.cull ? D3D12_CULL_MODE_NONE : st[2] == 1 ? D3D12_CULL_MODE_BACK : st[2] == 2 ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_NONE;
		d.RasterizerState.FrontCounterClockwise = FALSE;  // el mod voltea Y en el shader: la cara frontal de GL queda en sentido horario
		d.RasterizerState.DepthBias = (withDepth && v.bias) ? (INT)(biasF >= 0.0f ? biasF + 0.5f : biasF - 0.5f) : 0;
		d.RasterizerState.DepthBiasClamp = 0.0f;
		d.RasterizerState.SlopeScaledDepthBias = (withDepth && v.bias) ? slopeF : 0.0f;
		d.RasterizerState.DepthClipEnable = TRUE;
		d.DepthStencilState.DepthEnable = (withDepth && st[0] != 0) ? TRUE : FALSE;
		d.DepthStencilState.DepthWriteMask = (withDepth && st[0] != 0 && st[1]) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
		d.DepthStencilState.DepthFunc = depthFunc(st[0]);
		d.DepthStencilState.StencilEnable = FALSE;
		if (v.synth) d.InputLayout = { synthElems.data(), (UINT)synthElems.size() };
		else if (v.layout && !elems.empty()) d.InputLayout = { elems.data(), (UINT)elems.size() };
		else d.InputLayout = { nullptr, 0 };
		d.PrimitiveTopologyType = (D3D12_PRIMITIVE_TOPOLOGY_TYPE)st[10];
		d.NumRenderTargets = 1;
		d.RTVFormats[0] = kFormat;
		d.DSVFormat = withDepth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
		d.SampleDesc.Count = 1;
		HRESULT hr = g.dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&dst));
		if (FAILED(hr)) {
			if (report) {
				fail(("CreateGraphicsPipelineState(" + name + ")").c_str(), hr);
				g.lastError += describeShader(vs.Get(), "VS") + describeShader(ps.Get(), "PS");
			}
			return false;
		}
		return true;
	};
	const Variant full{};
	if (!build(false, full, out.noDepth, true)) {
		std::string diag = " | variantes:";
		ComPtr<ID3D12PipelineState> tmp;
		Variant v = full; v.blend = false;  diag += std::string(" sinBlend=")  + (build(false, v, tmp, false) ? "OK" : "FALLO"); tmp.Reset();
		v = full; v.layout = false;         diag += std::string(" sinLayout=") + (build(false, v, tmp, false) ? "OK" : "FALLO"); tmp.Reset();
		v = full; v.cull = false;           diag += std::string(" sinCull=")   + (build(false, v, tmp, false) ? "OK" : "FALLO"); tmp.Reset();
		v = full; v.synth = true;           diag += std::string(" layoutSintetico=") + (build(false, v, tmp, false) ? "OK" : "FALLO"); tmp.Reset();
		v = full; v.synth = true; v.blend = false; v.cull = false;
		diag += std::string(" sinteticoSinBlendSinCull=") + (build(false, v, tmp, false) ? "OK" : "FALLO"); tmp.Reset();
		g.lastError += diag + layoutDump();
		return 0;
	}
	if (out.wantsDepth && !build(true, full, out.withDepth, true)) {
		std::string diag = " | variantes(depth):";
		ComPtr<ID3D12PipelineState> tmp;
		Variant v = full; v.bias = false; diag += std::string(" sinSesgo=") + (build(true, v, tmp, false) ? "OK" : "FALLO"); tmp.Reset();
		g.lastError += diag;
		return 0;
	}

	const jlong id = g.nextPso++;
	g.pso.emplace(id, std::move(out));
	return id;
}

void destroyPipeline(jlong id) {
	g.lastPso = nullptr;
	g.lastPsoId = 0;
	auto it = g.pso.find(id);
	if (it == g.pso.end()) return;
	if (it->second.withDepth) g.psoTrash.push_back(it->second.withDepth);
	if (it->second.noDepth) g.psoTrash.push_back(it->second.noDepth);
	g.pso.erase(it);
}

struct DrawParams {
	jlong pso = 0, color = 0, depth = 0;
	const unsigned char* vb = nullptr;
	size_t vbSize = 0;
	const unsigned char* ib = nullptr;  // base del buffer de indices
	size_t ibSize = 0;                  // tamano total del buffer de indices
	size_t ibOff = 0;                   // desplazamiento en bytes del primer indice
	jlong vbId = 0, vbVer = 0, ibId = 0, ibVer = 0;  // id/version para residencia en GPU (0 = no)
	int idxBytes = 0;  // 0 = sin indices
	int baseVertex = 0, count = 0, instances = 1;
	bool scissor = false;
	int sx = 0, sy = 0, sw = 0, sh = 0;
	const jlong* ub = nullptr;   // kCbSlots * (direccion, tamano)
	const jlong* tex = nullptr;  // kTexSlots * (handle, esCubo)
	const jint* tp = nullptr;    // kTexSlots * (baseMip, mips, minF, magF, addrU, addrV, aniso, maxLod bits)
};

// Clase de tamano: multiplos de 64 KB hasta 64 KB; luego 16 pasos por octava (maximo ~6% de desperdicio).
static size_t bufClass(size_t n) {
	const size_t kMin = 64 * 1024;
	if (n <= kMin) return kMin;
	size_t p = kMin;
	while (p < n) p <<= 1;
	const size_t step = p / 16;
	return (n + step - 1) / step * step;
}

constexpr D3D12_RESOURCE_STATES kBufRead = (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER);

// paso 8: crea un buffer DEFAULT de 'cap' bytes. Primero intenta cortarlo de un heap grande (barato); si no hay sitio o falla,
// cae al CreateCommittedResource de siempre. 'placed' dice cual de los dos se uso.
ComPtr<ID3D12Resource> createGpuBuffer(size_t cap, bool& placed) {
	placed = false;
	D3D12_RESOURCE_DESC bd{};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = cap;
	bd.Height = 1;
	bd.DepthOrArraySize = 1;
	bd.MipLevels = 1;
	bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	const size_t kPage = gHeapPage;           // tamano de cada pagina
	const size_t kHeapBudget = gHeapBudget;  // tope total de paginas (despues se usa el camino committed)
	const size_t kAlign = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;  // 64 KB
	const size_t alloc = (cap + kAlign - 1) / kAlign * kAlign;
	if (alloc <= kPage / 4) {
		HeapPage* page = nullptr;
		for (auto& hp : g.bufHeaps) {
			if (hp.size - hp.used >= alloc) { page = &hp; break; }
		}
		if (!page && g.bufHeapBytes + kPage <= kHeapBudget) {
			D3D12_HEAP_DESC hd{};
			hd.SizeInBytes = kPage;
			hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
			hd.Alignment = kAlign;
			hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
			HeapPage np;
			if (SUCCEEDED(g.dev->CreateHeap(&hd, IID_PPV_ARGS(&np.heap)))) {
				np.size = kPage;
				g.bufHeaps.push_back(std::move(np));
				g.bufHeapBytes += kPage;
				page = &g.bufHeaps.back();
			}
		}
		if (page) {
			ComPtr<ID3D12Resource> r;
			if (SUCCEEDED(g.dev->CreatePlacedResource(page->heap.Get(), page->used, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r)))) {
				page->used += alloc;
				placed = true;
				g.pfPlaced++;
				return r;
			}
		}
	}
	D3D12_HEAP_PROPERTIES hp{};
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;
	ComPtr<ID3D12Resource> r;
	if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
	return r;
}

// Garantiza una copia en GPU actualizada de un buffer del juego. nullptr = usar el camino del anillo.
GBuf* residentBuffer(jlong id, const unsigned char* src, size_t size, jlong version) {
	if (!g.ready || !id || !src || !size) return nullptr;
	GBuf& b = g.gbuf[id];
	if (b.failed) return nullptr;
	if (b.res && b.size == size && b.version == version) return &b;

	const long long tStart = qpc();
	const size_t cap = bufClass(size);
	if (!b.res || b.cap != cap) {
		if (b.res) {
			PooledBuf old;
			old.res = b.res;
			old.cap = b.cap;
			old.state = b.state;
			old.placed = b.placed;
			g.gbufRetire.push_back(std::move(old));
			b.res.Reset();
		}
		auto pit = g.gbufPool.find(cap);
		if (pit != g.gbufPool.end() && !pit->second.empty()) {
			PooledBuf pb = std::move(pit->second.back());
			pit->second.pop_back();
			g.gbufPoolBytes -= pb.cap;
			b.res = pb.res;
			b.state = pb.state;
			b.placed = pb.placed;
			g.pfResReused++;
		} else {
			const long long tc = qpc();
			bool placed = false;
			b.res = createGpuBuffer(cap, placed);
			g.pfCreateTime += qpc() - tc;
			if (!b.res) {
				b.failed = true;  // sin memoria de video: este buffer sigue por el anillo
				return nullptr;
			}
			b.placed = placed;
			b.state = D3D12_RESOURCE_STATE_COPY_DEST;
			g.pfResCreated++;
		}
		b.cap = cap;
	}
	b.size = size;
	if (b.state != D3D12_RESOURCE_STATE_COPY_DEST) {
		barrier(b.res.Get(), b.state, D3D12_RESOURCE_STATE_COPY_DEST);
		b.state = D3D12_RESOURCE_STATE_COPY_DEST;
	}

	const size_t aligned = (size + 255) & ~(size_t)255;
	if (aligned + 4096 <= kUploadRing - kZeroCb) {
		if (g.ringOff + aligned + 256 > kUploadRing) {
			if (!submit(false)) return nullptr;
			// submit vacio la lista: el barrier/copia pendiente de arriba ya se ejecuto o se descarto; reinicia el estado
			if (b.state != D3D12_RESOURCE_STATE_COPY_DEST) {
				barrier(b.res.Get(), b.state, D3D12_RESOURCE_STATE_COPY_DEST);
				b.state = D3D12_RESOURCE_STATE_COPY_DEST;
			}
		}
		const size_t o = (g.ringOff + 255) & ~(size_t)255;
		g.ringOff = o + aligned;
		std::memcpy(g.ringPtr + o, src, size);
		g.list->CopyBufferRegion(b.res.Get(), 0, g.ring.Get(), o, size);
	} else {
		// buffer enorme: subida temporal propia
		D3D12_HEAP_PROPERTIES up{};
		up.Type = D3D12_HEAP_TYPE_UPLOAD;
		D3D12_RESOURCE_DESC ud{};
		ud.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		ud.Width = size;
		ud.Height = 1;
		ud.DepthOrArraySize = 1;
		ud.MipLevels = 1;
		ud.SampleDesc.Count = 1;
		ud.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ComPtr<ID3D12Resource> tmp;
		if (FAILED(g.dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &ud, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&tmp)))) {
			b.failed = true;
			return nullptr;
		}
		void* m = nullptr;
		D3D12_RANGE none{ 0, 0 };
		if (FAILED(tmp->Map(0, &none, &m)) || !m) { b.failed = true; return nullptr; }
		std::memcpy(m, src, size);
		tmp->Unmap(0, nullptr);
		g.list->CopyBufferRegion(b.res.Get(), 0, tmp.Get(), 0, size);
		g.trash.push_back(tmp);
	}
	barrier(b.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kBufRead);
	b.state = kBufRead;
	b.version = version;
	g.pfResUploads++;
	g.pfResBytes += size;
	g.pfResTime += qpc() - tStart;
	return &b;
}

void destroyBuffer(jlong id) {
	auto it = g.gbuf.find(id);
	if (it == g.gbuf.end()) return;
	if (it->second.res) {
		PooledBuf old;
		old.res = it->second.res;
		old.cap = it->second.cap;
		old.state = it->second.state;
		old.placed = it->second.placed;
		g.gbufRetire.push_back(std::move(old));
	}
	g.gbuf.erase(it);
}

static void drawImpl(const DrawParams& p);

void draw(const DrawParams& p) {
	drawImpl(p);
	g.pfDraws++;
}

constexpr D3D12_RESOURCE_STATES kSrvStates = (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

static void drawImpl(const DrawParams& p) {
	if (!g.ready || p.pso == 0) return;
	Pso* psp = nullptr;
	if (g.lastPso && g.lastPsoId == p.pso) {
		psp = g.lastPso;
	} else {
		auto pit = g.pso.find(p.pso);
		if (pit == g.pso.end()) { skip("pipeline nativo desconocido"); return; }
		psp = &pit->second;
		g.lastPso = psp;
		g.lastPsoId = p.pso;
	}
	Pso& ps = *psp;
	const int cmip = mipOf(p.color);
	Tex* ctp = findTex(idOf(p.color));
	if (!ctp) { skip("textura de color inexistente"); return; }
	Tex& ct = *ctp;
	if (ct.depth || ct.fmt != kFormat) { skip("destino de color no es RGBA8"); return; }
	if (p.count <= 0 || p.instances <= 0) return;
	const UINT cw = std::max(1u, ct.w >> cmip), chh = std::max(1u, ct.h >> cmip);

	Tex* dt = nullptr;
	if (ps.wantsDepth && p.depth) {
		Tex* d = findTex(p.depth);
		if (d && d->depth) dt = d;
	}

	// recorte
	D3D12_RECT sr{ 0, 0, (LONG)cw, (LONG)chh };
	if (p.scissor) {
		sr.left = std::max<LONG>(0, p.sx);
		sr.top = std::max<LONG>(0, p.sy);
		sr.right = std::min<LONG>((LONG)cw, (LONG)p.sx + p.sw);
		sr.bottom = std::min<LONG>((LONG)chh, (LONG)p.sy + p.sh);
	}
	if (sr.right <= sr.left || sr.bottom <= sr.top) return;  // GL tampoco dibuja nada

	// rango de vertices realmente usado (solo hace falta si el buffer no es residente: se copia solo eso al anillo)
	const bool hasVb = p.vb && p.vbSize && ps.stride;
	const unsigned char* ibp = p.ib ? p.ib + p.ibOff : nullptr;
	GBuf* gv = (hasVb && p.vbId) ? residentBuffer(p.vbId, p.vb, p.vbSize, p.vbVer) : nullptr;
	GBuf* gi = (p.idxBytes && p.ibId && p.ib) ? residentBuffer(p.ibId, p.ib, p.ibSize, p.ibVer) : nullptr;
	size_t vOff = 0, vLen = 0, iLen = 0;
	if (p.idxBytes) {
		iLen = (size_t)p.count * (size_t)p.idxBytes;
		if (!p.ib || p.ibSize < p.ibOff + iLen) { skip("draw indexado sin indices suficientes"); return; }
		vOff = (size_t)std::max(p.baseVertex, 0) * ps.stride;
		if (!gv) {
			uint32_t mx = 0;
			if (p.idxBytes == 2) {
				const uint16_t* q = (const uint16_t*)ibp;
				for (int i = 0; i < p.count; ++i) mx = std::max<uint32_t>(mx, q[i]);
			} else {
				const uint32_t* q = (const uint32_t*)ibp;
				for (int i = 0; i < p.count; ++i) mx = std::max<uint32_t>(mx, q[i]);
			}
			vLen = ((size_t)mx + 1) * ps.stride;
		}
	} else {
		vOff = (size_t)std::max(p.baseVertex, 0) * ps.stride;
		vLen = (size_t)p.count * ps.stride;
	}
	if (hasVb) {
		if (vOff >= p.vbSize) { skip("vertices fuera del buffer"); return; }
		if (vOff + vLen > p.vbSize) vLen = p.vbSize - vOff;
	}

	// espacio necesario en el anillo
	size_t need = 4096;
	if (hasVb && !gv) need += (vLen + 255) & ~(size_t)255;
	if (p.idxBytes && !gi) need += (iLen + 255) & ~(size_t)255;
	for (UINT i = 0; i < kCbSlots; ++i) {
		if (p.ub[i * 2] && p.ub[i * 2 + 1] > 0) need += ((size_t)p.ub[i * 2 + 1] + 255) & ~(size_t)255;
	}
	if (need > kUploadRing - kZeroCb) { skip("draw demasiado grande para el anillo de subida"); return; }
	if (g.ringOff + need > kUploadRing || g.srvNext + kTexSlots > kSrvRing || g.rtvNext >= kRtvRing || g.dsvNext >= kDsvRing) {
		if (!submit(false)) return;
	}
	if (gv || gi) g.pfResDraws++;

	StateCache& c = g.sc;  // (un submit() lo reinicia, por eso se usa siempre por referencia a g.sc)

	transition(ct, D3D12_RESOURCE_STATE_RENDER_TARGET);
	if (dt) transition(*dt, D3D12_RESOURCE_STATE_DEPTH_WRITE);

	// Texturas: transiciones + claves para reutilizar las tablas de descriptores ya creadas (SRV por frame, samplers para siempre).
	K16 sk;
	K8 pk;
	std::memset(&sk, 0, sizeof(sk));
	std::memset(&pk, 0, sizeof(pk));
	Tex* tptr[kTexSlots] = {};
	for (UINT i = 0; i < kTexSlots; ++i) {
		const jlong h = p.tex[i * 2];
		if (!h) continue;
		Tex* t = findTex(h);
		if (!t || t == &ct || t == dt) continue;  // leer y escribir la misma textura no es valido
		transition(*t, kSrvStates);
		const jint* q = p.tp + i * 8;
		tptr[i] = t;
		sk.w[i * 2] = (uint64_t)h;
		sk.w[i * 2 + 1] = (uint64_t)(uint32_t)q[0] | ((uint64_t)(uint32_t)q[1] << 16) | ((p.tex[i * 2 + 1] != 0 ? 1ull : 0ull) << 40);
		pk.w[i] = 0x8000000000000000ull | (uint64_t)(q[2] & 1) | ((uint64_t)(q[3] & 1) << 1) | ((uint64_t)(q[4] & 3) << 2) |
			((uint64_t)(q[5] & 3) << 4) | ((uint64_t)(q[6] & 0xFF) << 8) | ((uint64_t)(uint32_t)q[7] << 16);
	}

	UINT64 srvGpu = 0;
	if (c.srvKeyValid && c.srvKey == sk) {
		srvGpu = c.srvKeyGpu;
	} else {
		auto it = g.srvTables.find(sk);
		if (it != g.srvTables.end()) {
			srvGpu = it->second;
		} else {
			const UINT base = g.srvNext;
			g.srvNext += kTexSlots;
			D3D12_CPU_DESCRIPTOR_HANDLE cpu = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
			cpu.ptr += (SIZE_T)base * g.srvInc;
			for (UINT i = 0; i < kTexSlots; ++i) {
				TexBind b;
				if (tptr[i]) {
					const jint* q = p.tp + i * 8;
					b.t = tptr[i];
					b.cube = p.tex[i * 2 + 1] != 0;
					b.baseMip = q[0];
					b.mipCount = q[1];
				}
				makeSrv(cpu, b);
				cpu.ptr += g.srvInc;
			}
			D3D12_GPU_DESCRIPTOR_HANDLE gpu = g.srvHeap->GetGPUDescriptorHandleForHeapStart();
			gpu.ptr += (UINT64)base * g.srvInc;
			srvGpu = gpu.ptr;
			g.srvTables.emplace(sk, srvGpu);
		}
		c.srvKey = sk;
		c.srvKeyGpu = srvGpu;
		c.srvKeyValid = true;
	}

	UINT64 sampGpu = 0;
	if (c.sampKeyValid && c.sampKey == pk) {
		sampGpu = c.sampKeyGpu;
	} else {
		auto it = g.sampTables.find(pk);
		if (it != g.sampTables.end()) {
			sampGpu = it->second;
		} else {
			UINT idx[8] = {};
			for (UINT i = 0; i < kTexSlots; ++i) {
				if (!tptr[i]) continue;
				const jint* q = p.tp + i * 8;
				float lod = -1.0f;
				std::memcpy(&lod, &q[7], sizeof(float));
				idx[i] = getSampler(q[2], q[3], q[4], q[5], q[6], lod);
			}
			sampGpu = addSampTable(pk, idx);
		}
		c.sampKey = pk;
		c.sampKeyGpu = sampGpu;
		c.sampKeyValid = true;
	}

	// vistas del destino: solo se crean (y se fijan) cuando cambia el destino
	const jlong dKey = dt ? p.depth : 0;
	bool needTargets = false;
	if (!c.targetsValid || c.colorKey != p.color || c.depthKey != dKey) {
		D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.clearRtvHeap->GetCPUDescriptorHandleForHeapStart();
		rtv.ptr += (SIZE_T)g.rtvNext++ * g.rtvSize;
		makeRtv(ct, cmip, rtv);
		D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
		if (dt) {
			dsv = g.dsvHeap->GetCPUDescriptorHandleForHeapStart();
			dsv.ptr += (SIZE_T)g.dsvNext++ * g.dsvSize;
			D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
			dv.Format = DXGI_FORMAT_D32_FLOAT;
			dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
			g.dev->CreateDepthStencilView(dt->res.Get(), &dv, dsv);
		}
		c.rtv = rtv;
		c.dsv = dsv;
		c.hasDsv = dt != nullptr;
		c.colorKey = p.color;
		c.depthKey = dKey;
		c.targetsValid = true;
		needTargets = true;
	}

	const D3D12_GPU_VIRTUAL_ADDRESS base = g.ring->GetGPUVirtualAddress();
	auto alloc = [&](size_t n) -> size_t {
		const size_t o = (g.ringOff + 255) & ~(size_t)255;
		g.ringOff = o + n;
		return o;
	};
	D3D12_GPU_VIRTUAL_ADDRESS cb[kCbSlots];
	for (UINT i = 0; i < kCbSlots; ++i) {
		const jlong addr = p.ub[i * 2];
		const jlong len = p.ub[i * 2 + 1];
		if (addr && len > 0) {
			const unsigned char* srcb = (const unsigned char*)(intptr_t)addr;
			const size_t L = (size_t)len;
			if (L <= kShadowMax && c.shadowLen[i] == L && c.cbAddr[i] && std::memcmp(c.shadow[i], srcb, L) == 0) {
				cb[i] = c.cbAddr[i];  // mismo contenido que el ultimo draw: se reutiliza la copia ya subida
			} else {
				const size_t o = alloc(((size_t)len + 255) & ~(size_t)255);
				std::memcpy(g.ringPtr + o, srcb, L);
				g.pfRingBytes += L;
				cb[i] = base + o;
				if (L <= kShadowMax) {
					std::memcpy(c.shadow[i], srcb, L);
					c.shadowLen[i] = (UINT)L;
					c.cbAddr[i] = cb[i];
				} else {
					c.shadowLen[i] = 0;
				}
			}
		} else {
			cb[i] = base;  // bloque de ceros
		}
	}
	D3D12_VERTEX_BUFFER_VIEW vbv{};
	if (hasVb && gv) {
		vbv.BufferLocation = gv->res->GetGPUVirtualAddress() + vOff;
		vbv.SizeInBytes = (UINT)(gv->size - vOff);
		vbv.StrideInBytes = ps.stride;
	} else if (hasVb) {
		const size_t o = alloc(vLen);
		std::memcpy(g.ringPtr + o, p.vb + vOff, vLen);
		g.pfRingBytes += vLen;
		vbv.BufferLocation = base + o;
		vbv.SizeInBytes = (UINT)vLen;
		vbv.StrideInBytes = ps.stride;
	}
	D3D12_INDEX_BUFFER_VIEW ibv{};
	const bool hasIb = p.idxBytes != 0;
	if (hasIb && gi) {
		ibv.BufferLocation = gi->res->GetGPUVirtualAddress() + p.ibOff;
		ibv.SizeInBytes = (UINT)(gi->size - p.ibOff);
		ibv.Format = p.idxBytes == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
	} else if (hasIb) {
		const size_t o = alloc(iLen);
		std::memcpy(g.ringPtr + o, ibp, iLen);
		g.pfRingBytes += iLen;
		ibv.BufferLocation = base + o;
		ibv.SizeInBytes = (UINT)iLen;
		ibv.Format = p.idxBytes == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
	}

	ID3D12GraphicsCommandList* L = g.list.Get();
	ensureRoot();
	for (UINT i = 0; i < kCbSlots; ++i) {
		if (cb[i] != c.cb[i]) { L->SetGraphicsRootConstantBufferView(i, cb[i]); c.cb[i] = cb[i]; }
	}
	if (srvGpu != c.srvGpu) {
		D3D12_GPU_DESCRIPTOR_HANDLE hnd{ srvGpu };
		L->SetGraphicsRootDescriptorTable(kCbSlots, hnd);
		c.srvGpu = srvGpu;
	}
	if (sampGpu != c.sampGpu) {
		D3D12_GPU_DESCRIPTOR_HANDLE hnd{ sampGpu };
		L->SetGraphicsRootDescriptorTable(kCbSlots + 1, hnd);
		c.sampGpu = sampGpu;
	}
	ID3D12PipelineState* pso = dt ? ps.withDepth.Get() : ps.noDepth.Get();
	if (pso != c.pso) { L->SetPipelineState(pso); c.pso = pso; }
	if (needTargets) {
		L->OMSetRenderTargets(1, &c.rtv, FALSE, c.hasDsv ? &c.dsv : nullptr);
		D3D12_VIEWPORT vp{ 0.0f, 0.0f, (float)cw, (float)chh, 0.0f, 1.0f };
		L->RSSetViewports(1, &vp);
	}
	if (!c.srValid || std::memcmp(&c.sr, &sr, sizeof(sr)) != 0) {
		L->RSSetScissorRects(1, &sr);
		c.sr = sr;
		c.srValid = true;
	}
	if (!c.blendSet) {
		const float bf[4] = { 0, 0, 0, 0 };
		L->OMSetBlendFactor(bf);
		c.blendSet = true;
	}
	if (ps.topo != c.topo) { L->IASetPrimitiveTopology(ps.topo); c.topo = ps.topo; }
	if (hasVb && (vbv.BufferLocation != c.vbLoc || vbv.SizeInBytes != c.vbSize || vbv.StrideInBytes != c.vbStride)) {
		L->IASetVertexBuffers(0, 1, &vbv);
		c.vbLoc = vbv.BufferLocation; c.vbSize = vbv.SizeInBytes; c.vbStride = vbv.StrideInBytes;
	}
	if (hasIb) {
		if (ibv.BufferLocation != c.ibLoc || ibv.SizeInBytes != c.ibSize || ibv.Format != c.ibFmt) {
			L->IASetIndexBuffer(&ibv);
			c.ibLoc = ibv.BufferLocation; c.ibSize = ibv.SizeInBytes; c.ibFmt = ibv.Format;
		}
		L->DrawIndexedInstanced((UINT)p.count, (UINT)p.instances, 0, 0, 0);
	} else {
		L->DrawInstanced((UINT)p.count, (UINT)p.instances, 0, 0);
	}
	g.nDraws++;
}

// ---------- diagnostico y copias ----------

// Lee una textura RGBA8/R8 (un mip) de vuelta a la CPU y resume su contenido. Solo para depurar: vacia la lista y espera a la GPU.
std::string textureStats(jlong id, int mip) {
	if (!g.ready) return "no listo";
	auto it = g.tex.find(id);
	if (it == g.tex.end()) return "textura desconocida";
	Tex& t = it->second;
	if (t.depth) return "textura de profundidad";
	if (mip < 0 || (UINT)mip >= t.mips) return "mip fuera de rango";
	const UINT mw = std::max(1u, t.w >> mip), mh = std::max(1u, t.h >> mip);
	D3D12_RESOURCE_DESC rd = t.res->GetDesc();
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
	UINT rows = 0;
	UINT64 rowSize = 0, total = 0;
	g.dev->GetCopyableFootprints(&rd, (UINT)mip, 1, 0, &fp, &rows, &rowSize, &total);
	D3D12_HEAP_PROPERTIES hp{};
	hp.Type = D3D12_HEAP_TYPE_READBACK;
	D3D12_RESOURCE_DESC bd{};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = total;
	bd.Height = 1;
	bd.DepthOrArraySize = 1;
	bd.MipLevels = 1;
	bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ComPtr<ID3D12Resource> rb;
	if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)))) return "no se pudo crear el buffer de lectura";
	transition(t, D3D12_RESOURCE_STATE_COPY_SOURCE);
	D3D12_TEXTURE_COPY_LOCATION dl{}, sl{};
	dl.pResource = rb.Get();
	dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dl.PlacedFootprint = fp;
	sl.pResource = t.res.Get();
	sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	sl.SubresourceIndex = (UINT)mip;
	g.list->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
	if (!submit(false)) return "submit fallo";
	unsigned char* data = nullptr;
	D3D12_RANGE rr{ 0, (SIZE_T)total };
	if (FAILED(rb->Map(0, &rr, (void**)&data)) || !data) return "Map fallo";
	unsigned long long px = (unsigned long long)mw * mh, nz = 0, opaque = 0, sr = 0, sg = 0, sb = 0;
	unsigned maxA = 0;
	for (UINT y = 0; y < mh; ++y) {
		const unsigned char* row = data + (size_t)y * fp.Footprint.RowPitch;
		for (UINT x = 0; x < mw; ++x) {
			if (t.bpp == 4) {
				const unsigned char* q = row + (size_t)x * 4;
				if (q[0] | q[1] | q[2] | q[3]) ++nz;
				if (q[3]) { ++opaque; sr += q[0]; sg += q[1]; sb += q[2]; if (q[3] > maxA) maxA = q[3]; }
			} else if (row[x]) {
				++nz;
			}
		}
	}
	D3D12_RANGE none{ 0, 0 };
	rb->Unmap(0, &none);
	char buf[256];
	if (t.bpp == 4) {
		snprintf(buf, sizeof(buf), "mip%d %ux%u: pixeles no nulos=%llu/%llu | con alfa>0=%llu | alfa max=%u | rgb medio (alfa>0)=(%llu,%llu,%llu)",
			mip, mw, mh, nz, px, opaque, maxA, opaque ? sr / opaque : 0ull, opaque ? sg / opaque : 0ull, opaque ? sb / opaque : 0ull);
	} else {
		snprintf(buf, sizeof(buf), "mip%d %ux%u: pixeles no nulos=%llu/%llu", mip, mw, mh, nz, px);
	}
	return buf;
}

// Copia un rectangulo entre dos texturas del mismo formato (CommandEncoder.copyTextureToTexture).
void copyTexture(jlong from, jlong to, int mip, int dx, int dy, int sx, int sy, int w, int h) {
	if (!g.ready || from == to) return;
	auto a = g.tex.find(from), b = g.tex.find(to);
	if (a == g.tex.end() || b == g.tex.end()) return;
	Tex& s = a->second;
	Tex& d = b->second;
	if (s.fmt != d.fmt || mip < 0 || (UINT)mip >= s.mips || (UINT)mip >= d.mips) return;
	const int smw = (int)std::max(1u, s.w >> mip), smh = (int)std::max(1u, s.h >> mip);
	const int dmw = (int)std::max(1u, d.w >> mip), dmh = (int)std::max(1u, d.h >> mip);
	if (sx < 0 || sy < 0 || dx < 0 || dy < 0) return;
	w = std::min(w, std::min(smw - sx, dmw - dx));
	h = std::min(h, std::min(smh - sy, dmh - dy));
	if (w <= 0 || h <= 0) return;
	const bool full = sx == 0 && sy == 0 && dx == 0 && dy == 0 && w == smw && h == smh && smw == dmw && smh == dmh;
	if (s.depth && !full) return;  // D3D12 solo copia profundidad completa
	transition(s, D3D12_RESOURCE_STATE_COPY_SOURCE);
	transition(d, D3D12_RESOURCE_STATE_COPY_DEST);
	D3D12_TEXTURE_COPY_LOCATION sl{}, dl{};
	sl.pResource = s.res.Get();
	sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	sl.SubresourceIndex = (UINT)mip;
	dl.pResource = d.res.Get();
	dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dl.SubresourceIndex = (UINT)mip;
	if (full) {
		g.list->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
	} else {
		D3D12_BOX box{ (UINT)sx, (UINT)sy, 0, (UINT)(sx + w), (UINT)(sy + h), 1 };
		g.list->CopyTextureRegion(&dl, (UINT)dx, (UINT)dy, 0, &sl, &box);
	}
}

// ---------- fin de frame ----------

void flip(float fr, float fg, float fb) {
	if (!g.ready) return;
	const long long tf0 = qpc(), wf0 = g.pfWait, pf0 = g.pfPresent;
	UINT w = 0, h = 0;
	if (!getClientSize(w, h) || w == 0 || h == 0) {  // minimizada: ejecutar lo pendiente y no presentar
		g.pendingPresent = 0;
		submit(false);
		return;
	}
	if (w != g.width || h != g.height) {
		if (!resizeSwapchain(w, h)) { g.ready = false; return; }
	}

	Tex* src = findTex(g.pendingPresent);
	if (src && (src->depth || src->fmt != kFormat)) src = nullptr;
	if (src && g.srvNext + kTexSlots > kSrvRing) submit(false);

	const UINT idx = g.swap->GetCurrentBackBufferIndex();
	ID3D12Resource* bb = g.backbuf[idx].Get();
	D3D12_RESOURCE_STATES cur = D3D12_RESOURCE_STATE_PRESENT;
	auto to = [&](D3D12_RESOURCE_STATES s2) {
		if (cur != s2) { barrier(bb, cur, s2); cur = s2; }
	};

	D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
	rtv.ptr += (SIZE_T)idx * g.rtvSize;
	to(D3D12_RESOURCE_STATE_RENDER_TARGET);

	if (!src) {  // sin textura que presentar: color de diagnostico
		const float c[4] = { fr, fg, fb, 1.0f };
		g.list->ClearRenderTargetView(rtv, c, 0, nullptr);
		g.nFallbackFrames++;
	} else {
		transition(*src, kSrvStates);
		ensureRoot();
		ID3D12GraphicsCommandList* L = g.list.Get();
		const D3D12_GPU_VIRTUAL_ADDRESS ringBase = g.ring->GetGPUVirtualAddress();
		for (UINT i = 0; i < kCbSlots; ++i) L->SetGraphicsRootConstantBufferView(i, ringBase);

		const UINT srvBase = g.srvNext;
		g.srvNext += kTexSlots;
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += (SIZE_T)srvBase * g.srvInc;
		for (UINT i = 0; i < kTexSlots; ++i) {
			TexBind b;
			if (i == 0) { b.t = src; b.mipCount = 1; }
			makeSrv(cpu, b);
			cpu.ptr += g.srvInc;
		}
		D3D12_GPU_DESCRIPTOR_HANDLE gpu = g.srvHeap->GetGPUDescriptorHandleForHeapStart();
		gpu.ptr += (UINT64)srvBase * g.srvInc;
		L->SetGraphicsRootDescriptorTable(kCbSlots, gpu);

		// sampler de la ranura 0: lineal si hay que escalar, puntual si el tamano coincide (misma formula de clave que en draw)
		const int f = (src->w == g.width && src->h == g.height) ? 0 : 1;
		const float lod = -1.0f;
		uint32_t lodBits = 0;
		std::memcpy(&lodBits, &lod, sizeof(lodBits));
		K8 pk;
		std::memset(&pk, 0, sizeof(pk));
		pk.w[0] = 0x8000000000000000ull | (uint64_t)f | ((uint64_t)f << 1) | (1ull << 2) | (1ull << 4) | (1ull << 8) | ((uint64_t)lodBits << 16);
		UINT64 sg = 0;
		auto sit = g.sampTables.find(pk);
		if (sit != g.sampTables.end()) {
			sg = sit->second;
		} else {
			UINT idxs[8] = {};
			idxs[0] = getSampler(f, f, 1, 1, 1, lod);
			sg = addSampTable(pk, idxs);
		}
		D3D12_GPU_DESCRIPTOR_HANDLE sh{ sg };
		L->SetGraphicsRootDescriptorTable(kCbSlots + 1, sh);

		L->SetPipelineState(g.presentPso.Get());
		L->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
		D3D12_VIEWPORT vp{ 0.0f, 0.0f, (float)g.width, (float)g.height, 0.0f, 1.0f };
		D3D12_RECT sr{ 0, 0, (LONG)g.width, (LONG)g.height };
		L->RSSetViewports(1, &vp);
		L->RSSetScissorRects(1, &sr);
		L->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		L->DrawInstanced(3, 1, 0, 0);
		g.nPresentCopies++;
	}
	to(D3D12_RESOURCE_STATE_PRESENT);

	g.pendingPresent = 0;
	g.nFlips++;
	submit(true);
	// Espera (como mucho 100 ms) a que DXGI admita otro frame: mantiene la latencia acotada sin bloquear dentro de Present.
	if (g.latencyWait) {
		const long long t1 = qpc();
		WaitForSingleObjectEx(g.latencyWait, 100, TRUE);
		g.pfWait += qpc() - t1;
	}
	g.pfFlip += (qpc() - tf0) - (g.pfWait - wf0) - (g.pfPresent - pf0);
}

void shutdown() {
	g.ready = false;
	waitGpu();
	g.tex.clear();
	g.texIdx.clear();
	g.lastPso = nullptr;
	g.lastPsoId = 0;
	g.gbuf.clear();
	g.gbufPool.clear();
	g.gbufRetire.clear();
	g.gbufPoolBytes = 0;
	g.bufHeaps.clear();
	g.bufHeapBytes = 0;
	g.trash.clear();
	g.pso.clear();
	g.psoTrash.clear();
	g.presentPso.Reset();
	g.rootSig.Reset();
	g.srvTables.clear();
	g.sampTables.clear();
	g.sampHeap.Reset();
	g.sampStage.Reset();
	g.sampCache.clear();
	g.sampCount = 0;
	g.sampTableCount = 0;
	saveFrame();  // vuelca el frame actual a fr[cur] para liberar todos de la misma forma
	for (auto& f : g.fr) {
		if (f.ring) f.ring->Unmap(0, nullptr);
		f.ring.Reset();
		f.ringPtr = nullptr;
		f.alloc.Reset();
		f.srvHeap.Reset();
		f.rtvRing.Reset();
		f.dsvRing.Reset();
		f.trash.clear();
		f.gbufRetire.clear();
		f.psoTrash.clear();
	}
	for (auto& b : g.backbuf) b.Reset();
	g.list.Reset();
	g.rtvHeap.Reset();
	g.swap.Reset();
	g.fence.Reset();
	g.queue.Reset();
	g.dev.Reset();
	g.factory.Reset();
	if (g.latencyWait) { CloseHandle(g.latencyWait); g.latencyWait = nullptr; }
	if (g.fenceEvent) { CloseHandle(g.fenceEvent); g.fenceEvent = nullptr; }
}
}  // namespace

extern "C" {

#define LOCK std::lock_guard<std::mutex> _lk(g.mtx)

JNIEXPORT jboolean JNICALL Java_dev_dx12mod_client_Dx12Native_init(JNIEnv*, jclass, jlong hwnd) {
	LOCK;
	return init((HWND)(intptr_t)hwnd) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_dev_dx12mod_client_Dx12Native_shutdown(JNIEnv*, jclass) {
	LOCK;
	shutdown();
}

JNIEXPORT jlong JNICALL Java_dev_dx12mod_client_Dx12Native_createTexture(JNIEnv*, jclass, jint w, jint h, jint layers, jint mips, jint fmt) {
	LOCK;
	return createTexture(w, h, layers, mips, fmt);
}

JNIEXPORT void JNICALL Java_dev_dx12mod_client_Dx12Native_destroyTexture(JNIEnv*, jclass, jlong id) {
	LOCK;
	destroyTexture(id);
}

JNIEXPORT void JNICALL Java_dev_dx12mod_client_Dx12Native_clearColor(JNIEnv*, jclass, jlong id, jint argb, jint x, jint y, jint w, jint h) {
	LOCK;
	clearColor(id, argb, x, y, w, h);
}

JNIEXPORT void JNICALL Java_dev_dx12mod_client_Dx12Native_clearDepth(JNIEnv*, jclass, jlong id, jfloat d, jint x, jint y, jint w, jint h) {
	LOCK;
	clearDepth(id, d, x, y, w, h);
}

JNIEXPORT void JNICALL Java_dev_dx12mod_client_Dx12Native_writeTexture(JNIEnv*, jclass, jlong id, jlong srcAddr, jint srcRowBytes,
		jint mip, jint layer, jint dx, jint dy, jint w, jint h) {
	LOCK;
	writeTexture(id, (const unsigned char*)(intptr_t)srcAddr, srcRowBytes, mip, layer, dx, dy, w, h);
}

JNIEXPORT void JNICALL Java_dev_dx12mod_client_Dx12Native_presentTexture(JNIEnv*, jclass, jlong id) {
	LOCK;
	g.pendingPresent = id;
}

JNIEXPORT void JNICALL Java_dev_dx12mod_client_Dx12Native_flip(JNIEnv*, jclass, jfloat r, jfloat gr, jfloat b) {
	LOCK;
	flip(r, gr, b);
}

JNIEXPORT jstring JNICALL Java_dev_dx12mod_client_Dx12Native_stats(JNIEnv* env, jclass) {
	LOCK;
	char buf[900];
	snprintf(buf, sizeof(buf),
		"texturas vivas=%zu | frames=%llu | presentaciones=%llu | frames sin textura (color diag.)=%llu | clears=%llu | subidas=%llu (%llu KB) | draws=%llu | draws omitidos=%llu | pipelines=%zu",
		g.tex.size(), (unsigned long long)g.nFlips, (unsigned long long)g.nPresentCopies,
		(unsigned long long)g.nFallbackFrames, (unsigned long long)g.nClears,
		(unsigned long long)g.nUploads, (unsigned long long)(g.nUploadBytes / 1024),
		(unsigned long long)g.nDraws, (unsigned long long)g.nSkipped, g.pso.size());
	std::string out = buf;
	for (auto& r : g.skipReasons) out += "\n    omitido: " + r;
	return env->NewStringUTF(out.c_str());
}

JNIEXPORT jlong JNICALL Java_dev_dx12mod_client_Dx12Native_createPipeline(JNIEnv* env, jclass, jstring jname, jstring jvs, jstring jps,
		jintArray jlay, jintArray jst) {
	auto str = [&](jstring js) {
		const char* c = env->GetStringUTFChars(js, nullptr);
		std::string r(c ? c : "");
		if (c) env->ReleaseStringUTFChars(js, c);
		return r;
	};
	const std::string name = str(jname), vs = str(jvs), ps = str(jps);
	std::vector<jint> lay((size_t)env->GetArrayLength(jlay)), st((size_t)env->GetArrayLength(jst));
	if (!lay.empty()) env->GetIntArrayRegion(jlay, 0, (jsize)lay.size(), lay.data());
	if (!st.empty()) env->GetIntArrayRegion(jst, 0, (jsize)st.size(), st.data());
	LOCK;
	return createPipeline(name, vs, ps, lay, st);
}

JNIEXPORT void JNICALL Java_dev_dx12mod_client_Dx12Native_destroyPipeline(JNIEnv*, jclass, jlong id) {
	LOCK;
	destroyPipeline(id);
}

// Parametros de un draw en memoria nativa compartida con Java (ver Dx12RenderPass: mismos desplazamientos).
#pragma pack(push, 8)
struct PackedDraw {
	jlong pso, color, depth;               // 0, 8, 16
	jlong vbAddr, ibAddr;                  // 24, 32
	jlong vbId, vbVer, ibId, ibVer;        // 40, 48, 56, 64
	jint vbSize, ibSize, idxBytes, baseVertex, count, instances;  // 72 .. 92
	jint sx, sy, sw, sh, ibOff, pad;       // 96 .. 116
	jlong ub[kCbSlots * 2];                // 120
	jlong tex[kTexSlots * 2];              // 248
	jint tp[kTexSlots * 8];                // 376
};
#pragma pack(pop)
static_assert(sizeof(PackedDraw) == 632, "PackedDraw debe medir 632 bytes (Java escribe con estos desplazamientos)");
static_assert(offsetof(PackedDraw, ub) == 120 && offsetof(PackedDraw, tex) == 248 && offsetof(PackedDraw, tp) == 376, "desplazamientos de PackedDraw");

// Java comprueba al cargar que ambos lados coinciden en el tamano.
JNIEXPORT jint JNICALL Java_dev_dx12mod_client_Dx12Native_packedDrawSize(JNIEnv*, jclass) {
	return (jint)sizeof(PackedDraw);
}

JNIEXPORT void JNICALL Java_dev_dx12mod_client_Dx12Native_drawPacked(JNIEnv*, jclass, jlong addr) {
	if (!addr) return;
	const PackedDraw* k = (const PackedDraw*)(intptr_t)addr;
	if (k->pso == 0) return;  // pipeline sin traduccion
	DrawParams p;
	p.pso = k->pso;
	p.color = k->color;
	p.depth = k->depth;
	p.vb = (const unsigned char*)(intptr_t)k->vbAddr;
	p.vbSize = k->vbSize > 0 ? (size_t)k->vbSize : 0;
	p.ib = (const unsigned char*)(intptr_t)k->ibAddr;
	p.ibSize = k->ibSize > 0 ? (size_t)k->ibSize : 0;
	p.ibOff = k->ibOff > 0 ? (size_t)k->ibOff : 0;
	p.vbId = k->vbId; p.vbVer = k->vbVer; p.ibId = k->ibId; p.ibVer = k->ibVer;
	p.idxBytes = k->idxBytes;
	p.baseVertex = k->baseVertex;
	p.count = k->count;
	p.instances = k->instances;
	p.scissor = k->sw >= 0;
	p.sx = k->sx; p.sy = k->sy; p.sw = k->sw; p.sh = k->sh;
	p.ub = k->ub;
	p.tex = k->tex;
	p.tp = k->tp;
	LOCK;
	// se cronometra 1 de cada 16 draws (QueryPerformanceCounter en cada draw no es gratis)
	const bool sample = (g.pfDraws & 15u) == 0;
	const long long t0 = sample ? qpc() : 0, w0 = g.pfWait;
	draw(p);
	if (sample) g.pfNative += ((qpc() - t0) - (g.pfWait - w0)) * 16;
}

JNIEXPORT void JNICALL Java_dev_dx12mod_client_Dx12Native_setVsync(JNIEnv*, jclass, jint interval) {
	g.vsync = interval > 0 ? 1 : 0;
}

// Solo antes de init(): frames en vuelo (1 = como antes, CPU y GPU en serie; 2 o 3 = solapados).
JNIEXPORT void JNICALL Java_dev_dx12mod_client_Dx12Native_setInflight(JNIEnv*, jclass, jint n) {
	g.nInflight = (UINT)(n < 1 ? 1 : (n > (jint)kMaxInflight ? (jint)kMaxInflight : n));
}

JNIEXPORT jstring JNICALL Java_dev_dx12mod_client_Dx12Native_textureStats(JNIEnv* env, jclass, jlong id, jint mip) {
	LOCK;
	return env->NewStringUTF(textureStats(id, mip).c_str());
}

JNIEXPORT void JNICALL Java_dev_dx12mod_client_Dx12Native_copyTexture(JNIEnv*, jclass, jlong from, jlong to, jint mip,
		jint dx, jint dy, jint sx, jint sy, jint w, jint h) {
	LOCK;
	copyTexture(from, to, mip, dx, dy, sx, sy, w, h);
}


JNIEXPORT void JNICALL Java_dev_dx12mod_client_Dx12Native_destroyBuffer(JNIEnv*, jclass, jlong id) {
	LOCK;
	destroyBuffer(id);
}

// Devuelve un informe cada 300 frames (y reinicia contadores); cadena vacia el resto del tiempo.
JNIEXPORT jstring JNICALL Java_dev_dx12mod_client_Dx12Native_perfReport(JNIEnv* env, jclass) {
	LOCK;
	const long long now = qpc();
	if (!g.pfStart) g.pfStart = now;
	if (g.pfLast) {
		const long long d = now - g.pfLast;
		if (d > g.pfMaxFrame) g.pfMaxFrame = d;
		if (qpcMs(d) > 10.0) g.pfSlow++;
	}
	g.pfLast = now;
	g.pfFrames++;
	if (g.pfFrames < 300) return env->NewStringUTF("");
	const double n = (double)g.pfFrames;
	const double frameMs = qpcMs(now - g.pfStart) / n;
	const double nativeMs = (qpcMs(g.pfNative) + qpcMs(g.pfFlip)) / n;
	const double waitMs = qpcMs(g.pfWait) / n;
	const double presentMs = qpcMs(g.pfPresent) / n;
	double restMs = frameMs - nativeMs - waitMs - presentMs;
	if (restMs < 0) restMs = 0;
	size_t resBytes = 0;
	for (auto& kv : g.gbuf) resBytes += kv.second.size;
	char buf[900];
	snprintf(buf, sizeof(buf),
		"%d frames | %.0f FPS (%.2f ms/frame) = DLL %.2f + espera GPU/latencia %.2f + Present %.2f + Minecraft/Java %.2f ms | draws/frame=%.0f (residentes %.0f) | anillo=%.2f MB/frame | submits/frame=%.2f | VRAM residente=%.1f MB en %zu buffers (pool %.1f MB) | creados=%llu reciclados=%llu | PEOR frame=%.1f ms | frames>10ms=%llu | inflight=%u vsync=%d tearing=%d | colocados=%llu crear=%.2f ms/frame buffersRes=%.2f ms/frame heaps=%.0f MB perfil=%s anillo=%zu MB | shaders: de cache=%llu compilados=%llu (%.0f ms compilando)",
		(int)g.pfFrames, 1000.0 / (frameMs > 0 ? frameMs : 1.0), frameMs, nativeMs, waitMs, presentMs, restMs,
		g.pfDraws / n, g.pfResDraws / n, g.pfRingBytes / n / 1048576.0, g.pfSubmits / n,
		resBytes / 1048576.0, g.gbuf.size(), g.gbufPoolBytes / 1048576.0,
		(unsigned long long)g.pfResCreated, (unsigned long long)g.pfResReused,
		qpcMs(g.pfMaxFrame), (unsigned long long)g.pfSlow, g.nInflight, g.vsync, g.tearing ? 1 : 0,
		(unsigned long long)g.pfPlaced, qpcMs(g.pfCreateTime) / n, qpcMs(g.pfResTime) / n, g.bufHeapBytes / 1048576.0, gLowMem ? "bajo consumo" : "normal", (size_t)(kUploadRing >> 20),
		(unsigned long long)g.shHits, (unsigned long long)g.shMiss, qpcMs(g.shCompileTime));
	g.pfMaxFrame = 0;
	g.pfResCreated = g.pfResReused = g.pfSlow = 0;
	g.pfWait = g.pfPresent = g.pfNative = g.pfFlip = 0;
	g.pfDraw = g.pfResTime = 0;
	g.pfPlaced = 0;
	g.pfCreateTime = 0;
	g.pfDraws = g.pfResDraws = g.pfRingBytes = g.pfResUploads = g.pfResBytes = g.pfSubmits = g.pfFrames = 0;
	g.pfStart = now;
	return env->NewStringUTF(buf);
}

JNIEXPORT jstring JNICALL Java_dev_dx12mod_client_Dx12Native_adapterName(JNIEnv* env, jclass) {
	LOCK;
	return env->NewStringUTF(g.adapterName.c_str());
}

JNIEXPORT jstring JNICALL Java_dev_dx12mod_client_Dx12Native_lastError(JNIEnv* env, jclass) {
	LOCK;
	return env->NewStringUTF(g.lastError.c_str());
}

}  // extern "C"
