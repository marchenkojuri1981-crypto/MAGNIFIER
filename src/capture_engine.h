#pragma once

#include "monitor_manager.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <optional>

struct CaptureFrame {
    ID3D11Texture2D* texture{};
    DXGI_OUTDUPL_FRAME_INFO info{};
};

class CaptureEngine {
public:
    CaptureEngine();
    ~CaptureEngine();

    bool InitializeForMonitor(const MonitorInfo& source);
    void Shutdown();

    std::optional<CaptureFrame> AcquireFrame();
    void ReleaseFrame();

    bool NeedsReinitialize() const { return needs_reinitialize_; }
    bool Reinitialize();

    ID3D11Device* Device() const { return device_.Get(); }
    ID3D11DeviceContext* Context() const { return context_.Get(); }
    const DXGI_OUTPUT_DESC& OutputDesc() const { return output_desc_; }
    const D3D11_TEXTURE2D_DESC& FrameDesc() const { return frame_desc_; }

private:
    bool EnsureDevice();
    bool CreateDuplication(const MonitorInfo& source);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> current_frame_;
    bool frame_acquired_{false};
    DXGI_OUTPUT_DESC output_desc_{};
    D3D11_TEXTURE2D_DESC frame_desc_{};
    std::optional<MonitorInfo> source_monitor_;
    bool needs_reinitialize_{false};
};
