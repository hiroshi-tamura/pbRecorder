#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <mutex>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

namespace pb {

/// Singleton manager for a shared D3D11 device and immediate context.
/// Thread-safe: the device is created once on first access and reused.
class D3DManager {
public:
    /// Get the singleton instance.
    static D3DManager& instance();

    /// Initialize the D3D11 device. Called automatically by getDevice() if
    /// not already initialized. Can be called explicitly to force early init.
    /// Throws std::runtime_error on failure.
    void initialize();

    /// Get the shared D3D11 device.
    ID3D11Device* getDevice() const;

    /// Get the shared D3D11 device as ComPtr (adds ref).
    ComPtr<ID3D11Device> getDeviceComPtr() const;

    /// Get the immediate device context.
    ID3D11DeviceContext* getContext() const;

    /// Get the immediate device context as ComPtr (adds ref).
    ComPtr<ID3D11DeviceContext> getContextComPtr() const;

    /// Get the DXGI adapter associated with the device.
    ComPtr<IDXGIAdapter> getAdapter() const;

    /// Check whether the device has been created.
    bool isInitialized() const;

    /// Reset the device (e.g. after a device-lost error). Next call to
    /// getDevice() will re-create.
    void reset();

    // Non-copyable, non-movable
    D3DManager(const D3DManager&) = delete;
    D3DManager& operator=(const D3DManager&) = delete;
    D3DManager(D3DManager&&) = delete;
    D3DManager& operator=(D3DManager&&) = delete;

private:
    D3DManager() = default;
    ~D3DManager() = default;

    void ensureInitialized() const;

    mutable std::mutex mutex_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    bool initialized_ = false;
};

} // namespace pb
