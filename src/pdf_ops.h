#pragma once
// Editing operations layered on top of Document. Everything here mutates the
// live PDFium page objects / page tree; callers mark the page dirty via the
// Document and push an undo snapshot (Document::Commit) once an action settles.
//
// Rules every operation here follows (each learned from PDFium's behaviour):
//  * Any transform of an existing object also transforms its clip path —
//    FPDFPageObj_Transform alone leaves the clip behind, so moved content that
//    has a clip (common in Word/browser/report PDFs) would vanish.
//  * Objects inside a Form XObject must be promoted to the page before an edit,
//    because FPDFPage_GenerateContent does not rewrite a form's own stream.
//  * Exploding a form keeps the children at the form's place in the stacking
//    order, so nothing jumps above or below other content.
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

// What is under (px,py)? Lines are hit by their actual geometry (distance to
// the drawn stroke, or inside a filled shape) rather than their bounding box,
// so clicking a table line picks the line and clicking empty cell space picks
// nothing. Text/images are hit by their bounds. tolPt is in PDF points.
std::optional<ObjInfo> HitTest(Document& d, int page, float px, float py, float tolPt = 2.0f);

int ObjectType(FPDF_PAGEOBJECT obj);
PRect ObjectBounds(FPDF_PAGEOBJECT obj);                                // own container space
PRect ObjectPageBounds(FPDF_PAGEOBJECT obj, const FS_MATRIX& accum);   // mapped to page space
// Distance (points) from (px,py) to a top-level path's drawn geometry; for
// other object types, 0 inside the bounds and the distance to them outside.
float DistanceToObject(FPDF_PAGEOBJECT obj, float px, float py);
// Still present on `page` (at any nesting depth)? Pointer comparison only.
bool ObjectExists(Document& d, int page, FPDF_PAGEOBJECT obj);
float MatrixScale(const FS_MATRIX& m);   // sqrt(|det|): uniform scale factor

// ---- text ------------------------------------------------------------------

struct TextProps {
    std::wstring text;
    double fontSize = 12;                 // visual size in points (Tf x matrix scale)
    double tf = 12;                       // raw Tf operand
    std::string fontName;                 // PDF BaseFont of the object
    bool embedded = false;
    unsigned r = 0, g = 0, b = 0, a = 255; // fill colour
    bool isText = false;
};

TextProps ReadText(Document& d, int page, FPDF_PAGEOBJECT obj);
// Visual font size of a text object in its container's space. Many PDFs write
// "Tf 1" and put the size into the text matrix, so the raw Tf alone is wrong.
float VisualFontSize(FPDF_PAGEOBJECT text);

// Replaces a text object's string in place, keeping font, size, colour, position.
bool SetObjectText(Document& d, int page, FPDF_PAGEOBJECT obj, const std::wstring& text);
// Same, but if the PDF's own (often subset) font cannot show some of the typed
// characters, the object is rebuilt with a matching installed font at the exact
// same size, rotation and position. *replaced gets the object now holding the
// text (== obj unless a fallback happened).
// What happened when text was typed into a PDF font.
struct TextEditInfo {
    std::wstring missing;                  // characters the PDF's font doesn't contain
    std::string fontName;                  // the PDF's font (subset prefix removed)
    std::string standIn;                   // installed font drawing only the missing characters
    bool otherCopy = false;                // another copy of the same font in this PDF had them all
    std::vector<FPDF_PAGEOBJECT> pieces;   // every object now showing the text, left to right
    int groupId = 0;                       // group tying the pieces together (0 = single object)
    std::string trace;                     // which steps ran and why (diagnostics, no text content)
};
// Keeps the object's own font. Characters that font doesn't contain are taken
// from another copy of the same font in the PDF if one has them; otherwise only
// those characters are drawn in the closest installed font, sized to match, and
// the rest stays in the PDF's font (the pieces are grouped and edit as one).
bool SetObjectTextSafe(Document& d, int page, FPDF_PAGEOBJECT obj, const std::wstring& text,
                       FPDF_PAGEOBJECT* replaced, bool* usedFallback, TextEditInfo* info = nullptr);
// Text drawn in pieces by SetObjectTextSafe: all pieces of the text `obj`
// belongs to, left to right (empty if it is a normal single object).
std::vector<FPDF_PAGEOBJECT> TextPieces(Document& d, int page, FPDF_PAGEOBJECT obj);
// The text as the editor shows it: all pieces joined (with their word gaps),
// or the object's own text.
std::wstring EditableText(Document& d, int page, FPDF_PAGEOBJECT obj);
// Changes only the size of a text object, keeping its font, spacing, colour
// and clip (scales it about its start point). visualSize is in page points.
bool SetTextVisualSize(Document& d, int page, FPDF_PAGEOBJECT obj, float visualSize);
// PostScript name of the font a text object uses (subset prefix removed).
std::string TextFontName(FPDF_PAGEOBJECT obj);

// Fonts stored inside the open PDF, most used first.
struct DocFont {
    std::string name;        // BaseFont as written in the PDF (may carry an "ABCDEF+" subset prefix)
    bool embedded = false;   // the font program is inside the PDF
    bool subset = false;     // only the characters the PDF uses are included
    int uses = 0;
};
std::vector<DocFont> ListDocFonts(Document& d, int maxPages = 40);
// A live handle for a font stored in the PDF, taken from a text object that
// uses it (searching `preferPage` first). Optionally returns that object.
FPDF_FONT FindDocFont(Document& d, const std::string& name, int preferPage,
                      FPDF_PAGEOBJECT* sample = nullptr, int* samplePage = nullptr);

void SetObjectFillColor(Document& d, int page, FPDF_PAGEOBJECT obj, unsigned r, unsigned g, unsigned b, unsigned a);

// Creates a text object with its baseline origin at (px,py), page space.
FPDF_PAGEOBJECT AddText(Document& d, int page, const std::wstring& text, const FontChoice& fc,
                        double fontSize, float px, float py, unsigned r, unsigned g, unsigned b,
                        std::string& err);
// Adds text in the font of an existing text object (`styleFrom`, if still alive
// in the same document) or else from raw font bytes / a matching installed font.
FPDF_PAGEOBJECT AddTextLike(Document& d, int page, const std::wstring& text, FPDF_PAGEOBJECT styleFrom,
                            const Bytes& fontData, const std::string& fontBase, double visualSize,
                            float px, float py, unsigned r, unsigned g, unsigned b, std::string& err,
                            TextEditInfo* info = nullptr);
// Re-renders a text object in another font at `visualSize`, keeping position,
// rotation, colour, stacking order and group tag.
bool RestyleText(Document& d, int page, FPDF_PAGEOBJECT obj, const FontChoice& fc, double visualSize,
                 std::string& err, FPDF_PAGEOBJECT* out = nullptr);

// ---- geometry (all clip-path aware) ----------------------------------------

// `accum` is the object's container->page matrix (ObjInfo::accum); identity for
// a top-level object. Movement is expressed in PAGE points regardless of depth.
void TranslateObjectPage(Document& d, int page, FPDF_PAGEOBJECT obj, const FS_MATRIX& accum,
                         float dxPage, float dyPage);
void SetObjectPosPage(Document& d, int page, FPDF_PAGEOBJECT obj, const FS_MATRIX& accum,
                      float leftPage, float bottomPage);
// Maps the object's current page bounds onto `target`. Top-level objects only.
void ResizeObjectPageTopLevel(Document& d, int page, FPDF_PAGEOBJECT obj, PRect target);
// Applies an arbitrary page-space affine to a top-level object (group move/resize).
void TransformObjectPage(Document& d, int page, FPDF_PAGEOBJECT obj, const FS_MATRIX& m);
// Removes the object from its real parent (a page or a Form XObject).
bool DeleteObjectNested(Document& d, int page, FPDF_PAGEOBJECT obj, FPDF_PAGEOBJECT parent);

// If `target` sits inside a Form XObject, flatten the whole top-level group that
// contains it onto the page so `target` becomes a top-level page object (same
// handle). Required before an edit to a nested object can be saved.
bool PromoteObject(Document& d, int page, FPDF_PAGEOBJECT target);
void PromoteAll(Document& d, int page, const std::vector<FPDF_PAGEOBJECT>& objs);
// Breaks a top-level Form XObject into its child objects on the page.
bool ExplodeForm(Document& d, int page, FPDF_PAGEOBJECT form);

// Moves the very same object to another page (keeps its font, image data,
// clip, everything), shifted by (dx,dy) page points. Top-level objects only.
bool MoveObjectToPage(Document& d, int srcPage, FPDF_PAGEOBJECT obj, int dstPage, float dx, float dy);

// ---- splitting shared paths -------------------------------------------------

int SubpathCount(FPDF_PAGEOBJECT obj);
// How many movable pieces SplitPath would produce (0 = nothing to split):
// several sub-paths -> one piece each; one stroked outline (e.g. a table cell
// drawn as a rectangle) -> one piece per side.
int SplittableParts(FPDF_PAGEOBJECT obj);
// Splits a path into independent line objects (see SplittableParts). A filled
// and stroked outline keeps its fill as a separate fill-only shape underneath.
// Curves, dash patterns, caps/joins, colours and group tags are preserved.
// Returns the line pieces. Top-level objects only.
std::vector<FPDF_PAGEOBJECT> SplitPath(Document& d, int page, FPDF_PAGEOBJECT obj);

// ---- images ----------------------------------------------------------------

FPDF_PAGEOBJECT AddImage(Document& d, int page, const Image& img, float px, float py, float widthPt,
                         std::string& err);
// Full-resolution pixels of an image object (straight BGRA), ignoring masks.
bool ImagePixels(FPDF_PAGEOBJECT img, Image& out);
// Lifts the part of a picture inside `box` out into its own movable image
// object; the hole left behind is filled with the surrounding background
// colour. Works for scanned pages where every "line" is just pixels.
// Returns the new piece (top-level) or nullptr.
FPDF_PAGEOBJECT CutImageRegion(Document& d, int page, FPDF_PAGEOBJECT img, PRect box, std::string& err);

// ---- shapes ------------------------------------------------------------------

FPDF_PAGEOBJECT AddRect(Document& d, int page, PRect box, bool fill, unsigned fr, unsigned fg, unsigned fb,
                        bool stroke, unsigned sr, unsigned sg, unsigned sb, float strokeW);
FPDF_PAGEOBJECT AddLine(Document& d, int page, float x1, float y1, float x2, float y2,
                        unsigned r, unsigned g, unsigned b, float width);
int AddTable(Document& d, int page, PRect box, int cols, int rows, bool header, float headerH,
             unsigned r, unsigned g, unsigned b, float width);

struct StrokeProps { bool isPath = false; float width = 1; unsigned r = 0, g = 0, b = 0, a = 255; };
StrokeProps ReadStroke(FPDF_PAGEOBJECT obj);
void SetObjectStroke(Document& d, int page, FPDF_PAGEOBJECT obj, unsigned r, unsigned g, unsigned b, float width);
// Lines, however the PDF draws them: a stroked path, a thin filled rectangle,
// a whole grid of thin rectangles in one path, or a thin picture.
bool  IsLine(FPDF_PAGEOBJECT obj);
// "stroke", "bar", "bars", "picture" or "" (for reports).
const char* LineKindName(FPDF_PAGEOBJECT obj);
// Thickness as drawn (container space; page points for top-level objects).
float LineThickness(FPDF_PAGEOBJECT obj);
// Makes the line exactly `thick` thick - thinner or thicker. Some lines have to
// be rebuilt as a new object: the object that now is the line is returned.
FPDF_PAGEOBJECT SetLineThickness(Document& d, int page, FPDF_PAGEOBJECT obj, float thick);
// Colour a line is drawn in (stroke for stroked lines, fill for bars).
bool LineColor(FPDF_PAGEOBJECT obj, unsigned& r, unsigned& g, unsigned& b);
void SetLineColor(Document& d, int page, FPDF_PAGEOBJECT obj, unsigned r, unsigned g, unsigned b);

// Where a horizontal text's baseline starts (page space). False for rotated
// or non-text objects.
bool TextOrigin(FPDF_PAGEOBJECT obj, const FS_MATRIX& accum, float& x, float& y);

// ---- alignment: moving text into line with other text ----
struct AlignRef { float ox = 0, oy = 0; bool origin = false; PRect box; };
// Every text on `page` except `exclude`, as things to line up with.
std::vector<AlignRef> AlignRefs(Document& d, int page, const std::vector<FPDF_PAGEOBJECT>& exclude);
struct AlignHit { float dx = 0, dy = 0; bool x = false, y = false; float gx = 0, gy = 0; };
// A selection with bounds `start` (and baseline origin ox/oy when hasOrigin)
// is being moved by (dx,dy). Finds the nearest line-up within `thr` points:
// left start, centre or right edge (x) and baseline (y). Returns the
// corrections to add and where the guide lines are.
AlignHit AlignSnap(const std::vector<AlignRef>& refs, PRect start, bool hasOrigin, float ox, float oy,
                   float dx, float dy, float thr);

// Resizes several top-level objects (e.g. a table) from `cur` to `target`.
// Lines keep their thickness and text keeps its size (it moves with its cell).
// keepSpacing: the spacing between lines stays as it is - only the lines on
// the dragged edge move, so the outer row / column takes up the change.
// Returns the bounds actually reached (an edge stops before an inner line).
PRect ResizeGroup(Document& d, int page, const std::vector<FPDF_PAGEOBJECT>& objs, PRect cur, PRect target,
                  bool keepSpacing);

// ---- grouping ------------------------------------------------------------
// Persistent "PEGroup"/id content-mark tag; survives save + reload.
int  GetGroupId(FPDF_PAGEOBJECT obj);                 // 0 = ungrouped
void SetGroupId(Document& d, int page, FPDF_PAGEOBJECT obj, int id);
void ClearGroupId(Document& d, int page, FPDF_PAGEOBJECT obj);
int  MaxGroupId(Document& d);
std::vector<FPDF_PAGEOBJECT> GroupMembers(Document& d, int page, int id);

// ---- clipboard ---------------------------------------------------------------
// Self-contained description of one object, enough to rebuild it on any page.
// While the source object still exists, pasted text reuses its exact font.
struct ClipObject {
    int type = 0;
    FS_MATRIX matrix{ 1, 0, 0, 1, 0, 0 };
    PRect bounds;                        // page space at capture time
    int groupId = 0;
    // source identity (for exact font reuse)
    FPDF_PAGEOBJECT srcObj = nullptr;
    int srcPage = -1;
    uint32_t srcGen = 0;
    // text
    std::wstring text;
    double fontSize = 12;                // raw Tf
    double visualSize = 12;              // Tf x matrix scale
    std::string fontBase;
    Bytes fontData;                      // only for genuinely embedded fonts
    int textRender = 0;
    // path
    struct Seg { int type = 0; float x = 0, y = 0; bool close = false; };
    std::vector<Seg> segs;
    int fillMode = 0;
    bool stroke = false;
    std::vector<float> dash;
    float dashPhase = 0;
    int lineCap = -1, lineJoin = -1;
    // colours (fill for text/path, stroke for path)
    unsigned fr = 0, fg = 0, fb = 0, fa = 255, sr = 0, sg = 0, sb = 0, sa = 255;
    float strokeW = 1;
    // image
    Image image;                         // full resolution pixels
    Bytes jpeg;                          // original JPEG bytes (copied losslessly)
};

bool CaptureObject(Document& d, int page, FPDF_PAGEOBJECT obj, ClipObject& out);
FPDF_PAGEOBJECT PasteObject(Document& d, int page, const ClipObject& c, float dxPage, float dyPage,
                            int newGroupId, std::string& err);
PRect ClipBounds(const std::vector<ClipObject>& clip);

// ---- redaction ---------------------------------------------------------------
// True redaction: covered characters are removed from text (the rest of each
// line is rebuilt in place, same font and position), objects inside the box
// are deleted, pictures under the box get their pixels blacked out, and
// annotations / form fields touching the box are removed. Then the box is
// painted. Verified: removed text and replaced pixels are not left in the file.
struct RedactResult { int removed = 0, textSplit = 0, imagesMasked = 0, annotsRemoved = 0; };
RedactResult Redact(Document& d, int page, PRect box, unsigned r, unsigned g, unsigned b);

// ---- annotations ---------------------------------------------------------------

struct AnnotInfo { int index = -1; int subtype = 0; PRect rect; };
std::optional<AnnotInfo> AnnotAt(Document& d, int page, float px, float py, float tolPt, bool includeWidgets);
bool AnnotRect(Document& d, int page, int index, PRect& out);
// Moves an annotation (rect + its quad points / ink strokes).
bool MoveAnnot(Document& d, int page, int index, float dx, float dy);
bool DeleteAnnot(Document& d, int page, int index);
// Turns this page's annotations and form fields into normal page content.
bool FlattenPage(Document& d, int page);

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
bool ImportPagesFromFile(Document& d, const std::wstring& file, const std::string& pw,
                         const std::string& range, int at, std::string& err);
bool ExportPagesToImages(Document& d, const std::vector<int>& pages, const std::wstring& dir,
                         float dpi, std::string& err);

// ---- forms ---------------------------------------------------------------------

struct FieldInfo {
    int page;
    std::wstring name;
    std::wstring value;
    int type;        // FPDF_FORMFIELD_*
    PRect box;
};
std::vector<FieldInfo> ListFormFields(Document& d);
bool FlattenAll(Document& d);

// ---- metadata --------------------------------------------------------------

std::wstring GetMeta(Document& d, const char* key);

} // namespace ops
