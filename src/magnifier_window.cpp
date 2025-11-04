#include "magnifier_window.h"

#include "capture_engine.h"
#include "monitor_manager.h"
#include "resource.h"

#include <dwmapi.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <vector>

#include "logger.h"

namespace {
constexpr wchar_t kMagnifierWindowClass[] = L"ElectronicMagnifierWindow";
}

MagnifierWindow::MagnifierWindow() = default;
MagnifierWindow::~MagnifierWindow() {
    Shutdown();
}

bool MagnifierWindow::Initialize(HWND parent, ID3D11Device* device, ID3D11DeviceContext* context) {
    device_ = device;
    context_ = context;

    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = &MagnifierWindow::WindowProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = kMagnifierWindowClass;
    cls.hIcon = LoadIconW(cls.hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    cls.hIconSm = cls.hIcon;
    RegisterClassExW(&cls);

    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kMagnifierWindowClass,
        L"",
        WS_POPUP,
        0, 0, 100, 100,
        parent,
        nullptr,
        GetModuleHandleW(nullptr),
        this);

    if (!hwnd_) {
        return false;
    }

    SetWindowLongW(hwnd_, GWL_STYLE, WS_POPUP);
    SetWindowLongW(hwnd_, GWL_EXSTYLE, WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED);
    MARGINS margins{ -1 };
    DwmExtendFrameIntoClientArea(hwnd_, &margins);

    if (!CreateSwapChain()) {
        return false;
    }

    if (!CreatePipeline()) {
        return false;
    }

    ResizeIfNeeded();
    return true;
}

void MagnifierWindow::Shutdown() {
    swap_chain_.Reset();
    rtv_.Reset();
    sampler_.Reset();
    overlay_texture_.Reset();
    overlay_srv_.Reset();
    status_overlay_texture_.Reset();
    status_overlay_srv_.Reset();
    status_overlay_expire_tick_ = 0;

    index_buffer_.Reset();
    constant_buffer_.Reset();
    vertex_shader_.Reset();
    pixel_shader_.Reset();
    input_layout_.Reset();

    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

bool MagnifierWindow::AttachToMonitor(const MonitorInfo& monitor) {
    attached_monitor_ = monitor.handle;
    if (!hwnd_) {
        return false;
    }

    SetWindowPos(hwnd_, HWND_TOPMOST,
        monitor.bounds.left,
        monitor.bounds.top,
        monitor.bounds.right - monitor.bounds.left,
        monitor.bounds.bottom - monitor.bounds.top,
        SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    ResizeIfNeeded();
    return true;
}

void MagnifierWindow::PresentFrame(const CaptureFrame& frame, const ViewState& state) {
    if (!swap_chain_) {
        return;
    }

    ResizeIfNeeded();
    view_state_ = state;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    D3D11_TEXTURE2D_DESC desc{};
    frame.texture->GetDesc(&desc);

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = desc.Format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;

    if (FAILED(device_->CreateShaderResourceView(frame.texture, &srv_desc, &srv))) {
        return;
    }

    float tex_width = static_cast<float>(desc.Width);
    float tex_height = static_cast<float>(desc.Height);
    float left = static_cast<float>(state.source_region.left) / tex_width;
    float top = static_cast<float>(state.source_region.top) / tex_height;
    float width = static_cast<float>(state.source_region.right - state.source_region.left) / tex_width;
    float height = static_cast<float>(state.source_region.bottom - state.source_region.top) / tex_height;

    struct ViewConstants {
        float uv_rect[4];
        float render_flags[4];
    } constants{};
    constants.uv_rect[0] = left;
    constants.uv_rect[1] = top;
    constants.uv_rect[2] = width;
    constants.uv_rect[3] = height;
    constants.render_flags[0] = state.invert_colors ? 1.0f : 0.0f;
    constants.render_flags[1] = 0.0f;
    constants.render_flags[2] = 0.0f;
    constants.render_flags[3] = 0.0f;

    context_->UpdateSubresource(constant_buffer_.Get(), 0, nullptr, &constants, 0, 0);

    FLOAT clear[4] = {0, 0, 0, 1};
    context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    context_->ClearRenderTargetView(rtv_.Get(), clear);

    D3D11_VIEWPORT viewport{};
    viewport.TopLeftX = 0;
    viewport.TopLeftY = 0;
    viewport.Width = static_cast<FLOAT>(window_size_.cx);
    viewport.Height = static_cast<FLOAT>(window_size_.cy);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &viewport);

    UINT stride = sizeof(float) * 5;
    UINT offset = 0;
    ID3D11Buffer* vertex_buffers[] = { vertex_buffer_.Get() };
    context_->IASetVertexBuffers(0, 1, vertex_buffers, &stride, &offset);
    context_->IASetIndexBuffer(index_buffer_.Get(), DXGI_FORMAT_R32_UINT, 0);
    context_->IASetInputLayout(input_layout_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
    context_->PSSetShader(pixel_shader_.Get(), nullptr, 0);
    context_->PSSetShaderResources(0, 1, srv.GetAddressOf());
    context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    context_->VSSetConstantBuffers(0, 1, constant_buffer_.GetAddressOf());
    context_->PSSetConstantBuffers(0, 1, constant_buffer_.GetAddressOf());

    context_->DrawIndexed(6, 0, 0);

    if (state.cursor_visible && UpdateCursorTexture()) {
        DrawCursor(state);
    } else if (!state.cursor_visible) {
        cursor_visible_ = false;
    }

    DrawLayoutOverlay();

    swap_chain_->Present(1, 0);
}

LRESULT CALLBACK MagnifierWindow::WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        auto self = static_cast<MagnifierWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    auto self = reinterpret_cast<MagnifierWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        if (msg == WM_SIZE) {
            self->ResizeIfNeeded();
        }
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool MagnifierWindow::CreateSwapChain() {
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(device_->QueryInterface(IID_PPV_ARGS(&dxgi_device)))) {
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi_device->GetAdapter(&adapter))) {
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        return false;
    }

    RECT rect{};
    GetClientRect(hwnd_, &rect);
    window_size_.cx = rect.right - rect.left;
    window_size_.cy = rect.bottom - rect.top;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = window_size_.cx;
    desc.Height = window_size_.cy;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    if (FAILED(factory->CreateSwapChainForHwnd(device_, hwnd_, &desc, nullptr, nullptr, &swap_chain_))) {
        Logger::Error(L"CreateSwapChainForHwnd failed");
        return false;
    }

    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);

    return true;
}

bool MagnifierWindow::CreatePipeline() {
    static const char* kVertexShader = R"(
        struct VSInput {
            float3 position : POSITION;
            float2 uv : TEXCOORD0;
        };
        struct VSOutput {
            float4 position : SV_POSITION;
            float2 uv : TEXCOORD0;
        };
        cbuffer ViewConstants : register(b0) {
            float4 uv_rect;
            float4 render_flags;
        };
        VSOutput main(VSInput input) {
            VSOutput output;
            output.position = float4(input.position, 1.0);
            output.uv = uv_rect.xy + input.uv * uv_rect.zw;
            return output;
        }
    )";

    static const char* kPixelShader = R"(
        Texture2D source_tex : register(t0);
        SamplerState linear_sampler : register(s0);
        cbuffer ViewConstants : register(b0) {
            float4 uv_rect;
            float4 render_flags;
        };
        struct PSInput {
            float4 position : SV_POSITION;
            float2 uv : TEXCOORD0;
        };
        float4 main(PSInput input) : SV_TARGET {
            float4 color = source_tex.Sample(linear_sampler, input.uv);
            if (render_flags.x > 0.5f) {
                color.rgb = 1.0f - color.rgb;
            }
            return color;
        }
    )";

    Microsoft::WRL::ComPtr<ID3DBlob> vs_blob;
    Microsoft::WRL::ComPtr<ID3DBlob> ps_blob;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;

    if (FAILED(D3DCompile(kVertexShader, strlen(kVertexShader), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vs_blob, &errors))) {
        if (errors) {
            Logger::Error(std::wstring(L"VS compile error"));
        }
        return false;
    }

    if (FAILED(D3DCompile(kPixelShader, strlen(kPixelShader), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &ps_blob, &errors))) {
        if (errors) {
            Logger::Error(std::wstring(L"PS compile error"));
        }
        return false;
    }

    if (FAILED(device_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vertex_shader_))) {
        return false;
    }
    if (FAILED(device_->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &pixel_shader_))) {
        return false;
    }

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };

    if (FAILED(device_->CreateInputLayout(layout, ARRAYSIZE(layout), vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &input_layout_))) {
        return false;
    }

    struct Vertex {
        float position[3];
        float uv[2];
    };
    Vertex vertices[] = {
        {{-1.f, -1.f, 0.f}, {0.f, 1.f}},
        {{-1.f,  1.f, 0.f}, {0.f, 0.f}},
        {{ 1.f, -1.f, 0.f}, {1.f, 1.f}},
        {{ 1.f,  1.f, 0.f}, {1.f, 0.f}},
    };

    UINT indices[] = {0, 1, 2, 2, 1, 3};

    D3D11_BUFFER_DESC vb_desc{};
    vb_desc.ByteWidth = sizeof(vertices);
    vb_desc.Usage = D3D11_USAGE_IMMUTABLE;
    vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA vb_data{};
    vb_data.pSysMem = vertices;

    if (FAILED(device_->CreateBuffer(&vb_desc, &vb_data, &vertex_buffer_))) {
        return false;
    }

    D3D11_BUFFER_DESC ib_desc{};
    ib_desc.ByteWidth = sizeof(indices);
    ib_desc.Usage = D3D11_USAGE_IMMUTABLE;
    ib_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;

    D3D11_SUBRESOURCE_DATA ib_data{};
    ib_data.pSysMem = indices;

    if (FAILED(device_->CreateBuffer(&ib_desc, &ib_data, &index_buffer_))) {
        return false;
    }

    D3D11_BUFFER_DESC cb_desc{};
    cb_desc.ByteWidth = sizeof(float) * 8;
    cb_desc.Usage = D3D11_USAGE_DEFAULT;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    if (FAILED(device_->CreateBuffer(&cb_desc, nullptr, &constant_buffer_))) {
        return false;
    }

    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

    if (FAILED(device_->CreateSamplerState(&sampler_desc, &sampler_))) {
        return false;
    }

    D3D11_BUFFER_DESC pointer_vb_desc{};
    pointer_vb_desc.ByteWidth = sizeof(Vertex) * 4;
    pointer_vb_desc.Usage = D3D11_USAGE_DYNAMIC;
    pointer_vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    pointer_vb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateBuffer(&pointer_vb_desc, nullptr, &pointer_vertex_buffer_))) {
        return false;
    }

    D3D11_BLEND_DESC blend_desc{};
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device_->CreateBlendState(&blend_desc, &blend_state_))) {
        return false;
    }

    return true;
}

void MagnifierWindow::ResizeIfNeeded() {
    if (!swap_chain_) {
        return;
    }

    RECT rect{};
    GetClientRect(hwnd_, &rect);
    SIZE new_size{
        rect.right - rect.left,
        rect.bottom - rect.top
    };

    if (new_size.cx <= 0 || new_size.cy <= 0) {
        return;
    }

    if (window_size_.cx == new_size.cx && window_size_.cy == new_size.cy && rtv_) {
        return;
    }

    rtv_.Reset();

    window_size_ = new_size;
    swap_chain_->ResizeBuffers(0, window_size_.cx, window_size_.cy, DXGI_FORMAT_UNKNOWN, 0);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
    if (FAILED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
        return;
    }

    if (FAILED(device_->CreateRenderTargetView(back_buffer.Get(), nullptr, &rtv_))) {
        return;
    }
}

bool MagnifierWindow::UpdateCursorTexture() {
    CURSORINFO cursor_info{};
    cursor_info.cbSize = sizeof(cursor_info);
    if (!GetCursorInfo(&cursor_info) || !(cursor_info.flags & CURSOR_SHOWING)) {
        cursor_visible_ = false;
        return false;
    }

    cursor_visible_ = true;

    if (cursor_info.hCursor != last_cursor_ || !cursor_srv_) {
        HCURSOR cursor_copy = CopyIcon(cursor_info.hCursor);
        if (!cursor_copy) {
            cursor_visible_ = false;
            return false;
        }

        ICONINFO icon_info{};
        if (!GetIconInfo(cursor_copy, &icon_info)) {
            DestroyIcon(cursor_copy);
            cursor_visible_ = false;
            return false;
        }

        BITMAP bmp{};
        int width = 0;
        int height = 0;
        if (icon_info.hbmColor) {
            if (GetObject(icon_info.hbmColor, sizeof(bmp), &bmp) == 0) {
                DeleteObject(icon_info.hbmColor);
                if (icon_info.hbmMask) {
                    DeleteObject(icon_info.hbmMask);
                }
                DestroyIcon(cursor_copy);
                cursor_visible_ = false;
                return false;
            }
            width = bmp.bmWidth;
            height = bmp.bmHeight;
        } else if (icon_info.hbmMask) {
            if (GetObject(icon_info.hbmMask, sizeof(bmp), &bmp) == 0) {
                DeleteObject(icon_info.hbmMask);
                DestroyIcon(cursor_copy);
                cursor_visible_ = false;
                return false;
            }
            width = bmp.bmWidth;
            height = bmp.bmHeight / 2;
        } else {
            DestroyIcon(cursor_copy);
            cursor_visible_ = false;
            return false;
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HDC hdc = CreateCompatibleDC(nullptr);
        if (!hdc) {
            if (icon_info.hbmColor) {
                DeleteObject(icon_info.hbmColor);
            }
            if (icon_info.hbmMask) {
                DeleteObject(icon_info.hbmMask);
            }
            DestroyIcon(cursor_copy);
            cursor_visible_ = false;
            return false;
        }

        HBITMAP dib = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib || !bits) {
            if (dib) {
                DeleteObject(dib);
            }
            DeleteDC(hdc);
            if (icon_info.hbmColor) {
                DeleteObject(icon_info.hbmColor);
            }
            if (icon_info.hbmMask) {
                DeleteObject(icon_info.hbmMask);
            }
            DestroyIcon(cursor_copy);
            cursor_visible_ = false;
            return false;
        }

        HGDIOBJ old = SelectObject(hdc, dib);
        PatBlt(hdc, 0, 0, width, height, BLACKNESS);
        DrawIconEx(hdc, 0, 0, cursor_copy, width, height, 0, nullptr, DI_NORMAL);
        SelectObject(hdc, old);

        std::vector<uint8_t> pixel_data(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        std::memcpy(pixel_data.data(), bits, pixel_data.size());

        // Build alpha from mask to ensure visibility for monochrome cursors (e.g., Excel/Word).
        // If hbmMask exists, extract its top-half (AND mask) and use it to set per-pixel alpha.
        if (icon_info.hbmMask) {
            // hbmMask for color icons has height == icon height; for monochrome it is double-height.
            BITMAP mask_bmp{};
            GetObject(icon_info.hbmMask, sizeof(mask_bmp), &mask_bmp);
            int total_mask_height = mask_bmp.bmHeight;
            int expected_mask_height = height; // AND mask height we need
            bool has_xor_mask = total_mask_height >= (expected_mask_height * 2);

            // Prepare 1bpp DIB for the mask, top-down for easier addressing
            BITMAPINFO bmi_mask{};
            bmi_mask.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi_mask.bmiHeader.biWidth = width;
            bmi_mask.bmiHeader.biHeight = -total_mask_height; // top-down
            bmi_mask.bmiHeader.biPlanes = 1;
            bmi_mask.bmiHeader.biBitCount = 1;
            bmi_mask.bmiHeader.biCompression = BI_RGB;

            int mask_stride = ((width + 31) / 32) * 4; // bytes per row for 1bpp
            std::vector<uint8_t> mask_bits(static_cast<size_t>(mask_stride) * static_cast<size_t>(total_mask_height));
            GetDIBits(hdc, icon_info.hbmMask, 0, static_cast<UINT>(total_mask_height), mask_bits.data(), &bmi_mask, DIB_RGB_COLORS);

            // Top half rows 0..(expected_mask_height-1)
            for (int y = 0; y < height && y < expected_mask_height; ++y) {
                const uint8_t* row = mask_bits.data() + static_cast<size_t>(y) * static_cast<size_t>(mask_stride);
                const uint8_t* xor_row = has_xor_mask
                    ? mask_bits.data() + static_cast<size_t>(y + expected_mask_height) * static_cast<size_t>(mask_stride)
                    : nullptr;
                for (int x = 0; x < width; ++x) {
                    int byte_index = x / 8;
                    int bit_index = 7 - (x % 8);
                    bool mask_on = (row[byte_index] & (1 << bit_index)) != 0; // 1 means background
                    bool xor_on = xor_row ? ((xor_row[byte_index] & (1 << bit_index)) != 0) : false;
                    size_t p = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
                    // Pixels are transparent only when both AND mask keeps the background and XOR mask adds nothing.
                    bool transparent = mask_on && !xor_on;
                    pixel_data[p + 3] = transparent ? 0 : 255;
                }
            }
        } else {
            // No mask: ensure non-zero alpha for any non-black pixel
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    size_t p = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
                    uint8_t b = pixel_data[p + 0];
                    uint8_t g = pixel_data[p + 1];
                    uint8_t r = pixel_data[p + 2];
                    pixel_data[p + 3] = (r | g | b) ? 255 : 0;
                }
            }
        }

        DeleteObject(dib);
        DeleteDC(hdc);

        if (icon_info.hbmColor) {
            DeleteObject(icon_info.hbmColor);
        }
        if (icon_info.hbmMask) {
            DeleteObject(icon_info.hbmMask);
        }
        DestroyIcon(cursor_copy);

        D3D11_TEXTURE2D_DESC tex_desc{};
        tex_desc.Width = static_cast<UINT>(width);
        tex_desc.Height = static_cast<UINT>(height);
        tex_desc.MipLevels = 1;
        tex_desc.ArraySize = 1;
        tex_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        tex_desc.SampleDesc.Count = 1;
        tex_desc.Usage = D3D11_USAGE_IMMUTABLE;
        tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA subresource{};
        subresource.pSysMem = pixel_data.data();
        subresource.SysMemPitch = static_cast<UINT>(width * 4);

        cursor_texture_.Reset();
        cursor_srv_.Reset();

        if (FAILED(device_->CreateTexture2D(&tex_desc, &subresource, &cursor_texture_))) {
            cursor_visible_ = false;
            last_cursor_ = nullptr;
            return false;
        }

        if (FAILED(device_->CreateShaderResourceView(cursor_texture_.Get(), nullptr, &cursor_srv_))) {
            cursor_texture_.Reset();
            cursor_visible_ = false;
            last_cursor_ = nullptr;
            return false;
        }

        cursor_size_.cx = width;
        cursor_size_.cy = height;
        cursor_hotspot_.x = static_cast<LONG>(icon_info.xHotspot);
        cursor_hotspot_.y = static_cast<LONG>(icon_info.yHotspot);
        last_cursor_ = cursor_info.hCursor;
    }

    return cursor_visible_ && cursor_srv_;
}

void MagnifierWindow::DrawCursor(const ViewState& state) {
    if (!cursor_visible_ || !cursor_srv_ || !pointer_vertex_buffer_) {
        return;
    }

    float view_width = static_cast<float>(state.source_region.right - state.source_region.left);
    float view_height = static_cast<float>(state.source_region.bottom - state.source_region.top);
    if (view_width <= 0.0f || view_height <= 0.0f) {
        return;
    }

    float cursor_center_x = state.cursor_x;
    float cursor_center_y = state.cursor_y;
    if (cursor_center_x < static_cast<float>(state.source_region.left) ||
        cursor_center_x > static_cast<float>(state.source_region.right) ||
        cursor_center_y < static_cast<float>(state.source_region.top) ||
        cursor_center_y > static_cast<float>(state.source_region.bottom)) {
        return;
    }

    float cursor_left = cursor_center_x - static_cast<float>(state.source_region.left) - static_cast<float>(cursor_hotspot_.x);
    float cursor_top = cursor_center_y - static_cast<float>(state.source_region.top) - static_cast<float>(cursor_hotspot_.y);

    float scale_x = static_cast<float>(window_size_.cx) / view_width;
    float scale_y = static_cast<float>(window_size_.cy) / view_height;

    float left_px = cursor_left * scale_x;
    float top_px = cursor_top * scale_y;
    float right_px = left_px + static_cast<float>(cursor_size_.cx) * scale_x;
    float bottom_px = top_px + static_cast<float>(cursor_size_.cy) * scale_y;

    if (right_px < 0.0f || bottom_px < 0.0f ||
        left_px > static_cast<float>(window_size_.cx) ||
        top_px > static_cast<float>(window_size_.cy)) {
        return;
    }

    DrawTexturedQuad(cursor_srv_.Get(), left_px, top_px, right_px, bottom_px);
}
void MagnifierWindow::DrawLayoutOverlay() {
    bool layout_drawn = false;

    if (overlay_srv_) {
        if (overlay_expire_tick_ != 0 && GetTickCount64() >= overlay_expire_tick_) {
            overlay_srv_.Reset();
            overlay_texture_.Reset();
            overlay_expire_tick_ = 0;
        } else if (window_size_.cx > 0 && window_size_.cy > 0) {
            float left_px = 0.0f;
            float right_px = std::min(static_cast<float>(overlay_size_.cx), static_cast<float>(window_size_.cx));
            float bottom_px = static_cast<float>(window_size_.cy);
            float top_px = std::max(0.0f, bottom_px - static_cast<float>(overlay_size_.cy));

            if (right_px > left_px && bottom_px > top_px) {
                DrawTexturedQuad(overlay_srv_.Get(), left_px, top_px, right_px, bottom_px);
                layout_drawn = true;
            }
        }
    }

    if (!layout_drawn) {
        DrawStatusOverlay();
    }
}

void MagnifierWindow::DrawStatusOverlay() {
    if (status_overlay_expire_tick_ != 0 && GetTickCount64() >= status_overlay_expire_tick_) {
        status_overlay_expire_tick_ = 0;
        status_overlay_srv_.Reset();
        status_overlay_texture_.Reset();
        return;
    }
    if (!status_overlay_srv_) {
        return;
    }
    if (window_size_.cx <= 0 || window_size_.cy <= 0) {
        return;
    }

    float left_px = 0.0f;
    float right_px = std::min(static_cast<float>(status_overlay_size_.cx), static_cast<float>(window_size_.cx));
    float bottom_px = static_cast<float>(window_size_.cy);
    float top_px = std::max(0.0f, bottom_px - static_cast<float>(status_overlay_size_.cy));

    if (right_px <= left_px || bottom_px <= top_px) {
        return;
    }

    DrawTexturedQuad(status_overlay_srv_.Get(), left_px, top_px, right_px, bottom_px);
}

void MagnifierWindow::DrawTexturedQuad(ID3D11ShaderResourceView* srv, float left_px, float top_px, float right_px, float bottom_px) {
    if (!srv || !pointer_vertex_buffer_ || !context_ || !vertex_buffer_) {
        return;
    }

    if (window_size_.cx <= 0 || window_size_.cy <= 0) {
        return;
    }

    auto to_ndc_x = [this](float px) {
        return px / static_cast<float>(window_size_.cx) * 2.0f - 1.0f;
    };
    auto to_ndc_y = [this](float py) {
        return 1.0f - py / static_cast<float>(window_size_.cy) * 2.0f;
    };

    float left_ndc = to_ndc_x(left_px);
    float right_ndc = to_ndc_x(right_px);
    float top_ndc = to_ndc_y(top_px);
    float bottom_ndc = to_ndc_y(bottom_px);

    struct Vertex {
        float position[3];
        float uv[2];
    };

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(pointer_vertex_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }

    auto* vertices = reinterpret_cast<Vertex*>(mapped.pData);
    vertices[0] = {{left_ndc, bottom_ndc, 0.0f}, {0.0f, 1.0f}};
    vertices[1] = {{left_ndc, top_ndc, 0.0f}, {0.0f, 0.0f}};
    vertices[2] = {{right_ndc, bottom_ndc, 0.0f}, {1.0f, 1.0f}};
    vertices[3] = {{right_ndc, top_ndc, 0.0f}, {1.0f, 0.0f}};

    context_->Unmap(pointer_vertex_buffer_.Get(), 0);

    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    ID3D11Buffer* buffers[] = { pointer_vertex_buffer_.Get() };
    context_->IASetVertexBuffers(0, 1, buffers, &stride, &offset);
    context_->IASetIndexBuffer(index_buffer_.Get(), DXGI_FORMAT_R32_UINT, 0);

    struct ViewConstants {
        float uv_rect[4];
        float render_flags[4];
    } constants{};
    constants.uv_rect[0] = 0.0f;
    constants.uv_rect[1] = 0.0f;
    constants.uv_rect[2] = 1.0f;
    constants.uv_rect[3] = 1.0f;
    constants.render_flags[0] = 0.0f;
    constants.render_flags[1] = 0.0f;
    constants.render_flags[2] = 0.0f;
    constants.render_flags[3] = 0.0f;

    context_->UpdateSubresource(constant_buffer_.Get(), 0, nullptr, &constants, 0, 0);
    ID3D11ShaderResourceView* srv_ptr = srv;
    context_->PSSetShaderResources(0, 1, &srv_ptr);
    context_->OMSetBlendState(blend_state_.Get(), nullptr, 0xFFFFFFFF);

    context_->DrawIndexed(6, 0, 0);

    ID3D11ShaderResourceView* null_srv = nullptr;
    context_->PSSetShaderResources(0, 1, &null_srv);
    context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

    UINT base_stride = sizeof(float) * 5;
    ID3D11Buffer* base_buffers[] = { vertex_buffer_.Get() };
    context_->IASetVertexBuffers(0, 1, base_buffers, &base_stride, &offset);
}

bool MagnifierWindow::CreateOverlayTexture(const std::wstring& text, const SIZE& target_size,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv) {
    if (!device_) {
        return false;
    }

    if (text.empty()) {
        texture.Reset();
        srv.Reset();
        return true;
    }

    const int size = static_cast<int>(std::max<LONG>(1, std::min(target_size.cx, target_size.cy)));

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) {
        return false;
    }

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        if (dib) {
            DeleteObject(dib);
        }
        DeleteDC(hdc);
        return false;
    }

    HGDIOBJ old_bitmap = SelectObject(hdc, dib);

    HBRUSH brush = CreateSolidBrush(RGB(0, 0, 0));
    RECT rect{0, 0, size, size};
    FillRect(hdc, &rect, brush);
    DeleteObject(brush);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));

    std::vector<std::wstring> lines;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t next = text.find(L'\n', pos);
        if (next == std::wstring::npos) {
            lines.emplace_back(text.substr(pos));
            break;
        }
        lines.emplace_back(text.substr(pos, next - pos));
        pos = next + 1;
    }
    if (lines.empty()) {
        lines.emplace_back(text);
    }

    int best_height = size;
    for (int candidate = size; candidate >= 12; candidate -= 2) {
        HFONT test_font = CreateFontW(-candidate, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            VARIABLE_PITCH, L"Segoe UI");
        if (!test_font) {
            continue;
        }
        HGDIOBJ old_font = SelectObject(hdc, test_font);
        TEXTMETRICW metrics{};
        if (!GetTextMetricsW(hdc, &metrics)) {
            SelectObject(hdc, old_font);
            DeleteObject(test_font);
            continue;
        }
        int line_height = metrics.tmHeight;
        int total_height = line_height * static_cast<int>(lines.size());
        int max_width = 0;
        for (const auto& line : lines) {
            SIZE extent{};
            if (!line.empty()) {
                GetTextExtentPoint32W(hdc, line.c_str(), static_cast<int>(line.size()), &extent);
            } else {
                extent.cx = 0;
                extent.cy = line_height;
            }
            if (extent.cx > max_width) {
                max_width = extent.cx;
            }
        }
        SelectObject(hdc, old_font);
        DeleteObject(test_font);
        if (max_width <= size && total_height <= size) {
            best_height = candidate;
            break;
        }
    }

    HFONT final_font = CreateFontW(-best_height, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        VARIABLE_PITCH, L"Segoe UI");
    if (final_font) {
        HGDIOBJ old_font = SelectObject(hdc, final_font);
        TEXTMETRICW metrics{};
        GetTextMetricsW(hdc, &metrics);
        int line_height = metrics.tmHeight;
        int total_height = line_height * static_cast<int>(lines.size());
        int y = (size - total_height) / 2;
        for (const auto& line : lines) {
            SIZE extent{};
            if (!line.empty()) {
                GetTextExtentPoint32W(hdc, line.c_str(), static_cast<int>(line.size()), &extent);
            } else {
                extent.cx = 0;
            }
            int x = (size - extent.cx) / 2;
            TextOutW(hdc, x, y, line.c_str(), static_cast<int>(line.size()));
            y += line_height;
        }
        SelectObject(hdc, old_font);
        DeleteObject(final_font);
    }

    size_t pitch = static_cast<size_t>(size) * 4;
    std::vector<uint8_t> pixel_data(pitch * size);
    std::memcpy(pixel_data.data(), bits, pixel_data.size());

    SelectObject(hdc, old_bitmap);
    DeleteObject(dib);
    DeleteDC(hdc);

    for (size_t i = 0; i < pixel_data.size(); i += 4) {
        pixel_data[i + 3] = 255;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = size;
    desc.Height = size;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA subresource{};
    subresource.pSysMem = pixel_data.data();
    subresource.SysMemPitch = static_cast<UINT>(pitch);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> local_texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> local_srv;

    if (FAILED(device_->CreateTexture2D(&desc, &subresource, &local_texture))) {
        return false;
    }

    if (FAILED(device_->CreateShaderResourceView(local_texture.Get(), nullptr, &local_srv))) {
        return false;
    }

    texture = local_texture;
    srv = local_srv;
    return true;
}

void MagnifierWindow::ShowLayoutOverlay(const std::wstring& text, ULONGLONG duration_ms) {
    if (text.empty() || duration_ms == 0) {
        overlay_srv_.Reset();
        overlay_texture_.Reset();
        overlay_expire_tick_ = 0;
        return;
    }

    if (!CreateOverlayTexture(text, overlay_size_, overlay_texture_, overlay_srv_)) {
        overlay_srv_.Reset();
        overlay_texture_.Reset();
        return;
    }

    overlay_expire_tick_ = GetTickCount64() + duration_ms;
}

void MagnifierWindow::SetStatusBadge(const std::wstring& text, ULONGLONG duration_ms) {
    if (text.empty() || duration_ms == 0) {
        status_overlay_srv_.Reset();
        status_overlay_texture_.Reset();
        status_overlay_expire_tick_ = 0;
        return;
    }

    if (!CreateOverlayTexture(text, status_overlay_size_, status_overlay_texture_, status_overlay_srv_)) {
        status_overlay_srv_.Reset();
        status_overlay_texture_.Reset();
        status_overlay_expire_tick_ = 0;
        return;
    }
    status_overlay_expire_tick_ = GetTickCount64() + duration_ms;
}
