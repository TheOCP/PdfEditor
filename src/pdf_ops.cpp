#include "pdf_ops.h"
#include <cstring>
#include <algorithm>
#include <cmath>

namespace ops {

int ObjectType(FPDF_PAGEOBJECT obj) { return obj ? FPDFPageObj_GetType(obj) : FPDF_PAGEOBJ_UNKNOWN; }

PRect ObjectBounds(FPDF_PAGEOBJECT obj) {
    float l = 0, b = 0, r = 0, t = 0;
    if (obj) FPDFPageObj_GetBounds(obj, &l, &b, &r, &t);
    return PRect::FromPoints(l, b, r, t);
}

PRect ObjectPageBounds(FPDF_PAGEOBJECT obj, const FS_MATRIX& accum) {
    PRect lb = ObjectBounds(obj);
    float xs[4], ys[4];
    MatApply(accum, lb.l, lb.b, xs[0], ys[0]);
    MatApply(accum, lb.r, lb.b, xs[1], ys[1]);
    MatApply(accum, lb.r, lb.t, xs[2], ys[2]);
    MatApply(accum, lb.l, lb.t, xs[3], ys[3]);
    PRect r{ xs[0], ys[0], xs[0], ys[0] };
    for (int i = 1; i < 4; ++i) {
        r.l = std::min(r.l, xs[i]); r.r = std::max(r.r, xs[i]);
        r.b = std::min(r.b, ys[i]); r.t = std::max(r.t, ys[i]);
    }
    return r;
}

// Recursively gathers leaf objects, descending into Form XObjects. `accumParent`
// maps the current container's space to page space.
static void CollectLeaves(FPDF_PAGEOBJECT container, bool isPage, FPDF_PAGE page,
                          const FS_MATRIX& accumParent, std::vector<ObjInfo>& out, int depth) {
    if (depth > 16) return;   // guard against pathological nesting
    int n = isPage ? FPDFPage_CountObjects(page) : FPDFFormObj_CountObjects(container);
    for (int i = 0; i < n; ++i) {
        FPDF_PAGEOBJECT o = isPage ? FPDFPage_GetObject(page, i) : FPDFFormObj_GetObject(container, (unsigned long)i);
        if (!o) continue;
        int type = FPDFPageObj_GetType(o);
        if (type == FPDF_PAGEOBJ_FORM) {
            FS_MATRIX mf{ 1, 0, 0, 1, 0, 0 };
            FPDFPageObj_GetMatrix(o, &mf);
            FS_MATRIX accum = MatMul(mf, accumParent);   // child space -> page space
            size_t before = out.size();
            CollectLeaves(o, false, nullptr, accum, out, depth + 1);
            // A form with no drawable leaves is itself selectable (e.g. empty or opaque).
            if (out.size() == before) {
                ObjInfo info;
                info.obj = o;
                info.parent = isPage ? nullptr : container;
                info.type = type;
                info.accum = accumParent;
                info.bounds = ObjectPageBounds(o, accumParent);
                out.push_back(info);
            }
        } else if (type != FPDF_PAGEOBJ_UNKNOWN) {
            ObjInfo info;
            info.obj = o;
            info.parent = isPage ? nullptr : container;
            info.type = type;
            info.accum = accumParent;
            info.bounds = ObjectPageBounds(o, accumParent);
            out.push_back(info);
        }
    }
}

std::vector<ObjInfo> ListObjects(Document& d, int page) {
    std::vector<ObjInfo> v;
    FPDF_PAGE p = d.Page(page);
    if (!p) return v;
    FS_MATRIX id{ 1, 0, 0, 1, 0, 0 };
    CollectLeaves(nullptr, true, p, id, v, 0);
    return v;
}

std::optional<ObjInfo> HitTest(Document& d, int page, float px, float py, float tolPt) {
    auto objs = ListObjects(d, page);
    const ObjInfo* best = nullptr;
    float bestArea = 1e30f;
    for (const auto& o : objs) {
        PRect b = o.bounds;
        b.l -= tolPt; b.b -= tolPt; b.r += tolPt; b.t += tolPt;
        if (!b.Contains(px, py)) continue;
        float area = std::max(1.0f, o.bounds.W()) * std::max(1.0f, o.bounds.H());
        // Prefer the smallest (most specific) object; on a tie, the later/topmost one.
        if (area <= bestArea) { bestArea = area; best = &o; }
    }
    if (best) return *best;
    return std::nullopt;
}

// --------------------------------------------------------------------- text

TextProps ReadText(Document& d, int page, FPDF_PAGEOBJECT obj) {
    TextProps tp;
    if (!obj) return tp;
    if (FPDFPageObj_GetType(obj) == FPDF_PAGEOBJ_TEXT) {
        tp.isText = true;
        float sz = 12;
        if (FPDFTextObj_GetFontSize(obj, &sz)) tp.fontSize = sz;
        FPDF_TEXTPAGE text = d.TextPage(page);
        unsigned long bytes = FPDFTextObj_GetText(obj, text, nullptr, 0);
        if (bytes >= 2) {
            std::vector<unsigned short> buf(bytes / 2 + 1, 0);
            FPDFTextObj_GetText(obj, text, buf.data(), bytes);
            tp.text = reinterpret_cast<wchar_t*>(buf.data());
        }
        if (FPDF_FONT f = FPDFTextObj_GetFont(obj)) {
            char nm[256] = { 0 };
            size_t got = FPDFFont_GetBaseFontName(f, nm, sizeof(nm));
            if (got > 0) tp.fontName.assign(nm, strnlen(nm, sizeof(nm)));
        }
    }
    unsigned r = 0, g = 0, b = 0, a = 255;
    FPDFPageObj_GetFillColor(obj, &r, &g, &b, &a);
    tp.r = r; tp.g = g; tp.b = b; tp.a = a;
    return tp;
}

bool SetObjectText(Document& d, int page, FPDF_PAGEOBJECT obj, const std::wstring& text) {
    if (!obj || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_TEXT) return false;
    if (!FPDFText_SetText(obj, WS(text))) return false;
    d.ContentChanged(page);
    return true;
}

void SetObjectFillColor(Document& d, int page, FPDF_PAGEOBJECT obj, unsigned r, unsigned g, unsigned b, unsigned a) {
    if (!obj) return;
    FPDFPageObj_SetFillColor(obj, r, g, b, a);
    if (FPDFPageObj_GetType(obj) == FPDF_PAGEOBJ_PATH) FPDFPageObj_SetStrokeColor(obj, r, g, b, a);
    d.ContentChanged(page);
}

FPDF_PAGEOBJECT AddText(Document& d, int page, const std::wstring& text, const FontChoice& fc,
                        double fontSize, float px, float py, unsigned r, unsigned g, unsigned b,
                        std::string& err) {
    FPDF_PAGE p = d.Page(page);
    if (!p) { err = "page not loaded"; return nullptr; }
    FPDF_FONT font = d.GetFont(fc, err);
    if (!font) return nullptr;
    FPDF_PAGEOBJECT obj = FPDFPageObj_CreateTextObj(d.Raw(), font, (float)fontSize);
    if (!obj) { err = "could not create text object"; return nullptr; }
    if (!text.empty() && !FPDFText_SetText(obj, WS(text))) {
        FPDFPageObj_Destroy(obj);
        err = "could not set text (font may lack these glyphs)";
        return nullptr;
    }
    FPDFPageObj_SetFillColor(obj, r, g, b, 255);
    FPDFPageObj_Transform(obj, 1, 0, 0, 1, px, py);
    FPDFPage_InsertObject(p, obj);
    d.ContentChanged(page);
    return obj;
}

bool RestyleText(Document& d, int page, FPDF_PAGEOBJECT obj, const FontChoice& fc, double fontSize,
                 std::string& err) {
    FPDF_PAGE p = d.Page(page);
    if (!p || !obj || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_TEXT) { err = "not a text object"; return false; }
    TextProps old = ReadText(d, page, obj);
    FS_MATRIX m{ 1, 0, 0, 1, 0, 0 };
    FPDFPageObj_GetMatrix(obj, &m);
    FPDF_FONT font = d.GetFont(fc, err);
    if (!font) return false;
    FPDF_PAGEOBJECT repl = FPDFPageObj_CreateTextObj(d.Raw(), font, (float)fontSize);
    if (!repl) { err = "could not create text object"; return false; }
    if (!old.text.empty() && !FPDFText_SetText(repl, WS(old.text))) {
        FPDFPageObj_Destroy(repl);
        err = "font lacks some of these glyphs";
        return false;
    }
    FPDFPageObj_SetFillColor(repl, old.r, old.g, old.b, old.a ? old.a : 255);
    // CreateTextObj bakes the size into the glyphs; keep only the object's
    // placement (translation + any rotation/shear), dropping the old scale.
    FS_MATRIX place{ 1, 0, 0, 1, m.e, m.f };
    FPDFPageObj_SetMatrix(repl, &place);
    FPDFPage_InsertObject(p, repl);
    FPDFPage_RemoveObject(p, obj);
    FPDFPageObj_Destroy(obj);
    d.ContentChanged(page);
    return true;
}

// ----------------------------------------------------------------- geometry

void MoveObject(Document& d, int page, FPDF_PAGEOBJECT obj, float dx, float dy) {
    if (!obj || (dx == 0 && dy == 0)) return;
    FPDFPageObj_Transform(obj, 1, 0, 0, 1, dx, dy);
    d.ContentChanged(page);
}

void SetObjectPos(Document& d, int page, FPDF_PAGEOBJECT obj, float left, float bottom) {
    if (!obj) return;
    PRect b = ObjectBounds(obj);
    MoveObject(d, page, obj, left - b.l, bottom - b.b);
}

void ScaleObject(Document& d, int page, FPDF_PAGEOBJECT obj, float sx, float sy) {
    if (!obj || sx <= 0 || sy <= 0) return;
    PRect b = ObjectBounds(obj);
    FS_MATRIX m = MatScaleAbout(sx, sy, b.l, b.b);
    FPDFPageObj_Transform(obj, m.a, m.b, m.c, m.d, m.e, m.f);
    d.ContentChanged(page);
}

bool DeleteObject(Document& d, int page, FPDF_PAGEOBJECT obj) {
    FPDF_PAGE p = d.Page(page);
    if (!p || !obj) return false;
    if (!FPDFPage_RemoveObject(p, obj)) return false;
    FPDFPageObj_Destroy(obj);
    d.ContentChanged(page);
    return true;
}

void TranslateObjectPage(Document& d, int page, FPDF_PAGEOBJECT obj, const FS_MATRIX& accum,
                         float dxPage, float dyPage) {
    if (!obj || (dxPage == 0 && dyPage == 0)) return;
    // Convert the page-space delta into the object's own container space.
    FS_MATRIX inv;
    float lx = dxPage, ly = dyPage;
    if (MatInvert(accum, inv)) {           // linear part only (ignore translation)
        lx = inv.a * dxPage + inv.c * dyPage;
        ly = inv.b * dxPage + inv.d * dyPage;
    }
    FPDFPageObj_Transform(obj, 1, 0, 0, 1, lx, ly);
    d.ContentChanged(page);
}

void SetObjectPosPage(Document& d, int page, FPDF_PAGEOBJECT obj, const FS_MATRIX& accum,
                      float leftPage, float bottomPage) {
    if (!obj) return;
    PRect b = ObjectPageBounds(obj, accum);
    TranslateObjectPage(d, page, obj, accum, leftPage - b.l, bottomPage - b.b);
}

bool DeleteObjectNested(Document& d, int page, FPDF_PAGEOBJECT obj, FPDF_PAGEOBJECT parent) {
    FPDF_PAGE p = d.Page(page);
    if (!p || !obj) return false;
    bool ok = parent ? (FPDFFormObj_RemoveObject(parent, obj) != 0)
                     : (FPDFPage_RemoveObject(p, obj) != 0);
    if (!ok) return false;
    FPDFPageObj_Destroy(obj);
    d.ContentChanged(page);
    return true;
}

static bool SubtreeContains(FPDF_PAGEOBJECT form, FPDF_PAGEOBJECT target) {
    int n = FPDFFormObj_CountObjects(form);
    for (int i = 0; i < n; ++i) {
        FPDF_PAGEOBJECT c = FPDFFormObj_GetObject(form, (unsigned long)i);
        if (!c) continue;
        if (c == target) return true;
        if (FPDFPageObj_GetType(c) == FPDF_PAGEOBJ_FORM && SubtreeContains(c, target)) return true;
    }
    return false;
}

// Lifts every child of `form` onto `p`, baking `form`'s matrix into each, then
// destroys `form`. Recurses so nested groups flatten completely.
static void ExplodeFormToPage(FPDF_PAGE p, FPDF_PAGEOBJECT form) {
    FS_MATRIX mf{ 1, 0, 0, 1, 0, 0 };
    FPDFPageObj_GetMatrix(form, &mf);
    std::vector<FPDF_PAGEOBJECT> kids;
    int n = FPDFFormObj_CountObjects(form);
    for (int i = 0; i < n; ++i)
        if (FPDF_PAGEOBJECT c = FPDFFormObj_GetObject(form, (unsigned long)i)) kids.push_back(c);
    for (FPDF_PAGEOBJECT c : kids) {
        if (!FPDFFormObj_RemoveObject(form, c)) continue;
        FPDFPageObj_Transform(c, mf.a, mf.b, mf.c, mf.d, mf.e, mf.f);   // form space -> page space
        FPDFPage_InsertObject(p, c);
        if (FPDFPageObj_GetType(c) == FPDF_PAGEOBJ_FORM) ExplodeFormToPage(p, c);
    }
    FPDFPage_RemoveObject(p, form);
    FPDFPageObj_Destroy(form);
}

bool PromoteObject(Document& d, int page, FPDF_PAGEOBJECT target) {
    FPDF_PAGE p = d.Page(page);
    if (!p || !target) return false;
    int n = FPDFPage_CountObjects(p);
    for (int i = 0; i < n; ++i) {
        FPDF_PAGEOBJECT o = FPDFPage_GetObject(p, i);
        if (o == target) return true;                    // already top-level
        if (o && FPDFPageObj_GetType(o) == FPDF_PAGEOBJ_FORM && SubtreeContains(o, target)) {
            ExplodeFormToPage(p, o);
            d.ContentChanged(page);
            return true;
        }
    }
    return true;   // not nested under any top-level form
}

// ------------------------------------------------------------------- images

FPDF_PAGEOBJECT AddImage(Document& d, int page, const Image& img, float px, float py, float widthPt,
                         std::string& err) {
    FPDF_PAGE p = d.Page(page);
    if (!p) { err = "page not loaded"; return nullptr; }
    if (img.w <= 0 || img.h <= 0 || img.px.empty()) { err = "empty image"; return nullptr; }
    FPDF_PAGEOBJECT obj = FPDFPageObj_NewImageObj(d.Raw());
    if (!obj) { err = "could not create image object"; return nullptr; }

    // PDFium takes a copy of the bitmap on SetBitmap, so a stack bitmap is fine.
    FPDF_BITMAP bmp = FPDFBitmap_CreateEx(img.w, img.h, FPDFBitmap_BGRA,
                                          const_cast<uint32_t*>(img.px.data()), img.w * 4);
    if (!bmp) { FPDFPageObj_Destroy(obj); err = "bitmap allocation failed"; return nullptr; }
    FPDF_BOOL ok = FPDFImageObj_SetBitmap(nullptr, 0, obj, bmp);
    FPDFBitmap_Destroy(bmp);
    if (!ok) { FPDFPageObj_Destroy(obj); err = "could not attach image"; return nullptr; }

    if (widthPt <= 0) widthPt = std::min(400.0f, (float)img.w * 0.75f);
    float heightPt = widthPt * (float)img.h / (float)img.w;
    FS_MATRIX m{ widthPt, 0, 0, heightPt, px, py };   // image unit square -> placed rect
    FPDFPageObj_SetMatrix(obj, &m);
    FPDFPage_InsertObject(p, obj);
    // Bake immediately so the embedded image stream is finalised.
    FPDFPage_GenerateContent(p);
    d.ContentChanged(page);
    return obj;
}

// ----------------------------------------------------------- shapes/redaction

FPDF_PAGEOBJECT AddRect(Document& d, int page, PRect box, bool fill, unsigned fr, unsigned fg, unsigned fb,
                        bool stroke, unsigned sr, unsigned sg, unsigned sb, float strokeW) {
    FPDF_PAGE p = d.Page(page);
    if (!p) return nullptr;
    FPDF_PAGEOBJECT r = FPDFPageObj_CreateNewRect(box.l, box.b, box.W(), box.H());
    if (!r) return nullptr;
    if (fill) FPDFPageObj_SetFillColor(r, fr, fg, fb, 255);
    if (stroke) {
        FPDFPageObj_SetStrokeColor(r, sr, sg, sb, 255);
        FPDFPageObj_SetStrokeWidth(r, strokeW);
    }
    FPDFPath_SetDrawMode(r, fill ? FPDF_FILLMODE_WINDING : FPDF_FILLMODE_NONE, stroke ? 1 : 0);
    FPDFPage_InsertObject(p, r);
    d.ContentChanged(page);
    return r;
}

int Redact(Document& d, int page, PRect box, unsigned r, unsigned g, unsigned b) {
    FPDF_PAGE p = d.Page(page);
    if (!p) return 0;
    int removed = 0;
    // Remove top-level objects whose bounds fall entirely within the box.
    for (bool again = true; again;) {
        again = false;
        int n = FPDFPage_CountObjects(p);
        for (int i = 0; i < n; ++i) {
            FPDF_PAGEOBJECT o = FPDFPage_GetObject(p, i);
            if (!o) continue;
            if (ObjectBounds(o).Inside(box)) {
                if (FPDFPage_RemoveObject(p, o)) {
                    FPDFPageObj_Destroy(o);
                    ++removed;
                    again = true;
                    break;
                }
            }
        }
    }
    AddRect(d, page, box, true, r, g, b, false, 0, 0, 0, 0);
    d.ContentChanged(page);
    return removed;
}

// -------------------------------------------------------- extraction/search

std::wstring ExtractText(Document& d, int page) {
    FPDF_TEXTPAGE t = d.TextPage(page);
    if (!t) return {};
    int n = FPDFText_CountChars(t);
    if (n <= 0) return {};
    std::vector<unsigned short> buf((size_t)n + 1, 0);
    int got = FPDFText_GetText(t, 0, n, buf.data());
    if (got <= 0) return {};
    return std::wstring(reinterpret_cast<wchar_t*>(buf.data()), (size_t)got - (buf[got - 1] == 0 ? 1 : 0));
}

std::wstring ExtractAllText(Document& d) {
    std::wstring all;
    for (int i = 0; i < d.PageCount(); ++i) {
        if (i) all += L"\r\n\f\r\n";
        all += ExtractText(d, i);
    }
    return all;
}

std::vector<SearchHit> SearchText(Document& d, const std::wstring& needle, bool matchCase, bool whole) {
    std::vector<SearchHit> hits;
    if (needle.empty()) return hits;
    unsigned long flags = (matchCase ? FPDF_MATCHCASE : 0) | (whole ? FPDF_MATCHWHOLEWORD : 0);
    for (int pg = 0; pg < d.PageCount(); ++pg) {
        FPDF_TEXTPAGE t = d.TextPage(pg);
        if (!t) continue;
        FPDF_SCHHANDLE sh = FPDFText_FindStart(t, WS(needle), flags, 0);
        if (!sh) continue;
        while (FPDFText_FindNext(sh)) {
            int idx = FPDFText_GetSchResultIndex(sh);
            int cnt = FPDFText_GetSchCount(sh);
            if (cnt <= 0) continue;
            PRect box;
            bool first = true;
            for (int c = idx; c < idx + cnt; ++c) {
                double l, r, b, t2;
                if (!FPDFText_GetCharBox(t, c, &l, &r, &b, &t2)) continue;
                PRect cb = PRect::FromPoints((float)l, (float)b, (float)r, (float)t2);
                box = first ? cb : box.Union(cb);
                first = false;
            }
            if (!first) hits.push_back({ pg, box });
            if (hits.size() > 20000) break;
        }
        FPDFText_FindClose(sh);
    }
    return hits;
}

// ------------------------------------------------------------------ page tree

bool InsertBlankPage(Document& d, int at, float wPt, float hPt) {
    if (!d.Raw()) return false;
    if (at < 0 || at > d.PageCount()) at = d.PageCount();
    d.Bake();
    FPDF_PAGE p = FPDFPage_New(d.Raw(), at, wPt, hPt);
    if (!p) return false;
    FPDFPage_GenerateContent(p);
    FPDF_ClosePage(p);
    d.StructureChanged();
    return true;
}

bool DeletePages(Document& d, const std::vector<int>& pages) {
    if (!d.Raw() || pages.empty() || (int)pages.size() >= d.PageCount()) return false;
    std::vector<int> sorted = pages;
    std::sort(sorted.rbegin(), sorted.rend());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    d.Bake();
    for (int i : sorted)
        if (i >= 0 && i < d.PageCount()) FPDFPage_Delete(d.Raw(), i);
    d.StructureChanged();
    return true;
}

bool MovePage(Document& d, int from, int to) {
    int n = d.PageCount();
    if (!d.Raw() || from < 0 || from >= n || to < 0 || to >= n || from == to) return false;
    d.Bake();
    int idx = from;
    if (!FPDF_MovePages(d.Raw(), &idx, 1, to)) return false;
    d.StructureChanged();
    return true;
}

bool RotatePage(Document& d, int page, int quartersCW) {
    FPDF_PAGE p = d.Page(page);
    if (!p) return false;
    int cur = FPDFPage_GetRotation(p);
    int nr = ((cur + quartersCW) % 4 + 4) % 4;
    FPDFPage_SetRotation(p, nr);
    d.StructureChanged();
    return true;
}

bool DuplicatePage(Document& d, int page) {
    if (!d.Raw() || page < 0 || page >= d.PageCount()) return false;
    d.Bake();
    int idx = page;
    if (!FPDF_ImportPagesByIndex(d.Raw(), d.Raw(), &idx, 1, page + 1)) return false;
    d.StructureChanged();
    return true;
}

bool ImportPagesFromFile(Document& d, const std::wstring& file, const std::string& pw,
                         const std::string& range, int at, std::string& err) {
    if (!d.Raw()) { err = "no document open"; return false; }
    Bytes bytes;
    if (!ReadFileBytes(file, bytes)) { err = "could not read that PDF"; return false; }
    FPDF_DOCUMENT src = FPDF_LoadMemDocument64(bytes.data(), bytes.size(), pw.empty() ? nullptr : pw.c_str());
    if (!src) { err = PdfErrorString(FPDF_GetLastError()); return false; }
    if (at < 0 || at > d.PageCount()) at = d.PageCount();
    d.Bake();
    FPDF_BOOL ok = FPDF_ImportPages(d.Raw(), src, range.empty() ? nullptr : range.c_str(), at);
    FPDF_CloseDocument(src);
    if (!ok) { err = "import failed (check the page range)"; return false; }
    d.StructureChanged();
    return true;
}

bool ExportPagesToImages(Document& d, const std::vector<int>& pages, const std::wstring& dir,
                         float dpi, std::string& err) {
    if (pages.empty()) { err = "no pages selected"; return false; }
    float scale = dpi / 72.0f;
    std::vector<uint32_t> buf;
    for (int i : pages) {
        if (i < 0 || i >= d.PageCount()) continue;
        FS_SIZEF sz = d.PageSizePt(i);
        int w = std::max(1, (int)std::lround(sz.width * scale));
        int h = std::max(1, (int)std::lround(sz.height * scale));
        if ((int64_t)w * h > (int64_t)120000000) { err = "page too large at this DPI"; return false; }
        if (!d.Render(i, scale, 0, 0, w, h, buf, true, true)) { err = "render failed"; return false; }
        wchar_t name[64];
        swprintf(name, 64, L"\\page-%03d.png", i + 1);
        std::wstring out = dir + name;
        if (!SaveImageFile(out, buf.data(), w, h, err)) return false;
    }
    return true;
}

// ---------------------------------------------------------- forms/annotations

std::vector<FieldInfo> ListFormFields(Document& d) {
    std::vector<FieldInfo> out;
    FPDF_FORMHANDLE form = d.Form();
    if (!form) return out;
    for (int pg = 0; pg < d.PageCount(); ++pg) {
        FPDF_PAGE p = d.Page(pg);
        if (!p) continue;
        int na = FPDFPage_GetAnnotCount(p);
        for (int a = 0; a < na; ++a) {
            FPDF_ANNOTATION an = FPDFPage_GetAnnot(p, a);
            if (!an) continue;
            if (FPDFAnnot_GetSubtype(an) == FPDF_ANNOT_WIDGET) {
                FieldInfo fi;
                fi.page = pg;
                fi.type = FPDFAnnot_GetFormFieldType(form, an);
                fi.name = ReadUtf16([&](unsigned short* b, unsigned long n) {
                    return FPDFAnnot_GetFormFieldName(form, an, b, n);
                });
                fi.value = ReadUtf16([&](unsigned short* b, unsigned long n) {
                    return FPDFAnnot_GetFormFieldValue(form, an, b, n);
                });
                FS_RECTF r{};
                if (FPDFAnnot_GetRect(an, &r)) fi.box = PRect::FromPoints(r.left, r.bottom, r.right, r.top);
                out.push_back(std::move(fi));
            }
            FPDFPage_CloseAnnot(an);
        }
    }
    return out;
}

bool FlattenAll(Document& d) {
    if (!d.Raw() || d.PageCount() == 0) return false;
    d.Bake();
    bool any = false;
    for (int i = 0; i < d.PageCount(); ++i) {
        FPDF_PAGE p = d.Page(i);
        if (p && FPDFPage_Flatten(p, FLAT_NORMALDISPLAY) != FLATTEN_FAIL) any = true;
    }
    d.StructureChanged();
    return any;
}

std::wstring GetMeta(Document& d, const char* key) {
    if (!d.Raw()) return {};
    return ReadUtf16([&](unsigned short* b, unsigned long n) {
        return FPDF_GetMetaText(d.Raw(), key, b, n);
    });
}

} // namespace ops
