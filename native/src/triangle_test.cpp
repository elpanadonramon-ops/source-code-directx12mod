// Paso 2: programa de prueba independiente (sin Minecraft ni JNI).
// Abre una ventana y dibuja un triangulo con D3D12 puro.
// Uso: dx12_triangle.exe [--debug]   (--debug activa la capa de validacion de D3D12)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;

static const UINT kFrames = 2;
static const DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

struct Vertex { float pos[3]; float col[4]; };

static const char* kShader = R"(
struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR; };
VSOut VSMain(float3 pos : POSITION, float4 col : COLOR) {
	VSOut o; o.pos = float4(pos, 1.0); o.col = col; return o;
}
float4 PSMain(VSOut i) : SV_TARGET { return i.col; }
)";

static void Check(HRESULT hr, const char* what) {
	if (FAILED(hr)) {
		char msg[256];
		snprintf(msg, sizeof(msg), "%s fallo (HRESULT 0x%08X)", what, (unsigned)hr);
		printf("ERROR: %s\n", msg);
		MessageBoxA(nullptr, msg, "dx12_triangle", MB_ICONERROR);
		exit(1);
	}
}

struct App {
	HWND hwnd{};
	UINT width = 1280, height = 720;
	bool minimized = false;

	ComPtr<ID3D12Device> dev;
	ComPtr<ID3D12CommandQueue> queue;
	ComPtr<IDXGISwapChain3> swap;
	ComPtr<ID3D12DescriptorHeap> rtvHeap;
	UINT rtvSize = 0;
	ComPtr<ID3D12Resource> backbuf[kFrames];
	ComPtr<ID3D12CommandAllocator> alloc;
	ComPtr<ID3D12GraphicsCommandList> list;
	ComPtr<ID3D12RootSignature> rootSig;
	ComPtr<ID3D12PipelineState> pso;
	ComPtr<ID3D12Resource> vb;
	D3D12_VERTEX_BUFFER_VIEW vbv{};
	ComPtr<ID3D12Fence> fence;
	UINT64 fenceValue = 0;
	HANDLE fenceEvent{};
};
static App* g_app = nullptr;

static void WaitForGpu(App& a) {
	a.queue->Signal(a.fence.Get(), ++a.fenceValue);
	if (a.fence->GetCompletedValue() < a.fenceValue) {
		a.fence->SetEventOnCompletion(a.fenceValue, a.fenceEvent);
		WaitForSingleObject(a.fenceEvent, INFINITE);
	}
}

static void CreateRtvs(App& a) {
	D3D12_CPU_DESCRIPTOR_HANDLE h = a.rtvHeap->GetCPUDescriptorHandleForHeapStart();
	for (UINT i = 0; i < kFrames; ++i) {
		Check(a.swap->GetBuffer(i, IID_PPV_ARGS(&a.backbuf[i])), "GetBuffer");
		a.dev->CreateRenderTargetView(a.backbuf[i].Get(), nullptr, h);
		h.ptr += a.rtvSize;
	}
}

static void Resize(App& a, UINT w, UINT h) {
	if (w == 0 || h == 0) { a.minimized = true; return; }
	a.minimized = false;
	if (!a.swap) return;
	WaitForGpu(a);
	for (auto& b : a.backbuf) b.Reset();
	Check(a.swap->ResizeBuffers(kFrames, w, h, DXGI_FORMAT_UNKNOWN, 0), "ResizeBuffers");
	a.width = w; a.height = h;
	CreateRtvs(a);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
		case WM_SIZE:
			if (g_app) Resize(*g_app, LOWORD(lp), HIWORD(lp));
			return 0;
		case WM_KEYDOWN:
			if (wp == VK_ESCAPE) DestroyWindow(hwnd);
			return 0;
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

static ComPtr<ID3DBlob> Compile(const char* entry, const char* target) {
	ComPtr<ID3DBlob> blob, err;
	HRESULT hr = D3DCompile(kShader, strlen(kShader), "triangle.hlsl", nullptr, nullptr,
		entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &err);
	if (FAILED(hr)) {
		printf("Error de shader:\n%s\n", err ? (const char*)err->GetBufferPointer() : "?");
		Check(hr, "D3DCompile");
	}
	return blob;
}

int main(int argc, char** argv) {
	bool debug = argc > 1 && strcmp(argv[1], "--debug") == 0;
	App a;
	g_app = &a;

	// Ventana
	HINSTANCE inst = GetModuleHandleW(nullptr);
	WNDCLASSW wc{};
	wc.lpfnWndProc = WndProc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
	wc.lpszClassName = L"dx12mod_triangle";
	RegisterClassW(&wc);
	RECT r{ 0, 0, (LONG)a.width, (LONG)a.height };
	AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
	a.hwnd = CreateWindowW(wc.lpszClassName, L"DX12 triangulo", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
		CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
		nullptr, nullptr, inst, nullptr);

	// Factory + capa de debug opcional
	UINT factoryFlags = 0;
	if (debug) {
		ComPtr<ID3D12Debug> dbg;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
			dbg->EnableDebugLayer();
			factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
		} else {
			printf("Capa de debug no disponible (instala 'Graphics Tools' en Windows).\n");
		}
	}
	ComPtr<IDXGIFactory6> factory;
	Check(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");

	// Adaptador: la GPU de alto rendimiento que no sea software
	ComPtr<IDXGIAdapter1> adapter;
	for (UINT i = 0; factory->EnumAdapterByGpuPreference(
			i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++i) {
		DXGI_ADAPTER_DESC1 d{};
		adapter->GetDesc1(&d);
		if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
			SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr))) {
			std::wstring title = L"DX12 triangulo - " + std::wstring(d.Description);
			SetWindowTextW(a.hwnd, title.c_str());
			break;
		}
		adapter.Reset();
	}
	if (!adapter) { printf("No hay GPU compatible con D3D12\n"); return 1; }

	Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&a.dev)), "D3D12CreateDevice");

	// Cola de comandos
	D3D12_COMMAND_QUEUE_DESC qd{};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	Check(a.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&a.queue)), "CreateCommandQueue");

	// Swapchain (flip model)
	DXGI_SWAP_CHAIN_DESC1 sd{};
	sd.Width = a.width; sd.Height = a.height;
	sd.Format = kFormat;
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = kFrames;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	ComPtr<IDXGISwapChain1> sc1;
	Check(factory->CreateSwapChainForHwnd(a.queue.Get(), a.hwnd, &sd, nullptr, nullptr, &sc1), "CreateSwapChainForHwnd");
	factory->MakeWindowAssociation(a.hwnd, DXGI_MWA_NO_ALT_ENTER);
	Check(sc1.As(&a.swap), "QueryInterface IDXGISwapChain3");

	// RTVs
	D3D12_DESCRIPTOR_HEAP_DESC hd{};
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	hd.NumDescriptors = kFrames;
	Check(a.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&a.rtvHeap)), "CreateDescriptorHeap");
	a.rtvSize = a.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	CreateRtvs(a);

	// Root signature vacia (solo input assembler)
	D3D12_ROOT_SIGNATURE_DESC rs{};
	rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	ComPtr<ID3DBlob> rsBlob, rsErr;
	Check(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr), "SerializeRootSignature");
	Check(a.dev->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
		IID_PPV_ARGS(&a.rootSig)), "CreateRootSignature");

	// Pipeline state
	ComPtr<ID3DBlob> vs = Compile("VSMain", "vs_5_0");
	ComPtr<ID3DBlob> ps = Compile("PSMain", "ps_5_0");
	D3D12_INPUT_ELEMENT_DESC layout[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
	pd.pRootSignature = a.rootSig.Get();
	pd.InputLayout = { layout, _countof(layout) };
	pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
	pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
	pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pd.RasterizerState.DepthClipEnable = TRUE;
	pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	pd.SampleMask = UINT_MAX;
	pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pd.NumRenderTargets = 1;
	pd.RTVFormats[0] = kFormat;
	pd.SampleDesc.Count = 1;
	Check(a.dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&a.pso)), "CreateGraphicsPipelineState");

	// Comandos
	Check(a.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a.alloc)), "CreateCommandAllocator");
	Check(a.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a.alloc.Get(), a.pso.Get(),
		IID_PPV_ARGS(&a.list)), "CreateCommandList");
	a.list->Close();

	// Vertex buffer (heap de subida, suficiente para 3 vertices)
	Vertex verts[3] = {
		{ {  0.0f,  0.6f, 0.0f }, { 1, 0, 0, 1 } },
		{ {  0.6f, -0.6f, 0.0f }, { 0, 1, 0, 1 } },
		{ { -0.6f, -0.6f, 0.0f }, { 0, 0, 1, 1 } },
	};
	D3D12_HEAP_PROPERTIES hp{};
	hp.Type = D3D12_HEAP_TYPE_UPLOAD;
	D3D12_RESOURCE_DESC rd{};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	rd.Width = sizeof(verts);
	rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
	rd.Format = DXGI_FORMAT_UNKNOWN;
	rd.SampleDesc.Count = 1;
	rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	Check(a.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&a.vb)), "CreateCommittedResource");
	void* mapped = nullptr;
	D3D12_RANGE noRead{ 0, 0 };
	Check(a.vb->Map(0, &noRead, &mapped), "Map");
	memcpy(mapped, verts, sizeof(verts));
	a.vb->Unmap(0, nullptr);
	a.vbv = { a.vb->GetGPUVirtualAddress(), sizeof(verts), sizeof(Vertex) };

	// Fence
	Check(a.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&a.fence)), "CreateFence");
	a.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

	// Bucle principal
	MSG m{};
	while (true) {
		while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
			if (m.message == WM_QUIT) goto done;
			TranslateMessage(&m);
			DispatchMessageW(&m);
		}
		if (a.minimized) { Sleep(50); continue; }

		Check(a.alloc->Reset(), "Allocator Reset");
		Check(a.list->Reset(a.alloc.Get(), a.pso.Get()), "List Reset");
		a.list->SetGraphicsRootSignature(a.rootSig.Get());
		D3D12_VIEWPORT vp{ 0, 0, (float)a.width, (float)a.height, 0, 1 };
		D3D12_RECT sc{ 0, 0, (LONG)a.width, (LONG)a.height };
		a.list->RSSetViewports(1, &vp);
		a.list->RSSetScissorRects(1, &sc);

		UINT idx = a.swap->GetCurrentBackBufferIndex();
		D3D12_RESOURCE_BARRIER b{};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = a.backbuf[idx].Get();
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
		b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		a.list->ResourceBarrier(1, &b);

		D3D12_CPU_DESCRIPTOR_HANDLE rtv = a.rtvHeap->GetCPUDescriptorHandleForHeapStart();
		rtv.ptr += (SIZE_T)idx * a.rtvSize;
		a.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
		const float clear[4] = { 0.05f, 0.05f, 0.08f, 1.0f };
		a.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
		a.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		a.list->IASetVertexBuffers(0, 1, &a.vbv);
		a.list->DrawInstanced(3, 1, 0, 0);

		b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
		a.list->ResourceBarrier(1, &b);
		Check(a.list->Close(), "List Close");

		ID3D12CommandList* lists[] = { a.list.Get() };
		a.queue->ExecuteCommandLists(1, lists);
		Check(a.swap->Present(1, 0), "Present");
		WaitForGpu(a);  // simple a proposito; en pasos siguientes habra frames en vuelo
	}
done:
	WaitForGpu(a);
	CloseHandle(a.fenceEvent);
	return 0;
}
