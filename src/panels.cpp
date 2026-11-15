#include "app.h"
#include "dialogs.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------- menu bar

void App::DrawMenuBar() {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New", "Ctrl+N")) NewDoc();
        if (ImGui::MenuItem("Open...", "Ctrl+O")) OpenDialog();
        ImGui::Separator();
        if (ImGui::MenuItem("Save", "Ctrl+S", false, doc.Raw() != nullptr)) Save();
        if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S", false, doc.Raw() != nullptr)) SaveAs();
        ImGui::Separator();
        if (ImGui::MenuItem("Import pages...", nullptr, false, doc.Raw() != nullptr)) {
            std::wstring f;
            if (OpenFileDialog(hwnd_, { { L"PDF documents", L"*.pdf" } }, f)) {
                importFile = f; importRange.clear(); importPw.clear(); importAt = -1; showImport = true;
            }
        }
        if (ImGui::MenuItem("Export pages as images...", nullptr, false, doc.Raw() != nullptr)) ExportImagesDialog();
        if (ImGui::MenuItem("Extract all text...", nullptr, false, doc.Raw() != nullptr)) ExtractTextDialog();
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) PostQuitMessage(0);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, doc.CanUndo())) Undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, doc.CanRedo())) Redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Delete selected", "Del", false, sel.Valid(doc))) DeleteSelection();
        ImGui::Separator();
        bool has = doc.Raw() != nullptr;
        if (ImGui::MenuItem("Select all on page", "Ctrl+A", false, has)) SelectAllOnPage(false);
        if (ImGui::MenuItem("Select all lines (skip text/images)", "Ctrl+Shift+A", false, has)) SelectAllOnPage(true);
        ImGui::Separator();
        if (ImGui::MenuItem("Group all lines on page", nullptr, false, has)) GroupAllLines();
        if (ImGui::MenuItem("Group selection", nullptr, false, groupSel.size() > 1 && !selGroupId)) GroupSelection();
        if (ImGui::MenuItem("Ungroup", nullptr, false, selGroupId != 0)) UngroupSelection();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Page")) {
        bool has = doc.Raw() != nullptr;
        if (ImGui::MenuItem("Insert blank after current", nullptr, false, has))
            if (ops::InsertBlankPage(doc, currentPage + 1, doc.PageCount() ? doc.PageSizePt(currentPage).width : 612,
                                     doc.PageCount() ? doc.PageSizePt(currentPage).height : 792)) {
                CommitIfDirty("Insert page"); Toasts("Blank page inserted");
            }
        if (ImGui::MenuItem("Duplicate current", nullptr, false, has))
            if (ops::DuplicatePage(doc, currentPage)) { CommitIfDirty("Duplicate page"); Toasts("Page duplicated"); }
        if (ImGui::MenuItem("Delete current", nullptr, false, has && doc.PageCount() > 1))
            if (ops::DeletePages(doc, { currentPage })) { sel.Clear(); CommitIfDirty("Delete page"); Toasts("Page deleted"); }
        ImGui::Separator();
        if (ImGui::MenuItem("Rotate current 90\xC2\xB0 CW", nullptr, false, has))
            if (ops::RotatePage(doc, currentPage, 1)) { CommitIfDirty("Rotate page"); }
        if (ImGui::MenuItem("Rotate current 90\xC2\xB0 CCW", nullptr, false, has))
            if (ops::RotatePage(doc, currentPage, -1)) { CommitIfDirty("Rotate page"); }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Insert")) {
        bool has = doc.Raw() != nullptr;
        if (ImGui::MenuItem("Table / form...", nullptr, false, has)) showTable = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Text (click to place)", nullptr, tool == Tool::Text, has)) tool = Tool::Text;
        if (ImGui::MenuItem("Image (click to place)", nullptr, tool == Tool::Image, has)) tool = Tool::Image;
        if (ImGui::MenuItem("Rectangle (drag)", nullptr, tool == Tool::Rect, has)) tool = Tool::Rect;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Document")) {
        if (ImGui::MenuItem("Flatten forms && annotations", nullptr, false, doc.Raw() != nullptr)) DoFlatten();
        if (ImGui::MenuItem("Properties / metadata", nullptr, false, doc.Raw() != nullptr)) showMeta = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Page thumbnails", nullptr, &showThumbs);
        ImGui::MenuItem("Properties panel", nullptr, &showProps);
        ImGui::Separator();
        if (ImGui::MenuItem("Zoom in", "Ctrl+Wheel")) { zoom = std::min(8.0f, zoom * 1.15f); InvalidateAll(); }
        if (ImGui::MenuItem("Zoom out")) { zoom = std::max(0.1f, zoom / 1.15f); InvalidateAll(); }
        if (ImGui::MenuItem("Fit width")) fitPending = true;
        if (ImGui::MenuItem("Actual size (100%)")) { zoom = 1.0f; InvalidateAll(); }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About")) showAbout = true;
        ImGui::EndMenu();
    }
    // doc title + dirty marker
    if (doc.Raw()) {
        std::string title = W2U(doc.displayName) + (doc.modified ? " *" : "");
        float w = ImGui::CalcTextSize(title.c_str()).x;
        ImGui::SameLine(ImGui::GetWindowWidth() - w - 20);
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
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 6));
    ImGui::BeginChild("##toolbar", ImVec2(0, 42), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPosY(8);
    ImGui::SetCursorPosX(10);
    struct { Tool t; const char* name; const char* tip; } tools[] = {
        { Tool::Select, "Select", "Select, move, resize and edit objects (double-click text to edit)" },
        { Tool::Text, "Text", "Click to add new text" },
        { Tool::Image, "Image", "Click to place an image" },
        { Tool::Rect, "Rectangle", "Drag to draw a filled/outlined box" },
        { Tool::Redact, "Redact", "Drag to permanently remove content under a black box" },
        { Tool::Hand, "Pan", "Drag to pan the page" },
    };
    for (auto& e : tools) {
        if (ToolButton(e.name, tool == e.t)) tool = e.t;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", e.tip);
        ImGui::SameLine();
    }
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::Button("Undo") ) Undo();
    ImGui::SameLine();
    if (ImGui::Button("Redo")) Redo();
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    int zp = (int)std::lround(zoom * 100);
    if (ImGui::SliderInt("##zoom", &zp, 10, 800, "%d%%")) { zoom = zp / 100.0f; InvalidateAll(); }
    ImGui::SameLine();
    if (doc.Raw()) { ImGui::TextDisabled("Page %d / %d", currentPage + 1, doc.PageCount()); }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------- search bar

void App::DrawSearchBar() {
    ImGui::BeginChild("##searchbar", ImVec2(0, 36), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPosY(6);
    ImGui::SetCursorPosX(10);
    ImGui::TextUnformatted("Find:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260);
    if (focusSearch) { ImGui::SetKeyboardFocusHere(); focusSearch = false; }
    bool enter = ImGui::InputTextWithHint("##find", "text to search", &searchQuery,
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    bool go = ImGui::Button("Search");
    ImGui::SameLine();
    ImGui::Checkbox("Aa", &searchCase);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Match case");
    ImGui::SameLine();
    ImGui::Checkbox("W", &searchWhole);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Whole word");
    ImGui::SameLine();

    if ((enter || go) && doc.Raw() && !searchQuery.empty()) {
        searchHits = ops::SearchText(doc, U2W(searchQuery), searchCase, searchWhole);
        searchCurrent = searchHits.empty() ? -1 : 0;
        searchDirty = false;
        if (!searchHits.empty()) ScrollToPage(searchHits[0].page);
        Toasts(Fmt("%d match(es)", (int)searchHits.size()));
    }
    auto step = [&](int d) {
        if (searchHits.empty()) return;
        searchCurrent = (searchCurrent + d + (int)searchHits.size()) % (int)searchHits.size();
        ScrollToPage(searchHits[searchCurrent].page);
    };
    if (ImGui::Button("Prev")) step(-1);
    ImGui::SameLine();
    if (ImGui::Button("Next") || (enter && !searchHits.empty())) step(+1);
    ImGui::SameLine();
    if (!searchHits.empty())
        ImGui::Text("%d / %d", searchCurrent + 1, (int)searchHits.size());
    else if (!searchQuery.empty())
        ImGui::TextDisabled("no matches");
    ImGui::EndChild();
}

// ---------------------------------------------------------------- thumbnails

void App::DrawThumbnails() {
    ImGui::TextUnformatted("Pages");
    ImGui::Separator();
    int n = doc.PageCount();
    for (int i = 0; i < n; ++i) {
        ImGui::PushID(i);
        FS_SIZEF sz = doc.PageSizePt(i);
        float w = 140.0f;
        float h = w * sz.height / std::max(1.0f, sz.width);
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool sel_ = (i == currentPage);
        if (ImGui::Selectable("##th", sel_, 0, ImVec2(ImGui::GetContentRegionAvail().x, h + 22)))
            ScrollToPage(i);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 tl(p.x + (ImGui::GetContentRegionAvail().x - w) / 2, p.y + 2);
        dl->AddRectFilled(tl, ImVec2(tl.x + w, tl.y + h), IM_COL32(255, 255, 255, 255));
        dl->AddRect(tl, ImVec2(tl.x + w, tl.y + h), sel_ ? IM_COL32(60, 150, 255, 255) : IM_COL32(90, 90, 100, 255));
        if (i < (int)cache.size() && cache[i].tex.Valid())
            dl->AddImage(cache[i].tex.Id(), tl, ImVec2(tl.x + w, tl.y + h));
        dl->AddText(ImVec2(tl.x, tl.y + h + 2), IM_COL32(170, 170, 180, 255), Fmt("Page %d", i + 1).c_str());

        if (ImGui::BeginPopupContextItem("ctx")) {
            if (ImGui::MenuItem("Insert blank after"))
                if (ops::InsertBlankPage(doc, i + 1, sz.width, sz.height)) { CommitIfDirty("Insert page"); }
            if (ImGui::MenuItem("Duplicate"))
                if (ops::DuplicatePage(doc, i)) { CommitIfDirty("Duplicate page"); }
            if (ImGui::MenuItem("Rotate 90\xC2\xB0 CW"))
                if (ops::RotatePage(doc, i, 1)) { CommitIfDirty("Rotate page"); }
            if (ImGui::MenuItem("Delete", nullptr, false, n > 1))
                if (ops::DeletePages(doc, { i })) { sel.Clear(); CommitIfDirty("Delete page"); }
            ImGui::Separator();
            if (ImGui::MenuItem("Move up", nullptr, false, i > 0))
                if (ops::MovePage(doc, i, i - 1)) { CommitIfDirty("Move page"); }
            if (ImGui::MenuItem("Move down", nullptr, false, i < n - 1))
                if (ops::MovePage(doc, i, i + 1)) { CommitIfDirty("Move page"); }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------- properties

void App::DrawProperties() {
    if (ImGui::CollapsingHeader("New object style", ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto& fl = FontList();
        ImGui::TextUnformatted("Font");
        ImGui::SetNextItemWidth(-1);
        const char* cur = (fontIndex >= 0 && fontIndex < (int)fl.size()) ? fl[fontIndex].label.c_str() : "(none)";
        if (ImGui::BeginCombo("##font", cur)) {
            for (int i = 0; i < (int)fl.size(); ++i) {
                bool s = (i == fontIndex);
                if (ImGui::Selectable(fl[i].label.c_str(), s)) fontIndex = i;
                if (s) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::DragFloat("Size##new", &fontSize, 0.3f, 4.0f, 400.0f, "%.1f pt");
        ImGui::ColorEdit4("Fill", fillColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
        ImGui::ColorEdit4("Stroke", strokeColor, ImGuiColorEditFlags_NoInputs);
        ImGui::Checkbox("Filled", &rectFilled);
        ImGui::SameLine();
        ImGui::Checkbox("Outlined", &rectStroked);
        ImGui::SetNextItemWidth(120);
        ImGui::DragFloat("Line width", &strokeWidth, 0.1f, 0.2f, 20.0f, "%.1f");
    }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Selection", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (!sel.Valid(doc)) {
            ImGui::TextDisabled("Nothing selected.");
            ImGui::TextWrapped("Click an object, or drag a box over several. Double-click text to edit. "
                               "Right-click to group / ungroup.");
        } else if (groupSel.size() > 1) {
            // multi / group selection
            if (selGroupId) ImGui::Text("Group of %d objects (moves as one)", (int)groupSel.size());
            else ImGui::Text("%d objects selected", (int)groupSel.size());
            PRect b = SelectionBounds();
            float left = b.l, bottom = b.b;
            ImGui::SetNextItemWidth(90); ImGui::DragFloat("X", &left, 0.5f, -10000, 10000, "%.1f");
            bool xe = ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SameLine(); ImGui::SetNextItemWidth(90); ImGui::DragFloat("Y", &bottom, 0.5f, -10000, 10000, "%.1f");
            bool ye = ImGui::IsItemDeactivatedAfterEdit();
            if (xe || ye) { MoveSelection(left - b.l, bottom - b.b); CommitIfDirty("Move group"); }
            ImGui::Text("Size: %.1f x %.1f pt", b.W(), b.H());
            ImGui::TextDisabled("Arrows move - Alt+arrows / handles resize all together");
            ImGui::Separator();
            if (selGroupId) {
                if (ImGui::Button("Ungroup (separate lines)")) UngroupSelection();
            } else {
                if (ImGui::Button("Group into one")) GroupSelection();
            }
            ImGui::SameLine();
            if (ImGui::Button("Delete all")) DeleteSelection();
        } else {
            int t = ops::ObjectType(sel.obj);
            const char* tn = t == FPDF_PAGEOBJ_TEXT ? "Text" : t == FPDF_PAGEOBJ_IMAGE ? "Image" :
                             t == FPDF_PAGEOBJ_PATH ? "Path/Shape" : t == FPDF_PAGEOBJ_FORM ? "Form XObject" : "Object";
            ImGui::Text("Type: %s   (page %d)%s", tn, sel.page + 1, sel.parent ? "  [inside a group]" : "");
            PRect b = sel.PageBounds();
            float left = b.l, bottom = b.b, w = b.W(), h = b.H();
            ImGui::SetNextItemWidth(90); ImGui::DragFloat("X", &left, 0.5f, -10000, 10000, "%.1f");
            bool xEdited = ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SameLine(); ImGui::SetNextItemWidth(90); ImGui::DragFloat("Y", &bottom, 0.5f, -10000, 10000, "%.1f");
            bool yEdited = ImGui::IsItemDeactivatedAfterEdit();
            if ((xEdited || yEdited)) {
                EnsureSelectionEditable();
                ops::SetObjectPosPage(doc, sel.page, sel.obj, sel.accum, left, bottom);
                InvalidatePage(sel.page); CommitIfDirty("Move object");
            }
            ImGui::Text("Size: %.1f x %.1f pt", w, h);
            ImGui::TextDisabled("Arrows: move  -  Shift: x10  -  Alt+arrows: resize  -  drag handles to resize");

            if (t == FPDF_PAGEOBJ_TEXT) {
                ops::TextProps tp = ops::ReadText(doc, sel.page, sel.obj);
                ImGui::Separator();
                ImGui::TextWrapped("Font: %s", tp.fontName.empty() ? "(embedded)" : tp.fontName.c_str());
                ImGui::Text("Font size: %.1f pt", tp.fontSize);
                if (ImGui::Button("Edit text...")) {
                    EnsureSelectionEditable();
                    editing = true; editIsNew = false; editPage = sel.page; editObj = sel.obj;
                    editBuf = W2U(tp.text);
                }
                float col[4] = { tp.r / 255.0f, tp.g / 255.0f, tp.b / 255.0f, (tp.a ? tp.a : 255) / 255.0f };
                if (ImGui::ColorEdit4("Text colour", col, ImGuiColorEditFlags_NoInputs)) {
                    EnsureSelectionEditable();
                    ops::SetObjectFillColor(doc, sel.page, sel.obj, (unsigned)(col[0] * 255), (unsigned)(col[1] * 255),
                                            (unsigned)(col[2] * 255), (unsigned)(col[3] * 255));
                    InvalidatePage(sel.page);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) CommitIfDirty("Change colour");
                ImGui::TextDisabled("Restyle to selected font/size:");
                if (ImGui::Button("Apply font && size")) {
                    EnsureSelectionEditable();
                    std::string err;
                    if (ops::RestyleText(doc, sel.page, sel.obj, CurrentFont(), fontSize, err)) {
                        sel.gen = doc.Generation(); sel.obj = nullptr; sel.Clear();
                        InvalidatePage(editPage); CommitIfDirty("Restyle text");
                        Toasts("Font applied");
                    } else Toasts("Restyle failed: " + err, true);
                }
            }

            if (t == FPDF_PAGEOBJ_PATH) {
                ops::StrokeProps st = ops::ReadStroke(sel.obj);
                ImGui::Separator();
                ImGui::TextDisabled("Line / stroke");
                float lw = st.width;
                ImGui::SetNextItemWidth(-1);
                if (ImGui::DragFloat("Thickness", &lw, 0.05f, 0.1f, 40.0f, "%.2f pt")) {
                    EnsureSelectionEditable();
                    ops::SetObjectStroke(doc, sel.page, sel.obj, st.r, st.g, st.b, lw);
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
            }
            ImGui::Separator();
            if (ImGui::Button("Delete")) {
                int pg = sel.page;
                EnsureSelectionEditable();
                if (ops::DeleteObjectNested(doc, pg, sel.obj, sel.parent)) { sel.Clear(); CommitIfDirty("Delete object"); }
            }
        }
    }

    ImGui::Spacing();
    if (doc.Raw() && ImGui::CollapsingHeader("Form fields")) {
        auto fields = ops::ListFormFields(doc);
        if (fields.empty()) ImGui::TextDisabled("No interactive form fields.");
        for (auto& f : fields) {
            ImGui::BulletText("p%d: %s = %s", f.page + 1,
                              f.name.empty() ? "(unnamed)" : W2U(f.name).c_str(),
                              W2U(f.value).c_str());
        }
        if (!fields.empty() && ImGui::Button("Flatten form")) DoFlatten();
    }
}

// ---------------------------------------------------------------- modals

static void DrawTextEditor(App& a) {
    if (!a.editing) return;
    ImGui::OpenPopup("Edit text##te");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = true;
    if (ImGui::BeginPopupModal("Edit text##te", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled(a.editIsNew ? "New text" : "Editing existing text");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputTextMultiline("##txt", &a.editBuf, ImVec2(400, 90));
        bool ok = ImGui::Button("OK", ImVec2(90, 0));
        ImGui::SameLine();
        bool cancel = ImGui::Button("Cancel", ImVec2(90, 0));
        if (ok) {
            std::wstring w = U2W(a.editBuf);
            if (a.editIsNew) {
                if (!w.empty()) {
                    std::string err;
                    FPDF_PAGEOBJECT o = ops::AddText(a.doc, a.editPage, w, a.CurrentFont(), a.fontSize,
                                                     a.editAtX, a.editAtY,
                                                     (unsigned)(a.fillColor[0] * 255), (unsigned)(a.fillColor[1] * 255),
                                                     (unsigned)(a.fillColor[2] * 255), err);
                    if (o) {
                        a.SelectNewObject(a.editPage, o);
                        a.InvalidatePage(a.editPage); a.CommitIfDirty("Add text");
                        a.Toasts("Text added");
                    } else a.Toasts("Add text failed: " + err, true);
                }
            } else if (a.editObj) {
                if (ops::SetObjectText(a.doc, a.editPage, a.editObj, w)) {
                    a.InvalidatePage(a.editPage); a.CommitIfDirty("Edit text");
                    a.Toasts("Text updated");
                }
            }
            a.editing = false; a.editObj = nullptr;
            ImGui::CloseCurrentPopup();
        }
        if (cancel || !open) { a.editing = false; a.editObj = nullptr; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

void App::DrawModals() {
    DrawTextEditor(*this);

    // password
    if (showPassword) { ImGui::OpenPopup("Password required##pw"); showPassword = false; }
    if (ImGui::BeginPopupModal("Password required##pw", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("This PDF is encrypted. Enter its password:");
        ImGui::SetNextItemWidth(260);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool enter = ImGui::InputText("##pw", &pwInput, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
        if (!pwError.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "%s", pwError.c_str());
        if (ImGui::Button("Open", ImVec2(90, 0)) || enter) {
            std::string err;
            auto r = doc.LoadFile(pwFile, pwInput, err);
            if (r == Document::Load::Ok) {
                sel.Clear(); editing = false; zoom = 1.0f; fitPending = true; cache.clear();
                searchHits.clear(); searchCurrent = -1;
                Toasts("Opened " + W2U(doc.displayName));
                ImGui::CloseCurrentPopup();
            } else if (r == Document::Load::NeedPassword) {
                pwError = "Wrong password, try again.";
            } else {
                pwError = err;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // metadata
    if (showMeta) { ImGui::OpenPopup("Document properties##meta"); showMeta = false; }
    ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Document properties##meta", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        auto row = [&](const char* label, const char* key) {
            std::wstring v = ops::GetMeta(doc, key);
            ImGui::Text("%-10s %s", label, v.empty() ? "-" : W2U(v).c_str());
        };
        ImGui::Text("File:      %s", doc.path.empty() ? "(unsaved)" : W2U(doc.path).c_str());
        ImGui::Text("Pages:     %d", doc.PageCount());
        ImGui::Separator();
        row("Title:", "Title");
        row("Author:", "Author");
        row("Subject:", "Subject");
        row("Keywords:", "Keywords");
        row("Creator:", "Creator");
        row("Producer:", "Producer");
        ImGui::Separator();
        if (ImGui::Button("Close", ImVec2(100, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // import pages
    if (showImport) { ImGui::OpenPopup("Import pages##imp"); showImport = false; }
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Import pages##imp", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("From: %s", W2U(importFile).c_str());
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##range", "page range, e.g. 1-3,5  (blank = all)", &importRange);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##ipw", "password (if the source is encrypted)", &importPw, ImGuiInputTextFlags_Password);
        int mode = importAt < 0 ? 0 : 1;
        ImGui::RadioButton("Append at end", &mode, 0);
        ImGui::SameLine();
        ImGui::RadioButton("After current page", &mode, 1);
        importAt = mode == 0 ? -1 : currentPage + 1;
        if (ImGui::Button("Import", ImVec2(100, 0))) {
            std::string err;
            if (ops::ImportPagesFromFile(doc, importFile, importPw, importRange, importAt, err)) {
                CommitIfDirty("Import pages");
                Toasts("Pages imported");
                ImGui::CloseCurrentPopup();
            } else Toasts("Import failed: " + err, true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // extracted text
    if (showExtract) { ImGui::OpenPopup("Extracted text##ex"); showExtract = false; }
    ImGui::SetNextWindowSize(ImVec2(640, 480), ImGuiCond_Appearing);
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
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Insert table / form##tbl", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Builds a grid on the current page. Every line is a separate "
                           "object you can then move, resize or delete individually.");
        ImGui::Separator();
        ImGui::SetNextItemWidth(120); ImGui::InputInt("Columns", &tableCols);
        ImGui::SetNextItemWidth(120); ImGui::InputInt("Rows (body)", &tableRows);
        tableCols = std::max(1, std::min(tableCols, 100));
        tableRows = std::max(1, std::min(tableRows, 200));
        ImGui::Checkbox("Header row", &tableHeader);
        if (tableHeader) {
            ImGui::SameLine(); ImGui::SetNextItemWidth(100);
            ImGui::DragFloat("Header height", &tableHeaderH, 0.5f, 6, 200, "%.0f pt");
        }
        ImGui::SetNextItemWidth(120); ImGui::DragFloat("Margin", &tableMargin, 0.5f, 0, 300, "%.0f pt");
        ImGui::SetNextItemWidth(120); ImGui::DragFloat("Line width", &tableLineW, 0.05f, 0.2f, 10, "%.2f");
        ImGui::ColorEdit3("Line colour", tableColor, ImGuiColorEditFlags_NoInputs);
        if (ImGui::SmallButton("Match screenshot (4 cols, header)")) {
            tableCols = 4; tableRows = 1; tableHeader = true; tableHeaderH = 24; tableColor[0] = tableColor[1] = tableColor[2] = 0;
        }
        ImGui::Separator();
        if (ImGui::Button("Create", ImVec2(110, 0))) {
            FS_SIZEF sz = doc.PageSizePt(currentPage);
            PRect box{ tableMargin, tableMargin, sz.width - tableMargin, sz.height - tableMargin };
            int n = ops::AddTable(doc, currentPage, box, tableCols, tableRows, tableHeader, tableHeaderH,
                                  (unsigned)(tableColor[0] * 255), (unsigned)(tableColor[1] * 255), (unsigned)(tableColor[2] * 255),
                                  tableLineW);
            if (n > 0) {
                sel.Clear();
                CommitIfDirty("Insert table");
                Toasts(Fmt("Table added (%d editable lines) - switch to Select to move them", n));
                tool = Tool::Select;
                ImGui::CloseCurrentPopup();
            } else Toasts("Could not build the table (check the margin vs page size)", true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // about
    if (showAbout) { ImGui::OpenPopup("About##ab"); showAbout = false; }
    if (ImGui::BeginPopupModal("About##ab", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("PDF Editor");
        ImGui::Separator();
        ImGui::TextWrapped("A desktop PDF editor built on PDFium and Dear ImGui (DirectX 11).");
        ImGui::TextWrapped("View, edit text and images, draw shapes, redact, fill forms, reorder "
                           "and rotate pages, search, and export.");
        ImGui::Spacing();
        ImGui::TextDisabled("Rendering: PDFium (BSD-3).  UI: Dear ImGui (MIT).");
        if (ImGui::Button("Close", ImVec2(100, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
