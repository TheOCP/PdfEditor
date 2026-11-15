// PDF Editor - Win32 + DirectX 11 + Dear ImGui host.
#include "app.h"
#include "gfx.h"
#include "theme.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <objbase.h>
#include <shellapi.h>
#include <string>

int RunSelfTest();   // selftest.cpp

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static App g_app;

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

    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;    // don't scatter imgui.ini next to the exe
    ApplyDarkTheme();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(GfxDevice(), GfxContext());
    g_app.Init(hwnd);

    // Open a file passed on the command line.
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
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();
    return 0;
}
