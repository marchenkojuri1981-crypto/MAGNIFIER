#include "capture_engine.h"

#include "monitor_manager.h"
#include "logger.h"

#include <dxgi1_6.h>
#include <d3d11_1.h>
#include <vector>

namespace {
constexpr UINT kFrameTimeoutMs = 16;
}

CaptureEngine::CaptureEngine() = default;
CaptureEngine::~CaptureEngine() {
    Shutdown();
}

bool CaptureEngine::InitializeForMonitor(const MonitorInfo& source) {
    Shutdown();

    source_monitor_ = source;
    needs_reinitialize_ = false;

    if (!EnsureDevice()) {
        Logger::Error(L"Failed to initialize D3D11 device");
        return false;
    }

    if (!CreateDuplication(*source_monitor_)) {
        Logger::Error(L"Failed to create DXGI duplication");
        return false;
    }

    return true;
}

void CaptureEngine::Shutdown() {
    if (frame_acquired_ && duplication_) {
        duplication_->ReleaseFrame();
        frame_acquired_ = false;
    }
    duplication_.Reset();
    staging_.Reset();
    current_frame_.Reset();
    context_.Reset();
    device_.Reset();
    source_monitor_.reset();
    needs_reinitialize_ = false;
}

std::optional<CaptureFrame> CaptureEngine::AcquireFrame() {
    if (!duplication_) {
        return std::nullopt;
    }

    if (frame_acquired_) {
        duplication_->ReleaseFrame();
        frame_acquired_ = false;
        current_frame_.Reset();
    }

    DXGI_OUTDUPL_FRAME_INFO info{};
    Microsoft::WRL::ComPtr<IDXGIResource> resource;
    HRESULT hr = duplication_->AcquireNextFrame(kFrameTimeoutMs, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return std::nullopt;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        Logger::Error(L"Desktop duplication access lost");
        staging_.Reset();
        frame_acquired_ = false;
        current_frame_.Reset();
        needs_reinitialize_ = true;
        duplication_.Reset();
        return std::nullopt;
    }
    if (FAILED(hr)) {
        Logger::Error(L"AcquireNextFrame failed");
        duplication_.Reset();
        staging_.Reset();
        frame_acquired_ = false;
        current_frame_.Reset();
        needs_reinitialize_ = true;
        return std::nullopt;
    }

    hr = resource.As(&current_frame_);
    if (FAILED(hr)) {
        duplication_->ReleaseFrame();
        Logger::Error(L"Failed to query frame texture");
        frame_acquired_ = false;
        current_frame_.Reset();
        return std::nullopt;
    }

    current_frame_->GetDesc(&frame_desc_);

    context_->CopyResource(staging_.Get(), current_frame_.Get());

    duplication_->ReleaseFrame();
    frame_acquired_ = false;
    current_frame_.Reset();

    CaptureFrame frame{};
    frame.texture = staging_.Get();
    frame.info = info;
    return frame;
}

void CaptureEngine::ReleaseFrame() {
    if (frame_acquired_ && duplication_) {
        duplication_->ReleaseFrame();
        frame_acquired_ = false;
        current_frame_.Reset();
    }
}

bool CaptureEngine::Reinitialize() {
    if (!source_monitor_) {
        return false;
    }

    if (!EnsureDevice()) {
        return false;
    }

    duplication_.Reset();
    staging_.Reset();
    frame_acquired_ = false;
    current_frame_.Reset();

    if (!CreateDuplication(*source_monitor_)) {
        needs_reinitialize_ = true;
        return false;
    }

    needs_reinitialize_ = false;
    return true;
}

bool CaptureEngine::EnsureDevice() {
    if (device_) {
        return true;
    }

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    std::vector<D3D_FEATURE_LEVEL> levels = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    D3D_FEATURE_LEVEL created{};
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        levels.data(),
        static_cast<UINT>(levels.size()),
        D3D11_SDK_VERSION,
        &device_,
        &created,
        &context_);

    if (FAILED(hr)) {
        Logger::Error(L"D3D11CreateDevice failed");
        return false;
    }

    return true;
}

bool CaptureEngine::CreateDuplication(const MonitorInfo& source) {
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    HRESULT hr = device_.As(&dxgi_device);
    if (FAILED(hr)) {
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_device->GetAdapter(&adapter);
    if (FAILED(hr)) {
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIOutput> matched_output;
    UINT index = 0;
    while (true) {
        Microsoft::WRL::ComPtr<IDXGIOutput> output;
        if (adapter->EnumOutputs(index++, &output) == DXGI_ERROR_NOT_FOUND) {
            break;
        }

        DXGI_OUTPUT_DESC desc{};
        if (FAILED(output->GetDesc(&desc))) {
            continue;
        }

        if (desc.Monitor == source.handle) {
            matched_output = output;
            output_desc_ = desc;
            break;
        }
    }

    if (!matched_output) {
        Logger::Error(L"Matching output not found");
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
    hr = matched_output.As(&output1);
    if (FAILED(hr)) {
        return false;
    }

    hr = output1->DuplicateOutput(device_.Get(), &duplication_);
    if (FAILED(hr)) {
        Logger::Error(L"DuplicateOutput failed");
        return false;
    }

    DXGI_OUTDUPL_DESC duplic_desc{};
    duplication_->GetDesc(&duplic_desc);

    frame_desc_.Width = duplic_desc.ModeDesc.Width;
    frame_desc_.Height = duplic_desc.ModeDesc.Height;
    frame_desc_.Format = duplic_desc.ModeDesc.Format;
    frame_desc_.ArraySize = 1;
    frame_desc_.MipLevels = 1;
    frame_desc_.SampleDesc.Count = 1;
    frame_desc_.SampleDesc.Quality = 0;
    frame_desc_.Usage = D3D11_USAGE_DEFAULT;
    frame_desc_.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    frame_desc_.CPUAccessFlags = 0;
    frame_desc_.MiscFlags = 0;

    staging_.Reset();
    hr = device_->CreateTexture2D(&frame_desc_, nullptr, &staging_);
    if (FAILED(hr)) {
        Logger::Error(L"Failed to create staging texture");
        return false;
    }

    return true;
}
