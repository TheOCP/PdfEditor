#include "app.h"
#include "dialogs.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include <fpdf_signature.h>
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------- menu bar

void App::DrawMenuBar() {
    if (!ImGui::BeginMenuBar()) return;
    bool has = doc.Raw() != nullptr;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New", "Ctrl+N")) NewDoc();
        if (ImGui::MenuItem("Open...", "Ctrl+O")) OpenDialog();
        ImGui::Separator();
        if (ImGui::MenuItem("Save", "Ctrl+S", false, has)) Save();
        if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S", false, has)) SaveAs();
        ImGui::Separator();
        if (ImGui::MenuItem("Import pages from another PDF...", nullptr, false, has)) {
            std::wstring f;
            if (OpenFileDialog(hwnd_, { { L"PDF documents", L"*.pdf" } }, f)) {
                importFile = f; importRange.clear(); importPw.clear(); importAt = -1; showImport = true;
            }
        }
        if (ImGui::BeginMenu("Export pages as PNG", has)) {
            if (ImGui::MenuItem("96 DPI (screen)")) ExportImagesDialog(96);
            if (ImGui::MenuItem("150 DPI")) ExportImagesDialog(150);
            if (ImGui::MenuItem("300 DPI (print)")) ExportImagesDialog(300);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Extract all text...", nullptr, false, has)) ExtractTextDialog();
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Alt+F4")) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, doc.CanUndo())) Undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, doc.CanRedo())) Redo();
        ImGui::Separator();
        bool haveSel = sel.Valid(doc) || AnnotSelected();
        const char* cbt = ImGui::GetClipboardText();
        bool canPaste = has && (!clip.empty() || !limbo.empty() || (cbt && *cbt));
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, sel.Valid(doc))) CopySelection();
        if (ImGui::MenuItem("Cut", "Ctrl+X", false, haveSel)) CutSelection();
        if (ImGui::MenuItem("Paste (same position)", "Ctrl+V", false, canPaste)) PasteClipboard(false);
        if (ImGui::MenuItem("Paste at mouse", "Ctrl+Shift+V", false, canPaste)) PasteClipboard(true);
        if (ImGui::MenuItem("Delete selected", "Del", false, haveSel)) DeleteSelection();
        ImGui::Separator();
        if (ImGui::MenuItem("Select all on page", "Ctrl+A", false, has)) SelectAllOnPage(false);
        if (ImGui::MenuItem("Select all lines (skip text/images)", "Ctrl+Shift+A", false, has)) SelectAllOnPage(true);
        ImGui::Separator();
        if (ImGui::MenuItem("Group all lines on page", nullptr, false, has)) GroupAllLines();
        if (ImGui::MenuItem("Group selection", nullptr, false, groupSel.size() > 1 && !selGroupId)) GroupSelection();
        if (ImGui::MenuItem("Ungroup", nullptr, false, selGroupId != 0)) UngroupSelection();
        bool splittable = sel.Valid(doc) && groupSel.size() <= 1 && ops::SplittableParts(sel.obj) > 1;
        if (ImGui::MenuItem("Split selected line drawing into pieces", nullptr, false, splittable)) SplitSelectedPath(false, 0, 0);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Page")) {
        int cp = currentPage;
        FS_SIZEF sz = doc.PageCount() ? doc.PageSizePt(cp) : FS_SIZEF{ 612, 792 };
        if (ImGui::MenuItem("Insert blank after current", nullptr, false, has))
            if (ops::InsertBlankPage(doc, cp + 1, sz.width, sz.height)) { ClearSelection(); CommitIfDirty("Insert page"); }
        if (ImGui::MenuItem("Duplicate current", nullptr, false, has))
            if (ops::DuplicatePage(doc, cp)) { ClearSelection(); CommitIfDirty("Duplicate page"); }
        if (ImGui::MenuItem("Delete current", nullptr, false, has && doc.PageCount() > 1))
            if (ops::DeletePages(doc, { cp })) { ClearSelection(); CommitIfDirty("Delete page"); }
        ImGui::Separator();
        if (ImGui::MenuItem("Rotate current 90\xC2\xB0 right", nullptr, false, has))
            if (ops::RotatePage(doc, cp, 1)) { ClearSelection(); CommitIfDirty("Rotate page"); }
        if (ImGui::MenuItem("Rotate current 90\xC2\xB0 left", nullptr, false, has))
            if (ops::RotatePage(doc, cp, -1)) { ClearSelection(); CommitIfDirty("Rotate page"); }
        ImGui::Separator();
        if (ImGui::MenuItem("Make this page's annotations editable", nullptr, false, has)) FlattenCurrentPage();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Insert")) {
        if (ImGui::MenuItem("Table / form...", nullptr, false, has)) showTable = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Text (click to place)", nullptr, tool == Tool::Text, has)) tool = Tool::Text;
        if (ImGui::MenuItem("Image (click to place)", nullptr, tool == Tool::Image, has)) tool = Tool::Image;
        if (ImGui::MenuItem("Rectangle (drag)", nullptr, tool == Tool::Rect, has)) tool = Tool::Rect;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Document")) {
        if (ImGui::MenuItem("Flatten all forms && annotations", nullptr, false, has)) DoFlatten();
        if (ImGui::MenuItem("Properties / metadata", nullptr, false, has)) showMeta = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Page thumbnails", nullptr, &showThumbs);
        ImGui::MenuItem("Properties panel", nullptr, &showProps);
        ImGui::Separator();
        if (ImGui::MenuItem("Zoom in", "Ctrl +")) { zoom = std::min(8.0f, zoom * 1.15f); InvalidateAll(); }
        if (ImGui::MenuItem("Zoom out", "Ctrl -")) { zoom = std::max(0.1f, zoom / 1.15f); InvalidateAll(); }
        if (ImGui::MenuItem("Fit width", "Ctrl 0")) fitPending = true;
        if (ImGui::MenuItem("Actual size (100%)", "Ctrl 1")) { zoom = 96.0f / 72.0f; InvalidateAll(); }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("How to / keyboard shortcuts", "F1")) showHelp = true;
        if (ImGui::MenuItem("About")) showAbout = true;
        ImGui::Separator();
        ImGui::TextDisabled("by TheOCProductions & ShortGG");
        ImGui::EndMenu();
    }
    if (has) {
        std::string title = W2U(doc.displayName) + (doc.modified ? "  (unsaved)" : "");
        float w = ImGui::CalcTextSize(title.c_str()).x;
        ImGui::SameLine(ImGui::GetWindowWidth() - w - 20 * dpiScale);
        ImGui::TextDisabled("%s", title.c_str());
    }
    ImGui::EndMenuBar();
}

// ---------------------------------------------------------------- toolbar

static bool ToolButton(const char* label, bool active) {
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    bool c = ImGui::Button(label);
    if (active) ImGui::PopStyleColor();
    return c;
}

void App::DrawToolbar() {
    float s = dpiScale;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6 * s, 6 * s));
    ImGui::BeginChild("##toolbar", ImVec2(0, ImGui::GetFrameHeight() + 14 * s), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPos(ImVec2(10 * s, 7 * s));
    struct { Tool t; const char* name; const char* tip; } tools[] = {
        { Tool::Select, "Select", "Click / drag a box to select. Drag to move (also onto another page).\n"
                                  "Double-click text to type, double-click a line drawing to split it.\n"
                                  "Arrows nudge, Alt+arrows resize, right-click for more." },
        { Tool::Text, "Text", "Click existing text to edit it, or empty space to add new text\n(matches the PDF's font)" },
        { Tool::Image, "Image", "Click to place a picture" },
        { Tool::Rect, "Rectangle", "Drag to draw a box" },
        { Tool::Cut, "Cut piece", "Drag over part of a picture or scan to lift it out as a\nmovable piece - for lines that are only pixels" },
        { Tool::Redact, "Redact", "Drag to permanently remove what is under the box\n(text, pictures, annotations)" },
        { Tool::Hand, "Pan", "Drag to scroll the page (middle mouse also works)" },
    };
    for (auto& e : tools) {
        if (ToolButton(e.name, tool == e.t)) { if (editing) CommitInlineEdit(); tool = e.t; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", e.tip);
        ImGui::SameLine();
    }
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::Button("Undo")) Undo();
    ImGui::SameLine();
    if (ImGui::Button("Redo")) Redo();
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120 * s);
    int zp = (int)std::lround(zoom * 72.0f / 96.0f * 100.0f);
    if (ImGui::SliderInt("##zoom", &zp, 10, 600, "%d%%")) { zoom = zp / 100.0f * 96.0f / 72.0f; InvalidateAll(); }
    ImGui::SameLine();
    if (doc.Raw()) ImGui::TextDisabled("Page %d / %d", currentPage + 1, doc.PageCount());
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------- search bar

void App::DrawSearchBar() {
    float s = dpiScale;
    ImGui::BeginChild("##searchbar", ImVec2(0, ImGui::GetFrameHeight() + 10 * s), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPos(ImVec2(10 * s, 5 * s));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Find:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260 * s);
    if (focusSearch) { ImGui::SetKeyboardFocusHere(); focusSearch = false; }
    bool enter = ImGui::InputTextWithHint("##find", "text to search", &searchQuery, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    bool go = ImGui::Button("Search");
    ImGui::SameLine();
    ImGui::Checkbox("Aa", &searchCase);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Match case");
    ImGui::SameLine();
    ImGui::Checkbox("Whole word", &searchWhole);
    ImGui::SameLine();

    // Results follow the document: re-run after edits so highlights never go stale.
    if (searchDirty && !searchHits.empty() && !searchQuery.empty() && doc.Raw()) {
        searchHits = ops::SearchText(doc, U2W(searchQuery), searchCase, searchWhole);
        if (searchCurrent >= (int)searchHits.size()) searchCurrent = (int)searchHits.size() - 1;
    }
    searchDirty = false;
    if ((enter || go) && doc.Raw() && !searchQuery.empty()) {
        searchHits = ops::SearchText(doc, U2W(searchQuery), searchCase, searchWhole);
        searchCurrent = searchHits.empty() ? -1 : 0;
        if (!searchHits.empty()) ScrollToRect(searchHits[0].page, searchHits[0].box);
        Toasts(Fmt("%d match(es)", (int)searchHits.size()));
    }
    auto step = [&](int d) {
        if (searchHits.empty()) return;
        searchCurrent = (searchCurrent + d + (int)searchHits.size()) % (int)searchHits.size();
        ScrollToRect(searchHits[searchCurrent].page, searchHits[searchCurrent].box);
    };
    if (ImGui::Button("Prev")) step(-1);
    ImGui::SameLine();
    if (ImGui::Button("Next")) step(+1);
    ImGui::SameLine();
    if (!searchHits.empty()) ImGui::Text("%d / %d", searchCurrent + 1, (int)searchHits.size());
    else if (!searchQuery.empty()) ImGui::TextDisabled("no matches");
    if (ImGui::Button("Clear")) { searchHits.clear(); searchCurrent = -1; }
    ImGui::EndChild();
}

// ---------------------------------------------------------------- thumbnails

void App::DrawThumbnails() {
    ImGui::TextUnformatted("Pages");
    ImGui::Separator();
    int n = doc.PageCount();
    if ((int)thumbs.size() != n) thumbs.resize(n);
    int rendered = 0;
    float w = 140.0f * dpiScale;
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
        if (vis && (t.version != v || !t.tex.Valid()) && rendered < 2 && !dragging) {
            int tw = std::max(8, (int)w), th = std::max(8, (int)h);
            std::vector<uint32_t> buf;
            if (doc.Render(i, tw / std::max(1.0f, sz.width), 0, 0, tw, th, buf, true, true)) {
                t.tex.Upload(buf.data(), tw, th);
                t.version = v; t.w = tw;
            }
            ++rendered;
        }
        if (t.tex.Valid()) dl->AddImage(t.tex.Id(), tl, ImVec2(tl.x + w, tl.y + h));
        dl->AddRect(tl, ImVec2(tl.x + w, tl.y + h), cur ? IM_COL32(60, 150, 255, 255) : IM_COL32(90, 90, 100, 255));
        dl->AddText(ImVec2(tl.x, tl.y + h + 2), IM_COL32(170, 170, 180, 255), Fmt("Page %d", i + 1).c_str());

        if (ImGui::BeginPopupContextItem("ctx")) {
            if (ImGui::MenuItem("Insert blank after"))
                if (ops::InsertBlankPage(doc, i + 1, sz.width, sz.height)) { ClearSelection(); CommitIfDirty("Insert page"); }
            if (ImGui::MenuItem("Duplicate"))
                if (ops::DuplicatePage(doc, i)) { ClearSelection(); CommitIfDirty("Duplicate page"); }
            if (ImGui::MenuItem("Rotate 90\xC2\xB0 right"))
                if (ops::RotatePage(doc, i, 1)) { ClearSelection(); CommitIfDirty("Rotate page"); }
            if (ImGui::MenuItem("Delete", nullptr, false, n > 1))
                if (ops::DeletePages(doc, { i })) { ClearSelection(); CommitIfDirty("Delete page"); }
            ImGui::Separator();
            if (ImGui::MenuItem("Move up", nullptr, false, i > 0))
                if (ops::MovePage(doc, i, i - 1)) { ClearSelection(); CommitIfDirty("Move page"); }
            if (ImGui::MenuItem("Move down", nullptr, false, i < n - 1))
                if (ops::MovePage(doc, i, i + 1)) { ClearSelection(); CommitIfDirty("Move page"); }
            if (sel.Valid(doc) && sel.page != i) {
                ImGui::Separator();
                if (ImGui::MenuItem("Move selection to this page")) TransferSelectionToPage(i);
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------- properties

void App::DrawProperties() {
    float s = dpiScale;
    if (ImGui::CollapsingHeader("New text & shapes", ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto& fl = FontList();
        ImGui::Checkbox("New text matches the PDF's font", &matchNearbyFont);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("New text copies the font, size and colour of the nearest existing text.\n"
                              "Turn off to use the font, size and colour chosen below.");
        ImGui::BeginDisabled(matchNearbyFont);
        ImGui::TextUnformatted("Font");
        ImGui::SetNextItemWidth(-1);
        const char* cur = (fontIndex >= 0 && fontIndex < (int)fl.size()) ? fl[fontIndex].label.c_str() : "(none)";
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
        ImGui::DragFloat("##size", &fontSize, 0.2f, 4.0f, 300.0f, "Size %.1f pt");
        ImGui::EndDisabled();
        ImGui::ColorEdit4("Fill / text colour", fillColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
        ImGui::ColorEdit4("Outline colour", strokeColor, ImGuiColorEditFlags_NoInputs);
        ImGui::Checkbox("Filled", &rectFilled);
        ImGui::SameLine();
        ImGui::Checkbox("Outlined", &rectStroked);
        ImGui::SetNextItemWidth(120 * s);
        ImGui::DragFloat("Line width", &strokeWidth, 0.1f, 0.2f, 20.0f, "%.1f");
    }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Selection", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (AnnotSelected()) {
            ImGui::TextWrapped("Annotation (comment / markup / field).");
            ImGui::TextDisabled("Drag it or use the arrow keys to move it.");
            if (ImGui::Button("Delete annotation")) DeleteSelection();
            if (ImGui::Button("Make this page's annotations editable")) FlattenCurrentPage();
            ImGui::TextWrapped("That turns them into normal lines/text you can split, group and edit.");
        } else if (!sel.Valid(doc)) {
            ImGui::TextDisabled("Nothing selected.");
            ImGui::TextWrapped("Click anything, or drag a box over several things. Double-click text to type. "
                               "Right-click for group, split, copy and more. F1 shows all shortcuts.");
        } else if (groupSel.size() > 1) {
            if (selGroupId) ImGui::Text("Group of %d objects (moves as one)", (int)groupSel.size());
            else ImGui::Text("%d objects selected", (int)groupSel.size());
            PRect b = SelectionBounds();
            float left = b.l, bottom = b.b;
            ImGui::SetNextItemWidth(90 * s); ImGui::DragFloat("X", &left, 0.5f, -10000, 10000, "%.1f");
            bool xe = ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SameLine(); ImGui::SetNextItemWidth(90 * s); ImGui::DragFloat("Y", &bottom, 0.5f, -10000, 10000, "%.1f");
            bool ye = ImGui::IsItemDeactivatedAfterEdit();
            if (xe || ye) { MoveSelection(left - b.l, bottom - b.b); CommitIfDirty("Move group"); }
            ImGui::Text("Size: %.1f x %.1f pt", b.W(), b.H());
            ImGui::TextDisabled("Arrows move - Alt+arrows or the handles resize");
            ImGui::TextDisabled("Alt+click / double-click one piece to adjust it alone");
            ImGui::Separator();
            if (selGroupId) { if (ImGui::Button("Ungroup")) UngroupSelection(); }
            else { if (ImGui::Button("Group into one")) GroupSelection(); }
            ImGui::SameLine();
            if (ImGui::Button("Copy")) CopySelection();
            ImGui::SameLine();
            if (ImGui::Button("Delete all")) DeleteSelection();
        } else {
            int t = ops::ObjectType(sel.obj);
            const char* tn = t == FPDF_PAGEOBJ_TEXT ? "Text" : t == FPDF_PAGEOBJ_IMAGE ? "Picture" :
                             t == FPDF_PAGEOBJ_PATH ? "Line / shape" : t == FPDF_PAGEOBJ_FORM ? "Group (XObject)" :
                             t == FPDF_PAGEOBJ_SHADING ? "Gradient" : "Object";
            ImGui::Text("%s  -  page %d", tn, sel.page + 1);
            if (sel.parent) ImGui::TextDisabled("(inside a group; it is split out when you edit it)");
            PRect b = sel.PageBounds();
            float left = b.l, bottom = b.b;
            ImGui::SetNextItemWidth(90 * s); ImGui::DragFloat("X", &left, 0.5f, -10000, 10000, "%.1f");
            bool xe = ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SameLine(); ImGui::SetNextItemWidth(90 * s); ImGui::DragFloat("Y", &bottom, 0.5f, -10000, 10000, "%.1f");
            bool ye = ImGui::IsItemDeactivatedAfterEdit();
            if (xe || ye) {
                EnsureSelectionEditable();
                ops::SetObjectPosPage(doc, sel.page, sel.obj, sel.accum, left, bottom);
                InvalidatePage(sel.page);
                CommitIfDirty("Move");
            }
            ImGui::Text("Size: %.1f x %.1f pt", b.W(), b.H());

            if (t == FPDF_PAGEOBJ_TEXT) {
                ops::TextProps tp = ops::ReadText(doc, sel.page, sel.obj);
                double visual = tp.fontSize * ops::MatrixScale(sel.accum);
                ImGui::Separator();
                ImGui::TextWrapped("Font: %s%s", tp.fontName.empty() ? "(unnamed)" : StripSubsetPrefix(tp.fontName).c_str(),
                                   tp.embedded ? "  (stored in this PDF)" : "  (not stored - uses this PC's font)");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Stored in this PDF = the font file is inside the PDF itself, not installed in\n"
                                      "Windows. Usually only the characters the PDF uses are included.\n"
                                      "It is listed at the top of the font lists while this PDF is open.");
                ImGui::Text("Size: %.1f pt", visual);
                if (ImGui::Button("Edit text  (Enter)")) {
                    EnsureSelectionEditable();
                    BeginEditText(sel.page, sel.obj);
                }
                float col[4] = { tp.r / 255.0f, tp.g / 255.0f, tp.b / 255.0f, (tp.a ? tp.a : 255) / 255.0f };
                if (ImGui::ColorEdit4("Text colour", col, ImGuiColorEditFlags_NoInputs)) {
                    EnsureSelectionEditable();
                    ops::SetObjectFillColor(doc, sel.page, sel.obj, (unsigned)(col[0] * 255), (unsigned)(col[1] * 255),
                                            (unsigned)(col[2] * 255), (unsigned)(col[3] * 255));
                    InvalidatePage(sel.page);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) CommitIfDirty("Text colour");
                if (ImGui::TreeNode("Change font / size")) {
                    ImGui::TextWrapped("Editing text never changes its font or size. Use this only to switch on purpose.");
                    static float newSize = 0;
                    if (newSize <= 0 || ImGui::IsWindowAppearing()) newSize = (float)visual;
                    const auto& fl = FontList();
                    static int rIdx = -1;                 // -1 = keep the text's own font
                    if (rIdx >= (int)fl.size()) rIdx = -1;
                    std::string keepLbl = "Keep current font (" + StripSubsetPrefix(tp.fontName) + ")";
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
                    ImGui::DragFloat("##rs", &newSize, 0.2f, 2.0f, 300.0f, "Size %.1f pt");
                    if (ImGui::Button("Apply")) {
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
                            } else Toasts("Could not change the font: " + err, true);
                        }
                    }
                    ImGui::TreePop();
                }
            }

            if (t == FPDF_PAGEOBJ_PATH) {
                ops::StrokeProps st = ops::ReadStroke(sel.obj);
                ImGui::Separator();
                float lw = ops::LineThickness(sel.obj) * ops::MatrixScale(sel.accum);
                ImGui::SetNextItemWidth(-1);
                if (ImGui::DragFloat("##thick", &lw, 0.05f, 0.1f, 60.0f, "Thickness %.2f pt")) {
                    EnsureSelectionEditable();
                    ops::SetLineThickness(doc, sel.page, sel.obj, lw);
                    InvalidatePage(sel.page);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) CommitIfDirty("Line thickness");
                float lc[3] = { st.r / 255.0f, st.g / 255.0f, st.b / 255.0f };
                if (ImGui::ColorEdit3("Line colour", lc, ImGuiColorEditFlags_NoInputs)) {
                    EnsureSelectionEditable();
                    ops::SetObjectStroke(doc, sel.page, sel.obj, (unsigned)(lc[0] * 255), (unsigned)(lc[1] * 255),
                                         (unsigned)(lc[2] * 255), st.width);
                    InvalidatePage(sel.page);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) CommitIfDirty("Line colour");
                int parts = ops::SplittableParts(sel.obj);
                if (parts > 1 && ImGui::Button(Fmt("Split into %d separate lines", parts).c_str())) SplitSelectedPath(false, 0, 0);
            }
            if (t == FPDF_PAGEOBJ_IMAGE) {
                ImGui::Separator();
                ImGui::TextWrapped("To move only part of this picture (e.g. a line in a scan), use the Cut piece tool and drag a box over it.");
                if (ImGui::Button("Cut piece tool")) tool = Tool::Cut;
            }
            ImGui::Separator();
            if (ImGui::Button("Copy")) CopySelection();
            ImGui::SameLine();
            if (ImGui::Button("Delete")) DeleteSelection();
        }
    }

    ImGui::Spacing();
    if (doc.Raw() && ImGui::CollapsingHeader("Form fields")) {
        auto fields = ops::ListFormFields(doc);
        if (fields.empty()) ImGui::TextDisabled("No interactive form fields.");
        for (auto& f : fields)
            ImGui::BulletText("p%d: %s = %s", f.page + 1, f.name.empty() ? "(unnamed)" : W2U(f.name).c_str(), W2U(f.value).c_str());
        if (!fields.empty() && ImGui::Button("Flatten form")) DoFlatten();
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
    float scale = std::clamp(textPx / ImGui::GetFontSize(), 0.8f, 4.0f);
    ImGui::SetNextWindowPos(ImVec2(pos.x - 3, pos.y - 3));
    ImGui::SetNextWindowBgAlpha(0.98f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(2, 2));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(60, 150, 255, 255));
    ImGui::Begin("##inlinetext", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
    ImGui::SetWindowFontScale(scale);
    float want = ImGui::CalcTextSize(editBuf.c_str()).x + ImGui::GetFontSize() * 2.0f;
    ImGui::SetNextItemWidth(std::max({ minW + 16, want, 120.0f * dpiScale }));
    if (editFocus) { ImGui::SetKeyboardFocusHere(); editFocus = false; }
    bool enter = ImGui::InputText("##it", &editBuf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways,
                                  CaretToEnd, &caretPending);
    bool done = ImGui::IsItemDeactivated();
    bool esc = ImGui::IsKeyPressed(ImGuiKey_Escape);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled("Enter = apply   Esc = cancel");
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (esc) CancelInlineEdit();
    else if (enter || done) CommitInlineEdit();
}

// ---------------------------------------------------------------- modals

void App::DrawModals() {
    // password
    if (showPassword) { ImGui::OpenPopup("Password required##pw"); showPassword = false; }
    if (ImGui::BeginPopupModal("Password required##pw", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("This PDF is encrypted. Enter its password:");
        ImGui::SetNextItemWidth(260 * dpiScale);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool enter = ImGui::InputText("##pw", &pwInput, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
        if (!pwError.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "%s", pwError.c_str());
        if (ImGui::Button("Open", ImVec2(90 * dpiScale, 0)) || enter) {
            std::string err;
            ClearLimbo();
            auto r = doc.LoadFile(pwFile, pwInput, err);
            if (r == Document::Load::Ok) {
                ResetAfterLoad();
                Toasts("Opened " + W2U(doc.displayName));
                ImGui::CloseCurrentPopup();
            } else if (r == Document::Load::NeedPassword) {
                pwError = "Wrong password, try again.";
            } else {
                pwError = err;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90 * dpiScale, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // metadata
    if (showMeta) { ImGui::OpenPopup("Document properties##meta"); showMeta = false; }
    if (ImGui::BeginPopupModal("Document properties##meta", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        auto row = [&](const char* label, const char* key) {
            std::wstring v = ops::GetMeta(doc, key);
            ImGui::Text("%-10s %s", label, v.empty() ? "-" : W2U(v).c_str());
        };
        ImGui::Text("File:      %s", doc.path.empty() ? "(unsaved)" : W2U(doc.path).c_str());
        ImGui::Text("Pages:     %d", doc.PageCount());
        int sigs = doc.Raw() ? FPDF_GetSignatureCount(doc.Raw()) : 0;
        if (sigs > 0) ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "Digital signatures: %d (editing will invalidate them)", sigs);
        ImGui::Separator();
        row("Title:", "Title");
        row("Author:", "Author");
        row("Subject:", "Subject");
        row("Keywords:", "Keywords");
        row("Creator:", "Creator");
        row("Producer:", "Producer");
        ImGui::Separator();
        if (ImGui::Button("Close", ImVec2(100 * dpiScale, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // import pages
    if (showImport) { ImGui::OpenPopup("Import pages##imp"); showImport = false; }
    if (ImGui::BeginPopupModal("Import pages##imp", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("From: %s", W2U(importFile).c_str());
        ImGui::SetNextItemWidth(400 * dpiScale);
        ImGui::InputTextWithHint("##range", "page range, e.g. 1-3,5  (blank = all)", &importRange);
        ImGui::SetNextItemWidth(400 * dpiScale);
        ImGui::InputTextWithHint("##ipw", "password (if the source is encrypted)", &importPw, ImGuiInputTextFlags_Password);
        int mode = importAt < 0 ? 0 : 1;
        ImGui::RadioButton("Append at end", &mode, 0);
        ImGui::SameLine();
        ImGui::RadioButton("After current page", &mode, 1);
        importAt = mode == 0 ? -1 : currentPage + 1;
        if (ImGui::Button("Import", ImVec2(100 * dpiScale, 0))) {
            std::string err;
            if (ops::ImportPagesFromFile(doc, importFile, importPw, importRange, importAt, err)) {
                ClearSelection();
                CommitIfDirty("Import pages");
                Toasts("Pages imported");
                ImGui::CloseCurrentPopup();
            } else Toasts("Import failed: " + err, true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100 * dpiScale, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // extracted text
    if (showExtract) { ImGui::OpenPopup("Extracted text##ex"); showExtract = false; }
    ImGui::SetNextWindowSize(ImVec2(640 * dpiScale, 480 * dpiScale), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Extracted text##ex", nullptr, 0)) {
        if (ImGui::Button("Copy")) ImGui::SetClipboardText(extractedText.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Save as .txt...")) {
            std::wstring out;
            if (SaveFileDialog(hwnd_, { { L"Text files", L"*.txt" } }, L"extracted.txt", L"txt", out)) {
                static const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
                std::vector<uint8_t> bytes(bom, bom + 3);
                bytes.insert(bytes.end(), extractedText.begin(), extractedText.end());
                if (WriteFileBytes(out, bytes.data(), bytes.size())) Toasts("Saved text file");
                else Toasts("Could not write the text file", true);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::InputTextMultiline("##extext", &extractedText, ImVec2(-1, -1), ImGuiInputTextFlags_ReadOnly);
        ImGui::EndPopup();
    }

    // table / form
    if (showTable) { ImGui::OpenPopup("Insert table / form##tbl"); showTable = false; }
    if (ImGui::BeginPopupModal("Insert table / form##tbl", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        float s = dpiScale;
        ImGui::TextWrapped("Builds a grid on the current page. Every line is a separate object you can move, "
                           "resize, delete, or group.");
        ImGui::Separator();
        ImGui::SetNextItemWidth(120 * s); ImGui::InputInt("Columns", &tableCols);
        ImGui::SetNextItemWidth(120 * s); ImGui::InputInt("Rows (body)", &tableRows);
        tableCols = std::clamp(tableCols, 1, 100);
        tableRows = std::clamp(tableRows, 1, 200);
        ImGui::Checkbox("Header row", &tableHeader);
        if (tableHeader) {
            ImGui::SameLine(); ImGui::SetNextItemWidth(100 * s);
            ImGui::DragFloat("Header height", &tableHeaderH, 0.5f, 6, 200, "%.0f pt");
        }
        ImGui::SetNextItemWidth(120 * s); ImGui::DragFloat("Margin", &tableMargin, 0.5f, 0, 300, "%.0f pt");
        ImGui::SetNextItemWidth(120 * s); ImGui::DragFloat("Line width", &tableLineW, 0.05f, 0.2f, 10, "%.2f");
        ImGui::ColorEdit3("Line colour", tableColor, ImGuiColorEditFlags_NoInputs);
        if (ImGui::SmallButton("Preset: 4 columns + header (like the screenshot)")) {
            tableCols = 4; tableRows = 1; tableHeader = true; tableHeaderH = 24;
            tableColor[0] = tableColor[1] = tableColor[2] = 0;
        }
        ImGui::Separator();
        if (ImGui::Button("Create", ImVec2(110 * s, 0))) {
            FS_SIZEF sz = doc.PageSizePt(currentPage);
            PRect box{ tableMargin, tableMargin, sz.width - tableMargin, sz.height - tableMargin };
            int n = ops::AddTable(doc, currentPage, box, tableCols, tableRows, tableHeader, tableHeaderH,
                                  (unsigned)(tableColor[0] * 255), (unsigned)(tableColor[1] * 255),
                                  (unsigned)(tableColor[2] * 255), tableLineW);
            if (n > 0) {
                ClearSelection();
                CommitIfDirty("Insert table");
                Toasts(Fmt("Table added (%d editable lines)", n));
                tool = Tool::Select;
                ImGui::CloseCurrentPopup();
            } else Toasts("Could not build the table (check the margin vs page size)", true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110 * s, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // help
    if (showHelp) { ImGui::OpenPopup("How to##help"); showHelp = false; }
    ImGui::SetNextWindowSize(ImVec2(620 * dpiScale, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("How to##help", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(0.55f, 0.8f, 1, 1), "PDF Editor");
        ImGui::SameLine();
        ImGui::TextDisabled("by TheOCProductions & ShortGG");
        ImGui::Separator();
        auto row = [](const char* k, const char* v) {
            ImGui::TextColored(ImVec4(0.55f, 0.8f, 1, 1), "%s", k);
            ImGui::SameLine(ImGui::GetFontSize() * 13.0f);
            ImGui::TextWrapped("%s", v);
        };
        ImGui::TextDisabled("Selecting");
        row("Click", "select the line, text or picture under the mouse");
        row("Drag on empty space", "select everything the box touches");
        row("Ctrl+A / Ctrl+Shift+A", "select everything / only lines on the page");
        row("Alt+click", "pick one piece out of a group");
        ImGui::Separator();
        ImGui::TextDisabled("Moving & sizing");
        row("Drag", "move; drop it on another page to move it there");
        row("Arrow keys", "nudge 1 pt (Shift = 10 pt, Ctrl = 0.25 pt)");
        row("Alt+arrows / handles", "resize");
        row("Double-click a line", "split a drawing / table cell into separate lines");
        row("Cut piece tool", "lift part of a picture/scan out as a movable piece");
        ImGui::Separator();
        ImGui::TextDisabled("Text");
        row("Double-click / Enter", "type into existing text (font and size stay the same)");
        row("Text tool + click", "edit existing text, or add new text in the PDF's font");
        ImGui::Separator();
        ImGui::TextDisabled("Clipboard");
        row("Ctrl+C / Ctrl+X", "copy / cut (exact fonts and pictures)");
        row("Ctrl+V", "paste on the page under the mouse, same position");
        row("Ctrl+Shift+V", "paste at the mouse; also pastes text copied from other programs");
        ImGui::Separator();
        ImGui::TextDisabled("View");
        row("Ctrl+wheel / Ctrl +/-", "zoom;  Ctrl+0 fit width");
        row("Wheel / Shift+wheel", "scroll up-down / left-right; middle-drag pans");
        row("Right-click", "group, ungroup, split, copy, paste here, move to page");
        if (ImGui::Button("Close", ImVec2(100 * dpiScale, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // about
    if (showAbout) { ImGui::OpenPopup("About##ab"); showAbout = false; }
    if (ImGui::BeginPopupModal("About##ab", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("PDF Editor");
        ImGui::TextColored(ImVec4(0.55f, 0.8f, 1, 1), "by TheOCProductions & ShortGG");
        ImGui::Separator();
        ImGui::TextWrapped("A desktop PDF editor built on PDFium and Dear ImGui (DirectX 11).");
        ImGui::TextDisabled("Rendering: PDFium (BSD-3).  UI: Dear ImGui (MIT).");
        if (ImGui::Button("Close", ImVec2(100 * dpiScale, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
