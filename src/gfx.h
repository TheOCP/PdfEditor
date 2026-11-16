#pragma once
// D3D11 device / swap chain owned by the window, plus a small BGRA texture
// wrapper the canvas and thumbnails upload rendered pages into.
#include <windows.h>
#include <d3d11.h>
#include <cstdint>
#include <vector>
#include "imgui.h"

bool GfxCreate(HWND hwnd);
void GfxDestroy();
void GfxRequestResize(UINT w, UINT h);
// Applies a pending resize, binds + clears the back buffer.
void GfxBeginFrame(const float clear[4]);
// Returns false while the window is occluded/minimized.
bool GfxPresent();
bool GfxOccluded();
ID3D11Device* GfxDevice();
ID3D11DeviceContext* GfxContext();
// Copies the rendered back buffer (before Present) into BGRA pixels.
bool GfxCaptureBackbuffer(std::vector<uint32_t>& bgra, int& w, int& h);

class Texture {
public:
    Texture() = default;
    ~Texture() { Release(); }
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&& o) noexcept { *this = static_cast<Texture&&>(o); }
    Texture& operator=(Texture&& o) noexcept {
        if (this != &o) {
            Release();
            tex_ = o.tex_; srv_ = o.srv_; w_ = o.w_; h_ = o.h_;
            o.tex_ = nullptr; o.srv_ = nullptr; o.w_ = o.h_ = 0;
        }
        return *this;
    }

    // Uploads a tightly packed BGRA image, (re)creating the texture on size change.
    bool Upload(const uint32_t* bgra, int w, int h);
    void Release();
    bool Valid() const { return srv_ != nullptr; }
    int W() const { return w_; }
    int H() const { return h_; }
    ImTextureID Id() const { return (ImTextureID)(uintptr_t)srv_; }

private:
    ID3D11Texture2D* tex_ = nullptr;
    ID3D11ShaderResourceView* srv_ = nullptr;
    int w_ = 0, h_ = 0;
};
