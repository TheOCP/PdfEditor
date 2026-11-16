#pragma once
// App: the whole editor state. The window host (main.cpp) owns one App and calls
// Draw() each frame; the UI is split across app.cpp (menu/commands), canvas.cpp
// (the page viewport + editing interaction), panels.cpp (side panels + modals)
// and dialogs.cpp (native file pickers).
#include "pdf_doc.h"
#include "pdf_ops.h"
#include "gfx.h"
#include "sysfonts.h"
#include <deque>
#include <memory>

enum class Tool { Select, Text, Image, Rect, Redact, Hand };

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
    // Current page-space bounds (recomputed; nested objects move within a form).
    PRect PageBounds() const { return obj ? ops::ObjectPageBounds(obj, accum) : bounds; }
};

// One cached page raster + the layout rectangle it occupied last frame.
struct PageTex {
    Texture tex;
    uint32_t version = ~0u;
    float scale = 0;         // device px per point it was rendered at
    // screen-space rect from the last laid-out frame (for hit testing)
    ImVec2 topLeft{ 0, 0 };
    float dispW = 0, dispH = 0;  // on-screen size in pixels
    bool laid = false;
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

    // --- commands (app.cpp) ---
    void NewDoc();
    void OpenDialog();
    void OpenPath(const std::wstring& path);
    bool Save();
    bool SaveAs();
    void Undo();
    void Redo();
    void DoFlatten();
    void ExportImagesDialog();
    void ExtractTextDialog();

    void Toasts(const std::string& msg, bool error = false);

    // --- canvas (canvas.cpp) ---
    void DrawCanvas();
    void HandleCanvasInput(bool hovered);
    void InvalidatePage(int i);
    void InvalidateAll();
    void ScrollToPage(int i);

    // --- panels (panels.cpp) ---
    void DrawMenuBar();
    void DrawToolbar();
    void DrawThumbnails();
    void DrawProperties();
    void DrawSearchBar();
    void DrawModals();

    // state ------------------------------------------------------------------
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
    int currentPage = 0;         // top-most visible page
    int formPage = -1;           // page whose form field currently has focus
    std::vector<PageTex> cache;

    // new-object style
    float fillColor[4] = { 0.10f, 0.10f, 0.12f, 1.0f };
    float strokeColor[4] = { 0.85f, 0.15f, 0.15f, 1.0f };
    bool rectFilled = true;
    bool rectStroked = false;
    float strokeWidth = 1.5f;
    int fontIndex = 0;           // index into FontList()
    float fontSize = 14.0f;
    bool matchNearbyFont = true; // new text copies the PDF's font near the click
    // font sampled at the last Text-tool click (the PDF's own font)
    std::vector<uint8_t> pendingFontData;
    std::string pendingFontBase;
    double pendingFontSize = 0;
    bool pendingFontValid = false;

    // inline text editor
    bool editing = false;
    bool editIsNew = false;
    int editPage = -1;
    FPDF_PAGEOBJECT editObj = nullptr;
    float editAtX = 0, editAtY = 0;     // placement point for a new text obj
    std::string editBuf;

    // drag interaction
    bool dragging = false;
    int dragMode = 0;            // 0 none, 1 move obj, 2 rect, 3 redact, 4 resize
    int dragPage = -1;
    ImVec2 dragStartScreen{ 0, 0 };
    float dragStartPx = 0, dragStartPy = 0;
    float dragLastPx = 0, dragLastPy = 0;
    PRect dragRect;
    int resizeHandle = -1;       // 0..7 = TL,T,TR,R,BR,B,BL,L
    PRect resizeStart;           // selection page bounds at grab
    bool nudgeDirty = false;     // arrow-key edits pending a commit

    // multi-selection / grouping (all members are top-level objects on sel.page)
    std::vector<FPDF_PAGEOBJECT> groupSel;
    int selGroupId = 0;          // >0 if the selection is a saved group
    bool openObjCtx = false;     // request to open the object context menu
    PRect marquee;               // marquee rectangle (dragMode 5)

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

    bool showImport = false;
    std::wstring importFile;
    std::string importRange;
    std::string importPw;
    int importAt = -1;

    bool showExtract = false;
    std::string extractedText;

    float exportDpi = 150.0f;

    Toast toast;

    // helpers ----------------------------------------------------------------
    const std::vector<FontChoice>& FontList();
    FontChoice CurrentFont();
    bool HasDoc() const { return const_cast<Document&>(doc).PageCount() > 0 || const_cast<Document&>(doc).Raw(); }
    // Map a screen point to (page, pageX, pageY); returns false if not over a page.
    bool ScreenToPage(ImVec2 screen, int& page, float& px, float& py);
    void SelectObjectAt(int page, float px, float py);
    // Promotes a nested selection to a top-level object so edits persist on save.
    // Returns true if the selection is editable afterwards.
    bool EnsureSelectionEditable();

    // --- multi-selection / grouping ---
    // Select every top-level object on the current page; linesOnly restricts to
    // path/line objects (skips text and images) so a table frame can be grouped
    // and resized without dragging its text along.
    void SelectAllOnPage(bool linesOnly);
    void GroupAllLines();                   // select all lines + group in one step
    void RefreshGroupSel();                 // rebuild groupSel from sel (whole group if grouped)
    PRect SelectionBounds();                // union page bounds of groupSel
    void MoveSelection(float dxPage, float dyPage);
    void ResizeSelectionTo(PRect target);   // scales all members about current bounds
    void DeleteSelection();
    void GroupSelection();                  // tag all members with one new group id
    void UngroupSelection();                // clear the tag (or explode a Form XObject)
    // Splits the selected multi-line path into individual lines; optionally keeps
    // the one nearest (px,py) selected so it can be moved immediately.
    void SplitSelectedPath(bool pickNearest, float px, float py);
    bool SelectionIsGrouped() const { return selGroupId != 0; }

    // --- clipboard / cross-page ---
    void CopySelection();
    void CutSelection();
    void PasteClipboard();                  // onto the current page
    void TransferSelectionToPage(int tp);   // move the selection to another page
    std::vector<ops::ClipObject> clip;
    void SelectNewObject(int pg, FPDF_PAGEOBJECT o) {
        sel.page = pg; sel.obj = o; sel.parent = nullptr;
        sel.accum = FS_MATRIX{ 1, 0, 0, 1, 0, 0 }; sel.gen = doc.Generation();
        groupSel.assign(1, o); selGroupId = 0;
    }
    void CommitIfDirty(const std::string& label);
    void ForwardFormKey(int vk, bool down);
    void ForwardFormChar(unsigned int ch);

private:
    std::vector<FontChoice> fontList_;
    bool fontListBuilt_ = false;
    bool pendingCommit_ = false;
    std::string pendingLabel_;
};
