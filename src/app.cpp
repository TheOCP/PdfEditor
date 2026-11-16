#include "app.h"
#include "dialogs.h"
#include "system.h"
#include "i18n.h"
#include "icons.h"
#include "theme.h"
#include "imgui.h"
#include <windows.h>
#include <shellapi.h>
#include <fpdf_signature.h>

static std::vector<FileFilter> PdfFilter() {
    return { { TW("PDF documents"), L"*.pdf" }, { TW("All files"), L"*.*" } };
}

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

void App::ChangeLanguage(Lang l, bool save) {
    SetLanguage(l);
    if (!save) { fontListBuilt_ = false; lastTitle.clear(); return; }
    SaveSettings();
    fontListBuilt_ = false;          // font labels are translated
    lastTitle.clear();
}

void App::ChangeUiScale(float factor) {
    SetUiScaleSetting(factor);
    SaveSettings();
    uiScaleChanged = true;           // the window host rebuilds the fonts between frames
}

const std::vector<FontChoice>& App::FontList() {
    uint32_t id = doc.Raw() ? doc.DocId() : 0;
    if (!fontListBuilt_ || id != fontListDoc_ || fontListLang_ != LanguageGeneration()) {
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
                fc.label = nm + (df.embedded ? T("  (in this PDF)") : T("  (used in this PDF)"));
                fontList_.push_back(fc);
            }
            docFontCount = (int)fontList_.size();
        }
        for (auto f : StandardFonts()) {
            f.label = f.stdName + T("  (standard, not embedded)");
            fontList_.push_back(f);
        }
        for (const auto& f : SystemFonts()) fontList_.push_back(ChoiceFor(f));
        fontListBuilt_ = true;
        fontListDoc_ = id;
        fontListLang_ = LanguageGeneration();
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

FPDF_PAGEOBJECT App::AddTextInFont(int page, const std::wstring& text, double size, float x, float y,
                                   unsigned r, unsigned g, unsigned b, std::string& err, ops::TextEditInfo* info) {
    FontChoice fc = CurrentFont();
    if (fc.IsDoc() && fc.handle) {
        FPDF_PAGEOBJECT sample = nullptr;
        ops::FindDocFont(doc, fc.docName, page, &sample);
        if (sample)
            if (FPDF_PAGEOBJECT o = ops::AddTextLike(doc, page, text, sample, {}, fc.docName, size, x, y, r, g, b, err, info))
                return o;
    }
    FPDF_PAGEOBJECT o = ops::AddText(doc, page, text, fc, size, x, y, r, g, b, err);
    if (info) {
        *info = ops::TextEditInfo{};
        if (o) info->pieces = { o };
    }
    return o;
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
void App::ScrollToRect(int page, PRect r) { pendingScrollRectPage = page; pendingScrollRect = r; }

void App::CommitIfDirty(const std::string& label) {
    doc.Commit(label);
    InvalidateAll();
    searchDirty = true;
}

// --------------------------------------------------------------- file commands

// The document's name as shown: a new document is "Untitled.pdf" in the
// engine and gets the name in the UI language here.
std::wstring App::DocName() const {
    if (doc.displayName.empty() || doc.displayName == L"Untitled.pdf") return TW("Untitled.pdf");
    return doc.displayName;
}

bool App::ConfirmDiscard() {
    if (editing) CommitInlineEdit();
    EndFormFocus();
    if (!doc.Raw() || !doc.modified) return true;
    std::string name = W2U(DocName());
    std::wstring msg = U2W(Fmt(T("Save changes to \"%s\" first?"), name.c_str()));
    int r = MessageBoxW(hwnd_, msg.c_str(), TW("PDF Editor").c_str(), MB_YESNOCANCEL | MB_ICONWARNING);
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
    searchedFor.clear();
    pendingFontValid = false;
    pasteCount = 0;
    fontDefaultPending = true;               // default to the new PDF's own font
}

void App::NewDoc() {
    if (!ConfirmDiscard()) return;
    ClearLimbo();
    if (doc.CreateBlank(612, 792)) {
        ResetAfterLoad();
        Toasts(T("New blank document"));
    } else {
        Toasts(T("Could not create a new document"), true);
    }
}

void App::OpenDialog() {
    std::wstring path;
    if (OpenFileDialog(hwnd_, PdfFilter(), path)) OpenPath(path);   // OpenPath asks about unsaved changes
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
        ErrorBox(hwnd_, TW("Could not open this PDF:") + L"\n" + U2W(T(err)));
        return;
    }
    ResetAfterLoad();
    std::string name = W2U(doc.displayName);
    if (FPDF_GetSignatureCount(doc.Raw()) > 0)
        Toasts(Fmt(T("Opened %s - note: it is digitally signed; any edit invalidates the signature"), name.c_str()));
    else
        Toasts(Fmt(T("Opened %s"), name.c_str()));
}

bool App::Save() {
    if (!doc.Raw()) return false;
    if (editing) CommitInlineEdit();
    if (doc.path.empty()) return SaveAs();
    std::string err;
    doc.KillFormFocus();
    if (!doc.SaveFile(doc.path, false, false, err)) {
        ErrorBox(hwnd_, TW("Save failed:") + L"\n" + U2W(T(err)));
        return false;
    }
    Toasts(Fmt(T("Saved %s"), W2U(doc.displayName).c_str()));
    return true;
}

bool App::SaveAs() {
    if (!doc.Raw()) return false;
    if (editing) CommitInlineEdit();
    std::wstring name = DocName();
    std::wstring out;
    if (!SaveFileDialog(hwnd_, PdfFilter(), name.c_str(), L"pdf", out)) return false;
    std::string err;
    doc.KillFormFocus();
    if (!doc.SaveFile(out, false, false, err)) {
        ErrorBox(hwnd_, TW("Save failed:") + L"\n" + U2W(T(err)));
        return false;
    }
    Toasts(Fmt(T("Saved %s"), W2U(doc.displayName).c_str()));
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
        Toasts(T("Undone"));
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
        Toasts(T("Redone"));
    }
}

void App::DoFlatten() {
    if (!doc.Raw()) return;
    if (AskYesNoCancel(hwnd_,
            TW("Flatten merges all form fields and annotations into the page content.\n"
               "Afterwards they can no longer be edited as fields. Continue?").c_str(),
            TW("Flatten").c_str()) != IDYES)
        return;
    EndFormFocus();
    if (ops::FlattenAll(doc)) {
        ClearSelection();
        CommitIfDirty("Flatten");
        Toasts(T("Flattened annotations and form fields"));
    } else {
        Toasts(T("Nothing to flatten"), true);
    }
}

void App::FlattenCurrentPage() {
    if (!doc.Raw()) return;
    int pg = AnnotSelected() ? annSelPage : currentPage;
    EndFormFocus();
    if (ops::FlattenPage(doc, pg)) {
        ClearSelection();
        CommitIfDirty("Annotations to content");
        Toasts(Fmt(T("Page %d: annotations are now normal content - click them to edit, split or move"), pg + 1));
    } else {
        Toasts(T("This page has no annotations to convert"), true);
    }
}

void App::ExportImagesDialog(float dpi) {
    if (!doc.Raw()) return;
    std::wstring dir;
    if (!PickFolderDialog(hwnd_, dir, TW("Choose a folder for the page images").c_str())) return;
    std::vector<int> pages;
    for (int i = 0; i < doc.PageCount(); ++i) pages.push_back(i);
    std::string err;
    if (ops::ExportPagesToImages(doc, pages, dir, dpi, err))
        Toasts(Fmt(T("Exported %d page image(s) at %.0f DPI"), (int)pages.size(), dpi));
    else
        Toasts(std::string(T("Export failed: ")) + T(err), true);
}

void App::ExtractTextDialog() {
    if (!doc.Raw()) return;
    extractedText = W2U(ops::ExtractAllText(doc));
    showExtract = true;
}

void App::UpdateTitle() {
    std::wstring app = TW("PDF Editor");
    std::wstring t = app;
    if (doc.Raw())
        t = DocName() + (doc.modified ? L" *" : L"") + L" - " + app;
    if (t != lastTitle) {
        SetWindowTextW(hwnd_, t.c_str());
        lastTitle = t;
    }
}

// --------------------------------------------------------------- frame

// A thin vertical bar between two panels; dragging it resizes the panel.
// `sign` +1: dragging right makes the panel wider (left panel), -1: narrower.
static void Splitter(const char* id, float& width, float sign, float h, float dpi) {
    float w = 6.0f * dpi;
    ImGui::InvisibleButton(id, ImVec2(w, h));
    bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive()) width += ImGui::GetIO().MouseDelta.x * sign;
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    float x = (a.x + b.x) * 0.5f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(x, a.y), ImVec2(x, b.y),
                                        hot ? theme::AccentU32 : ImGui::GetColorU32(theme::Line), hot ? 2.0f * dpi : 1.0f);
}

void App::Draw() {
    UpdateTitle();
    float s = dpiScale;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_MenuBar;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * s, 7 * s));   // roomier menu bar
    ImGui::Begin("##root", nullptr, flags);
    ImGui::PopStyleVar(3);

    DrawMenuBar();
    DrawToolbar();
    DrawSearchBar();

    // Panels follow the window size: sensible proportions, draggable
    // dividers, and on narrow windows the page list steps aside so the page
    // itself keeps enough room.
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float split = 6.0f * s;
    bool open = doc.Raw() != nullptr;                // start screen: just the page area
    bool thumbsOn = open && showThumbs && avail.x >= 760 * s;
    bool propsOn = open && showProps && avail.x >= 520 * s;
    float thumbsW = 0, propsW = 0;
    if (thumbsOn) {
        float want = thumbsWUser > 0 ? thumbsWUser : std::clamp(avail.x * 0.11f, 120 * s, 200 * s);
        thumbsW = std::clamp(want, 96 * s, std::max(96 * s, avail.x * 0.3f));
    }
    if (propsOn) {
        float want = propsWUser > 0 ? propsWUser : std::clamp(avail.x * 0.24f, 270 * s, 400 * s);
        propsW = std::clamp(want, 220 * s, std::max(220 * s, avail.x * 0.45f));
    }
    float minCanvas = 260 * s;
    float used = thumbsW + propsW + (thumbsOn ? split : 0) + (propsOn ? split : 0);
    if (avail.x - used < minCanvas && propsOn) propsW = std::max(200 * s, propsW - (minCanvas - (avail.x - used)));

    if (thumbsOn) {
        ImGui::BeginChild("##thumbs", ImVec2(thumbsW, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding);
        DrawThumbnails();
        ImGui::EndChild();
        ImGui::SameLine(0, 0);
        float w = thumbsW;
        Splitter("##splitL", w, +1.0f, avail.y, s);
        if (w != thumbsW) thumbsWUser = w;
        ImGui::SameLine(0, 0);
    }
    float canvasW = std::max(50.0f, avail.x - thumbsW - propsW - (thumbsOn ? split : 0) - (propsOn ? split : 0));
    ImGui::BeginChild("##canvas", ImVec2(canvasW, avail.y), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawCanvas();
    ImGui::EndChild();
    if (propsOn) {
        ImGui::SameLine(0, 0);
        float w = propsW;
        Splitter("##splitR", w, -1.0f, avail.y, s);
        if (w != propsW) propsWUser = w;
        ImGui::SameLine(0, 0);
        ImGui::BeginChild("##props", ImVec2(propsW, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding);
        DrawProperties();
        ImGui::EndChild();
    }
    DrawModals();
    ImGui::End();

    DrawInlineEditor();

    // Notification: a small card at the bottom centre; long messages wrap.
    if (!toast.text.empty() && ImGui::GetTime() < toast.until) {
        float maxW = std::min(vp->WorkSize.x * 0.6f, 720 * s);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y - 24 * s),
                                ImGuiCond_Always, ImVec2(0.5f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8 * s);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * s, 10 * s));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::Raised);
        ImGui::PushStyleColor(ImGuiCol_Border, toast.error ? theme::Bad : theme::Line);
        ImGui::Begin("##toast", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::TextColored(toast.error ? theme::Bad : theme::Good, "%s", toast.error ? ICON_INFO : ICON_CHECK);
        ImGui::SameLine(0, 10 * s);
        float textW = std::min(ImGui::CalcTextSize(toast.text.c_str()).x, maxW);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + textW + 1.0f);
        ImGui::TextUnformatted(toast.text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
    }

    // global shortcuts (never while typing in a text box)
    ImGuiIO& io = ImGui::GetIO();
    bool typing = io.WantTextInput || editing;
    if (ImGui::IsKeyPressed(ImGuiKey_F1)) showHelp = true;
    if (io.KeyCtrl && !typing) {
        if (ImGui::IsKeyPressed(ImGuiKey_S)) { if (io.KeyShift) SaveAs(); else Save(); }
        if (ImGui::IsKeyPressed(ImGuiKey_O)) OpenDialog();
        if (ImGui::IsKeyPressed(ImGuiKey_P) && doc.Raw()) Print();
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

// ------------------------------------------------------------------ printing

void App::Print() {
    if (!doc.Raw()) return;
    if (editing) CommitInlineEdit();
    std::wstring title = doc.path.empty() ? std::wstring(L"PDF") : doc.path.substr(doc.path.find_last_of(L"\\/") + 1);
    PrintResult r = PrintDocument(hwnd_, doc, currentPage, title);
    if (r.printed) Toasts(Fmt(T("Sent %d page(s) to the printer"), r.pages));
    else if (!r.cancelled && !r.error.empty()) Toasts(std::string(T("Printing failed: ")) + T(r.error), true);
}

// ------------------------------------------------------- default PDF app

void App::MakeDefaultPdfApp() {
    std::string err;
    if (!RegisterPdfApp(L"Software", ThisExePath(), err)) { Toasts(std::string(T("Could not register: ")) + T(err), true); return; }
    OpenDefaultAppSettings();
    Toasts(T("In Windows Settings, set PDF Editor as the default for .pdf"));
}
