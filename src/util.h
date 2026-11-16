#pragma once
// Small helpers shared across the editor: UTF-8 <-> wide text, file IO, rects.
// Windows: wchar_t is UTF-16 (like PDFium). Android/Linux: wchar_t is UTF-32,
// so text is converted at the PDFium boundary (WS / FromUtf16).
#ifdef _WIN32
#include <windows.h>
#else
#include <cstdio>
#include <unistd.h>
#endif
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

#ifdef _WIN32
inline std::string W2U(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

inline std::wstring U2W(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// PDFium speaks UTF-16LE (FPDF_WIDESTRING = const unsigned short*). On Windows
// wchar_t is UTF-16 too, so a wstring's buffer can be passed straight through.
inline const unsigned short* WS(const std::wstring& w) {
    return reinterpret_cast<const unsigned short*>(w.c_str());
}

// UTF-16 from PDFium -> wstring (n = number of code units, or up to the NUL).
inline std::wstring FromUtf16(const unsigned short* p, size_t n = (size_t)-1) {
    if (!p) return {};
    if (n == (size_t)-1) return std::wstring(reinterpret_cast<const wchar_t*>(p));
    return std::wstring(reinterpret_cast<const wchar_t*>(p), n);
}
#else
inline std::string W2U(const std::wstring& w) {
    std::string s;
    for (wchar_t wc : w) {
        uint32_t c = (uint32_t)wc;
        if (c < 0x80) s += (char)c;
        else if (c < 0x800) { s += (char)(0xC0 | (c >> 6)); s += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { s += (char)(0xE0 | (c >> 12)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
        else { s += (char)(0xF0 | (c >> 18)); s += (char)(0x80 | ((c >> 12) & 0x3F)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
    }
    return s;
}

inline std::wstring U2W(const std::string& s) {
    std::wstring w;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; len = 3; }
        else if ((c >> 3) == 30) { cp = c & 0x07; len = 4; }
        else { ++i; continue; }
        if (i + len > s.size()) break;
        for (int k = 1; k < len; ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        w += (wchar_t)cp;
        i += len;
    }
    return w;
}

// PDFium speaks UTF-16LE; wchar_t is UTF-32 here, so WS() hands PDFium a
// converted copy that lives until the end of the full expression (the call).
struct Utf16Arg {
    std::u16string s;
    operator const unsigned short*() const { return reinterpret_cast<const unsigned short*>(s.c_str()); }
};
inline Utf16Arg WS(const std::wstring& w) {
    Utf16Arg a;
    for (wchar_t wc : w) {
        uint32_t c = (uint32_t)wc;
        if (c >= 0x10000) { c -= 0x10000; a.s += (char16_t)(0xD800 + (c >> 10)); a.s += (char16_t)(0xDC00 + (c & 0x3FF)); }
        else a.s += (char16_t)c;
    }
    return a;
}

inline std::wstring FromUtf16(const unsigned short* p, size_t n = (size_t)-1) {
    std::wstring w;
    if (!p) return w;
    for (size_t i = 0; n == (size_t)-1 ? p[i] != 0 : i < n; ++i) {
        uint32_t c = p[i];
        if (c >= 0xD800 && c <= 0xDBFF && (n == (size_t)-1 ? p[i + 1] != 0 : i + 1 < n)) {
            uint32_t d = p[i + 1];
            if (d >= 0xDC00 && d <= 0xDFFF) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); ++i; }
        }
        w += (wchar_t)c;
    }
    return w;
}
#endif

// Reads a PDFium "returns byte length incl. NUL" UTF-16 string getter.
template <typename F>
inline std::wstring ReadUtf16(F getter) {
    unsigned long bytes = getter(nullptr, 0);
    if (bytes <= 2) return {};
    std::vector<unsigned short> buf(bytes / 2 + 1, 0);
    getter(buf.data(), bytes);
    return FromUtf16(buf.data());
}

#ifdef _WIN32
inline bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    _fseeki64(f, 0, SEEK_END);
    long long n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return false; }
    out.resize((size_t)n);
    size_t got = n ? fread(out.data(), 1, (size_t)n, f) : 0;
    fclose(f);
    return got == (size_t)n;
}

// Writes via a temp file + rename so a failed save never truncates the original.
inline bool WriteFileBytes(const std::wstring& path, const void* data, size_t n) {
    std::wstring tmp = path + L".tmp~";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    size_t put = n ? fwrite(data, 1, n, f) : 0;
    bool ok = (put == n) && fflush(f) == 0;
    fclose(f);
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}
#else
inline bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(W2U(path).c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return false; }
    out.resize((size_t)n);
    size_t got = n ? fread(out.data(), 1, (size_t)n, f) : 0;
    fclose(f);
    return got == (size_t)n;
}

inline bool WriteFileBytes(const std::wstring& path, const void* data, size_t n) {
    std::string p = W2U(path), tmp = p + ".tmp~";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    size_t put = n ? fwrite(data, 1, n, f) : 0;
    bool ok = (put == n) && fflush(f) == 0;
    fclose(f);
    if (!ok || rename(tmp.c_str(), p.c_str()) != 0) { unlink(tmp.c_str()); return false; }
    return true;
}
#endif

inline std::wstring FileNameOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

inline std::wstring DirOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? L"" : path.substr(0, p);
}

inline std::wstring StemOf(const std::wstring& path) {
    std::wstring n = FileNameOf(path);
    size_t d = n.find_last_of(L'.');
    return d == std::wstring::npos ? n : n.substr(0, d);
}

inline std::wstring LowerExt(const std::wstring& path) {
    std::wstring n = FileNameOf(path);
    size_t d = n.find_last_of(L'.');
    if (d == std::wstring::npos) return L"";
    std::wstring e = n.substr(d);
    for (auto& c : e) c = (wchar_t)towlower(c);
    return e;
}

// Rect in PDF page space (y up): l < r, b < t.
struct PRect {
    float l = 0, b = 0, r = 0, t = 0;
    float W() const { return r - l; }
    float H() const { return t - b; }
    bool Contains(float x, float y) const { return x >= l && x <= r && y >= b && y <= t; }
    bool Intersects(const PRect& o) const { return !(o.l > r || o.r < l || o.b > t || o.t < b); }
    bool Inside(const PRect& o) const { return l >= o.l && r <= o.r && b >= o.b && t <= o.t; }
    float Area() const { return std::max(0.0f, W()) * std::max(0.0f, H()); }
    PRect Union(const PRect& o) const { return { std::min(l, o.l), std::min(b, o.b), std::max(r, o.r), std::max(t, o.t) }; }
    PRect Intersect(const PRect& o) const { return { std::max(l, o.l), std::max(b, o.b), std::min(r, o.r), std::min(t, o.t) }; }
    static PRect FromPoints(float x0, float y0, float x1, float y1) {
        return { std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1) };
    }
};

inline std::string Fmt(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

// Parses "1-3, 5, 8-" (1-based, inclusive) into 0-based page indices.
inline bool ParsePageRange(const std::string& s, int pageCount, std::vector<int>& out) {
    out.clear();
    size_t i = 0;
    auto skip = [&] { while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i; };
    auto num = [&](int& v) -> bool {
        skip();
        if (i >= s.size() || !isdigit((unsigned char)s[i])) return false;
        v = 0;
        while (i < s.size() && isdigit((unsigned char)s[i])) v = v * 10 + (s[i++] - '0');
        return true;
    };
    while (true) {
        skip();
        if (i >= s.size()) break;
        int a = 0, b = 0;
        if (!num(a)) return false;
        skip();
        b = a;
        if (i < s.size() && s[i] == '-') {
            ++i;
            if (!num(b)) b = pageCount;
        }
        if (a < 1 || b < a || b > pageCount) return false;
        for (int p = a; p <= b; ++p) out.push_back(p - 1);
        skip();
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size()) return false;
    }
    return !out.empty();
}
