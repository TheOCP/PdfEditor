// Image file IO for Android. Pictures are decoded by the app (Android's own
// decoders) and handed over as pixels, so loading by path is not used here;
// saving writes a plain PNG (stored deflate blocks, no external library).
#include "imageio.h"
#include "util.h"
#include <cstdio>

bool LoadImageFile(const std::wstring&, Image&, std::string& err) {
    err = "not supported on this platform";
    return false;
}

namespace {

uint32_t Crc(const uint8_t* p, size_t n, uint32_t crc = 0xFFFFFFFFu) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

void Put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x);
}

void Chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    Put32(out, (uint32_t)data.size());
    std::vector<uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    Put32(out, Crc(body.data(), body.size()) ^ 0xFFFFFFFFu);
}

} // namespace

bool SaveImageFile(const std::wstring& path, const uint32_t* bgra, int w, int h, std::string& err) {
    if (!bgra || w <= 0 || h <= 0) { err = "empty image"; return false; }
    // Raw scanlines: filter byte 0 + RGBA.
    std::vector<uint8_t> raw;
    raw.reserve((size_t)h * (w * 4 + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        for (int x = 0; x < w; ++x) {
            uint32_t p = bgra[(size_t)y * w + x];
            raw.push_back((uint8_t)(p >> 16)); raw.push_back((uint8_t)(p >> 8));
            raw.push_back((uint8_t)p); raw.push_back((uint8_t)(p >> 24));
        }
    }
    // zlib stream of stored blocks + Adler-32.
    std::vector<uint8_t> z = { 0x78, 0x01 };
    size_t pos = 0;
    while (pos < raw.size() || raw.empty()) {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back((uint8_t)n); z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)~n); z.push_back((uint8_t)(~n >> 8));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
        if (last) break;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    Put32(z, (b << 16) | a);

    std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    std::vector<uint8_t> ihdr;
    Put32(ihdr, (uint32_t)w); Put32(ihdr, (uint32_t)h);
    ihdr.push_back(8); ihdr.push_back(6); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    Chunk(png, "IHDR", ihdr);
    Chunk(png, "IDAT", z);
    Chunk(png, "IEND", {});
    if (!WriteFileBytes(path, png.data(), png.size())) { err = "could not create the output file"; return false; }
    return true;
}
