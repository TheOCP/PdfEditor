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
    int index = -1;
    int type = 0;            // FPDF_PAGEOBJ_*
    PRect bounds;            // page space (points), y up
    FS_MATRIX matrix{ 1, 0, 0, 1, 0, 0 };
};

// Top-level objects on a page, in draw order (last = on top).
std::vector<ObjInfo> ListObjects(Document& d, int page);

// Topmost object whose bounds contain (px,py); nullptr if none.
std::optional<ObjInfo> HitTest(Document& d, int page, float px, float py);

int ObjectType(FPDF_PAGEOBJECT obj);
PRect ObjectBounds(FPDF_PAGEOBJECT obj);

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

// ---- images ----------------------------------------------------------------

// Places `img` with its lower-left at (px,py), drawn `widthPt` wide (height
// follows the aspect ratio). Returns the new image object or nullptr.
FPDF_PAGEOBJECT AddImage(Document& d, int page, const Image& img, float px, float py, float widthPt,
                         std::string& err);

// ---- shapes / redaction ----------------------------------------------------

FPDF_PAGEOBJECT AddRect(Document& d, int page, PRect box, bool fill, unsigned fr, unsigned fg, unsigned fb,
                        bool stroke, unsigned sr, unsigned sg, unsigned sb, float strokeW);

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
