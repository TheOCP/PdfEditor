#include "app.h"
#include "dialogs.h"
#include "imageio.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>

namespace {
const float kGap = 16.0f;    // vertical gap between pages (pixels)
const float kPad = 16.0f;    // top/bottom padding
const int   kMaxTexDim = 4096;

ImU32 Col(const float c[4]) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], c[3])); }

// 8 resize handles for a screen rect: 0 TL,1 T,2 TR,3 R,4 BR,5 B,6 BL,7 L.
void HandlePoints(ImVec2 p0, ImVec2 p1, ImVec2 out[8]) {
    float mx = (p0.x + p1.x) * 0.5f, my = (p0.y + p1.y) * 0.5f;
    out[0] = { p0.x, p0.y }; out[1] = { mx, p0.y }; out[2] = { p1.x, p0.y };
    out[3] = { p1.x, my };   out[4] = { p1.x, p1.y }; out[5] = { mx, p1.y };
    out[6] = { p0.x, p1.y }; out[7] = { p0.x, my };
}
// Which handle is within `tol` px of `m`, or -1.
int HandleAt(const ImVec2 h[8], ImVec2 m, float tol) {
    for (int i = 0; i < 8; ++i)
        if (std::fabs(h[i].x - m.x) <= tol && std::fabs(h[i].y - m.y) <= tol) return i;
    return -1;
}
} // namespace

// Page-space point -> screen pixel, using the page's laid-out rect.
static ImVec2 PageToScreen(App& a, int i, float px, float py) {
    float dx, dy;
    a.doc.PageToDev(i, a.zoom, px, py, dx, dy);
    return ImVec2(a.cache[i].topLeft.x + dx, a.cache[i].topLeft.y + dy);
}

bool App::ScreenToPage(ImVec2 s, int& page, float& px, float& py) {
    for (int i = 0; i < (int)cache.size(); ++i) {
        PageTex& c = cache[i];
        if (!c.laid) continue;
        if (s.x >= c.topLeft.x && s.x <= c.topLeft.x + c.dispW &&
            s.y >= c.topLeft.y && s.y <= c.topLeft.y + c.dispH) {
            doc.DevToPage(i, zoom, s.x - c.topLeft.x, s.y - c.topLeft.y, px, py);
            page = i;
            return true;
        }
    }
    return false;
}

void App::SelectObjectAt(int page, float px, float py) {
    float tol = 4.0f / std::max(0.05f, zoom);   // ~4 screen px, in points
    auto hit = ops::HitTest(doc, page, px, py, tol);
    if (hit) sel.Set(doc, page, *hit);
    else sel.Clear();
}

bool App::EnsureSelectionEditable() {
    if (!sel.Valid(doc)) return false;
    if (!sel.parent) return true;                 // already top-level
    FPDF_PAGEOBJECT target = sel.obj;
    if (!ops::PromoteObject(doc, sel.page, target)) return false;
    // Same handle, now a top-level page object with its geometry baked in.
    sel.parent = nullptr;
    sel.accum = FS_MATRIX{ 1, 0, 0, 1, 0, 0 };
    sel.bounds = ops::ObjectBounds(target);
    InvalidatePage(sel.page);
    return sel.Valid(doc);
}

// ------------------------------------------------------- multi-select / groups

// ------------------------------------------------------- clipboard / cross-page

static std::vector<FPDF_PAGEOBJECT> SelMembers(App& a) {
    if (!a.groupSel.empty()) return a.groupSel;
    if (a.sel.obj) return { a.sel.obj };
    return {};
}

void App::CopySelection() {
    if (!sel.Valid(doc)) return;
    if (groupSel.size() <= 1 && sel.parent) EnsureSelectionEditable();   // bring a nested object to the page first
    clip.clear();
    for (auto o : SelMembers(*this)) {
        ops::ClipObject c;
        if (ops::CaptureObject(doc, sel.page, o, c)) clip.push_back(c);
    }
    if (clip.empty()) Toasts("Nothing copyable in the selection", true);
    else Toasts(Fmt("Copied %d object(s)", (int)clip.size()));
}

void App::CutSelection() {
    if (!sel.Valid(doc)) return;
    CopySelection();
    if (!clip.empty()) DeleteSelection();
}

void App::PasteClipboard() {
    if (clip.empty() || doc.PageCount() == 0) return;
    int tp = currentPage;
    bool allGrouped = clip.size() > 1;
    int g0 = clip.empty() ? 0 : clip[0].groupId;
    for (auto& c : clip) if (c.groupId == 0 || c.groupId != g0) allGrouped = false;
    int gid = allGrouped ? ops::MaxGroupId(doc) + 1 : 0;
    std::vector<FPDF_PAGEOBJECT> pasted;
    std::string err;
    for (auto& c : clip) {
        FPDF_PAGEOBJECT o = ops::PasteObject(doc, tp, c, 12.0f, -12.0f, gid, err);
        if (o) pasted.push_back(o);
    }
    if (pasted.empty()) { Toasts("Paste failed: " + err, true); return; }
    sel.page = tp; sel.obj = pasted[0]; sel.parent = nullptr;
    sel.accum = FS_MATRIX{ 1, 0, 0, 1, 0, 0 }; sel.gen = doc.Generation();
    groupSel = pasted; selGroupId = gid;
    CommitIfDirty("Paste");
    ScrollToPage(tp);
    Toasts(Fmt("Pasted %d object(s) onto page %d", (int)pasted.size(), tp + 1));
}

void App::TransferSelectionToPage(int tp) {
    if (!sel.Valid(doc) || tp < 0 || tp >= doc.PageCount() || tp == sel.page) return;
    int src = sel.page;
    std::vector<ops::ClipObject> tmp;
    auto members = SelMembers(*this);
    for (auto o : members) {
        ops::ClipObject c;
        if (ops::CaptureObject(doc, src, o, c)) tmp.push_back(c);
    }
    if (tmp.empty()) return;
    for (auto o : members) ops::DeleteObjectNested(doc, src, o, nullptr);
    bool keepGroup = selGroupId != 0;
    int gid = keepGroup ? ops::MaxGroupId(doc) + 1 : 0;
    std::vector<FPDF_PAGEOBJECT> pasted;
    std::string err;
    for (auto& c : tmp) {
        FPDF_PAGEOBJECT o = ops::PasteObject(doc, tp, c, 0, 0, gid, err);
        if (o) pasted.push_back(o);
    }
    sel.Clear(); groupSel = pasted; selGroupId = gid;
    if (!pasted.empty()) {
        sel.page = tp; sel.obj = pasted[0]; sel.parent = nullptr;
        sel.accum = FS_MATRIX{ 1, 0, 0, 1, 0, 0 }; sel.gen = doc.Generation();
    }
    CommitIfDirty("Move to page");
    ScrollToPage(tp);
    Toasts(Fmt("Moved %d object(s) to page %d", (int)pasted.size(), tp + 1));
}

void App::SelectAllOnPage(bool linesOnly) {
    if (doc.PageCount() == 0) return;
    int pg = currentPage;
    groupSel.clear(); selGroupId = 0; sel.Clear();
    int common = -1; bool sameGroup = true;
    for (auto& o : ops::ListObjects(doc, pg)) {
        if (o.parent) continue;                          // top-level only
        if (linesOnly && o.type != FPDF_PAGEOBJ_PATH) continue;
        groupSel.push_back(o.obj);
        int gid = ops::GetGroupId(o.obj);
        if (common == -1) common = gid; else if (common != gid) sameGroup = false;
    }
    if (groupSel.empty()) { Toasts(linesOnly ? "No lines on this page" : "Nothing on this page", true); return; }
    ops::ObjInfo first;
    first.obj = groupSel[0]; first.parent = nullptr; first.bounds = ops::ObjectBounds(groupSel[0]);
    sel.Set(doc, pg, first);
    selGroupId = (sameGroup && common > 0) ? common : 0;
    Toasts(Fmt("Selected %d %s", (int)groupSel.size(), linesOnly ? "line(s)" : "object(s)"));
}

void App::GroupAllLines() {
    SelectAllOnPage(true);
    if (groupSel.size() >= 2 && !selGroupId) GroupSelection();
    else if (selGroupId) Toasts("These lines are already a group");
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
    if (groupSel.size() <= 1) return sel.PageBounds();
    PRect u = ops::ObjectBounds(groupSel[0]);
    for (size_t i = 1; i < groupSel.size(); ++i) u = u.Union(ops::ObjectBounds(groupSel[i]));
    return u;
}

void App::MoveSelection(float dx, float dy) {
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
    PRect cur = SelectionBounds();
    float sx = (cur.W() > 0.5f && target.W() > 0) ? target.W() / cur.W() : 1.0f;
    float sy = (cur.H() > 0.5f && target.H() > 0) ? target.H() / cur.H() : 1.0f;
    FS_MATRIX m{ sx, 0, 0, sy, target.l - cur.l * sx, target.b - cur.b * sy };
    for (auto o : groupSel) ops::TransformObjectPage(doc, sel.page, o, m);
    InvalidatePage(sel.page);
}

void App::DeleteSelection() {
    if (!sel.Valid(doc)) return;
    int pg = sel.page;
    if (groupSel.size() <= 1) {
        EnsureSelectionEditable();
        ops::DeleteObjectNested(doc, pg, sel.obj, sel.parent);
    } else {
        for (auto o : groupSel) ops::DeleteObjectNested(doc, pg, o, nullptr);
    }
    sel.Clear(); groupSel.clear(); selGroupId = 0;
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
    Toasts(Fmt("Grouped %d objects - they now move as one", n));
}

void App::SplitSelectedPath(bool pickNearest, float px, float py) {
    if (!sel.Valid(doc) || ops::ObjectType(sel.obj) != FPDF_PAGEOBJ_PATH) return;
    EnsureSelectionEditable();                       // bring it onto the page if nested
    if (ops::SubpathCount(sel.obj) < 2) { Toasts("This is already a single line"); return; }
    int pg = sel.page;
    auto parts = ops::SplitPath(doc, pg, sel.obj);
    if (parts.empty()) return;
    auto selectOne = [&](FPDF_PAGEOBJECT o) {
        ops::ObjInfo oi; oi.obj = o; oi.parent = nullptr; oi.bounds = ops::ObjectBounds(o);
        sel.Set(doc, pg, oi); groupSel = { o }; selGroupId = 0;
    };
    if (pickNearest) {
        FPDF_PAGEOBJECT best = parts[0]; float bd = 1e30f;
        for (auto o : parts) {
            PRect b = ops::ObjectBounds(o);
            float cx = (b.l + b.r) * 0.5f, cy = (b.b + b.t) * 0.5f;
            float dd = b.Contains(px, py) ? 0.0f : (cx - px) * (cx - px) + (cy - py) * (cy - py);
            if (dd < bd) { bd = dd; best = o; }
        }
        selectOne(best);
    } else {
        groupSel = parts; selGroupId = 0; selectOne(parts[0]); groupSel = parts;
    }
    CommitIfDirty("Split into lines");
    Toasts(Fmt("Split into %d separate lines - now move any of them", (int)parts.size()));
}

void App::UngroupSelection() {
    if (!sel.Valid(doc)) return;
    int pg = sel.page;
    if (selGroupId) {
        for (auto o : ops::GroupMembers(doc, pg, selGroupId)) ops::ClearGroupId(doc, pg, o);
        selGroupId = 0;
        groupSel = { sel.obj };
        CommitIfDirty("Ungroup");
        Toasts("Split into separate lines");
        return;
    }
    if (!sel.parent && ops::ObjectType(sel.obj) == FPDF_PAGEOBJ_FORM) {
        ops::ExplodeForm(doc, pg, sel.obj);
        sel.Clear(); groupSel.clear();
        CommitIfDirty("Ungroup");
        Toasts("Broke the group into separate pieces");
    }
}

// Is there a form widget under this page point? Returns the annot or nullptr.
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

// Renders page i to its cached texture at the current zoom if stale.
static void EnsurePageTexture(App& a, int i) {
    PageTex& c = a.cache[i];
    uint32_t v = a.doc.PageVersion(i);
    float scale = a.zoom;
    FS_SIZEF sz = a.doc.PageSizePt(i);
    int w = std::max(1, (int)std::lround(sz.width * scale));
    int h = std::max(1, (int)std::lround(sz.height * scale));
    // Cap texture size; if the page would exceed it, render at a reduced scale.
    float rscale = scale;
    if (w > kMaxTexDim || h > kMaxTexDim) {
        rscale = scale * std::min((float)kMaxTexDim / w, (float)kMaxTexDim / h);
        w = std::max(1, (int)std::lround(sz.width * rscale));
        h = std::max(1, (int)std::lround(sz.height * rscale));
    }
    if (c.tex.Valid() && c.version == v && std::fabs(c.scale - rscale) < 1e-4f) return;
    std::vector<uint32_t> buf;
    if (a.doc.Render(i, rscale, 0, 0, w, h, buf, true, true)) {
        c.tex.Upload(buf.data(), w, h);
        c.version = v;
        c.scale = rscale;
    }
}

void App::DrawCanvas() {
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 region = ImGui::GetContentRegionAvail();
    if (region.x < 10 || region.y < 10) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + region.x, origin.y + region.y), IM_COL32(28, 28, 32, 255));

    int n = doc.PageCount();
    if (n == 0) {
        const char* msg = "Open a PDF  (Ctrl+O)   or   create a new one  (Ctrl+N)";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(origin.x + (region.x - ts.x) / 2, origin.y + region.y / 2 - ts.y), IM_COL32(150, 150, 160, 255), msg);
        // still capture area so drops/clicks don't fall through
        ImGui::InvisibleButton("##canvasHit", region);
        return;
    }
    if ((int)cache.size() != n) cache.resize(n);

    // --- total content height for scroll clamping ---
    float totalH = kPad;
    std::vector<float> dispW(n), dispH(n);
    for (int i = 0; i < n; ++i) {
        FS_SIZEF sz = doc.PageSizePt(i);
        dispW[i] = sz.width * zoom;
        dispH[i] = sz.height * zoom;
        totalH += dispH[i] + kGap;
    }
    totalH += kPad - kGap;
    float maxContentW = 0;
    for (int i = 0; i < n; ++i) maxContentW = std::max(maxContentW, dispW[i]);

    // Fit-to-width on first layout after an open.
    if (fitPending && maxContentW > 0) {
        zoom = std::max(0.1f, std::min(6.0f, (region.x - 40) / (maxContentW / zoom)));
        fitPending = false;
        for (int i = 0; i < n; ++i) { dispW[i] = doc.PageSizePt(i).width * zoom; dispH[i] = doc.PageSizePt(i).height * zoom; }
        totalH = kPad; for (int i = 0; i < n; ++i) totalH += dispH[i] + kGap; totalH += kPad - kGap;
        maxContentW = 0; for (int i = 0; i < n; ++i) maxContentW = std::max(maxContentW, dispW[i]);
    }

    // Capture the whole area for input.
    ImGui::InvisibleButton("##canvasHit", region,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    bool hovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();

    // Scroll to a requested page.
    if (pendingScrollPage >= 0 && pendingScrollPage < n) {
        float y = kPad;
        for (int i = 0; i < pendingScrollPage; ++i) y += dispH[i] + kGap;
        scrollY = y - 20;
        pendingScrollPage = -1;
    }

    float viewW = region.x;
    float maxScroll = std::max(0.0f, totalH - region.y);
    // How far the widest page overflows the viewport horizontally.
    float maxScrollX = std::max(0.0f, maxContentW - (viewW - 40.0f));

    // Wheel: zoom (Ctrl), horizontal scroll (Shift), else vertical scroll.
    if (hovered && io.MouseWheel != 0) {
        if (io.KeyCtrl) {
            float oldZoom = zoom;
            zoom = std::clamp(zoom * (io.MouseWheel > 0 ? 1.1f : 1.0f / 1.1f), 0.1f, 8.0f);
            // keep the point under the cursor stable (both axes)
            float cy = io.MousePos.y - origin.y + scrollY;
            scrollY += cy * (zoom / oldZoom - 1.0f);
            float cx = io.MousePos.x - origin.x + scrollX;
            scrollX += cx * (zoom / oldZoom - 1.0f);
            InvalidateAll();
        } else if (io.KeyShift) {
            scrollX -= io.MouseWheel * 80.0f;
        } else {
            scrollY -= io.MouseWheel * 80.0f;
        }
    }
    if (hovered && io.MouseWheelH != 0) scrollX -= io.MouseWheelH * 80.0f;   // trackpad / tilt wheel

    // Middle-drag or Hand tool: pan both axes.
    bool panning = (tool == Tool::Hand && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) ||
                   (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle));
    if (panning) {
        scrollY -= io.MouseDelta.y;
        scrollX -= io.MouseDelta.x;
    }
    scrollY = std::clamp(scrollY, 0.0f, maxScroll);
    scrollX = std::clamp(scrollX, 0.0f, maxScrollX);

    // --- lay out + draw each page ---
    float baseX = origin.x + std::max(20.0f, (region.x - maxContentW) / 2) - scrollX;
    float y = origin.y + kPad - scrollY;
    int topVisible = 0;
    for (int i = 0; i < n; ++i) {
        PageTex& c = cache[i];
        c.laid = false;
        float x = baseX + (maxContentW - dispW[i]) / 2;
        ImVec2 tl(x, y);
        ImVec2 br(x + dispW[i], y + dispH[i]);
        bool visible = br.y >= origin.y && tl.y <= origin.y + region.y;
        if (visible) {
            if (tl.y <= origin.y + 4) topVisible = i;
            EnsurePageTexture(*this, i);
            c.topLeft = tl;
            c.dispW = dispW[i];
            c.dispH = dispH[i];
            c.laid = true;
            dl->AddRectFilled(ImVec2(tl.x + 3, tl.y + 3), ImVec2(br.x + 3, br.y + 3), IM_COL32(0, 0, 0, 90)); // shadow
            dl->AddRectFilled(tl, br, IM_COL32(255, 255, 255, 255));
            if (c.tex.Valid())
                dl->AddImage(c.tex.Id(), tl, br);
            dl->AddRect(tl, br, IM_COL32(70, 70, 80, 255));
            // page number tag
            char tag[32];
            snprintf(tag, sizeof(tag), "%d", i + 1);
            dl->AddText(ImVec2(tl.x + 4, tl.y - 16), IM_COL32(170, 170, 180, 255), tag);

            // search highlights on this page
            for (int h = 0; h < (int)searchHits.size(); ++h) {
                if (searchHits[h].page != i) continue;
                ImVec2 a0 = PageToScreen(*this, i, searchHits[h].box.l, searchHits[h].box.t);
                ImVec2 a1 = PageToScreen(*this, i, searchHits[h].box.r, searchHits[h].box.b);
                ImU32 col = (h == searchCurrent) ? IM_COL32(255, 170, 40, 110) : IM_COL32(255, 230, 0, 70);
                dl->AddRectFilled(ImVec2(std::min(a0.x, a1.x), std::min(a0.y, a1.y)),
                                  ImVec2(std::max(a0.x, a1.x), std::max(a0.y, a1.y)), col);
            }
        }
        y += dispH[i] + kGap;
    }
    currentPage = topVisible;
    doc.currentPageHint = topVisible;

    // --- selection overlay ---
    if (sel.Valid(doc) && sel.page >= 0 && cache[sel.page].laid) {
        bool grp = groupSel.size() > 1;
        // thin outline around each member of a multi/group selection
        if (grp) {
            for (auto o : groupSel) {
                PRect mb = ops::ObjectBounds(o);
                ImVec2 m0 = PageToScreen(*this, sel.page, mb.l, mb.t);
                ImVec2 m1 = PageToScreen(*this, sel.page, mb.r, mb.b);
                dl->AddRect(ImVec2(std::min(m0.x, m1.x), std::min(m0.y, m1.y)),
                            ImVec2(std::max(m0.x, m1.x), std::max(m0.y, m1.y)), IM_COL32(60, 150, 255, 120));
            }
        }
        PRect b = SelectionBounds();
        sel.bounds = b;
        ImVec2 c0 = PageToScreen(*this, sel.page, b.l, b.t);
        ImVec2 c1 = PageToScreen(*this, sel.page, b.r, b.b);
        ImVec2 p0(std::min(c0.x, c1.x) - 1, std::min(c0.y, c1.y) - 1);
        ImVec2 p1(std::max(c0.x, c1.x) + 1, std::max(c0.y, c1.y) + 1);
        ImU32 boxCol = selGroupId ? IM_COL32(70, 200, 120, 255) : IM_COL32(60, 150, 255, 255);
        dl->AddRect(p0, p1, boxCol, 0, 0, grp ? 2.0f : 1.5f);
        if (tool == Tool::Select) {
            ImVec2 h[8];
            HandlePoints(p0, p1, h);
            for (int i = 0; i < 8; ++i) {
                dl->AddRectFilled(ImVec2(h[i].x - 4, h[i].y - 4), ImVec2(h[i].x + 4, h[i].y + 4), IM_COL32(255, 255, 255, 255));
                dl->AddRect(ImVec2(h[i].x - 4, h[i].y - 4), ImVec2(h[i].x + 4, h[i].y + 4), boxCol);
            }
        }
    }

    // --- marquee preview ---
    if (dragging && dragMode == 5 && dragPage >= 0 && cache[dragPage].laid) {
        ImVec2 a0 = PageToScreen(*this, dragPage, marquee.l, marquee.t);
        ImVec2 a1 = PageToScreen(*this, dragPage, marquee.r, marquee.b);
        ImVec2 p0(std::min(a0.x, a1.x), std::min(a0.y, a1.y));
        ImVec2 p1(std::max(a0.x, a1.x), std::max(a0.y, a1.y));
        dl->AddRectFilled(p0, p1, IM_COL32(60, 150, 255, 30));
        dl->AddRect(p0, p1, IM_COL32(60, 150, 255, 200));
    }

    // --- drag preview (rect / redact) ---
    if (dragging && (dragMode == 2 || dragMode == 3) && dragPage >= 0 && cache[dragPage].laid) {
        ImVec2 a0 = PageToScreen(*this, dragPage, dragRect.l, dragRect.t);
        ImVec2 a1 = PageToScreen(*this, dragPage, dragRect.r, dragRect.b);
        ImVec2 p0(std::min(a0.x, a1.x), std::min(a0.y, a1.y));
        ImVec2 p1(std::max(a0.x, a1.x), std::max(a0.y, a1.y));
        if (dragMode == 3) {
            dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 160));
            dl->AddRect(p0, p1, IM_COL32(255, 60, 60, 255));
        } else {
            ImU32 f = rectFilled ? Col(fillColor) : 0;
            if (rectFilled) dl->AddRectFilled(p0, p1, f);
            dl->AddRect(p0, p1, Col(strokeColor), 0, 0, rectStroked ? strokeWidth : 1.0f);
        }
    }

    // ================= interaction =================
    HandleCanvasInput(hovered);
}

// Pointer / keyboard handling for the page viewport.
void App::HandleCanvasInput(bool hovered) {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mouse = io.MousePos;

    // Drop any stale multi-selection once the selection is no longer valid
    // (e.g. after undo/redo or a structural page change invalidated handles).
    if (!sel.Valid(doc) && !groupSel.empty()) { groupSel.clear(); selGroupId = 0; }

    // Forward queued characters / edit keys to a focused form field.
    if (formPage >= 0 && doc.Form()) {
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
            if (ImGui::IsKeyPressed(e.k, false)) { ForwardFormKey(e.vk, true); ForwardFormKey(e.vk, false); }
    }

    // Delete selected object(s).
    if (!editing && sel.Valid(doc) && ImGui::IsKeyPressed(ImGuiKey_Delete) && formPage < 0)
        DeleteSelection();

    // ---- arrow-key nudge / resize of the selection ----
    if (!editing && sel.Valid(doc) && formPage < 0) {
        float step = io.KeyShift ? 10.0f : (io.KeyCtrl ? 0.25f : 1.0f);
        bool alt = io.KeyAlt;   // Alt = resize (grow/shrink from lower-left) instead of move
        auto nudge = [&](float dx, float dy) {
            if (alt) {
                PRect b = SelectionBounds();
                PRect t{ b.l, b.b, b.r + dx, b.t + dy };
                if (t.r < t.l + 1) t.r = t.l + 1;
                if (t.t < t.b + 1) t.t = t.b + 1;
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
        bool anyDown = ImGui::IsKeyDown(ImGuiKey_LeftArrow) || ImGui::IsKeyDown(ImGuiKey_RightArrow) ||
                       ImGui::IsKeyDown(ImGuiKey_UpArrow) || ImGui::IsKeyDown(ImGuiKey_DownArrow);
        if (nudgeDirty && !anyDown) { CommitIfDirty("Nudge"); nudgeDirty = false; }
    }

    int page; float px, py;
    bool over = hovered && ScreenToPage(mouse, page, px, py);

    // ---- grab a resize handle of the current selection ----
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && tool == Tool::Select && !dragging &&
        sel.Valid(doc) && sel.page >= 0 && sel.page < (int)cache.size() && cache[sel.page].laid) {
        PRect b = SelectionBounds();
        ImVec2 c0 = PageToScreen(*this, sel.page, b.l, b.t);
        ImVec2 c1 = PageToScreen(*this, sel.page, b.r, b.b);
        ImVec2 p0(std::min(c0.x, c1.x) - 1, std::min(c0.y, c1.y) - 1);
        ImVec2 p1(std::max(c0.x, c1.x) + 1, std::max(c0.y, c1.y) + 1);
        ImVec2 h[8];
        HandlePoints(p0, p1, h);
        int hi = HandleAt(h, mouse, 6.0f);
        if (hi >= 0) {
            if (groupSel.size() <= 1) EnsureSelectionEditable();
            dragging = true; dragMode = 4; dragPage = sel.page; resizeHandle = hi;
            resizeStart = SelectionBounds();
        }
    }

    // ---- right-click: select what's under the cursor, open the object menu ----
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && over && formPage < 0) {
        auto hit = ops::HitTest(doc, page, px, py, 4.0f / std::max(0.05f, zoom));
        if (hit) {
            sel.Set(doc, page, *hit);
            if (!io.KeyAlt) RefreshGroupSel(); else { groupSel = { sel.obj }; selGroupId = 0; }
            openObjCtx = true;
        }
    }

    // ---- mouse down ----
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && over && !dragging) {
        // Always give the form engine first refusal (click to focus/toggle).
        FPDF_ANNOTATION widget = FormFieldAt(*this, page, px, py);
        if (widget) FPDFPage_CloseAnnot(widget);
        if (widget && doc.Form() && (tool == Tool::Select || tool == Tool::Hand)) {
            FORM_OnLButtonDown(doc.Form(), doc.Page(page), 0, px, py);
            FORM_OnLButtonUp(doc.Form(), doc.Page(page), 0, px, py);
            formPage = page;
            doc.currentPageHint = page;
            sel.Clear();
            InvalidatePage(page);
            doc.formDirty = true;
        } else {
            if (formPage >= 0) { doc.KillFormFocus(); InvalidatePage(formPage); formPage = -1; }
            switch (tool) {
            case Tool::Select: {
                // Clicking inside the current group keeps the whole set (drag it as one),
                // unless Alt is held - then pick the single line under the cursor instead.
                bool insideGroup = !io.KeyAlt && groupSel.size() > 1 && sel.Valid(doc) && sel.page == page &&
                                   SelectionBounds().Contains(px, py);
                if (!insideGroup) {
                    SelectObjectAt(page, px, py);
                    if (sel.Valid(doc)) {
                        if (!io.KeyAlt) RefreshGroupSel();
                        else { groupSel = { sel.obj }; selGroupId = 0; }
                    }
                }
                if (sel.Valid(doc)) {
                    dragging = true; dragMode = 1; dragPage = page;
                    dragLastPx = px; dragLastPy = py;
                } else {
                    // empty space: start a marquee rubber-band selection
                    dragging = true; dragMode = 5; dragPage = page;
                    dragStartPx = px; dragStartPy = py;
                    marquee = { px, py, px, py };
                    groupSel.clear(); selGroupId = 0;
                }
                break;
            }
            case Tool::Text: {
                editing = true; editIsNew = true; editPage = page;
                editAtX = px; editAtY = py; editBuf.clear();
                editObj = nullptr;
                // Sample the PDF's own font nearest the click so new text can match it.
                pendingFontValid = false;
                FPDF_PAGEOBJECT nearest = nullptr; float bestd = 1e30f;
                for (auto& o : ops::ListObjects(doc, page)) {
                    if (o.type != FPDF_PAGEOBJ_TEXT) continue;
                    float cx = (o.bounds.l + o.bounds.r) * 0.5f, cy = (o.bounds.b + o.bounds.t) * 0.5f;
                    float dd = o.bounds.Contains(px, py) ? 0.0f : (cx - px) * (cx - px) + (cy - py) * (cy - py);
                    if (dd < bestd) { bestd = dd; nearest = o.obj; }
                }
                if (nearest) {
                    ops::ClipObject c;
                    if (ops::CaptureObject(doc, page, nearest, c) && c.type == FPDF_PAGEOBJ_TEXT) {
                        pendingFontData = c.fontData;
                        pendingFontBase = c.fontBase;
                        pendingFontSize = c.fontSize;
                        pendingFontValid = true;
                    }
                }
                break;
            }
            case Tool::Image: {
                std::wstring f;
                if (OpenFileDialog(hwnd_, { { L"Images", L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.webp" }, { L"All files", L"*.*" } }, f)) {
                    Image img; std::string err;
                    if (LoadImageFile(f, img, err)) {
                        FS_SIZEF sz = doc.PageSizePt(page);
                        float w = std::min(sz.width * 0.5f, (float)img.w * 0.75f);
                        FPDF_PAGEOBJECT o = ops::AddImage(doc, page, img, px, py - w * img.h / img.w, w, err);
                        if (o) {
                            SelectNewObject(page, o);
                            CommitIfDirty("Add image");
                            Toasts("Image added");
                        } else Toasts("Add image failed: " + err, true);
                    } else Toasts("Could not load image: " + err, true);
                }
                break;
            }
            case Tool::Rect:
            case Tool::Redact:
                dragging = true; dragMode = (tool == Tool::Redact) ? 3 : 2;
                dragPage = page; dragStartPx = px; dragStartPy = py;
                dragRect = { px, py, px, py };
                break;
            default: break;
            }
        }
    }

    // ---- dragging ----
    if (dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (dragMode == 1 && sel.Valid(doc)) {
            int p2; float nx, ny;
            if (ScreenToPage(mouse, p2, nx, ny) && p2 == dragPage) {
                float ddx = nx - dragLastPx, ddy = ny - dragLastPy;
                if (ddx != 0 || ddy != 0) {
                    MoveSelection(ddx, ddy);
                    dragLastPx = nx; dragLastPy = ny;
                }
            }
        } else if (dragMode == 4 && sel.Valid(doc)) {
            int p2; float nx, ny;
            if (ScreenToPage(mouse, p2, nx, ny) && p2 == dragPage) {
                const float MIN = 1.0f;
                PRect t = resizeStart;
                bool L = (resizeHandle == 0 || resizeHandle == 6 || resizeHandle == 7);
                bool R = (resizeHandle == 2 || resizeHandle == 3 || resizeHandle == 4);
                bool T = (resizeHandle == 0 || resizeHandle == 1 || resizeHandle == 2);
                bool B = (resizeHandle == 4 || resizeHandle == 5 || resizeHandle == 6);
                if (L) t.l = std::min(nx, resizeStart.r - MIN);
                if (R) t.r = std::max(nx, resizeStart.l + MIN);
                if (T) t.t = std::max(ny, resizeStart.b + MIN);
                if (B) t.b = std::min(ny, resizeStart.t - MIN);
                ResizeSelectionTo(t);
            }
        } else if (dragMode == 5) {
            int p2; float nx, ny;
            if (ScreenToPage(mouse, p2, nx, ny) && p2 == dragPage)
                marquee = PRect::FromPoints(dragStartPx, dragStartPy, nx, ny);
        } else if (dragMode == 2 || dragMode == 3) {
            int p2; float nx, ny;
            if (ScreenToPage(mouse, p2, nx, ny) && p2 == dragPage)
                dragRect = PRect::FromPoints(dragStartPx, dragStartPy, nx, ny);
        }
    }

    // ---- mouse up ----
    if (dragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (dragMode == 1) {
            // Dropped over a different page? Move the selection onto that page.
            int dp; float dpx, dpy;
            if (ScreenToPage(mouse, dp, dpx, dpy) && dp != dragPage)
                TransferSelectionToPage(dp);
            else if (sel.Valid(doc))
                CommitIfDirty(groupSel.size() > 1 ? "Move group" : "Move object");
        } else if (dragMode == 4) {
            if (sel.Valid(doc)) CommitIfDirty("Resize");
            resizeHandle = -1;
        } else if (dragMode == 5) {
            // finalize marquee: select every top-level object intersecting the box
            if (marquee.Area() > 1) {
                groupSel.clear(); selGroupId = 0; sel.Clear();
                for (auto& o : ops::ListObjects(doc, dragPage)) {
                    if (o.parent) continue;              // top-level only
                    if (o.bounds.Intersects(marquee)) groupSel.push_back(o.obj);
                }
                if (!groupSel.empty()) {
                    ops::ObjInfo first;
                    first.obj = groupSel[0]; first.parent = nullptr; first.bounds = ops::ObjectBounds(groupSel[0]);
                    sel.Set(doc, dragPage, first);
                    Toasts(Fmt("%d objects selected - right-click to group", (int)groupSel.size()));
                }
            }
        } else if (dragMode == 2 && dragRect.Area() > 4) {
            FPDF_PAGEOBJECT o = ops::AddRect(doc, dragPage, dragRect, rectFilled,
                                             (unsigned)(fillColor[0] * 255), (unsigned)(fillColor[1] * 255), (unsigned)(fillColor[2] * 255),
                                             rectStroked, (unsigned)(strokeColor[0] * 255), (unsigned)(strokeColor[1] * 255), (unsigned)(strokeColor[2] * 255),
                                             strokeWidth);
            if (o) { SelectNewObject(dragPage, o); CommitIfDirty("Add rectangle"); Toasts("Rectangle added"); }
        } else if (dragMode == 3 && dragRect.Area() > 4) {
            int removed = ops::Redact(doc, dragPage, dragRect, 0, 0, 0);
            CommitIfDirty("Redact");
            Toasts(Fmt("Redacted (removed %d object%s under the box)", removed, removed == 1 ? "" : "s"));
            sel.Clear();
        }
        dragging = false; dragMode = 0; dragPage = -1;
    }

    // ---- double-click: edit text, or pick one line out of a group ----
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && over && tool == Tool::Select) {
        auto hit = ops::HitTest(doc, page, px, py, 4.0f / std::max(0.05f, zoom));
        if (hit && hit->type == FPDF_PAGEOBJ_TEXT) {
            ops::TextProps tp = ops::ReadText(doc, page, hit->obj);
            sel.Set(doc, page, *hit);
            EnsureSelectionEditable();                 // flatten group so the text edit persists
            editing = true; editIsNew = false; editPage = page; editObj = sel.obj;
            editBuf = W2U(tp.text);
        } else if (hit) {
            sel.Set(doc, page, *hit);
            groupSel = { sel.obj };
            selGroupId = 0;
            // A path that draws several lines at once -> split so one line can move alone.
            if (hit->type == FPDF_PAGEOBJ_PATH && ops::SubpathCount(hit->obj) > 1) {
                SplitSelectedPath(true, px, py);
            } else if (ops::GetGroupId(sel.obj)) {
                Toasts("Editing one line - drag it or use arrow keys; it stays in the group");
            }
        }
    }

    // ---- Escape clears the selection; Ctrl+A selects everything on the page ----
    if (!editing && formPage < 0) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { sel.Clear(); groupSel.clear(); selGroupId = 0; }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A)) SelectAllOnPage(io.KeyShift);  // Shift = lines only
    }

    // ---- object context menu (right-click) ----
    if (openObjCtx) { ImGui::OpenPopup("##objctx"); openObjCtx = false; }
    if (ImGui::BeginPopup("##objctx")) {
        bool grouped = selGroupId != 0;
        bool multi = groupSel.size() > 1;
        bool isForm = sel.Valid(doc) && !sel.parent && ops::ObjectType(sel.obj) == FPDF_PAGEOBJ_FORM;
        if (multi && !grouped)
            if (ImGui::MenuItem(Fmt("Group these %d into one", (int)groupSel.size()).c_str())) GroupSelection();
        if (grouped) {
            ImGui::TextDisabled("Grouped - moves as one piece");
            if (ImGui::MenuItem("Ungroup (back to separate lines)")) UngroupSelection();
        }
        if (isForm)
            if (ImGui::MenuItem("Break apart (ungroup)")) UngroupSelection();
        bool splittable = sel.Valid(doc) && ops::ObjectType(sel.obj) == FPDF_PAGEOBJ_PATH &&
                          ops::SubpathCount(sel.obj) > 1;
        if (splittable)
            if (ImGui::MenuItem(Fmt("Split into %d separate lines", ops::SubpathCount(sel.obj)).c_str()))
                SplitSelectedPath(false, 0, 0);
        if (multi || grouped || isForm || splittable) ImGui::Separator();
        if (ImGui::MenuItem("Copy", "Ctrl+C")) CopySelection();
        if (ImGui::MenuItem("Cut", "Ctrl+X")) CutSelection();
        if (ImGui::MenuItem("Paste here", "Ctrl+V", false, !clip.empty())) PasteClipboard();
        if (doc.PageCount() > 1) {
            if (ImGui::BeginMenu("Move to page")) {
                for (int i = 0; i < doc.PageCount(); ++i) {
                    if (i == sel.page) continue;
                    if (ImGui::MenuItem(Fmt("Page %d", i + 1).c_str())) { TransferSelectionToPage(i); break; }
                }
                ImGui::EndMenu();
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete", "Del")) DeleteSelection();
        ImGui::EndPopup();
    }
}
