#include <jni.h>
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <sstream>
#include <string>

using Microsoft::WRL::ComPtr;

static std::string narrow(const wchar_t* w) {
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	std::string s(n > 0 ? n - 1 : 0, '\0');
	if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
	return s;
}

static const char* flName(D3D_FEATURE_LEVEL l) {
	switch (l) {
		case D3D_FEATURE_LEVEL_12_2: return "12_2";
		case D3D_FEATURE_LEVEL_12_1: return "12_1";
		case D3D_FEATURE_LEVEL_12_0: return "12_0";
		case D3D_FEATURE_LEVEL_11_1: return "11_1";
		default: return "11_0";
	}
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_dx12mod_client_Dx12Native_probe(JNIEnv* env, jclass) {
	std::ostringstream out;

	ComPtr<IDXGIFactory6> factory;
	if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
		return env->NewStringUTF("ERROR: CreateDXGIFactory2 fallo");

	ComPtr<IDXGIAdapter1> adapter;
	for (UINT i = 0; factory->EnumAdapterByGpuPreference(
			i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++i) {
		DXGI_ADAPTER_DESC1 d{};
		adapter->GetDesc1(&d);
		out << "Adapter " << i << ": " << narrow(d.Description)
			<< " | VRAM " << (d.DedicatedVideoMemory >> 20) << " MB";
		if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
			out << " | software (omitido)\n";
			adapter.Reset();
			continue;
		}

		ComPtr<ID3D12Device> dev;
		HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
		if (FAILED(hr)) {
			out << " | D3D12CreateDevice fallo (0x" << std::hex << hr << std::dec << ")\n";
			adapter.Reset();
			continue;
		}

		D3D_FEATURE_LEVEL levels[] = {
			D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
			D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
		D3D12_FEATURE_DATA_FEATURE_LEVELS fl{};
		fl.NumFeatureLevels = _countof(levels);
		fl.pFeatureLevelsRequested = levels;
		dev->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl));
		out << " | FL " << flName(fl.MaxSupportedFeatureLevel);

		D3D12_FEATURE_DATA_SHADER_MODEL sm{ D3D_SHADER_MODEL_6_6 };
		if (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))))
			out << " | SM <6.0";
		else
			out << " | SM 0x" << std::hex << sm.HighestShaderModel << std::dec;
		out << "\n";
		adapter.Reset();
	}
	return env->NewStringUTF(out.str().c_str());
}
