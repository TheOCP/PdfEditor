// Headless smoke test: exercises the document + ops pipeline without a window.
#include <cstdarg>
// Run with:  PdfEditor.exe --selftest   (allocates a console and prints results).
#include "pdf_doc.h"
#include "pdf_ops.h"
#include "sysfonts.h"
#include "imageio.h"
#include "system.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <string>
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

// ---- helpers for the editing tests -----------------------------------------

// A one-page PDF (612x792, Helvetica as /F1) whose content stream is `content`.
static std::shared_ptr<Bytes> MakePdf(const std::string& content) {
    std::vector<std::string> objs = {
        "<</Type/Catalog/Pages 2 0 R>>",
        "<</Type/Pages/Kids[3 0 R]/Count 1>>",
        "<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]/Resources<</Font<</F1 4 0 R>>>>/Contents 5 0 R>>",
        "<</Type/Font/Subtype/Type1/BaseFont/Helvetica>>",
        "<</Length " + std::to_string(content.size()) + ">>stream\n" + content + "endstream",
    };
    std::string pdf = "%PDF-1.4\n";
    std::vector<size_t> off;
    for (size_t i = 0; i < objs.size(); ++i) { off.push_back(pdf.size()); pdf += std::to_string(i + 1) + " 0 obj\n" + objs[i] + "\nendobj\n"; }
    size_t xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(objs.size() + 1) + "\n0000000000 65535 f \n";
    char b[32];
    for (size_t o : off) { snprintf(b, sizeof b, "%010zu 00000 n \n", o); pdf += b; }
    pdf += "trailer\n<</Size " + std::to_string(objs.size() + 1) + "/Root 1 0 R>>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return std::make_shared<Bytes>(pdf.begin(), pdf.end());
}

static uint32_t PixelAt(Document& d, int page, float px, float py) {
    FS_SIZEF sz = d.PageSizePt(page);
    int w = (int)sz.width, h = (int)sz.height;
    std::vector<uint32_t> buf;
    if (!d.Render(page, 1.0f, 0, 0, w, h, buf, true, true)) return 0xFFFFFF;
    float dx, dy;
    d.PageToDev(page, 1.0f, px, py, dx, dy);
    int x = std::clamp((int)dx, 0, w - 1), y = std::clamp((int)dy, 0, h - 1);
    return buf[(size_t)y * w + x] & 0xFFFFFF;
}
static bool Dark(uint32_t c) { return (c & 255) + ((c >> 8) & 255) + ((c >> 16) & 255) < 200; }

static std::shared_ptr<Bytes> Saved(Document& d) {
    d.Bake();
    auto b = std::make_shared<Bytes>();
    d.SaveBytes(*b, 0);
    return b;
}

static int JpegBlock(void* param, unsigned long pos, unsigned char* buf, unsigned long size) {
    const Bytes* b = static_cast<const Bytes*>(param);
    if ((uint64_t)pos + size > b->size()) return 0;
    memcpy(buf, b->data() + pos, size);
    return 1;
}

static void RunEditTests() {
    std::string e;
    FontChoice helv; helv.stdName = "Helvetica";
    FS_MATRIX id{ 1, 0, 0, 1, 0, 0 };

    Out("\nMoving content that has a clip path:\n");
    {
        Document d;
        d.LoadBytes(MakePdf("q 50 350 300 100 re W n 6 w 0 G 0 400 m 600 400 l S Q\n"), "", e);
        FPDF_PAGEOBJECT ln = nullptr;
        for (auto& o : ops::ListObjects(d, 0)) if (o.type == FPDF_PAGEOBJ_PATH) ln = o.obj;
        Check(ln && Dark(PixelAt(d, 0, 200, 400)), "clipped line visible before the move");
        ops::TranslateObjectPage(d, 0, ln, id, 0, 100);
        Check(Dark(PixelAt(d, 0, 200, 500)), "moved line is still visible (its clip moved with it)");
        Document r;
        r.LoadBytes(Saved(d), "", e);
        Check(Dark(PixelAt(r, 0, 200, 500)), "...and still visible after save + reload");
    }

    Out("\nUngrouping keeps the stacking order:\n");
    {
        Document src;
        src.CreateBlank(612, 792);
        ops::AddLine(src, 0, 100, 400, 500, 400, 0, 0, 0, 8);
        src.Bake();
        Document dst;
        dst.CreateBlank(612, 792);
        FPDF_XOBJECT xo = FPDF_NewXObjectFromPage(dst.Raw(), src.Raw(), 0);
        FPDF_PAGEOBJECT form = FPDF_NewFormObjectFromXObject(xo);
        FPDFPage_InsertObject(dst.Page(0), form);                                     // bottom
        ops::AddRect(dst, 0, { 50, 350, 550, 450 }, true, 255, 255, 255, false, 0, 0, 0, 1); // white cover on top
        FPDF_CloseXObject(xo);
        Check(!Dark(PixelAt(dst, 0, 300, 400)), "line hidden under the white box");
        FPDF_PAGEOBJECT inner = nullptr;
        for (auto& o : ops::ListObjects(dst, 0)) if (o.type == FPDF_PAGEOBJ_PATH && o.parent) inner = o.obj;
        ops::PromoteObject(dst, 0, inner);
        Check(!Dark(PixelAt(dst, 0, 300, 400)), "after ungrouping the line is still under the box");
    }

    Out("\nCopying curves, dashes, fonts and pictures exactly:\n");
    {
        Document d;
        d.CreateBlank(612, 792);
        ops::InsertBlankPage(d, 1, 612, 792);
        FPDF_PAGEOBJECT p = FPDFPageObj_CreateNewPath(100, 100);
        FPDFPath_BezierTo(p, 150, 200, 250, 200, 300, 100);
        FPDFPath_SetDrawMode(p, FPDF_FILLMODE_NONE, 1);
        FPDFPageObj_SetStrokeWidth(p, 2);
        FPDFPageObj_SetStrokeColor(p, 0, 0, 0, 255);
        FPDFPage_InsertObject(d.Page(0), p);
        d.ContentChanged(0);
        ops::ClipObject c;
        ops::CaptureObject(d, 0, p, c);
        FPDF_PAGEOBJECT q = ops::PasteObject(d, 0, c, 0, -50, 0, e);
        int bez = 0;
        for (int i = 0; q && i < FPDFPath_CountSegments(q); ++i)
            if (FPDFPathSegment_GetType(FPDFPath_GetPathSegment(q, i)) == FPDF_SEGMENT_BEZIERTO) ++bez;
        Check(q && bez == 3, "a pasted curve is still a curve (not straightened)");

        FPDF_PAGEOBJECT l = ops::AddLine(d, 0, 100, 300, 400, 300, 0, 0, 0, 1);
        float dash[2] = { 3, 2 };
        FPDFPageObj_SetDashArray(l, dash, 2, 0);
        ops::CaptureObject(d, 0, l, c);
        FPDF_PAGEOBJECT l2 = ops::PasteObject(d, 0, c, 0, -20, 0, e);
        Check(l2 && FPDFPageObj_GetDashCount(l2) == 2, "a pasted dotted line stays dotted");

        const SysFont* arial = MatchSystemFont("Arial");
        FontChoice ar = arial ? ChoiceFor(*arial) : helv;
        FPDF_PAGEOBJECT t = ops::AddText(d, 0, L"Kontostand 987,65", ar, 12, 72, 700, 0, 0, 0, e);
        ops::CaptureObject(d, 0, t, c);
        FPDF_PAGEOBJECT tq = ops::PasteObject(d, 1, c, 0, 0, 0, e);
        Check(tq && FPDFTextObj_GetFont(tq) == FPDFTextObj_GetFont(t), "copied text uses the exact same font as the original");
        Document r;
        r.LoadBytes(Saved(d), "", e);
        Check(ops::ExtractText(r, 1).find(L"Kontostand 987,65") != std::wstring::npos, "copied text is intact on page 2 after reload");

        Image img;
        img.w = 400; img.h = 200;
        img.px.assign((size_t)img.w * img.h, 0xFF3366CCu);
        FPDF_PAGEOBJECT im = ops::AddImage(d, 0, img, 100, 450, 100, e);   // 400 px shown 100 pt wide
        ops::CaptureObject(d, 0, im, c);
        FPDF_PAGEOBJECT iq = ops::PasteObject(d, 0, c, 0, 100, 0, e);
        unsigned w = 0, h = 0;
        if (iq) FPDFImageObj_GetImagePixelSize(iq, &w, &h);
        Check(w == 400 && h == 200, "a copied picture keeps its full resolution");

        wchar_t tmpDir[MAX_PATH];
        GetTempPathW(MAX_PATH, tmpDir);
        std::wstring jpgPath = std::wstring(tmpDir) + L"pdfeditor_selftest.jpg";
        Image small;
        small.w = 64; small.h = 64;
        small.px.assign(64 * 64, 0xFF808080u);
        Bytes jb;
        if (SaveImageFile(jpgPath, small.px.data(), 64, 64, e) && ReadFileBytes(jpgPath, jb)) {
            FPDF_PAGEOBJECT ji = FPDFPageObj_NewImageObj(d.Raw());
            FPDF_FILEACCESS fa{};
            fa.m_FileLen = (unsigned long)jb.size();
            fa.m_GetBlock = JpegBlock;
            fa.m_Param = &jb;
            FPDFImageObj_LoadJpegFileInline(nullptr, 0, ji, &fa);
            FS_MATRIX m{ 64, 0, 0, 64, 300, 300 };
            FPDFPageObj_SetMatrix(ji, &m);
            FPDFPage_InsertObject(d.Page(0), ji);
            d.ContentChanged(0);
            ops::CaptureObject(d, 0, ji, c);
            Check(!c.jpeg.empty(), "a JPEG picture is captured as its original JPEG bytes");
            FPDF_PAGEOBJECT jq = ops::PasteObject(d, 0, c, 80, 0, 0, e);
            char filt[32] = { 0 };
            if (jq) FPDFImageObj_GetImageFilter(jq, 0, filt, sizeof(filt) - 1);
            Check(jq && strcmp(filt, "DCTDecode") == 0, "the pasted copy is still a JPEG (lossless, small file)");
            DeleteFileW(jpgPath.c_str());
        } else {
            Check(false, "could not create the test JPEG");
        }
    }

    Out("\nSplitting table cells into sides:\n");
    {
        Document d;
        d.CreateBlank(612, 792);
        FPDF_PAGEOBJECT cell = ops::AddRect(d, 0, { 100, 100, 300, 200 }, false, 0, 0, 0, true, 0, 0, 0, 1);
        Check(ops::SplittableParts(cell) == 4, "an outlined table cell counts as 4 sides");
        auto sides = ops::SplitPath(d, 0, cell);
        Check(sides.size() == 4, "the cell splits into 4 separate lines");
        if (sides.size() == 4) {
            PRect other = ops::ObjectBounds(sides[1]);
            PRect mine = ops::ObjectBounds(sides[0]);
            ops::TranslateObjectPage(d, 0, sides[0], id, 0, 30);
            PRect other2 = ops::ObjectBounds(sides[1]);
            Check(std::fabs(ops::ObjectBounds(sides[0]).b - (mine.b + 30)) < 0.5f && std::fabs(other2.b - other.b) < 0.01f,
                  "one side moves on its own; the others stay");
        }
        FPDF_PAGEOBJECT filled = ops::AddRect(d, 0, { 350, 100, 500, 200 }, true, 200, 200, 255, true, 0, 0, 0, 1);
        auto s2 = ops::SplitPath(d, 0, filled);
        int paths = 0;
        for (auto& o : ops::ListObjects(d, 0)) if (o.type == FPDF_PAGEOBJ_PATH) ++paths;
        uint32_t inside = PixelAt(d, 0, 425, 150);
        Check(s2.size() == 4 && paths == 9 && ((inside & 255) > 230 && ((inside >> 16) & 255) < 230),
              "a filled + outlined cell: 4 movable sides, and its fill stays underneath");
    }

    Out("\nMoving objects between pages:\n");
    {
        Document d;
        d.CreateBlank(612, 792);
        ops::InsertBlankPage(d, 1, 612, 792);
        FPDF_PAGEOBJECT t = ops::AddText(d, 0, L"Umzug 1.234,56", helv, 14, 72, 700, 0, 0, 0, e);
        Check(ops::MoveObjectToPage(d, 0, t, 1, 0, -100), "move a text object onto page 2");
        Document r;
        r.LoadBytes(Saved(d), "", e);
        Check(ops::ExtractText(r, 1).find(L"Umzug 1.234,56") != std::wstring::npos &&
              ops::ExtractText(r, 0).find(L"Umzug") == std::wstring::npos, "after reload the text is on page 2 only");
    }

    Out("\nText size and font when editing:\n");
    {
        Document d;
        d.CreateBlank(612, 792);
        FPDF_PAGEOBJECT t1 = FPDFPageObj_NewTextObj(d.Raw(), "Helvetica", 1.0f);
        FPDFText_SetText(t1, WS(L"Tf one"));
        FS_MATRIX m{ 10, 0, 0, 10, 72, 600 };
        FPDFPageObj_SetMatrix(t1, &m);
        FPDFPage_InsertObject(d.Page(0), t1);
        d.ContentChanged(0);
        Check(std::fabs(ops::VisualFontSize(t1) - 10.0f) < 0.01f, "size reads as 10 pt even when stored as Tf 1 x matrix 10");

        FPDF_PAGEOBJECT t2 = ops::AddText(d, 0, L"Saldo", helv, 12, 72, 500, 0, 0, 0, e);
        FS_MATRIX m0{ 1, 0, 0, 1, 0, 0 };
        FPDFPageObj_GetMatrix(t2, &m0);
        FPDF_PAGEOBJECT rep = nullptr;
        bool fb = true;
        bool ok = ops::SetObjectTextSafe(d, 0, t2, L"Saldo 1.234,56 \x20AC", &rep, &fb);
        Check(ok && !fb && rep == t2, "editing keeps the original font when it has the characters");
        float before = ops::VisualFontSize(t2);
        ok = ops::SetObjectTextSafe(d, 0, t2, L"\x041F\x0440\x0438\x0432\x0435\x0442", &rep, &fb);
        FS_MATRIX m1{ 1, 0, 0, 1, 0, 0 };
        if (rep) FPDFPageObj_GetMatrix(rep, &m1);
        Check(ok && fb, "characters the PDF font lacks switch to a matching installed font");
        float after = rep ? ops::VisualFontSize(rep) : 0;
        Check(rep && std::fabs(m1.e - m0.e) < 0.05f && std::fabs(m1.f - m0.f) < 0.05f &&
                  std::fabs(after - before) < before * 0.15f,
              Fmt("that fallback starts at the same spot and matches the size (%.2f pt -> %.2f pt, x %.2f -> %.2f)",
                  before, after, m0.e, m1.e).c_str());
        Check(ops::ExtractText(d, 0).find(L"\x041F\x0440\x0438\x0432\x0435\x0442") != std::wstring::npos,
              "the new characters are really there");
    }

    Out("\nTrue redaction:\n");
    {
        Document d;
        d.CreateBlank(612, 792);
        ops::AddText(d, 0, L"IBAN DE12 3456 7890", helv, 14, 72, 700, 0, 0, 0, e);
        auto hits = ops::SearchText(d, L"3456", false, false);
        Check(!hits.empty(), "found the digits to redact");
        if (!hits.empty()) {
            ops::RedactResult rr = ops::Redact(d, 0, hits[0].box, 0, 0, 0);
            Document r;
            r.LoadBytes(Saved(d), "", e);
            std::wstring tx = ops::ExtractText(r, 0);
            Check(tx.find(L"3456") == std::wstring::npos, "the covered digits are gone from the saved file");
            Check(tx.find(L"IBAN") != std::wstring::npos && tx.find(L"7890") != std::wstring::npos,
                  "the rest of the line is kept");
            Check(rr.textSplit == 1, "the line was cut, not deleted");
        }
        Image g;
        g.w = 100; g.h = 100;
        g.px.assign(100 * 100, 0xFF00AA00u);
        FPDF_PAGEOBJECT im = ops::AddImage(d, 0, g, 300, 300, 100, e);
        ops::RedactResult r2 = ops::Redact(d, 0, { 320, 320, 360, 360 }, 0, 0, 0);
        Image after;
        bool got = ops::ImagePixels(im, after);
        Check(r2.imagesMasked == 1 && got && (after.px[60 * 100 + 40] & 0xFFFFFF) == 0,
              "picture pixels under the box are blacked out");
    }

    Out("\nCutting a piece out of a picture (scanned lines):\n");
    {
        Document d;
        d.CreateBlank(612, 792);
        Image scan;
        scan.w = 200; scan.h = 200;
        scan.px.assign(200 * 200, 0xFFFFFFFFu);
        for (int y = 98; y <= 102; ++y)
            for (int x = 0; x < 200; ++x) scan.px[(size_t)y * 200 + x] = 0xFF000000u;
        FPDF_PAGEOBJECT im = ops::AddImage(d, 0, scan, 100, 100, 200, e);   // 1 px per pt; row 100 at y=200
        FPDF_PAGEOBJECT piece = ops::CutImageRegion(d, 0, im, { 90, 195, 310, 205 }, e);
        Check(piece != nullptr, "cut the line out of the scan as its own piece");
        Image pp, rest;
        bool g1 = piece && ops::ImagePixels(piece, pp), g2 = ops::ImagePixels(im, rest);
        bool pieceHasLine = g1 && std::any_of(pp.px.begin(), pp.px.end(), [](uint32_t q) { return (q & 0xFFFFFF) == 0; });
        Check(pieceHasLine, "the piece contains the line");
        Check(g2 && (rest.px[100 * 200 + 50] & 0xFFFFFF) == 0xFFFFFF, "the hole is filled with the paper colour");
        if (piece) {
            PRect b0 = ops::ObjectBounds(piece);
            ops::TranslateObjectPage(d, 0, piece, id, 0, -40);
            Check(std::fabs(ops::ObjectBounds(piece).b - (b0.b - 40)) < 0.5f && Dark(PixelAt(d, 0, 200, 160)),
                  "the piece can be moved (the line is now 40 pt lower)");
        }
    }

    Out("\nAnnotations and precise clicking:\n");
    {
        Document d;
        d.CreateBlank(612, 792);
        FPDF_ANNOTATION an = FPDFPage_CreateAnnot(d.Page(0), FPDF_ANNOT_SQUARE);
        FS_RECTF rc{ 100, 500, 200, 450 };    // left, top, right, bottom
        FPDFAnnot_SetRect(an, &rc);
        FPDFPage_CloseAnnot(an);
        auto ai = ops::AnnotAt(d, 0, 150, 475, 1, false);
        Check(ai.has_value(), "an annotation is found under the mouse");
        if (ai) {
            ops::MoveAnnot(d, 0, ai->index, 10, 20);
            PRect r;
            ops::AnnotRect(d, 0, ai->index, r);
            Check(std::fabs(r.l - 110) < 0.01f && std::fabs(r.b - 470) < 0.01f, "the annotation moved by (10, 20)");
        }
        FPDF_PAGEOBJECT cell = ops::AddRect(d, 0, { 100, 100, 300, 300 }, false, 0, 0, 0, true, 0, 0, 0, 1);
        FPDF_PAGEOBJECT word = ops::AddText(d, 0, L"Wert", helv, 14, 104, 200, 0, 0, 0, e);
        auto h1 = ops::HitTest(d, 0, 250, 250, 2);
        auto h2 = ops::HitTest(d, 0, 100.4f, 250, 2);
        auto h3 = ops::HitTest(d, 0, 115, 204, 2);
        Check(!h1, "clicking empty space inside a table cell selects nothing");
        Check(h2 && h2->obj == cell, "clicking right on the cell's border line selects it");
        Check(h3 && h3->obj == word, "clicking text next to the border selects the text");
    }
}

// Development aid: how the lines of a real PDF are drawn and whether each one
// can be made thinner and thicker. Works on a copy in memory; nothing is saved.
// Reports geometry only (no text).
int RunLineCheck(const wchar_t* pdf) {
    fopen_s(&g_log, "line-check.log", "w");
    PdfInit();
    int failures = 0;
    {
        Document d;
        std::string err;
        if (d.LoadFile(pdf, "", err) != Document::Load::Ok) { Out("could not open (%s)\n", err.c_str()); PdfShutdown(); return 2; }
        Out("pages: %d\n", d.PageCount());
        for (int pg = 0; pg < d.PageCount(); ++pg) {
            std::map<std::string, int> kinds;
            int nested = 0, ok = 0, bad = 0;
            float tmin = 1e9f, tmax = 0;
            std::vector<ops::ObjInfo> lines;
            for (const auto& oi : ops::ListObjects(d, pg))
                if (ops::IsLine(oi.obj)) lines.push_back(oi);
            for (const auto& oi : lines) {
                FPDF_PAGEOBJECT o = oi.obj;
                kinds[ops::LineKindName(o)]++;
                float sc = ops::MatrixScale(oi.accum);
                float t0 = ops::LineThickness(o) * sc;
                tmin = std::min(tmin, t0); tmax = std::max(tmax, t0);
                if (oi.parent) { ++nested; ops::PromoteObject(d, pg, o); sc = 1.0f; }
                o = ops::SetLineThickness(d, pg, o, 0.3f / sc);
                bool thin = std::fabs(ops::LineThickness(o) * sc - 0.3f) < 0.03f;
                o = ops::SetLineThickness(d, pg, o, 3.0f / sc);
                bool thick = std::fabs(ops::LineThickness(o) * sc - 3.0f) < 0.1f;
                if (thin && thick) ++ok; else ++bad;
            }
            std::string ks;
            for (const auto& [k, n] : kinds) ks += k + "=" + std::to_string(n) + " ";
            if (lines.empty()) Out("page %d: no lines\n", pg + 1);
            else Out("page %d: %zu lines (%s) nested=%d thickness %.2f..%.2f pt -> thinner+thicker ok=%d failed=%d\n",
                     pg + 1, lines.size(), ks.c_str(), nested, tmin, tmax, ok, bad);
            failures += bad;
        }
    }
    PdfShutdown();
    Out("%s\n", failures ? "SOME LINES FAILED" : "ALL LINES OK");
    if (g_log) fclose(g_log);
    return failures ? 1 : 0;
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
        ops::SetLineThickness(d2, 0, bar, 0.5f);
        Check(std::fabs(ops::LineThickness(bar) - 0.5f) < 0.05f, "filled bar made thinner");
    }

    // ---- existing lines drawn in other ways can be made thinner ----
    Out("\nExisting lines (stretched, grid as one shape, picture):\n");
    {
        Document d3;
        d3.CreateBlank(612, 792);
        FPDF_PAGE p3 = d3.Page(0);
        // A unit line stretched 500x along x: drawn 2 pt thick, but sqrt(det) = 31.6.
        FPDF_PAGEOBJECT st = FPDFPageObj_CreateNewPath(0, 0);
        FPDFPath_LineTo(st, 1, 0);
        FPDFPath_SetDrawMode(st, FPDF_FILLMODE_NONE, 1);
        FPDFPageObj_SetStrokeColor(st, 0, 0, 0, 255);
        FPDFPageObj_SetStrokeWidth(st, 2.0f);
        FPDFPageObj_Transform(st, 500, 0, 0, 1, 50, 700);
        FPDFPage_InsertObject(p3, st);
        Check(std::fabs(ops::LineThickness(st) - 2.0f) < 0.05f, "stretched stroked line: real thickness read (2 pt)");
        st = ops::SetLineThickness(d3, 0, st, 0.5f);
        Check(std::fabs(ops::LineThickness(st) - 0.5f) < 0.05f, "stretched stroked line made thinner (0.5 pt)");
        PRect sb = ops::ObjectBounds(st);
        Check(sb.W() > 495 && sb.W() < 505, "...and keeps its length");

        // A stroked box stretched unevenly: rebuilt so every side gets the same width.
        FPDF_PAGEOBJECT bx = FPDFPageObj_CreateNewRect(0, 0, 1, 1);
        FPDFPath_SetDrawMode(bx, FPDF_FILLMODE_NONE, 1);
        FPDFPageObj_SetStrokeColor(bx, 0, 0, 0, 255);
        FPDFPageObj_SetStrokeWidth(bx, 0.02f);
        FPDFPageObj_Transform(bx, 200, 0, 0, 50, 100, 500);
        FPDFPage_InsertObject(p3, bx);
        bx = ops::SetLineThickness(d3, 0, bx, 1.0f);
        Check(std::fabs(ops::LineThickness(bx) - 1.0f) < 0.05f, "unevenly stretched box border set to 1 pt");

        // A whole grid drawn as ONE filled path of 2 pt rectangles + a big filled cell.
        FPDF_PAGEOBJECT grid = FPDFPageObj_CreateNewPath(50, 300);
        auto rect = [&](float x, float y, float w, float h) {
            FPDFPath_MoveTo(grid, x, y); FPDFPath_LineTo(grid, x + w, y); FPDFPath_LineTo(grid, x + w, y + h);
            FPDFPath_LineTo(grid, x, y + h); FPDFPath_Close(grid);
        };
        rect(50, 300, 400, 2);      // bottom
        rect(50, 400, 400, 2);      // top
        rect(50, 300, 2, 102);      // left
        rect(448, 300, 2, 102);     // right
        rect(100, 320, 60, 60);     // a filled square (not a line)
        FPDFPath_SetDrawMode(grid, FPDF_FILLMODE_WINDING, 0);
        FPDFPageObj_SetFillColor(grid, 0, 0, 0, 255);
        FPDFPage_InsertObject(p3, grid);
        Check(std::fabs(ops::LineThickness(grid) - 2.0f) < 0.05f, "grid drawn as one shape: line thickness 2 pt (not its size)");
        grid = ops::SetLineThickness(d3, 0, grid, 0.5f);
        Check(std::fabs(ops::LineThickness(grid) - 0.5f) < 0.05f, "grid lines made thinner (0.5 pt)");
        PRect gb = ops::ObjectBounds(grid);
        Check(gb.W() > 398 && gb.H() > 98, "grid keeps its size");
        bool squareKept = false;
        for (const auto& oi : ops::ListObjects(d3, 0))
            if (oi.obj == grid) squareKept = ops::DistanceToObject(grid, 130, 350) < 0.01f;
        Check(squareKept, "the filled square in the same shape is untouched");

        // A thin picture used as a line.
        Image img;
        img.w = 200; img.h = 2;
        img.px.assign((size_t)img.w * img.h, 0xFF000000u);   // opaque black
        std::string ierr;
        FPDF_PAGEOBJECT pic = ops::AddImage(d3, 0, img, 300, 150, 400, ierr);
        Check(pic && ops::IsLine(pic), "thin picture counts as a line");
        if (pic) {
            float t0 = ops::LineThickness(pic);
            ops::SetLineThickness(d3, 0, pic, t0 / 4);
            Check(std::fabs(ops::LineThickness(pic) - t0 / 4) < 0.05f, "picture line made thinner");
        }
        Check(!ops::IsLine(ops::AddRect(d3, 0, { 300, 300, 400, 380 }, true, 0, 0, 0, false, 0, 0, 0, 1)),
              "a filled box is not a line");

        // Round trip.
        d3.Bake();
        Bytes b3; d3.SaveBytes(b3, 0);
        Document r3; std::string e3;
        Check(r3.LoadBytes(std::make_shared<Bytes>(b3), "", e3) == Document::Load::Ok, "thinned lines survive save + reload");
    }

    // ---- resizing a table: lines keep their thickness, optional fixed spacing ----
    Out("\nTable resize (thickness kept, spacing kept on request):\n");
    {
        auto build = [](Document& t) {
            t.CreateBlank(612, 792);
            ops::AddTable(t, 0, { 100, 400, 400, 700 }, 3, 3, false, 0, 0, 0, 0, 1.0f);   // rows 100 pt, cols 100 pt
            std::vector<FPDF_PAGEOBJECT> all;
            for (const auto& oi : ops::ListObjects(t, 0)) all.push_back(oi.obj);
            return all;
        };
        auto bounds = [](const std::vector<FPDF_PAGEOBJECT>& v) {
            PRect u = ops::ObjectBounds(v[0]);
            for (auto o : v) u = u.Union(ops::ObjectBounds(o));
            return u;
        };
        auto rowsY = [](const std::vector<FPDF_PAGEOBJECT>& v) {
            std::vector<float> ys;
            for (auto o : v) { PRect b = ops::ObjectBounds(o); if (b.W() > b.H()) ys.push_back((b.b + b.t) / 2); }
            std::sort(ys.begin(), ys.end());
            return ys;
        };
        {
            Document t;
            auto all = build(t);
            PRect cur = bounds(all);
            PRect target{ cur.l, cur.b - cur.H(), cur.r, cur.t };            // twice as tall
            for (int step = 1; step <= 10; ++step) {                         // like dragging the mouse
                PRect c = bounds(all);
                PRect tg{ cur.l, cur.b - cur.H() * step / 10.0f, cur.r, cur.t };
                ops::ResizeGroup(t, 0, all, c, tg, false);
            }
            (void)target;
            float maxT = 0;
            for (auto o : all) maxT = std::max(maxT, ops::LineThickness(o));
            Check(std::fabs(maxT - 1.0f) < 0.02f, "taller table: lines still 1 pt thick");
            auto ys = rowsY(all);
            Check(ys.size() == 4 && std::fabs((ys[1] - ys[0]) - (ys[3] - ys[2])) < 1.0f && ys[1] - ys[0] > 180,
                  "taller table: rows stay evenly spaced (about 200 pt)");
        }
        {
            Document t;
            auto all = build(t);
            PRect cur = bounds(all);
            auto ys0 = rowsY(all);
            for (int step = 1; step <= 10; ++step) {
                PRect c = bounds(all);
                PRect tg{ cur.l, cur.b - 50.0f * step / 10.0f, cur.r + 30.0f * step / 10.0f, cur.t };
                ops::ResizeGroup(t, 0, all, c, tg, true);
            }
            auto ys = rowsY(all);
            Check(ys.size() == 4 && std::fabs((ys[2] - ys[1]) - (ys0[2] - ys0[1])) < 0.01f &&
                  std::fabs((ys[3] - ys[2]) - (ys0[3] - ys0[2])) < 0.01f, "keep spacing: inner row spacing unchanged");
            Check(std::fabs((ys[1] - ys[0]) - (ys0[1] - ys0[0] + 50)) < 0.5f, "keep spacing: only the bottom row grew");
            PRect nb = bounds(all);
            Check(std::fabs(nb.W() - (cur.W() + 30)) < 0.5f, "keep spacing: table got wider by the drag");
            // shrink far past the inner line: stops before it
            PRect c = bounds(all);
            PRect got = ops::ResizeGroup(t, 0, all, c, PRect{ c.l, c.b + 400, c.r, c.t }, true);
            auto ys2 = rowsY(all);
            Check(ys2[0] < ys2[1] && got.b < ys2[1], "keep spacing: an edge never crosses an inner line");
        }
    }

    // ---- printing (to "Microsoft Print to PDF", no dialog) ----
    Out("\nPrinting:\n");
    {
        HDC pdc = CreateDCW(L"WINSPOOL", L"Microsoft Print to PDF", nullptr, nullptr);
        if (!pdc) {
            Out("  (no \"Microsoft Print to PDF\" printer - skipped)\n");
        } else {
            Document pdoc;
            pdoc.CreateBlank(595, 842);
            ops::InsertBlankPage(pdoc, 1, 842, 595);                    // a landscape page
            FontChoice fc; fc.stdName = "Helvetica";
            std::string e;
            ops::AddText(pdoc, 0, L"Printed page one", fc, 28, 72, 700, 0, 0, 0, e);
            ops::AddText(pdoc, 1, L"Landscape page", fc, 28, 72, 500, 0, 0, 0, e);
            pdoc.Commit("print test");
            wchar_t tmp[MAX_PATH] = {};
            GetTempPathW(MAX_PATH, tmp);
            std::wstring out = std::wstring(tmp) + L"pdfeditor-print-test.pdf";
            DeleteFileW(out.c_str());
            PrintResult pr = PrintPages(pdc, pdoc, { 0, 1 }, 1, L"print test", out.c_str());
            DeleteDC(pdc);
            Check(pr.printed && pr.pages == 2, "printed both pages");
            Document back;
            std::string be;
            bool loaded = false;
            for (int t = 0; t < 50 && !loaded; ++t) {               // the spooler writes the file
                loaded = back.LoadFile(out, "", be) == Document::Load::Ok;
                if (!loaded) Sleep(100);
            }
            Check(loaded && back.PageCount() == 2, "printout has 2 pages");
            if (loaded && back.PageCount() == 2) {
                auto ink = [&](int pg) {
                    FS_SIZEF z = back.PageSizePt(pg);
                    std::vector<uint32_t> px;
                    int w = (int)(z.width / 2), h = (int)(z.height / 2);
                    back.Render(pg, 0.5f, 0, 0, w, h, px);
                    int dark = 0;
                    for (uint32_t c : px) if (((c & 0xFF) + ((c >> 8) & 0xFF) + ((c >> 16) & 0xFF)) < 300) ++dark;
                    return dark;
                };
                Check(ink(0) > 50 && ink(1) > 50, "printed pages carry the text");
                FS_SIZEF z0 = back.PageSizePt(0);
                Check(z0.height > z0.width, "printed on portrait paper (landscape page turned to fit)");
            }
            back.Close();
            DeleteFileW(out.c_str());
        }
    }

    // ---- default PDF app registration (scratch registry key) ----
    Out("\nDefault PDF app registration:\n");
    {
        const std::wstring base = L"Software\\PdfEditorSelfTest";
        RegDeleteTreeW(HKEY_CURRENT_USER, base.c_str());
        std::string err;
        bool ok = RegisterPdfApp(base, L"C:\\Apps\\PdfEditor.exe", err);
        Check(ok, "registration written");
        wchar_t buf[512] = {};
        DWORD cb = sizeof(buf);
        LONG rc = RegGetValueW(HKEY_CURRENT_USER, (base + L"\\Classes\\PdfEditor.PDF\\shell\\open\\command").c_str(),
                               nullptr, RRF_RT_REG_SZ, nullptr, buf, &cb);
        Check(rc == ERROR_SUCCESS && std::wstring(buf) == L"\"C:\\Apps\\PdfEditor.exe\" \"%1\"", "open command passes the file");
        cb = sizeof(buf);
        rc = RegGetValueW(HKEY_CURRENT_USER, (base + L"\\TheOCProductions\\PdfEditor\\Capabilities\\FileAssociations").c_str(),
                          L".pdf", RRF_RT_REG_SZ, nullptr, buf, &cb);
        Check(rc == ERROR_SUCCESS && std::wstring(buf) == L"PdfEditor.PDF", "listed as a PDF app for Windows Settings");
        RegDeleteTreeW(HKEY_CURRENT_USER, base.c_str());
    }

    // ---- text baseline for alignment ----
    Out("\nAlignment helper:\n");
    {
        Document a;
        a.CreateBlank(612, 792);
        std::string aerr;
        FontChoice fc; fc.stdName = "Helvetica";
        FPDF_PAGEOBJECT t1 = ops::AddText(a, 0, L"Left aligned", fc, 12, 72, 700, 0, 0, 0, aerr);
        float ox = 0, oy = 0;
        Check(t1 && ops::TextOrigin(t1, FS_MATRIX{ 1, 0, 0, 1, 0, 0 }, ox, oy) && std::fabs(ox - 72) < 0.5f,
              "text origin = where its baseline starts");

        // "Always show guide lines": which guides meet something exactly
        FS_MATRIX idm{ 1, 0, 0, 1, 0, 0 };
        ops::AddText(a, 0, L"Below", fc, 12, 72, 650, 0, 0, 0, aerr);        // same start, other row
        ops::AddText(a, 0, L"Same row", fc, 12, 300, 700, 0, 0, 0, aerr);    // same baseline
        ops::AddLine(a, 0, 400, 600, 400, 760, 0, 0, 0, 1.0f);              // vertical line
        std::vector<FPDF_PAGEOBJECT> mine{ t1 };
        auto refs = ops::GuideRefs(a, 0, mine);
        Check(refs.size() == 3, "guide lines look at every other object (text and lines)");
        ops::GuideEdges ge = ops::SelectionEdges(mine, idm, ops::ObjectBounds(t1));
        Check(ge.baseline && std::fabs(ge.x[0] - 72) < 0.01f && std::fabs(ge.y[0] - 700) < 0.01f,
              "guides go through the text's start and baseline");
        ops::GuideMatch gm = ops::MatchGuides(refs, ge, 0.25f);
        Check(gm.x[0] && gm.y[0] && gm.boxes.size() == 2, "guides show the text below and the text in the same row");
        ops::TranslateObjectPage(a, 0, t1, idm, 3, 0);
        ge = ops::SelectionEdges(mine, idm, ops::ObjectBounds(t1));
        gm = ops::MatchGuides(refs, ge, 0.25f);
        Check(!gm.x[0] && gm.y[0], "3 pt to the right: start no longer lines up, row still does");
        PRect tb = ops::ObjectBounds(t1);
        ops::TranslateObjectPage(a, 0, t1, idm, 400 - tb.r, 0);
        ge = ops::SelectionEdges(mine, idm, ops::ObjectBounds(t1));
        gm = ops::MatchGuides(refs, ge, 0.25f);
        Check(gm.x[1], "right edge meets the vertical line");
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
    RunEditTests();
    PdfShutdown();

    Out("\n%s  (%d failure%s)\n", g_fail == 0 ? "ALL PASSED" : "FAILURES", g_fail, g_fail == 1 ? "" : "s");
    if (g_log) fclose(g_log);
    return g_fail == 0 ? 0 : 1;
}
