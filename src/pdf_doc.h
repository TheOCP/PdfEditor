#pragma once
// Document: owns one PDFium document plus everything tied to its lifetime —
// lazily loaded pages / text pages, the form-fill environment, the font cache,
// and a snapshot-based undo/redo stack (every edit is undoable because each
// commit is a full serialized copy of the document).
#include <fpdfview.h>
#include <fpdf_edit.h>
#include <fpdf_text.h>
#include <fpdf_annot.h>
#include <fpdf_formfill.h>
#include <fpdf_save.h>
#include <fpdf_ppo.h>
#include <fpdf_flatten.h>
#include <fpdf_transformpage.h>
#include <fpdf_attachment.h>
#include <fpdf_doc.h>

#include <map>
#include <memory>
#include <string>
#include <vector>
#include "util.h"

using Bytes = std::vector<uint8_t>;
using BytesPtr = std::shared_ptr<Bytes>;

// PDF row-vector matrix: (x,y) -> (a x + c y + e, b x + d y + f).
FS_MATRIX MatMul(const FS_MATRIX& first, const FS_MATRIX& then);
FS_MATRIX MatTranslate(float x, float y);
FS_MATRIX MatScaleAbout(float sx, float sy, float ox, float oy);
FS_MATRIX MatRotateAbout(float degrees, float ox, float oy);
bool MatInvert(const FS_MATRIX& m, FS_MATRIX& out);
void MatApply(const FS_MATRIX& m, float x, float y, float& ox, float& oy);

// Font picked in the UI: one of the 14 standard PDF fonts, or a TTF file.
struct FontChoice {
    std::string stdName;      // e.g. "Helvetica" (used when file and docName are empty)
    std::wstring file;        // e.g. C:\Windows\Fonts\arial.ttf
    std::string docName;      // a font stored inside the open PDF (its BaseFont name)
    FPDF_FONT handle = nullptr;   // live handle for docName, resolved right before use
    std::string label;        // shown in the UI
    bool IsDoc() const { return !docName.empty(); }
    bool IsStandard() const { return file.empty() && docName.empty(); }
    std::string Key() const {
        return IsDoc() ? "doc:" + docName : file.empty() ? "std:" + stdName : "ttf:" + W2U(file);
    }
};

class Document;

struct FormInfo : FPDF_FORMFILLINFO {
    Document* owner = nullptr;
};

class Document {
public:
    Document();
    ~Document();
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    enum class Load { Ok, NeedPassword, Error };
    Load LoadFile(const std::wstring& file, const std::string& pw, std::string& err);
    Load LoadBytes(BytesPtr bytes, const std::string& pw, std::string& err);
    bool CreateBlank(float widthPt, float heightPt);
    // Releases the PDFium document now (must happen before FPDF_DestroyLibrary).
    void Close() { CloseDocument(); }

    // Writes the document. FPDF_NO_INCREMENTAL by default (clean rewrite).
    bool SaveFile(const std::wstring& file, bool removeSecurity, bool incremental, std::string& err);
    bool SaveBytes(Bytes& out, unsigned long flags);

    // ---- pages
    int PageCount() const { return (int)slots_.size(); }
    FS_SIZEF PageSizePt(int i) const;       // displayed size (page /Rotate applied)
    FPDF_PAGE Page(int i);                  // loads on demand
    FPDF_TEXTPAGE TextPage(int i);          // reloaded after the page changes
    int PageIndexOf(FPDF_PAGE p) const;
    int Rotation(int i);                    // 0..3 quarter turns clockwise
    PRect PageBox(int i);                   // crop box ∩ media box, page space

    // ---- change tracking
    void ContentChanged(int i);             // page objects edited (needs GenerateContent)
    void VisualChanged(int i);              // annots/forms changed (re-render only)
    void StructureChanged();                // page list/sizes changed: closes every page
    void Commit(const std::string& label);  // bakes + pushes an undo snapshot
    bool CanUndo() const { return !undo_.empty(); }
    bool CanRedo() const { return !redo_.empty(); }
    std::string UndoLabel() const { return current_.label; }
    std::string RedoLabel() const { return redo_.empty() ? "" : redo_.back().label; }
    bool Undo();
    bool Redo();
    uint32_t PageVersion(int i) const { return (i >= 0 && i < PageCount()) ? slots_[i].version : 0; }
    // Bumps whenever FPDF_PAGE / FPDF_PAGEOBJECT handles became invalid.
    uint32_t Generation() const { return generation_; }
    // Changes only when the underlying PDFium document is replaced (open, new,
    // undo/redo); page objects removed from a page stay valid while it is equal.
    uint32_t DocId() const { return docId_; }
    uint64_t EditCounter() const { return edits_; }

    // ---- rendering: BGRA of the device rect (rx,ry,rw,rh) of the page drawn at
    // `scale` device pixels per PDF point.
    bool Render(int i, float scale, int rx, int ry, int rw, int rh, std::vector<uint32_t>& out,
                bool annots = true, bool forms = true);

    // ---- page space <-> device pixels (page origin at 0,0) at `scale`
    void PageToDev(int i, float scale, float px, float py, float& dx, float& dy);
    void DevToPage(int i, float scale, float dx, float dy, float& px, float& py);

    // ---- fonts (cached per loaded document)
    FPDF_FONT GetFont(const FontChoice& fc, std::string& err);

    // ---- forms
    FPDF_FORMHANDLE Form() const { return form_; }
    void KillFormFocus();
    bool formDirty = false;
    int formCursor = 0;
    int currentPageHint = 0;

    FPDF_DOCUMENT Raw() const { return doc_; }
    void Bake();                            // GenerateContent on dirty pages

    std::wstring path;
    std::wstring displayName;
    bool modified = false;
    std::string password;

private:
    struct Slot {
        FPDF_PAGE page = nullptr;
        FPDF_TEXTPAGE text = nullptr;
        uint32_t textVersion = ~0u;
        uint32_t version = 1;
        bool contentDirty = false;
    };
    struct State {
        BytesPtr bytes;
        std::string label;
    };

    Load OpenFromBytes(BytesPtr bytes, const std::string& pw, std::string& err);
    void CloseAllPages();
    void CloseDocument();
    void InitForm();
    void RebuildSlots();
    void TrimUndo();

    FPDF_DOCUMENT doc_ = nullptr;
    FPDF_FORMHANDLE form_ = nullptr;
    FormInfo formInfo_{};
    BytesPtr loadedFrom_;                   // must outlive doc_ (PDFium reads lazily)
    State current_;
    std::vector<State> undo_, redo_;
    std::vector<Slot> slots_;
    std::vector<FS_SIZEF> sizes_;
    std::map<std::string, FPDF_FONT> fonts_;
    std::map<std::wstring, Bytes> fontData_;
    uint32_t generation_ = 1;
    uint32_t docId_ = 1;
    uint64_t edits_ = 1;
};

// Library lifetime (FPDF_InitLibraryWithConfig / FPDF_DestroyLibrary).
void PdfInit();
void PdfShutdown();
std::string PdfErrorString(unsigned long code);
