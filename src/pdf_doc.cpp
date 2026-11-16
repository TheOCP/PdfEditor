#include "pdf_doc.h"
#include <cmath>
#include <ctime>
#include <numeric>

// ------------------------------------------------------------------ matrices

FS_MATRIX MatMul(const FS_MATRIX& m1, const FS_MATRIX& m2) {
    FS_MATRIX r;
    r.a = m1.a * m2.a + m1.b * m2.c;
    r.b = m1.a * m2.b + m1.b * m2.d;
    r.c = m1.c * m2.a + m1.d * m2.c;
    r.d = m1.c * m2.b + m1.d * m2.d;
    r.e = m1.e * m2.a + m1.f * m2.c + m2.e;
    r.f = m1.e * m2.b + m1.f * m2.d + m2.f;
    return r;
}

FS_MATRIX MatTranslate(float x, float y) { return { 1, 0, 0, 1, x, y }; }

FS_MATRIX MatScaleAbout(float sx, float sy, float ox, float oy) {
    return { sx, 0, 0, sy, ox - sx * ox, oy - sy * oy };
}

FS_MATRIX MatRotateAbout(float degrees, float ox, float oy) {
    double r = degrees * 3.14159265358979323846 / 180.0;
    float c = (float)std::cos(r), s = (float)std::sin(r);
    FS_MATRIX rot{ c, s, -s, c, 0, 0 };
    return MatMul(MatMul(MatTranslate(-ox, -oy), rot), MatTranslate(ox, oy));
}

bool MatInvert(const FS_MATRIX& m, FS_MATRIX& o) {
    double det = (double)m.a * m.d - (double)m.b * m.c;
    if (std::fabs(det) < 1e-12) return false;
    o.a = (float)(m.d / det);
    o.b = (float)(-m.b / det);
    o.c = (float)(-m.c / det);
    o.d = (float)(m.a / det);
    o.e = (float)((m.c * (double)m.f - m.d * (double)m.e) / det);
    o.f = (float)((m.b * (double)m.e - m.a * (double)m.f) / det);
    return true;
}

void MatApply(const FS_MATRIX& m, float x, float y, float& ox, float& oy) {
    ox = m.a * x + m.c * y + m.e;
    oy = m.b * x + m.d * y + m.f;
}

// ------------------------------------------------------------------ library

void PdfInit() {
    FPDF_LIBRARY_CONFIG cfg{};
    cfg.version = 2;
    FPDF_InitLibraryWithConfig(&cfg);
}

void PdfShutdown() { FPDF_DestroyLibrary(); }

std::string PdfErrorString(unsigned long code) {
    switch (code) {
    case FPDF_ERR_SUCCESS:  return "no error";
    case FPDF_ERR_FILE:     return "file not found or could not be opened";
    case FPDF_ERR_FORMAT:   return "not a PDF or the file is corrupted";
    case FPDF_ERR_PASSWORD: return "password required or incorrect";
    case FPDF_ERR_SECURITY: return "unsupported security scheme";
    case FPDF_ERR_PAGE:     return "page not found or content error";
    default:                return "unknown error";
    }
}

// ------------------------------------------------------------------ form callbacks

static Document* Owner(FPDF_FORMFILLINFO* p) { return static_cast<FormInfo*>(p)->owner; }

static void CB_Invalidate(FPDF_FORMFILLINFO* p, FPDF_PAGE page, double, double, double, double) {
    Document* d = Owner(p);
    int i = d->PageIndexOf(page);
    if (i >= 0) d->VisualChanged(i);
}
static void CB_OutputSelectedRect(FPDF_FORMFILLINFO* p, FPDF_PAGE page, double, double, double, double) {
    CB_Invalidate(p, page, 0, 0, 0, 0);
}
static void CB_SetCursor(FPDF_FORMFILLINFO* p, int type) { Owner(p)->formCursor = type; }
static int CB_SetTimer(FPDF_FORMFILLINFO*, int, TimerCallback) { return 0; }
static void CB_KillTimer(FPDF_FORMFILLINFO*, int) {}
static FPDF_SYSTEMTIME CB_GetLocalTime(FPDF_FORMFILLINFO*) {
    FPDF_SYSTEMTIME r;
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    r.wYear = st.wYear; r.wMonth = st.wMonth; r.wDayOfWeek = st.wDayOfWeek; r.wDay = st.wDay;
    r.wHour = st.wHour; r.wMinute = st.wMinute; r.wSecond = st.wSecond; r.wMilliseconds = st.wMilliseconds;
#else
    time_t now = time(nullptr);
    tm lt{};
    localtime_r(&now, &lt);
    r.wYear = (unsigned short)(lt.tm_year + 1900); r.wMonth = (unsigned short)(lt.tm_mon + 1);
    r.wDayOfWeek = (unsigned short)lt.tm_wday; r.wDay = (unsigned short)lt.tm_mday;
    r.wHour = (unsigned short)lt.tm_hour; r.wMinute = (unsigned short)lt.tm_min;
    r.wSecond = (unsigned short)lt.tm_sec; r.wMilliseconds = 0;
#endif
    return r;
}
static void CB_OnChange(FPDF_FORMFILLINFO* p) {
    Document* d = Owner(p);
    d->formDirty = true;
    d->modified = true;
}
static FPDF_PAGE CB_GetPage(FPDF_FORMFILLINFO* p, FPDF_DOCUMENT, int index) { return Owner(p)->Page(index); }
static FPDF_PAGE CB_GetCurrentPage(FPDF_FORMFILLINFO* p, FPDF_DOCUMENT) {
    Document* d = Owner(p);
    return d->Page(d->currentPageHint);
}
static int CB_GetRotation(FPDF_FORMFILLINFO*, FPDF_PAGE) { return 0; }
static void CB_ExecuteNamedAction(FPDF_FORMFILLINFO*, FPDF_BYTESTRING) {}
static void CB_SetTextFieldFocus(FPDF_FORMFILLINFO*, FPDF_WIDESTRING, FPDF_DWORD, FPDF_BOOL) {}
static void CB_DoURIAction(FPDF_FORMFILLINFO*, FPDF_BYTESTRING) {}
static void CB_DoGoToAction(FPDF_FORMFILLINFO*, int, int, float*, int) {}

// ------------------------------------------------------------------ Document

Document::Document() {}

Document::~Document() { CloseDocument(); }

void Document::InitForm() {
    formInfo_ = FormInfo{};
    formInfo_.version = 1;
    formInfo_.owner = this;
    formInfo_.FFI_Invalidate = CB_Invalidate;
    formInfo_.FFI_OutputSelectedRect = CB_OutputSelectedRect;
    formInfo_.FFI_SetCursor = CB_SetCursor;
    formInfo_.FFI_SetTimer = CB_SetTimer;
    formInfo_.FFI_KillTimer = CB_KillTimer;
    formInfo_.FFI_GetLocalTime = CB_GetLocalTime;
    formInfo_.FFI_OnChange = CB_OnChange;
    formInfo_.FFI_GetPage = CB_GetPage;
    formInfo_.FFI_GetCurrentPage = CB_GetCurrentPage;
    formInfo_.FFI_GetRotation = CB_GetRotation;
    formInfo_.FFI_ExecuteNamedAction = CB_ExecuteNamedAction;
    formInfo_.FFI_SetTextFieldFocus = CB_SetTextFieldFocus;
    formInfo_.FFI_DoURIAction = CB_DoURIAction;
    formInfo_.FFI_DoGoToAction = CB_DoGoToAction;
    form_ = FPDFDOC_InitFormFillEnvironment(doc_, &formInfo_);
    if (form_) {
        FPDF_SetFormFieldHighlightColor(form_, FPDF_FORMFIELD_UNKNOWN, 0xFFDDE4FF);
        FPDF_SetFormFieldHighlightAlpha(form_, 90);
    }
    formDirty = false;
}

void Document::RebuildSlots() {
    int n = doc_ ? FPDF_GetPageCount(doc_) : 0;
    slots_.assign(std::max(0, n), Slot{});
    sizes_.assign(std::max(0, n), FS_SIZEF{ 612, 792 });
    for (int i = 0; i < n; ++i) {
        FS_SIZEF s{};
        if (FPDF_GetPageSizeByIndexF(doc_, i, &s) && s.width > 0 && s.height > 0) sizes_[i] = s;
        slots_[i].version = (uint32_t)(generation_ * 1000 + 1);
    }
}

void Document::CloseAllPages() {
    for (auto& s : slots_) {
        if (s.text) { FPDFText_ClosePage(s.text); s.text = nullptr; }
        if (s.page) {
            if (form_) FORM_OnBeforeClosePage(s.page, form_);
            FPDF_ClosePage(s.page);
            s.page = nullptr;
        }
        s.contentDirty = false;
    }
}

void Document::CloseDocument() {
    if (!doc_) return;
    CloseAllPages();
    for (auto& [k, f] : fonts_) FPDFFont_Close(f);
    fonts_.clear();
    if (form_) { FPDFDOC_ExitFormFillEnvironment(form_); form_ = nullptr; }
    FPDF_CloseDocument(doc_);
    doc_ = nullptr;
    loadedFrom_.reset();
    slots_.clear();
    sizes_.clear();
}

Document::Load Document::OpenFromBytes(BytesPtr bytes, const std::string& pw, std::string& err) {
    if (!bytes || bytes->empty()) { err = "file is empty"; return Load::Error; }
    FPDF_DOCUMENT d = FPDF_LoadMemDocument64(bytes->data(), bytes->size(), pw.empty() ? nullptr : pw.c_str());
    if (!d) {
        unsigned long e = FPDF_GetLastError();
        if (e == FPDF_ERR_PASSWORD) return Load::NeedPassword;
        err = PdfErrorString(e);
        return Load::Error;
    }
    CloseDocument();
    doc_ = d;
    loadedFrom_ = bytes;
    password = pw;
    ++generation_;
    ++docId_;
    ++edits_;
    InitForm();
    RebuildSlots();
    return Load::Ok;
}

Document::Load Document::LoadBytes(BytesPtr bytes, const std::string& pw, std::string& err) {
    Load r = OpenFromBytes(bytes, pw, err);
    if (r != Load::Ok) return r;
    current_ = { bytes, "Open" };
    undo_.clear();
    redo_.clear();
    modified = false;
    return r;
}

Document::Load Document::LoadFile(const std::wstring& file, const std::string& pw, std::string& err) {
    auto bytes = std::make_shared<Bytes>();
    if (!ReadFileBytes(file, *bytes)) { err = "could not read the file"; return Load::Error; }
    Load r = LoadBytes(bytes, pw, err);
    if (r == Load::Ok) {
        path = file;
        displayName = FileNameOf(file);
    }
    return r;
}

bool Document::CreateBlank(float w, float h) {
    CloseDocument();
    doc_ = FPDF_CreateNewDocument();
    if (!doc_) return false;
    FPDF_PAGE p = FPDFPage_New(doc_, 0, w, h);
    if (p) FPDF_ClosePage(p);
    ++generation_;
    ++docId_;
    InitForm();
    RebuildSlots();
    auto b = std::make_shared<Bytes>();
    SaveBytes(*b, FPDF_NO_INCREMENTAL);
    current_ = { b, "New document" };
    undo_.clear();
    redo_.clear();
    path.clear();
    displayName = L"Untitled.pdf";
    modified = false;
    return true;
}

struct MemWriter : FPDF_FILEWRITE {
    Bytes* out = nullptr;
};
static int WriteBlockCb(FPDF_FILEWRITE* self, const void* data, unsigned long size) {
    auto* w = static_cast<MemWriter*>(self);
    auto* p = static_cast<const uint8_t*>(data);
    w->out->insert(w->out->end(), p, p + size);
    return 1;
}

bool Document::SaveBytes(Bytes& out, unsigned long flags) {
    if (!doc_) return false;
    Bake();
    out.clear();
    MemWriter w;
    w.version = 1;
    w.WriteBlock = WriteBlockCb;
    w.out = &out;
    return FPDF_SaveAsCopy(doc_, &w, flags) != 0;
}

bool Document::SaveFile(const std::wstring& file, bool removeSecurity, bool incremental, std::string& err) {
    unsigned long flags = (incremental && !removeSecurity) ? FPDF_INCREMENTAL : FPDF_NO_INCREMENTAL;
    if (removeSecurity) flags |= FPDF_REMOVE_SECURITY;
    Bytes b;
    if (!SaveBytes(b, flags)) { err = "PDFium could not serialize the document"; return false; }
    if (!WriteFileBytes(file, b.data(), b.size())) {
        err = "could not write the file (is it open in another program?)";
        return false;
    }
    path = file;
    displayName = FileNameOf(file);
    modified = false;
    return true;
}

FS_SIZEF Document::PageSizePt(int i) const {
    if (i < 0 || i >= (int)sizes_.size()) return { 612, 792 };
    return sizes_[i];
}

FPDF_PAGE Document::Page(int i) {
    if (!doc_ || i < 0 || i >= PageCount()) return nullptr;
    Slot& s = slots_[i];
    if (!s.page) {
        s.page = FPDF_LoadPage(doc_, i);
        if (s.page && form_) FORM_OnAfterLoadPage(s.page, form_);
    }
    return slots_[i].page;
}

FPDF_TEXTPAGE Document::TextPage(int i) {
    FPDF_PAGE p = Page(i);
    if (!p) return nullptr;
    Slot& s = slots_[i];
    if (!s.text || s.textVersion != s.version) {
        if (s.text) FPDFText_ClosePage(s.text);
        s.text = FPDFText_LoadPage(p);
        s.textVersion = s.version;
    }
    return s.text;
}

int Document::PageIndexOf(FPDF_PAGE p) const {
    for (int i = 0; i < (int)slots_.size(); ++i)
        if (slots_[i].page == p) return i;
    return -1;
}

int Document::Rotation(int i) {
    FPDF_PAGE p = Page(i);
    if (!p) return 0;
    int r = FPDFPage_GetRotation(p);
    return (r >= 0 && r <= 3) ? r : 0;
}

PRect Document::PageBox(int i) {
    FPDF_PAGE p = Page(i);
    FS_RECTF r{};
    if (!p || !FPDF_GetPageBoundingBox(p, &r)) return { 0, 0, 612, 792 };
    return PRect::FromPoints(r.left, r.bottom, r.right, r.top);
}

void Document::ContentChanged(int i) {
    if (i < 0 || i >= PageCount()) return;
    slots_[i].contentDirty = true;
    ++slots_[i].version;
    ++edits_;
    modified = true;
}

void Document::VisualChanged(int i) {
    if (i < 0 || i >= PageCount()) return;
    ++slots_[i].version;
    ++edits_;
}

void Document::Bake() {
    KillFormFocus();
    for (auto& s : slots_) {
        if (s.contentDirty && s.page) FPDFPage_GenerateContent(s.page);
        s.contentDirty = false;
    }
}

void Document::KillFormFocus() {
    if (form_) FORM_ForceToKillFocus(form_);
}

void Document::StructureChanged() {
    Bake();
    CloseAllPages();
    ++generation_;
    RebuildSlots();
    ++edits_;
    modified = true;
}

void Document::TrimUndo() {
    const size_t kMaxStates = 100;
    const size_t kMaxBytes = (size_t)1536 << 20;
    auto total = [&] {
        size_t t = current_.bytes ? current_.bytes->size() : 0;
        for (auto& s : undo_) t += s.bytes ? s.bytes->size() : 0;
        for (auto& s : redo_) t += s.bytes ? s.bytes->size() : 0;
        return t;
    };
    while (!undo_.empty() && (undo_.size() > kMaxStates || total() > kMaxBytes))
        undo_.erase(undo_.begin());
}

void Document::Commit(const std::string& label) {
    if (!doc_) return;
    auto b = std::make_shared<Bytes>();
    if (!SaveBytes(*b, FPDF_NO_INCREMENTAL)) return;
    undo_.push_back(current_);
    current_ = { b, label };
    redo_.clear();
    TrimUndo();
    modified = true;
    formDirty = false;
    ++edits_;
}

bool Document::Undo() {
    if (undo_.empty()) return false;
    KillFormFocus();
    State target = undo_.back();
    std::string err;
    if (OpenFromBytes(target.bytes, password, err) != Load::Ok) return false;
    undo_.pop_back();
    redo_.push_back(current_);
    current_ = target;
    modified = true;
    return true;
}

bool Document::Redo() {
    if (redo_.empty()) return false;
    KillFormFocus();
    State target = redo_.back();
    std::string err;
    if (OpenFromBytes(target.bytes, password, err) != Load::Ok) return false;
    redo_.pop_back();
    undo_.push_back(current_);
    current_ = target;
    modified = true;
    return true;
}

bool Document::Render(int i, float scale, int rx, int ry, int rw, int rh, std::vector<uint32_t>& out,
                      bool annots, bool forms) {
    FPDF_PAGE p = Page(i);
    if (!p || rw <= 0 || rh <= 0) return false;
    FS_SIZEF sz = PageSizePt(i);
    int fw = std::max(1, (int)std::lround(sz.width * scale));
    int fh = std::max(1, (int)std::lround(sz.height * scale));
    out.assign((size_t)rw * rh, 0xFFFFFFFFu);
    FPDF_BITMAP bmp = FPDFBitmap_CreateEx(rw, rh, FPDFBitmap_BGRA, out.data(), rw * 4);
    if (!bmp) return false;
    int flags = annots ? FPDF_ANNOT : 0;
    FPDF_RenderPageBitmap(bmp, p, -rx, -ry, fw, fh, 0, flags);
    if (forms && form_) FPDF_FFLDraw(form_, bmp, p, -rx, -ry, fw, fh, 0, flags);
    FPDFBitmap_Destroy(bmp);
    for (auto& px : out) px |= 0xFF000000u;
    return true;
}

void Document::PageToDev(int i, float s, float px, float py, float& dx, float& dy) {
    PRect b = PageBox(i);
    switch (Rotation(i)) {
    default:
    case 0: dx = (px - b.l) * s; dy = (b.t - py) * s; break;
    case 1: dx = (py - b.b) * s; dy = (px - b.l) * s; break;
    case 2: dx = (b.r - px) * s; dy = (py - b.b) * s; break;
    case 3: dx = (b.t - py) * s; dy = (b.r - px) * s; break;
    }
}

void Document::DevToPage(int i, float s, float dx, float dy, float& px, float& py) {
    PRect b = PageBox(i);
    switch (Rotation(i)) {
    default:
    case 0: px = dx / s + b.l; py = b.t - dy / s; break;
    case 1: py = dx / s + b.b; px = dy / s + b.l; break;
    case 2: px = b.r - dx / s; py = dy / s + b.b; break;
    case 3: py = b.t - dx / s; px = b.r - dy / s; break;
    }
}

FPDF_FONT Document::GetFont(const FontChoice& fc, std::string& err) {
    if (!doc_) return nullptr;
    // Fonts stored in the PDF are used through a live handle and never cached:
    // the handle belongs to the page objects that use it.
    if (fc.handle) return fc.handle;
    if (fc.IsDoc()) { err = "that font is no longer used anywhere in this PDF"; return nullptr; }
    std::string key = fc.Key();
    auto it = fonts_.find(key);
    if (it != fonts_.end()) return it->second;
    FPDF_FONT f = nullptr;
    if (fc.IsStandard()) {
        f = FPDFText_LoadStandardFont(doc_, fc.stdName.c_str());
    } else {
        auto dit = fontData_.find(fc.file);
        if (dit == fontData_.end()) {
            Bytes data;
            if (!ReadFileBytes(fc.file, data)) { err = "could not read font file"; return nullptr; }
            dit = fontData_.emplace(fc.file, std::move(data)).first;
        }
        f = FPDFText_LoadFont(doc_, dit->second.data(), (uint32_t)dit->second.size(), FPDF_FONT_TRUETYPE, 1);
    }
    if (!f) { err = "PDFium could not load this font"; return nullptr; }
    fonts_[key] = f;
    return f;
}
