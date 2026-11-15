#pragma once
// Image file IO through WIC: any format Windows can decode (PNG, JPEG, BMP,
// GIF, TIFF, WebP/HEIC with the store codecs) in, PNG/JPEG/BMP/TIFF out.
#include <cstdint>
#include <string>
#include <vector>

struct Image {
    int w = 0, h = 0;
    std::vector<uint32_t> px;   // straight (non-premultiplied) BGRA, row-major
};

bool LoadImageFile(const std::wstring& path, Image& out, std::string& err);
// Encoder is picked from the extension (.png default, .jpg/.jpeg, .bmp, .tif/.tiff).
bool SaveImageFile(const std::wstring& path, const uint32_t* bgra, int w, int h, std::string& err);
