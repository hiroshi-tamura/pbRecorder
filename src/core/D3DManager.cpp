#include "D3DManager.h"

#include <d3d11.h>
#include <dxgi1_2.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace pb {

D3DManager& D3DManager::instance() {
    static D3DManager inst;
    return inst;
}

void D3DManager::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        return;
    }

    UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    // Feature levels to try, in order of preference.
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    UINT numFeatureLevels = static_cast<UINT>(sizeof(featureLevels) / sizeof(featureLevels[0]));

    D3D_FEATURE_LEVEL achievedLevel{};

    // First, try to create a hardware device.
    HRESULT hr = D3D11CreateDevice(
        nullptr,                    // default adapter
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,                    // no software module
        creationFlags,
        featureLevels,
        numFeatureLevels,
        D3D11_SDK_VERSION,
        device_.ReleaseAndGetAddressOf(),
        &achievedLevel,
        context_.ReleaseAndGetAddressOf()
    );

    // If debug layer is not available, retry without it.
#ifdef _DEBUG
    if (FAILED(hr) && (creationFlags & D3D11_CREATE_DEVICE_DEBUG)) {
        creationFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            creationFlags,
            featureLevels,
            numFeatureLevels,
            D3D11_SDK_VERSION,
            device_.ReleaseAndGetAddressOf(),
            &achievedLevel,
            context_.ReleaseAndGetAddressOf()
        );
    }
#endif

    if (FAILED(hr)) {
        // Fallback to WARP (software renderer).
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels,
            numFeatureLevels,
            D3D11_SDK_VERSION,
            device_.ReleaseAndGetAddressOf(),
            &achievedLevel,
            context_.ReleaseAndGetAddressOf()
        );
    }

    if (FAILED(hr)) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "Failed to create D3D11 device: HRESULT 0x%08lX",
                 static_cast<unsigned long>(hr));
        throw std::runtime_error(buf);
    }

    // Enable multithreaded device access for safety.
    ComPtr<ID3D10Multithread> multithread;
    hr = device_.As(&multithread);
    if (SUCCEEDED(hr) && multithread) {
        multithread->SetMultithreadProtected(TRUE);
    }

    initialized_ = true;
}

ID3D11Device* D3DManager::getDevice() const {
    ensureInitialized();
    return device_.Get();
}

ComPtr<ID3D11Device> D3DManager::getDeviceComPtr() const {
    ensureInitialized();
    return device_;
}

ID3D11DeviceContext* D3DManager::getContext() const {
    ensureInitialized();
    return context_.Get();
}

ComPtr<ID3D11DeviceContext> D3DManager::getContextComPtr() const {
    ensureInitialized();
    return context_;
}

ComPtr<IDXGIAdapter> D3DManager::getAdapter() const {
    ensureInitialized();

    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = device_.As(&dxgiDevice);
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to query IDXGIDevice from D3D11 device");
    }

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(adapter.GetAddressOf());
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to get DXGI adapter");
    }

    return adapter;
}

bool D3DManager::isInitialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
}

void D3DManager::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    context_.Reset();
    device_.Reset();
    initialized_ = false;
}

void D3DManager::ensureInitialized() const {
    // Double-checked locking pattern: check without lock first.
    if (!initialized_) {
        // const_cast is acceptable here because initialization is logically
        // const (lazy init of a singleton).
        const_cast<D3DManager*>(this)->initialize();
    }
}

} // namespace pb
