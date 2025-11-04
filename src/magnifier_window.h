#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <windows.h>

#include <string>
#include <optional>

struct MonitorInfo;
struct CaptureFrame;

struct ViewState {
    RECT source_region{};
    float zoom{2.0f};
    bool cursor_visible{false};
    bool invert_colors{false};
    float cursor_x{0.0f};
    float cursor_y{0.0f};
};

class MagnifierWindow {
public:
    MagnifierWindow();
    ~MagnifierWindow();

    bool Initialize(HWND parent, ID3D11Device* device, ID3D11DeviceContext* context);
    void Shutdown();

    bool AttachToMonitor(const MonitorInfo& monitor);
    void PresentFrame(const CaptureFrame& frame, const ViewState& state);
    void ShowLayoutOverlay(const std::wstring& text, ULONGLONG duration_ms);
    void SetStatusBadge(const std::wstring& text, ULONGLONG duration_ms);

    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

    bool CreateSwapChain();
    bool CreatePipeline();
    void ResizeIfNeeded();
    bool UpdateCursorTexture();
    void DrawCursor(const ViewState& state);
    void DrawLayoutOverlay();
    void DrawStatusOverlay();
    void DrawTexturedQuad(ID3D11ShaderResourceView* srv, float left_px, float top_px, float right_px, float bottom_px);
    bool CreateOverlayTexture(const std::wstring& text, const SIZE& target_size,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv);

    HWND hwnd_{};
    HMONITOR attached_monitor_{};
    ViewState view_state_{};

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> input_layout_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertex_buffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> pointer_vertex_buffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> index_buffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constant_buffer_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_state_;

    ID3D11Device* device_{};
    ID3D11DeviceContext* context_{};
    SIZE window_size_{};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> cursor_texture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cursor_srv_;
    SIZE cursor_size_{};
    POINT cursor_hotspot_{};
    bool cursor_visible_{false};
    HCURSOR last_cursor_{};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> overlay_texture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> overlay_srv_;
    ULONGLONG overlay_expire_tick_{0};
    SIZE overlay_size_{400, 400};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> status_overlay_texture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> status_overlay_srv_;
    SIZE status_overlay_size_{400, 400};
    ULONGLONG status_overlay_expire_tick_{0};
};
