// Headless smoke test: exercises the document + ops pipeline without a window.
#include <cstdarg>
// Run with:  PdfEditor.exe --selftest   (allocates a console and prints results).
#include "pdf_doc.h"
#include "pdf_ops.h"
#include "sysfonts.h"
#include <windows.h>
#include <cstdio>

static int g_fail = 0;
static FILE* g_log = nullptr;
static void Out(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    if (g_log) { fputs(buf, g_log); fflush(g_log); }
}
static void Check(bool cond, const char* what) {
    Out(cond ? "[ ok ] %s\n" : "[FAIL] %s\n", what);
    if (!cond) ++g_fail;
}

int RunSelfTest() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
    FILE* f;
    freopen_s(&f, "CONOUT$", "w", stdout);
    fopen_s(&g_log, "selftest.log", "w");
    Out("PDF Editor self-test\n--------------------\n");

    PdfInit();
    {
        Document doc;
        Check(doc.CreateBlank(612, 792), "create blank document");
        Check(doc.PageCount() == 1, "one page");

        FontChoice fc;
        fc.stdName = "Helvetica";
        std::string err;
        FPDF_PAGEOBJECT t = ops::AddText(doc, 0, L"Hello, self-test 123", fc, 24, 72, 700, 0, 0, 0, err);
        Check(t != nullptr, "add text object");
        doc.Commit("add text");

        FPDF_PAGEOBJECT r = ops::AddRect(doc, 0, { 72, 600, 300, 660 }, true, 230, 60, 60, false, 0, 0, 0, 1.0f);
        Check(r != nullptr, "add rectangle");
        doc.Commit("add rect");

        Check(ops::InsertBlankPage(doc, 1, 612, 792), "insert second page");
        Check(doc.PageCount() == 2, "two pages after insert");
        doc.Commit("insert page");

        Check(ops::DuplicatePage(doc, 0), "duplicate page 0");
        Check(doc.PageCount() == 3, "three pages after duplicate");

        Check(ops::RotatePage(doc, 0, 1), "rotate page 0");

        std::wstring text = ops::ExtractText(doc, 0);
        Check(text.find(L"self-test") != std::wstring::npos, "extracted text contains the string");

        auto hits = ops::SearchText(doc, L"Hello", false, false);
        Check(!hits.empty(), "search finds 'Hello'");

        bool movedOk = ops::MovePage(doc, 2, 0);
        Check(movedOk, "move page");

        Check(doc.Undo(), "undo");
        Check(doc.Redo(), "redo");

        Bytes out;
        Check(doc.SaveBytes(out, 0) && out.size() > 200, "serialize to bytes");

        // Reload the serialized bytes and confirm it parses.
        Document doc2;
        auto bytes = std::make_shared<Bytes>(out);
        Check(doc2.LoadBytes(bytes, "", err) == Document::Load::Ok, "reload serialized document");
        Check(doc2.PageCount() == doc.PageCount(), "page count round-trips");

        Out("\nInstalled TTF fonts found: %zu\n", SystemFonts().size());
        bool arial = MatchSystemFont("Helvetica", nullptr) != nullptr;
        Check(arial, "font matcher returns a candidate for 'Helvetica'");
    }
    PdfShutdown();

    Out("\n%s  (%d failure%s)\n", g_fail == 0 ? "ALL PASSED" : "FAILURES", g_fail, g_fail == 1 ? "" : "s");
    if (g_log) fclose(g_log);
    return g_fail == 0 ? 0 : 1;
}
