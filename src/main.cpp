// PDF Editor - Win32 + DirectX 11 + Dear ImGui host.
#include "app.h"
#include "gfx.h"
#include "i18n.h"
#include "icons.h"
#include "imageio.h"
#include "theme.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <objbase.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <cstdio>
#include <string>

int RunSelfTest();   // selftest.cpp

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static App g_app;
static float g_dpi = 1.0f;            // Windows scaling of the current monitor
static float g_pendingDpi = 0;        // set by WM_DPICHANGED, applied between frames
static bool g_shots = false;          // --ui-shots: the hidden window may be larger than the screen

static std::string FontPath(const wchar_t* file) {
    wchar_t win[MAX_PATH] = L"C:\\Windows";
    GetWindowsDirectoryW(win, MAX_PATH);
    std::wstring p = std::wstring(win) + L"\\Fonts\\" + file;
    return GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES ? std::string() : W2U(p);
}

// UI fonts at the monitor's scale times the user's interface size:
// Segoe UI (Latin incl. äöüß, Greek, Cyrillic, punctuation, currency) with the
// Windows icon font merged in, plus Segoe UI Semibold for titles.
static void BuildFonts(float dpi) {
    static const ImWchar kRanges[] = {
        0x0020, 0x024F, 0x0370, 0x03FF, 0x0400, 0x052F, 0x1E00, 0x1EFF,
        0x2000, 0x206F, 0x20A0, 0x20CF, 0x2100, 0x214F, 0x2190, 0x21FF,
        0x2200, 0x22FF, 0x2500, 0x25FF, 0,
    };
    float factor = UiScaleSetting() > 0 ? UiScaleSetting() : 1.0f;
    float scale = dpi * factor;
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    float size = 17.0f * scale;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    std::string ui = FontPath(L"segoeui.ttf");
    ImFont* main = ui.empty() ? nullptr : io.Fonts->AddFontFromFileTTF(ui.c_str(), size, &cfg, kRanges);
    if (!main) {
        ImFontConfig def;
        def.SizePixels = 13.0f * scale;
        io.Fonts->AddFontDefault(&def);
    }
    std::string icons = FontPath(L"SegoeIcons.ttf");          // Windows 11
    if (icons.empty()) icons = FontPath(L"segmdl2.ttf");     // Windows 10
    if (main && !icons.empty()) {
        ImFontConfig ic;
        ic.MergeMode = true;
        ic.PixelSnapH = true;
        ic.GlyphMinAdvanceX = size;
        ic.GlyphOffset = ImVec2(0, 2.0f * scale);
        io.Fonts->AddFontFromFileTTF(icons.c_str(), size * 0.95f, &ic, reinterpret_cast<const ImWchar*>(kIconRanges));
    }
    std::string semi = FontPath(L"seguisb.ttf");
    g_app.fontTitle = semi.empty() ? nullptr : io.Fonts->AddFontFromFileTTF(semi.c_str(), 30.0f * scale, &cfg, kRanges);
    ImGui::GetStyle() = ImGuiStyle();
    ApplyDarkTheme();
    ImGui::GetStyle().ScaleAllSizes(scale);
    g_app.dpiScale = scale;
}

// Title bar in the editor's own dark grey, so the window frame and the app
// read as one piece. Windows 11 honours the caption colour; Windows 10 at
// least gets the dark frame.
static void ApplyTitleBarTheme(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    COLORREF caption = RGB(30, 30, 35);    // theme Panel
    COLORREF text = RGB(232, 234, 239);    // theme Text
    COLORREF border = RGB(51, 51, 60);     // theme Line
    DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, 36 /*DWMWA_TEXT_COLOR*/, &text, sizeof(text));
    DwmSetWindowAttribute(hwnd, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof(border));
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) GfxRequestResize((UINT)LOWORD(lParam), (UINT)HIWORD(lParam));
        return 0;
    case WM_GETMINMAXINFO: {            // small enough for any screen, big enough to stay usable
        MINMAXINFO* mm = reinterpret_cast<MINMAXINFO*>(lParam);
        float d = GetDpiForWindow(hWnd) / 96.0f;
        mm->ptMinTrackSize.x = (LONG)(640 * d);
        mm->ptMinTrackSize.y = (LONG)(420 * d);
        if (g_shots) mm->ptMaxTrackSize.x = mm->ptMaxTrackSize.y = 16000;
        return 0;
    }
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wParam;
        wchar_t path[MAX_PATH];
        if (DragQueryFileW(drop, 0, path, MAX_PATH)) g_app.OpenPath(path);
        DragFinish(drop);
        return 0;
    }
    case WM_DPICHANGED: {
        g_pendingDpi = HIWORD(wParam) / 96.0f;
        const RECT* r = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hWnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_CLOSE:
        if (g_app.ConfirmDiscard()) DestroyWindow(hWnd);   // asks to save unsaved changes
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void RenderFrame(const float clear[4]) {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    g_app.Draw();
    ImGui::Render();
    GfxBeginFrame(clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

// --ui-shots <folder> [pdf]: renders every screen and dialog, in English and
// German, at several window sizes and scales, into PNG files, and lists any
// window whose content is wider than its visible area (= cut-off text) in
// overflow.txt. Development aid; nothing is shown on screen.
static int RunUiShots(HWND hwnd, const std::wstring& dir, const std::wstring& pdf) {
    SetLogMissingTranslations(true);
    CreateDirectoryW(dir.c_str(), nullptr);
    FILE* report = _wfopen((dir + L"\\overflow.txt").c_str(), L"wb");
    const float clear[4] = { 0.06f, 0.06f, 0.07f, 1.00f };
    struct Cfg { int w, h; float scale; };
    const Cfg cfgs[] = { { 1280, 720, 1.0f }, { 1280, 720, 1.5f }, { 1920, 1080, 1.0f }, { 1920, 1080, 1.5f },
                         { 2560, 1440, 1.0f }, { 3840, 2160, 2.0f }, { 1024, 700, 1.0f } };
    const char* states[] = { "welcome", "main", "text", "group", "toast", "help", "settings", "table", "meta",
                             "about", "import", "edit", "password", "line", "lines" };
    SetUiScaleSetting(1.0f);
    for (int li = 0; li < 2; ++li) {
        g_app.ChangeLanguage(li ? Lang::German : Lang::English, false);   // never saved
        for (const Cfg& c : cfgs) {
            RECT r{ 0, 0, c.w, c.h };
            AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
            SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
            BuildFonts(c.scale);
            ImGui_ImplDX11_InvalidateDeviceObjects();
            g_app.InvalidateAll();
            g_app.thumbsWUser = g_app.propsWUser = 0;
            for (const char* st : states) {
                std::string name = st;
                if (GImGui->OpenPopupStack.Size > 0) ImGui::ClosePopupToLevel(0, false);
                g_app.CancelInlineEdit();
                g_app.toast.text.clear();
                g_app.uiShotOpenAll = false;
                if (name == "welcome") {
                    g_app.doc.Close();
                    g_app.cache.clear();
                    g_app.thumbs.clear();
                } else if (!g_app.doc.Raw() && !pdf.empty()) {
                    g_app.OpenPath(pdf);
                    g_app.toast.text.clear();
                }
                g_app.ClearSelection();
                FPDF_PAGEOBJECT firstText = nullptr;
                if (g_app.doc.Raw())
                    for (auto& o : ops::ListObjects(g_app.doc, 0))
                        if (o.type == FPDF_PAGEOBJ_TEXT && !o.parent) { firstText = o.obj; break; }
                if (name == "text" && firstText) {
                    for (auto& o : ops::ListObjects(g_app.doc, 0))
                        if (o.obj == firstText) { g_app.sel.Set(g_app.doc, 0, o); break; }
                    g_app.groupSel = { firstText };
                    g_app.uiShotOpenAll = true;
                } else if (name == "group") {
                    g_app.SelectAllOnPage(false);
                    g_app.toast.text.clear();
                } else if (name == "toast") {
                    g_app.Toasts(Fmt(T("%s is not in the font %s - drawn with %s instead"), "\"8\"", "AllAndNone", "Segoe UI Light"));
                } else if (name == "help") g_app.showHelp = true;
                else if (name == "settings") g_app.showSettings = true;
                else if (name == "table") g_app.showTable = true;
                else if (name == "meta") g_app.showMeta = true;
                else if (name == "about") g_app.showAbout = true;
                else if (name == "import") { g_app.importFile = L"C:\\Users\\Name\\Documents\\Another document.pdf"; g_app.showImport = true; }
                else if (name == "edit" && firstText) g_app.BeginEditText(0, firstText);
                else if (name == "line" || name == "lines") {
                    // A blank page with a table grid: one line / every line selected.
                    g_app.doc.CreateBlank(595, 842);
                    g_app.cache.clear();
                    g_app.thumbs.clear();
                    ops::AddTable(g_app.doc, 0, PRect{ 60, 500, 535, 780 }, 4, 3, true, 24, 0, 0, 0, 0.5f);
                    g_app.doc.Commit("table");
                    if (name == "lines") {
                        g_app.SelectAllOnPage(true);
                    } else {
                        for (auto& o : ops::ListObjects(g_app.doc, 0))
                            if (o.type == FPDF_PAGEOBJ_PATH) { g_app.sel.Set(g_app.doc, 0, o); g_app.groupSel = { o.obj }; break; }
                    }
                    g_app.toast.text.clear();
                    g_app.fitPending = true;
                }
                else if (name == "password") {
                    g_app.pwFile = L"x.pdf";
                    g_app.pwError = "Wrong password, try again.";
                    g_app.showPassword = true;
                }
                for (int f = 0; f < 6; ++f) {
                    RenderFrame(clear);
                    if (f < 5) GfxPresent();
                }
                // Cut-off check: content wider than the space a window shows.
                ImGuiContext& g = *GImGui;
                for (ImGuiWindow* w : g.Windows) {
                    if (!w->WasActive || w->Hidden) continue;
                    if (w->Flags & ImGuiWindowFlags_HorizontalScrollbar) continue;
                    float visible = w->InnerRect.GetWidth() - w->WindowPadding.x * 2.0f;
                    float content = w->ContentSizeIdeal.x;
                    if (content > visible + 2.0f && report)
                        fprintf(report, "%s %dx%d@%.0f%% %-9s  %-40s content %.0f > visible %.0f\r\n",
                                li ? "de" : "en", c.w, c.h, c.scale * 100, st, w->Name, content, visible);
                }
                std::vector<uint32_t> px;
                int w = 0, h = 0;
                if (GfxCaptureBackbuffer(px, w, h)) {
                    wchar_t file[MAX_PATH];
                    swprintf(file, MAX_PATH, L"%s\\%s_%dx%d_%d_%hs.png", dir.c_str(), li ? L"de" : L"en", c.w, c.h,
                             (int)(c.scale * 100), st);
                    std::string err;
                    SaveImageFile(file, px.data(), w, h, err);
                }
                GfxPresent();
            }
        }
    }
    if (report) fclose(report);
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);   // WIC needs COM, self-test too
    if (lpCmdLine && wcsstr(lpCmdLine, L"--selftest")) {
        int rc = RunSelfTest();
        CoUninitialize();
        return rc;
    }
    LoadSettings();
    PdfInit();

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring shotsDir, shotsPdf, openFile;
    for (int i = 1; argv && i < argc; ++i) {
        if (wcscmp(argv[i], L"--ui-shots") == 0 && i + 1 < argc) {
            shotsDir = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != L'-') shotsPdf = argv[++i];
        } else if (argv[i][0] != L'-' && GetFileAttributesW(argv[i]) != INVALID_FILE_ATTRIBUTES && openFile.empty()) {
            openFile = argv[i];
        }
    }
    if (argv) LocalFree(argv);
    bool shots = !shotsDir.empty();
    g_shots = shots;

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance,
                       LoadIconW(hInstance, MAKEINTRESOURCEW(1)), LoadCursorW(nullptr, IDC_ARROW),
                       nullptr, nullptr, L"PdfEditorWnd", LoadIconW(hInstance, MAKEINTRESOURCEW(1)) };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, TW("PDF Editor").c_str(), WS_OVERLAPPEDWINDOW,
                              80, 60, 1400, 900, nullptr, nullptr, wc.hInstance, nullptr);

    if (!GfxCreate(hwnd)) {
        GfxDestroy();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        MessageBoxW(nullptr, TW("Could not initialise Direct3D 11.").c_str(), TW("PDF Editor").c_str(), MB_ICONERROR);
        return 1;
    }

    ApplyTitleBarTheme(hwnd);
    if (!shots) {
        ShowWindow(hwnd, SW_SHOWMAXIMIZED);
        UpdateWindow(hwnd);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;    // keyboard nav stays off so arrows nudge the selection
    g_dpi = GetDpiForWindow(hwnd) / 96.0f;
    BuildFonts(g_dpi);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(GfxDevice(), GfxContext());
    g_app.Init(hwnd);

    if (shots) {
        int rc = RunUiShots(hwnd, shotsDir, shotsPdf);
        g_app.Shutdown();
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        GfxDestroy();
        PdfShutdown();
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        CoUninitialize();
        return rc;
    }
    if (!openFile.empty()) g_app.OpenPath(openFile);

    const float clear[4] = { 0.06f, 0.06f, 0.07f, 1.00f };
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (GfxOccluded()) { Sleep(10); continue; }

        if (g_pendingDpi > 0 || g_app.uiScaleChanged) {    // other monitor scale / interface size
            if (g_pendingDpi > 0) g_dpi = g_pendingDpi;
            BuildFonts(g_dpi);
            ImGui_ImplDX11_InvalidateDeviceObjects();
            g_app.InvalidateAll();
            g_pendingDpi = 0;
            g_app.uiScaleChanged = false;
        }

        RenderFrame(clear);
        GfxPresent();
    }

    g_app.Shutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    GfxDestroy();
    PdfShutdown();
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();
    return 0;
}
