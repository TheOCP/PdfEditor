#include "app.h"
#include "dialogs.h"
#include "i18n.h"
#include "icons.h"
#include "theme.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include <fpdf_signature.h>
#include <algorithm>
#include <cfloat>
#include <cmath>

// ------------------------------------------------------------- layout helpers
//
// Labels are translated (German words are longer), so nothing here assumes a
// fixed text width: rows wrap, buttons size to their label, long checkbox
// labels wrap inside narrow panels.

static float ButtonWidth(const char* label) {
    return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}
static float ButtonWidth(const char* label, float minW) { return std::max(minW, ButtonWidth(label)); }

// Lays items out left to right and starts a new row when the next one would
// not fit into the space that was available when the row began.
class FlowRow {
public:
    explicit FlowRow(float spacing)
        : sp_(spacing), x0_(ImGui::GetCursorPosX()),
          right_(ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x) {}
    void Before(float w) {
        if (n_++ > 0) {
            if (lastRight_ + sp_ + w <= right_) ImGui::SameLine(0, sp_);
            else ImGui::SetCursorPosX(x0_);
        }
    }
    void After() { lastRight_ = ImGui::GetItemRectMax().x; }
    // Would an item of width w still fit on the current row?
    bool Fits(float w) const { return n_ == 0 || lastRight_ + sp_ + w <= right_; }
    bool Button(const char* label) {
        Before(ButtonWidth(label));
        bool r = ImGui::Button(label);
        After();
        return r;
    }

private:
    float sp_, x0_, right_, lastRight_ = 0;
    int n_ = 0;
};

// A checkbox whose label wraps when it is wider than the space left.
static bool CheckboxWrap(const char* label, bool* v) {
    float box = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
    if (box + ImGui::CalcTextSize(label, nullptr, true).x <= ImGui::GetContentRegionAvail().x)
        return ImGui::Checkbox(label, v);
    ImGui::PushID(label);
    bool changed = ImGui::Checkbox("##cb", v);
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextWrapped("%s", label);
    if (ImGui::IsItemClicked()) { *v = !*v; changed = true; }
    ImGui::PopID();
    return changed;
}

// Dialogs open centred and never grow past the visible area.
static void CenterNextWindow() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
}
static void FitNextWindowToScreen() {
    ImVec2 vs = ImGui::GetMainViewport()->WorkSize;
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(vs.x * 0.96f, vs.y * 0.92f));
    CenterNextWindow();
}

// ---------------------------------------------------------------- menu bar

void App::DrawMenuBar() {
    if (!ImGui::BeginMenuBar()) return;
    bool has = doc.Raw() != nullptr;
    if (ImGui::BeginMenu(T("File"))) {
        if (ImGui::MenuItem(T("New"), K("Ctrl+N"))) NewDoc();
        if (ImGui::MenuItem(T("Open..."), K("Ctrl+O"))) OpenDialog();
        ImGui::Separator();
        if (ImGui::MenuItem(T("Save"), K("Ctrl+S"), false, has)) Save();
        if (ImGui::MenuItem(T("Save As..."), K("Ctrl+Shift+S"), false, has)) SaveAs();
        ImGui::Separator();
        if (ImGui::MenuItem(T("Import pages from another PDF..."), nullptr, false, has)) {
            std::wstring f;
            if (OpenFileDialog(hwnd_, { { TW("PDF documents"), L"*.pdf" } }, f)) {
                importFile = f; importRange.clear(); importPw.clear(); importAt = -1; showImport = true;
            }
        }
        if (ImGui::BeginMenu(T("Export pages as PNG"), has)) {
            if (ImGui::MenuItem(T("96 DPI (screen)"))) ExportImagesDialog(96);
            if (ImGui::MenuItem(T("150 DPI"))) ExportImagesDialog(150);
            if (ImGui::MenuItem(T("300 DPI (print)"))) ExportImagesDialog(300);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(T("Extract all text..."), nullptr, false, has)) ExtractTextDialog();
        ImGui::Separator();
        if (ImGui::MenuItem(T("Exit"), "Alt+F4")) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(T("Edit"))) {
        if (ImGui::MenuItem(T("Undo"), K("Ctrl+Z"), false, doc.CanUndo())) Undo();
        if (ImGui::MenuItem(T("Redo"), K("Ctrl+Y"), false, doc.CanRedo())) Redo();
        ImGui::Separator();
        bool haveSel = sel.Valid(doc) || AnnotSelected();
        const char* cbt = ImGui::GetClipboardText();
        bool canPaste = has && (!clip.empty() || !limbo.empty() || (cbt && *cbt));
        if (ImGui::MenuItem(T("Copy"), K("Ctrl+C"), false, sel.Valid(doc))) CopySelection();
        if (ImGui::MenuItem(T("Cut"), K("Ctrl+X"), false, haveSel)) CutSelection();
        if (ImGui::MenuItem(T("Paste (same position)"), K("Ctrl+V"), false, canPaste)) PasteClipboard(false);
        if (ImGui::MenuItem(T("Paste at mouse"), K("Ctrl+Shift+V"), false, canPaste)) PasteClipboard(true);
        if (ImGui::MenuItem(T("Delete selected"), K("Del"), false, haveSel)) DeleteSelection();
        ImGui::Separator();
        if (ImGui::MenuItem(T("Select all on page"), K("Ctrl+A"), false, has)) SelectAllOnPage(false);
        if (ImGui::MenuItem(T("Select all lines (skip text/images)"), K("Ctrl+Shift+A"), false, has)) SelectAllOnPage(true);
        ImGui::Separator();
        if (ImGui::MenuItem(T("Group all lines on page"), nullptr, false, has)) GroupAllLines();
        if (ImGui::MenuItem(T("Group selection"), nullptr, false, groupSel.size() > 1 && !selGroupId)) GroupSelection();
        if (ImGui::MenuItem(T("Ungroup"), nullptr, false, selGroupId != 0)) UngroupSelection();
        bool splittable = sel.Valid(doc) && groupSel.size() <= 1 && ops::SplittableParts(sel.obj) > 1;
        if (ImGui::MenuItem(T("Split selected line drawing into pieces"), nullptr, false, splittable)) SplitSelectedPath(false, 0, 0);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(T("Page"))) {
        int cp = currentPage;
        FS_SIZEF sz = doc.PageCount() ? doc.PageSizePt(cp) : FS_SIZEF{ 612, 792 };
        if (ImGui::MenuItem(T("Insert blank after current"), nullptr, false, has))
            if (ops::InsertBlankPage(doc, cp + 1, sz.width, sz.height)) { ClearSelection(); CommitIfDirty("Insert page"); }
        if (ImGui::MenuItem(T("Duplicate current"), nullptr, false, has))
            if (ops::DuplicatePage(doc, cp)) { ClearSelection(); CommitIfDirty("Duplicate page"); }
        if (ImGui::MenuItem(T("Delete current"), nullptr, false, has && doc.PageCount() > 1))
            if (ops::DeletePages(doc, { cp })) { ClearSelection(); CommitIfDirty("Delete page"); }
        ImGui::Separator();
        if (ImGui::MenuItem(T("Rotate current 90\xC2\xB0 right"), nullptr, false, has))
            if (ops::RotatePage(doc, cp, 1)) { ClearSelection(); CommitIfDirty("Rotate page"); }
        if (ImGui::MenuItem(T("Rotate current 90\xC2\xB0 left"), nullptr, false, has))
            if (ops::RotatePage(doc, cp, -1)) { ClearSelection(); CommitIfDirty("Rotate page"); }
        ImGui::Separator();
        if (ImGui::MenuItem(T("Make this page's annotations editable"), nullptr, false, has)) FlattenCurrentPage();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(T("Insert"))) {
        if (ImGui::MenuItem(T("Table / form..."), nullptr, false, has)) showTable = true;
        ImGui::Separator();
        if (ImGui::MenuItem(T("Text (click to place)"), nullptr, tool == Tool::Text, has)) tool = Tool::Text;
        if (ImGui::MenuItem(T("Image (click to place)"), nullptr, tool == Tool::Image, has)) tool = Tool::Image;
        if (ImGui::MenuItem(T("Rectangle (drag)"), nullptr, tool == Tool::Rect, has)) tool = Tool::Rect;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(T("Document"))) {
        if (ImGui::MenuItem(T("Flatten all forms & annotations"), nullptr, false, has)) DoFlatten();
        if (ImGui::MenuItem(T("Properties / metadata"), nullptr, false, has)) showMeta = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(T("View"))) {
        ImGui::MenuItem(T("Page thumbnails"), nullptr, &showThumbs);
        ImGui::MenuItem(T("Properties panel"), nullptr, &showProps);
        ImGui::Separator();
        if (ImGui::MenuItem(T("Zoom in"), K("Ctrl +"))) { zoom = std::min(8.0f, zoom * 1.15f); InvalidateAll(); }
        if (ImGui::MenuItem(T("Zoom out"), K("Ctrl -"))) { zoom = std::max(0.1f, zoom / 1.15f); InvalidateAll(); }
        if (ImGui::MenuItem(T("Fit width"), K("Ctrl 0"))) fitPending = true;
        if (ImGui::MenuItem(T("Actual size (100%)"), K("Ctrl 1"))) { zoom = 96.0f / 72.0f; InvalidateAll(); }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(T("Settings"))) {
        if (ImGui::BeginMenu(T("Language"))) {
            if (ImGui::MenuItem("English", nullptr, Language() == Lang::English)) ChangeLanguage(Lang::English);
            if (ImGui::MenuItem("Deutsch", nullptr, Language() == Lang::German)) ChangeLanguage(Lang::German);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(T("Settings..."))) showSettings = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(T("Help"))) {
        if (ImGui::MenuItem(T("How to / keyboard shortcuts"), "F1")) showHelp = true;
        if (ImGui::MenuItem(T("About"))) showAbout = true;
        ImGui::Separator();
        ImGui::TextDisabled("%s", T("by TheOCProductions & ShortGG"));
        ImGui::EndMenu();
    }
    if (has) {
        // The file name on the right, only when there is room next to the menus.
        std::string title = W2U(doc.displayName) + (doc.modified ? T("  (unsaved)") : "");
        float w = ImGui::CalcTextSize(title.c_str()).x;
        float x = ImGui::GetWindowWidth() - w - 20 * dpiScale;
        if (x > ImGui::GetCursorPosX() + 24 * dpiScale) {
            ImGui::SameLine(x);
            ImGui::TextDisabled("%s", title.c_str());
        }
    }
    ImGui::EndMenuBar();
}

// ---------------------------------------------------------------- toolbar

// One toolbar button: icon, plus its name when there is room. The tooltip
// always names it (and its shortcut).
static bool ToolbarButton(const char* icon, const char* label, bool showLabel, bool active, const char* tip) {
    std::string text = showLabel ? std::string(icon) + "  " + label : std::string(icon);
    ImGui::PushID(label);
    bool r = FlatButton((text + "##b").c_str(), active);
    ImGui::PopID();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        if (tip && *tip) ImGui::SetTooltip("%s\n%s", label, tip);
        else ImGui::SetTooltip("%s", label);
    }
    return r;
}
static float ToolbarButtonWidth(const char* icon, const char* label, bool showLabel) {
    std::string text = showLabel ? std::string(icon) + "  " + label : std::string(icon);
    return ButtonWidth(text.c_str());
}

void App::DrawToolbar() {
    float s = dpiScale;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4 * s, 6 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(9 * s, 6 * s));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::Panel);
    ImGui::BeginChild("##toolbar", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPos(ImVec2(10 * s, 7 * s));

    struct ToolDef { Tool t; const char* icon; const char* name; const char* tip; };
    static const ToolDef tools[] = {
        { Tool::Select, ICON_SELECT, "Select", "Select, move and resize" },
        { Tool::Text, ICON_TEXT, "Text", "Edit or add text" },
        { Tool::Image, ICON_IMAGE, "Image", "Place a picture" },
        { Tool::Rect, ICON_RECT, "Rectangle", "Draw a rectangle" },
        { Tool::Cut, ICON_CROP, "Cut piece", "Lift part of a picture out" },
        { Tool::Redact, ICON_REDACT, "Redact", "Remove content permanently" },
        { Tool::Hand, ICON_PAN, "Pan", "Scroll the page" },
    };
    float gap = 14 * s;
    float searchW = std::clamp(ImGui::GetContentRegionAvail().x * 0.18f, 140 * s, 240 * s);
    float zoomW = 70 * s;
    // Labels only when everything fits on one row; otherwise icons with tooltips.
    auto rowWidth = [&](bool labels) {
        float w = 0, sp = ImGui::GetStyle().ItemSpacing.x;
        auto add = [&](float x) { w += x + sp; };
        add(ToolbarButtonWidth(ICON_OPEN, T("Open"), labels));
        add(ToolbarButtonWidth(ICON_SAVE, T("Save"), labels));
        w += gap;
        add(ToolbarButtonWidth(ICON_UNDO, T("Undo"), false));
        add(ToolbarButtonWidth(ICON_REDO, T("Redo"), false));
        w += gap;
        for (auto& e : tools) add(ToolbarButtonWidth(e.icon, T(e.name), labels));
        w += gap;
        add(ToolbarButtonWidth(ICON_ZOOMOUT, "", false));
        add(zoomW);
        add(ToolbarButtonWidth(ICON_ZOOMIN, "", false));
        add(ToolbarButtonWidth(ICON_FIT, "", false));
        w += gap;
        add(searchW);
        return w;
    };
    bool labels = rowWidth(true) <= ImGui::GetContentRegionAvail().x - 20 * s;
    bool has = doc.Raw() != nullptr;

    FlowRow row(ImGui::GetStyle().ItemSpacing.x);
    auto button = [&](const char* icon, const char* name, bool showLabel, bool active, const char* tip, bool enabled = true) {
        row.Before(ToolbarButtonWidth(icon, name, showLabel));
        ImGui::BeginDisabled(!enabled);
        bool r = ToolbarButton(icon, name, showLabel, active, tip);
        ImGui::EndDisabled();
        row.After();
        return r;
    };
    auto spacer = [&]() { row.Before(gap); ImGui::Dummy(ImVec2(gap - ImGui::GetStyle().ItemSpacing.x, 1)); row.After(); };

    if (button(ICON_OPEN, T("Open"), labels, false, K("Ctrl+O"))) OpenDialog();
    if (button(ICON_SAVE, T("Save"), labels, false, K("Ctrl+S"), has)) Save();
    spacer();
    if (button(ICON_UNDO, T("Undo"), false, false, K("Ctrl+Z"), doc.CanUndo())) Undo();
    if (button(ICON_REDO, T("Redo"), false, false, K("Ctrl+Y"), doc.CanRedo())) Redo();
    spacer();
    for (auto& e : tools)
        if (button(e.icon, T(e.name), labels, tool == e.t, T(e.tip), has)) { if (editing) CommitInlineEdit(); tool = e.t; }
    spacer();
    if (button(ICON_ZOOMOUT, T("Zoom out"), false, false, K("Ctrl -"), has)) { zoom = std::max(0.1f, zoom / 1.15f); InvalidateAll(); }
    row.Before(zoomW);
    ImGui::SetNextItemWidth(zoomW);
    ImGui::BeginDisabled(!has);
    int zp = (int)std::lround(zoom * 72.0f / 96.0f * 100.0f);
    if (ImGui::DragInt("##zoom", &zp, 1.0f, 10, 600, "%d%%")) { zoom = zp / 100.0f * 96.0f / 72.0f; InvalidateAll(); }
    ImGui::EndDisabled();
    row.After();
    if (button(ICON_ZOOMIN, T("Zoom in"), false, false, K("Ctrl +"), has)) { zoom = std::min(8.0f, zoom * 1.15f); InvalidateAll(); }
    if (button(ICON_FIT, T("Fit width"), false, false, K("Ctrl 0"), has)) fitPending = true;
    spacer();

    // Search: a box, then (once something was searched) previous / next and the count.
    row.Before(searchW);
    ImGui::SetNextItemWidth(searchW);
    if (focusSearch) { ImGui::SetKeyboardFocusHere(); focusSearch = false; }
    std::string hint = std::string(ICON_SEARCH "  ") + T("Search");
    ImGui::BeginDisabled(!has);
    bool enter = ImGui::InputTextWithHint("##find", hint.c_str(), &searchQuery, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::EndDisabled();
    row.After();
    auto runSearch = [&]() {
        searchHits = ops::SearchText(doc, U2W(searchQuery), searchCase, searchWhole);
        searchCurrent = searchHits.empty() ? -1 : 0;
        searchedFor = searchQuery;
        if (!searchHits.empty()) ScrollToRect(searchHits[0].page, searchHits[0].box);
    };
    if (searchDirty && !searchHits.empty() && !searchQuery.empty() && has) {
        searchHits = ops::SearchText(doc, U2W(searchQuery), searchCase, searchWhole);
        if (searchCurrent >= (int)searchHits.size()) searchCurrent = (int)searchHits.size() - 1;
    }
    searchDirty = false;
    if (enter && has && !searchQuery.empty()) runSearch();
    if (searchQuery.empty()) { searchHits.clear(); searchCurrent = -1; searchedFor.clear(); }
    if (!searchedFor.empty()) {
        auto step = [&](int d) {
            if (searchHits.empty()) return;
            searchCurrent = (searchCurrent + d + (int)searchHits.size()) % (int)searchHits.size();
            ScrollToRect(searchHits[searchCurrent].page, searchHits[searchCurrent].box);
        };
        if (button(ICON_PREV, T("Prev"), false, false, nullptr, !searchHits.empty())) step(-1);
        if (button(ICON_NEXT, T("Next"), false, false, nullptr, !searchHits.empty())) step(+1);
        std::string status = searchHits.empty() ? std::string(T("no matches"))
                                                : Fmt("%d / %d", searchCurrent + 1, (int)searchHits.size());
        row.Before(ImGui::CalcTextSize(status.c_str()).x);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", status.c_str());
        row.After();
        if (button("Aa", T("Match case"), false, searchCase, nullptr)) { searchCase = !searchCase; runSearch(); }
        if (button("ab|", T("Whole word"), false, searchWhole, nullptr)) { searchWhole = !searchWhole; runSearch(); }
    }

    std::string pg = has ? Fmt(T("Page %d / %d"), currentPage + 1, doc.PageCount()) : std::string();
    if (has && row.Fits(gap + ImGui::CalcTextSize(pg.c_str()).x)) {     // only when it fits on the row
        spacer();
        row.Before(ImGui::CalcTextSize(pg.c_str()).x);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", pg.c_str());
        row.After();
    }
    ImGui::Dummy(ImVec2(0, 1 * s));
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

// Search lives in the toolbar now; kept for the frame layout.
void App::DrawSearchBar() {}

// ---------------------------------------------------------------- thumbnails

void App::DrawThumbnails() {
    ImGui::TextUnformatted(T("Pages"));
    ImGui::Separator();
    int n = doc.PageCount();
    if ((int)thumbs.size() != n) thumbs.resize(n);
    int rendered = 0;
    // Thumbnails fill the panel, whatever width it has.
    float w = std::max(40.0f * dpiScale, ImGui::GetContentRegionAvail().x - 12.0f * dpiScale);
    for (int i = 0; i < n; ++i) {
        ImGui::PushID(i);
        FS_SIZEF sz = doc.PageSizePt(i);
        float h = w * sz.height / std::max(1.0f, sz.width);
        ImVec2 p = ImGui::GetCursorScreenPos();
        float availW = ImGui::GetContentRegionAvail().x;
        bool cur = (i == currentPage);
        if (ImGui::Selectable("##th", cur, 0, ImVec2(availW, h + 22 * dpiScale))) ScrollToPage(i);
        bool vis = ImGui::IsItemVisible();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 tl(p.x + (availW - w) / 2, p.y + 2);
        dl->AddRectFilled(tl, ImVec2(tl.x + w, tl.y + h), IM_COL32(255, 255, 255, 255));
        // Thumbnails render on their own (lazily, a couple per frame), so pages
        // that aren't on screen still show their content.
        ThumbTex& t = thumbs[i];
        uint32_t v = doc.PageVersion(i);
        int tw = std::max(8, (int)w), th = std::max(8, (int)h);
        if (vis && (t.version != v || !t.tex.Valid() || t.w != tw) && rendered < 2 && !dragging) {
            std::vector<uint32_t> buf;
            if (doc.Render(i, tw / std::max(1.0f, sz.width), 0, 0, tw, th, buf, true, true)) {
                t.tex.Upload(buf.data(), tw, th);
                t.version = v; t.w = tw;
            }
            ++rendered;
        }
        if (t.tex.Valid()) dl->AddImage(t.tex.Id(), tl, ImVec2(tl.x + w, tl.y + h));
        dl->AddRect(tl, ImVec2(tl.x + w, tl.y + h), cur ? IM_COL32(60, 150, 255, 255) : IM_COL32(90, 90, 100, 255));
        dl->AddText(ImVec2(tl.x, tl.y + h + 2), IM_COL32(170, 170, 180, 255), Fmt(T("Page %d"), i + 1).c_str());

        if (ImGui::BeginPopupContextItem("ctx")) {
            if (ImGui::MenuItem(T("Insert blank after")))
                if (ops::InsertBlankPage(doc, i + 1, sz.width, sz.height)) { ClearSelection(); CommitIfDirty("Insert page"); }
            if (ImGui::MenuItem(T("Duplicate")))
                if (ops::DuplicatePage(doc, i)) { ClearSelection(); CommitIfDirty("Duplicate page"); }
            if (ImGui::MenuItem(T("Rotate 90\xC2\xB0 right")))
                if (ops::RotatePage(doc, i, 1)) { ClearSelection(); CommitIfDirty("Rotate page"); }
            if (ImGui::MenuItem(T("Delete"), nullptr, false, n > 1))
                if (ops::DeletePages(doc, { i })) { ClearSelection(); CommitIfDirty("Delete page"); }
            ImGui::Separator();
            if (ImGui::MenuItem(T("Move up"), nullptr, false, i > 0))
                if (ops::MovePage(doc, i, i - 1)) { ClearSelection(); CommitIfDirty("Move page"); }
            if (ImGui::MenuItem(T("Move down"), nullptr, false, i < n - 1))
                if (ops::MovePage(doc, i, i + 1)) { ClearSelection(); CommitIfDirty("Move page"); }
            if (sel.Valid(doc) && sel.page != i) {
                ImGui::Separator();
                if (ImGui::MenuItem(T("Move selection to this page"))) TransferSelectionToPage(i);
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
}

// ------------------------------------------------------------ line thickness

// "Line thickness" for the selected lines - one line, several, or a whole
// table grid at once: a slider plus an exact number with - / + steps.
static void DrawLineThickness(App& a) {
    std::vector<FPDF_PAGEOBJECT> lines;
    bool single = a.groupSel.size() <= 1;
    if (single) {
        if (ops::ObjectType(a.sel.obj) == FPDF_PAGEOBJ_PATH) lines.push_back(a.sel.obj);
    } else {
        for (auto o : a.groupSel)
            if (ops::ObjectType(o) == FPDF_PAGEOBJ_PATH) lines.push_back(o);
    }
    if (lines.empty()) return;
    float scale = single ? ops::MatrixScale(a.sel.accum) : 1.0f;
    float cur = 0;
    for (auto o : lines) cur = std::max(cur, ops::LineThickness(o) * scale);
    auto apply = [&](float v) {
        v = std::clamp(v, 0.05f, 100.0f);
        if (single) a.EnsureSelectionEditable();
        std::vector<FPDF_PAGEOBJECT> targets = single ? std::vector<FPDF_PAGEOBJECT>{ a.sel.obj } : lines;
        for (auto o : targets) ops::SetLineThickness(a.doc, a.sel.page, o, v / (single ? ops::MatrixScale(a.sel.accum) : 1.0f));
        a.InvalidatePage(a.sel.page);
    };

    ImGui::Separator();
    ImGui::TextUnformatted(T("Line thickness"));
    if (lines.size() > 1) {
        ImGui::SameLine();
        ImGui::TextDisabled(T("(%d lines)"), (int)lines.size());
    }
    float v = cur;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##thickSlider", &v, 0.1f, 20.0f, "%.2f pt", ImGuiSliderFlags_Logarithmic)) apply(v);
    if (ImGui::IsItemDeactivatedAfterEdit()) a.CommitIfDirty("Line thickness");

    const float step = 0.25f;
    float bw = ImGui::GetFrameHeight() * 1.4f;
    float sp = ImGui::GetStyle().ItemInnerSpacing.x;
    if (ImGui::Button("-##thinner", ImVec2(bw, 0))) { apply(std::max(0.05f, cur - step)); a.CommitIfDirty("Line thickness"); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Thinner"));
    ImGui::SameLine(0, sp);
    float n = cur;
    ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x - bw - sp));
    if (ImGui::InputFloat("##thickValue", &n, 0.0f, 0.0f, "%.2f pt")) apply(n);
    if (ImGui::IsItemDeactivatedAfterEdit()) a.CommitIfDirty("Line thickness");
    ImGui::SameLine(0, sp);
    if (ImGui::Button("+##thicker", ImVec2(bw, 0))) { apply(cur + step); a.CommitIfDirty("Line thickness"); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Thicker"));
}

// ---------------------------------------------------------------- properties

void App::DrawProperties() {
    float s = dpiScale;
    if (ImGui::CollapsingHeader(T("Selection##sel"), ImGuiTreeNodeFlags_DefaultOpen)) {
        float xyW = std::min(90 * s, (ImGui::GetContentRegionAvail().x - 60 * s) * 0.5f);
        if (AnnotSelected()) {
            ImGui::TextWrapped("%s", T("Annotation (comment / markup / field)."));
            FlowRow row(ImGui::GetStyle().ItemSpacing.x);
            if (row.Button(T("Delete annotation"))) DeleteSelection();
            if (row.Button(T("Make annotations editable"))) FlattenCurrentPage();
        } else if (!sel.Valid(doc)) {
            ImGui::TextDisabled("%s", T("Nothing selected."));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", T("Click something on the page to edit it."));
            ImGui::PopStyleColor();
        } else if (groupSel.size() > 1) {
            if (selGroupId) ImGui::TextWrapped(T("Group of %d objects (moves as one)"), (int)groupSel.size());
            else ImGui::TextWrapped(T("%d objects selected"), (int)groupSel.size());
            PRect b = SelectionBounds();
            float left = b.l, bottom = b.b;
            ImGui::SetNextItemWidth(xyW); ImGui::DragFloat("X", &left, 0.5f, -10000, 10000, "%.1f");
            bool xe = ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SameLine(); ImGui::SetNextItemWidth(xyW); ImGui::DragFloat("Y", &bottom, 0.5f, -10000, 10000, "%.1f");
            bool ye = ImGui::IsItemDeactivatedAfterEdit();
            if (xe || ye) { MoveSelection(left - b.l, bottom - b.b); CommitIfDirty("Move group"); }
            ImGui::TextWrapped(T("Size: %.1f x %.1f pt"), b.W(), b.H());
            DrawLineThickness(*this);
            ImGui::Separator();
            FlowRow row(ImGui::GetStyle().ItemSpacing.x);
            if (selGroupId) { if (row.Button(T("Ungroup"))) UngroupSelection(); }
            else { if (row.Button(T("Group into one"))) GroupSelection(); }
            if (row.Button(T("Copy"))) CopySelection();
            if (row.Button(T("Delete all"))) DeleteSelection();
        } else {
            int t = ops::ObjectType(sel.obj);
            const char* tn = t == FPDF_PAGEOBJ_TEXT ? "Text" : t == FPDF_PAGEOBJ_IMAGE ? "Picture" :
                             t == FPDF_PAGEOBJ_PATH ? "Line / shape" : t == FPDF_PAGEOBJ_FORM ? "Group (XObject)" :
                             t == FPDF_PAGEOBJ_SHADING ? "Gradient" : "Object";
            ImGui::TextWrapped(T("%s  -  page %d"), T(tn), sel.page + 1);
            PRect b = sel.PageBounds();
            float left = b.l, bottom = b.b;
            ImGui::SetNextItemWidth(xyW); ImGui::DragFloat("X", &left, 0.5f, -10000, 10000, "%.1f");
            bool xe = ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SameLine(); ImGui::SetNextItemWidth(xyW); ImGui::DragFloat("Y", &bottom, 0.5f, -10000, 10000, "%.1f");
            bool ye = ImGui::IsItemDeactivatedAfterEdit();
            if (xe || ye) {
                EnsureSelectionEditable();
                ops::SetObjectPosPage(doc, sel.page, sel.obj, sel.accum, left, bottom);
                InvalidatePage(sel.page);
                CommitIfDirty("Move");
            }
            ImGui::TextWrapped(T("Size: %.1f x %.1f pt"), b.W(), b.H());

            if (t == FPDF_PAGEOBJ_TEXT) {
                ops::TextProps tp = ops::ReadText(doc, sel.page, sel.obj);
                double visual = tp.fontSize * ops::MatrixScale(sel.accum);
                ImGui::Separator();
                ImGui::TextWrapped(T("Font: %s%s"), tp.fontName.empty() ? T("(unnamed)") : StripSubsetPrefix(tp.fontName).c_str(),
                                   tp.embedded ? T("  (stored in this PDF)") : T("  (not stored - uses this PC's font)"));
                if (ImGui::IsItemHovered() && tp.embedded)
                    ImGui::SetTooltip("%s", T("The font file is stored inside this PDF."));
                ImGui::Text(T("Size: %.1f pt"), visual);
                if (ImGui::Button(T("Edit text  (Enter)"))) {
                    EnsureSelectionEditable();
                    BeginEditText(sel.page, sel.obj);
                }
                float col[4] = { tp.r / 255.0f, tp.g / 255.0f, tp.b / 255.0f, (tp.a ? tp.a : 255) / 255.0f };
                if (ImGui::ColorEdit4(T("Text colour##tc"), col, ImGuiColorEditFlags_NoInputs)) {
                    EnsureSelectionEditable();
                    ops::SetObjectFillColor(doc, sel.page, sel.obj, (unsigned)(col[0] * 255), (unsigned)(col[1] * 255),
                                            (unsigned)(col[2] * 255), (unsigned)(col[3] * 255));
                    InvalidatePage(sel.page);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) CommitIfDirty("Text colour");
                if (uiShotOpenAll) ImGui::SetNextItemOpen(true);
                if (ImGui::TreeNode(T("Change font / size###chfont"))) {
                    static float newSize = 0;
                    if (newSize <= 0 || ImGui::IsWindowAppearing()) newSize = (float)visual;
                    const auto& fl = FontList();
                    static int rIdx = -1;                 // -1 = keep the text's own font
                    if (rIdx >= (int)fl.size()) rIdx = -1;
                    std::string keepLbl = Fmt(T("Keep current font (%s)"), StripSubsetPrefix(tp.fontName).c_str());
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::BeginCombo("##rf", rIdx < 0 ? keepLbl.c_str() : fl[rIdx].label.c_str())) {
                        if (ImGui::Selectable(keepLbl.c_str(), rIdx < 0)) rIdx = -1;
                        ImGui::Separator();
                        for (int i = 0; i < (int)fl.size(); ++i) {
                            if (i == docFontCount && docFontCount > 0) ImGui::Separator();
                            if (ImGui::Selectable(fl[i].label.c_str(), i == rIdx)) rIdx = i;
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::SetNextItemWidth(-1);
                    ImGui::DragFloat("##rs", &newSize, 0.2f, 2.0f, 300.0f, T("Size %.1f pt"));
                    if (ImGui::Button(T("Apply"))) {
                        EnsureSelectionEditable();
                        if (rIdx < 0) {                   // size only: font, spacing and colour untouched
                            float sc = ops::MatrixScale(sel.accum);
                            if (ops::SetTextVisualSize(doc, sel.page, sel.obj, newSize / (sc > 0 ? sc : 1.0f))) {
                                InvalidatePage(sel.page);
                                CommitIfDirty("Change size");
                            }
                        } else {
                            FontChoice fc = fl[rIdx];
                            if (fc.IsDoc()) fc.handle = ops::FindDocFont(doc, fc.docName, sel.page);
                            std::string err;
                            FPDF_PAGEOBJECT out = nullptr;
                            if (ops::RestyleText(doc, sel.page, sel.obj, fc, newSize, err, &out)) {
                                SelectNewObject(sel.page, out);
                                InvalidatePage(sel.page);
                                CommitIfDirty("Change font");
                            } else Toasts(std::string(T("Could not change the font: ")) + T(err), true);
                        }
                    }
                    ImGui::TreePop();
                }
            }

            if (t == FPDF_PAGEOBJ_PATH) {
                ops::StrokeProps st = ops::ReadStroke(sel.obj);
                DrawLineThickness(*this);
                float lc[3] = { st.r / 255.0f, st.g / 255.0f, st.b / 255.0f };
                if (ImGui::ColorEdit3(T("Line colour##lc"), lc, ImGuiColorEditFlags_NoInputs)) {
                    EnsureSelectionEditable();
                    ops::SetObjectStroke(doc, sel.page, sel.obj, (unsigned)(lc[0] * 255), (unsigned)(lc[1] * 255),
                                         (unsigned)(lc[2] * 255), st.width);
                    InvalidatePage(sel.page);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) CommitIfDirty("Line colour");
                int parts = ops::SplittableParts(sel.obj);
                if (parts > 1 && ImGui::Button(Fmt(T("Split into %d separate lines"), parts).c_str())) SplitSelectedPath(false, 0, 0);
            }
            if (t == FPDF_PAGEOBJ_IMAGE) {
                ImGui::Separator();
                if (ImGui::Button((std::string(ICON_CROP "  ") + T("Cut piece tool")).c_str())) tool = Tool::Cut;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Lift part of a picture out"));
            }
            ImGui::Separator();
            FlowRow row(ImGui::GetStyle().ItemSpacing.x);
            if (row.Button(T("Copy"))) CopySelection();
            if (row.Button(T("Delete"))) DeleteSelection();
        }
    }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader(T("New text & shapes##newstyle"), ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto& fl = FontList();
        CheckboxWrap(T("New text matches the PDF's font"), &matchNearbyFont);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Uses the font, size and colour of the nearest text."));
        ImGui::BeginDisabled(matchNearbyFont);
        ImGui::TextUnformatted(T("Font"));
        ImGui::SetNextItemWidth(-1);
        const char* cur = (fontIndex >= 0 && fontIndex < (int)fl.size()) ? fl[fontIndex].label.c_str() : T("(none)");
        if (ImGui::BeginCombo("##font", cur)) {
            for (int i = 0; i < (int)fl.size(); ++i) {
                bool on = (i == fontIndex);
                if (i == docFontCount && docFontCount > 0) ImGui::Separator();
                if (ImGui::Selectable(fl[i].label.c_str(), on)) fontIndex = i;
                if (on) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::DragFloat("##size", &fontSize, 0.2f, 4.0f, 300.0f, T("Size %.1f pt"));
        ImGui::EndDisabled();
        ImGui::ColorEdit4(T("Fill / text colour##fill"), fillColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
        ImGui::ColorEdit4(T("Outline colour##stroke"), strokeColor, ImGuiColorEditFlags_NoInputs);
        {
            FlowRow row(ImGui::GetStyle().ItemSpacing.x);
            float cb = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
            const char* fillLbl = T("Filled");
            const char* outLbl = T("Outlined");
            row.Before(cb + ImGui::CalcTextSize(fillLbl).x);
            ImGui::Checkbox(fillLbl, &rectFilled);
            row.After();
            row.Before(cb + ImGui::CalcTextSize(outLbl).x);
            ImGui::Checkbox(outLbl, &rectStroked);
            row.After();
        }
        ImGui::SetNextItemWidth(std::min(120 * s, ImGui::GetContentRegionAvail().x * 0.45f));
        ImGui::DragFloat(T("Line width##lw"), &strokeWidth, 0.1f, 0.2f, 20.0f, "%.1f");
    }

    ImGui::Spacing();
    if (doc.Raw() && ImGui::CollapsingHeader(T("Form fields##ff"))) {
        auto fields = ops::ListFormFields(doc);
        if (fields.empty()) ImGui::TextDisabled("%s", T("No interactive form fields."));
        for (auto& f : fields) {
            ImGui::Bullet();
            ImGui::TextWrapped(T("p%d: %s = %s"), f.page + 1, f.name.empty() ? T("(unnamed)") : W2U(f.name).c_str(),
                               W2U(f.value).c_str());
        }
        if (!fields.empty() && ImGui::Button(T("Flatten form"))) DoFlatten();
    }
}

// ------------------------------------------------------- inline text editor

// Puts the caret at the end (no select-all) the first time the box gets focus.
static int CaretToEnd(ImGuiInputTextCallbackData* d) {
    bool* pending = static_cast<bool*>(d->UserData);
    if (*pending) {
        d->CursorPos = d->BufTextLen;
        d->SelectionStart = d->SelectionEnd = d->CursorPos;
        *pending = false;
    }
    return 0;
}

void App::DrawInlineEditor() {
    if (!editing) return;
    if (editPage < 0 || editPage >= (int)cache.size() || !cache[editPage].laid) { CommitInlineEdit(); return; }
    static bool caretPending = false;
    if (editFocus) caretPending = true;

    // Anchor on the text itself, at roughly the size it has on screen.
    ImVec2 pos;
    float textPx, minW;
    if (editObj) {
        PRect b = ops::ObjectBounds(editObj);
        pos = PageToScreenPt(editPage, b.l, b.t);
        float visual = ops::VisualFontSize(editObj);
        textPx = std::max(visual, 4.0f) * zoom;
        minW = b.W() * zoom;
    } else {
        float sz = (matchNearbyFont && pendingFontValid) ? (float)pendingFontSize : fontSize;
        pos = PageToScreenPt(editPage, editAtX, editAtY + sz * 0.9f);
        textPx = sz * zoom;
        minW = 160 * dpiScale;
    }
    // Same size as the text on the page (only tiny, zoomed-out text is enlarged
    // to stay readable) - a bigger box made editing look like a size change.
    float scale = std::clamp(std::max(textPx, 9.0f * dpiScale) / ImGui::GetFontSize(), 0.25f, 8.0f);
    // Keep the box on screen even for text near the right or bottom edge.
    ImVec2 vp0 = ImGui::GetMainViewport()->WorkPos, vs = ImGui::GetMainViewport()->WorkSize;
    pos.x = std::clamp(pos.x, vp0.x, vp0.x + vs.x * 0.8f);
    pos.y = std::clamp(pos.y, vp0.y, vp0.y + vs.y - 60 * dpiScale);
    ImGui::SetNextWindowPos(ImVec2(pos.x - 3, pos.y - 3));
    ImGui::SetNextWindowBgAlpha(0.98f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * dpiScale, 5 * dpiScale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6 * dpiScale);
    ImGui::PushStyleColor(ImGuiCol_Border, theme::Accent);
    ImGui::Begin("##inlinetext", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
    float hintW = ImGui::CalcTextSize(T("Enter = apply   Esc = cancel")).x;
    ImGui::SetWindowFontScale(scale);
    float want = std::max(ImGui::CalcTextSize(editBuf.c_str()).x + ImGui::GetFontSize() * 2.0f, hintW);
    float maxW = vp0.x + vs.x - pos.x - 8 * dpiScale;
    ImGui::SetNextItemWidth(std::min(std::max({ minW + 16, want, 120.0f * dpiScale }), std::max(120.0f * dpiScale, maxW)));
    if (editFocus) { ImGui::SetKeyboardFocusHere(); editFocus = false; }
    bool enter = ImGui::InputText("##it", &editBuf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways,
                                  CaretToEnd, &caretPending);
    bool done = ImGui::IsItemDeactivated();
    bool esc = ImGui::IsKeyPressed(ImGuiKey_Escape);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled("%s", T("Enter = apply   Esc = cancel"));
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    if (esc) CancelInlineEdit();
    else if (enter || done) CommitInlineEdit();
}

// ---------------------------------------------------------------- modals

void App::DrawModals() {
    float s = dpiScale;
    float btnMin = 90 * s;

    // password
    if (showPassword) { ImGui::OpenPopup("###pw"); showPassword = false; }
    FitNextWindowToScreen();
    if (ImGui::BeginPopupModal(T("Password required###pw"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", T("This PDF is encrypted. Enter its password:"));
        ImGui::SetNextItemWidth(std::min(260 * s, ImGui::GetMainViewport()->WorkSize.x * 0.8f));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool enter = ImGui::InputText("##pw", &pwInput, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
        if (!pwError.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "%s", T(pwError));
        if (PrimaryButton(T("Open"), ImVec2(ButtonWidth(T("Open"), btnMin), 0)) || enter) {
            std::string err;
            ClearLimbo();
            auto r = doc.LoadFile(pwFile, pwInput, err);
            if (r == Document::Load::Ok) {
                ResetAfterLoad();
                Toasts(Fmt(T("Opened %s"), W2U(doc.displayName).c_str()));
                ImGui::CloseCurrentPopup();
            } else if (r == Document::Load::NeedPassword) {
                pwError = "Wrong password, try again.";
            } else {
                pwError = err;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(ButtonWidth(T("Cancel"), btnMin), 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // metadata
    if (showMeta) { ImGui::OpenPopup("###meta"); showMeta = false; }
    FitNextWindowToScreen();
    if (ImGui::BeginPopupModal(T("Document properties###meta"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static const char* kLabels[] = { "File:", "Pages:", "Title:", "Author:", "Subject:", "Keywords:", "Creator:", "Producer:" };
        float labelW = 0;
        for (const char* l : kLabels) labelW = std::max(labelW, ImGui::CalcTextSize(T(l)).x);
        labelW += ImGui::GetFontSize();
        auto line = [&](const char* label, const std::string& value) {
            ImGui::TextUnformatted(T(label));
            ImGui::SameLine(labelW + ImGui::GetStyle().WindowPadding.x);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(520 * s, ImGui::GetMainViewport()->WorkSize.x * 0.6f));
            ImGui::TextWrapped("%s", value.c_str());
            ImGui::PopTextWrapPos();
        };
        line("File:", doc.path.empty() ? T("(unsaved)") : W2U(doc.path));
        line("Pages:", std::to_string(doc.PageCount()));
        int sigs = doc.Raw() ? FPDF_GetSignatureCount(doc.Raw()) : 0;
        if (sigs > 0) ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), T("Digital signatures: %d (editing will invalidate them)"), sigs);
        ImGui::Separator();
        const char* keys[] = { "Title", "Author", "Subject", "Keywords", "Creator", "Producer" };
        for (int i = 0; i < 6; ++i) {
            std::wstring v = ops::GetMeta(doc, keys[i]);
            line(kLabels[i + 2], v.empty() ? std::string("-") : W2U(v));
        }
        ImGui::Separator();
        if (ImGui::Button(T("Close"), ImVec2(ButtonWidth(T("Close"), btnMin), 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // import pages
    if (showImport) { ImGui::OpenPopup("###imp"); showImport = false; }
    FitNextWindowToScreen();
    if (ImGui::BeginPopupModal(T("Import pages###imp"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        float fieldW = std::min(400 * s, ImGui::GetMainViewport()->WorkSize.x * 0.8f);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + fieldW);
        ImGui::TextWrapped(T("From: %s"), W2U(importFile).c_str());
        ImGui::PopTextWrapPos();
        ImGui::SetNextItemWidth(fieldW);
        ImGui::InputTextWithHint("##range", T("page range, e.g. 1-3,5  (blank = all)"), &importRange);
        ImGui::SetNextItemWidth(fieldW);
        ImGui::InputTextWithHint("##ipw", T("password (if the source is encrypted)"), &importPw, ImGuiInputTextFlags_Password);
        int mode = importAt < 0 ? 0 : 1;
        {
            FlowRow row(ImGui::GetStyle().ItemSpacing.x);
            float rb = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
            const char* a = T("Append at end");
            const char* b = T("After current page");
            row.Before(rb + ImGui::CalcTextSize(a).x);
            ImGui::RadioButton(a, &mode, 0);
            row.After();
            row.Before(rb + ImGui::CalcTextSize(b).x);
            ImGui::RadioButton(b, &mode, 1);
            row.After();
        }
        importAt = mode == 0 ? -1 : currentPage + 1;
        if (PrimaryButton(T("Import"), ImVec2(ButtonWidth(T("Import"), btnMin), 0))) {
            std::string err;
            if (ops::ImportPagesFromFile(doc, importFile, importPw, importRange, importAt, err)) {
                ClearSelection();
                CommitIfDirty("Import pages");
                Toasts(T("Pages imported"));
                ImGui::CloseCurrentPopup();
            } else Toasts(std::string(T("Import failed: ")) + T(err), true);
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(ButtonWidth(T("Cancel"), btnMin), 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // extracted text
    if (showExtract) { ImGui::OpenPopup("###ex"); showExtract = false; }
    {
        ImVec2 vs = ImGui::GetMainViewport()->WorkSize;
        ImGui::SetNextWindowSize(ImVec2(std::min(640 * s, vs.x * 0.92f), std::min(480 * s, vs.y * 0.85f)), ImGuiCond_Appearing);
        CenterNextWindow();
    }
    if (ImGui::BeginPopupModal(T("Extracted text###ex"), nullptr, 0)) {
        FlowRow row(ImGui::GetStyle().ItemSpacing.x);
        if (row.Button(T("Copy"))) ImGui::SetClipboardText(extractedText.c_str());
        if (row.Button(T("Save as .txt..."))) {
            std::wstring out;
            if (SaveFileDialog(hwnd_, { { TW("Text files"), L"*.txt" } }, L"extracted.txt", L"txt", out)) {
                static const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
                std::vector<uint8_t> bytes(bom, bom + 3);
                bytes.insert(bytes.end(), extractedText.begin(), extractedText.end());
                if (WriteFileBytes(out, bytes.data(), bytes.size())) Toasts(T("Saved text file"));
                else Toasts(T("Could not write the text file"), true);
            }
        }
        if (row.Button(T("Close"))) ImGui::CloseCurrentPopup();
        ImGui::InputTextMultiline("##extext", &extractedText, ImVec2(-1, -1), ImGuiInputTextFlags_ReadOnly);
        ImGui::EndPopup();
    }

    // table / form
    if (showTable) { ImGui::OpenPopup("###tbl"); showTable = false; }
    FitNextWindowToScreen();
    if (ImGui::BeginPopupModal(T("Insert table / form###tbl"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        float numW = 120 * s;
        ImGui::SetNextItemWidth(numW); ImGui::InputInt(T("Columns##cols"), &tableCols);
        ImGui::SetNextItemWidth(numW); ImGui::InputInt(T("Rows (body)##rows"), &tableRows);
        tableCols = std::clamp(tableCols, 1, 100);
        tableRows = std::clamp(tableRows, 1, 200);
        ImGui::Checkbox(T("Header row##hdr"), &tableHeader);
        if (tableHeader) {
            ImGui::SameLine(); ImGui::SetNextItemWidth(100 * s);
            ImGui::DragFloat(T("Header height##hh"), &tableHeaderH, 0.5f, 6, 200, "%.0f pt");
        }
        ImGui::SetNextItemWidth(numW); ImGui::DragFloat(T("Margin##mg"), &tableMargin, 0.5f, 0, 300, "%.0f pt");
        ImGui::SetNextItemWidth(numW); ImGui::DragFloat(T("Line width##tlw"), &tableLineW, 0.05f, 0.2f, 10, "%.2f");
        ImGui::ColorEdit3(T("Line colour##tlc"), tableColor, ImGuiColorEditFlags_NoInputs);
        ImGui::Separator();
        if (PrimaryButton(T("Create"), ImVec2(ButtonWidth(T("Create"), 110 * s), 0))) {
            FS_SIZEF sz = doc.PageSizePt(currentPage);
            PRect box{ tableMargin, tableMargin, sz.width - tableMargin, sz.height - tableMargin };
            int n = ops::AddTable(doc, currentPage, box, tableCols, tableRows, tableHeader, tableHeaderH,
                                  (unsigned)(tableColor[0] * 255), (unsigned)(tableColor[1] * 255),
                                  (unsigned)(tableColor[2] * 255), tableLineW);
            if (n > 0) {
                ClearSelection();
                CommitIfDirty("Insert table");
                Toasts(Fmt(T("Table added (%d editable lines)"), n));
                tool = Tool::Select;
                ImGui::CloseCurrentPopup();
            } else Toasts(T("Could not build the table (check the margin vs page size)"), true);
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(ButtonWidth(T("Cancel"), 110 * s), 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // help: two columns that size to the (translated) shortcut names; the
    // body scrolls when the screen is small.
    if (showHelp) { ImGui::OpenPopup("###help"); showHelp = false; }
    {
        ImVec2 vs = ImGui::GetMainViewport()->WorkSize;
        ImGui::SetNextWindowSize(ImVec2(std::min(760 * s, vs.x * 0.94f), 0), ImGuiCond_Always);
        CenterNextWindow();
    }
    if (ImGui::BeginPopupModal(T("How to###help"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(0.55f, 0.8f, 1, 1), "%s", T("PDF Editor"));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", T("by TheOCProductions & ShortGG"));
        ImGui::Separator();
        struct Row { const char* k; const char* v; };
        static const Row kRows[] = {
            { nullptr, "Selecting" },
            { "Click", "select the line, text or picture under the mouse" },
            { "Drag on empty space", "select everything the box touches" },
            { "Ctrl+A / Ctrl+Shift+A", "select everything / only lines on the page" },
            { "Alt+click", "pick one piece out of a group" },
            { nullptr, "Moving & sizing" },
            { "Drag", "move; drop it on another page to move it there" },
            { "Arrow keys", "nudge 1 pt (Shift = 10 pt, Ctrl = 0.25 pt)" },
            { "Alt+arrows / handles", "resize" },
            { "Double-click a line", "split a drawing / table cell into separate lines" },
            { "Cut piece tool", "lift part of a picture/scan out as a movable piece" },
            { nullptr, "Text" },
            { "Double-click / Enter", "type into existing text (font and size stay the same)" },
            { "Text tool + click", "edit existing text, or add new text in the PDF's font" },
            { nullptr, "Clipboard" },
            { "Ctrl+C / Ctrl+X", "copy / cut (exact fonts and pictures)" },
            { "Ctrl+V", "paste on the page under the mouse, same position" },
            { "Ctrl+Shift+V", "paste at the mouse; also pastes text copied from other programs" },
            { nullptr, "View" },
            { "Ctrl+wheel / Ctrl +/-", "zoom;  Ctrl+0 fit width" },
            { "Wheel / Shift+wheel", "scroll up-down / left-right; middle-drag pans" },
            { "Right-click", "group, ungroup, split, copy, paste here, move to page" },
        };
        float keyW = 0;
        for (auto& r : kRows) if (r.k) keyW = std::max(keyW, ImGui::CalcTextSize(T(r.k)).x);
        // Very narrow windows: the key column may take at most 45% of the width.
        keyW = std::min(keyW + ImGui::GetFontSize(), ImGui::GetContentRegionAvail().x * 0.45f);
        float bodyH = ImGui::GetMainViewport()->WorkSize.y * 0.92f - ImGui::GetFrameHeightWithSpacing() * 4.0f;
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, std::max(120 * s, bodyH)));
        ImGui::BeginChild("##helpbody", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY);
        for (auto& r : kRows) {
            if (!r.k) {
                if (&r != &kRows[0]) ImGui::Separator();
                ImGui::TextDisabled("%s", T(r.v));
                continue;
            }
            float y = ImGui::GetCursorPosY();
            ImGui::PushTextWrapPos(keyW - ImGui::GetFontSize() * 0.5f);
            ImGui::TextColored(ImVec4(0.55f, 0.8f, 1, 1), "%s", T(r.k));
            ImGui::PopTextWrapPos();
            float y2 = ImGui::GetCursorPosY();
            ImGui::SetCursorPos(ImVec2(keyW, y));
            ImGui::TextWrapped("%s", T(r.v));
            if (ImGui::GetCursorPosY() < y2) ImGui::SetCursorPosY(y2);
        }
        ImGui::EndChild();
        if (ImGui::Button(T("Close"), ImVec2(ButtonWidth(T("Close"), 100 * s), 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // about
    if (showAbout) { ImGui::OpenPopup("###ab"); showAbout = false; }
    FitNextWindowToScreen();
    if (ImGui::BeginPopupModal(T("About###ab"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(T("PDF Editor"));
        ImGui::TextColored(ImVec4(0.55f, 0.8f, 1, 1), "%s", T("by TheOCProductions & ShortGG"));
        ImGui::Separator();
        ImGui::TextDisabled("%s", T("Rendering: PDFium (BSD-3).  UI: Dear ImGui (MIT)."));
        if (ImGui::Button(T("Close"), ImVec2(ButtonWidth(T("Close"), 100 * s), 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // settings
    if (showSettings) { ImGui::OpenPopup("###settings"); showSettings = false; }
    FitNextWindowToScreen();
    if (ImGui::BeginPopupModal(T("Settings###settings"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        float comboW = std::min(260 * s, ImGui::GetMainViewport()->WorkSize.x * 0.7f);
        ImGui::TextUnformatted(T("Language"));
        ImGui::SetNextItemWidth(comboW);
        if (ImGui::BeginCombo("##lang", Language() == Lang::German ? "Deutsch" : "English")) {
            if (ImGui::Selectable("English", Language() == Lang::English)) ChangeLanguage(Lang::English);
            if (ImGui::Selectable("Deutsch", Language() == Lang::German)) ChangeLanguage(Lang::German);
            ImGui::EndCombo();
        }
        ImGui::Spacing();
        ImGui::TextUnformatted(T("Interface size"));
        static const int kSizes[] = { 75, 85, 100, 115, 125, 150, 175, 200 };
        float curF = UiScaleSetting() > 0 ? UiScaleSetting() : 1.0f;
        int cur = (int)std::lround(curF * 100.0f);
        auto sizeLabel = [](int pct) {
            return pct == 100 ? std::string(T("100% (like Windows)")) : std::to_string(pct) + "%";
        };
        ImGui::SetNextItemWidth(comboW);
        if (ImGui::BeginCombo("##uiscale", sizeLabel(cur).c_str())) {
            for (int pct : kSizes)
                if (ImGui::Selectable(sizeLabel(pct).c_str(), pct == cur)) ChangeUiScale(pct / 100.0f);
            ImGui::EndCombo();
        }
        ImGui::Spacing();
        if (ImGui::Button(T("Close"), ImVec2(ButtonWidth(T("Close"), 100 * s), 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
