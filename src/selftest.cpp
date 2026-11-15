// Headless smoke test: exercises the document + ops pipeline without a window.
#include <cstdarg>
// Run with:  PdfEditor.exe --selftest   (allocates a console and prints results).
#include "pdf_doc.h"
#include "pdf_ops.h"
#include "sysfonts.h"
#include <windows.h>
#include <cstdio>
#include <cmath>

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

    // ---- nested objects inside a Form XObject (the "lines are pictures" case) ----
    Out("\nNested Form XObject editing:\n");
    {
        Document src;
        src.CreateBlank(300, 300);
        ops::AddRect(src, 0, { 20, 20, 280, 24 }, true, 0, 0, 0, false, 0, 0, 0, 1);   // thin 'lines'
        ops::AddRect(src, 0, { 20, 60, 280, 64 }, true, 0, 0, 0, false, 0, 0, 0, 1);
        ops::AddRect(src, 0, { 20, 100, 280, 104 }, true, 0, 0, 0, false, 0, 0, 0, 1);
        src.Bake();

        Document dst;
        dst.CreateBlank(612, 792);
        FPDF_XOBJECT xo = FPDF_NewXObjectFromPage(dst.Raw(), src.Raw(), 0);
        Check(xo != nullptr, "create XObject from a page");
        FPDF_PAGEOBJECT form = xo ? FPDF_NewFormObjectFromXObject(xo) : nullptr;
        Check(form != nullptr, "create form object (wraps the lines as one 'picture')");
        if (form) {
            FS_MATRIX m{ 1, 0, 0, 1, 100, 100 };     // place the group at +100,+100
            FPDFPageObj_SetMatrix(form, &m);
            FPDFPage_InsertObject(dst.Page(0), form);
            dst.ContentChanged(0);
            dst.Bake();
        }
        if (xo) FPDF_CloseXObject(xo);

        auto leaves = ops::ListObjects(dst, 0);
        int paths = 0; bool undescended = false;
        for (auto& o : leaves) { if (o.type == FPDF_PAGEOBJ_PATH) ++paths; if (o.type == FPDF_PAGEOBJ_FORM) undescended = true; }
        Check(paths >= 3, "descend into the form: individual line objects are reachable");
        Check(!undescended, "no opaque form container left blocking selection");

        bool mapped = false;
        for (auto& o : leaves)
            if (o.type == FPDF_PAGEOBJ_PATH && o.bounds.l > 110 && o.bounds.l < 135 && o.bounds.b > 90) mapped = true;
        Check(mapped, "nested line bounds mapped to page space (placement offset applied)");

        FPDF_PAGEOBJECT victim = nullptr, vparent = nullptr;
        for (auto& o : leaves) if (o.type == FPDF_PAGEOBJ_PATH) { victim = o.obj; vparent = o.parent; break; }
        Check(vparent != nullptr, "nested line reports its Form XObject parent");
        int before = (int)leaves.size();
        Check(ops::PromoteObject(dst, 0, victim), "promote nested line to top level");
        Check(ops::DeleteObjectNested(dst, 0, victim, nullptr), "delete the promoted line");
        dst.Bake();
        Bytes nb;
        Check(dst.SaveBytes(nb, 0) && nb.size() > 200, "serialize after nested delete");
        Document re; std::string e2;
        Check(re.LoadBytes(std::make_shared<Bytes>(nb), "", e2) == Document::Load::Ok, "reload after nested delete");
        auto leaves2 = ops::ListObjects(re, 0);
        Check((int)leaves2.size() == before - 1, "nested deletion persisted through save + reload");

        FPDF_PAGEOBJECT mover = nullptr; PRect pb0;
        for (auto& o : leaves2) if (o.type == FPDF_PAGEOBJ_PATH) { mover = o.obj; pb0 = o.bounds; break; }
        if (mover) {
            Check(ops::PromoteObject(re, 0, mover), "promote nested line before move");
            FS_MATRIX id{ 1, 0, 0, 1, 0, 0 };
            ops::TranslateObjectPage(re, 0, mover, id, 40, -30);
            re.Bake();
            Bytes mb;
            re.SaveBytes(mb, 0);
            Document re2; std::string e3;
            Check(re2.LoadBytes(std::make_shared<Bytes>(mb), "", e3) == Document::Load::Ok, "reload after nested move");
            bool moved = false;
            for (auto& o : ops::ListObjects(re2, 0))
                if (o.type == FPDF_PAGEOBJ_PATH && std::fabs(o.bounds.l - (pb0.l + 40)) < 2.0f) moved = true;
            Check(moved, "nested line move persisted through save + reload");
        }
    }
    PdfShutdown();

    Out("\n%s  (%d failure%s)\n", g_fail == 0 ? "ALL PASSED" : "FAILURES", g_fail, g_fail == 1 ? "" : "s");
    if (g_log) fclose(g_log);
    return g_fail == 0 ? 0 : 1;
}
