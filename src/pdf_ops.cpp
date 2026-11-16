#include "pdf_ops.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <map>

namespace ops {

// ============================================================== shared helpers

// Transform an object AND its clip path. PDFium's FPDFPageObj_Transform leaves
// the clip where it was, so clipped content would be cut off after a move.
static void Xform(FPDF_PAGEOBJECT o, double a, double b, double c, double d, double e, double f) {
    FPDFPageObj_Transform(o, a, b, c, d, e, f);
    FPDFPageObj_TransformClipPath(o, a, b, c, d, e, f);
}
static void Xform(FPDF_PAGEOBJECT o, const FS_MATRIX& m) { Xform(o, m.a, m.b, m.c, m.d, m.e, m.f); }

float MatrixScale(const FS_MATRIX& m) {
    float s = std::sqrt(std::fabs(m.a * m.d - m.b * m.c));
    return s > 1e-6f ? s : 1.0f;
}

static FS_MATRIX ObjMatrix(FPDF_PAGEOBJECT o) {
    FS_MATRIX m{ 1, 0, 0, 1, 0, 0 };
    if (o) FPDFPageObj_GetMatrix(o, &m);
    return m;
}

// Index of a top-level object on its page, or -1.
static int TopIndex(FPDF_PAGE p, FPDF_PAGEOBJECT o) {
    int n = FPDFPage_CountObjects(p);
    for (int i = 0; i < n; ++i)
        if (FPDFPage_GetObject(p, i) == o) return i;
    return -1;
}

static void InsertAt(FPDF_PAGE p, FPDF_PAGEOBJECT o, int index) {
    if (index < 0 || index > FPDFPage_CountObjects(p) || !FPDFPage_InsertObjectAtIndex(p, o, (size_t)index))
        FPDFPage_InsertObject(p, o);
}

static std::wstring FromUtf32(unsigned int u) {
    std::wstring s;
    if (u >= 0x10000 && u <= 0x10FFFF) {
        u -= 0x10000;
        s += (wchar_t)(0xD800 + (u >> 10));
        s += (wchar_t)(0xDC00 + (u & 0x3FF));
    } else if (u) {
        s += (wchar_t)u;
    }
    return s;
}

// ---- path geometry ----------------------------------------------------------

struct PPt { float x = 0, y = 0; };
struct POp { bool bez = false; PPt c1, c2, end; };
struct PSub { PPt start; std::vector<POp> ops; bool closed = false; };

static std::vector<ClipObject::Seg> RawSegs(FPDF_PAGEOBJECT o) {
    std::vector<ClipObject::Seg> v;
    int n = FPDFPath_CountSegments(o);
    for (int i = 0; i < n; ++i) {
        FPDF_PATHSEGMENT s = FPDFPath_GetPathSegment(o, i);
        if (!s) continue;
        ClipObject::Seg g;
        g.type = FPDFPathSegment_GetType(s);
        FPDFPathSegment_GetPoint(s, &g.x, &g.y);
        g.close = FPDFPathSegment_GetClose(s) != 0;
        v.push_back(g);
    }
    return v;
}

// A cubic curve is exposed as three consecutive BEZIERTO points (c1, c2, end).
static std::vector<PSub> ParseSegs(const std::vector<ClipObject::Seg>& r) {
    std::vector<PSub> subs;
    PPt cur;
    for (size_t i = 0; i < r.size(); ++i) {
        const auto& s = r[i];
        bool close = s.close;
        if (s.type == FPDF_SEGMENT_MOVETO) {
            subs.push_back(PSub{});
            subs.back().start = { s.x, s.y };
            cur = subs.back().start;
        } else {
            if (subs.empty() || subs.back().closed) {        // implicit new sub-path
                PPt st = subs.empty() ? cur : subs.back().start;
                subs.push_back(PSub{});
                subs.back().start = st;
            }
            POp op;
            if (s.type == FPDF_SEGMENT_BEZIERTO && i + 2 < r.size() &&
                r[i + 1].type == FPDF_SEGMENT_BEZIERTO && r[i + 2].type == FPDF_SEGMENT_BEZIERTO) {
                op.bez = true;
                op.c1 = { s.x, s.y };
                op.c2 = { r[i + 1].x, r[i + 1].y };
                op.end = { r[i + 2].x, r[i + 2].y };
                close = r[i + 2].close;
                i += 2;
            } else {
                op.end = { s.x, s.y };
            }
            subs.back().ops.push_back(op);
            cur = op.end;
        }
        if (close && !subs.empty()) { subs.back().closed = true; cur = subs.back().start; }
    }
    return subs;
}

static std::vector<PSub> ParsePath(FPDF_PAGEOBJECT o) { return ParseSegs(RawSegs(o)); }

static FPDF_PAGEOBJECT BuildPath(const std::vector<PSub>& subs) {
    if (subs.empty()) return nullptr;
    FPDF_PAGEOBJECT o = FPDFPageObj_CreateNewPath(subs[0].start.x, subs[0].start.y);
    if (!o) return nullptr;
    for (size_t k = 0; k < subs.size(); ++k) {
        const PSub& sp = subs[k];
        if (k > 0) FPDFPath_MoveTo(o, sp.start.x, sp.start.y);
        for (const POp& op : sp.ops) {
            if (op.bez) FPDFPath_BezierTo(o, op.c1.x, op.c1.y, op.c2.x, op.c2.y, op.end.x, op.end.y);
            else FPDFPath_LineTo(o, op.end.x, op.end.y);
        }
        if (sp.closed) FPDFPath_Close(o);
    }
    return o;
}

struct PathStyle {
    FS_MATRIX m{ 1, 0, 0, 1, 0, 0 };
    int fillMode = 0;
    bool stroke = false;
    unsigned fr = 0, fg = 0, fb = 0, fa = 255, sr = 0, sg = 0, sb = 0, sa = 255;
    float sw = 1;
    std::vector<float> dash;
    float phase = 0;
    int cap = -1, join = -1;
    int gid = 0;
};

static PathStyle ReadPathStyle(FPDF_PAGEOBJECT o) {
    PathStyle s;
    s.m = ObjMatrix(o);
    FPDF_BOOL st = 0;
    FPDFPath_GetDrawMode(o, &s.fillMode, &st);
    s.stroke = st != 0;
    FPDFPageObj_GetFillColor(o, &s.fr, &s.fg, &s.fb, &s.fa);
    FPDFPageObj_GetStrokeColor(o, &s.sr, &s.sg, &s.sb, &s.sa);
    FPDFPageObj_GetStrokeWidth(o, &s.sw);
    int dc = FPDFPageObj_GetDashCount(o);
    if (dc > 0 && dc < 64) {
        s.dash.resize((size_t)dc);
        if (!FPDFPageObj_GetDashArray(o, s.dash.data(), s.dash.size())) s.dash.clear();
        FPDFPageObj_GetDashPhase(o, &s.phase);
    }
    s.cap = FPDFPageObj_GetLineCap(o);
    s.join = FPDFPageObj_GetLineJoin(o);
    s.gid = GetGroupId(o);
    return s;
}

static void ApplyPathStyle(Document& d, int page, FPDF_PAGEOBJECT o, const PathStyle& s, bool fill, bool stroke) {
    FPDFPath_SetDrawMode(o, fill ? (s.fillMode ? s.fillMode : FPDF_FILLMODE_WINDING) : FPDF_FILLMODE_NONE, stroke ? 1 : 0);
    if (fill) FPDFPageObj_SetFillColor(o, s.fr, s.fg, s.fb, s.fa);
    if (stroke) {
        FPDFPageObj_SetStrokeColor(o, s.sr, s.sg, s.sb, s.sa);
        FPDFPageObj_SetStrokeWidth(o, s.sw > 0 ? s.sw : 1.0f);
        if (!s.dash.empty()) FPDFPageObj_SetDashArray(o, s.dash.data(), s.dash.size(), s.phase);
        if (s.cap >= 0) FPDFPageObj_SetLineCap(o, s.cap);
        if (s.join >= 0) FPDFPageObj_SetLineJoin(o, s.join);
    }
    FPDFPageObj_SetMatrix(o, &s.m);
    if (s.gid) SetGroupId(d, page, o, s.gid);
}

// Flattens sub-paths to page-space polylines (curves sampled).
static void Polylines(const std::vector<PSub>& subs, const FS_MATRIX& full,
                      std::vector<std::vector<PPt>>& polys, std::vector<bool>& closed) {
    auto map = [&](PPt p) { PPt o; MatApply(full, p.x, p.y, o.x, o.y); return o; };
    for (const PSub& sp : subs) {
        std::vector<PPt> poly;
        PPt cur = sp.start;
        poly.push_back(map(cur));
        for (const POp& op : sp.ops) {
            if (op.bez) {
                for (int k = 1; k <= 12; ++k) {
                    float t = k / 12.0f, u = 1 - t;
                    PPt q;
                    q.x = u * u * u * cur.x + 3 * u * u * t * op.c1.x + 3 * u * t * t * op.c2.x + t * t * t * op.end.x;
                    q.y = u * u * u * cur.y + 3 * u * u * t * op.c1.y + 3 * u * t * t * op.c2.y + t * t * t * op.end.y;
                    poly.push_back(map(q));
                }
            } else {
                poly.push_back(map(op.end));
            }
            cur = op.end;
        }
        polys.push_back(std::move(poly));
        closed.push_back(sp.closed);
    }
}

static float DistToSeg(PPt p, PPt a, PPt b) {
    float vx = b.x - a.x, vy = b.y - a.y;
    float wx = p.x - a.x, wy = p.y - a.y;
    float len2 = vx * vx + vy * vy;
    float t = len2 > 1e-12f ? std::clamp((wx * vx + wy * vy) / len2, 0.0f, 1.0f) : 0.0f;
    float dx = a.x + t * vx - p.x, dy = a.y + t * vy - p.y;
    return std::sqrt(dx * dx + dy * dy);
}

static float DistToPolys(PPt p, const std::vector<std::vector<PPt>>& polys, const std::vector<bool>& closed,
                         bool closeAll) {
    float best = 1e30f;
    for (size_t k = 0; k < polys.size(); ++k) {
        const auto& pl = polys[k];
        if (pl.size() == 1) best = std::min(best, DistToSeg(p, pl[0], pl[0]));
        for (size_t i = 1; i < pl.size(); ++i) best = std::min(best, DistToSeg(p, pl[i - 1], pl[i]));
        if ((closed[k] || closeAll) && pl.size() > 2) best = std::min(best, DistToSeg(p, pl.back(), pl.front()));
    }
    return best;
}

static bool InsidePolys(PPt p, const std::vector<std::vector<PPt>>& polys, bool evenOdd) {
    int winding = 0, crossings = 0;
    for (const auto& pl : polys) {
        size_t n = pl.size();
        if (n < 3) continue;
        for (size_t i = 0; i < n; ++i) {
            PPt a = pl[i], b = pl[(i + 1) % n];
            if ((a.y <= p.y) != (b.y <= p.y)) {
                float x = a.x + (p.y - a.y) * (b.x - a.x) / (b.y - a.y);
                if (x > p.x) { ++crossings; winding += (b.y > a.y) ? 1 : -1; }
            }
        }
    }
    return evenOdd ? (crossings & 1) != 0 : winding != 0;
}

// ---- bitmaps -----------------------------------------------------------------

static bool BitmapToImage(FPDF_BITMAP bm, Image& out) {
    if (!bm) return false;
    int w = FPDFBitmap_GetWidth(bm), h = FPDFBitmap_GetHeight(bm);
    int stride = FPDFBitmap_GetStride(bm), fmt = FPDFBitmap_GetFormat(bm);
    const uint8_t* buf = static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bm));
    if (w <= 0 || h <= 0 || !buf) return false;
    out.w = w; out.h = h;
    out.px.assign((size_t)w * h, 0xFFFFFFFFu);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = buf + (size_t)y * stride;
        uint32_t* dst = &out.px[(size_t)y * w];
        for (int x = 0; x < w; ++x) {
            uint32_t b, g, r, a = 255;
            switch (fmt) {
            case FPDFBitmap_Gray: b = g = r = row[x]; break;
            case FPDFBitmap_BGR: b = row[x * 3]; g = row[x * 3 + 1]; r = row[x * 3 + 2]; break;
            case FPDFBitmap_BGRx: b = row[x * 4]; g = row[x * 4 + 1]; r = row[x * 4 + 2]; break;
            case FPDFBitmap_BGRA: b = row[x * 4]; g = row[x * 4 + 1]; r = row[x * 4 + 2]; a = row[x * 4 + 3]; break;
            case FPDFBitmap_BGRA_Premul:
                a = row[x * 4 + 3];
                b = a ? std::min(255u, (row[x * 4] * 255u + a / 2) / a) : 0;
                g = a ? std::min(255u, (row[x * 4 + 1] * 255u + a / 2) / a) : 0;
                r = a ? std::min(255u, (row[x * 4 + 2] * 255u + a / 2) / a) : 0;
                break;
            default: return false;
            }
            dst[x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    return true;
}

// Writes pixels into an image object. Fully opaque pixels are written without
// an alpha channel so PDFium doesn't attach a pointless soft mask.
static bool SetImagePixels(FPDF_PAGE* pages, int count, FPDF_PAGEOBJECT o, const Image& img) {
    if (img.w <= 0 || img.h <= 0 || img.px.size() < (size_t)img.w * img.h) return false;
    bool opaque = std::all_of(img.px.begin(), img.px.end(), [](uint32_t p) { return (p >> 24) == 255; });
    FPDF_BITMAP bmp = FPDFBitmap_CreateEx(img.w, img.h, opaque ? FPDFBitmap_BGRx : FPDFBitmap_BGRA,
                                          const_cast<uint32_t*>(img.px.data()), img.w * 4);
    if (!bmp) return false;
    FPDF_BOOL ok = FPDFImageObj_SetBitmap(pages, count, o, bmp);
    FPDFBitmap_Destroy(bmp);
    return ok != 0;
}

bool ImagePixels(FPDF_PAGEOBJECT img, Image& out) {
    if (!img || FPDFPageObj_GetType(img) != FPDF_PAGEOBJ_IMAGE) return false;
    FPDF_BITMAP bm = FPDFImageObj_GetBitmap(img);
    bool ok = BitmapToImage(bm, out);
    if (bm) FPDFBitmap_Destroy(bm);
    return ok;
}

// ============================================================ inspection

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

static void CollectLeaves(FPDF_PAGEOBJECT container, bool isPage, FPDF_PAGE page,
                          const FS_MATRIX& accumParent, std::vector<ObjInfo>& out, int depth) {
    if (depth > 16) return;
    int n = isPage ? FPDFPage_CountObjects(page) : FPDFFormObj_CountObjects(container);
    for (int i = 0; i < n; ++i) {
        FPDF_PAGEOBJECT o = isPage ? FPDFPage_GetObject(page, i) : FPDFFormObj_GetObject(container, (unsigned long)i);
        if (!o) continue;
        int type = FPDFPageObj_GetType(o);
        if (type == FPDF_PAGEOBJ_FORM) {
            FS_MATRIX accum = MatMul(ObjMatrix(o), accumParent);   // child space -> page space
            size_t before = out.size();
            CollectLeaves(o, false, nullptr, accum, out, depth + 1);
            if (out.size() == before) {      // an empty/opaque form stays selectable as a whole
                ObjInfo info;
                info.obj = o; info.parent = isPage ? nullptr : container;
                info.type = type; info.accum = accumParent;
                info.bounds = ObjectPageBounds(o, accumParent);
                out.push_back(info);
            }
        } else if (type != FPDF_PAGEOBJ_UNKNOWN) {
            ObjInfo info;
            info.obj = o; info.parent = isPage ? nullptr : container;
            info.type = type; info.accum = accumParent;
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

bool ObjectExists(Document& d, int page, FPDF_PAGEOBJECT obj) {
    FPDF_PAGE p = d.Page(page);
    if (!p || !obj) return false;
    int n = FPDFPage_CountObjects(p);
    for (int i = 0; i < n; ++i) {
        FPDF_PAGEOBJECT o = FPDFPage_GetObject(p, i);
        if (o == obj) return true;
        if (o && FPDFPageObj_GetType(o) == FPDF_PAGEOBJ_FORM && SubtreeContains(o, obj)) return true;
    }
    return false;
}

// Distance from a page point to a path leaf's drawn geometry (`full` maps the
// path's own coordinates to page space). Sets *inside for filled shapes.
static float PathDistance(FPDF_PAGEOBJECT o, const FS_MATRIX& full, float px, float py, bool* inside,
                          float* halfStroke) {
    std::vector<std::vector<PPt>> polys;
    std::vector<bool> closed;
    Polylines(ParsePath(o), full, polys, closed);
    int fm = 0; FPDF_BOOL st = 0;
    FPDFPath_GetDrawMode(o, &fm, &st);
    float sw = 0;
    if (st) { FPDFPageObj_GetStrokeWidth(o, &sw); sw *= MatrixScale(full); }
    if (halfStroke) *halfStroke = sw * 0.5f;
    PPt p{ px, py };
    if (inside) *inside = fm != FPDF_FILLMODE_NONE && InsidePolys(p, polys, fm == FPDF_FILLMODE_ALTERNATE);
    return DistToPolys(p, polys, closed, fm != FPDF_FILLMODE_NONE);
}

float DistanceToObject(FPDF_PAGEOBJECT obj, float px, float py) {
    if (!obj) return 1e30f;
    if (FPDFPageObj_GetType(obj) == FPDF_PAGEOBJ_PATH) {
        bool inside = false; float hs = 0;
        float dd = PathDistance(obj, ObjMatrix(obj), px, py, &inside, &hs);
        return inside ? 0.0f : std::max(0.0f, dd - hs);
    }
    PRect b = ObjectBounds(obj);
    float dx = std::max({ b.l - px, 0.0f, px - b.r });
    float dy = std::max({ b.b - py, 0.0f, py - b.t });
    return std::sqrt(dx * dx + dy * dy);
}

std::optional<ObjInfo> HitTest(Document& d, int page, float px, float py, float tolPt) {
    auto objs = ListObjects(d, page);
    const ObjInfo* bestEdge = nullptr; float bestEdgeD = 1e30f;
    const ObjInfo* bestFill = nullptr; float bestFillA = 1e30f;
    const ObjInfo* bestBox = nullptr;  float bestBoxA = 1e30f;
    for (const auto& o : objs) {
        PRect bb = o.bounds;
        float pad = tolPt + 1.0f;
        if (px < bb.l - pad || px > bb.r + pad || py < bb.b - pad || py > bb.t + pad) continue;
        float area = std::max(1.0f, bb.W()) * std::max(1.0f, bb.H());
        if (o.type == FPDF_PAGEOBJ_PATH) {
            FS_MATRIX full = MatMul(ObjMatrix(o.obj), o.accum);
            bool inside = false; float hs = 0;
            float dd = PathDistance(o.obj, full, px, py, &inside, &hs);
            float edge = std::max(0.0f, dd - hs);
            if (edge <= tolPt && edge <= bestEdgeD + 0.01f) { bestEdgeD = std::min(bestEdgeD, edge); bestEdge = &o; }
            if (inside && area <= bestFillA) { bestFillA = area; bestFill = &o; }
        } else {
            PRect e = bb;
            float t2 = tolPt * 0.5f;
            e.l -= t2; e.b -= t2; e.r += t2; e.t += t2;
            if (e.Contains(px, py) && area <= bestBoxA) { bestBoxA = area; bestBox = &o; }
        }
    }
    // A line wins if the click is right on it, or if it isn't inside a text/image box.
    if (bestEdge && (!bestBox || bestEdgeD <= tolPt * 0.5f || !bestBox->bounds.Contains(px, py))) return *bestEdge;
    if (bestBox) return *bestBox;
    if (bestFill) return *bestFill;
    if (bestEdge) return *bestEdge;
    return std::nullopt;
}

// ================================================================== text

float VisualFontSize(FPDF_PAGEOBJECT text) {
    float sz = 12;
    if (!text || !FPDFTextObj_GetFontSize(text, &sz)) return 12;
    return sz * MatrixScale(ObjMatrix(text));
}

// The text a text object really contains. PDFium's text layer inserts
// "generated" spaces wherever glyphs are spaced apart; for letter-spaced or
// individually positioned text that means a space between every letter, and
// writing those back made edited text twice as wide. Keep only real
// characters, plus generated spaces that look like word gaps (not uniform
// letter spacing) and sit between real characters.
static std::wstring TextOf(Document& d, int page, FPDF_PAGEOBJECT obj) {
    FPDF_TEXTPAGE tp = d.TextPage(page);
    if (!tp || !obj) return {};
    std::vector<std::pair<unsigned, bool>> cs;      // (unicode, generated)
    int n = FPDFText_CountChars(tp);
    for (int i = 0; i < n; ++i) {
        if (FPDFText_GetTextObject(tp, i) != obj) continue;
        cs.push_back({ FPDFText_GetUnicode(tp, i), FPDFText_IsGenerated(tp, i) == 1 });
    }
    int lastReal = -1, real = 0, genGaps = 0;
    for (int k = 0; k < (int)cs.size(); ++k)
        if (!cs[k].second) { lastReal = k; ++real; }
    for (int k = 0; k < lastReal; ++k)
        if (cs[k].second && cs[k].first == L' ') ++genGaps;
    bool letterSpaced = genGaps > 0 && genGaps * 10 >= std::max(1, real - 1) * 4;
    // Dates and amounts ("12.01.2026", "1.234,56") never have word gaps next to
    // punctuation; a generated space there is just spacing in the PDF's layout.
    auto punct = [](unsigned u) {
        return u == '.' || u == ',' || u == ':' || u == ';' || u == '/' || u == '-' || u == '(' || u == ')' ||
               u == '\'' || u == '%' || u == 0x20AC;
    };
    std::wstring s;
    for (int k = 0; k < (int)cs.size(); ++k) {
        if (!cs[k].second) { s += FromUtf32(cs[k].first); continue; }
        if (letterSpaced || k >= lastReal || cs[k].first != L' ' || s.empty() || s.back() == L' ') continue;
        unsigned next = 0;
        for (int j = k + 1; j < (int)cs.size(); ++j) if (!cs[j].second) { next = cs[j].first; break; }
        if (punct((unsigned)s.back()) || punct(next)) continue;
        s += L' ';
    }
    return s;
}

// Scale (kx, ky) that makes `alt` draw `sample` in the same box as `ref` does,
// so a stand-in font can't look bigger, smaller, wider or narrower.
static void MatchFontMetrics(Document& d, FPDF_FONT ref, FPDF_FONT alt, float tf, const std::wstring& sample,
                             float& kx, float& ky) {
    kx = ky = 1.0f;
    if (!ref || !alt || ref == alt || tf <= 0) return;
    std::wstring s;
    for (wchar_t c : sample) if (!iswspace(c)) s += c;
    if (s.empty()) return;
    auto box = [&](FPDF_FONT f, PRect& r) {
        FPDF_PAGEOBJECT t = FPDFPageObj_CreateTextObj(d.Raw(), f, tf);
        if (!t) return false;
        bool ok = FPDFText_SetText(t, WS(s)) != 0;
        if (ok) r = ObjectBounds(t);
        FPDFPageObj_Destroy(t);
        return ok && r.W() > 0.05f && r.H() > 0.05f;
    };
    PRect a, b;
    if (box(ref, a) && box(alt, b)) {
        kx = std::clamp(a.W() / b.W(), 0.4f, 2.5f);
        ky = std::clamp(a.H() / b.H(), 0.4f, 2.5f);
    }
}

// Scale (kx, ky) so `alt` at `tf` with matrix `m` draws `refText` in exactly the
// box the original text really occupied on the page (`refBox`, measured before
// any change). Measuring the real box works for every font type — including
// fonts that can't re-encode text (Identity-H without a usable ToUnicode,
// common in bank statements), where re-typing the text to measure it fails.
static void CalibrateToBox(Document& d, const PRect& refBox, const std::wstring& refText, FPDF_FONT refFont,
                           FPDF_FONT alt, float tf, const FS_MATRIX& m, float& kx, float& ky) {
    kx = ky = 1.0f;
    if (!alt || tf <= 0) return;
    std::wstring sample;
    for (wchar_t c : refText) if (!iswspace(c)) sample += c;
    bool rotated = std::fabs(m.b) > 1e-4f || std::fabs(m.c) > 1e-4f;
    if (!rotated && !sample.empty() && refBox.W() > 0.3f && refBox.H() > 0.3f) {
        if (FPDF_PAGEOBJECT t = FPDFPageObj_CreateTextObj(d.Raw(), alt, tf)) {
            bool ok = FPDFText_SetText(t, WS(refText)) != 0;
            PRect r;
            if (ok) { FPDFPageObj_SetMatrix(t, &m); r = ObjectBounds(t); }
            FPDFPageObj_Destroy(t);
            if (ok && r.W() > 0.3f && r.H() > 0.3f) {
                kx = std::clamp(refBox.W() / r.W(), 0.4f, 2.5f);
                ky = std::clamp(refBox.H() / r.H(), 0.4f, 2.5f);
                return;
            }
        }
    }
    MatchFontMetrics(d, refFont, alt, tf, refText, kx, ky);   // rotated text: compare the fonts directly
}

TextProps ReadText(Document& d, int page, FPDF_PAGEOBJECT obj) {
    TextProps tp;
    if (!obj) return tp;
    if (FPDFPageObj_GetType(obj) == FPDF_PAGEOBJ_TEXT) {
        tp.isText = true;
        float sz = 12;
        if (FPDFTextObj_GetFontSize(obj, &sz)) tp.tf = sz;
        tp.fontSize = tp.tf * MatrixScale(ObjMatrix(obj));
        tp.text = TextOf(d, page, obj);
        if (FPDF_FONT f = FPDFTextObj_GetFont(obj)) {
            char nm[256] = { 0 };
            if (FPDFFont_GetBaseFontName(f, nm, sizeof(nm)) > 0) tp.fontName.assign(nm, strnlen(nm, sizeof(nm)));
            tp.embedded = FPDFFont_GetIsEmbedded(f) == 1;
        }
    }
    unsigned r = 0, g = 0, b = 0, a = 255;
    FPDFPageObj_GetFillColor(obj, &r, &g, &b, &a);
    tp.r = r; tp.g = g; tp.b = b; tp.a = a;
    return tp;
}

static std::string FontNameOf(FPDF_FONT f) {
    char nm[256] = { 0 };
    if (f && FPDFFont_GetBaseFontName(f, nm, sizeof(nm)) > 0) return std::string(nm, strnlen(nm, sizeof(nm)));
    return {};
}
static std::string BaseFontOf(FPDF_PAGEOBJECT t) { return FontNameOf(FPDFTextObj_GetFont(t)); }

// Does every visible character of `want` survive a round trip through the
// object's font? A subset font lacking a glyph maps it to nothing.
static bool CharsRoundTrip(Document& d, int page, FPDF_PAGEOBJECT o, const std::wstring& want,
                           std::wstring* missing = nullptr) {
    d.ContentChanged(page);                 // bump the page version so the text page reloads
    std::wstring got = TextOf(d, page, o);
    std::map<wchar_t, int> need;
    for (wchar_t c : want)
        if (!iswspace(c)) ++need[c];
    for (wchar_t c : got)
        if (need.count(c)) --need[c];
    bool ok = true;
    for (auto& kv : need)
        if (kv.second > 0) {
            ok = false;
            if (missing && missing->find(kv.first) == std::wstring::npos) *missing += kv.first;
        }
    return ok;
}

// Embedded program -> standard-14 by name -> matching installed font -> Helvetica.
static FPDF_FONT LoadFontFor(Document& d, const Bytes& data, const std::string& base, bool preferSystem = false) {
    if (!data.empty())
        if (FPDF_FONT f = FPDFText_LoadFont(d.Raw(), data.data(), (uint32_t)data.size(), FPDF_FONT_TRUETYPE, 1))
            return f;
    static const char* kStd14[] = {
        "Helvetica", "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique",
        "Times-Roman", "Times-Bold", "Times-Italic", "Times-BoldItalic",
        "Courier", "Courier-Bold", "Courier-Oblique", "Courier-BoldOblique", "Symbol", "ZapfDingbats",
    };
    auto system = [&]() -> FPDF_FONT {
        bool ex = false;
        const SysFont* sf = MatchSystemFont(base.empty() ? "Arial" : base, &ex);
        if (!sf) return nullptr;
        Bytes fd;
        if (!ReadFileBytes(sf->file, fd)) return nullptr;
        return FPDFText_LoadFont(d.Raw(), fd.data(), (uint32_t)fd.size(), FPDF_FONT_TRUETYPE, 1);
    };
    if (preferSystem)
        if (FPDF_FONT f = system()) return f;
    for (const char* n : kStd14)
        if (base == n)
            if (FPDF_FONT f = FPDFText_LoadStandardFont(d.Raw(), base.c_str())) return f;
    if (FPDF_FONT f = system()) return f;
    return FPDFText_LoadStandardFont(d.Raw(), "Helvetica");
}

// --------------------------------------------------- keeping the PDF's font

// The installed font closest to a PDF font: the same family if installed,
// otherwise picked by the font's own style flags (serif / monospace, weight,
// italic), so a custom font like "AllAndNone" gets a look-alike, not Helvetica.
static FPDF_FONT StandInFont(Document& d, FPDF_FONT ref, const std::string& base) {
    bool exact = false;
    MatchSystemFont(base, &exact);
    if (exact || !ref) return LoadFontFor(d, {}, base, true);
    int flags = FPDFFont_GetFlags(ref);
    int weight = FPDFFont_GetWeight(ref);
    int angle = 0;
    FPDFFont_GetItalicAngle(ref, &angle);
    std::string lname = base;
    for (auto& c : lname) c = (char)tolower((unsigned char)c);
    bool mono = flags > 0 && (flags & 1);
    bool serif = flags > 0 && (flags & 2);
    bool bold = weight >= 600 || lname.find("bold") != std::string::npos || lname.find("black") != std::string::npos;
    bool italic = angle != 0 || (flags > 0 && (flags & 64)) || lname.find("italic") != std::string::npos ||
                  lname.find("oblique") != std::string::npos;
    std::string nm = mono ? "CourierNew" : serif ? "TimesNewRoman" : "Arial";
    if (bold || italic) nm += std::string("-") + (bold ? "Bold" : "") + (italic ? "Italic" : "");
    return LoadFontFor(d, {}, nm, true);
}

// Everything that makes text look the way it does, apart from font and string.
struct TextLook {
    FS_MATRIX m{ 1, 0, 0, 1, 0, 0 };
    float tf = 12;
    unsigned fr = 0, fg = 0, fb = 0, fa = 255, sr = 0, sg = 0, sb = 0, sa = 255;
    int render = 0;
};

static TextLook LookOf(FPDF_PAGEOBJECT obj) {
    TextLook lk;
    lk.m = ObjMatrix(obj);
    FPDFTextObj_GetFontSize(obj, &lk.tf);
    FPDFPageObj_GetFillColor(obj, &lk.fr, &lk.fg, &lk.fb, &lk.fa);
    FPDFPageObj_GetStrokeColor(obj, &lk.sr, &lk.sg, &lk.sb, &lk.sa);
    lk.render = FPDFTextObj_GetTextRenderMode(obj);
    return lk;
}

static FPDF_PAGEOBJECT NewTextObj(Document& d, FPDF_FONT f, const TextLook& lk, const FS_MATRIX& m,
                                  const std::wstring& s) {
    if (!f) return nullptr;
    FPDF_PAGEOBJECT o = FPDFPageObj_CreateTextObj(d.Raw(), f, lk.tf);
    if (!o) return nullptr;
    if (!FPDFText_SetText(o, WS(s))) { FPDFPageObj_Destroy(o); return nullptr; }
    FPDFPageObj_SetMatrix(o, &m);
    FPDFPageObj_SetFillColor(o, lk.fr, lk.fg, lk.fb, lk.fa);
    FPDFPageObj_SetStrokeColor(o, lk.sr, lk.sg, lk.sb, lk.sa);
    if (lk.render >= 0) FPDFTextObj_SetTextRenderMode(o, (FPDF_TEXT_RENDERMODE)lk.render);
    return o;
}

static bool Upright(const FS_MATRIX& m) {
    return std::fabs(m.b) < 1e-4f && std::fabs(m.c) < 1e-4f && m.a > 0 && m.d > 0;
}

// Page x where the next character after this text object would start (its last
// character's origin + advance), read from the text layer.
static float AdvanceEnd(Document& d, int page, FPDF_PAGEOBJECT o) {
    d.ContentChanged(page);
    float best = -1e30f;
    if (FPDF_TEXTPAGE tp = d.TextPage(page)) {
        int n = FPDFText_CountChars(tp);
        for (int i = 0; i < n; ++i) {
            if (FPDFText_GetTextObject(tp, i) != o || FPDFText_IsGenerated(tp, i) == 1) continue;
            FS_RECTF r;
            if (FPDFText_GetLooseCharBox(tp, i, &r)) best = std::max(best, std::max(r.left, r.right));
        }
    }
    return best > -1e29f ? best : ObjectBounds(o).r;
}

// Pieces of one text share a "PEText" mark: id, and alt=1 on stand-in pieces.
static bool MarkNamed(FPDF_PAGEOBJECTMARK mark, const wchar_t* name) {
    unsigned long n = 0;
    unsigned short buf[32] = { 0 };
    if (!FPDFPageObjMark_GetName(mark, buf, sizeof(buf) - 2, &n)) return false;
    return std::wstring(reinterpret_cast<wchar_t*>(buf)) == name;
}
static int PieceId(FPDF_PAGEOBJECT obj, bool* alt = nullptr, bool* ownGroup = nullptr) {
    if (alt) *alt = false;
    if (ownGroup) *ownGroup = false;
    if (!obj) return 0;
    for (int i = 0, n = FPDFPageObj_CountMarks(obj); i < n; ++i) {
        FPDF_PAGEOBJECTMARK m = FPDFPageObj_GetMark(obj, (unsigned long)i);
        if (!m || !MarkNamed(m, L"PEText")) continue;
        int v = 0, a = 0, g = 0;
        if (!FPDFPageObjMark_GetParamIntValue(m, "id", &v)) return 0;
        if (alt && FPDFPageObjMark_GetParamIntValue(m, "alt", &a)) *alt = a != 0;
        if (ownGroup && FPDFPageObjMark_GetParamIntValue(m, "grp", &g)) *ownGroup = g != 0;
        return v;
    }
    return 0;
}
static void ClearPieceMark(FPDF_PAGEOBJECT obj) {
    for (int i = FPDFPageObj_CountMarks(obj) - 1; i >= 0; --i) {
        FPDF_PAGEOBJECTMARK m = FPDFPageObj_GetMark(obj, (unsigned long)i);
        if (m && MarkNamed(m, L"PEText")) FPDFPageObj_RemoveMark(obj, m);
    }
}
static void SetPieceMark(Document& d, FPDF_PAGEOBJECT obj, int id, bool alt, bool ownGroup) {
    ClearPieceMark(obj);
    FPDF_PAGEOBJECTMARK m = FPDFPageObj_AddMark(obj, "PEText");
    if (!m) return;
    FPDFPageObjMark_SetIntParam(d.Raw(), obj, m, "id", id);
    FPDFPageObjMark_SetIntParam(d.Raw(), obj, m, "alt", alt ? 1 : 0);
    FPDFPageObjMark_SetIntParam(d.Raw(), obj, m, "grp", ownGroup ? 1 : 0);
}
static int MaxPieceId(Document& d) {
    int mx = 0;
    for (int pg = 0; pg < d.PageCount(); ++pg)
        if (FPDF_PAGE p = d.Page(pg))
            for (int i = 0, n = FPDFPage_CountObjects(p); i < n; ++i) mx = std::max(mx, PieceId(FPDFPage_GetObject(p, i)));
    return mx;
}

// Many PDFs store a separate copy ("subset") of the same font per page, each
// with only the letters that page uses. Try the other copies for the letters
// this one lacks. Returns the new object (inserted after `after`) or null.
static FPDF_PAGEOBJECT TryOtherCopies(Document& d, int page, FPDF_PAGEOBJECT after, FPDF_FONT current,
                                      const std::string& base, const TextLook& lk, const std::wstring& text) {
    std::string fam = StripSubsetPrefix(base);
    if (fam.empty()) return nullptr;
    FPDF_PAGE p = d.Page(page);
    if (!p) return nullptr;
    for (const auto& df : ListDocFonts(d, d.PageCount())) {
        if (df.name == base || StripSubsetPrefix(df.name) != fam) continue;
        FPDF_FONT f = FindDocFont(d, df.name, page);
        if (!f || f == current) continue;
        FPDF_PAGEOBJECT o = NewTextObj(d, f, lk, lk.m, text);
        if (!o) continue;
        int idx = TopIndex(p, after);
        InsertAt(p, o, idx >= 0 ? idx + 1 : -1);
        if (CharsRoundTrip(d, page, o, text)) return o;
        if (FPDFPage_RemoveObject(p, o)) FPDFPageObj_Destroy(o);
    }
    d.ContentChanged(page);
    return nullptr;
}

// Draws `text` from lk.m's origin in pieces: `font` (the PDF's own) for every
// character it has, `alt` scaled by (kx, ky) only for the ones in `missing`.
// `reuse` (on the page, in `font`) becomes the first own-font piece so it keeps
// its spacing, clip and tags; if no piece needs it, it is removed. Pieces are
// tagged as one text and grouped so they move and edit together.
static std::vector<FPDF_PAGEOBJECT> PlacePieces(Document& d, int page, FPDF_PAGEOBJECT reuse, FPDF_FONT font,
                                                FPDF_FONT alt, float kx, float ky, const TextLook& lk,
                                                const std::wstring& text, const std::wstring& missing, int groupId) {
    struct Run { std::wstring s; bool alt; };
    std::vector<Run> runs;
    for (wchar_t c : text) {
        bool a = iswspace(c) ? (!runs.empty() && runs.back().alt) : missing.find(c) != std::wstring::npos;
        if (runs.empty() || runs.back().alt != a) runs.push_back({ {}, a });
        runs.back().s += c;
    }
    std::vector<FPDF_PAGEOBJECT> out;
    FPDF_PAGE p = d.Page(page);
    if (!p) return out;
    FPDF_PAGEOBJECT anchor = reuse;
    float x = lk.m.e;
    std::vector<bool> isAlt;
    for (size_t i = 0; i < runs.size(); ++i) {
        const Run& r = runs[i];
        FPDF_PAGEOBJECT o = nullptr;
        if (!r.alt && reuse) {
            o = reuse;
            reuse = nullptr;
            FPDFText_SetText(o, WS(r.s));
            FS_MATRIX cur = ObjMatrix(o);
            Xform(o, 1, 0, 0, 1, x - cur.e, lk.m.f - cur.f);   // moves its clip along
        } else {
            FS_MATRIX mm = r.alt ? MatMul(FS_MATRIX{ kx, 0, 0, ky, 0, 0 }, lk.m) : lk.m;
            mm.e = x;
            mm.f = lk.m.f;
            o = NewTextObj(d, r.alt ? alt : font, lk, mm, r.s);
            if (!o) continue;
            int idx = anchor ? TopIndex(p, anchor) : -1;
            InsertAt(p, o, idx >= 0 ? idx + 1 : -1);
        }
        anchor = o;
        out.push_back(o);
        isAlt.push_back(r.alt);
        if (i + 1 < runs.size()) x = AdvanceEnd(d, page, o);
    }
    if (reuse && FPDFPage_RemoveObject(p, reuse)) FPDFPageObj_Destroy(reuse);
    if (out.size() > 1) {
        bool own = groupId == 0;
        int gid = own ? MaxGroupId(d) + 1 : groupId;
        int pid = MaxPieceId(d) + 1;
        for (size_t i = 0; i < out.size(); ++i) {
            SetGroupId(d, page, out[i], gid);
            SetPieceMark(d, out[i], pid, isAlt[i], own);
        }
    }
    d.ContentChanged(page);
    return out;
}

std::vector<FPDF_PAGEOBJECT> TextPieces(Document& d, int page, FPDF_PAGEOBJECT obj) {
    std::vector<FPDF_PAGEOBJECT> v;
    int id = PieceId(obj);
    FPDF_PAGE p = d.Page(page);
    if (!id || !p) return v;
    for (int i = 0, n = FPDFPage_CountObjects(p); i < n; ++i) {
        FPDF_PAGEOBJECT o = FPDFPage_GetObject(p, i);
        if (o && FPDFPageObj_GetType(o) == FPDF_PAGEOBJ_TEXT && PieceId(o) == id) v.push_back(o);
    }
    std::stable_sort(v.begin(), v.end(), [](FPDF_PAGEOBJECT a, FPDF_PAGEOBJECT b) { return ObjMatrix(a).e < ObjMatrix(b).e; });
    if (v.size() < 2) v.clear();
    return v;
}

FPDF_PAGEOBJECT JoinTextPieces(Document& d, int page, const std::vector<FPDF_PAGEOBJECT>& pieces) {
    FPDF_PAGE p = d.Page(page);
    if (!p || pieces.empty()) return nullptr;
    FPDF_PAGEOBJECT base = pieces[0];
    for (FPDF_PAGEOBJECT o : pieces) {
        bool alt = true;
        PieceId(o, &alt);
        if (!alt) { base = o; break; }
    }
    bool ownGroup = false;
    PieceId(base, nullptr, &ownGroup);
    FS_MATRIX start = ObjMatrix(pieces[0]), cur = ObjMatrix(base);
    Xform(base, 1, 0, 0, 1, start.e - cur.e, start.f - cur.f);
    for (FPDF_PAGEOBJECT o : pieces)
        if (o != base && FPDFPage_RemoveObject(p, o)) FPDFPageObj_Destroy(o);
    ClearPieceMark(base);
    if (ownGroup) ClearGroupId(d, page, base);
    d.ContentChanged(page);
    return base;
}

bool SetObjectText(Document& d, int page, FPDF_PAGEOBJECT obj, const std::wstring& text) {
    if (!obj || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_TEXT || text.empty()) return false;
    if (!FPDFText_SetText(obj, WS(text))) return false;
    d.ContentChanged(page);
    return true;
}

bool SetObjectTextSafe(Document& d, int page, FPDF_PAGEOBJECT obj, const std::wstring& text,
                       FPDF_PAGEOBJECT* replaced, bool* usedFallback, TextEditInfo* info) {
    TextEditInfo local;
    TextEditInfo& ti = info ? *info : local;
    ti = TextEditInfo{};
    if (replaced) *replaced = obj;
    if (usedFallback) *usedFallback = false;
    if (!obj || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_TEXT || text.empty()) return false;
    FPDF_PAGE p = d.Page(page);
    if (!p) return false;
    auto done = [&](std::vector<FPDF_PAGEOBJECT> pieces) {
        if (replaced) *replaced = pieces.empty() ? nullptr : pieces[0];
        ti.pieces = std::move(pieces);
        ti.groupId = ti.pieces.size() > 1 ? GetGroupId(ti.pieces[0]) : 0;
        d.ContentChanged(page);
        return !ti.pieces.empty();
    };

    // Remember the exact look before touching anything.
    TextLook lk = LookOf(obj);
    std::string base = BaseFontOf(obj);
    ti.fontName = StripSubsetPrefix(base);
    int gid = GetGroupId(obj);
    FPDF_FONT refFont = FPDFTextObj_GetFont(obj);
    std::wstring orig = TextOf(d, page, obj);
    PRect origBox = ObjectBounds(obj);                  // the real box, before anything changes
    bool verifiable = !orig.empty();                    // the text layer can see this object

    // 1. The object's own font has every character: done, nothing else changes.
    if (FPDFText_SetText(obj, WS(text)) && (!verifiable || CharsRoundTrip(d, page, obj, text, &ti.missing)))
        return done({ obj });
    if (ti.missing.empty())                             // couldn't tell which: treat all as missing
        for (wchar_t c : text) if (!iswspace(c) && ti.missing.find(c) == std::wstring::npos) ti.missing += c;

    // 2. Another copy of the same font in this PDF has them all: still the same font.
    if (FPDF_PAGEOBJECT o = TryOtherCopies(d, page, obj, refFont, base, lk, text)) {
        if (gid) SetGroupId(d, page, o, gid);
        if (FPDFPage_RemoveObject(p, obj)) FPDFPageObj_Destroy(obj);
        ti.otherCopy = true;
        ti.missing.clear();
        return done({ o });
    }

    // 3. Only the missing characters come from the closest installed font, scaled
    //    so the original text would fill exactly its box -> same size and width.
    FPDF_FONT alt = StandInFont(d, refFont, base);
    if (!alt) return false;
    float kx = 1, ky = 1;
    CalibrateToBox(d, origBox, orig, refFont, alt, lk.tf, lk.m, kx, ky);
    ti.standIn = StripSubsetPrefix(FontNameOf(alt));
    if (usedFallback) *usedFallback = true;
    if (Upright(lk.m))
        return done(PlacePieces(d, page, obj, refFont, alt, kx, ky, lk, text, ti.missing, gid));

    // Rotated or mirrored text: the whole string in the stand-in font.
    FPDF_PAGEOBJECT repl = NewTextObj(d, alt, lk, MatMul(FS_MATRIX{ kx, 0, 0, ky, 0, 0 }, lk.m), text);
    if (!repl) return false;
    if (gid) SetGroupId(d, page, repl, gid);
    int idx = TopIndex(p, obj);
    InsertAt(p, repl, idx >= 0 ? idx + 1 : -1);
    if (FPDFPage_RemoveObject(p, obj)) FPDFPageObj_Destroy(obj);
    return done({ repl });
}

bool SetTextVisualSize(Document& d, int page, FPDF_PAGEOBJECT obj, float visualSize) {
    if (!obj || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_TEXT || visualSize <= 0) return false;
    float cur = VisualFontSize(obj);
    if (cur <= 0) return false;
    float k = visualSize / cur;
    if (std::fabs(k - 1.0f) < 1e-4f) return true;
    FS_MATRIX m = ObjMatrix(obj);
    Xform(obj, MatScaleAbout(k, k, m.e, m.f));   // about the text's start point: font untouched
    d.ContentChanged(page);
    return true;
}

std::string TextFontName(FPDF_PAGEOBJECT obj) { return StripSubsetPrefix(BaseFontOf(obj)); }

std::vector<DocFont> ListDocFonts(Document& d, int maxPages) {
    std::vector<DocFont> out;
    std::map<std::string, size_t> at;
    int n = std::min(d.PageCount(), std::max(1, maxPages));
    for (int pg = 0; pg < n; ++pg) {
        for (auto& o : ListObjects(d, pg)) {
            if (o.type != FPDF_PAGEOBJ_TEXT) continue;
            FPDF_FONT f = FPDFTextObj_GetFont(o.obj);
            if (!f) continue;
            std::string name = BaseFontOf(o.obj);
            if (name.empty()) continue;
            auto it = at.find(name);
            if (it == at.end()) {
                DocFont df;
                df.name = name;
                df.embedded = FPDFFont_GetIsEmbedded(f) == 1;
                df.subset = StripSubsetPrefix(name) != name;
                at[name] = out.size();
                out.push_back(df);
                it = at.find(name);
            }
            out[it->second].uses++;
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const DocFont& a, const DocFont& b) { return a.uses > b.uses; });
    return out;
}

FPDF_FONT FindDocFont(Document& d, const std::string& name, int preferPage, FPDF_PAGEOBJECT* sample, int* samplePage) {
    if (name.empty() || d.PageCount() == 0) return nullptr;
    int n = d.PageCount();
    int first = std::clamp(preferPage, 0, n - 1);
    std::vector<int> order{ first };
    for (int i = 0; i < n; ++i) if (i != first) order.push_back(i);
    for (int pg : order) {
        for (auto& o : ListObjects(d, pg)) {
            if (o.type != FPDF_PAGEOBJ_TEXT || BaseFontOf(o.obj) != name) continue;
            if (sample) *sample = o.obj;
            if (samplePage) *samplePage = pg;
            return FPDFTextObj_GetFont(o.obj);
        }
    }
    return nullptr;
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

FPDF_PAGEOBJECT AddTextLike(Document& d, int page, const std::wstring& text, FPDF_PAGEOBJECT styleFrom,
                            const Bytes& fontData, const std::string& fontBase, double visualSize,
                            float px, float py, unsigned r, unsigned g, unsigned b, std::string& err,
                            TextEditInfo* info) {
    TextEditInfo local;
    TextEditInfo& ti = info ? *info : local;
    ti = TextEditInfo{};
    FPDF_PAGE p = d.Page(page);
    if (!p) { err = "page not loaded"; return nullptr; }
    if (text.empty()) { err = "empty text"; return nullptr; }
    auto make = [&](FPDF_FONT font, float kx, float ky) -> FPDF_PAGEOBJECT {
        if (!font) return nullptr;
        FPDF_PAGEOBJECT o = FPDFPageObj_CreateTextObj(d.Raw(), font, (float)visualSize);
        if (!o) return nullptr;
        if (!FPDFText_SetText(o, WS(text))) { FPDFPageObj_Destroy(o); return nullptr; }
        FPDFPageObj_SetFillColor(o, r, g, b, 255);
        FPDFPageObj_Transform(o, kx, 0, 0, ky, px, py);
        FPDFPage_InsertObject(p, o);
        return o;
    };
    bool live = styleFrom && FPDFPageObj_GetType(styleFrom) == FPDF_PAGEOBJ_TEXT;
    FPDF_FONT f = live ? FPDFTextObj_GetFont(styleFrom) : nullptr;
    if (!f) f = LoadFontFor(d, fontData, fontBase);
    // Copy the neighbour's horizontal/vertical squeeze too (condensed text).
    float ax = 1, ay = 1, stf = (float)visualSize;
    FS_MATRIX sm{ 1, 0, 0, 1, 0, 0 };
    if (live) {
        sm = ObjMatrix(styleFrom);
        FPDFTextObj_GetFontSize(styleFrom, &stf);
        if (std::fabs(sm.b) < 1e-4f && std::fabs(sm.c) < 1e-4f) {
            float s = MatrixScale(sm);
            ax = std::fabs(sm.a) / s;
            ay = std::fabs(sm.d) / s;
        }
    }
    FPDF_PAGEOBJECT o = make(f, ax, ay);
    std::string base = live ? BaseFontOf(styleFrom) : FontNameOf(f);
    ti.fontName = StripSubsetPrefix(base);
    // The neighbour's font is often a subset lacking a typed letter: try the PDF's
    // other copies of that font, else draw only the missing letters in the
    // closest installed font, scaled to the neighbour's real glyph box.
    if (o && !CharsRoundTrip(d, page, o, text, &ti.missing)) {
        if (ti.missing.empty())
            for (wchar_t c : text) if (!iswspace(c) && ti.missing.find(c) == std::wstring::npos) ti.missing += c;
        TextLook lk;
        lk.m = FS_MATRIX{ ax, 0, 0, ay, px, py };
        lk.tf = (float)visualSize;
        lk.fr = r; lk.fg = g; lk.fb = b;
        lk.sr = r; lk.sg = g; lk.sb = b;
        if (FPDF_PAGEOBJECT c = TryOtherCopies(d, page, o, f, base, lk, text)) {
            if (FPDFPage_RemoveObject(p, o)) FPDFPageObj_Destroy(o);
            ti.otherCopy = true;
            ti.missing.clear();
            ti.pieces = { c };
            d.ContentChanged(page);
            return c;
        }
        FPDF_FONT alt = StandInFont(d, f, base);
        float kx = 1, ky = 1;
        if (live) CalibrateToBox(d, ObjectBounds(styleFrom), TextOf(d, page, styleFrom), f, alt, stf, sm, kx, ky);
        else MatchFontMetrics(d, f, alt, (float)visualSize, text, kx, ky);
        ti.standIn = StripSubsetPrefix(FontNameOf(alt));
        ti.pieces = PlacePieces(d, page, o, f, alt, kx, ky, lk, text, ti.missing, 0);
        ti.groupId = ti.pieces.size() > 1 ? GetGroupId(ti.pieces[0]) : 0;
        o = ti.pieces.empty() ? nullptr : ti.pieces[0];
    } else if (o) {
        ti.pieces = { o };
    }
    if (!o) { err = "could not create the text"; return nullptr; }
    d.ContentChanged(page);
    return o;
}

bool RestyleText(Document& d, int page, FPDF_PAGEOBJECT obj, const FontChoice& fc, double visualSize,
                 std::string& err, FPDF_PAGEOBJECT* out) {
    FPDF_PAGE p = d.Page(page);
    if (!p || !obj || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_TEXT) { err = "not a text object"; return false; }
    TextProps old = ReadText(d, page, obj);
    if (old.text.empty()) { err = "this text can't be read back (no text layer)"; return false; }
    FS_MATRIX m = ObjMatrix(obj);
    float s = MatrixScale(m);
    FS_MATRIX dir{ m.a / s, m.b / s, m.c / s, m.d / s, m.e, m.f };   // keep rotation + position, unit scale
    FPDF_FONT font = d.GetFont(fc, err);
    if (!font) return false;
    FPDF_PAGEOBJECT repl = FPDFPageObj_CreateTextObj(d.Raw(), font, (float)visualSize);
    if (!repl) { err = "could not create text object"; return false; }
    if (!FPDFText_SetText(repl, WS(old.text))) {
        FPDFPageObj_Destroy(repl);
        err = "that font lacks some of these characters";
        return false;
    }
    FPDFPageObj_SetFillColor(repl, old.r, old.g, old.b, old.a ? old.a : 255);
    int render = FPDFTextObj_GetTextRenderMode(obj);
    if (render >= 0) FPDFTextObj_SetTextRenderMode(repl, (FPDF_TEXT_RENDERMODE)render);
    FPDFPageObj_SetMatrix(repl, &dir);
    if (int gid = GetGroupId(obj)) SetGroupId(d, page, repl, gid);
    int idx = TopIndex(p, obj);
    InsertAt(p, repl, idx >= 0 ? idx + 1 : -1);
    if (FPDFPage_RemoveObject(p, obj)) FPDFPageObj_Destroy(obj);
    if (out) *out = repl;
    d.ContentChanged(page);
    return true;
}

// ============================================================== geometry

void TranslateObjectPage(Document& d, int page, FPDF_PAGEOBJECT obj, const FS_MATRIX& accum,
                         float dxPage, float dyPage) {
    if (!obj || (dxPage == 0 && dyPage == 0)) return;
    FS_MATRIX inv;
    float lx = dxPage, ly = dyPage;
    if (MatInvert(accum, inv)) {           // page delta -> container delta (linear part only)
        lx = inv.a * dxPage + inv.c * dyPage;
        ly = inv.b * dxPage + inv.d * dyPage;
    }
    Xform(obj, 1, 0, 0, 1, lx, ly);
    d.ContentChanged(page);
}

void SetObjectPosPage(Document& d, int page, FPDF_PAGEOBJECT obj, const FS_MATRIX& accum,
                      float leftPage, float bottomPage) {
    if (!obj) return;
    PRect b = ObjectPageBounds(obj, accum);
    TranslateObjectPage(d, page, obj, accum, leftPage - b.l, bottomPage - b.b);
}

void ResizeObjectPageTopLevel(Document& d, int page, FPDF_PAGEOBJECT obj, PRect target) {
    if (!obj) return;
    PRect c = ObjectBounds(obj);
    float sx = (c.W() > 0.5f && target.W() > 0) ? target.W() / c.W() : 1.0f;
    float sy = (c.H() > 0.5f && target.H() > 0) ? target.H() / c.H() : 1.0f;
    Xform(obj, sx, 0, 0, sy, target.l - c.l * sx, target.b - c.b * sy);
    d.ContentChanged(page);
}

void TransformObjectPage(Document& d, int page, FPDF_PAGEOBJECT obj, const FS_MATRIX& m) {
    if (!obj) return;
    Xform(obj, m);
    d.ContentChanged(page);
}

bool DeleteObjectNested(Document& d, int page, FPDF_PAGEOBJECT obj, FPDF_PAGEOBJECT parent) {
    FPDF_PAGE p = d.Page(page);
    if (!p || !obj) return false;
    bool ok = parent ? (FPDFFormObj_RemoveObject(parent, obj) != 0) : (FPDFPage_RemoveObject(p, obj) != 0);
    if (!ok) return false;
    FPDFPageObj_Destroy(obj);
    d.ContentChanged(page);
    return true;
}

// Lifts every child of `form` onto `p` right where the form sat in the stacking
// order, baking the form's matrix into each child and its clip, then destroys
// the form. Recurses so nested groups flatten completely.
static void ExplodeFormToPage(FPDF_PAGE p, FPDF_PAGEOBJECT form) {
    FS_MATRIX mf = ObjMatrix(form);
    std::vector<FPDF_PAGEOBJECT> kids;
    int n = FPDFFormObj_CountObjects(form);
    for (int i = 0; i < n; ++i)
        if (FPDF_PAGEOBJECT c = FPDFFormObj_GetObject(form, (unsigned long)i)) kids.push_back(c);
    int at = TopIndex(p, form);
    int k = 0;
    std::vector<FPDF_PAGEOBJECT> subForms;
    for (FPDF_PAGEOBJECT c : kids) {
        if (!FPDFFormObj_RemoveObject(form, c)) continue;
        Xform(c, mf);                                     // form space -> page space
        InsertAt(p, c, at >= 0 ? at + 1 + k : -1);
        ++k;
        if (FPDFPageObj_GetType(c) == FPDF_PAGEOBJ_FORM) subForms.push_back(c);
    }
    FPDFPage_RemoveObject(p, form);
    FPDFPageObj_Destroy(form);
    for (FPDF_PAGEOBJECT f : subForms) ExplodeFormToPage(p, f);
}

bool PromoteObject(Document& d, int page, FPDF_PAGEOBJECT target) {
    FPDF_PAGE p = d.Page(page);
    if (!p || !target) return false;
    int n = FPDFPage_CountObjects(p);
    for (int i = 0; i < n; ++i) {
        FPDF_PAGEOBJECT o = FPDFPage_GetObject(p, i);
        if (o == target) return true;
        if (o && FPDFPageObj_GetType(o) == FPDF_PAGEOBJ_FORM && SubtreeContains(o, target)) {
            ExplodeFormToPage(p, o);
            d.ContentChanged(page);
            return true;
        }
    }
    return true;
}

void PromoteAll(Document& d, int page, const std::vector<FPDF_PAGEOBJECT>& objs) {
    for (FPDF_PAGEOBJECT o : objs) PromoteObject(d, page, o);
}

bool ExplodeForm(Document& d, int page, FPDF_PAGEOBJECT form) {
    FPDF_PAGE p = d.Page(page);
    if (!p || !form || FPDFPageObj_GetType(form) != FPDF_PAGEOBJ_FORM) return false;
    ExplodeFormToPage(p, form);
    d.ContentChanged(page);
    return true;
}

bool MoveObjectToPage(Document& d, int srcPage, FPDF_PAGEOBJECT obj, int dstPage, float dx, float dy) {
    FPDF_PAGE ps = d.Page(srcPage), pd = d.Page(dstPage);
    if (!ps || !pd || !obj || srcPage == dstPage) return false;
    if (!FPDFPage_RemoveObject(ps, obj)) return false;
    if (dx != 0 || dy != 0) Xform(obj, 1, 0, 0, 1, dx, dy);
    if (!FPDFPage_InsertObject(pd, obj)) {
        FPDFPage_InsertObject(ps, obj);        // put it back rather than lose it
        return false;
    }
    d.ContentChanged(srcPage);
    d.ContentChanged(dstPage);
    return true;
}

// ====================================================== splitting shared paths

int SubpathCount(FPDF_PAGEOBJECT obj) {
    if (!obj || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_PATH) return 0;
    return (int)ParsePath(obj).size();
}

static int EdgeCount(const PSub& sp) {
    int n = (int)sp.ops.size();
    if (sp.closed && !sp.ops.empty()) {
        PPt last = sp.ops.back().end;
        if (std::fabs(last.x - sp.start.x) > 1e-3f || std::fabs(last.y - sp.start.y) > 1e-3f) ++n;
    }
    return n;
}

int SplittableParts(FPDF_PAGEOBJECT obj) {
    if (!obj || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_PATH) return 0;
    auto subs = ParsePath(obj);
    if (subs.size() >= 2) return (int)subs.size();
    if (subs.size() == 1) {
        int fm = 0; FPDF_BOOL st = 0;
        FPDFPath_GetDrawMode(obj, &fm, &st);
        int e = EdgeCount(subs[0]);
        if (st && e >= 2) return e;
    }
    return 0;
}

std::vector<FPDF_PAGEOBJECT> SplitPath(Document& d, int page, FPDF_PAGEOBJECT obj) {
    std::vector<FPDF_PAGEOBJECT> out;
    FPDF_PAGE p = d.Page(page);
    if (!p || !obj || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_PATH) return out;
    auto subs = ParsePath(obj);
    PathStyle st = ReadPathStyle(obj);
    bool fill = st.fillMode != FPDF_FILLMODE_NONE;
    std::vector<FPDF_PAGEOBJECT> pieces;    // in stacking order
    FPDF_PAGEOBJECT fillPiece = nullptr;

    if (subs.size() >= 2) {
        for (const PSub& sp : subs) {
            FPDF_PAGEOBJECT o = BuildPath({ sp });
            if (!o) continue;
            ApplyPathStyle(d, page, o, st, fill, st.stroke);
            pieces.push_back(o);
            out.push_back(o);
        }
    } else if (subs.size() == 1 && st.stroke && EdgeCount(subs[0]) >= 2) {
        const PSub& sp = subs[0];
        if (fill) {                          // keep the fill as its own shape underneath
            fillPiece = BuildPath(subs);
            if (fillPiece) { ApplyPathStyle(d, page, fillPiece, st, true, false); pieces.push_back(fillPiece); }
        }
        PPt cur = sp.start;
        for (const POp& op : sp.ops) {
            PSub e;
            e.start = cur;
            e.ops.push_back(op);
            if (FPDF_PAGEOBJECT o = BuildPath({ e })) {
                ApplyPathStyle(d, page, o, st, false, true);
                pieces.push_back(o);
                out.push_back(o);
            }
            cur = op.end;
        }
        if (EdgeCount(sp) > (int)sp.ops.size()) {      // the implicit closing side
            PSub e;
            e.start = cur;
            POp op; op.end = sp.start;
            e.ops.push_back(op);
            if (FPDF_PAGEOBJECT o = BuildPath({ e })) {
                ApplyPathStyle(d, page, o, st, false, true);
                pieces.push_back(o);
                out.push_back(o);
            }
        }
    } else {
        return out;                          // a single filled shape: nothing to split
    }

    int idx = TopIndex(p, obj);
    for (size_t k = 0; k < pieces.size(); ++k) InsertAt(p, pieces[k], idx >= 0 ? idx + 1 + (int)k : -1);
    if (FPDFPage_RemoveObject(p, obj)) FPDFPageObj_Destroy(obj);
    d.ContentChanged(page);
    return out;
}

// ================================================================= images

FPDF_PAGEOBJECT AddImage(Document& d, int page, const Image& img, float px, float py, float widthPt,
                         std::string& err) {
    FPDF_PAGE p = d.Page(page);
    if (!p) { err = "page not loaded"; return nullptr; }
    if (img.w <= 0 || img.h <= 0 || img.px.empty()) { err = "empty image"; return nullptr; }
    FPDF_PAGEOBJECT obj = FPDFPageObj_NewImageObj(d.Raw());
    if (!obj) { err = "could not create image object"; return nullptr; }
    if (!SetImagePixels(nullptr, 0, obj, img)) { FPDFPageObj_Destroy(obj); err = "could not attach image"; return nullptr; }
    if (widthPt <= 0) widthPt = std::min(400.0f, (float)img.w * 0.75f);
    float heightPt = widthPt * (float)img.h / (float)img.w;
    FS_MATRIX m{ widthPt, 0, 0, heightPt, px, py };
    FPDFPageObj_SetMatrix(obj, &m);
    FPDFPage_InsertObject(p, obj);
    d.ContentChanged(page);
    return obj;
}

FPDF_PAGEOBJECT CutImageRegion(Document& d, int page, FPDF_PAGEOBJECT img, PRect box, std::string& err) {
    FPDF_PAGE p = d.Page(page);
    if (!p || !img || FPDFPageObj_GetType(img) != FPDF_PAGEOBJ_IMAGE) { err = "not a picture"; return nullptr; }
    Image full;
    if (!ImagePixels(img, full)) { err = "could not read the picture's pixels"; return nullptr; }
    FS_MATRIX M = ObjMatrix(img), inv;
    if (!MatInvert(M, inv)) { err = "the picture has no size"; return nullptr; }

    // box (page) -> image unit square -> pixel rectangle (row 0 is the top)
    float us[4], vs[4];
    MatApply(inv, box.l, box.b, us[0], vs[0]);
    MatApply(inv, box.r, box.b, us[1], vs[1]);
    MatApply(inv, box.r, box.t, us[2], vs[2]);
    MatApply(inv, box.l, box.t, us[3], vs[3]);
    float u0 = std::clamp(*std::min_element(us, us + 4), 0.0f, 1.0f), u1 = std::clamp(*std::max_element(us, us + 4), 0.0f, 1.0f);
    float v0 = std::clamp(*std::min_element(vs, vs + 4), 0.0f, 1.0f), v1 = std::clamp(*std::max_element(vs, vs + 4), 0.0f, 1.0f);
    int w = full.w, h = full.h;
    int x0 = std::clamp((int)std::floor(u0 * w), 0, w), x1 = std::clamp((int)std::ceil(u1 * w), 0, w);
    int y0 = std::clamp((int)std::floor((1 - v1) * h), 0, h), y1 = std::clamp((int)std::ceil((1 - v0) * h), 0, h);
    if (x1 - x0 < 1 || y1 - y0 < 1) { err = "the box doesn't cover any of the picture"; return nullptr; }

    Image piece;
    piece.w = x1 - x0; piece.h = y1 - y0;
    piece.px.resize((size_t)piece.w * piece.h);
    for (int y = 0; y < piece.h; ++y)
        memcpy(&piece.px[(size_t)y * piece.w], &full.px[(size_t)(y0 + y) * w + x0], (size_t)piece.w * 4);

    // Background = the most common colour on a ring just outside the cut.
    struct Bin { int n = 0; uint64_t s[3] = { 0, 0, 0 }; };
    std::map<uint32_t, Bin> hist;
    auto sample = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        uint32_t px = full.px[(size_t)y * w + x];
        Bin& e = hist[(px >> 3) & 0x1F1F1F];
        e.n++;
        e.s[0] += px & 255; e.s[1] += (px >> 8) & 255; e.s[2] += (px >> 16) & 255;
    };
    for (int x = x0 - 1; x <= x1; ++x) { sample(x, y0 - 1); sample(x, y1); }
    for (int y = y0; y < y1; ++y) { sample(x0 - 1, y); sample(x1, y); }
    uint32_t bg = 0xFFFFFFFFu;
    int bestN = 0;
    for (auto& kv : hist)
        if (kv.second.n > bestN) {
            bestN = kv.second.n;
            uint64_t n = (uint64_t)bestN;
            bg = 0xFF000000u | (uint32_t)((kv.second.s[2] / n) << 16) |
                 (uint32_t)((kv.second.s[1] / n) << 8) | (uint32_t)(kv.second.s[0] / n);
        }
    for (int y = y0; y < y1; ++y)
        std::fill(&full.px[(size_t)y * w + x0], &full.px[(size_t)y * w + x1], bg);

    FPDF_PAGE pages[1] = { p };
    if (!SetImagePixels(pages, 1, img, full)) { err = "could not update the picture"; return nullptr; }

    FPDF_PAGEOBJECT po = FPDFPageObj_NewImageObj(d.Raw());
    if (!po || !SetImagePixels(nullptr, 0, po, piece)) {
        if (po) FPDFPageObj_Destroy(po);
        err = "could not create the piece";
        return nullptr;
    }
    float pu0 = (float)x0 / w, pu1 = (float)x1 / w;
    float pv0 = 1.0f - (float)y1 / h, pv1 = 1.0f - (float)y0 / h;
    FS_MATRIX S{ pu1 - pu0, 0, 0, pv1 - pv0, pu0, pv0 };   // piece unit square -> part of the image
    FS_MATRIX pm = MatMul(S, M);
    FPDFPageObj_SetMatrix(po, &pm);
    int idx = TopIndex(p, img);
    InsertAt(p, po, idx >= 0 ? idx + 1 : -1);
    d.ContentChanged(page);
    return po;
}

// ================================================================= shapes

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

FPDF_PAGEOBJECT AddLine(Document& d, int page, float x1, float y1, float x2, float y2,
                        unsigned r, unsigned g, unsigned b, float width) {
    FPDF_PAGE p = d.Page(page);
    if (!p) return nullptr;
    FPDF_PAGEOBJECT ln = FPDFPageObj_CreateNewPath(x1, y1);
    if (!ln) return nullptr;
    FPDFPath_LineTo(ln, x2, y2);
    FPDFPageObj_SetStrokeColor(ln, r, g, b, 255);
    FPDFPageObj_SetStrokeWidth(ln, width <= 0 ? 1.0f : width);
    FPDFPath_SetDrawMode(ln, FPDF_FILLMODE_NONE, 1);
    FPDFPage_InsertObject(p, ln);
    d.ContentChanged(page);
    return ln;
}

int AddTable(Document& d, int page, PRect box, int cols, int rows, bool header, float headerH,
             unsigned r, unsigned g, unsigned b, float width) {
    FPDF_PAGE p = d.Page(page);
    if (!p || box.W() <= 1 || box.H() <= 1) return 0;
    cols = std::max(1, std::min(cols, 100));
    rows = std::max(1, std::min(rows, 200));
    int count = 0;
    for (int i = 0; i <= cols; ++i) {
        float x = box.l + box.W() * i / cols;
        if (AddLine(d, page, x, box.b, x, box.t, r, g, b, width)) ++count;
    }
    for (int j = 0; j <= rows; ++j) {
        float y = box.b + box.H() * j / rows;
        if (AddLine(d, page, box.l, y, box.r, y, r, g, b, width)) ++count;
    }
    if (header && headerH > 0 && headerH < box.H()) {
        float y = box.t - headerH;
        if (AddLine(d, page, box.l, y, box.r, y, r, g, b, width)) ++count;
    }
    d.ContentChanged(page);
    return count;
}

StrokeProps ReadStroke(FPDF_PAGEOBJECT obj) {
    StrokeProps s;
    if (!obj) return s;
    s.isPath = FPDFPageObj_GetType(obj) == FPDF_PAGEOBJ_PATH;
    FPDFPageObj_GetStrokeWidth(obj, &s.width);
    unsigned r = 0, g = 0, b = 0, a = 255;
    if (FPDFPageObj_GetStrokeColor(obj, &r, &g, &b, &a)) { s.r = r; s.g = g; s.b = b; s.a = a; }
    return s;
}

void SetObjectStroke(Document& d, int page, FPDF_PAGEOBJECT obj, unsigned r, unsigned g, unsigned b, float width) {
    if (!obj) return;
    FPDFPageObj_SetStrokeColor(obj, r, g, b, 255);
    if (width > 0) FPDFPageObj_SetStrokeWidth(obj, width);
    d.ContentChanged(page);
}

float LineThickness(FPDF_PAGEOBJECT obj) {
    if (!obj || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_PATH) return 0;
    int fm = 0; FPDF_BOOL st = 0;
    FPDFPath_GetDrawMode(obj, &fm, &st);
    if (st && fm == FPDF_FILLMODE_NONE) {
        float sw = 1;
        FPDFPageObj_GetStrokeWidth(obj, &sw);
        return sw * MatrixScale(ObjMatrix(obj));
    }
    PRect b = ObjectBounds(obj);
    return std::min(b.W(), b.H());
}

void SetLineThickness(Document& d, int page, FPDF_PAGEOBJECT obj, float thick) {
    if (!obj || thick <= 0 || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_PATH) return;
    int fm = 0; FPDF_BOOL st = 0;
    FPDFPath_GetDrawMode(obj, &fm, &st);
    if (st && fm == FPDF_FILLMODE_NONE) {    // a stroked line: thickness is the stroke width
        FPDFPageObj_SetStrokeWidth(obj, thick / MatrixScale(ObjMatrix(obj)));
        d.ContentChanged(page);
        return;
    }
    PRect b = ObjectBounds(obj);             // a filled bar: grow its short side about the centre
    float w = b.W(), h = b.H();
    if (w <= h) {
        float cx = (b.l + b.r) * 0.5f, sx = thick / std::max(w, 0.01f);
        Xform(obj, sx, 0, 0, 1, cx - sx * cx, 0);
    } else {
        float cy = (b.b + b.t) * 0.5f, sy = thick / std::max(h, 0.01f);
        Xform(obj, 1, 0, 0, sy, 0, cy - sy * cy);
    }
    d.ContentChanged(page);
}

// =============================================================== grouping

static bool MarkIsGroup(FPDF_PAGEOBJECTMARK mark) {
    unsigned long n = 0;
    unsigned short buf[32] = { 0 };
    if (!FPDFPageObjMark_GetName(mark, buf, sizeof(buf) - 2, &n)) return false;
    return std::wstring(reinterpret_cast<wchar_t*>(buf)) == L"PEGroup";
}

int GetGroupId(FPDF_PAGEOBJECT obj) {
    if (!obj) return 0;
    int n = FPDFPageObj_CountMarks(obj);
    for (int i = 0; i < n; ++i) {
        FPDF_PAGEOBJECTMARK m = FPDFPageObj_GetMark(obj, (unsigned long)i);
        if (m && MarkIsGroup(m)) {
            int v = 0;
            if (FPDFPageObjMark_GetParamIntValue(m, "id", &v)) return v;
        }
    }
    return 0;
}

void ClearGroupId(Document& d, int page, FPDF_PAGEOBJECT obj) {
    if (!obj) return;
    for (int i = FPDFPageObj_CountMarks(obj) - 1; i >= 0; --i) {
        FPDF_PAGEOBJECTMARK m = FPDFPageObj_GetMark(obj, (unsigned long)i);
        if (m && MarkIsGroup(m)) FPDFPageObj_RemoveMark(obj, m);
    }
    d.ContentChanged(page);
}

void SetGroupId(Document& d, int page, FPDF_PAGEOBJECT obj, int id) {
    if (!obj) return;
    ClearGroupId(d, page, obj);
    FPDF_PAGEOBJECTMARK m = FPDFPageObj_AddMark(obj, "PEGroup");
    if (m) FPDFPageObjMark_SetIntParam(d.Raw(), obj, m, "id", id);
    d.ContentChanged(page);
}

int MaxGroupId(Document& d) {
    int mx = 0;
    for (int pg = 0; pg < d.PageCount(); ++pg) {
        FPDF_PAGE p = d.Page(pg);
        if (!p) continue;
        int n = FPDFPage_CountObjects(p);
        for (int i = 0; i < n; ++i) mx = std::max(mx, GetGroupId(FPDFPage_GetObject(p, i)));
    }
    return mx;
}

std::vector<FPDF_PAGEOBJECT> GroupMembers(Document& d, int page, int id) {
    std::vector<FPDF_PAGEOBJECT> v;
    FPDF_PAGE p = d.Page(page);
    if (!p || id == 0) return v;
    int n = FPDFPage_CountObjects(p);
    for (int i = 0; i < n; ++i) {
        FPDF_PAGEOBJECT o = FPDFPage_GetObject(p, i);
        if (o && GetGroupId(o) == id) v.push_back(o);
    }
    return v;
}

// ============================================================== clipboard

static int GetBlockMem(void* param, unsigned long pos, unsigned char* buf, unsigned long size) {
    const Bytes* b = static_cast<const Bytes*>(param);
    if ((uint64_t)pos + size > b->size()) return 0;
    memcpy(buf, b->data() + pos, size);
    return 1;
}

static bool IsPlainJpeg(FPDF_PAGEOBJECT img, FPDF_PAGE page) {
    if (FPDFImageObj_GetImageFilterCount(img) != 1) return false;
    char name[32] = { 0 };
    if (FPDFImageObj_GetImageFilter(img, 0, name, sizeof(name) - 1) == 0) return false;
    if (strcmp(name, "DCTDecode") != 0) return false;
    FPDF_IMAGEOBJ_METADATA md{};
    if (!FPDFImageObj_GetImageMetadata(img, page, &md)) return false;
    return md.colorspace == FPDF_COLORSPACE_DEVICEGRAY || md.colorspace == FPDF_COLORSPACE_DEVICERGB;
}

bool CaptureObject(Document& d, int page, FPDF_PAGEOBJECT obj, ClipObject& out) {
    if (!obj) return false;
    out = ClipObject{};
    out.type = FPDFPageObj_GetType(obj);
    out.matrix = ObjMatrix(obj);
    out.bounds = ObjectBounds(obj);
    out.groupId = GetGroupId(obj);
    out.srcObj = obj;
    out.srcPage = page;
    out.srcGen = d.Generation();
    FPDFPageObj_GetFillColor(obj, &out.fr, &out.fg, &out.fb, &out.fa);
    FPDFPageObj_GetStrokeColor(obj, &out.sr, &out.sg, &out.sb, &out.sa);
    FPDFPageObj_GetStrokeWidth(obj, &out.strokeW);

    if (out.type == FPDF_PAGEOBJ_TEXT) {
        float sz = 12;
        FPDFTextObj_GetFontSize(obj, &sz);
        out.fontSize = sz;
        out.visualSize = sz * MatrixScale(out.matrix);
        out.text = TextOf(d, page, obj);
        out.textRender = FPDFTextObj_GetTextRenderMode(obj);
        if (FPDF_FONT f = FPDFTextObj_GetFont(obj)) {
            out.fontBase = BaseFontOf(obj);
            if (FPDFFont_GetIsEmbedded(f) == 1) {
                size_t need = 0;
                if (FPDFFont_GetFontData(f, nullptr, 0, &need) && need > 0 && need < (size_t)50 * 1024 * 1024) {
                    out.fontData.resize(need);
                    size_t got = 0;
                    if (FPDFFont_GetFontData(f, out.fontData.data(), need, &got)) out.fontData.resize(got);
                    else out.fontData.clear();
                }
            }
        }
        return !out.text.empty();
    }
    if (out.type == FPDF_PAGEOBJ_PATH) {
        PathStyle st = ReadPathStyle(obj);
        out.fillMode = st.fillMode;
        out.stroke = st.stroke;
        out.dash = st.dash;
        out.dashPhase = st.phase;
        out.lineCap = st.cap;
        out.lineJoin = st.join;
        out.segs = RawSegs(obj);
        return !out.segs.empty();
    }
    if (out.type == FPDF_PAGEOBJ_IMAGE) {
        FPDF_PAGE p = d.Page(page);
        if (IsPlainJpeg(obj, p)) {
            unsigned long n = FPDFImageObj_GetImageDataRaw(obj, nullptr, 0);
            if (n > 0 && n < 200u * 1024 * 1024) {
                out.jpeg.resize(n);
                if (FPDFImageObj_GetImageDataRaw(obj, out.jpeg.data(), n) != n) out.jpeg.clear();
            }
        }
        if (!ImagePixels(obj, out.image)) {
            FPDF_BITMAP bm = FPDFImageObj_GetRenderedBitmap(d.Raw(), p, obj);
            BitmapToImage(bm, out.image);
            if (bm) FPDFBitmap_Destroy(bm);
        } else if (out.matrix.b == 0 && out.matrix.c == 0 && out.matrix.a > 0 && out.matrix.d > 0) {
            // Raw pixels ignore the soft mask; borrow transparency from the rendered copy.
            FPDF_BITMAP bm = FPDFImageObj_GetRenderedBitmap(d.Raw(), p, obj);
            Image rend;
            if (BitmapToImage(bm, rend) && rend.w > 0 && rend.h > 0 &&
                std::any_of(rend.px.begin(), rend.px.end(), [](uint32_t q) { return (q >> 24) != 255; })) {
                for (int y = 0; y < out.image.h; ++y)
                    for (int x = 0; x < out.image.w; ++x) {
                        uint32_t a = rend.px[(size_t)(y * rend.h / out.image.h) * rend.w + (x * rend.w / out.image.w)] >> 24;
                        uint32_t& q = out.image.px[(size_t)y * out.image.w + x];
                        q = (q & 0x00FFFFFFu) | (a << 24);
                    }
                out.jpeg.clear();            // JPEG can't carry the mask; use the pixels
            }
            if (bm) FPDFBitmap_Destroy(bm);
        }
        return out.image.w > 0 || !out.jpeg.empty();
    }
    return false;   // forms are exploded before copying; shadings can't be rebuilt
}

FPDF_PAGEOBJECT PasteObject(Document& d, int page, const ClipObject& c, float dx, float dy,
                            int newGroupId, std::string& err) {
    FPDF_PAGE p = d.Page(page);
    if (!p) { err = "page not loaded"; return nullptr; }
    FS_MATRIX m = c.matrix;
    m.e += dx; m.f += dy;
    FPDF_PAGEOBJECT o = nullptr;

    if (c.type == FPDF_PAGEOBJ_TEXT) {
        bool live = c.srcObj && c.srcGen == d.Generation() && ObjectExists(d, c.srcPage, c.srcObj) &&
                    FPDFPageObj_GetType(c.srcObj) == FPDF_PAGEOBJ_TEXT;
        auto build = [&](FPDF_FONT font) -> FPDF_PAGEOBJECT {
            if (!font) return nullptr;
            FPDF_PAGEOBJECT t = FPDFPageObj_CreateTextObj(d.Raw(), font, (float)c.fontSize);
            if (!t) return nullptr;
            if (!FPDFText_SetText(t, WS(c.text))) { FPDFPageObj_Destroy(t); return nullptr; }
            FPDFPageObj_SetFillColor(t, c.fr, c.fg, c.fb, c.fa ? c.fa : 255);
            FPDFPageObj_SetStrokeColor(t, c.sr, c.sg, c.sb, c.sa ? c.sa : 255);
            if (c.textRender >= 0) FPDFTextObj_SetTextRenderMode(t, (FPDF_TEXT_RENDERMODE)c.textRender);
            FPDFPageObj_SetMatrix(t, &m);
            FPDFPage_InsertObject(p, t);
            return t;
        };
        // Same document and the source still exists: reuse its exact font.
        o = build(live ? FPDFTextObj_GetFont(c.srcObj) : LoadFontFor(d, c.fontData, c.fontBase));
        if (o && !live && !CharsRoundTrip(d, page, o, c.text)) {
            if (FPDFPage_RemoveObject(p, o)) FPDFPageObj_Destroy(o);
            o = build(LoadFontFor(d, {}, c.fontBase, true));
        }
        if (!o) { err = "could not recreate the text"; return nullptr; }
        // Without the original font object, make the copy occupy the original's
        // exact box so it can't come out bigger or smaller.
        if (!live && c.matrix.b == 0 && c.matrix.c == 0 && c.bounds.W() > 0.5f && c.bounds.H() > 0.5f) {
            PRect nb = ObjectBounds(o);
            if (nb.W() > 0.5f && nb.H() > 0.5f) {
                float kx = std::clamp(c.bounds.W() / nb.W(), 0.4f, 2.5f);
                float ky = std::clamp(c.bounds.H() / nb.H(), 0.4f, 2.5f);
                if (std::fabs(kx - 1) > 0.02f || std::fabs(ky - 1) > 0.02f) {
                    FS_MATRIX sm = MatMul(FS_MATRIX{ kx, 0, 0, ky, 0, 0 }, m);
                    FPDFPageObj_SetMatrix(o, &sm);
                }
            }
        }
    } else {
        if (c.type == FPDF_PAGEOBJ_PATH) {
            o = BuildPath(ParseSegs(c.segs));
            if (o) {
                PathStyle st;
                st.m = m; st.fillMode = c.fillMode; st.stroke = c.stroke;
                st.fr = c.fr; st.fg = c.fg; st.fb = c.fb; st.fa = c.fa ? c.fa : 255;
                st.sr = c.sr; st.sg = c.sg; st.sb = c.sb; st.sa = c.sa ? c.sa : 255;
                st.sw = c.strokeW; st.dash = c.dash; st.phase = c.dashPhase;
                st.cap = c.lineCap; st.join = c.lineJoin;
                ApplyPathStyle(d, page, o, st, c.fillMode != FPDF_FILLMODE_NONE, c.stroke);
            }
        } else if (c.type == FPDF_PAGEOBJ_IMAGE) {
            o = FPDFPageObj_NewImageObj(d.Raw());
            bool ok = false;
            if (o && !c.jpeg.empty()) {
                FPDF_FILEACCESS fa{};
                fa.m_FileLen = (unsigned long)c.jpeg.size();
                fa.m_GetBlock = GetBlockMem;
                fa.m_Param = const_cast<Bytes*>(&c.jpeg);
                ok = FPDFImageObj_LoadJpegFileInline(nullptr, 0, o, &fa) != 0;   // lossless copy
            }
            if (o && !ok) ok = SetImagePixels(nullptr, 0, o, c.image);
            if (o && !ok) { FPDFPageObj_Destroy(o); o = nullptr; }
            if (o) FPDFPageObj_SetMatrix(o, &m);
        }
        if (!o) { err = "could not recreate the object"; return nullptr; }
        FPDFPage_InsertObject(p, o);
    }
    if (newGroupId) SetGroupId(d, page, o, newGroupId);
    d.ContentChanged(page);
    return o;
}

PRect ClipBounds(const std::vector<ClipObject>& clip) {
    PRect u;
    bool first = true;
    for (const auto& c : clip) {
        u = first ? c.bounds : u.Union(c.bounds);
        first = false;
    }
    return u;
}

// ============================================================== redaction

RedactResult Redact(Document& d, int page, PRect box, unsigned r, unsigned g, unsigned b) {
    RedactResult res;
    FPDF_PAGE p = d.Page(page);
    if (!p || box.Area() <= 0) return res;

    // 1. Bring any group touching the box onto the page so its content can be cut.
    for (bool again = true; again;) {
        again = false;
        int n = FPDFPage_CountObjects(p);
        for (int i = 0; i < n; ++i) {
            FPDF_PAGEOBJECT o = FPDFPage_GetObject(p, i);
            if (o && FPDFPageObj_GetType(o) == FPDF_PAGEOBJ_FORM && ObjectBounds(o).Intersects(box)) {
                ExplodeFormToPage(p, o);
                again = true;
                break;
            }
        }
    }
    d.ContentChanged(page);                 // reload the text layer from the live objects

    // 2. Text: drop covered characters, rebuild the rest of each line in place.
    struct Ch { std::wstring u; double ox = 0, oy = 0; bool covered = false; };
    std::map<FPDF_PAGEOBJECT, std::vector<Ch>> perObj;
    std::vector<FPDF_PAGEOBJECT> order;
    if (FPDF_TEXTPAGE tp = d.TextPage(page)) {
        int nch = FPDFText_CountChars(tp);
        for (int i = 0; i < nch; ++i) {
            if (FPDFText_IsGenerated(tp, i) == 1) continue;
            FPDF_PAGEOBJECT to = FPDFText_GetTextObject(tp, i);
            if (!to || !ObjectBounds(to).Intersects(box)) continue;
            FS_RECTF rc{};
            if (!FPDFText_GetLooseCharBox(tp, i, &rc)) continue;
            Ch c;
            c.u = FromUtf32(FPDFText_GetUnicode(tp, i));
            FPDFText_GetCharOrigin(tp, i, &c.ox, &c.oy);
            c.covered = PRect::FromPoints(rc.left, rc.bottom, rc.right, rc.top).Intersects(box);
            if (!perObj.count(to)) order.push_back(to);
            perObj[to].push_back(c);
        }
    }
    for (FPDF_PAGEOBJECT to : order) {
        const auto& chars = perObj[to];
        bool any = std::any_of(chars.begin(), chars.end(), [](const Ch& c) { return c.covered; });
        if (!any) continue;
        int idx = TopIndex(p, to);
        if (idx < 0) continue;
        FS_MATRIX m = ObjMatrix(to);
        float tf = 12; FPDFTextObj_GetFontSize(to, &tf);
        unsigned fr = 0, fg = 0, fb = 0, fa = 255, sr = 0, sg = 0, sb = 0, sa = 255;
        FPDFPageObj_GetFillColor(to, &fr, &fg, &fb, &fa);
        FPDFPageObj_GetStrokeColor(to, &sr, &sg, &sb, &sa);
        int render = FPDFTextObj_GetTextRenderMode(to);
        int gid = GetGroupId(to);
        std::string base = BaseFontOf(to);
        FPDF_FONT font = FPDFTextObj_GetFont(to);
        int inserted = 0;
        size_t i = 0;
        while (i < chars.size()) {
            if (chars[i].covered) { ++i; continue; }
            size_t j = i;
            std::wstring run;
            while (j < chars.size() && !chars[j].covered) run += chars[j++].u;
            bool blank = std::all_of(run.begin(), run.end(), [](wchar_t c) { return iswspace(c) != 0; });
            if (!run.empty() && !blank) {
                auto build = [&](FPDF_FONT f) -> FPDF_PAGEOBJECT {
                    if (!f) return nullptr;
                    FPDF_PAGEOBJECT t = FPDFPageObj_CreateTextObj(d.Raw(), f, tf);
                    if (!t) return nullptr;
                    if (!FPDFText_SetText(t, WS(run))) { FPDFPageObj_Destroy(t); return nullptr; }
                    FS_MATRIX rm{ m.a, m.b, m.c, m.d, (float)chars[i].ox, (float)chars[i].oy };
                    FPDFPageObj_SetMatrix(t, &rm);
                    FPDFPageObj_SetFillColor(t, fr, fg, fb, fa);
                    FPDFPageObj_SetStrokeColor(t, sr, sg, sb, sa);
                    if (render >= 0) FPDFTextObj_SetTextRenderMode(t, (FPDF_TEXT_RENDERMODE)render);
                    if (gid) SetGroupId(d, page, t, gid);
                    InsertAt(p, t, idx + 1 + inserted);
                    return t;
                };
                FPDF_PAGEOBJECT t = build(font);
                if (t && !CharsRoundTrip(d, page, t, run)) {
                    if (FPDFPage_RemoveObject(p, t)) FPDFPageObj_Destroy(t);
                    t = build(LoadFontFor(d, {}, base, true));
                }
                if (t) ++inserted;
            }
            i = j;
        }
        if (FPDFPage_RemoveObject(p, to)) FPDFPageObj_Destroy(to);
        if (inserted) ++res.textSplit; else ++res.removed;
    }

    // 3. Everything else: inside the box -> delete; pictures overlapping -> black out pixels.
    for (bool again = true; again;) {
        again = false;
        int n = FPDFPage_CountObjects(p);
        for (int i = 0; i < n; ++i) {
            FPDF_PAGEOBJECT o = FPDFPage_GetObject(p, i);
            if (!o) continue;
            PRect ob = ObjectBounds(o);
            if (ob.Inside(box)) {
                if (FPDFPage_RemoveObject(p, o)) { FPDFPageObj_Destroy(o); ++res.removed; again = true; break; }
            }
        }
    }
    int n = FPDFPage_CountObjects(p);
    for (int i = 0; i < n; ++i) {
        FPDF_PAGEOBJECT o = FPDFPage_GetObject(p, i);
        if (!o || FPDFPageObj_GetType(o) != FPDF_PAGEOBJ_IMAGE || !ObjectBounds(o).Intersects(box)) continue;
        Image img;
        FS_MATRIX M = ObjMatrix(o), inv;
        if (!ImagePixels(o, img) || !MatInvert(M, inv)) continue;
        bool changed = false;
        for (int y = 0; y < img.h; ++y)
            for (int x = 0; x < img.w; ++x) {
                float u = (x + 0.5f) / img.w, v = 1.0f - (y + 0.5f) / img.h, qx, qy;
                MatApply(M, u, v, qx, qy);
                if (box.Contains(qx, qy)) {
                    img.px[(size_t)y * img.w + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
                    changed = true;
                }
            }
        FPDF_PAGE pages[1] = { p };
        if (changed && SetImagePixels(pages, 1, o, img)) ++res.imagesMasked;
    }

    // 4. Annotations / form fields touching the box.
    for (int i = FPDFPage_GetAnnotCount(p) - 1; i >= 0; --i) {
        FPDF_ANNOTATION an = FPDFPage_GetAnnot(p, i);
        if (!an) continue;
        FS_RECTF rc{};
        bool hit = FPDFAnnot_GetRect(an, &rc) &&
                   PRect::FromPoints(rc.left, rc.bottom, rc.right, rc.top).Intersects(box);
        if (hit && FPDFAnnot_GetSubtype(an) == FPDF_ANNOT_WIDGET) FPDFAnnot_SetStringValue(an, "V", WS(L""));
        FPDFPage_CloseAnnot(an);
        if (hit && FPDFPage_RemoveAnnot(p, i)) ++res.annotsRemoved;
    }

    AddRect(d, page, box, true, r, g, b, false, 0, 0, 0, 0);
    d.ContentChanged(page);
    return res;
}

// ============================================================= annotations

std::optional<AnnotInfo> AnnotAt(Document& d, int page, float px, float py, float tolPt, bool includeWidgets) {
    FPDF_PAGE p = d.Page(page);
    if (!p) return std::nullopt;
    for (int i = FPDFPage_GetAnnotCount(p) - 1; i >= 0; --i) {
        FPDF_ANNOTATION an = FPDFPage_GetAnnot(p, i);
        if (!an) continue;
        int st = FPDFAnnot_GetSubtype(an);
        FS_RECTF rc{};
        bool ok = st != FPDF_ANNOT_POPUP && st != FPDF_ANNOT_LINK && (includeWidgets || st != FPDF_ANNOT_WIDGET) &&
                  FPDFAnnot_GetRect(an, &rc);
        FPDFPage_CloseAnnot(an);
        if (!ok) continue;
        PRect r = PRect::FromPoints(rc.left, rc.bottom, rc.right, rc.top);
        PRect e{ r.l - tolPt, r.b - tolPt, r.r + tolPt, r.t + tolPt };
        if (e.Contains(px, py)) return AnnotInfo{ i, st, r };
    }
    return std::nullopt;
}

bool AnnotRect(Document& d, int page, int index, PRect& out) {
    FPDF_PAGE p = d.Page(page);
    if (!p || index < 0 || index >= FPDFPage_GetAnnotCount(p)) return false;
    FPDF_ANNOTATION an = FPDFPage_GetAnnot(p, index);
    if (!an) return false;
    FS_RECTF rc{};
    bool ok = FPDFAnnot_GetRect(an, &rc);
    FPDFPage_CloseAnnot(an);
    if (ok) out = PRect::FromPoints(rc.left, rc.bottom, rc.right, rc.top);
    return ok;
}

bool MoveAnnot(Document& d, int page, int index, float dx, float dy) {
    FPDF_PAGE p = d.Page(page);
    if (!p || index < 0 || index >= FPDFPage_GetAnnotCount(p)) return false;
    FPDF_ANNOTATION an = FPDFPage_GetAnnot(p, index);
    if (!an) return false;
    FS_RECTF rc{};
    bool ok = FPDFAnnot_GetRect(an, &rc);
    if (ok) {
        rc.left += dx; rc.right += dx; rc.top += dy; rc.bottom += dy;
        FPDFAnnot_SetRect(an, &rc);
        size_t nq = FPDFAnnot_CountAttachmentPoints(an);
        for (size_t q = 0; q < nq; ++q) {
            FS_QUADPOINTSF qp{};
            if (!FPDFAnnot_GetAttachmentPoints(an, q, &qp)) continue;
            qp.x1 += dx; qp.x2 += dx; qp.x3 += dx; qp.x4 += dx;
            qp.y1 += dy; qp.y2 += dy; qp.y3 += dy; qp.y4 += dy;
            FPDFAnnot_SetAttachmentPoints(an, q, &qp);
        }
        if (FPDFAnnot_GetSubtype(an) == FPDF_ANNOT_INK) {
            std::vector<std::vector<FS_POINTF>> strokes;
            unsigned long ns = FPDFAnnot_GetInkListCount(an);
            for (unsigned long s = 0; s < ns; ++s) {
                unsigned long cnt = FPDFAnnot_GetInkListPath(an, s, nullptr, 0);
                std::vector<FS_POINTF> pts(cnt);
                if (cnt) FPDFAnnot_GetInkListPath(an, s, pts.data(), cnt);
                for (auto& pt : pts) { pt.x += dx; pt.y += dy; }
                strokes.push_back(std::move(pts));
            }
            if (!strokes.empty() && FPDFAnnot_RemoveInkList(an))
                for (auto& s : strokes)
                    if (!s.empty()) FPDFAnnot_AddInkStroke(an, s.data(), s.size());
        }
    }
    FPDFPage_CloseAnnot(an);
    if (ok) d.ContentChanged(page);
    return ok;
}

bool DeleteAnnot(Document& d, int page, int index) {
    FPDF_PAGE p = d.Page(page);
    if (!p || index < 0 || index >= FPDFPage_GetAnnotCount(p)) return false;
    if (!FPDFPage_RemoveAnnot(p, index)) return false;
    d.ContentChanged(page);
    return true;
}

bool FlattenPage(Document& d, int page) {
    FPDF_PAGE p = d.Page(page);
    if (!p) return false;
    d.Bake();
    int r = FPDFPage_Flatten(p, FLAT_NORMALDISPLAY);
    d.StructureChanged();
    return r == FLATTEN_SUCCESS;
}

// ===================================================== extraction / search

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

// ============================================================== page tree

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
    d.Bake();
    int cur = FPDFPage_GetRotation(p);
    FPDFPage_SetRotation(p, ((cur + quartersCW) % 4 + 4) % 4);
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
        if (!SaveImageFile(dir + name, buf.data(), w, h, err)) return false;
    }
    return true;
}

// ================================================================== forms

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
                fi.name = ReadUtf16([&](unsigned short* b, unsigned long n) { return FPDFAnnot_GetFormFieldName(form, an, b, n); });
                fi.value = ReadUtf16([&](unsigned short* b, unsigned long n) { return FPDFAnnot_GetFormFieldValue(form, an, b, n); });
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
        if (p && FPDFPage_Flatten(p, FLAT_NORMALDISPLAY) == FLATTEN_SUCCESS) any = true;
    }
    d.StructureChanged();
    return any;
}

std::wstring GetMeta(Document& d, const char* key) {
    if (!d.Raw()) return {};
    return ReadUtf16([&](unsigned short* b, unsigned long n) { return FPDF_GetMetaText(d.Raw(), key, b, n); });
}

} // namespace ops
