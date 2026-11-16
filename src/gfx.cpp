#include "gfx.h"

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static UINT g_resizeW = 0, g_resizeH = 0;
static bool g_occluded = false;

static void CreateRT() {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

static void DestroyRT() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

bool GfxCreate(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                           D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (hr != S_OK) return false;
    CreateRT();
    return true;
}

void GfxDestroy() {
    DestroyRT();
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}

void GfxRequestResize(UINT w, UINT h) { g_resizeW = w; g_resizeH = h; }

void GfxBeginFrame(const float clear[4]) {
    if (g_resizeW && g_resizeH) {
        DestroyRT();
        g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
        g_resizeW = g_resizeH = 0;
        CreateRT();
    }
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_ctx->ClearRenderTargetView(g_rtv, clear);
}

bool GfxCaptureBackbuffer(std::vector<uint32_t>& bgra, int& w, int& h) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return false;
    D3D11_TEXTURE2D_DESC d{};
    back->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    bool ok = SUCCEEDED(g_dev->CreateTexture2D(&d, nullptr, &staging));
    if (ok) {
        g_ctx->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE m{};
        ok = SUCCEEDED(g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m));
        if (ok) {
            w = (int)d.Width;
            h = (int)d.Height;
            bgra.resize((size_t)w * h);
            for (int y = 0; y < h; ++y) {
                const uint8_t* row = static_cast<const uint8_t*>(m.pData) + (size_t)y * m.RowPitch;
                for (int x = 0; x < w; ++x) {
                    const uint8_t* p = row + x * 4;   // R G B A
                    bgra[(size_t)y * w + x] = 0xFF000000u | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
                }
            }
            g_ctx->Unmap(staging, 0);
        }
        staging->Release();
    }
    back->Release();
    return ok;
}

bool GfxPresent() {
    HRESULT hr = g_swap->Present(1, 0);
    g_occluded = (hr == DXGI_STATUS_OCCLUDED);
    return !g_occluded;
}

bool GfxOccluded() {
    if (!g_occluded) return false;
    if (g_swap->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) return true;
    g_occluded = false;
    return false;
}

ID3D11Device* GfxDevice() { return g_dev; }
ID3D11DeviceContext* GfxContext() { return g_ctx; }

bool Texture::Upload(const uint32_t* bgra, int w, int h) {
    if (!g_dev || w <= 0 || h <= 0) return false;
    if (!tex_ || w != w_ || h != h_) {
        Release();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = (UINT)w;
        d.Height = (UINT)h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{ bgra, (UINT)(w * 4), 0 };
        if (FAILED(g_dev->CreateTexture2D(&d, &init, &tex_))) { tex_ = nullptr; return false; }
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = d.Format;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        if (FAILED(g_dev->CreateShaderResourceView(tex_, &sv, &srv_))) { Release(); return false; }
        w_ = w;
        h_ = h;
        return true;
    }
    g_ctx->UpdateSubresource(tex_, 0, nullptr, bgra, (UINT)(w * 4), 0);
    return true;
}

void Texture::Release() {
    if (srv_) { srv_->Release(); srv_ = nullptr; }
    if (tex_) { tex_->Release(); tex_ = nullptr; }
    w_ = h_ = 0;
}
