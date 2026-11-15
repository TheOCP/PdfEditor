#pragma once
// Thin wrappers over the common Win32 item dialogs (IFileDialog) and message boxes.
#include <windows.h>
#include <string>
#include <vector>

struct FileFilter {
    const wchar_t* label;   // "PDF documents"
    const wchar_t* spec;    // "*.pdf"
};

// Returns false if the user cancelled.
bool OpenFileDialog(HWND owner, const std::vector<FileFilter>& filters, std::wstring& out,
                    const wchar_t* title = nullptr);
bool SaveFileDialog(HWND owner, const std::vector<FileFilter>& filters, const wchar_t* defName,
                    const wchar_t* defExt, std::wstring& out, const wchar_t* title = nullptr);
bool PickFolderDialog(HWND owner, std::wstring& out, const wchar_t* title = nullptr);

int AskYesNoCancel(HWND owner, const wchar_t* text, const wchar_t* caption);
void ErrorBox(HWND owner, const std::wstring& text);
