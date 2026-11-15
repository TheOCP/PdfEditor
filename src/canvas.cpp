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
    auto hit = ops::HitTest(doc, page, px, py);
    if (hit) {
        sel.page = page;
        sel.obj = hit->obj;
        sel.gen = doc.Generation();
        sel.bounds = hit->bounds;
    } else {
        sel.Clear();
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

    // Wheel: zoom (Ctrl) or scroll.
    if (hovered && io.MouseWheel != 0) {
        if (io.KeyCtrl) {
            float oldZoom = zoom;
            zoom = std::clamp(zoom * (io.MouseWheel > 0 ? 1.1f : 1.0f / 1.1f), 0.1f, 8.0f);
            // keep the point under the cursor stable
            float cy = io.MousePos.y - origin.y + scrollY;
            scrollY += cy * (zoom / oldZoom - 1.0f);
            InvalidateAll();
        } else {
            scrollY -= io.MouseWheel * 80.0f;
        }
    }
    float maxScroll = std::max(0.0f, totalH - region.y);
    scrollY = std::clamp(scrollY, 0.0f, maxScroll);

    // Middle-drag or Hand tool: pan.
    bool panning = (tool == Tool::Hand && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) ||
                   (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle));
    if (panning) {
        ImVec2 dd = io.MouseDelta;
        scrollY -= dd.y;
        scrollX -= dd.x;
        scrollY = std::clamp(scrollY, 0.0f, maxScroll);
    }

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
        PRect b = ops::ObjectBounds(sel.obj);
        sel.bounds = b;
        ImVec2 c0 = PageToScreen(*this, sel.page, b.l, b.t);
        ImVec2 c1 = PageToScreen(*this, sel.page, b.r, b.b);
        ImVec2 p0(std::min(c0.x, c1.x) - 1, std::min(c0.y, c1.y) - 1);
        ImVec2 p1(std::max(c0.x, c1.x) + 1, std::max(c0.y, c1.y) + 1);
        dl->AddRect(p0, p1, IM_COL32(60, 150, 255, 255), 0, 0, 1.5f);
        dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + 0), IM_COL32(60, 150, 255, 40));
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

    // Delete selected object.
    if (!editing && sel.Valid(doc) && ImGui::IsKeyPressed(ImGuiKey_Delete) && formPage < 0) {
        int pg = sel.page;
        if (ops::DeleteObject(doc, pg, sel.obj)) {
            sel.Clear();
            CommitIfDirty("Delete object");
        }
    }

    int page; float px, py;
    bool over = hovered && ScreenToPage(mouse, page, px, py);

    // ---- mouse down ----
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && over) {
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
            case Tool::Select:
                SelectObjectAt(page, px, py);
                if (sel.Valid(doc)) {
                    dragging = true; dragMode = 1; dragPage = page;
                    dragLastPx = px; dragLastPy = py;
                }
                break;
            case Tool::Text:
                editing = true; editIsNew = true; editPage = page;
                editAtX = px; editAtY = py; editBuf.clear();
                editObj = nullptr;
                break;
            case Tool::Image: {
                std::wstring f;
                if (OpenFileDialog(hwnd_, { { L"Images", L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.webp" }, { L"All files", L"*.*" } }, f)) {
                    Image img; std::string err;
                    if (LoadImageFile(f, img, err)) {
                        FS_SIZEF sz = doc.PageSizePt(page);
                        float w = std::min(sz.width * 0.5f, (float)img.w * 0.75f);
                        FPDF_PAGEOBJECT o = ops::AddImage(doc, page, img, px, py - w * img.h / img.w, w, err);
                        if (o) {
                            sel.page = page; sel.obj = o; sel.gen = doc.Generation();
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
                ops::MoveObject(doc, dragPage, sel.obj, nx - dragLastPx, ny - dragLastPy);
                dragLastPx = nx; dragLastPy = ny;
                InvalidatePage(dragPage);
            }
        } else if (dragMode == 2 || dragMode == 3) {
            int p2; float nx, ny;
            if (ScreenToPage(mouse, p2, nx, ny) && p2 == dragPage)
                dragRect = PRect::FromPoints(dragStartPx, dragStartPy, nx, ny);
        }
    }

    // ---- mouse up ----
    if (dragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (dragMode == 1) {
            if (sel.Valid(doc)) CommitIfDirty("Move object");
        } else if (dragMode == 2 && dragRect.Area() > 4) {
            FPDF_PAGEOBJECT o = ops::AddRect(doc, dragPage, dragRect, rectFilled,
                                             (unsigned)(fillColor[0] * 255), (unsigned)(fillColor[1] * 255), (unsigned)(fillColor[2] * 255),
                                             rectStroked, (unsigned)(strokeColor[0] * 255), (unsigned)(strokeColor[1] * 255), (unsigned)(strokeColor[2] * 255),
                                             strokeWidth);
            if (o) { sel.page = dragPage; sel.obj = o; sel.gen = doc.Generation(); CommitIfDirty("Add rectangle"); Toasts("Rectangle added"); }
        } else if (dragMode == 3 && dragRect.Area() > 4) {
            int removed = ops::Redact(doc, dragPage, dragRect, 0, 0, 0);
            CommitIfDirty("Redact");
            Toasts(Fmt("Redacted (removed %d object%s under the box)", removed, removed == 1 ? "" : "s"));
            sel.Clear();
        }
        dragging = false; dragMode = 0; dragPage = -1;
    }

    // ---- double-click: edit text in place ----
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && over && tool == Tool::Select) {
        auto hit = ops::HitTest(doc, page, px, py);
        if (hit && hit->type == FPDF_PAGEOBJ_TEXT) {
            ops::TextProps tp = ops::ReadText(doc, page, hit->obj);
            editing = true; editIsNew = false; editPage = page; editObj = hit->obj;
            editBuf = W2U(tp.text);
            sel.page = page; sel.obj = hit->obj; sel.gen = doc.Generation();
        }
    }
}
