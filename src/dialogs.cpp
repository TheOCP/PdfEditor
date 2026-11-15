#include "dialogs.h"
#include <shobjidl.h>
#include <shlwapi.h>

namespace {
template <typename T>
struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    operator bool() const { return p != nullptr; }
};

bool RunDialog(HWND owner, IFileDialog* dlg, const std::vector<FileFilter>& filters, std::wstring& out) {
    std::vector<COMDLG_FILTERSPEC> specs;
    for (auto& f : filters) specs.push_back({ f.label, f.spec });
    if (!specs.empty()) dlg->SetFileTypes((UINT)specs.size(), specs.data());
    if (FAILED(dlg->Show(owner))) return false;
    Com<IShellItem> item;
    if (FAILED(dlg->GetResult(&item)) || !item) return false;
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return false;
    out = path;
    CoTaskMemFree(path);
    return true;
}
} // namespace

bool OpenFileDialog(HWND owner, const std::vector<FileFilter>& filters, std::wstring& out, const wchar_t* title) {
    Com<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return false;
    if (title) dlg->SetTitle(title);
    return RunDialog(owner, dlg.p, filters, out);
}

bool SaveFileDialog(HWND owner, const std::vector<FileFilter>& filters, const wchar_t* defName,
                    const wchar_t* defExt, std::wstring& out, const wchar_t* title) {
    Com<IFileSaveDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return false;
    if (title) dlg->SetTitle(title);
    if (defName) dlg->SetFileName(defName);
    if (defExt) dlg->SetDefaultExtension(defExt);
    return RunDialog(owner, dlg.p, filters, out);
}

bool PickFolderDialog(HWND owner, std::wstring& out, const wchar_t* title) {
    Com<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS);
    if (title) dlg->SetTitle(title);
    return RunDialog(owner, dlg.p, {}, out);
}

int AskYesNoCancel(HWND owner, const wchar_t* text, const wchar_t* caption) {
    return MessageBoxW(owner, text, caption, MB_YESNOCANCEL | MB_ICONWARNING);
}

void ErrorBox(HWND owner, const std::wstring& text) {
    MessageBoxW(owner, text.c_str(), L"PDF Editor", MB_OK | MB_ICONERROR);
}
