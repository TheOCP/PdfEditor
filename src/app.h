#pragma once
// App: the whole editor state. The window host (main.cpp) owns one App and calls
// Draw() each frame; the UI is split across app.cpp (menu/commands), canvas.cpp
// (the page viewport + editing interaction), panels.cpp (side panels + modals)
// and dialogs.cpp (native file pickers).
#include "pdf_doc.h"
#include "pdf_ops.h"
#include "gfx.h"
#include "sysfonts.h"
#include "i18n.h"
#include <memory>

enum class Tool { Select, Text, Image, Rect, Redact, Cut, Hand };

struct Selection {
    int page = -1;
    FPDF_PAGEOBJECT obj = nullptr;
    FPDF_PAGEOBJECT parent = nullptr;          // containing Form XObject, null = top level
    FS_MATRIX accum{ 1, 0, 0, 1, 0, 0 };       // container space -> page space
    uint32_t gen = 0;                          // Document generation when captured
    PRect bounds;                              // page space
    void Clear() { page = -1; obj = nullptr; parent = nullptr; accum = { 1, 0, 0, 1, 0, 0 }; }
    bool Valid(Document& d) const { return obj && page >= 0 && gen == d.Generation(); }
    void Set(Document& d, int pg, const ops::ObjInfo& o) {
        page = pg; obj = o.obj; parent = o.parent; accum = o.accum; bounds = o.bounds; gen = d.Generation();
    }
    PRect PageBounds() const { return obj ? ops::ObjectPageBounds(obj, accum) : bounds; }
};

// One cached page raster. At normal zoom it covers the whole page; at high zoom
// it covers only the visible part (a tile) so the page stays sharp.
struct PageTex {
    Texture tex;
    uint32_t version = ~0u;
    float scale = 0;             // device px per point it was rendered at
    int tx = 0, ty = 0, tw = 0, th = 0;   // covered region, in display pixels of the page
    ImVec2 topLeft{ 0, 0 };      // screen rect of the page last frame (for hit testing)
    float dispW = 0, dispH = 0;
    bool laid = false;
};

struct ThumbTex {
    Texture tex;
    uint32_t version = ~0u;
    int w = 0;
};

struct Toast {
    std::string text;
    double until = 0;
    bool error = false;
};

class App {
public:
    void Init(HWND hwnd);
    void Draw();
    void Shutdown();
    // Asks to save unsaved changes. Returns false if the user cancelled.
    bool ConfirmDiscard();
    float dpiScale = 1.0f;

    // --- commands (app.cpp) ---
    void NewDoc();
    void OpenDialog();
    void OpenPath(const std::wstring& path);
    bool Save();
    bool SaveAs();
    void Undo();
    void Redo();
    void DoFlatten();
    void FlattenCurrentPage();
    void ExportImagesDialog(float dpi);
    void ExtractTextDialog();
    void Toasts(const std::string& msg, bool error = false);
    void ChangeLanguage(Lang l, bool save = true);   // applies right away, remembered
    void DrawWelcome(ImVec2 origin, ImVec2 region);  // start screen (no document open)
    void ChangeUiScale(float factor);           // 1.0 = Windows scaling

    // --- canvas (canvas.cpp) ---
    void DrawCanvas();
    void HandleCanvasInput(bool hovered, ImVec2 origin, ImVec2 region);
    void InvalidatePage(int i);
    void InvalidateAll();
    void ScrollToPage(int i);
    void ScrollToRect(int page, PRect r);
    ImVec2 PageToScreenPt(int page, float px, float py);
    bool ScreenToPage(ImVec2 screen, int& page, float& px, float& py);

    // --- panels (panels.cpp) ---
    void DrawMenuBar();
    void DrawToolbar();
    void DrawThumbnails();
    void DrawProperties();
    void DrawSearchBar();
    void DrawModals();
    void DrawInlineEditor();

    // --- inline text editing ---
    void BeginEditText(int page, FPDF_PAGEOBJECT obj);
    void BeginNewText(int page, float px, float py);
    void CommitInlineEdit();
    void CancelInlineEdit();

    // --- selection ---
    void SelectObjectAt(int page, float px, float py);
    bool EnsureSelectionEditable();             // promote a nested selection to the page
    void ClearSelection();
    void SelectAllOnPage(bool linesOnly);
    void GroupAllLines();
    void RefreshGroupSel();
    PRect SelectionBounds();
    void MoveSelection(float dxPage, float dyPage);
    void ResizeSelectionTo(PRect target);
    void DeleteSelection();
    void GroupSelection();
    void UngroupSelection();
    void SplitSelectedPath(bool pickNearest, float px, float py);
    void SelectNewObject(int pg, FPDF_PAGEOBJECT o);
    void SelectObjects(int pg, const std::vector<FPDF_PAGEOBJECT>& objs, int groupId);
    void CutPieceFromImage(int page, PRect box);

    // --- annotations ---
    bool AnnotSelected() const { return annSelPage >= 0 && annSelIdx >= 0; }
    void ClearAnnotSel() { annSelPage = -1; annSelIdx = -1; }

    // --- clipboard / cross-page ---
    void CopySelection();
    void CutSelection();
    void PasteClipboard(bool atCursor);         // in place, or at the mouse
    void PasteAt(int page, float px, float py); // "Paste here"
    void TransferSelectionToPage(int tp, float dx = 0, float dy = 0);
    void ClearLimbo();

    void CommitIfDirty(const std::string& label);
    void ForwardFormKey(int vk, bool down);
    void ForwardFormChar(unsigned int ch);
    void EndFormFocus();

    // Font menu entries: fonts stored in the open PDF (most used first), then
    // the 14 standard PDF fonts, then fonts installed on this PC.
    const std::vector<FontChoice>& FontList();
    FontChoice CurrentFont();                // resolves a live handle for PDF fonts
    int docFontCount = 0;                    // how many entries at the top come from the PDF
    // New text in the font picked in the list (a PDF font keeps its exact look,
    // missing characters handled like an edit).
    FPDF_PAGEOBJECT AddTextInFont(int page, const std::wstring& text, double size, float x, float y,
                                  unsigned r, unsigned g, unsigned b, std::string& err, ops::TextEditInfo* info);

    // ============================== state
    HWND hwnd_ = nullptr;
    Document doc;
    Tool tool = Tool::Select;
    Selection sel;

    // view
    float zoom = 1.0f;
    float scrollX = 0, scrollY = 0;
    bool fitPending = true;
    bool showThumbs = true;
    bool showProps = true;
    int pendingScrollPage = -1;
    int pendingScrollRectPage = -1;
    PRect pendingScrollRect;
    int currentPage = 0;         // the most visible page
    int formPage = -1;           // page whose form field has focus
    std::vector<PageTex> cache;
    std::vector<ThumbTex> thumbs;
    ImVec2 canvasOrigin{ 0, 0 }, canvasSize{ 0, 0 };

    // pointer
    int hoverPage = -1;
    float hoverPx = 0, hoverPy = 0;

    // new-object style
    float fillColor[4] = { 0.10f, 0.10f, 0.12f, 1.0f };
    float strokeColor[4] = { 0.85f, 0.15f, 0.15f, 1.0f };
    bool rectFilled = true;
    bool rectStroked = false;
    float strokeWidth = 1.5f;
    int fontIndex = 0;
    float fontSize = 12.0f;
    bool matchNearbyFont = true;
    // style sampled from the text nearest the last Text-tool click
    FPDF_PAGEOBJECT pendingStyleObj = nullptr;
    uint32_t pendingStyleGen = 0;
    std::vector<uint8_t> pendingFontData;
    std::string pendingFontBase;
    double pendingFontSize = 0;
    unsigned pendingColor = 0;
    bool pendingFontValid = false;

    // inline text editor
    bool editing = false;
    bool editIsNew = false;
    bool editFocus = false;
    int editPage = -1;
    FPDF_PAGEOBJECT editObj = nullptr;
    float editAtX = 0, editAtY = 0;
    std::string editBuf;
    std::string editOriginal;

    // drag interaction
    bool dragging = false;
    bool dragArmed = false;      // moves start only after a small threshold
    int dragMode = 0;            // 1 move, 2 rect, 3 redact, 4 resize, 5 marquee, 6 annot, 7 cut
    int dragPage = -1;
    ImVec2 dragStartScreen{ 0, 0 };
    float dragStartPx = 0, dragStartPy = 0;
    float dragLastPx = 0, dragLastPy = 0;
    float grabDX = 0, grabDY = 0;           // cursor minus selection lower-left at grab
    PRect dragRect;
    int resizeHandle = -1;
    PRect resizeStart;
    bool nudgeDirty = false;
    int ghostPage = -1;                     // cross-page drop preview
    PRect ghostRect;

    // multi-selection / grouping
    std::vector<FPDF_PAGEOBJECT> groupSel;
    int selGroupId = 0;
    bool openObjCtx = false;
    int ctxPage = -1;
    float ctxPx = 0, ctxPy = 0;
    PRect marquee;

    // annotation selection
    int annSelPage = -1, annSelIdx = -1;

    // clipboard
    std::vector<ops::ClipObject> clip;
    std::vector<FPDF_PAGEOBJECT> limbo;     // cut objects kept alive for an exact paste
    uint32_t limboDoc = 0;
    int clipSrcPage = -1;
    std::string clipText;                   // what we last put on the system clipboard
    unsigned long clipSeq = 0;              // Windows clipboard sequence number at our copy
    int pasteCount = 0;

    // table / form generator
    bool showTable = false;
    int tableCols = 4;
    int tableRows = 1;
    bool tableHeader = true;
    float tableHeaderH = 24.0f;
    float tableMargin = 54.0f;
    float tableLineW = 1.0f;
    float tableColor[3] = { 0, 0, 0 };

    // search
    std::string searchQuery;
    bool searchCase = false;
    bool searchWhole = false;
    std::vector<ops::SearchHit> searchHits;
    std::string searchedFor;                // query the current results belong to
    int searchCurrent = -1;
    bool searchDirty = false;
    bool focusSearch = false;

    // modals
    bool showPassword = false;
    std::wstring pwFile;
    std::string pwInput;
    std::string pwError;
    bool showMeta = false;
    bool showAbout = false;
    bool showHelp = false;
    bool showSettings = false;
    bool uiScaleChanged = false;            // fonts must be rebuilt (window host)
    bool uiShotOpenAll = false;             // --ui-shots: open collapsible sections
    float thumbsWUser = 0, propsWUser = 0;  // panel widths set by dragging (0 = automatic)
    ImFont* fontTitle = nullptr;            // large semibold font (start screen)
    bool showImport = false;
    std::wstring importFile;
    std::string importRange;
    std::string importPw;
    int importAt = -1;
    bool showExtract = false;
    std::string extractedText;

    Toast toast;
    std::wstring lastTitle;

private:
    void ResetAfterLoad();
    void UpdateTitle();
    std::vector<FontChoice> fontList_;
    bool fontListBuilt_ = false;
    uint32_t fontListDoc_ = 0;
    unsigned fontListLang_ = 0;
    bool fontDefaultPending = true;   // a new PDF was opened: default to its most used font
};
