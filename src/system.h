#pragma once
// Printing and Windows integration (PDF Editor as the default PDF app).
#include "pdf_doc.h"
#include <windows.h>
#include <string>
#include <vector>

struct PrintResult {
    bool printed = false;     // a print job was sent
    bool cancelled = false;   // the user closed the print dialog
    int pages = 0;
    std::string error;
};

// Shows the Windows print dialog (printer, pages / current page, copies) and
// prints the document as it is now, including unsaved edits and form fields.
// Each page is fitted to the paper and turned to match its orientation.
PrintResult PrintDocument(HWND owner, Document& doc, int currentPage, const std::wstring& title);
// The printing itself, on a printer DC (outputFile: print-to-file target, or null).
PrintResult PrintPages(HDC dc, Document& doc, const std::vector<int>& pages, int copies,
                       const std::wstring& title, const wchar_t* outputFile = nullptr);

// Registers this program for .pdf files for the current user (no admin rights
// needed). `base` is the registry root under HKEY_CURRENT_USER ("Software";
// the self-test uses a scratch key).
bool RegisterPdfApp(const std::wstring& base, const std::wstring& exe, std::string& err);
// Is this program what Windows opens PDF files with?
bool IsDefaultPdfApp();
// Windows only lets the user pick the default: opens Settings at this app.
void OpenDefaultAppSettings();
// Full path of the running program.
std::wstring ThisExePath();
