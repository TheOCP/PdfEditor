#pragma once
// Editing operations layered on top of Document. Everything here mutates the
// live PDFium page objects / page tree; callers mark the page dirty via the
// Document and push an undo snapshot (Document::Commit) once an action settles.
#include "pdf_doc.h"
#include "imageio.h"
#include "sysfonts.h"
#include <optional>

namespace ops {

// ---- object inspection -----------------------------------------------------

struct ObjInfo {
    FPDF_PAGEOBJECT obj = nullptr;
    FPDF_PAGEOBJECT parent = nullptr;   // containing Form XObject, or null at top level
    int type = 0;                       // FPDF_PAGEOBJ_*
    PRect bounds;                        // PAGE space (points), y up — already mapped
    FS_MATRIX accum{ 1, 0, 0, 1, 0, 0 }; // container space -> page space
};

// Every selectable leaf object on a page, descending through Form XObjects so
// individual nested lines / images / paths are reachable (not just the container).
std::vector<ObjInfo> ListObjects(Document& d, int page);

// Smallest leaf whose (tolerance-expanded) page bounds contain (px,py); this
// makes thin lines easy to click. tolPt is in PDF points.
std::optional<ObjInfo> HitTest(Document& d, int page, float px, float py, float tolPt = 2.0f);

int ObjectType(FPDF_PAGEOBJECT obj);
PRect ObjectBounds(FPDF_PAGEOBJECT obj);                    // local (container) space
PRect ObjectPageBounds(FPDF_PAGEOBJECT obj, const FS_MATRIX& accum);  // mapped to page space

// ---- text ------------------------------------------------------------------

struct TextProps {
    std::wstring text;
    double fontSize = 12;
    std::string fontName;                 // PDF BaseFont of the object
    unsigned r = 0, g = 0, b = 0, a = 255; // fill colour
    bool isText = false;
};

TextProps ReadText(Document& d, int page, FPDF_PAGEOBJECT obj);

// Replaces a text object's string in place, keeping its font, size, colour and
// position. Returns false if the object isn't text or the string can't be set.
bool SetObjectText(Document& d, int page, FPDF_PAGEOBJECT obj, const std::wstring& text);

void SetObjectFillColor(Document& d, int page, FPDF_PAGEOBJECT obj, unsigned r, unsigned g, unsigned b, unsigned a);

// Creates a text object and inserts it at (px,py) = baseline origin, page space.
// `fc` chooses the font; returns the new object or nullptr.
FPDF_PAGEOBJECT AddText(Document& d, int page, const std::wstring& text, const FontChoice& fc,
                        double fontSize, float px, float py, unsigned r, unsigned g, unsigned b,
                        std::string& err);

// Re-flows a text object onto a different font / size, preserving position+colour.
bool RestyleText(Document& d, int page, FPDF_PAGEOBJECT obj, const FontChoice& fc, double fontSize,
                 std::string& err);

// ---- geometry --------------------------------------------------------------

void MoveObject(Document& d, int page, FPDF_PAGEOBJECT obj, float dxPt, float dyPt);
void SetObjectPos(Document& d, int page, FPDF_PAGEOBJECT obj, float leftPt, float bottomPt);
// Scales about the object's lower-left corner.
void ScaleObject(Document& d, int page, FPDF_PAGEOBJECT obj, float sx, float sy);
bool DeleteObject(Document& d, int page, FPDF_PAGEOBJECT obj);

// --- nested-aware variants (work for objects inside Form XObjects too) ------
// `accum` is the object's container->page matrix (ObjInfo::accum); identity for
// a top-level object. Movement is expressed in PAGE points regardless of depth.
void TranslateObjectPage(Document& d, int page, FPDF_PAGEOBJECT obj, const FS_MATRIX& accum,
                         float dxPage, float dyPage);
void SetObjectPosPage(Document& d, int page, FPDF_PAGEOBJECT obj, const FS_MATRIX& accum,
                      float leftPage, float bottomPage);
// Removes the object from its real parent (a page or a Form XObject).
bool DeleteObjectNested(Document& d, int page, FPDF_PAGEOBJECT obj, FPDF_PAGEOBJECT parent);

// If `target` sits inside a Form XObject, flatten the whole top-level group that
// contains it onto the page (baking each form's matrix into its children), so
// `target` becomes a top-level page object. This is required before edits can
// persist: FPDFPage_GenerateContent only rewrites the page stream, not the
// stream inside a form XObject. `target`'s handle stays valid (same pointer).
// Returns true if target is top-level afterwards (or already was).
bool PromoteObject(Document& d, int page, FPDF_PAGEOBJECT target);

// Breaks a top-level Form XObject into its child objects on the page.
bool ExplodeForm(Document& d, int page, FPDF_PAGEOBJECT form);

// ---- images ----------------------------------------------------------------

// Places `img` with its lower-left at (px,py), drawn `widthPt` wide (height
// follows the aspect ratio). Returns the new image object or nullptr.
FPDF_PAGEOBJECT AddImage(Document& d, int page, const Image& img, float px, float py, float widthPt,
                         std::string& err);

// ---- shapes / redaction ----------------------------------------------------

FPDF_PAGEOBJECT AddRect(Document& d, int page, PRect box, bool fill, unsigned fr, unsigned fg, unsigned fb,
                        bool stroke, unsigned sr, unsigned sg, unsigned sb, float strokeW);

// A single straight line segment as its own editable stroked path object.
FPDF_PAGEOBJECT AddLine(Document& d, int page, float x1, float y1, float x2, float y2,
                        unsigned r, unsigned g, unsigned b, float width);

// Builds a table/grid as individual line objects inside `box`, so every line can
// be selected, moved, resized or deleted on its own. `cols`/`rows` are the body
// divisions; `header` adds a separator line `headerH` points below the top edge.
// Returns the number of line objects created.
int AddTable(Document& d, int page, PRect box, int cols, int rows, bool header, float headerH,
             unsigned r, unsigned g, unsigned b, float width);

// Maps the object's current page bounds onto `target` (scale + reposition).
// Top-level objects only — promote first. Degenerate axes (e.g. a line's
// thickness) are left unscaled so lines keep their width.
void ResizeObjectPageTopLevel(Document& d, int page, FPDF_PAGEOBJECT obj, PRect target);

// Stroke of a path object (line colour + width).
struct StrokeProps { bool isPath = false; float width = 1; unsigned r = 0, g = 0, b = 0, a = 255; };
StrokeProps ReadStroke(FPDF_PAGEOBJECT obj);
void SetObjectStroke(Document& d, int page, FPDF_PAGEOBJECT obj, unsigned r, unsigned g, unsigned b, float width);

// Applies an arbitrary affine to a top-level object (used for group move/resize).
void TransformObjectPage(Document& d, int page, FPDF_PAGEOBJECT obj, const FS_MATRIX& m);

// ---- grouping ------------------------------------------------------------
// Objects are grouped by a persistent content-mark tag ("PEGroup"/id) that
// survives save + reload, so a set of lines can act as one object and be split
// again. Grouping needs top-level objects (promote nested ones first).
int  GetGroupId(FPDF_PAGEOBJECT obj);                 // 0 = ungrouped
void SetGroupId(Document& d, int page, FPDF_PAGEOBJECT obj, int id);
void ClearGroupId(Document& d, int page, FPDF_PAGEOBJECT obj);
int  MaxGroupId(Document& d);                         // highest id in use (0 if none)
std::vector<FPDF_PAGEOBJECT> GroupMembers(Document& d, int page, int id);

// Removes every top-level object fully inside `box`, then paints `box` solid.
// This actually deletes the covered content (unlike a black highlight on top).
int Redact(Document& d, int page, PRect box, unsigned r, unsigned g, unsigned b);

// ---- text extraction / search ---------------------------------------------

std::wstring ExtractText(Document& d, int page);
std::wstring ExtractAllText(Document& d);

struct SearchHit {
    int page;
    PRect box;   // page space
};
std::vector<SearchHit> SearchText(Document& d, const std::wstring& needle, bool matchCase, bool whole);

// ---- page tree -------------------------------------------------------------

bool InsertBlankPage(Document& d, int at, float wPt, float hPt);
bool DeletePages(Document& d, const std::vector<int>& pages);   // 0-based
bool MovePage(Document& d, int from, int to);
bool RotatePage(Document& d, int page, int quartersCW);         // relative
bool DuplicatePage(Document& d, int page);
// Appends (or inserts at `at`, -1 = end) pages `range` ("1-3,5") from another PDF file.
bool ImportPagesFromFile(Document& d, const std::wstring& file, const std::string& pw,
                         const std::string& range, int at, std::string& err);
// Renders each page of `img`-like source? no — rasterizes selected pages to PNGs.
bool ExportPagesToImages(Document& d, const std::vector<int>& pages, const std::wstring& dir,
                         float dpi, std::string& err);

// ---- forms / annotations ---------------------------------------------------

struct FieldInfo {
    int page;
    std::wstring name;
    std::wstring value;
    int type;        // FPDF_FORMFIELD_*
    PRect box;
};
std::vector<FieldInfo> ListFormFields(Document& d);
// Bakes annotations + form fields into page content so values can't be changed.
bool FlattenAll(Document& d);

// ---- metadata --------------------------------------------------------------

std::wstring GetMeta(Document& d, const char* key);   // "Title","Author","Subject","Keywords","Creator","Producer"

} // namespace ops
