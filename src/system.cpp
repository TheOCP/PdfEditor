#include "system.h"
#include <ole2.h>        // before commdlg.h: PRINTDLGEX needs the COM declarations
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <algorithm>
#include <cmath>
#include <vector>

// ------------------------------------------------------------------ printing

namespace {

// Pixels rotated 90 degrees clockwise.
std::vector<uint32_t> RotateCW(const std::vector<uint32_t>& src, int w, int h) {
    std::vector<uint32_t> out((size_t)w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            out[(size_t)x * h + (h - 1 - y)] = src[(size_t)y * w + x];
    return out;
}

}  // namespace

PrintResult PrintDocument(HWND owner, Document& doc, int currentPage, const std::wstring& title) {
    PrintResult r;
    int n = doc.PageCount();
    if (n <= 0) { r.error = "nothing to print"; return r; }

    PRINTPAGERANGE ranges[32] = {};
    PRINTDLGEXW pd = {};
    pd.lStructSize = sizeof(pd);
    pd.hwndOwner = owner;
    pd.Flags = PD_RETURNDC | PD_USEDEVMODECOPIESANDCOLLATE | PD_NOSELECTION;
    pd.nMinPage = 1;
    pd.nMaxPage = (DWORD)n;
    pd.nMaxPageRanges = 32;
    pd.lpPageRanges = ranges;
    pd.nCopies = 1;
    pd.nStartPage = START_PAGE_GENERAL;
    HRESULT hr = PrintDlgExW(&pd);
    auto cleanup = [&]() {
        if (pd.hDC) DeleteDC(pd.hDC);
        if (pd.hDevMode) GlobalFree(pd.hDevMode);
        if (pd.hDevNames) GlobalFree(pd.hDevNames);
    };
    if (FAILED(hr)) { r.error = "the print dialog could not be opened"; cleanup(); return r; }
    if (pd.dwResultAction != PD_RESULT_PRINT || !pd.hDC) { r.cancelled = true; cleanup(); return r; }

    std::vector<int> pages;
    if (pd.Flags & PD_CURRENTPAGE) {
        pages.push_back(std::clamp(currentPage, 0, n - 1));
    } else if (pd.Flags & PD_PAGENUMS) {
        for (DWORD k = 0; k < pd.nPageRanges; ++k)
            for (DWORD p = ranges[k].nFromPage; p <= ranges[k].nToPage; ++p)
                if (p >= 1 && p <= (DWORD)n) pages.push_back((int)p - 1);
    } else {
        for (int i = 0; i < n; ++i) pages.push_back(i);
    }
    if (pages.empty()) { r.error = "no pages in that range"; cleanup(); return r; }

    // Copies: the printer driver makes them itself when it can (then nCopies is 1).
    r = PrintPages(pd.hDC, doc, pages, pd.nCopies > 1 ? (int)pd.nCopies : 1, title);
    cleanup();
    return r;
}

PrintResult PrintPages(HDC dc, Document& doc, const std::vector<int>& pages, int copies,
                       const std::wstring& title, const wchar_t* outputFile) {
    PrintResult r;
    DOCINFOW di = {};
    di.cbSize = sizeof(di);
    std::wstring name = title.empty() ? std::wstring(L"PDF") : title;
    di.lpszDocName = name.c_str();
    di.lpszOutput = outputFile;
    if (StartDocW(dc, &di) <= 0) { r.error = "the printer did not accept the job"; return r; }

    int pw = GetDeviceCaps(dc, HORZRES), ph = GetDeviceCaps(dc, VERTRES);
    int dpix = std::max(1, GetDeviceCaps(dc, LOGPIXELSX)), dpiy = std::max(1, GetDeviceCaps(dc, LOGPIXELSY));
    bool paperPortrait = ph >= pw;
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    copies = std::max(1, copies);
    bool failed = false;
    for (int c = 0; c < copies && !failed; ++c) {
        for (int pg : pages) {
            FS_SIZEF sz = doc.PageSizePt(pg);
            if (sz.width <= 0 || sz.height <= 0) continue;
            bool pagePortrait = sz.height >= sz.width;
            bool turn = pagePortrait != paperPortrait && std::fabs(sz.width - sz.height) > 1.0f;
            // Size on paper at 100 %, then fitted into the printable area.
            float wIn = (turn ? sz.height : sz.width) / 72.0f, hIn = (turn ? sz.width : sz.height) / 72.0f;
            float fit = std::min(pw / (wIn * dpix), ph / (hIn * dpiy));
            int dw = std::max(1, (int)std::lround(wIn * dpix * fit));
            int dh = std::max(1, (int)std::lround(hIn * dpiy * fit));
            // Render at the printer's resolution, at most 300 dpi (sharp, bounded memory).
            float dpi = std::min((float)std::max(dpix, dpiy) * fit, 300.0f);
            float scale = dpi / 72.0f;
            int bw = std::max(1, (int)std::lround(sz.width * scale));
            int bh = std::max(1, (int)std::lround(sz.height * scale));
            std::vector<uint32_t> px;
            if (!doc.Render(pg, scale, 0, 0, bw, bh, px, true, true, true)) continue;
            if (turn) { px = RotateCW(px, bw, bh); std::swap(bw, bh); }

            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = bw;
            bi.bmiHeader.biHeight = -bh;              // top-down
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            if (StartPage(dc) <= 0) { failed = true; break; }
            int x = (pw - dw) / 2, y = (ph - dh) / 2;
            StretchDIBits(dc, x, y, dw, dh, 0, 0, bw, bh, px.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
            if (EndPage(dc) <= 0) { failed = true; break; }
            ++r.pages;
        }
    }
    if (failed) { AbortDoc(dc); r.error = "printing stopped"; }
    else EndDoc(dc);
    r.printed = !failed && r.pages > 0;
    return r;
}

// ------------------------------------------------------- default PDF app

namespace {

const wchar_t* kProgId = L"PdfEditor.PDF";
const wchar_t* kAppName = L"PDF Editor";

bool SetStr(const std::wstring& sub, const wchar_t* name, const std::wstring& val) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    LONG e = RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(val.c_str()),
                            (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return e == ERROR_SUCCESS;
}

bool SetEmpty(const std::wstring& sub, const wchar_t* name) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    LONG e = RegSetValueExW(k, name, 0, REG_NONE, nullptr, 0);
    RegCloseKey(k);
    return e == ERROR_SUCCESS;
}

}  // namespace

std::wstring ThisExePath() {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    return std::wstring(buf, n);
}

bool RegisterPdfApp(const std::wstring& base, const std::wstring& exe, std::string& err) {
    if (exe.empty()) { err = "program path unknown"; return false; }
    std::wstring file = exe.substr(exe.find_last_of(L"\\/") + 1);
    std::wstring cls = base + L"\\Classes\\";
    std::wstring cmd = L"\"" + exe + L"\" \"%1\"";
    std::wstring icon = L"\"" + exe + L"\",0";
    std::wstring caps = base + L"\\TheOCProductions\\PdfEditor\\Capabilities";
    bool ok = true;
    // The document type this program opens.
    ok &= SetStr(cls + kProgId, nullptr, L"PDF Document");
    ok &= SetStr(cls + kProgId, L"FriendlyTypeName", L"PDF Document");
    ok &= SetStr(cls + kProgId + L"\\DefaultIcon", nullptr, icon);
    ok &= SetStr(cls + kProgId + L"\\shell\\open\\command", nullptr, cmd);
    // Offered for .pdf in "Open with".
    ok &= SetEmpty(cls + L".pdf\\OpenWithProgids", kProgId);
    ok &= SetStr(cls + L"Applications\\" + file, L"FriendlyAppName", kAppName);
    ok &= SetStr(cls + L"Applications\\" + file + L"\\shell\\open\\command", nullptr, cmd);
    ok &= SetEmpty(cls + L"Applications\\" + file + L"\\SupportedTypes", L".pdf");
    // Listed under Settings > Apps > Default apps.
    ok &= SetStr(caps, L"ApplicationName", kAppName);
    ok &= SetStr(caps, L"ApplicationDescription", L"View and edit PDF documents");
    ok &= SetStr(caps, L"ApplicationIcon", icon);
    ok &= SetStr(caps + L"\\FileAssociations", L".pdf", kProgId);
    ok &= SetStr(base + L"\\RegisteredApplications", kAppName, caps);
    if (!ok) { err = "could not write the registration"; return false; }
    if (base == L"Software") SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}

bool IsDefaultPdfApp() {
    IApplicationAssociationRegistration* reg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ApplicationAssociationRegistration, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&reg))) || !reg)
        return false;
    LPWSTR prog = nullptr;
    bool mine = false;
    if (SUCCEEDED(reg->QueryCurrentDefault(L".pdf", AT_FILEEXTENSION, AL_EFFECTIVE, &prog)) && prog) {
        std::wstring p = prog;
        std::wstring file = ThisExePath();
        file = file.substr(file.find_last_of(L"\\/") + 1);
        mine = _wcsicmp(p.c_str(), kProgId) == 0 || _wcsicmp(p.c_str(), (L"Applications\\" + file).c_str()) == 0;
        CoTaskMemFree(prog);
    }
    reg->Release();
    return mine;
}

void OpenDefaultAppSettings() {
    // Windows 11: straight to this app's page; older builds fall back to the list.
    HINSTANCE h = ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps?registeredAppUser=PDF%20Editor",
                                nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)h <= 32) ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps", nullptr, nullptr, SW_SHOWNORMAL);
}
