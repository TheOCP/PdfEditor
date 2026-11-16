#include "app.h"
#include "dialogs.h"
#include "imageio.h"
#include "imgui.h"
#include "i18n.h"
#include "icons.h"
#include "theme.h"
#include <algorithm>
#include <cfloat>
#include <cmath>

namespace {
const float kGap = 16.0f;              // vertical gap between pages (pixels)
const float kPad = 16.0f;              // top/bottom padding
const int   kMaxTexDim = 4096;
const int64_t kMaxFullPixels = 9000000; // larger pages render only their visible part

ImU32 Col(const float c[4]) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], c[3])); }

// 8 resize handles for a screen rect: 0 TL,1 T,2 TR,3 R,4 BR,5 B,6 BL,7 L.
void HandlePoints(ImVec2 p0, ImVec2 p1, ImVec2 out[8]) {
    float mx = (p0.x + p1.x) * 0.5f, my = (p0.y + p1.y) * 0.5f;
    out[0] = { p0.x, p0.y }; out[1] = { mx, p0.y }; out[2] = { p1.x, p0.y };
    out[3] = { p1.x, my };   out[4] = { p1.x, p1.y }; out[5] = { mx, p1.y };
    out[6] = { p0.x, p1.y }; out[7] = { p0.x, my };
}
int HandleAt(const ImVec2 h[8], ImVec2 m, float tol) {
    for (int i = 0; i < 8; ++i)
        if (std::fabs(h[i].x - m.x) <= tol && std::fabs(h[i].y - m.y) <= tol) return i;
    return -1;
}
ImGuiMouseCursor HandleCursor(int h) {
    switch (h) {
    case 0: case 4: return ImGuiMouseCursor_ResizeNWSE;
    case 2: case 6: return ImGuiMouseCursor_ResizeNESW;
    case 1: case 5: return ImGuiMouseCursor_ResizeNS;
    default: return ImGuiMouseCursor_ResizeEW;
    }
}
float Dist(ImVec2 a, ImVec2 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }
} // namespace

// ------------------------------------------------------------ coordinates

ImVec2 App::PageToScreenPt(int i, float px, float py) {
    if (i < 0 || i >= (int)cache.size()) return ImVec2(-1e6f, -1e6f);
    float dx, dy;
    doc.PageToDev(i, zoom, px, py, dx, dy);
    return ImVec2(cache[i].topLeft.x + dx, cache[i].topLeft.y + dy);
}

bool App::ScreenToPage(ImVec2 s, int& page, float& px, float& py) {
    for (int i = 0; i < (int)cache.size(); ++i) {
        PageTex& c = cache[i];
        if (!c.laid) continue;
        if (s.x >= c.topLeft.x && s.x <= c.topLeft.x + c.dispW && s.y >= c.topLeft.y && s.y <= c.topLeft.y + c.dispH) {
            doc.DevToPage(i, zoom, s.x - c.topLeft.x, s.y - c.topLeft.y, px, py);
            page = i;
            return true;
        }
    }
    return false;
}

// Screen-space box of a page-space rect.
static void ScreenBox(App& a, int page, PRect r, ImVec2& p0, ImVec2& p1) {
    ImVec2 c0 = a.PageToScreenPt(page, r.l, r.t), c1 = a.PageToScreenPt(page, r.r, r.b);
    ImVec2 c2 = a.PageToScreenPt(page, r.l, r.b), c3 = a.PageToScreenPt(page, r.r, r.t);
    p0 = ImVec2(std::min({ c0.x, c1.x, c2.x, c3.x }), std::min({ c0.y, c1.y, c2.y, c3.y }));
    p1 = ImVec2(std::max({ c0.x, c1.x, c2.x, c3.x }), std::max({ c0.y, c1.y, c2.y, c3.y }));
}

// -------------------------------------------------------------- selection

void App::ClearSelection() {
    sel.Clear();
    groupSel.clear();
    selGroupId = 0;
    ClearAnnotSel();
}

void App::SelectObjectAt(int page, float px, float py) {
    auto hit = ops::HitTest(doc, page, px, py, 5.0f * dpiScale / std::max(0.05f, zoom));
    ClearAnnotSel();
    if (hit) sel.Set(doc, page, *hit);
    else sel.Clear();
}

bool App::EnsureSelectionEditable() {
    if (!sel.Valid(doc)) return false;
    if (!sel.parent) return true;
    FPDF_PAGEOBJECT target = sel.obj;
    if (!ops::PromoteObject(doc, sel.page, target)) return false;
    sel.parent = nullptr;
    sel.accum = FS_MATRIX{ 1, 0, 0, 1, 0, 0 };
    sel.bounds = ops::ObjectBounds(target);
    if (groupSel.size() <= 1) groupSel = { target };
    InvalidatePage(sel.page);
    return sel.Valid(doc);
}

void App::SelectNewObject(int pg, FPDF_PAGEOBJECT o) {
    ClearAnnotSel();
    sel.page = pg; sel.obj = o; sel.parent = nullptr;
    sel.accum = FS_MATRIX{ 1, 0, 0, 1, 0, 0 }; sel.gen = doc.Generation();
    sel.bounds = ops::ObjectBounds(o);
    groupSel.assign(1, o);
    selGroupId = 0;
}

void App::SelectObjects(int pg, const std::vector<FPDF_PAGEOBJECT>& objs, int groupId) {
    ClearSelection();
    if (objs.empty()) return;
    SelectNewObject(pg, objs[0]);
    groupSel = objs;
    selGroupId = groupId;
}

void App::RefreshGroupSel() {
    groupSel.clear();
    selGroupId = 0;
    if (!sel.Valid(doc)) return;
    if (!sel.parent) {
        int gid = ops::GetGroupId(sel.obj);
        if (gid) { selGroupId = gid; groupSel = ops::GroupMembers(doc, sel.page, gid); }
    }
    if (groupSel.empty()) groupSel.push_back(sel.obj);
}

PRect App::SelectionBounds() {
    if (AnnotSelected()) {
        PRect r;
        ops::AnnotRect(doc, annSelPage, annSelIdx, r);
        return r;
    }
    if (groupSel.size() <= 1) return sel.PageBounds();
    PRect u = ops::ObjectBounds(groupSel[0]);
    for (size_t i = 1; i < groupSel.size(); ++i) u = u.Union(ops::ObjectBounds(groupSel[i]));
    return u;
}

void App::MoveSelection(float dx, float dy) {
    if (AnnotSelected()) {
        ops::MoveAnnot(doc, annSelPage, annSelIdx, dx, dy);
        InvalidatePage(annSelPage);
        return;
    }
    if (!sel.Valid(doc)) return;
    if (groupSel.size() <= 1) {
        if (sel.parent) EnsureSelectionEditable();
        ops::TranslateObjectPage(doc, sel.page, sel.obj, sel.accum, dx, dy);
    } else {
        FS_MATRIX id{ 1, 0, 0, 1, 0, 0 };
        for (auto o : groupSel) ops::TranslateObjectPage(doc, sel.page, o, id, dx, dy);
    }
    InvalidatePage(sel.page);
}

void App::ResizeSelectionTo(PRect target) {
    if (!sel.Valid(doc)) return;
    if (groupSel.size() <= 1) {
        if (sel.parent) EnsureSelectionEditable();
        ops::ResizeObjectPageTopLevel(doc, sel.page, sel.obj, target);
        InvalidatePage(sel.page);
        return;
    }
    // Lines keep their thickness, text its size; with "keep line spacing" only
    // the outer row / column changes.
    PRect cur = SelectionBounds();
    ops::ResizeGroup(doc, sel.page, groupSel, cur, target, SettingFlag("keepspacing", false));
    InvalidatePage(sel.page);
}

// ------------------------------------------------------------- alignment

void App::PrepareAlign() {
    alignCands.clear();
    guideX.clear(); guideY.clear();
    alignOn = false; alignHasOrigin = false;
    if (!sel.Valid(doc)) return;
    std::vector<FPDF_PAGEOBJECT> mine = groupSel.size() > 1 ? groupSel : std::vector<FPDF_PAGEOBJECT>{ sel.obj };
    for (auto o : mine) {
        if (ops::ObjectType(o) != FPDF_PAGEOBJ_TEXT) continue;
        alignOn = true;
        FS_MATRIX acc = (o == sel.obj) ? sel.accum : FS_MATRIX{ 1, 0, 0, 1, 0, 0 };
        if (!alignHasOrigin && ops::TextOrigin(o, acc, alignOx, alignOy)) alignHasOrigin = true;
    }
    if (alignOn) alignCands = ops::AlignRefs(doc, sel.page, mine);
}

// (dx, dy) = how far the selection would move from where the drag began.
// Lines it up with the nearest other text (left start, centre, right edge,
// baseline) when that is within a few pixels.
void App::SnapMove(float& dx, float& dy, bool freePlacement) {
    guideX.clear(); guideY.clear();
    if (!alignOn || alignCands.empty() || !sel.Valid(doc)) return;
    bool show = SettingFlag("alignguides", true);
    bool snap = SettingFlag("alignsnap", true) && !freePlacement;
    if (!show && !snap) return;
    ImVec2 a = PageToScreenPt(sel.page, 0, 0), b = PageToScreenPt(sel.page, 100, 0);
    float pxPerPt = std::max(0.01f, std::fabs(b.x - a.x) / 100.0f);
    float thr = 8.0f * dpiScale / pxPerPt;
    ops::AlignHit h = ops::AlignSnap(alignCands, moveStart, alignHasOrigin, alignOx, alignOy, dx, dy, thr);
    if (h.x) { if (snap) dx += h.dx; if (show) guideX.push_back(h.gx); }
    if (h.y) { if (snap) dy += h.dy; if (show) guideY.push_back(h.gy); }
}

// Axis lines on screen, cut to the visible canvas (pages can be far taller
// than the window when zoomed in).
static void GuideLine(App& a, ImDrawList* dl, ImVec2 p, ImVec2 q, ImU32 col, float w, bool dashed) {
    ImVec2 lo = a.canvasOrigin, hi(a.canvasOrigin.x + a.canvasSize.x, a.canvasOrigin.y + a.canvasSize.y);
    auto clampV = [&](ImVec2 v) { return ImVec2(std::clamp(v.x, lo.x, hi.x), std::clamp(v.y, lo.y, hi.y)); };
    if ((p.x < lo.x && q.x < lo.x) || (p.x > hi.x && q.x > hi.x) || (p.y < lo.y && q.y < lo.y) || (p.y > hi.y && q.y > hi.y)) return;
    p = clampV(p); q = clampV(q);
    float len = Dist(p, q);
    if (len < 1) return;
    if (!dashed) { dl->AddLine(p, q, col, w); return; }
    float dash = 6.0f * a.dpiScale, gap = 4.0f * a.dpiScale;
    ImVec2 d((q.x - p.x) / len, (q.y - p.y) / len);
    for (float t = 0; t < len; t += dash + gap) {
        float e = std::min(len, t + dash);
        dl->AddLine(ImVec2(p.x + d.x * t, p.y + d.y * t), ImVec2(p.x + d.x * e, p.y + d.y * e), col, w);
    }
}

// Lines through the selected text's start, right edge, baseline and top,
// across the whole page - pink where they meet another object exactly.
void App::DrawSelectionGuides(ImDrawList* dl) {
    if (!SettingFlag("guidesalways", false) || AnnotSelected() || !sel.Valid(doc)) return;
    if (sel.page < 0 || sel.page >= (int)cache.size() || !cache[sel.page].laid) return;
    std::vector<FPDF_PAGEOBJECT> mine = groupSel.size() > 1 ? groupSel : std::vector<FPDF_PAGEOBJECT>{ sel.obj };
    bool text = false;
    for (auto o : mine) text |= ops::ObjectType(o) == FPDF_PAGEOBJ_TEXT;
    if (!text) return;

    if (guideRefsPage != sel.page || guideRefsGen != doc.Generation() || guideRefsDoc != doc.DocId() ||
        guideRefsObj != sel.obj || guideRefsCount != mine.size()) {
        guideRefs = ops::GuideRefs(doc, sel.page, mine);
        guideRefsPage = sel.page; guideRefsGen = doc.Generation(); guideRefsDoc = doc.DocId();
        guideRefsObj = sel.obj; guideRefsCount = mine.size();
    }
    ops::GuideEdges e = ops::SelectionEdges(mine, sel.accum, SelectionBounds());
    ops::GuideMatch m = ops::MatchGuides(guideRefs, e, 0.25f);

    PRect pb = doc.PageBox(sel.page);
    ImU32 cGuide = IM_COL32(40, 160, 255, 210), cHit = IM_COL32(255, 64, 170, 240);
    float w = std::max(1.0f, 1.0f * dpiScale), wHit = std::max(1.0f, 1.5f * dpiScale);
    for (const PRect& r : m.boxes) {
        ImVec2 p0, p1;
        ScreenBox(*this, sel.page, r, p0, p1);
        dl->AddRect(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1), IM_COL32(255, 64, 170, 150), 0, 0, w);
    }
    for (int k = 0; k < 2; ++k) {
        GuideLine(*this, dl, PageToScreenPt(sel.page, e.x[k], pb.b), PageToScreenPt(sel.page, e.x[k], pb.t),
                  m.x[k] ? cHit : cGuide, m.x[k] ? wHit : w, !m.x[k]);
        GuideLine(*this, dl, PageToScreenPt(sel.page, pb.l, e.y[k]), PageToScreenPt(sel.page, pb.r, e.y[k]),
                  m.y[k] ? cHit : cGuide, m.y[k] ? wHit : w, !m.y[k]);
    }
}

void App::DeleteSelection() {
    if (AnnotSelected()) {
        int pg = annSelPage;
        if (ops::DeleteAnnot(doc, pg, annSelIdx)) { ClearAnnotSel(); CommitIfDirty("Delete annotation"); }
        return;
    }
    if (!sel.Valid(doc)) return;
    int pg = sel.page;
    if (groupSel.size() <= 1) {
        EnsureSelectionEditable();
        ops::DeleteObjectNested(doc, pg, sel.obj, sel.parent);
    } else {
        for (auto o : groupSel) ops::DeleteObjectNested(doc, pg, o, nullptr);
    }
    ClearSelection();
    CommitIfDirty("Delete");
}

void App::GroupSelection() {
    if (groupSel.size() < 2 || !sel.Valid(doc)) return;
    int pg = sel.page;
    int gid = ops::MaxGroupId(doc) + 1;
    for (auto o : groupSel) ops::SetGroupId(doc, pg, o, gid);
    selGroupId = gid;
    int n = (int)groupSel.size();
    CommitIfDirty("Group objects");
    Toasts(Fmt(T("Grouped %d objects"), n));
}

void App::UngroupSelection() {
    if (!sel.Valid(doc)) return;
    int pg = sel.page;
    if (selGroupId) {
        for (auto o : ops::GroupMembers(doc, pg, selGroupId)) ops::ClearGroupId(doc, pg, o);
        selGroupId = 0;
        groupSel = { sel.obj };
        CommitIfDirty("Ungroup");
        Toasts(T("Split into separate pieces"));
        return;
    }
    if (!sel.parent && ops::ObjectType(sel.obj) == FPDF_PAGEOBJ_FORM) {
        ops::ExplodeForm(doc, pg, sel.obj);
        ClearSelection();
        CommitIfDirty("Ungroup");
        Toasts(T("Split into separate pieces"));
    }
}

void App::SplitSelectedPath(bool pickNearest, float px, float py) {
    if (!sel.Valid(doc) || ops::ObjectType(sel.obj) != FPDF_PAGEOBJ_PATH) return;
    EnsureSelectionEditable();
    if (ops::SplittableParts(sel.obj) < 2) { Toasts(T("This is already a single line")); return; }
    int pg = sel.page;
    auto parts = ops::SplitPath(doc, pg, sel.obj);
    if (parts.empty()) return;
    if (pickNearest) {
        FPDF_PAGEOBJECT best = parts[0];
        float bd = 1e30f;
        for (auto o : parts) {
            float dd = ops::DistanceToObject(o, px, py);
            if (dd <= bd) { bd = dd; best = o; }
        }
        SelectObjects(pg, { best }, 0);
    } else {
        SelectObjects(pg, parts, 0);
    }
    CommitIfDirty("Split into lines");
    Toasts(Fmt(T("Split into %d separate lines"), (int)parts.size()));
}

// Selects leaves (optionally only lines) on `pg` matching `want`, promoting any
// that sit inside groups so every one of them can actually be edited.
static void SelectLeaves(App& a, int pg, bool linesOnly, const PRect* within) {
    std::vector<FPDF_PAGEOBJECT> pick, nested;
    for (auto& o : ops::ListObjects(a.doc, pg)) {
        if (o.type == FPDF_PAGEOBJ_FORM) continue;
        if (linesOnly && !ops::IsLine(o.obj)) continue;      // real lines only, however drawn
        if (within && !o.bounds.Intersects(*within)) continue;
        pick.push_back(o.obj);
        if (o.parent) nested.push_back(o.obj);
    }
    if (pick.empty()) {
        a.ClearSelection();
        if (!within) a.Toasts(T(linesOnly ? "No lines on this page" : "Nothing on this page"), true);
        return;
    }
    if (!nested.empty()) ops::PromoteAll(a.doc, pg, nested);   // same handles, now top-level
    int common = -1;
    bool same = true;
    for (auto o : pick) {
        int gid = ops::GetGroupId(o);
        if (common == -1) common = gid; else if (common != gid) same = false;
    }
    a.SelectObjects(pg, pick, (same && common > 0) ? common : 0);
    a.InvalidatePage(pg);
    a.Toasts(Fmt(T(linesOnly ? "Selected %d line(s)" : "Selected %d object(s)"), (int)pick.size()));
}

void App::SelectAllOnPage(bool linesOnly) {
    if (doc.PageCount() == 0) return;
    SelectLeaves(*this, currentPage, linesOnly, nullptr);
}

void App::GroupAllLines() {
    SelectAllOnPage(true);
    if (groupSel.size() >= 2 && !selGroupId) GroupSelection();
    else if (selGroupId) Toasts(T("These lines are already a group"));
}

// --------------------------------------------------------------- clipboard

static std::vector<FPDF_PAGEOBJECT> SelMembers(App& a) {
    if (!a.groupSel.empty()) return a.groupSel;
    if (a.sel.obj) return { a.sel.obj };
    return {};
}

void App::ClearLimbo() {
    // Cut objects belong to the current PDFium document; free them only while it lives.
    if (!limbo.empty() && limboDoc == doc.DocId())
        for (auto o : limbo) FPDFPageObj_Destroy(o);
    limbo.clear();
}

void App::CopySelection() {
    if (AnnotSelected()) {
        Toasts(T("Annotations can't be copied - right-click > Make this page's annotations editable"), true);
        return;
    }
    if (!sel.Valid(doc)) return;
    if (groupSel.size() <= 1 && sel.parent) EnsureSelectionEditable();
    ClearLimbo();
    clip.clear();
    clipSrcPage = sel.page;
    pasteCount = 0;
    std::wstring textAll;
    int skipped = 0;
    for (auto o : SelMembers(*this)) {
        ops::ClipObject c;
        if (ops::CaptureObject(doc, sel.page, o, c)) {
            if (c.type == FPDF_PAGEOBJ_TEXT) {
                if (!textAll.empty()) textAll += L" ";
                textAll += c.text;
            }
            clip.push_back(std::move(c));
        } else {
            ++skipped;
        }
    }
    // The plain text also goes onto the Windows clipboard for other apps.
    if (!textAll.empty()) {
        clipText = W2U(textAll);
        ImGui::SetClipboardText(clipText.c_str());
    } else {
        clipText.clear();
    }
    clipSeq = GetClipboardSequenceNumber();
    if (clip.empty()) Toasts(T("Nothing in the selection can be copied"), true);
    else Toasts(Fmt(T(skipped ? "Copied %d object(s) (some parts could not be copied)" : "Copied %d object(s)"),
                    (int)clip.size()));
}

void App::CutSelection() {
    if (AnnotSelected()) { DeleteSelection(); return; }
    if (!sel.Valid(doc)) return;
    if (groupSel.size() <= 1 && sel.parent) EnsureSelectionEditable();
    CopySelection();
    if (clip.empty()) return;
    // Keep the real objects alive so the paste is exact (font, image, clip).
    int pg = sel.page;
    FPDF_PAGE p = doc.Page(pg);
    for (auto o : SelMembers(*this))
        if (p && FPDFPage_RemoveObject(p, o)) limbo.push_back(o);
    limboDoc = doc.DocId();
    doc.ContentChanged(pg);
    ClearSelection();
    CommitIfDirty("Cut");
    Toasts(Fmt(T("Cut %d object(s)"), (int)limbo.size()));
}

// Creates new text from plain text (e.g. copied in another program), in the
// style of the nearest existing text, one object per line.
static std::vector<FPDF_PAGEOBJECT> PasteTextAsObjects(App& a, int page, float x, float yTop, const std::string& utf8) {
    std::vector<FPDF_PAGEOBJECT> made;
    FPDF_PAGEOBJECT style = nullptr;
    ops::ClipObject c;
    float bestd = 1e30f;
    FS_MATRIX bestAccum{ 1, 0, 0, 1, 0, 0 };
    for (auto& o : ops::ListObjects(a.doc, page)) {
        if (o.type != FPDF_PAGEOBJ_TEXT) continue;
        float cx = (o.bounds.l + o.bounds.r) * 0.5f, cy = (o.bounds.b + o.bounds.t) * 0.5f;
        float dd = (cx - x) * (cx - x) + (cy - yTop) * (cy - yTop);
        if (dd < bestd) { bestd = dd; style = o.obj; bestAccum = o.accum; }
    }
    bool styled = style && ops::CaptureObject(a.doc, page, style, c);
    double size = styled ? c.visualSize * ops::MatrixScale(bestAccum) : a.fontSize;
    if (size < 2 || size > 200) size = a.fontSize;
    unsigned r = styled ? c.fr : (unsigned)(a.fillColor[0] * 255);
    unsigned g = styled ? c.fg : (unsigned)(a.fillColor[1] * 255);
    unsigned b = styled ? c.fb : (unsigned)(a.fillColor[2] * 255);
    std::wstring all = U2W(utf8);
    std::vector<std::wstring> lines(1);
    for (wchar_t ch : all) {
        if (ch == L'\r') continue;
        if (ch == L'\n') lines.emplace_back(); else lines.back() += ch;
    }
    float y = yTop - (float)size * 0.8f;
    for (auto& ln : lines) {
        bool blank = std::all_of(ln.begin(), ln.end(), [](wchar_t ch) { return iswspace(ch) != 0; });
        if (!ln.empty() && !blank) {
            std::string err;
            ops::TextEditInfo ti;
            FPDF_PAGEOBJECT o = styled && a.matchNearbyFont
                ? ops::AddTextLike(a.doc, page, ln, style, c.fontData, c.fontBase, size, x, y, r, g, b, err, &ti)
                : a.AddTextInFont(page, ln, size, x, y, r, g, b, err, &ti);
            if (!ti.pieces.empty()) made.insert(made.end(), ti.pieces.begin(), ti.pieces.end());
            else if (o) made.push_back(o);
        }
        y -= (float)size * 1.2f;
    }
    return made;
}

void App::PasteClipboard(bool atCursor) {
    if (!doc.Raw() || doc.PageCount() == 0) return;
    const char* sys = ImGui::GetClipboardText();
    std::string sysText = sys ? sys : "";
    // Something was copied elsewhere after our last copy -> paste that text.
    bool external = !sysText.empty() && (clip.empty() || GetClipboardSequenceNumber() != clipSeq);
    int tp = hoverPage >= 0 ? hoverPage : currentPage;
    if (tp < 0 || tp >= doc.PageCount()) tp = 0;

    if (external) {
        float x, y;
        if (hoverPage >= 0) { x = hoverPx; y = hoverPy; }
        else { FS_SIZEF sz = doc.PageSizePt(tp); x = 72; y = sz.height * 0.6f; }
        auto made = PasteTextAsObjects(*this, tp, x, y, sysText);
        if (made.empty()) { Toasts(T("Nothing to paste"), true); return; }
        SelectObjects(tp, made, 0);
        CommitIfDirty("Paste text");
        Toasts(Fmt(T(made.size() == 1 ? "Pasted text (%d line) onto page %d" : "Pasted text (%d lines) onto page %d"),
                   (int)made.size(), tp + 1));
        return;
    }
    if (clip.empty()) return;

    PRect cb = ops::ClipBounds(clip);
    float dx = 0, dy = 0;
    if (atCursor && hoverPage >= 0) {           // top-left corner at the mouse
        dx = hoverPx - cb.l; dy = hoverPy - cb.t;
    } else if (tp == clipSrcPage && limbo.empty()) {
        ++pasteCount;                           // same page: offset so the copy is visible
        dx = 10.0f * pasteCount; dy = -10.0f * pasteCount;
    }
    int keepGid = 0;
    bool allGrouped = clip.size() > 1 && clip[0].groupId != 0;
    for (auto& c : clip) if (c.groupId != clip[0].groupId) allGrouped = false;

    std::vector<FPDF_PAGEOBJECT> pasted;
    if (!limbo.empty() && limboDoc == doc.DocId()) {
        // Exact paste of what was cut: the very same objects.
        FPDF_PAGE p = doc.Page(tp);
        for (auto o : limbo) {
            if (dx != 0 || dy != 0) ops::TransformObjectPage(doc, tp, o, FS_MATRIX{ 1, 0, 0, 1, dx, dy });
            if (p && FPDFPage_InsertObject(p, o)) pasted.push_back(o);
        }
        limbo.clear();
        doc.ContentChanged(tp);
        if (allGrouped) keepGid = clip[0].groupId;
        for (size_t i = 0; i < clip.size() && i < pasted.size(); ++i) {   // later pastes reuse their fonts
            clip[i].srcObj = pasted[i]; clip[i].srcPage = tp; clip[i].srcGen = doc.Generation();
        }
        clipSrcPage = tp;
        pasteCount = 0;
    } else {
        limbo.clear();
        if (allGrouped) keepGid = ops::MaxGroupId(doc) + 1;
        std::string err;
        for (auto& c : clip)
            if (FPDF_PAGEOBJECT o = ops::PasteObject(doc, tp, c, dx, dy, keepGid, err)) pasted.push_back(o);
        if (pasted.empty()) { Toasts(std::string(T("Paste failed: ")) + T(err), true); return; }
    }
    SelectObjects(tp, pasted, keepGid);
    CommitIfDirty("Paste");
    ScrollToRect(tp, SelectionBounds());
    Toasts(Fmt(T("Pasted %d object(s) onto page %d"), (int)pasted.size(), tp + 1));
}

void App::PasteAt(int page, float px, float py) {
    int hp = hoverPage; float hx = hoverPx, hy = hoverPy;
    hoverPage = page; hoverPx = px; hoverPy = py;
    PasteClipboard(true);
    hoverPage = hp; hoverPx = hx; hoverPy = hy;
}

void App::TransferSelectionToPage(int tp, float dx, float dy) {
    if (!sel.Valid(doc) || tp < 0 || tp >= doc.PageCount() || tp == sel.page) return;
    if (groupSel.size() <= 1 && sel.parent) EnsureSelectionEditable();
    int src = sel.page;
    int gid = selGroupId;
    std::vector<FPDF_PAGEOBJECT> moved;
    for (auto o : SelMembers(*this))
        if (ops::MoveObjectToPage(doc, src, o, tp, dx, dy)) moved.push_back(o);
    if (moved.empty()) { Toasts(T("Could not move to that page"), true); return; }
    SelectObjects(tp, moved, gid);
    InvalidatePage(src);
    InvalidatePage(tp);
    CommitIfDirty("Move to page");
    ScrollToRect(tp, SelectionBounds());
    Toasts(Fmt(T("Moved %d object(s) to page %d"), (int)moved.size(), tp + 1));
}

// ------------------------------------------------------- picture pieces

void App::CutPieceFromImage(int page, PRect box) {
    const ops::ObjInfo* best = nullptr;
    float bestArea = 0;
    auto leaves = ops::ListObjects(doc, page);
    for (auto& o : leaves) {
        if (o.type != FPDF_PAGEOBJ_IMAGE || !o.bounds.Intersects(box)) continue;
        float a = o.bounds.Intersect(box).Area();
        if (a >= bestArea) { bestArea = a; best = &o; }
    }
    if (!best) {
        Toasts(T("There is no picture under that box."), true);
        return;
    }
    FPDF_PAGEOBJECT img = best->obj;
    if (best->parent) ops::PromoteObject(doc, page, img);
    std::string err;
    FPDF_PAGEOBJECT piece = ops::CutImageRegion(doc, page, img, box, err);
    if (!piece) { Toasts(std::string(T("Could not cut: ")) + T(err), true); return; }
    SelectNewObject(page, piece);
    tool = Tool::Select;
    InvalidatePage(page);
    CommitIfDirty("Cut piece from picture");
    Toasts(T("Piece lifted out of the picture"));
}

// ------------------------------------------------------------- start screen

void App::DrawWelcome(ImVec2 origin, ImVec2 region) {
    float s = dpiScale;
    const ImGuiStyle& st = ImGui::GetStyle();
    const char* title = T("PDF Editor");
    const char* sub = T("by TheOCProductions & ShortGG");
    std::string openL = std::string(ICON_OPEN "   ") + T("Open PDF");
    std::string newL = std::string(ICON_NEW "   ") + T("New document");
    const char* hint = T("or drop a PDF file here");
    ImFont* big = fontTitle ? fontTitle : ImGui::GetFont();
    ImVec2 titleSz = big->CalcTextSizeA(big->FontSize, FLT_MAX, 0.0f, title);
    float bh = ImGui::GetFrameHeight() + 10 * s;
    float bw = std::max(ImGui::CalcTextSize(openL.c_str()).x, ImGui::CalcTextSize(newL.c_str()).x) + st.FramePadding.x * 2 + 24 * s;
    bool stack = bw * 2 + st.ItemSpacing.x + 64 * s > region.x;          // narrow: buttons under each other
    float rowW = stack ? bw : bw * 2 + st.ItemSpacing.x;
    float cardW = std::min(region.x - 24 * s, std::max(titleSz.x, rowW) + 72 * s);
    float cardH = 40 * s + titleSz.y + ImGui::GetTextLineHeight() + 28 * s + (stack ? bh * 2 + st.ItemSpacing.y : bh) +
                  18 * s + ImGui::GetTextLineHeight() + 36 * s;
    ImVec2 c0(origin.x + (region.x - cardW) * 0.5f, origin.y + std::max(12 * s, (region.y - cardH) * 0.45f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(c0, ImVec2(c0.x + cardW, c0.y + cardH), ImGui::GetColorU32(theme::Panel), 14 * s);
    dl->AddRect(c0, ImVec2(c0.x + cardW, c0.y + cardH), ImGui::GetColorU32(theme::Line), 14 * s);

    float y = c0.y + 40 * s;
    ImGui::PushFont(big);
    ImGui::SetCursorScreenPos(ImVec2(c0.x + (cardW - titleSz.x) * 0.5f, y));
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    y += titleSz.y;
    float subW = ImGui::CalcTextSize(sub).x;
    ImGui::SetCursorScreenPos(ImVec2(c0.x + (cardW - subW) * 0.5f, y));
    ImGui::TextDisabled("%s", sub);
    y += ImGui::GetTextLineHeight() + 28 * s;

    float bx = c0.x + (cardW - rowW) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(bx, y));
    if (PrimaryButton(openL.c_str(), ImVec2(bw, bh))) OpenDialog();
    if (stack) { y += bh + st.ItemSpacing.y; ImGui::SetCursorScreenPos(ImVec2(bx, y)); }
    else ImGui::SameLine();
    if (ImGui::Button(newL.c_str(), ImVec2(bw, bh))) NewDoc();
    y += bh + 18 * s;
    float hw = ImGui::CalcTextSize(hint).x;
    ImGui::SetCursorScreenPos(ImVec2(c0.x + (cardW - hw) * 0.5f, y));
    ImGui::TextDisabled("%s", hint);
}

// ------------------------------------------------------- inline text editing

void App::BeginEditText(int page, FPDF_PAGEOBJECT obj) {
    if (editing) CommitInlineEdit();
    editing = true;
    editIsNew = false;
    editFocus = true;
    editPage = page;
    editObj = obj;
    editBuf = W2U(ops::EditableText(doc, page, obj));   // pieces of one text read as one
    editOriginal = editBuf;
}

// Selects everything that now shows an edited text (one object or its pieces).
static void SelectEdited(App& a, int page, const ops::TextEditInfo& ti, FPDF_PAGEOBJECT first) {
    if (ti.pieces.size() > 1) a.SelectObjects(page, ti.pieces, ti.groupId);
    else if (first) a.SelectNewObject(page, first);
}

// Appends how an edit was carried out (font name, steps, measurements - never
// the text) to edit-trace.log next to the program, for diagnosing odd PDFs.
static void LogEditTrace(const ops::TextEditInfo& ti) {
    if (ti.trace.empty()) return;
    wchar_t exe[MAX_PATH] = { 0 };
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
    std::wstring path(exe);
    size_t slash = path.find_last_of(L"/\\");
    path = (slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1)) + L"edit-trace.log";
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::string line = Fmt("%04d-%02d-%02d %02d:%02d:%02d  pieces %d, missing %d, stand-in '%s': %s",
                           st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, (int)ti.pieces.size(),
                           (int)ti.missing.size(), ti.standIn.c_str(), ti.trace.c_str());
    if (FILE* f = _wfopen(path.c_str(), L"ab")) {
        fwrite(line.data(), 1, line.size(), f);
        fwrite("\r\n", 1, 2, f);
        fclose(f);
    }
}

// Plain words for what happened when the PDF's font lacked a typed character.
static std::string FontNote(const ops::TextEditInfo& ti) {
    if (ti.standIn.empty() || ti.missing.empty()) return {};
    std::string chars;
    for (wchar_t c : ti.missing) {
        if (!chars.empty()) chars += " ";
        chars += "\"" + W2U(std::wstring(1, c)) + "\"";
    }
    std::string font = ti.fontName.empty() ? std::string(T("The PDF's font")) : ti.fontName;
    return Fmt(T("%s is not in the font %s - drawn with %s instead"), chars.c_str(), font.c_str(), ti.standIn.c_str());
}

void App::BeginNewText(int page, float px, float py) {
    if (editing) CommitInlineEdit();
    // Sample the PDF's own text nearest the click, so new text matches it.
    pendingFontValid = false;
    const ops::ObjInfo* nearest = nullptr;
    float bestd = 1e30f;
    auto leaves = ops::ListObjects(doc, page);
    for (auto& o : leaves) {
        if (o.type != FPDF_PAGEOBJ_TEXT) continue;
        float cx = (o.bounds.l + o.bounds.r) * 0.5f, cy = (o.bounds.b + o.bounds.t) * 0.5f;
        float dd = o.bounds.Contains(px, py) ? 0.0f : (cx - px) * (cx - px) + (cy - py) * (cy - py);
        if (dd < bestd) { bestd = dd; nearest = &o; }
    }
    if (nearest) {
        ops::ClipObject c;
        if (ops::CaptureObject(doc, page, nearest->obj, c)) {
            pendingStyleObj = nearest->obj;
            pendingStyleGen = doc.Generation();
            pendingFontData = c.fontData;
            pendingFontBase = c.fontBase;
            pendingFontSize = c.visualSize * ops::MatrixScale(nearest->accum);
            if (pendingFontSize < 2 || pendingFontSize > 200) pendingFontSize = fontSize;
            pendingColor = (c.fr << 16) | (c.fg << 8) | c.fb;
            pendingFontValid = true;
        }
    }
    double size = (matchNearbyFont && pendingFontValid) ? pendingFontSize : fontSize;
    editing = true;
    editIsNew = true;
    editFocus = true;
    editPage = page;
    editObj = nullptr;
    editAtX = px;
    editAtY = py - (float)size * 0.8f;   // the text's top lands where you clicked
    editBuf.clear();
    editOriginal.clear();
}

void App::CommitInlineEdit() {
    if (!editing) return;
    editing = false;
    std::wstring w = U2W(editBuf);
    while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n')) w.pop_back();
    bool blank = std::all_of(w.begin(), w.end(), [](wchar_t c) { return iswspace(c) != 0; });
    if (editIsNew) {
        if (w.empty() || blank) return;
        bool match = matchNearbyFont && pendingFontValid;
        double size = match ? pendingFontSize : fontSize;
        unsigned r = match ? (pendingColor >> 16) & 255 : (unsigned)(fillColor[0] * 255);
        unsigned g = match ? (pendingColor >> 8) & 255 : (unsigned)(fillColor[1] * 255);
        unsigned b = match ? pendingColor & 255 : (unsigned)(fillColor[2] * 255);
        std::string err;
        FPDF_PAGEOBJECT o = nullptr;
        ops::TextEditInfo ti;
        if (match) {
            FPDF_PAGEOBJECT style = (pendingStyleGen == doc.Generation() && ops::ObjectExists(doc, editPage, pendingStyleObj))
                                        ? pendingStyleObj : nullptr;
            o = ops::AddTextLike(doc, editPage, w, style, pendingFontData, pendingFontBase, size, editAtX, editAtY, r, g, b,
                                 err, &ti);
        }
        if (!o) o = AddTextInFont(editPage, w, size, editAtX, editAtY, r, g, b, err, &ti);
        if (o) {
            SelectEdited(*this, editPage, ti, o);
            InvalidatePage(editPage);
            CommitIfDirty("Add text");
            std::string note = FontNote(ti);
            if (!note.empty()) Toasts(note);
        } else {
            Toasts(std::string(T("Could not add the text: ")) + T(err), true);
        }
    } else if (editObj) {
        FPDF_PAGEOBJECT obj = editObj;
        editObj = nullptr;
        if (editBuf == editOriginal) return;          // unchanged: leave the object exactly as it was
        if (!ops::ObjectExists(doc, editPage, obj)) return;
        if (blank) {
            std::vector<FPDF_PAGEOBJECT> all = ops::TextPieces(doc, editPage, obj);
            if (all.empty()) all = { obj };
            bool any = false;
            for (FPDF_PAGEOBJECT o : all) any |= ops::DeleteObjectNested(doc, editPage, o, nullptr);
            if (any) {
                ClearSelection();
                CommitIfDirty("Delete text");
            }
            return;
        }
        FPDF_PAGEOBJECT repl = nullptr;
        bool fallback = false;
        ops::TextEditInfo ti;
        bool okEdit = ops::SetObjectTextSafe(doc, editPage, obj, w, &repl, &fallback, &ti);
        LogEditTrace(ti);
        if (okEdit) {
            SelectEdited(*this, editPage, ti, repl);
            InvalidatePage(editPage);
            CommitIfDirty("Edit text");
            std::string note = FontNote(ti);
            if (!note.empty()) Toasts(note);
        } else {
            Toasts(T("Could not change this text"), true);
        }
    }
    editObj = nullptr;
}

void App::CancelInlineEdit() {
    editing = false;
    editObj = nullptr;
}

// ------------------------------------------------------------------- forms

static FPDF_ANNOTATION FormFieldAt(App& a, int page, float px, float py) {
    if (!a.doc.Form()) return nullptr;
    FPDF_PAGE p = a.doc.Page(page);
    if (!p) return nullptr;
    FS_POINTF pt{ px, py };
    return FPDFAnnot_GetFormFieldAtPoint(a.doc.Form(), p, &pt);
}

void App::ForwardFormKey(int vk, bool down) {
    if (formPage < 0 || !doc.Form()) return;
    FPDF_PAGE p = doc.Page(formPage);
    if (!p) return;
    if (down) FORM_OnKeyDown(doc.Form(), p, vk, 0);
    else FORM_OnKeyUp(doc.Form(), p, vk, 0);
    InvalidatePage(formPage);
}

void App::ForwardFormChar(unsigned int ch) {
    if (formPage < 0 || !doc.Form()) return;
    FPDF_PAGE p = doc.Page(formPage);
    if (!p) return;
    FORM_OnChar(doc.Form(), p, (int)ch, 0);
    InvalidatePage(formPage);
}

void App::EndFormFocus() {
    if (formPage < 0) return;
    doc.KillFormFocus();
    InvalidatePage(formPage);
    formPage = -1;
    if (doc.formDirty) CommitIfDirty("Fill form");    // typing into a field becomes one undo step
}

// --------------------------------------------------------------- rendering

// Renders page i (or its visible part at high zoom) into its texture if stale.
static void EnsurePageTexture(App& a, int i, ImVec2 tl, float dispW, float dispH, ImVec2 vp0, ImVec2 vp1) {
    PageTex& c = a.cache[i];
    uint32_t v = a.doc.PageVersion(i);
    float scale = a.zoom;
    int fw = std::max(1, (int)std::lround(dispW)), fh = std::max(1, (int)std::lround(dispH));
    int tx = 0, ty = 0, tw = fw, th = fh;
    bool same = c.tex.Valid() && c.version == v && std::fabs(c.scale - scale) < 1e-4f;
    if ((int64_t)fw * fh > kMaxFullPixels || fw > kMaxTexDim || fh > kMaxTexDim) {
        int nx0 = std::max(0, (int)(vp0.x - tl.x)), ny0 = std::max(0, (int)(vp0.y - tl.y));
        int nx1 = std::min(fw, (int)std::ceil(vp1.x - tl.x)), ny1 = std::min(fh, (int)std::ceil(vp1.y - tl.y));
        if (nx1 <= nx0 || ny1 <= ny0) return;
        if (same && c.tx <= nx0 && c.ty <= ny0 && c.tx + c.tw >= nx1 && c.ty + c.th >= ny1) return;
        const int margin = 256;
        tx = std::max(0, nx0 - margin); ty = std::max(0, ny0 - margin);
        tw = std::min({ fw - tx, nx1 - nx0 + 2 * margin, kMaxTexDim });
        th = std::min({ fh - ty, ny1 - ny0 + 2 * margin, kMaxTexDim });
    } else if (same && c.tx == 0 && c.ty == 0 && c.tw == fw && c.th == fh) {
        return;
    }
    std::vector<uint32_t> buf;
    if (a.doc.Render(i, scale, tx, ty, tw, th, buf, true, true)) {
        c.tex.Upload(buf.data(), tw, th);
        c.version = v; c.scale = scale;
        c.tx = tx; c.ty = ty; c.tw = tw; c.th = th;
    }
}

void App::DrawCanvas() {
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 region = ImGui::GetContentRegionAvail();
    canvasOrigin = origin;
    canvasSize = region;
    if (region.x < 10 || region.y < 10) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + region.x, origin.y + region.y), theme::CanvasBg);

    int n = doc.PageCount();
    if (n == 0) {
        DrawWelcome(origin, region);
        hoverPage = -1;
        return;
    }
    if ((int)cache.size() != n) cache.resize(n);
    if ((int)thumbs.size() != n) thumbs.resize(n);

    std::vector<float> dispW(n), dispH(n);
    auto layout = [&](float& totalH, float& maxW) {
        totalH = kPad; maxW = 0;
        for (int i = 0; i < n; ++i) {
            FS_SIZEF sz = doc.PageSizePt(i);
            dispW[i] = sz.width * zoom;
            dispH[i] = sz.height * zoom;
            totalH += dispH[i] + kGap;
            maxW = std::max(maxW, dispW[i]);
        }
        totalH += kPad - kGap;
    };
    float totalH, maxContentW;
    layout(totalH, maxContentW);
    if (fitPending && maxContentW > 0) {
        zoom = std::clamp((region.x - 40) / (maxContentW / zoom), 0.1f, 6.0f);
        fitPending = false;
        layout(totalH, maxContentW);
    }

    ImGui::InvisibleButton("##canvasHit", region,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
                           ImGuiButtonFlags_MouseButtonRight);
    bool hovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();

    auto pageTop = [&](int p) { float y = kPad; for (int i = 0; i < p; ++i) y += dispH[i] + kGap; return y; };
    if (pendingScrollPage >= 0 && pendingScrollPage < n) {
        scrollY = pageTop(pendingScrollPage) - 20;
        pendingScrollPage = -1;
    }
    if (pendingScrollRectPage >= 0 && pendingScrollRectPage < n) {
        float dx, dy;
        doc.PageToDev(pendingScrollRectPage, zoom, pendingScrollRect.l, pendingScrollRect.t, dx, dy);
        float y = pageTop(pendingScrollRectPage) + dy;
        float top = y - scrollY;
        if (top < 20 || top > region.y - 60) scrollY = y - region.y * 0.3f;
        pendingScrollRectPage = -1;
    }

    float maxScroll = std::max(0.0f, totalH - region.y);
    float maxScrollX = std::max(0.0f, maxContentW - (region.x - 40.0f));

    if (hovered && io.MouseWheel != 0) {
        if (io.KeyCtrl) {
            float oldZoom = zoom;
            zoom = std::clamp(zoom * (io.MouseWheel > 0 ? 1.1f : 1.0f / 1.1f), 0.1f, 8.0f);
            float cy = io.MousePos.y - origin.y + scrollY;
            scrollY += cy * (zoom / oldZoom - 1.0f);
            float cx = io.MousePos.x - origin.x + scrollX;
            scrollX += cx * (zoom / oldZoom - 1.0f);
            InvalidateAll();
            layout(totalH, maxContentW);
            maxScroll = std::max(0.0f, totalH - region.y);
            maxScrollX = std::max(0.0f, maxContentW - (region.x - 40.0f));
        } else if (io.KeyShift) {
            scrollX -= io.MouseWheel * 80.0f * dpiScale;
        } else {
            scrollY -= io.MouseWheel * 80.0f * dpiScale;
        }
    }
    if (hovered && io.MouseWheelH != 0) scrollX -= io.MouseWheelH * 80.0f * dpiScale;
    bool panning = (tool == Tool::Hand && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) ||
                   (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle));
    if (panning) { scrollY -= io.MouseDelta.y; scrollX -= io.MouseDelta.x; }
    // Auto-scroll while dragging near an edge (e.g. carrying an object to another page).
    if (dragging && dragMode != 4) {
        float m = io.MousePos.y, edge = 40.0f * dpiScale, speed = 14.0f * dpiScale;
        if (m < origin.y + edge) scrollY -= speed * (1.0f - std::max(0.0f, m - origin.y) / edge);
        else if (m > origin.y + region.y - edge) scrollY += speed * (1.0f - std::max(0.0f, origin.y + region.y - m) / edge);
        float mx = io.MousePos.x;
        if (maxScrollX > 0) {
            if (mx < origin.x + edge) scrollX -= speed * 0.7f;
            else if (mx > origin.x + region.x - edge) scrollX += speed * 0.7f;
        }
    }
    scrollY = std::clamp(scrollY, 0.0f, maxScroll);
    scrollX = std::clamp(scrollX, 0.0f, maxScrollX);

    // --- lay out + draw pages ---
    ImVec2 vp0 = origin, vp1(origin.x + region.x, origin.y + region.y);
    float baseX = origin.x + std::max(20.0f, (region.x - maxContentW) / 2) - scrollX;
    float y = origin.y + kPad - scrollY;
    int firstVis = -1, lastVis = -1;
    float bestVisH = -1;
    for (int i = 0; i < n; ++i) {
        PageTex& c = cache[i];
        c.laid = false;
        float x = baseX + (maxContentW - dispW[i]) / 2;
        ImVec2 tl(x, y), br(x + dispW[i], y + dispH[i]);
        bool visible = br.y >= vp0.y && tl.y <= vp1.y;
        if (visible) {
            if (firstVis < 0) firstVis = i;
            lastVis = i;
            float visH = std::min(br.y, vp1.y) - std::max(tl.y, vp0.y);
            if (visH > bestVisH) { bestVisH = visH; currentPage = i; }
            c.topLeft = tl; c.dispW = dispW[i]; c.dispH = dispH[i]; c.laid = true;
            EnsurePageTexture(*this, i, tl, dispW[i], dispH[i], vp0, vp1);
            dl->AddRectFilled(ImVec2(tl.x + 3, tl.y + 3), ImVec2(br.x + 3, br.y + 3), IM_COL32(0, 0, 0, 90));
            dl->AddRectFilled(tl, br, IM_COL32(255, 255, 255, 255));
            if (c.tex.Valid())
                dl->AddImage(c.tex.Id(), ImVec2(tl.x + c.tx, tl.y + c.ty), ImVec2(tl.x + c.tx + c.tw, tl.y + c.ty + c.th));
            dl->AddRect(tl, br, IM_COL32(70, 70, 80, 255));
            char tag[32];
            snprintf(tag, sizeof(tag), "%d", i + 1);
            dl->AddText(ImVec2(tl.x + 4, tl.y - 16 * dpiScale), IM_COL32(170, 170, 180, 255), tag);
            for (int h = 0; h < (int)searchHits.size(); ++h) {
                if (searchHits[h].page != i) continue;
                ImVec2 a0, a1;
                ScreenBox(*this, i, searchHits[h].box, a0, a1);
                dl->AddRectFilled(a0, a1, h == searchCurrent ? IM_COL32(255, 170, 40, 110) : IM_COL32(255, 230, 0, 70));
            }
        }
        y += dispH[i] + kGap;
    }
    doc.currentPageHint = currentPage;
    // Free textures of pages far from the view (keeps GPU memory flat on long documents).
    for (int i = 0; i < n; ++i)
        if (!cache[i].laid && (i < firstVis - 2 || i > lastVis + 2) && cache[i].tex.Valid()) {
            cache[i].tex.Release();
            cache[i].version = ~0u;
        }

    // --- pointer position on the page ---
    hoverPage = -1;
    if (hovered) {
        int hp; float hx, hy;
        if (ScreenToPage(io.MousePos, hp, hx, hy)) { hoverPage = hp; hoverPx = hx; hoverPy = hy; }
    }

    float hs = 4.0f * dpiScale;
    // --- annotation selection ---
    if (AnnotSelected() && annSelPage < n && cache[annSelPage].laid) {
        PRect r;
        if (ops::AnnotRect(doc, annSelPage, annSelIdx, r)) {
            ImVec2 p0, p1;
            ScreenBox(*this, annSelPage, r, p0, p1);
            dl->AddRect(ImVec2(p0.x - 2, p0.y - 2), ImVec2(p1.x + 2, p1.y + 2), IM_COL32(255, 150, 40, 255), 0, 0, 2.0f);
        } else {
            ClearAnnotSel();
        }
    }
    // --- object selection ---
    if (sel.Valid(doc) && sel.page >= 0 && sel.page < n && cache[sel.page].laid) {
        bool grp = groupSel.size() > 1;
        if (grp)
            for (auto o : groupSel) {
                ImVec2 m0, m1;
                ScreenBox(*this, sel.page, ops::ObjectBounds(o), m0, m1);
                dl->AddRect(m0, m1, IM_COL32(60, 150, 255, 120));
            }
        PRect b = SelectionBounds();
        sel.bounds = b;
        ImVec2 p0, p1;
        ScreenBox(*this, sel.page, b, p0, p1);
        p0.x -= 1; p0.y -= 1; p1.x += 1; p1.y += 1;
        ImU32 boxCol = selGroupId ? IM_COL32(70, 200, 120, 255) : IM_COL32(60, 150, 255, 255);
        dl->AddRect(p0, p1, boxCol, 0, 0, grp ? 2.0f : 1.5f);
        if (tool == Tool::Select && !editing) {
            ImVec2 h[8];
            HandlePoints(p0, p1, h);
            for (int i = 0; i < 8; ++i) {
                dl->AddRectFilled(ImVec2(h[i].x - hs, h[i].y - hs), ImVec2(h[i].x + hs, h[i].y + hs), IM_COL32(255, 255, 255, 255));
                dl->AddRect(ImVec2(h[i].x - hs, h[i].y - hs), ImVec2(h[i].x + hs, h[i].y + hs), boxCol);
            }
        }
    }
    // --- guide lines through selected text (when switched on) ---
    DrawSelectionGuides(dl);
    // --- alignment guides while moving text ---
    if (dragging && dragMode == 1 && dragArmed && sel.Valid(doc) && sel.page < n && cache[sel.page].laid &&
        (!guideX.empty() || !guideY.empty())) {
        PRect pb = doc.PageBox(sel.page);
        ImU32 gc = IM_COL32(255, 64, 170, 235);
        float w = std::max(1.0f, 1.25f * dpiScale);
        for (float x : guideX) {
            ImVec2 p0, p1;
            ScreenBox(*this, sel.page, PRect{ x, pb.b, x, pb.t }, p0, p1);
            dl->AddLine(ImVec2(p0.x, p0.y), ImVec2(p0.x, p1.y), gc, w);
        }
        for (float y : guideY) {
            ImVec2 p0, p1;
            ScreenBox(*this, sel.page, PRect{ pb.l, y, pb.r, y }, p0, p1);
            dl->AddLine(ImVec2(p0.x, p0.y), ImVec2(p1.x, p0.y), gc, w);
        }
    }
    // --- drop preview on another page ---
    if (dragging && dragMode == 1 && ghostPage >= 0 && ghostPage < n && cache[ghostPage].laid) {
        ImVec2 p0, p1;
        ScreenBox(*this, ghostPage, ghostRect, p0, p1);
        dl->AddRectFilled(p0, p1, IM_COL32(60, 150, 255, 40));
        dl->AddRect(p0, p1, IM_COL32(60, 150, 255, 255), 0, 0, 2.0f);
        dl->AddText(ImVec2(p0.x, p1.y + 2), IM_COL32(60, 150, 255, 255), Fmt(T("drop on page %d"), ghostPage + 1).c_str());
    }
    // --- rubber-band previews ---
    auto boxPreview = [&](int pg, PRect r, ImU32 fill, ImU32 line) {
        if (pg < 0 || pg >= n || !cache[pg].laid) return;
        ImVec2 p0, p1;
        ScreenBox(*this, pg, r, p0, p1);
        if (fill) dl->AddRectFilled(p0, p1, fill);
        dl->AddRect(p0, p1, line, 0, 0, 1.5f);
    };
    if (dragging && dragMode == 5) boxPreview(dragPage, marquee, IM_COL32(60, 150, 255, 30), IM_COL32(60, 150, 255, 200));
    if (dragging && dragMode == 3) boxPreview(dragPage, dragRect, IM_COL32(0, 0, 0, 160), IM_COL32(255, 60, 60, 255));
    if (dragging && dragMode == 7) boxPreview(dragPage, dragRect, IM_COL32(255, 200, 0, 40), IM_COL32(255, 170, 0, 255));
    if (dragging && dragMode == 2) boxPreview(dragPage, dragRect, rectFilled ? Col(fillColor) : 0, Col(strokeColor));

    HandleCanvasInput(hovered, origin, region);
}

// ------------------------------------------------------------- input

void App::HandleCanvasInput(bool hovered, ImVec2 origin, ImVec2 region) {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mouse = io.MousePos;
    bool typing = io.WantTextInput;       // the search box, an inline edit, a panel field...

    if (!sel.Valid(doc) && !groupSel.empty()) { groupSel.clear(); selGroupId = 0; }

    // Keys for a focused form field.
    if (formPage >= 0 && doc.Form() && !typing) {
        for (ImWchar ch : io.InputQueueCharacters)
            if (ch >= 32) ForwardFormChar(ch);
        struct { ImGuiKey k; int vk; } keys[] = {
            { ImGuiKey_Backspace, VK_BACK }, { ImGuiKey_Delete, VK_DELETE },
            { ImGuiKey_LeftArrow, VK_LEFT }, { ImGuiKey_RightArrow, VK_RIGHT },
            { ImGuiKey_UpArrow, VK_UP }, { ImGuiKey_DownArrow, VK_DOWN },
            { ImGuiKey_Home, VK_HOME }, { ImGuiKey_End, VK_END },
            { ImGuiKey_Enter, VK_RETURN }, { ImGuiKey_Tab, VK_TAB },
        };
        for (auto& e : keys)
            if (ImGui::IsKeyPressed(e.k, true)) { ForwardFormKey(e.vk, true); ForwardFormKey(e.vk, false); }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) EndFormFocus();
    }

    // ---- keyboard on the selection (never while typing somewhere) ----
    bool keysFree = !editing && !typing && formPage < 0;
    bool haveSel = sel.Valid(doc) || AnnotSelected();
    if (keysFree && haveSel) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) DeleteSelection();
        float step = io.KeyShift ? 10.0f : (io.KeyCtrl ? 0.25f : 1.0f);
        bool alt = io.KeyAlt && !AnnotSelected();
        auto nudge = [&](float dx, float dy) {
            if (alt) {
                PRect b = SelectionBounds();
                PRect t{ b.l, b.b, std::max(b.l + 1, b.r + dx), std::max(b.b + 1, b.t + dy) };
                ResizeSelectionTo(t);
            } else {
                MoveSelection(dx, dy);
            }
            nudgeDirty = true;
        };
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))  nudge(-step, 0);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) nudge(+step, 0);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))    nudge(0, +step);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))  nudge(0, -step);
        if ((ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_F2)) && sel.Valid(doc) &&
            groupSel.size() <= 1 && ops::ObjectType(sel.obj) == FPDF_PAGEOBJ_TEXT) {
            EnsureSelectionEditable();
            BeginEditText(sel.page, sel.obj);
        }
    }
    bool arrowsDown = ImGui::IsKeyDown(ImGuiKey_LeftArrow) || ImGui::IsKeyDown(ImGuiKey_RightArrow) ||
                      ImGui::IsKeyDown(ImGuiKey_UpArrow) || ImGui::IsKeyDown(ImGuiKey_DownArrow);
    if (nudgeDirty && !arrowsDown) { CommitIfDirty("Nudge"); nudgeDirty = false; }
    if (keysFree) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            if (dragging) { dragging = false; dragMode = 0; ghostPage = -1; }
            ClearSelection();
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A)) SelectAllOnPage(io.KeyShift);
        if (hovered || ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
            float pageStep = region.y * 0.9f;
            if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) scrollY += pageStep;
            if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) scrollY -= pageStep;
            if (!haveSel && ImGui::IsKeyPressed(ImGuiKey_Home)) scrollY = 0;
            if (!haveSel && ImGui::IsKeyPressed(ImGuiKey_End)) scrollY = 1e9f;
        }
    }

    int page; float px, py;
    bool over = hovered && ScreenToPage(mouse, page, px, py);
    float tolPt = 5.0f * dpiScale / std::max(0.05f, zoom);

    // ---- hover feedback: cursor shape ----
    if (hovered && !dragging && tool == Tool::Select && sel.Valid(doc) && sel.page < (int)cache.size() &&
        cache[sel.page].laid && !editing) {
        ImVec2 p0, p1, h[8];
        ScreenBox(*this, sel.page, SelectionBounds(), p0, p1);
        HandlePoints(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1), h);
        int hh = HandleAt(h, mouse, 6.0f * dpiScale);
        if (hh >= 0) ImGui::SetMouseCursor(HandleCursor(hh));
        else if (mouse.x >= p0.x && mouse.x <= p1.x && mouse.y >= p0.y && mouse.y <= p1.y)
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }
    if (hovered && over && tool == Tool::Text) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    if (hovered && tool == Tool::Hand) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    // ---- left button down ----
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !dragging) {
        if (editing) CommitInlineEdit();
        bool handled = false;
        // 1. a resize handle of the current selection
        if (tool == Tool::Select && sel.Valid(doc) && sel.page < (int)cache.size() && cache[sel.page].laid) {
            ImVec2 p0, p1, h[8];
            ScreenBox(*this, sel.page, SelectionBounds(), p0, p1);
            HandlePoints(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1), h);
            int hi = HandleAt(h, mouse, 6.0f * dpiScale);
            if (hi >= 0) {
                if (groupSel.size() <= 1) EnsureSelectionEditable();
                dragging = true; dragMode = 4; dragArmed = false; dragPage = sel.page;
                dragStartScreen = mouse; resizeHandle = hi; resizeStart = SelectionBounds();
                handled = true;
            }
        }
        if (!handled && over) {
            FPDF_ANNOTATION widget = (tool == Tool::Select && !io.KeyAlt) ? FormFieldAt(*this, page, px, py) : nullptr;
            if (widget) FPDFPage_CloseAnnot(widget);
            if (widget && doc.Form()) {
                // Form field: hand the click to the form engine (focus, checkboxes...).
                FORM_OnLButtonDown(doc.Form(), doc.Page(page), 0, px, py);
                FORM_OnLButtonUp(doc.Form(), doc.Page(page), 0, px, py);
                formPage = page;
                doc.currentPageHint = page;
                ClearSelection();
                InvalidatePage(page);
            } else {
                EndFormFocus();
                switch (tool) {
                case Tool::Select: {
                    bool insideGroup = !io.KeyAlt && groupSel.size() > 1 && sel.Valid(doc) && sel.page == page &&
                                       SelectionBounds().Contains(px, py);
                    if (!insideGroup) {
                        SelectObjectAt(page, px, py);
                        if (sel.Valid(doc)) {
                            if (!io.KeyAlt) RefreshGroupSel(); else { groupSel = { sel.obj }; selGroupId = 0; }
                        } else if (auto an = ops::AnnotAt(doc, page, px, py, tolPt, io.KeyAlt)) {
                            ClearSelection();
                            annSelPage = page; annSelIdx = an->index;
                        }
                    }
                    if (sel.Valid(doc) || AnnotSelected()) {
                        dragging = true; dragArmed = false;
                        dragMode = AnnotSelected() ? 6 : 1;
                        dragPage = page; dragStartScreen = mouse;
                        dragLastPx = px; dragLastPy = py;
                        dragStartPx = px; dragStartPy = py;
                        PRect sb = SelectionBounds();
                        grabDX = px - sb.l; grabDY = py - sb.b;
                        moveStart = sb;
                        if (dragMode == 1) PrepareAlign();
                    } else {
                        ClearSelection();
                        dragging = true; dragMode = 5; dragPage = page;
                        dragStartPx = px; dragStartPy = py;
                        marquee = { px, py, px, py };
                    }
                    break;
                }
                case Tool::Text: {
                    auto hit = ops::HitTest(doc, page, px, py, tolPt);
                    if (hit && hit->type == FPDF_PAGEOBJ_TEXT) {      // click existing text = edit it
                        sel.Set(doc, page, *hit);
                        EnsureSelectionEditable();
                        groupSel = { sel.obj };
                        BeginEditText(page, sel.obj);
                    } else {
                        ClearSelection();
                        BeginNewText(page, px, py);
                    }
                    break;
                }
                case Tool::Image: {
                    std::wstring f;
                    if (OpenFileDialog(hwnd_, { { TW("Images"), L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.webp" },
                                                { TW("All files"), L"*.*" } }, f)) {
                        Image img; std::string err;
                        if (LoadImageFile(f, img, err)) {
                            FS_SIZEF sz = doc.PageSizePt(page);
                            float w = std::min(sz.width * 0.5f, (float)img.w * 0.75f);
                            if (FPDF_PAGEOBJECT o = ops::AddImage(doc, page, img, px, py - w * img.h / img.w, w, err)) {
                                SelectNewObject(page, o);
                                CommitIfDirty("Add image");
                                Toasts(T("Image added"));
                            } else Toasts(std::string(T("Add image failed: ")) + T(err), true);
                        } else Toasts(std::string(T("Could not load image: ")) + T(err), true);
                    }
                    break;
                }
                case Tool::Rect:
                case Tool::Redact:
                case Tool::Cut:
                    dragging = true;
                    dragMode = tool == Tool::Rect ? 2 : tool == Tool::Redact ? 3 : 7;
                    dragPage = page; dragStartPx = px; dragStartPy = py;
                    dragRect = { px, py, px, py };
                    break;
                default: break;
                }
            }
        } else if (!handled && tool == Tool::Select) {
            ClearSelection();        // clicked the grey background
        }
    }

    // ---- right button: select what's under the cursor, then the context menu ----
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (editing) CommitInlineEdit();
        if (over) {
            bool inSel = sel.Valid(doc) && sel.page == page && SelectionBounds().Contains(px, py) && groupSel.size() > 1;
            if (!inSel) {
                auto hit = ops::HitTest(doc, page, px, py, tolPt);
                if (hit) {
                    ClearAnnotSel();
                    sel.Set(doc, page, *hit);
                    if (!io.KeyAlt) RefreshGroupSel(); else { groupSel = { sel.obj }; selGroupId = 0; }
                } else if (auto an = ops::AnnotAt(doc, page, px, py, tolPt, true)) {
                    ClearSelection();
                    annSelPage = page; annSelIdx = an->index;
                } else {
                    ClearSelection();
                }
            }
            ctxPage = page; ctxPx = px; ctxPy = py;
            openObjCtx = true;
        }
    }

    // ---- dragging ----
    if (dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        int p2 = -1; float nx = 0, ny = 0;
        bool onPage = ScreenToPage(mouse, p2, nx, ny);
        if ((dragMode == 1 || dragMode == 6 || dragMode == 4) && !dragArmed && Dist(mouse, dragStartScreen) > 4.0f * dpiScale) {
            dragArmed = true;     // a real drag, not the jitter of a click / double-click
        }
        if (dragMode == 1 && dragArmed && sel.Valid(doc)) {
            if (onPage && p2 == dragPage) {
                ghostPage = -1;
                float rdx = nx - dragStartPx, rdy = ny - dragStartPy;
                SnapMove(rdx, rdy, io.KeyAlt);          // Alt = place freely
                PRect cb = SelectionBounds();
                float ddx = moveStart.l + rdx - cb.l, ddy = moveStart.b + rdy - cb.b;
                if (std::fabs(ddx) > 1e-4f || std::fabs(ddy) > 1e-4f) MoveSelection(ddx, ddy);
                dragLastPx = nx; dragLastPy = ny;
            } else if (onPage) {
                guideX.clear(); guideY.clear();
                PRect b = SelectionBounds();
                ghostPage = p2;
                ghostRect = { nx - grabDX, ny - grabDY, nx - grabDX + b.W(), ny - grabDY + b.H() };
            } else {
                ghostPage = -1;
            }
        } else if (dragMode == 6 && dragArmed && AnnotSelected()) {
            if (onPage && p2 == dragPage) {
                float ddx = nx - dragLastPx, ddy = ny - dragLastPy;
                if (ddx != 0 || ddy != 0) { MoveSelection(ddx, ddy); dragLastPx = nx; dragLastPy = ny; }
            }
        } else if (dragMode == 4 && dragArmed && sel.Valid(doc)) {
            if (onPage && p2 == dragPage) {
                const float MIN = 1.0f;
                PRect t = resizeStart;
                bool L = resizeHandle == 0 || resizeHandle == 6 || resizeHandle == 7;
                bool R = resizeHandle == 2 || resizeHandle == 3 || resizeHandle == 4;
                bool T = resizeHandle == 0 || resizeHandle == 1 || resizeHandle == 2;
                bool B = resizeHandle == 4 || resizeHandle == 5 || resizeHandle == 6;
                if (L) t.l = std::min(nx, resizeStart.r - MIN);
                if (R) t.r = std::max(nx, resizeStart.l + MIN);
                if (T) t.t = std::max(ny, resizeStart.b + MIN);
                if (B) t.b = std::min(ny, resizeStart.t - MIN);
                ResizeSelectionTo(t);
            }
        } else if (dragMode == 5) {
            if (onPage && p2 == dragPage) marquee = PRect::FromPoints(dragStartPx, dragStartPy, nx, ny);
        } else if (dragMode == 2 || dragMode == 3 || dragMode == 7) {
            if (onPage && p2 == dragPage) dragRect = PRect::FromPoints(dragStartPx, dragStartPy, nx, ny);
        }
    }

    // ---- release ----
    if (dragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (dragMode == 1 && dragArmed) {
            if (ghostPage >= 0 && ghostPage != dragPage) {
                PRect b = SelectionBounds();
                TransferSelectionToPage(ghostPage, ghostRect.l - b.l, ghostRect.b - b.b);
            } else if (sel.Valid(doc)) {
                CommitIfDirty(groupSel.size() > 1 ? "Move group" : "Move");
            }
        } else if (dragMode == 6 && dragArmed) {
            CommitIfDirty("Move annotation");
        } else if (dragMode == 4 && dragArmed) {
            if (sel.Valid(doc)) CommitIfDirty("Resize");
        } else if (dragMode == 5) {
            float minPt = 3.0f / std::max(0.05f, zoom);
            if (marquee.W() > minPt || marquee.H() > minPt) SelectLeaves(*this, dragPage, io.KeyShift, &marquee);
        } else if (dragMode == 2 && dragRect.Area() > 4) {
            FPDF_PAGEOBJECT o = ops::AddRect(doc, dragPage, dragRect, rectFilled,
                                             (unsigned)(fillColor[0] * 255), (unsigned)(fillColor[1] * 255), (unsigned)(fillColor[2] * 255),
                                             rectStroked, (unsigned)(strokeColor[0] * 255), (unsigned)(strokeColor[1] * 255),
                                             (unsigned)(strokeColor[2] * 255), strokeWidth);
            if (o) { SelectNewObject(dragPage, o); CommitIfDirty("Add rectangle"); }
        } else if (dragMode == 3 && dragRect.Area() > 4) {
            ClearSelection();
            ops::RedactResult r = ops::Redact(doc, dragPage, dragRect, 0, 0, 0);
            InvalidatePage(dragPage);
            CommitIfDirty("Redact");
            Toasts(Fmt(T("Redacted: %d removed, %d text line(s) cut, %d picture(s) blacked out, %d annotation(s) removed"),
                       r.removed, r.textSplit, r.imagesMasked, r.annotsRemoved));
        } else if (dragMode == 7 && dragRect.Area() > 4) {
            CutPieceFromImage(dragPage, dragRect);
        }
        dragging = false; dragArmed = false; dragMode = 0; dragPage = -1; ghostPage = -1; resizeHandle = -1;
        guideX.clear(); guideY.clear(); alignCands.clear(); alignOn = false;
    }

    // ---- double-click: type into text, or pick out one line ----
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && over && tool == Tool::Select && !io.KeyAlt) {
        auto hit = ops::HitTest(doc, page, px, py, tolPt);
        if (hit && hit->type == FPDF_PAGEOBJ_TEXT) {
            dragging = false; dragMode = 0;
            sel.Set(doc, page, *hit);
            EnsureSelectionEditable();
            groupSel = { sel.obj }; selGroupId = 0;
            BeginEditText(page, sel.obj);
        } else if (hit) {
            dragging = false; dragMode = 0;
            sel.Set(doc, page, *hit);
            groupSel = { sel.obj }; selGroupId = 0;
            if (hit->type == FPDF_PAGEOBJ_PATH && ops::SplittableParts(hit->obj) > 1)
                SplitSelectedPath(true, px, py);
            else if (ops::GetGroupId(sel.obj))
                Toasts(T("Editing one piece of the group"));
        }
    }

    // ---- context menu ----
    if (openObjCtx) { ImGui::OpenPopup("##objctx"); openObjCtx = false; }
    if (ImGui::BeginPopup("##objctx")) {
        bool any = false;
        if (AnnotSelected()) {
            ImGui::TextDisabled("%s", T("Annotation"));
            if (ImGui::MenuItem(T("Delete annotation"), K("Del"))) DeleteSelection();
            if (ImGui::MenuItem(T("Make this page's annotations editable"))) FlattenCurrentPage();
            any = true;
        } else if (sel.Valid(doc)) {
            bool grouped = selGroupId != 0, multi = groupSel.size() > 1;
            int t = ops::ObjectType(sel.obj);
            bool isForm = !sel.parent && t == FPDF_PAGEOBJ_FORM;
            int parts = (!multi && t == FPDF_PAGEOBJ_PATH) ? ops::SplittableParts(sel.obj) : 0;
            if (!multi && t == FPDF_PAGEOBJ_TEXT && ImGui::MenuItem(T("Edit text"), K("Enter / double-click"))) {
                EnsureSelectionEditable();
                BeginEditText(sel.page, sel.obj);
            }
            if (multi && !grouped && ImGui::MenuItem(Fmt(T("Group these %d into one"), (int)groupSel.size()).c_str())) GroupSelection();
            if (grouped) {
                if (ImGui::MenuItem(T("Ungroup"))) UngroupSelection();
            }
            if (isForm && ImGui::MenuItem(T("Break apart (ungroup)"))) UngroupSelection();
            if (parts > 1 && ImGui::MenuItem(Fmt(T("Split into %d separate lines"), parts).c_str())) SplitSelectedPath(false, 0, 0);
            if (!multi && t == FPDF_PAGEOBJ_IMAGE && ImGui::MenuItem(T("Cut piece tool"))) tool = Tool::Cut;
            ImGui::Separator();
            if (ImGui::MenuItem(T("Copy"), K("Ctrl+C"))) CopySelection();
            if (ImGui::MenuItem(T("Cut"), K("Ctrl+X"))) CutSelection();
            if (doc.PageCount() > 1 && ImGui::BeginMenu(T("Move to page"))) {
                for (int i = 0; i < doc.PageCount(); ++i) {
                    if (i == sel.page) continue;
                    if (ImGui::MenuItem(Fmt(T("Page %d"), i + 1).c_str())) { TransferSelectionToPage(i); break; }
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(T("Delete"), K("Del"))) DeleteSelection();
            any = true;
        }
        if (any) ImGui::Separator();
        const char* cbt = ImGui::GetClipboardText();
        bool canPaste = !clip.empty() || !limbo.empty() || (cbt && *cbt);
        if (ImGui::MenuItem(T("Paste here"), K("Ctrl+Shift+V"), false, canPaste && ctxPage >= 0)) PasteAt(ctxPage, ctxPx, ctxPy);
        if (ImGui::MenuItem(T("Select all on this page"), K("Ctrl+A"))) SelectAllOnPage(false);
        if (ImGui::MenuItem(T("Select all lines on this page"), K("Ctrl+Shift+A"))) SelectAllOnPage(true);
        if (ImGui::MenuItem(T("Group all lines on this page"))) GroupAllLines();
        ImGui::EndPopup();
    }
}
