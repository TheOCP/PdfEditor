#include "imageio.h"
#include "util.h"
#include <wincodec.h>

namespace {

template <typename T>
struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    operator T*() const { return p; }
};

bool Factory(Com<IWICImagingFactory>& f, std::string& err) {
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    if (FAILED(hr)) { err = Fmt("WIC unavailable (0x%08lX)", (unsigned long)hr); return false; }
    return true;
}

} // namespace

bool LoadImageFile(const std::wstring& path, Image& out, std::string& err) {
    Com<IWICImagingFactory> f;
    if (!Factory(f, err)) return false;
    Com<IWICBitmapDecoder> dec;
    HRESULT hr = f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    if (FAILED(hr)) { err = "not an image Windows can decode"; return false; }
    Com<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) { err = "image has no frames"; return false; }
    Com<IWICFormatConverter> conv;
    if (FAILED(f->CreateFormatConverter(&conv))) { err = "WIC converter failed"; return false; }
    hr = conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                          WICBitmapPaletteTypeMedianCut);
    if (FAILED(hr)) { err = "could not convert image to BGRA"; return false; }
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    if (!w || !h || (uint64_t)w * h > (uint64_t)20000 * 20000) { err = "image size unsupported"; return false; }
    out.w = (int)w;
    out.h = (int)h;
    out.px.assign((size_t)w * h, 0);
    hr = conv->CopyPixels(nullptr, w * 4, (UINT)(out.px.size() * 4), reinterpret_cast<BYTE*>(out.px.data()));
    if (FAILED(hr)) { err = "could not read pixels"; return false; }
    return true;
}

bool SaveImageFile(const std::wstring& path, const uint32_t* bgra, int w, int h, std::string& err) {
    Com<IWICImagingFactory> f;
    if (!Factory(f, err)) return false;
    std::wstring ext = LowerExt(path);
    GUID container = GUID_ContainerFormatPng;
    bool opaque = false;
    if (ext == L".jpg" || ext == L".jpeg") { container = GUID_ContainerFormatJpeg; opaque = true; }
    else if (ext == L".bmp") { container = GUID_ContainerFormatBmp; opaque = true; }
    else if (ext == L".tif" || ext == L".tiff") container = GUID_ContainerFormatTiff;

    std::wstring tmp = path + L".tmp~";
    Com<IWICStream> stream;
    if (FAILED(f->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE))) {
        err = "could not create the output file";
        return false;
    }
    bool ok = false;
    {
        Com<IWICBitmapEncoder> enc;
        Com<IWICBitmapFrameEncode> frame;
        Com<IPropertyBag2> props;
        if (SUCCEEDED(f->CreateEncoder(container, nullptr, &enc)) &&
            SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
            SUCCEEDED(enc->CreateNewFrame(&frame, &props))) {
            if (container == GUID_ContainerFormatJpeg && props) {
                PROPBAG2 opt{};
                opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
                VARIANT v;
                VariantInit(&v);
                v.vt = VT_R4;
                v.fltVal = 0.95f;
                props->Write(1, &opt, &v);
            }
            frame->Initialize(props);
            frame->SetSize((UINT)w, (UINT)h);
            // Formats without alpha get the image composited over white first.
            std::vector<uint32_t> flat;
            const uint32_t* src = bgra;
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
            if (opaque) {
                flat.resize((size_t)w * h);
                for (size_t i = 0; i < flat.size(); ++i) {
                    uint32_t p = bgra[i];
                    uint32_t a = p >> 24;
                    auto mix = [&](uint32_t c) { return (c * a + 255 * (255 - a) + 127) / 255; };
                    flat[i] = 0xFF000000u | (mix((p >> 16) & 255) << 16) | (mix((p >> 8) & 255) << 8) | mix(p & 255);
                }
                src = flat.data();
                fmt = GUID_WICPixelFormat24bppBGR;
            }
            WICPixelFormatGUID want = fmt;
            frame->SetPixelFormat(&want);
            HRESULT hr;
            if (want == GUID_WICPixelFormat32bppBGRA) {
                hr = frame->WritePixels((UINT)h, (UINT)w * 4, (UINT)((size_t)w * h * 4),
                                        reinterpret_cast<BYTE*>(const_cast<uint32_t*>(src)));
            } else {
                // Encoder wants something else (24bpp BGR for JPEG/BMP): pack it.
                std::vector<BYTE> rgb((size_t)w * h * 3);
                for (size_t i = 0; i < (size_t)w * h; ++i) {
                    rgb[i * 3 + 0] = (BYTE)(src[i] & 255);
                    rgb[i * 3 + 1] = (BYTE)((src[i] >> 8) & 255);
                    rgb[i * 3 + 2] = (BYTE)((src[i] >> 16) & 255);
                }
                UINT stride = (UINT)w * 3;
                hr = frame->WritePixels((UINT)h, stride, (UINT)rgb.size(), rgb.data());
            }
            ok = SUCCEEDED(hr) && SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
        }
    }
    if (stream.p) { stream.p->Release(); stream.p = nullptr; }   // closes the file before the rename
    if (!ok) {
        DeleteFileW(tmp.c_str());
        err = "image encoder failed";
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        err = "could not replace the output file";
        return false;
    }
    return true;
}
