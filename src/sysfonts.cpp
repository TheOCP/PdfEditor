#include "sysfonts.h"
#include <shlobj.h>
#include <algorithm>
#include <cctype>
#include <cstring>

namespace {

std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string NoSpaces(const std::string& s) {
    std::string o;
    for (char c : s)
        if (c != ' ' && c != '_' && c != '-') o += c;
    return o;
}

bool EndsWith(const std::string& s, const std::string& t) {
    return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

// Style words that can trail a family name, longest first so "semibold" wins over "bold".
const char* kStyleWords[] = {
    "extralight", "ultralight", "semilight", "demibold", "semibold", "extrabold", "ultrabold",
    "condensed", "oblique", "italic", "regular", "medium", "light",
    "black", "heavy", "book", "thin", "bold", "normal",
};

// Splits a normalized (lowercase, no spaces) name into family + style keys.
void SplitStyle(std::string n, std::string& family, std::string& style) {
    style.clear();
    for (const char* suf : { "psmt", "mt", "ps" })
        if (EndsWith(n, suf) && n.size() > strlen(suf) + 2) { n.resize(n.size() - strlen(suf)); break; }
    bool again = true;
    while (again) {
        again = false;
        for (const char* w : kStyleWords) {
            std::string sw = w;
            if (n.size() > sw.size() + 1 && EndsWith(n, sw)) {
                n.resize(n.size() - sw.size());
                style = sw + style;
                again = true;
                break;
            }
        }
        for (const char* suf : { "psmt", "mt", "ps" })
            if (EndsWith(n, suf) && n.size() > strlen(suf) + 2) { n.resize(n.size() - strlen(suf)); again = true; break; }
    }
    family = n;
}

std::string CanonStyle(const std::string& s) {
    std::string t = s;
    auto rep = [&](const char* a, const char* b) {
        size_t p;
        while ((p = t.find(a)) != std::string::npos) t.replace(p, strlen(a), b);
    };
    rep("oblique", "italic");
    rep("regular", "");
    rep("normal", "");
    rep("roman", "");
    rep("book", "");
    rep("demibold", "semibold");
    return t;
}

bool HasBold(const std::string& s) {
    return s.find("bold") != std::string::npos || s.find("black") != std::string::npos ||
           s.find("heavy") != std::string::npos;
}
bool HasItalic(const std::string& s) { return s.find("italic") != std::string::npos; }

void ReadFontKey(HKEY root, bool absolutePaths, std::vector<SysFont>& out) {
    HKEY k;
    if (RegOpenKeyExW(root, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", 0, KEY_READ, &k) != ERROR_SUCCESS)
        return;
    wchar_t winFonts[MAX_PATH] = L"C:\\Windows\\Fonts";
    SHGetFolderPathW(nullptr, CSIDL_FONTS, nullptr, 0, winFonts);
    for (DWORD i = 0;; ++i) {
        wchar_t name[512];
        BYTE data[2048];
        DWORD nameLen = 512, dataLen = sizeof(data) - 2, type = 0;
        LONG r = RegEnumValueW(k, i, name, &nameLen, nullptr, &type, data, &dataLen);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS || type != REG_SZ) continue;
        data[dataLen] = 0;
        data[dataLen + 1] = 0;
        std::wstring file = reinterpret_cast<wchar_t*>(data);
        if (LowerExt(file) != L".ttf") continue;            // TTC/OTF can't be embedded as FontFile2
        if (file.find(L'\\') == std::wstring::npos || !absolutePaths)
            if (file.find(L':') == std::wstring::npos) file = std::wstring(winFonts) + L"\\" + file;
        if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        std::string disp = W2U(name);
        size_t paren = disp.find(" (");
        if (paren != std::string::npos) disp.resize(paren);
        if (disp.find('&') != std::string::npos) continue;   // multi-face entries
        SysFont f;
        f.name = disp;
        f.file = file;
        SplitStyle(Lower(NoSpaces(disp)), f.family, f.style);
        f.style = CanonStyle(f.style);
        out.push_back(std::move(f));
    }
    RegCloseKey(k);
}

} // namespace

const std::vector<SysFont>& SystemFonts() {
    static std::vector<SysFont> fonts = [] {
        std::vector<SysFont> v;
        ReadFontKey(HKEY_LOCAL_MACHINE, false, v);
        ReadFontKey(HKEY_CURRENT_USER, true, v);
        std::sort(v.begin(), v.end(), [](const SysFont& a, const SysFont& b) { return Lower(a.name) < Lower(b.name); });
        v.erase(std::unique(v.begin(), v.end(), [](const SysFont& a, const SysFont& b) { return a.name == b.name; }), v.end());
        return v;
    }();
    return fonts;
}

const std::vector<FontChoice>& StandardFonts() {
    static std::vector<FontChoice> v = [] {
        std::vector<FontChoice> r;
        for (const char* n : { "Helvetica", "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique",
                               "Times-Roman", "Times-Bold", "Times-Italic", "Times-BoldItalic",
                               "Courier", "Courier-Bold", "Courier-Oblique", "Courier-BoldOblique",
                               "Symbol", "ZapfDingbats" }) {
            FontChoice c;
            c.stdName = n;
            c.label = std::string(n) + "  (standard, not embedded)";
            r.push_back(c);
        }
        return r;
    }();
    return v;
}

std::string StripSubsetPrefix(const std::string& s) {
    if (s.size() > 7 && s[6] == '+') {
        bool upper = true;
        for (int i = 0; i < 6; ++i) upper &= (s[i] >= 'A' && s[i] <= 'Z');
        if (upper) return s.substr(7);
    }
    return s;
}

FontChoice ChoiceFor(const SysFont& f) {
    FontChoice c;
    c.file = f.file;
    c.label = f.name;
    return c;
}

const SysFont* MatchSystemFont(const std::string& pdfName, bool* exact) {
    if (exact) *exact = false;
    const auto& fonts = SystemFonts();
    if (fonts.empty()) return nullptr;

    std::string n = Lower(StripSubsetPrefix(pdfName));
    std::string famPart = n, stylePart;
    size_t sep = n.find_first_of("-,");
    if (sep != std::string::npos) { famPart = n.substr(0, sep); stylePart = n.substr(sep + 1); }
    std::string family, style2;
    SplitStyle(NoSpaces(famPart), family, style2);
    std::string sp = NoSpaces(stylePart);
    for (const char* suf : { "psmt", "mt", "ps" })
        if (EndsWith(sp, suf)) { sp.resize(sp.size() - strlen(suf)); break; }
    std::string style = CanonStyle(style2 + sp);

    // Generic PDF names -> the Windows faces that ship with every install.
    struct Alias { const char* from; const char* to; };
    const Alias aliases[] = {
        { "helvetica", "arial" }, { "helveticaneue", "arial" }, { "arialmt", "arial" },
        { "times", "timesnewroman" }, { "timesroman", "timesnewroman" }, { "timesnewromanps", "timesnewroman" },
        { "courier", "couriernew" }, { "couriernewps", "couriernew" },
    };
    auto findFamily = [&](const std::string& fam) {
        std::vector<const SysFont*> v;
        for (auto& f : fonts)
            if (f.family == fam) v.push_back(&f);
        return v;
    };
    auto cands = findFamily(family);
    bool byName = !cands.empty();
    if (cands.empty())
        for (auto& a : aliases)
            if (family == a.from) { cands = findFamily(a.to); break; }
    if (cands.empty()) {
        // Last resort keeps the look roughly right: serif -> Times New Roman, mono -> Courier New, else Arial.
        const char* fb = "arial";
        if (family.find("times") != std::string::npos || family.find("serif") != std::string::npos ||
            family.find("roman") != std::string::npos || family.find("georgia") != std::string::npos)
            fb = "timesnewroman";
        if (family.find("mono") != std::string::npos || family.find("courier") != std::string::npos ||
            family.find("consol") != std::string::npos)
            fb = "couriernew";
        cands = findFamily(fb);
    }
    if (cands.empty()) return nullptr;
    if (exact) *exact = byName;

    const SysFont* best = nullptr;
    int bestScore = -1000;
    for (auto* f : cands) {
        int score = 0;
        if (f->style == style) score += 100;
        if (HasBold(f->style) == HasBold(style)) score += 20;
        if (HasItalic(f->style) == HasItalic(style)) score += 20;
        score -= (int)f->style.size();   // prefer the plainest face among equals
        if (score > bestScore) { bestScore = score; best = f; }
    }
    return best;
}
