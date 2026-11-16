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

    // ---- table generator + resize ----
    Out("\nTable / form + resize:\n");
    {
        Document t;
        t.CreateBlank(612, 792);
        int lines = ops::AddTable(t, 0, { 54, 54, 558, 738 }, 4, 1, true, 24, 0, 0, 0, 1.0f);
        // 4 cols -> 5 verticals, 1 row -> 2 horizontals, + 1 header = 8
        Check(lines == 8, "table built the expected number of lines");
        auto objs = ops::ListObjects(t, 0);
        int paths = 0; for (auto& o : objs) if (o.type == FPDF_PAGEOBJ_PATH) ++paths;
        Check(paths == 8, "every table line is its own editable object");
        t.Commit("table");

        // resize one line and confirm its bounds change + persist
        FPDF_PAGEOBJECT line = nullptr; PRect lb0;
        for (auto& o : objs) if (o.type == FPDF_PAGEOBJ_PATH) { line = o.obj; lb0 = o.bounds; break; }
        Check(line != nullptr, "got a table line to resize");
        PRect target{ lb0.l, lb0.b, lb0.l + 200, lb0.b + 50 };
        ops::ResizeObjectPageTopLevel(t, 0, line, target);
        t.Bake();
        Bytes tb; t.SaveBytes(tb, 0);
        Document tr; std::string te;
        Check(tr.LoadBytes(std::make_shared<Bytes>(tb), "", te) == Document::Load::Ok, "reload after resize");
        bool grew = false;
        for (auto& o : ops::ListObjects(tr, 0))
            if (o.type == FPDF_PAGEOBJ_PATH && o.bounds.W() > 150) grew = true;
        Check(grew, "resize persisted through save + reload");
    }

    // ---- line thickness works in any orientation ----
    Out("\nLine thickness (any orientation):\n");
    {
        Document d2;
        d2.CreateBlank(400, 400);
        FPDF_PAGEOBJECT hz = ops::AddLine(d2, 0, 50, 200, 350, 200, 0, 0, 0, 1.0f);   // horizontal hairline
        FPDF_PAGEOBJECT vt = ops::AddLine(d2, 0, 200, 50, 200, 350, 0, 0, 0, 1.0f);   // vertical hairline
        ops::SetLineThickness(d2, 0, hz, 6.0f);
        ops::SetLineThickness(d2, 0, vt, 6.0f);
        Check(std::fabs(ops::LineThickness(hz) - 6.0f) < 0.5f, "horizontal line thickened");
        Check(std::fabs(ops::LineThickness(vt) - 6.0f) < 0.5f, "vertical line thickened");
        FPDF_PAGEOBJECT bar = ops::AddRect(d2, 0, { 50, 100, 350, 103 }, true, 0, 0, 0, false, 0, 0, 0, 1);  // filled 3pt bar
        ops::SetLineThickness(d2, 0, bar, 10.0f);
        Check(std::fabs(ops::LineThickness(bar) - 10.0f) < 0.6f, "filled horizontal bar thickened by geometry");
    }

    // ---- split one multi-line path into movable lines ----
    Out("\nSplit a shared path into separate lines:\n");
    {
        Document s;
        s.CreateBlank(400, 400);
        FPDF_PAGEOBJECT path = FPDFPageObj_CreateNewPath(50, 100);
        FPDFPath_LineTo(path, 350, 100);
        FPDFPath_MoveTo(path, 50, 200);
        FPDFPath_LineTo(path, 350, 200);
        FPDFPath_MoveTo(path, 50, 300);
        FPDFPath_LineTo(path, 350, 300);
        FPDFPath_SetDrawMode(path, FPDF_FILLMODE_NONE, 1);
        FPDFPageObj_SetStrokeColor(path, 0, 0, 0, 255);
        FPDFPageObj_SetStrokeWidth(path, 1);
        FPDFPage_InsertObject(s.Page(0), path);
        s.ContentChanged(0);
        Check(ops::SubpathCount(path) == 3, "one path holds 3 sub-lines");
        auto parts = ops::SplitPath(s, 0, path);
        Check(parts.size() == 3, "split into 3 separate line objects");
        int paths = 0;
        for (auto& o : ops::ListObjects(s, 0)) if (o.type == FPDF_PAGEOBJ_PATH) ++paths;
        Check(paths == 3, "page now has 3 independent lines");
        PRect b0 = ops::ObjectBounds(parts[0]);
        FS_MATRIX idm{ 1, 0, 0, 1, 0, 0 };
        ops::TranslateObjectPage(s, 0, parts[0], idm, 0, -15);
        Check(std::fabs(ops::ObjectBounds(parts[0]).b - (b0.b - 15)) < 0.5f, "one split line moves on its own");
    }

    // ---- clipboard copy across pages ----
    Out("\nCopy objects across pages:\n");
    {
        Document d3;
        d3.CreateBlank(612, 792);
        ops::InsertBlankPage(d3, 1, 612, 792);               // page 2
        FontChoice fc; fc.stdName = "Helvetica";
        std::string e;
        ops::AddText(d3, 0, L"copy me", fc, 20, 72, 700, 10, 20, 30, e);
        ops::AddLine(d3, 0, 72, 680, 400, 680, 0, 0, 0, 2.0f);
        d3.Commit("setup");
        // capture everything on page 0, paste onto page 1
        int pasted = 0; std::string err;
        for (auto& o : ops::ListObjects(d3, 0)) {
            ops::ClipObject c;
            if (ops::CaptureObject(d3, 0, o.obj, c))
                if (ops::PasteObject(d3, 1, c, 0, 0, 0, err)) ++pasted;
        }
        Check(pasted == 2, "captured + pasted both objects onto page 2");
        d3.Commit("paste");
        Bytes cb; d3.SaveBytes(cb, 0);
        Document cr; std::string ce;
        Check(cr.LoadBytes(std::make_shared<Bytes>(cb), "", ce) == Document::Load::Ok, "reload after cross-page paste");
        int txt = 0, path = 0;
        for (auto& o : ops::ListObjects(cr, 1)) {
            if (o.type == FPDF_PAGEOBJ_TEXT) ++txt;
            if (o.type == FPDF_PAGEOBJ_PATH) ++path;
        }
        Check(txt >= 1 && path >= 1, "pasted text + line present on page 2 after reload");
        std::wstring t2 = ops::ExtractText(cr, 1);
        Check(t2.find(L"copy me") != std::wstring::npos, "pasted text keeps its content");
    }

    // ---- group resize keeps the grid aligned ----
    Out("\nGroup resize stays perfect:\n");
    {
        Document q;
        q.CreateBlank(612, 792);
        ops::AddTable(q, 0, { 54, 54, 558, 738 }, 4, 2, true, 24, 0, 0, 0, 1.0f);
        q.Bake();
        std::vector<FPDF_PAGEOBJECT> lines;
        for (auto& o : ops::ListObjects(q, 0)) if (o.type == FPDF_PAGEOBJ_PATH) lines.push_back(o.obj);
        Check(lines.size() >= 8, "table produced the frame lines");
        PRect cur = ops::ObjectBounds(lines[0]);
        for (size_t i = 1; i < lines.size(); ++i) cur = cur.Union(ops::ObjectBounds(lines[i]));
        // non-uniform resize (wider + shorter), as a handle drag would do
        PRect target{ cur.l + 10, cur.b + 10, cur.l + 10 + cur.W() * 1.5f, cur.b + 10 + cur.H() * 0.7f };
        float sx = target.W() / cur.W(), sy = target.H() / cur.H();
        FS_MATRIX m{ sx, 0, 0, sy, target.l - cur.l * sx, target.b - cur.b * sy };
        for (auto o : lines) ops::TransformObjectPage(q, 0, o, m);
        PRect nu = ops::ObjectBounds(lines[0]);
        for (size_t i = 1; i < lines.size(); ++i) nu = nu.Union(ops::ObjectBounds(lines[i]));
        bool exact = std::fabs(nu.l - target.l) < 0.5f && std::fabs(nu.b - target.b) < 0.5f &&
                     std::fabs(nu.r - target.r) < 0.5f && std::fabs(nu.t - target.t) < 0.5f;
        Check(exact, "resized frame lands exactly on the target (lines stay a clean grid)");
    }

    // ---- grouping tags persist through save/reload ----
    Out("\nGrouping (tag survives save/reload):\n");
    {
        Document g;
        g.CreateBlank(612, 792);
        ops::AddTable(g, 0, { 54, 54, 558, 738 }, 3, 1, false, 0, 0, 0, 0, 1.0f);
        int gid = 7, tagged = 0;
        for (auto& o : ops::ListObjects(g, 0))
            if (o.type == FPDF_PAGEOBJ_PATH) { ops::SetGroupId(g, 0, o.obj, gid); ++tagged; }
        Check(tagged > 0, "tagged the table lines with a group id");
        Check((int)ops::GroupMembers(g, 0, gid).size() == tagged, "GroupMembers returns every tagged line");
        Check(ops::MaxGroupId(g) == gid, "MaxGroupId finds the id");
        // move ONE member on its own - it must stay in the group (tag kept).
        FPDF_PAGEOBJECT one = ops::GroupMembers(g, 0, gid)[1];
        PRect obefore = ops::ObjectBounds(one);
        FS_MATRIX idm{ 1, 0, 0, 1, 0, 0 };
        ops::TranslateObjectPage(g, 0, one, idm, 0, -20);
        Check(ops::GetGroupId(one) == gid, "a single line keeps its group tag after moving");
        Check((int)ops::GroupMembers(g, 0, gid).size() == tagged, "group still has all its lines");
        Check(std::fabs(ops::ObjectBounds(one).b - (obefore.b - 20)) < 0.5f, "that line actually moved");
        g.Commit("group");
        Bytes gb; g.SaveBytes(gb, 0);
        Document gr; std::string ge;
        Check(gr.LoadBytes(std::make_shared<Bytes>(gb), "", ge) == Document::Load::Ok, "reload grouped doc");
        int found = 0;
        for (auto& o : ops::ListObjects(gr, 0)) if (ops::GetGroupId(o.obj) == gid) ++found;
        Check(found == tagged, "group tag survived save + reload");
        for (auto o : ops::GroupMembers(gr, 0, gid)) ops::ClearGroupId(gr, 0, o);
        int still = 0;
        for (auto& o : ops::ListObjects(gr, 0)) if (ops::GetGroupId(o.obj) != 0) ++still;
        Check(still == 0, "ungroup cleared every tag");
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
