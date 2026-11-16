// PDF Editor - Win32 + DirectX 11 + Dear ImGui host.
#include "app.h"
#include "gfx.h"
#include "theme.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <objbase.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <string>

int RunSelfTest();   // selftest.cpp

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static App g_app;
static float g_pendingDpiScale = 0;   // set by WM_DPICHANGED, applied between frames

// UI font: Segoe UI at the monitor's scale, with Latin (incl. äöüß), Greek,
// Cyrillic, punctuation and currency (€) so text from the PDF shows correctly
// in the edit boxes. Falls back to ImGui's built-in font if Segoe UI is missing.
static void BuildFonts(float scale) {
    static const ImWchar kRanges[] = {
        0x0020, 0x024F, 0x0370, 0x03FF, 0x0400, 0x052F, 0x1E00, 0x1EFF,
        0x2000, 0x206F, 0x20A0, 0x20CF, 0x2100, 0x214F, 0x2190, 0x21FF,
        0x2200, 0x22FF, 0x2500, 0x25FF, 0,
    };
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    wchar_t win[MAX_PATH] = L"C:\\Windows";
    GetWindowsDirectoryW(win, MAX_PATH);
    std::string path = W2U(std::wstring(win) + L"\\Fonts\\segoeui.ttf");
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES ||
        !io.Fonts->AddFontFromFileTTF(path.c_str(), 17.0f * scale, &cfg, kRanges)) {
        ImFontConfig def;
        def.SizePixels = 13.0f * scale;
        io.Fonts->AddFontDefault(&def);
    }
    ImGui::GetStyle() = ImGuiStyle();
    ApplyDarkTheme();
    ImGui::GetStyle().ScaleAllSizes(scale);
    g_app.dpiScale = scale;
}

// Title bar in the editor's own dark grey (the menu bar colour), so the window
// frame and the app read as one piece. Windows 11 honours the caption colour;
// Windows 10 at least gets the dark frame.
static void ApplyTitleBarTheme(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    COLORREF caption = RGB(28, 28, 33);    // theme MenuBarBg (0.11, 0.11, 0.13)
    COLORREF text = RGB(230, 232, 237);    // theme Text
    COLORREF border = RGB(46, 46, 54);     // theme Border
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
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wParam;
        wchar_t path[MAX_PATH];
        if (DragQueryFileW(drop, 0, path, MAX_PATH)) g_app.OpenPath(path);
        DragFinish(drop);
        return 0;
    }
    case WM_DPICHANGED: {
        g_pendingDpiScale = HIWORD(wParam) / 96.0f;
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

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int) {
    if (lpCmdLine && wcsstr(lpCmdLine, L"--selftest"))
        return RunSelfTest();

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    PdfInit();

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance,
                       LoadIconW(hInstance, MAKEINTRESOURCEW(1)), LoadCursorW(nullptr, IDC_ARROW),
                       nullptr, nullptr, L"PdfEditorWnd", LoadIconW(hInstance, MAKEINTRESOURCEW(1)) };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"PDF Editor", WS_OVERLAPPEDWINDOW,
                              80, 60, 1400, 900, nullptr, nullptr, wc.hInstance, nullptr);

    if (!GfxCreate(hwnd)) {
        GfxDestroy();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        MessageBoxW(nullptr, L"Could not initialise Direct3D 11.", L"PDF Editor", MB_ICONERROR);
        return 1;
    }

    ApplyTitleBarTheme(hwnd);
    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;    // keyboard nav stays off so arrows nudge the selection
    BuildFonts(GetDpiForWindow(hwnd) / 96.0f);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(GfxDevice(), GfxContext());
    g_app.Init(hwnd);

    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            for (int i = 1; i < argc; ++i)
                if (argv[i][0] != L'-' && GetFileAttributesW(argv[i]) != INVALID_FILE_ATTRIBUTES) {
                    g_app.OpenPath(argv[i]);
                    break;
                }
            LocalFree(argv);
        }
    }

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

        if (g_pendingDpiScale > 0) {           // moved to a monitor with another scale
            BuildFonts(g_pendingDpiScale);
            ImGui_ImplDX11_InvalidateDeviceObjects();
            g_app.InvalidateAll();
            g_pendingDpiScale = 0;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        g_app.Draw();

        ImGui::Render();
        GfxBeginFrame(clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
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
