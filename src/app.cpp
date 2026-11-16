#include "app.h"
#include "dialogs.h"
#include "imgui.h"
#include <windows.h>
#include <shellapi.h>

static const std::vector<FileFilter> kPdfFilter = { { L"PDF documents", L"*.pdf" }, { L"All files", L"*.*" } };

void App::Init(HWND hwnd) {
    hwnd_ = hwnd;
    DragAcceptFiles(hwnd, TRUE);
}

void App::Shutdown() {
    cache.clear();
}

const std::vector<FontChoice>& App::FontList() {
    if (!fontListBuilt_) {
        fontList_ = StandardFonts();
        for (const auto& f : SystemFonts()) fontList_.push_back(ChoiceFor(f));
        fontListBuilt_ = true;
    }
    return fontList_;
}

FontChoice App::CurrentFont() {
    const auto& fl = FontList();
    if (fontIndex < 0 || fontIndex >= (int)fl.size()) return fl.empty() ? FontChoice{} : fl[0];
    return fl[fontIndex];
}

void App::Toasts(const std::string& msg, bool error) {
    toast.text = msg;
    toast.error = error;
    toast.until = ImGui::GetTime() + (error ? 6.0 : 3.5);
}

void App::InvalidatePage(int i) {
    if (i >= 0 && i < (int)cache.size()) cache[i].version = ~0u;
}
void App::InvalidateAll() {
    for (auto& c : cache) c.version = ~0u;
}
void App::ScrollToPage(int i) { pendingScrollPage = i; }

void App::CommitIfDirty(const std::string& label) {
    doc.Commit(label);
    InvalidateAll();
    searchDirty = true;
}

// --------------------------------------------------------------- file commands

void App::NewDoc() {
    if (doc.CreateBlank(612, 792)) {
        sel.Clear(); groupSel.clear(); selGroupId = 0;
        editing = false;
        zoom = 1.0f;
        fitPending = true;
        cache.clear();
        searchHits.clear();
        Toasts("New blank document");
    } else {
        Toasts("Could not create a new document", true);
    }
}

void App::OpenDialog() {
    std::wstring path;
    if (OpenFileDialog(hwnd_, kPdfFilter, path)) OpenPath(path);
}

void App::OpenPath(const std::wstring& path) {
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
    sel.Clear(); groupSel.clear(); selGroupId = 0;
    editing = false;
    zoom = 1.0f;
    fitPending = true;
    cache.clear();
    searchHits.clear();
    searchCurrent = -1;
    Toasts("Opened " + W2U(doc.displayName));
}

bool App::Save() {
    if (!doc.Raw()) return false;
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
    if (doc.Undo()) {
        sel.Clear(); groupSel.clear(); selGroupId = 0;
        editing = false;
        InvalidateAll();
        searchDirty = true;
        Toasts("Undo");
    }
}

void App::Redo() {
    if (doc.Redo()) {
        sel.Clear(); groupSel.clear(); selGroupId = 0;
        editing = false;
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
    if (ops::FlattenAll(doc)) {
        CommitIfDirty("Flatten");
        sel.Clear(); groupSel.clear(); selGroupId = 0;
        Toasts("Flattened annotations and form fields");
    } else {
        Toasts("Nothing to flatten", true);
    }
}

void App::ExportImagesDialog() {
    if (!doc.Raw()) return;
    std::wstring dir;
    if (!PickFolderDialog(hwnd_, dir, L"Choose a folder for the page images")) return;
    std::vector<int> pages;
    for (int i = 0; i < doc.PageCount(); ++i) pages.push_back(i);
    std::string err;
    if (ops::ExportPagesToImages(doc, pages, dir, exportDpi, err))
        Toasts(Fmt("Exported %d page image(s)", (int)pages.size()));
    else
        Toasts("Export failed: " + err, true);
}

void App::ExtractTextDialog() {
    if (!doc.Raw()) return;
    extractedText = W2U(ops::ExtractAllText(doc));
    showExtract = true;
}

// --------------------------------------------------------------- frame

void App::Draw() {
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

    // body: [thumbnails | canvas | properties]
    float propsW = showProps ? 320.0f : 0.0f;
    float thumbsW = showThumbs ? 180.0f : 0.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();

    if (showThumbs) {
        ImGui::BeginChild("##thumbs", ImVec2(thumbsW, avail.y), ImGuiChildFlags_Borders);
        DrawThumbnails();
        ImGui::EndChild();
        ImGui::SameLine(0, 0);
    }

    float canvasW = avail.x - thumbsW - propsW;
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

    // toast
    if (!toast.text.empty() && ImGui::GetTime() < toast.until) {
        ImVec2 p = vp->WorkPos;
        ImGui::SetNextWindowPos(ImVec2(p.x + 16, p.y + vp->WorkSize.y - 48));
        ImGui::SetNextWindowBgAlpha(0.92f);
        ImGui::Begin("##toast", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::TextColored(toast.error ? ImVec4(1, 0.5f, 0.5f, 1) : ImVec4(0.6f, 0.9f, 0.7f, 1), "%s",
                           toast.text.c_str());
        ImGui::End();
    }

    ImGui::End();

    // global shortcuts
    ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && !editing) {
        if (ImGui::IsKeyPressed(ImGuiKey_S)) { if (io.KeyShift) SaveAs(); else Save(); }
        if (ImGui::IsKeyPressed(ImGuiKey_O)) OpenDialog();
        if (ImGui::IsKeyPressed(ImGuiKey_N)) NewDoc();
        if (ImGui::IsKeyPressed(ImGuiKey_Z)) Undo();
        if (ImGui::IsKeyPressed(ImGuiKey_Y)) Redo();
        if (ImGui::IsKeyPressed(ImGuiKey_F)) focusSearch = true;
        if (!io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_C)) CopySelection();
            if (ImGui::IsKeyPressed(ImGuiKey_X)) CutSelection();
            if (ImGui::IsKeyPressed(ImGuiKey_V)) PasteClipboard();
        }
    }
}
