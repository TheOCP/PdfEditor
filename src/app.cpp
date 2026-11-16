#include "app.h"
#include "dialogs.h"
#include "imgui.h"
#include <windows.h>
#include <shellapi.h>
#include <fpdf_signature.h>

static const std::vector<FileFilter> kPdfFilter = { { L"PDF documents", L"*.pdf" }, { L"All files", L"*.*" } };

void App::Init(HWND hwnd) {
    hwnd_ = hwnd;
    DragAcceptFiles(hwnd, TRUE);
    zoom = 96.0f / 72.0f;
}

void App::Shutdown() {
    ClearLimbo();           // cut objects must be freed while their document still exists
    cache.clear();
    thumbs.clear();
    doc.Close();            // before PdfShutdown(); the static App would otherwise close it too late
}

const std::vector<FontChoice>& App::FontList() {
    uint32_t id = doc.Raw() ? doc.DocId() : 0;
    if (!fontListBuilt_ || id != fontListDoc_) {
        std::string prevKey = (fontIndex >= 0 && fontIndex < (int)fontList_.size()) ? fontList_[fontIndex].Key() : "";
        fontList_.clear();
        docFontCount = 0;
        if (doc.Raw()) {
            // Fonts stored in the PDF first (one entry per family; most used copy).
            for (const auto& df : ops::ListDocFonts(doc)) {
                std::string nm = StripSubsetPrefix(df.name);
                bool dup = false;
                for (auto& e : fontList_) if (StripSubsetPrefix(e.docName) == nm) { dup = true; break; }
                if (dup || nm.empty()) continue;
                FontChoice fc;
                fc.docName = df.name;
                fc.label = nm + (df.embedded ? "  (in this PDF)" : "  (used in this PDF)");
                fontList_.push_back(fc);
            }
            docFontCount = (int)fontList_.size();
        }
        for (auto& f : StandardFonts()) fontList_.push_back(f);
        for (const auto& f : SystemFonts()) fontList_.push_back(ChoiceFor(f));
        fontListBuilt_ = true;
        fontListDoc_ = id;
        int keep = -1;
        for (int i = 0; i < (int)fontList_.size() && !prevKey.empty(); ++i)
            if (fontList_[i].Key() == prevKey) { keep = i; break; }
        if (fontDefaultPending && docFontCount > 0) fontIndex = 0;   // new PDF: its most used font
        else fontIndex = keep >= 0 ? keep : 0;
        if (doc.Raw()) fontDefaultPending = false;
    }
    return fontList_;
}

FontChoice App::CurrentFont() {
    const auto& fl = FontList();
    FontChoice fc = (fontIndex >= 0 && fontIndex < (int)fl.size()) ? fl[fontIndex] : (fl.empty() ? FontChoice{} : fl[0]);
    if (fc.IsDoc()) {
        fc.handle = ops::FindDocFont(doc, fc.docName, currentPage);
        if (!fc.handle)                          // no longer used in the PDF: closest installed font
            if (const SysFont* sf = MatchSystemFont(fc.docName)) return ChoiceFor(*sf);
    }
    return fc;
}

void App::Toasts(const std::string& msg, bool error) {
    toast.text = msg;
    toast.error = error;
    toast.until = ImGui::GetTime() + (error ? 6.0 : 4.0);
}

void App::InvalidatePage(int i) {
    if (i >= 0 && i < (int)cache.size()) cache[i].version = ~0u;
}
void App::InvalidateAll() {
    for (auto& c : cache) c.version = ~0u;
}
void App::ScrollToPage(int i) { pendingScrollPage = i; }
void App::ScrollToRect(int page, PRect r) { pendingScrollRectPage = page; pendingScrollRect = r; }

void App::CommitIfDirty(const std::string& label) {
    doc.Commit(label);
    InvalidateAll();
    searchDirty = true;
}

// --------------------------------------------------------------- file commands

bool App::ConfirmDiscard() {
    if (editing) CommitInlineEdit();
    EndFormFocus();
    if (!doc.Raw() || !doc.modified) return true;
    std::wstring msg = L"Save changes to \"" + (doc.displayName.empty() ? std::wstring(L"Untitled.pdf") : doc.displayName) +
                       L"\" first?";
    int r = MessageBoxW(hwnd_, msg.c_str(), L"PDF Editor", MB_YESNOCANCEL | MB_ICONWARNING);
    if (r == IDCANCEL) return false;
    if (r == IDYES) return Save();
    return true;
}

void App::ResetAfterLoad() {
    ClearSelection();
    editing = false;
    editObj = nullptr;
    formPage = -1;
    zoom = 96.0f / 72.0f;
    fitPending = true;
    scrollX = scrollY = 0;
    cache.clear();
    thumbs.clear();
    searchHits.clear();
    searchCurrent = -1;
    pendingFontValid = false;
    pasteCount = 0;
    fontDefaultPending = true;               // default to the new PDF's own font
}

void App::NewDoc() {
    if (!ConfirmDiscard()) return;
    ClearLimbo();
    if (doc.CreateBlank(612, 792)) {
        ResetAfterLoad();
        Toasts("New blank document");
    } else {
        Toasts("Could not create a new document", true);
    }
}

void App::OpenDialog() {
    std::wstring path;
    if (OpenFileDialog(hwnd_, kPdfFilter, path)) OpenPath(path);   // OpenPath asks about unsaved changes
}

void App::OpenPath(const std::wstring& path) {
    if (!ConfirmDiscard()) return;
    ClearLimbo();
    std::string err;
    auto r = doc.LoadFile(path, "", err);
    if (r == Document::Load::NeedPassword) {
        pwFile = path;
        pwInput.clear();
        pwError.clear();
        showPassword = true;
        return;
    }
    if (r != Document::Load::Ok) {
        ErrorBox(hwnd_, L"Could not open this PDF:\n" + U2W(err));
        return;
    }
    ResetAfterLoad();
    if (FPDF_GetSignatureCount(doc.Raw()) > 0)
        Toasts("Opened " + W2U(doc.displayName) + " - note: it is digitally signed; any edit invalidates the signature");
    else
        Toasts("Opened " + W2U(doc.displayName));
}

bool App::Save() {
    if (!doc.Raw()) return false;
    if (editing) CommitInlineEdit();
    if (doc.path.empty()) return SaveAs();
    std::string err;
    doc.KillFormFocus();
    if (!doc.SaveFile(doc.path, false, false, err)) {
        ErrorBox(hwnd_, L"Save failed:\n" + U2W(err));
        return false;
    }
    Toasts("Saved " + W2U(doc.displayName));
    return true;
}

bool App::SaveAs() {
    if (!doc.Raw()) return false;
    if (editing) CommitInlineEdit();
    std::wstring name = doc.displayName.empty() ? L"document.pdf" : doc.displayName;
    std::wstring out;
    if (!SaveFileDialog(hwnd_, kPdfFilter, name.c_str(), L"pdf", out)) return false;
    std::string err;
    doc.KillFormFocus();
    if (!doc.SaveFile(out, false, false, err)) {
        ErrorBox(hwnd_, L"Save failed:\n" + U2W(err));
        return false;
    }
    Toasts("Saved " + W2U(doc.displayName));
    return true;
}

void App::Undo() {
    if (editing) { CancelInlineEdit(); return; }
    EndFormFocus();
    ClearLimbo();
    if (doc.Undo()) {
        ClearSelection();
        InvalidateAll();
        searchDirty = true;
        Toasts("Undo");
    }
}

void App::Redo() {
    if (editing) return;
    EndFormFocus();
    ClearLimbo();
    if (doc.Redo()) {
        ClearSelection();
        InvalidateAll();
        searchDirty = true;
        Toasts("Redo");
    }
}

void App::DoFlatten() {
    if (!doc.Raw()) return;
    if (AskYesNoCancel(hwnd_,
            L"Flatten merges all form fields and annotations into the page content.\n"
            L"Afterwards they can no longer be edited as fields. Continue?",
            L"Flatten") != IDYES)
        return;
    EndFormFocus();
    if (ops::FlattenAll(doc)) {
        ClearSelection();
        CommitIfDirty("Flatten");
        Toasts("Flattened annotations and form fields");
    } else {
        Toasts("Nothing to flatten", true);
    }
}

void App::FlattenCurrentPage() {
    if (!doc.Raw()) return;
    int pg = AnnotSelected() ? annSelPage : currentPage;
    EndFormFocus();
    if (ops::FlattenPage(doc, pg)) {
        ClearSelection();
        CommitIfDirty("Annotations to content");
        Toasts(Fmt("Page %d: annotations are now normal content - click them to edit, split or move", pg + 1));
    } else {
        Toasts("This page has no annotations to convert", true);
    }
}

void App::ExportImagesDialog(float dpi) {
    if (!doc.Raw()) return;
    std::wstring dir;
    if (!PickFolderDialog(hwnd_, dir, L"Choose a folder for the page images")) return;
    std::vector<int> pages;
    for (int i = 0; i < doc.PageCount(); ++i) pages.push_back(i);
    std::string err;
    if (ops::ExportPagesToImages(doc, pages, dir, dpi, err))
        Toasts(Fmt("Exported %d page image(s) at %.0f DPI", (int)pages.size(), dpi));
    else
        Toasts("Export failed: " + err, true);
}

void App::ExtractTextDialog() {
    if (!doc.Raw()) return;
    extractedText = W2U(ops::ExtractAllText(doc));
    showExtract = true;
}

void App::UpdateTitle() {
    std::wstring t = L"PDF Editor";
    if (doc.Raw()) t = (doc.displayName.empty() ? L"Untitled.pdf" : doc.displayName) + (doc.modified ? L" *" : L"") + L" - PDF Editor";
    if (t != lastTitle) {
        SetWindowTextW(hwnd_, t.c_str());
        lastTitle = t;
    }
}

// --------------------------------------------------------------- frame

void App::Draw() {
    UpdateTitle();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_MenuBar;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##root", nullptr, flags);
    ImGui::PopStyleVar(2);

    DrawMenuBar();
    DrawToolbar();
    DrawSearchBar();

    float propsW = showProps ? 330.0f * dpiScale : 0.0f;
    float thumbsW = showThumbs ? 180.0f * dpiScale : 0.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (showThumbs) {
        ImGui::BeginChild("##thumbs", ImVec2(thumbsW, avail.y), ImGuiChildFlags_Borders);
        DrawThumbnails();
        ImGui::EndChild();
        ImGui::SameLine(0, 0);
    }
    float canvasW = std::max(50.0f, avail.x - thumbsW - propsW);
    ImGui::BeginChild("##canvas", ImVec2(canvasW, avail.y), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawCanvas();
    ImGui::EndChild();
    if (showProps) {
        ImGui::SameLine(0, 0);
        ImGui::BeginChild("##props", ImVec2(propsW, avail.y), ImGuiChildFlags_Borders);
        DrawProperties();
        ImGui::EndChild();
    }
    DrawModals();
    ImGui::End();

    DrawInlineEditor();

    if (!toast.text.empty() && ImGui::GetTime() < toast.until) {
        ImVec2 p = vp->WorkPos;
        ImGui::SetNextWindowPos(ImVec2(p.x + 16 * dpiScale, p.y + vp->WorkSize.y - 16 * dpiScale), ImGuiCond_Always, ImVec2(0, 1));
        ImGui::SetNextWindowBgAlpha(0.94f);
        ImGui::Begin("##toast", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::TextColored(toast.error ? ImVec4(1, 0.5f, 0.5f, 1) : ImVec4(0.6f, 0.9f, 0.7f, 1), "%s", toast.text.c_str());
        ImGui::End();
    }

    // global shortcuts (never while typing in a text box)
    ImGuiIO& io = ImGui::GetIO();
    bool typing = io.WantTextInput || editing;
    if (ImGui::IsKeyPressed(ImGuiKey_F1)) showHelp = true;
    if (io.KeyCtrl && !typing) {
        if (ImGui::IsKeyPressed(ImGuiKey_S)) { if (io.KeyShift) SaveAs(); else Save(); }
        if (ImGui::IsKeyPressed(ImGuiKey_O)) OpenDialog();
        if (ImGui::IsKeyPressed(ImGuiKey_N)) NewDoc();
        if (ImGui::IsKeyPressed(ImGuiKey_Z)) Undo();
        if (ImGui::IsKeyPressed(ImGuiKey_Y)) Redo();
        if (ImGui::IsKeyPressed(ImGuiKey_F)) focusSearch = true;
        if (ImGui::IsKeyPressed(ImGuiKey_C)) CopySelection();
        if (ImGui::IsKeyPressed(ImGuiKey_X)) CutSelection();
        if (ImGui::IsKeyPressed(ImGuiKey_V)) PasteClipboard(io.KeyShift);
        if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) { zoom = std::min(8.0f, zoom * 1.15f); InvalidateAll(); }
        if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) { zoom = std::max(0.1f, zoom / 1.15f); InvalidateAll(); }
        if (ImGui::IsKeyPressed(ImGuiKey_0) || ImGui::IsKeyPressed(ImGuiKey_Keypad0)) fitPending = true;
        if (ImGui::IsKeyPressed(ImGuiKey_1) || ImGui::IsKeyPressed(ImGuiKey_Keypad1)) { zoom = 96.0f / 72.0f; InvalidateAll(); }
    }
}
