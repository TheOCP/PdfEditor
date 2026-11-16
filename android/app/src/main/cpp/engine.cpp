// JNI bridge: the Android app drives the same editing engine as the desktop
// version (pdf_doc / pdf_ops - exact glyphs, letter positions, stand-ins for
// missing characters, ...). This file holds the document and the selection and
// mirrors the desktop app's behaviour; results go back as small JSON strings.
// PDFium is not thread-safe: every entry point takes the same lock.
#include <jni.h>
#include <android/bitmap.h>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <fpdf_signature.h>
#include <unistd.h>
#include "pdf_doc.h"
#include "pdf_ops.h"

namespace {

std::recursive_mutex g_lock;
#define LOCK std::lock_guard<std::recursive_mutex> guard(g_lock)

struct Sel {
    int page = -1;
    FPDF_PAGEOBJECT obj = nullptr;
    FPDF_PAGEOBJECT parent = nullptr;
    FS_MATRIX accum{ 1, 0, 0, 1, 0, 0 };
    uint32_t gen = 0;
    std::vector<FPDF_PAGEOBJECT> group;   // every selected object (>= 1 when valid)
    int groupId = 0;
    int annPage = -1, annIdx = -1;        // a selected annotation instead
};

struct Engine {
    Document doc;
    Sel sel;
    std::vector<ops::ClipObject> clip;
    std::vector<FPDF_PAGEOBJECT> limbo;   // cut objects kept for an exact paste
    uint32_t limboDoc = 0;
    int clipSrcPage = -1, pasteCount = 0;
    std::string lastError;
    std::vector<FontChoice> fonts;        // font list for "change font" / new text
    uint32_t fontsDoc = 0;
    int docFontCount = 0;
    // alignment while text is dragged
    std::vector<ops::AlignRef> alignRefs;
    PRect alignStart;
    bool alignHasOrigin = false;
    float alignOx = 0, alignOy = 0;
    // "always show guide lines": the other objects' edges, per selection
    std::vector<ops::AlignRef> guideRefs;
    int guidePage = -1;
    uint32_t guideGen = 0, guideDoc = 0;
    FPDF_PAGEOBJECT guideObj = nullptr;
    size_t guideCount = 0;
};

Engine& E() {
    static Engine* e = new Engine();
    return *e;
}

// ----------------------------------------------------------------- JSON

struct Json {
    std::string s;
    bool first = true;
    static std::string Esc(const std::string& v) {
        std::string o = "\"";
        for (unsigned char c : v) {
            if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
            else if (c == '\n') o += "\\n";
            else if (c == '\r') o += "\\r";
            else if (c == '\t') o += "\\t";
            else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
            else o += (char)c;
        }
        return o + "\"";
    }
    Json& Begin() { s += "{"; first = true; return *this; }
    Json& End() { s += "}"; first = false; return *this; }
    Json& Key(const char* k) { if (!first) s += ","; first = false; s += Esc(k) + ":"; return *this; }
    Json& Str(const char* k, const std::string& v) { Key(k); s += Esc(v); return *this; }
    Json& Num(const char* k, double v) { Key(k); char b[32]; snprintf(b, sizeof(b), "%.4f", v); s += b; return *this; }
    Json& Int(const char* k, long long v) { Key(k); s += std::to_string(v); return *this; }
    Json& Bool(const char* k, bool v) { Key(k); s += v ? "true" : "false"; return *this; }
    Json& Rect(const char* k, const PRect& r) {
        Key(k);
        char b[160];
        snprintf(b, sizeof(b), "[%.3f,%.3f,%.3f,%.3f]", r.l, r.b, r.r, r.t);
        s += b;
        return *this;
    }
    Json& Raw(const char* k, const std::string& raw) { Key(k); s += raw; return *this; }
};

jstring J(JNIEnv* env, const std::string& s) { return env->NewStringUTF(s.c_str()); }
std::string S(JNIEnv* env, jstring js) {
    if (!js) return {};
    const char* c = env->GetStringUTFChars(js, nullptr);
    std::string s = c ? c : "";
    if (c) env->ReleaseStringUTFChars(js, c);
    return s;
}
// Java strings arrive as modified UTF-8; go through UTF-16 for anything typed.
std::wstring W(JNIEnv* env, jstring js) {
    if (!js) return {};
    const jchar* c = env->GetStringChars(js, nullptr);
    jsize n = env->GetStringLength(js);
    std::wstring w = FromUtf16(reinterpret_cast<const unsigned short*>(c), (size_t)n);
    env->ReleaseStringChars(js, c);
    return w;
}
jstring JW(JNIEnv* env, const std::wstring& w) {
    auto u16 = WS(w);
    const unsigned short* p = u16;
    size_t n = 0;
    while (p[n]) ++n;
    return env->NewString(reinterpret_cast<const jchar*>(p), (jsize)n);
}
Bytes BytesOf(JNIEnv* env, jbyteArray a) {
    Bytes b;
    if (!a) return b;
    jsize n = env->GetArrayLength(a);
    b.resize((size_t)n);
    env->GetByteArrayRegion(a, 0, n, reinterpret_cast<jbyte*>(b.data()));
    return b;
}

// ------------------------------------------------------------- selection

bool SelValid() {
    Engine& e = E();
    return e.sel.obj && e.sel.page >= 0 && e.sel.gen == e.doc.Generation();
}
bool AnnotSel() { return E().sel.annPage >= 0 && E().sel.annIdx >= 0; }

void ClearSel() { E().sel = Sel{}; }

void SelectOne(int page, FPDF_PAGEOBJECT o) {
    Sel s;
    s.page = page; s.obj = o; s.gen = E().doc.Generation();
    s.group = { o };
    E().sel = s;
}

void SelectMany(int page, const std::vector<FPDF_PAGEOBJECT>& objs, int gid) {
    ClearSel();
    if (objs.empty()) return;
    SelectOne(page, objs[0]);
    E().sel.group = objs;
    E().sel.groupId = gid;
}

void SelectInfo(int page, const ops::ObjInfo& o) {
    Sel s;
    s.page = page; s.obj = o.obj; s.parent = o.parent; s.accum = o.accum; s.gen = E().doc.Generation();
    s.group = { o.obj };
    E().sel = s;
}

void RefreshGroup() {
    Sel& s = E().sel;
    s.group.clear();
    s.groupId = 0;
    if (!SelValid()) return;
    if (!s.parent) {
        int gid = ops::GetGroupId(s.obj);
        if (gid) { s.groupId = gid; s.group = ops::GroupMembers(E().doc, s.page, gid); }
    }
    if (s.group.empty()) s.group.push_back(s.obj);
}

bool EnsureEditable() {
    Sel& s = E().sel;
    if (!SelValid()) return false;
    if (!s.parent) return true;
    if (!ops::PromoteObject(E().doc, s.page, s.obj)) return false;
    s.parent = nullptr;
    s.accum = FS_MATRIX{ 1, 0, 0, 1, 0, 0 };
    if (s.group.size() <= 1) s.group = { s.obj };
    return SelValid();
}

PRect SelBounds() {
    Sel& s = E().sel;
    if (AnnotSel()) {
        PRect r;
        ops::AnnotRect(E().doc, s.annPage, s.annIdx, r);
        return r;
    }
    if (!SelValid()) return {};
    if (s.group.size() <= 1) return ops::ObjectPageBounds(s.obj, s.accum);
    PRect u = ops::ObjectBounds(s.group[0]);
    for (size_t i = 1; i < s.group.size(); ++i) u = u.Union(ops::ObjectBounds(s.group[i]));
    return u;
}

std::vector<FPDF_PAGEOBJECT> Members() {
    Sel& s = E().sel;
    if (!s.group.empty()) return s.group;
    if (s.obj) return { s.obj };
    return {};
}

void Commit(const char* label) { E().doc.Commit(label); }

std::string SelectionJson() {
    Engine& e = E();
    Sel& s = e.sel;
    Json j;
    j.Begin();
    if (AnnotSel()) {
        j.Bool("has", true).Bool("annot", true).Int("page", s.annPage).Int("count", 1).Rect("bounds", SelBounds());
        return j.End().s;
    }
    if (!SelValid()) return j.Bool("has", false).End().s;
    int t = ops::ObjectType(s.obj);
    j.Bool("has", true).Bool("annot", false).Int("page", s.page).Int("count", (int)s.group.size())
        .Int("group", s.groupId).Int("type", t).Rect("bounds", SelBounds()).Bool("nested", s.parent != nullptr);
    // Text that can be typed into: one text object, or the pieces one edit made of it.
    bool textEdit = t == FPDF_PAGEOBJ_TEXT &&
        (s.group.size() <= 1 || ops::TextPieces(e.doc, s.page, s.obj).size() == s.group.size());
    j.Bool("textEdit", textEdit);
    if (s.group.size() <= 1 && t == FPDF_PAGEOBJ_TEXT) {
        ops::TextProps tp = ops::ReadText(e.doc, s.page, s.obj);
        j.Str("font", StripSubsetPrefix(tp.fontName)).Bool("embedded", tp.embedded)
            .Num("size", tp.fontSize * ops::MatrixScale(s.accum))
            .Int("r", tp.r).Int("g", tp.g).Int("b", tp.b);
    }
    if (s.group.size() <= 1 && ops::IsLine(s.obj)) {
        unsigned r = 0, g = 0, b = 0;
        bool col = ops::LineColor(s.obj, r, g, b);
        j.Bool("line", true).Num("thickness", ops::LineThickness(s.obj) * ops::MatrixScale(s.accum))
            .Bool("lineColor", col).Int("r", r).Int("g", g).Int("b", b);
    }
    if (s.group.size() <= 1 && t == FPDF_PAGEOBJ_PATH) j.Int("parts", ops::SplittableParts(s.obj));
    if (s.group.size() > 1) {
        int lines = 0, texts = 0;
        for (auto o : s.group) { lines += ops::IsLine(o) ? 1 : 0; texts += ops::ObjectType(o) == FPDF_PAGEOBJ_TEXT ? 1 : 0; }
        j.Int("lines", lines).Int("texts", texts);
    }
    return j.End().s;
}

std::string EditInfoJson(bool ok, const ops::TextEditInfo& ti) {
    Json j;
    j.Begin().Bool("ok", ok).Str("missing", W2U(ti.missing)).Str("font", ti.fontName).Str("standIn", ti.standIn)
        .Int("pieces", (int)ti.pieces.size());
    return j.End().s;
}

// ------------------------------------------------------------- font list

void BuildFonts() {
    Engine& e = E();
    if (!e.fonts.empty() && e.fontsDoc == e.doc.DocId()) return;
    e.fonts.clear();
    e.docFontCount = 0;
    if (e.doc.Raw()) {
        for (const auto& df : ops::ListDocFonts(e.doc)) {
            std::string nm = StripSubsetPrefix(df.name);
            bool dup = nm.empty();
            for (auto& f : e.fonts) if (StripSubsetPrefix(f.docName) == nm) dup = true;
            if (dup) continue;
            FontChoice fc;
            fc.docName = df.name;
            fc.label = nm;
            fc.stdName = df.embedded ? "doc" : "docref";
            e.fonts.push_back(fc);
        }
        e.docFontCount = (int)e.fonts.size();
    }
    for (auto f : StandardFonts()) { f.label = f.stdName; e.fonts.push_back(f); }
    for (const auto& f : SystemFonts()) e.fonts.push_back(ChoiceFor(f));
    e.fontsDoc = e.doc.DocId();
}

FontChoice ResolveFont(int idx, int page) {
    Engine& e = E();
    BuildFonts();
    if (idx < 0 || idx >= (int)e.fonts.size()) idx = e.docFontCount > 0 ? 0 : std::min<int>(e.docFontCount, (int)e.fonts.size() - 1);
    FontChoice fc = e.fonts.empty() ? FontChoice{} : e.fonts[idx];
    if (fc.IsDoc()) {
        fc.handle = ops::FindDocFont(e.doc, fc.docName, page);
        if (!fc.handle)
            if (const SysFont* sf = MatchSystemFont(fc.docName)) return ChoiceFor(*sf);
    }
    return fc;
}

void ClearLimbo() {
    Engine& e = E();
    if (!e.limbo.empty() && e.limboDoc == e.doc.DocId())
        for (auto o : e.limbo) FPDFPageObj_Destroy(o);
    e.limbo.clear();
}

void AfterLoad() {
    Engine& e = E();
    ClearSel();
    e.clip.clear();
    e.limbo.clear();
    e.pasteCount = 0;
    e.fonts.clear();
}

} // namespace

#define JFN(ret, name) extern "C" JNIEXPORT ret JNICALL Java_com_theocp_pdfeditor_Engine_##name

// ============================================================ document

JFN(void, nInit)(JNIEnv*, jobject) {
    LOCK;
    static bool done = false;
    if (!done) { PdfInit(); done = true; }
}

JFN(jint, nOpen)(JNIEnv* env, jobject, jbyteArray data, jstring pw, jstring name) {
    LOCK;
    Engine& e = E();
    ClearLimbo();
    auto bytes = std::make_shared<Bytes>(BytesOf(env, data));
    std::string err;
    auto r = e.doc.LoadBytes(bytes, S(env, pw), err);
    if (r == Document::Load::Ok) {
        e.doc.displayName = U2W(S(env, name));
        AfterLoad();
        return 0;
    }
    e.lastError = err;
    return r == Document::Load::NeedPassword ? 1 : 2;
}

JFN(jboolean, nNew)(JNIEnv*, jobject) {
    LOCK;
    ClearLimbo();
    bool ok = E().doc.CreateBlank(595, 842);   // A4
    AfterLoad();
    return ok;
}

JFN(void, nClose)(JNIEnv*, jobject) {
    LOCK;
    ClearLimbo();
    AfterLoad();
    E().doc.Close();
}

JFN(jstring, nLastError)(JNIEnv* env, jobject) { LOCK; return J(env, E().lastError); }

JFN(jbyteArray, nSave)(JNIEnv* env, jobject) {
    LOCK;
    Engine& e = E();
    if (!e.doc.Raw()) return nullptr;
    e.doc.KillFormFocus();
    Bytes out;
    if (!e.doc.SaveBytes(out, FPDF_NO_INCREMENTAL)) return nullptr;
    jbyteArray a = env->NewByteArray((jsize)out.size());
    env->SetByteArrayRegion(a, 0, (jsize)out.size(), reinterpret_cast<const jbyte*>(out.data()));
    return a;
}

JFN(void, nMarkSaved)(JNIEnv*, jobject) { LOCK; E().doc.modified = false; }
JFN(jboolean, nModified)(JNIEnv*, jobject) { LOCK; return E().doc.modified; }
JFN(jboolean, nIsOpen)(JNIEnv*, jobject) { LOCK; return E().doc.Raw() != nullptr; }
JFN(jint, nPageCount)(JNIEnv*, jobject) { LOCK; return E().doc.Raw() ? E().doc.PageCount() : 0; }
JFN(jint, nDocId)(JNIEnv*, jobject) { LOCK; return (jint)E().doc.DocId(); }
JFN(jint, nPageVersion)(JNIEnv*, jobject, jint i) { LOCK; return (jint)E().doc.PageVersion(i); }
JFN(jboolean, nCanUndo)(JNIEnv*, jobject) { LOCK; return E().doc.CanUndo(); }
JFN(jboolean, nCanRedo)(JNIEnv*, jobject) { LOCK; return E().doc.CanRedo(); }

JFN(jfloatArray, nPageSize)(JNIEnv* env, jobject, jint i) {
    LOCK;
    FS_SIZEF sz = E().doc.Raw() ? E().doc.PageSizePt(i) : FS_SIZEF{ 0, 0 };
    float v[2] = { sz.width, sz.height };
    jfloatArray a = env->NewFloatArray(2);
    env->SetFloatArrayRegion(a, 0, 2, v);
    return a;
}

JFN(jboolean, nUndo)(JNIEnv*, jobject) {
    LOCK;
    ClearLimbo();
    bool ok = E().doc.Undo();
    if (ok) ClearSel();
    return ok;
}
JFN(jboolean, nRedo)(JNIEnv*, jobject) {
    LOCK;
    ClearLimbo();
    bool ok = E().doc.Redo();
    if (ok) ClearSel();
    return ok;
}

// Renders the device rect (x, y, w, h) of page i at `scale` px/pt into an
// ARGB_8888 bitmap of size w x h.
JFN(jboolean, nRender)(JNIEnv* env, jobject, jint i, jfloat scale, jint x, jint y, jint w, jint h, jobject bitmap) {
    LOCK;
    Engine& e = E();
    if (!e.doc.Raw() || i < 0 || i >= e.doc.PageCount()) return false;
    std::vector<uint32_t> px;
    if (!e.doc.Render(i, scale, x, y, w, h, px, true, true)) return false;
    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS) return false;
    void* dst = nullptr;
    if (AndroidBitmap_lockPixels(env, bitmap, &dst) != ANDROID_BITMAP_RESULT_SUCCESS) return false;
    int rows = std::min<int>(h, (int)info.height), cols = std::min<int>(w, (int)info.width);
    for (int r = 0; r < rows; ++r) {
        uint32_t* out = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(dst) + (size_t)r * info.stride);
        const uint32_t* in = px.data() + (size_t)r * w;
        for (int c = 0; c < cols; ++c) {
            uint32_t p = in[c];                                     // B G R A in memory
            out[c] = (p & 0xFF00FF00u) | ((p >> 16) & 0xFF) | ((p & 0xFF) << 16);   // -> R G B A
        }
    }
    AndroidBitmap_unlockPixels(env, bitmap);
    return true;
}

JFN(jfloatArray, nDevToPage)(JNIEnv* env, jobject, jint i, jfloat scale, jfloat dx, jfloat dy) {
    LOCK;
    float px = 0, py = 0;
    if (E().doc.Raw()) E().doc.DevToPage(i, scale, dx, dy, px, py);
    float v[2] = { px, py };
    jfloatArray a = env->NewFloatArray(2);
    env->SetFloatArrayRegion(a, 0, 2, v);
    return a;
}

JFN(jfloatArray, nPageToDev)(JNIEnv* env, jobject, jint i, jfloat scale, jfloat px, jfloat py) {
    LOCK;
    float dx = 0, dy = 0;
    if (E().doc.Raw()) E().doc.PageToDev(i, scale, px, py, dx, dy);
    float v[2] = { dx, dy };
    jfloatArray a = env->NewFloatArray(2);
    env->SetFloatArrayRegion(a, 0, 2, v);
    return a;
}

// ============================================================ selection

// A tap with the select tool: the object under the finger (its whole group
// unless `single`), else an annotation. Tapping inside the current group keeps it.
JFN(jstring, nTap)(JNIEnv* env, jobject, jint page, jfloat px, jfloat py, jfloat tol, jboolean single) {
    LOCK;
    Engine& e = E();
    if (!e.doc.Raw()) return J(env, "{\"has\":false}");
    bool insideGroup = !single && e.sel.group.size() > 1 && SelValid() && e.sel.page == page && SelBounds().Contains(px, py);
    if (!insideGroup) {
        auto hit = ops::HitTest(e.doc, page, px, py, tol);
        if (hit) {
            SelectInfo(page, *hit);
            if (!single) RefreshGroup();
        } else if (auto an = ops::AnnotAt(e.doc, page, px, py, tol, single)) {
            ClearSel();
            e.sel.annPage = page; e.sel.annIdx = an->index;
        } else {
            ClearSel();
        }
    }
    return J(env, SelectionJson());
}

JFN(jstring, nSelection)(JNIEnv* env, jobject) { LOCK; return J(env, SelectionJson()); }
JFN(void, nClearSelection)(JNIEnv*, jobject) { LOCK; ClearSel(); }

static int SelectLeaves(int pg, bool linesOnly, const PRect* within) {
    Engine& e = E();
    std::vector<FPDF_PAGEOBJECT> pick, nested;
    for (auto& o : ops::ListObjects(e.doc, pg)) {
        if (o.type == FPDF_PAGEOBJ_FORM) continue;
        if (linesOnly && !ops::IsLine(o.obj)) continue;      // real lines only, however drawn
        if (within && !o.bounds.Intersects(*within)) continue;
        pick.push_back(o.obj);
        if (o.parent) nested.push_back(o.obj);
    }
    if (pick.empty()) { ClearSel(); return 0; }
    if (!nested.empty()) ops::PromoteAll(e.doc, pg, nested);
    int common = -1;
    bool same = true;
    for (auto o : pick) {
        int gid = ops::GetGroupId(o);
        if (common == -1) common = gid; else if (common != gid) same = false;
    }
    SelectMany(pg, pick, (same && common > 0) ? common : 0);
    return (int)pick.size();
}

JFN(jint, nSelectRect)(JNIEnv*, jobject, jint page, jfloat l, jfloat b, jfloat r, jfloat t) {
    LOCK;
    PRect box = PRect::FromPoints(l, b, r, t);
    return SelectLeaves(page, false, &box);
}
JFN(jint, nSelectAll)(JNIEnv*, jobject, jint page, jboolean linesOnly) { LOCK; return SelectLeaves(page, linesOnly, nullptr); }

// Moves the selection by (dx, dy) page points. Call nCommit afterwards (end of drag).
JFN(void, nMoveSelection)(JNIEnv*, jobject, jfloat dx, jfloat dy) {
    LOCK;
    Engine& e = E();
    Sel& s = e.sel;
    if (AnnotSel()) { ops::MoveAnnot(e.doc, s.annPage, s.annIdx, dx, dy); return; }
    if (!SelValid()) return;
    if (s.group.size() <= 1) {
        if (s.parent) EnsureEditable();
        ops::TranslateObjectPage(e.doc, s.page, s.obj, s.accum, dx, dy);
    } else {
        FS_MATRIX id{ 1, 0, 0, 1, 0, 0 };
        for (auto o : s.group) ops::TranslateObjectPage(e.doc, s.page, o, id, dx, dy);
    }
}

// Lines keep their thickness and text its size; keepSpacing: only the outer
// row / column of a table changes.
JFN(void, nResizeSelection)(JNIEnv*, jobject, jfloat l, jfloat b, jfloat r, jfloat t, jboolean keepSpacing) {
    LOCK;
    Engine& e = E();
    Sel& s = e.sel;
    if (!SelValid()) return;
    PRect target = PRect::FromPoints(l, b, r, t);
    if (s.group.size() <= 1) {
        if (s.parent) EnsureEditable();
        ops::ResizeObjectPageTopLevel(e.doc, s.page, s.obj, target);
        return;
    }
    ops::ResizeGroup(e.doc, s.page, s.group, SelBounds(), target, keepSpacing);
}

// Start of a drag: remembers where the selection was and the other text on
// the page. Returns false when the selection has no text (no alignment).
JFN(jboolean, nAlignBegin)(JNIEnv*, jobject) {
    LOCK;
    Engine& e = E();
    Sel& s = e.sel;
    e.alignRefs.clear();
    e.alignHasOrigin = false;
    if (!SelValid()) return false;
    bool text = false;
    for (auto o : Members()) {
        if (ops::ObjectType(o) != FPDF_PAGEOBJ_TEXT) continue;
        text = true;
        FS_MATRIX acc = (o == s.obj) ? s.accum : FS_MATRIX{ 1, 0, 0, 1, 0, 0 };
        if (!e.alignHasOrigin && ops::TextOrigin(o, acc, e.alignOx, e.alignOy)) e.alignHasOrigin = true;
    }
    if (!text) return false;
    e.alignStart = SelBounds();
    e.alignRefs = ops::AlignRefs(e.doc, s.page, Members());
    return true;
}

// (dx, dy) = total move since nAlignBegin. Returns [dx', dy', guideX, guideY]
// with the snapped move (when snap) and the guide positions (NaN = none).
JFN(jfloatArray, nAlignSnap)(JNIEnv* env, jobject, jfloat dx, jfloat dy, jfloat thr, jboolean snap) {
    LOCK;
    Engine& e = E();
    float v[4] = { dx, dy, NAN, NAN };
    if (!e.alignRefs.empty()) {
        ops::AlignHit h = ops::AlignSnap(e.alignRefs, e.alignStart, e.alignHasOrigin, e.alignOx, e.alignOy, dx, dy, thr);
        if (h.x) { if (snap) v[0] += h.dx; v[2] = h.gx; }
        if (h.y) { if (snap) v[1] += h.dy; v[3] = h.gy; }
    }
    jfloatArray a = env->NewFloatArray(4);
    env->SetFloatArrayRegion(a, 0, 4, v);
    return a;
}

// Current page bounds of the selection: [l, b, r, t].
JFN(jfloatArray, nSelBounds)(JNIEnv* env, jobject) {
    LOCK;
    PRect r = SelBounds();
    float v[4] = { r.l, r.b, r.r, r.t };
    jfloatArray a = env->NewFloatArray(4);
    env->SetFloatArrayRegion(a, 0, 4, v);
    return a;
}

// Guide lines through the selected text (page points):
// [left, right, baseline, top, hits, l,b,r,t of each object they meet...]
// hits: 1 left, 2 right, 4 baseline, 8 top line up exactly with something.
// Empty when nothing with text is selected.
JFN(jfloatArray, nGuides)(JNIEnv* env, jobject) {
    LOCK;
    Engine& e = E();
    Sel& s = e.sel;
    std::vector<float> v;
    if (SelValid() && !AnnotSel()) {
        std::vector<FPDF_PAGEOBJECT> mine = Members();
        bool text = false;
        for (auto o : mine) text |= ops::ObjectType(o) == FPDF_PAGEOBJ_TEXT;
        if (text) {
            if (e.guidePage != s.page || e.guideGen != e.doc.Generation() || e.guideDoc != e.doc.DocId() ||
                e.guideObj != s.obj || e.guideCount != mine.size()) {
                e.guideRefs = ops::GuideRefs(e.doc, s.page, mine);
                e.guidePage = s.page; e.guideGen = e.doc.Generation(); e.guideDoc = e.doc.DocId();
                e.guideObj = s.obj; e.guideCount = mine.size();
            }
            ops::GuideEdges g = ops::SelectionEdges(mine, s.accum, SelBounds());
            ops::GuideMatch m = ops::MatchGuides(e.guideRefs, g, 0.25f);
            int hits = (m.x[0] ? 1 : 0) | (m.x[1] ? 2 : 0) | (m.y[0] ? 4 : 0) | (m.y[1] ? 8 : 0);
            v = { g.x[0], g.x[1], g.y[0], g.y[1], (float)hits };
            for (const PRect& r : m.boxes) { v.push_back(r.l); v.push_back(r.b); v.push_back(r.r); v.push_back(r.t); }
        }
    }
    jfloatArray a = env->NewFloatArray((jsize)v.size());
    if (!v.empty()) env->SetFloatArrayRegion(a, 0, (jsize)v.size(), v.data());
    return a;
}

JFN(void, nCommit)(JNIEnv* env, jobject, jstring label) { LOCK; Commit(S(env, label).c_str()); }

JFN(void, nDelete)(JNIEnv*, jobject) {
    LOCK;
    Engine& e = E();
    Sel& s = e.sel;
    if (AnnotSel()) {
        if (ops::DeleteAnnot(e.doc, s.annPage, s.annIdx)) { ClearSel(); Commit("Delete annotation"); }
        return;
    }
    if (!SelValid()) return;
    int pg = s.page;
    if (s.group.size() <= 1) {
        EnsureEditable();
        ops::DeleteObjectNested(e.doc, pg, s.obj, s.parent);
    } else {
        for (auto o : s.group) ops::DeleteObjectNested(e.doc, pg, o, nullptr);
    }
    ClearSel();
    Commit("Delete");
}

JFN(jint, nGroup)(JNIEnv*, jobject) {
    LOCK;
    Engine& e = E();
    Sel& s = e.sel;
    if (s.group.size() < 2 || !SelValid()) return 0;
    int gid = ops::MaxGroupId(e.doc) + 1;
    for (auto o : s.group) ops::SetGroupId(e.doc, s.page, o, gid);
    s.groupId = gid;
    Commit("Group objects");
    return (int)s.group.size();
}

JFN(jboolean, nUngroup)(JNIEnv*, jobject) {
    LOCK;
    Engine& e = E();
    Sel& s = e.sel;
    if (!SelValid()) return false;
    if (s.groupId) {
        for (auto o : ops::GroupMembers(e.doc, s.page, s.groupId)) ops::ClearGroupId(e.doc, s.page, o);
        s.groupId = 0;
        s.group = { s.obj };
        Commit("Ungroup");
        return true;
    }
    if (!s.parent && ops::ObjectType(s.obj) == FPDF_PAGEOBJ_FORM) {
        ops::ExplodeForm(e.doc, s.page, s.obj);
        ClearSel();
        Commit("Ungroup");
        return true;
    }
    return false;
}

JFN(jint, nSplit)(JNIEnv*, jobject) {
    LOCK;
    Engine& e = E();
    Sel& s = e.sel;
    if (!SelValid() || ops::ObjectType(s.obj) != FPDF_PAGEOBJ_PATH) return 0;
    EnsureEditable();
    if (ops::SplittableParts(s.obj) < 2) return 0;
    int pg = s.page;
    auto parts = ops::SplitPath(e.doc, pg, s.obj);
    if (parts.empty()) return 0;
    SelectMany(pg, parts, 0);
    Commit("Split into lines");
    return (int)parts.size();
}

// ============================================================ clipboard

JFN(jstring, nCopy)(JNIEnv* env, jobject) {
    LOCK;
    Engine& e = E();
    Json j;
    j.Begin();
    if (AnnotSel()) return J(env, j.Str("error", "annotation").End().s);
    if (!SelValid()) return J(env, j.Int("count", 0).End().s);
    if (e.sel.group.size() <= 1 && e.sel.parent) EnsureEditable();
    ClearLimbo();
    e.clip.clear();
    e.clipSrcPage = e.sel.page;
    e.pasteCount = 0;
    std::wstring textAll;
    int skipped = 0;
    for (auto o : Members()) {
        ops::ClipObject c;
        if (ops::CaptureObject(e.doc, e.sel.page, o, c)) {
            if (c.type == FPDF_PAGEOBJ_TEXT) { if (!textAll.empty()) textAll += L" "; textAll += c.text; }
            e.clip.push_back(std::move(c));
        } else ++skipped;
    }
    return J(env, j.Int("count", (int)e.clip.size()).Int("skipped", skipped).Str("text", W2U(textAll)).End().s);
}

JFN(jstring, nCut)(JNIEnv* env, jobject) {
    LOCK;
    Engine& e = E();
    if (AnnotSel()) {
        int pg = e.sel.annPage;
        if (ops::DeleteAnnot(e.doc, pg, e.sel.annIdx)) { ClearSel(); Commit("Delete annotation"); }
        return J(env, "{\"count\":1}");
    }
    if (!SelValid()) return J(env, "{\"count\":0}");
    jstring copied = Java_com_theocp_pdfeditor_Engine_nCopy(env, nullptr);
    if (e.clip.empty()) return copied;
    int pg = e.sel.page;
    FPDF_PAGE p = e.doc.Page(pg);
    for (auto o : Members())
        if (p && FPDFPage_RemoveObject(p, o)) e.limbo.push_back(o);
    e.limboDoc = e.doc.DocId();
    e.doc.ContentChanged(pg);
    ClearSel();
    Commit("Cut");
    return copied;
}

JFN(jboolean, nHasClip)(JNIEnv*, jobject) { LOCK; return !E().clip.empty(); }

// Pastes what was copied / cut. atPoint: top-left corner at (x, y) on `page`;
// otherwise the same position (offset a little when pasting on the same page).
JFN(jint, nPaste)(JNIEnv*, jobject, jint tp, jfloat x, jfloat y, jboolean atPoint) {
    LOCK;
    Engine& e = E();
    if (!e.doc.Raw() || e.clip.empty() || tp < 0 || tp >= e.doc.PageCount()) return 0;
    PRect cb = ops::ClipBounds(e.clip);
    float dx = 0, dy = 0;
    if (atPoint) { dx = x - cb.l; dy = y - cb.t; }
    else if (tp == e.clipSrcPage && e.limbo.empty()) { ++e.pasteCount; dx = 10.0f * e.pasteCount; dy = -10.0f * e.pasteCount; }
    int keepGid = 0;
    bool allGrouped = e.clip.size() > 1 && e.clip[0].groupId != 0;
    for (auto& c : e.clip) if (c.groupId != e.clip[0].groupId) allGrouped = false;
    std::vector<FPDF_PAGEOBJECT> pasted;
    if (!e.limbo.empty() && e.limboDoc == e.doc.DocId()) {
        FPDF_PAGE p = e.doc.Page(tp);
        for (auto o : e.limbo) {
            if (dx != 0 || dy != 0) ops::TransformObjectPage(e.doc, tp, o, FS_MATRIX{ 1, 0, 0, 1, dx, dy });
            if (p && FPDFPage_InsertObject(p, o)) pasted.push_back(o);
        }
        e.limbo.clear();
        e.doc.ContentChanged(tp);
        if (allGrouped) keepGid = e.clip[0].groupId;
        for (size_t i = 0; i < e.clip.size() && i < pasted.size(); ++i) {
            e.clip[i].srcObj = pasted[i]; e.clip[i].srcPage = tp; e.clip[i].srcGen = e.doc.Generation();
        }
        e.clipSrcPage = tp;
        e.pasteCount = 0;
    } else {
        e.limbo.clear();
        if (allGrouped) keepGid = ops::MaxGroupId(e.doc) + 1;
        std::string err;
        for (auto& c : e.clip)
            if (FPDF_PAGEOBJECT o = ops::PasteObject(e.doc, tp, c, dx, dy, keepGid, err)) pasted.push_back(o);
        if (pasted.empty()) { e.lastError = err; return 0; }
    }
    SelectMany(tp, pasted, keepGid);
    Commit("Paste");
    return (int)pasted.size();
}

JFN(jint, nMoveToPage)(JNIEnv*, jobject, jint tp) {
    LOCK;
    Engine& e = E();
    Sel& s = e.sel;
    if (!SelValid() || tp < 0 || tp >= e.doc.PageCount() || tp == s.page) return 0;
    if (s.group.size() <= 1 && s.parent) EnsureEditable();
    int src = s.page, gid = s.groupId;
    std::vector<FPDF_PAGEOBJECT> moved;
    for (auto o : Members())
        if (ops::MoveObjectToPage(e.doc, src, o, tp, 0, 0)) moved.push_back(o);
    if (moved.empty()) return 0;
    SelectMany(tp, moved, gid);
    Commit("Move to page");
    return (int)moved.size();
}

// ============================================================ text

static FPDF_PAGEOBJECT NearestText(int page, float px, float py, float* visualSize = nullptr) {
    Engine& e = E();
    FPDF_PAGEOBJECT best = nullptr;
    float bestd = 1e30f;
    for (auto& o : ops::ListObjects(e.doc, page)) {
        if (o.type != FPDF_PAGEOBJ_TEXT || o.parent) continue;
        float cx = (o.bounds.l + o.bounds.r) * 0.5f, cy = (o.bounds.b + o.bounds.t) * 0.5f;
        float dd = o.bounds.Contains(px, py) ? 0.0f : (cx - px) * (cx - px) + (cy - py) * (cy - py);
        if (dd < bestd) {
            bestd = dd;
            best = o.obj;
            if (visualSize) *visualSize = ops::VisualFontSize(o.obj);
        }
    }
    return best;
}

// Text of the selected text object (all its pieces), for the edit field.
JFN(jstring, nEditText)(JNIEnv* env, jobject) {
    LOCK;
    Engine& e = E();
    if (!SelValid() || ops::ObjectType(e.sel.obj) != FPDF_PAGEOBJ_TEXT) return nullptr;
    EnsureEditable();
    return JW(env, ops::EditableText(e.doc, e.sel.page, e.sel.obj));
}

// Writes the edited text with the exact same font, size and letter positions.
JFN(jstring, nCommitText)(JNIEnv* env, jobject, jstring jtext) {
    LOCK;
    Engine& e = E();
    ops::TextEditInfo ti;
    if (!SelValid() || ops::ObjectType(e.sel.obj) != FPDF_PAGEOBJ_TEXT) return J(env, EditInfoJson(false, ti));
    EnsureEditable();
    std::wstring w = W(env, jtext);
    while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n')) w.pop_back();
    int pg = e.sel.page;
    FPDF_PAGEOBJECT obj = e.sel.obj;
    bool blank = std::all_of(w.begin(), w.end(), [](wchar_t c) { return iswspace(c) != 0; });
    if (blank) {
        std::vector<FPDF_PAGEOBJECT> all = ops::TextPieces(e.doc, pg, obj);
        if (all.empty()) all = { obj };
        for (auto o : all) ops::DeleteObjectNested(e.doc, pg, o, nullptr);
        ClearSel();
        Commit("Delete text");
        ti.pieces.clear();
        return J(env, EditInfoJson(true, ti));
    }
    if (ops::EditableText(e.doc, pg, obj) == w) return J(env, EditInfoJson(true, ti));   // unchanged
    FPDF_PAGEOBJECT repl = nullptr;
    bool fb = false;
    bool ok = ops::SetObjectTextSafe(e.doc, pg, obj, w, &repl, &fb, &ti);
    if (ok) {
        if (ti.pieces.size() > 1) SelectMany(pg, ti.pieces, ti.groupId);
        else if (repl) SelectOne(pg, repl);
        Commit("Edit text");
    }
    return J(env, EditInfoJson(ok, ti));
}

// New text at (x, y) (top-left of the text) - in the style of the nearest
// text (matchNearby) or in the chosen font / size / colour.
JFN(jstring, nAddText)(JNIEnv* env, jobject, jint page, jfloat x, jfloat yTop, jstring jtext, jboolean matchNearby,
                       jfloat size, jint r, jint g, jint b, jint fontIdx) {
    LOCK;
    Engine& e = E();
    ops::TextEditInfo ti;
    if (!e.doc.Raw()) return J(env, EditInfoJson(false, ti));
    std::wstring w = W(env, jtext);
    std::string err;
    FPDF_PAGEOBJECT o = nullptr;
    float nearSize = 0;
    FPDF_PAGEOBJECT style = matchNearby ? NearestText(page, x, yTop, &nearSize) : nullptr;
    if (style) {
        ops::ClipObject c;
        if (ops::CaptureObject(e.doc, page, style, c)) {
            double sz = c.visualSize;
            if (sz < 2 || sz > 200) sz = size;
            o = ops::AddTextLike(e.doc, page, w, style, c.fontData, c.fontBase, sz, x, yTop - (float)sz * 0.8f,
                                 c.fr, c.fg, c.fb, err, &ti);
        }
    }
    if (!o) {
        FontChoice fc = ResolveFont(fontIdx, page);
        float y = yTop - size * 0.8f;
        if (fc.IsDoc() && fc.handle) {
            FPDF_PAGEOBJECT sample = nullptr;
            ops::FindDocFont(e.doc, fc.docName, page, &sample);
            if (sample) o = ops::AddTextLike(e.doc, page, w, sample, {}, fc.docName, size, x, y, r, g, b, err, &ti);
        }
        if (!o) {
            o = ops::AddText(e.doc, page, w, fc, size, x, y, r, g, b, err);
            ti = ops::TextEditInfo{};
            if (o) ti.pieces = { o };
        }
    }
    if (!o) { e.lastError = err; return J(env, EditInfoJson(false, ti)); }
    if (ti.pieces.size() > 1) SelectMany(page, ti.pieces, ti.groupId);
    else SelectOne(page, o);
    Commit("Add text");
    return J(env, EditInfoJson(true, ti));
}

JFN(void, nSetTextColor)(JNIEnv*, jobject, jint r, jint g, jint b) {
    LOCK;
    Engine& e = E();
    if (!SelValid()) return;
    EnsureEditable();
    for (auto o : Members()) ops::SetObjectFillColor(e.doc, e.sel.page, o, r, g, b, 255);
    Commit("Text colour");
}

JFN(jboolean, nSetTextSize)(JNIEnv*, jobject, jfloat size) {
    LOCK;
    Engine& e = E();
    if (!SelValid() || ops::ObjectType(e.sel.obj) != FPDF_PAGEOBJ_TEXT) return false;
    EnsureEditable();
    bool ok = ops::SetTextVisualSize(e.doc, e.sel.page, e.sel.obj, size);
    if (ok) Commit("Change size");
    return ok;
}

// Font list: [{"label":..., "kind":"doc|docref|std|sys"}], PDF fonts first.
JFN(jstring, nFonts)(JNIEnv* env, jobject) {
    LOCK;
    Engine& e = E();
    BuildFonts();
    std::string s = "[";
    for (size_t i = 0; i < e.fonts.size(); ++i) {
        const FontChoice& f = e.fonts[i];
        std::string kind = f.IsDoc() ? f.stdName : f.IsStandard() ? "std" : "sys";
        if (i) s += ",";
        s += "{\"label\":" + Json::Esc(f.IsDoc() ? f.label : f.IsStandard() ? f.stdName : f.label) + ",\"kind\":\"" + kind + "\"}";
    }
    return J(env, s + "]");
}

JFN(jboolean, nRestyle)(JNIEnv*, jobject, jint fontIdx, jfloat size) {
    LOCK;
    Engine& e = E();
    if (!SelValid() || ops::ObjectType(e.sel.obj) != FPDF_PAGEOBJ_TEXT) return false;
    EnsureEditable();
    FontChoice fc = ResolveFont(fontIdx, e.sel.page);
    std::string err;
    FPDF_PAGEOBJECT out = nullptr;
    if (!ops::RestyleText(e.doc, e.sel.page, e.sel.obj, fc, size, err, &out)) { e.lastError = err; return false; }
    SelectOne(e.sel.page, out);
    Commit("Change font");
    return true;
}

// ============================================================ shapes / pictures

// Thickness (> 0) and / or colour (r >= 0) of every selected line - one line,
// several, or a whole table grid. commit=false while a slider is dragged.
JFN(void, nSetLine)(JNIEnv*, jobject, jfloat thickness, jint r, jint g, jint b, jboolean commit) {
    LOCK;
    Engine& e = E();
    if (!SelValid()) return;
    Sel& s = e.sel;
    bool single = s.group.size() <= 1;
    if (single) EnsureEditable();
    float scale = single ? ops::MatrixScale(s.accum) : 1.0f;
    if (s.group.empty()) s.group = { s.obj };
    for (auto& o : s.group) {
        if (!ops::IsLine(o)) continue;
        if (r >= 0) ops::SetLineColor(e.doc, s.page, o, r, g, b);
        if (thickness > 0) {
            FPDF_PAGEOBJECT n = ops::SetLineThickness(e.doc, s.page, o, thickness / (scale > 0 ? scale : 1.0f));
            if (n != o) { if (s.obj == o) s.obj = n; o = n; }     // the line was rebuilt
        }
    }
    if (commit) Commit("Line");
}

// Thickest selected line and how many lines are selected: [thickness, count].
JFN(jfloatArray, nLineInfo)(JNIEnv* env, jobject) {
    LOCK;
    Engine& e = E();
    float thick = 0, n = 0;
    if (SelValid()) {
        float scale = e.sel.group.size() <= 1 ? ops::MatrixScale(e.sel.accum) : 1.0f;
        for (auto o : Members())
            if (ops::IsLine(o)) { thick = std::max(thick, ops::LineThickness(o) * scale); n += 1; }
    }
    float v[2] = { thick, n };
    jfloatArray a = env->NewFloatArray(2);
    env->SetFloatArrayRegion(a, 0, 2, v);
    return a;
}

JFN(jboolean, nAddRect)(JNIEnv*, jobject, jint page, jfloat l, jfloat b, jfloat r, jfloat t, jboolean filled,
                        jint fr, jint fg, jint fb, jboolean stroked, jint sr, jint sg, jint sb, jfloat width) {
    LOCK;
    Engine& e = E();
    PRect box = PRect::FromPoints(l, b, r, t);
    FPDF_PAGEOBJECT o = ops::AddRect(e.doc, page, box, filled, fr, fg, fb, stroked, sr, sg, sb, width);
    if (!o) return false;
    SelectOne(page, o);
    Commit("Add rectangle");
    return true;
}

// pixels: ARGB ints (Android Bitmap.getPixels), centred at (cx, cy), widthPt wide.
JFN(jboolean, nAddImage)(JNIEnv* env, jobject, jint page, jintArray pixels, jint w, jint h, jfloat cx, jfloat cy,
                         jfloat widthPt) {
    LOCK;
    Engine& e = E();
    if (!e.doc.Raw() || !pixels || w <= 0 || h <= 0) return false;
    Image img;
    img.w = w; img.h = h;
    img.px.resize((size_t)w * h);
    env->GetIntArrayRegion(pixels, 0, w * h, reinterpret_cast<jint*>(img.px.data()));   // ARGB == BGRA in memory
    float hPt = widthPt * h / (float)w;
    std::string err;
    FPDF_PAGEOBJECT o = ops::AddImage(e.doc, page, img, cx - widthPt / 2, cy - hPt / 2, widthPt, err);
    if (!o) { e.lastError = err; return false; }
    SelectOne(page, o);
    Commit("Add image");
    return true;
}

JFN(jint, nCutPiece)(JNIEnv*, jobject, jint page, jfloat l, jfloat b, jfloat r, jfloat t) {
    LOCK;
    Engine& e = E();
    PRect box = PRect::FromPoints(l, b, r, t);
    FPDF_PAGEOBJECT img = nullptr, parent = nullptr;
    float bestArea = 0;
    for (auto& o : ops::ListObjects(e.doc, page)) {
        if (o.type != FPDF_PAGEOBJ_IMAGE || !o.bounds.Intersects(box)) continue;
        float a = o.bounds.Intersect(box).Area();
        if (a >= bestArea) { bestArea = a; img = o.obj; parent = o.parent; }
    }
    if (!img) return 1;                                   // no picture under the box
    if (parent) ops::PromoteObject(e.doc, page, img);
    std::string err;
    FPDF_PAGEOBJECT piece = ops::CutImageRegion(e.doc, page, img, box, err);
    if (!piece) { e.lastError = err; return 2; }
    SelectOne(page, piece);
    Commit("Cut piece from picture");
    return 0;
}

JFN(jstring, nRedact)(JNIEnv* env, jobject, jint page, jfloat l, jfloat b, jfloat r, jfloat t) {
    LOCK;
    Engine& e = E();
    ClearSel();
    ops::RedactResult rr = ops::Redact(e.doc, page, PRect::FromPoints(l, b, r, t), 0, 0, 0);
    Commit("Redact");
    Json j;
    j.Begin().Int("removed", rr.removed).Int("textSplit", rr.textSplit).Int("images", rr.imagesMasked)
        .Int("annots", rr.annotsRemoved);
    return J(env, j.End().s);
}

JFN(jint, nAddTable)(JNIEnv*, jobject, jint page, jint cols, jint rows, jboolean header, jfloat headerH, jfloat margin,
                     jfloat lineW, jint r, jint g, jint b) {
    LOCK;
    Engine& e = E();
    FS_SIZEF sz = e.doc.PageSizePt(page);
    PRect box{ margin, margin, sz.width - margin, sz.height - margin };
    int n = ops::AddTable(e.doc, page, box, cols, rows, header, headerH, r, g, b, lineW);
    if (n > 0) { ClearSel(); Commit("Insert table"); }
    return n;
}

// ============================================================ pages / document

JFN(jboolean, nInsertBlank)(JNIEnv*, jobject, jint at) {
    LOCK;
    Engine& e = E();
    int ref = std::clamp(at - 1, 0, std::max(0, e.doc.PageCount() - 1));
    FS_SIZEF sz = e.doc.PageCount() ? e.doc.PageSizePt(ref) : FS_SIZEF{ 595, 842 };
    bool ok = ops::InsertBlankPage(e.doc, at, sz.width, sz.height);
    if (ok) { ClearSel(); Commit("Insert page"); }
    return ok;
}
JFN(jboolean, nDuplicatePage)(JNIEnv*, jobject, jint i) {
    LOCK;
    bool ok = ops::DuplicatePage(E().doc, i);
    if (ok) { ClearSel(); Commit("Duplicate page"); }
    return ok;
}
JFN(jboolean, nDeletePage)(JNIEnv*, jobject, jint i) {
    LOCK;
    if (E().doc.PageCount() <= 1) return false;
    bool ok = ops::DeletePages(E().doc, { i });
    if (ok) { ClearSel(); Commit("Delete page"); }
    return ok;
}
JFN(jboolean, nRotatePage)(JNIEnv*, jobject, jint i, jint quarters) {
    LOCK;
    bool ok = ops::RotatePage(E().doc, i, quarters);
    if (ok) { ClearSel(); Commit("Rotate page"); }
    return ok;
}
JFN(jboolean, nMovePage)(JNIEnv*, jobject, jint from, jint to) {
    LOCK;
    bool ok = ops::MovePage(E().doc, from, to);
    if (ok) { ClearSel(); Commit("Move page"); }
    return ok;
}

// Imports pages from another PDF (bytes) at index `at` (-1 = end).
JFN(jboolean, nImportPages)(JNIEnv* env, jobject, jbyteArray data, jstring pw, jstring range, jint at) {
    LOCK;
    Engine& e = E();
    // The desktop importer reads a file; hand it a temporary one in the app cache.
    std::string dir = S(env, nullptr);
    jclass cls = env->FindClass("com/theocp/pdfeditor/Engine");
    jfieldID fid = cls ? env->GetStaticFieldID(cls, "cacheDir", "Ljava/lang/String;") : nullptr;
    jstring jdir = fid ? (jstring)env->GetStaticObjectField(cls, fid) : nullptr;
    dir = S(env, jdir);
    std::wstring tmp = U2W(dir + "/import.pdf");
    Bytes b = BytesOf(env, data);
    if (!WriteFileBytes(tmp, b.data(), b.size())) { e.lastError = "could not read that PDF"; return false; }
    std::string err;
    bool ok = ops::ImportPagesFromFile(e.doc, tmp, S(env, pw), S(env, range), at, err);
    unlink(W2U(tmp).c_str());
    if (!ok) { e.lastError = err; return false; }
    ClearSel();
    Commit("Import pages");
    return true;
}

JFN(jboolean, nFlattenAll)(JNIEnv*, jobject) {
    LOCK;
    E().doc.KillFormFocus();
    bool ok = ops::FlattenAll(E().doc);
    if (ok) { ClearSel(); Commit("Flatten"); }
    return ok;
}
JFN(jboolean, nFlattenPage)(JNIEnv*, jobject, jint i) {
    LOCK;
    bool ok = ops::FlattenPage(E().doc, i);
    if (ok) { ClearSel(); Commit("Annotations to content"); }
    return ok;
}

JFN(jstring, nSearch)(JNIEnv* env, jobject, jstring q, jboolean matchCase, jboolean whole) {
    LOCK;
    auto hits = ops::SearchText(E().doc, W(env, q), matchCase, whole);
    std::string s = "[";
    char b[128];
    for (size_t i = 0; i < hits.size(); ++i) {
        snprintf(b, sizeof(b), "%s[%d,%.2f,%.2f,%.2f,%.2f]", i ? "," : "", hits[i].page, hits[i].box.l, hits[i].box.b,
                 hits[i].box.r, hits[i].box.t);
        s += b;
    }
    return J(env, s + "]");
}

JFN(jstring, nExtractText)(JNIEnv* env, jobject) { LOCK; return JW(env, ops::ExtractAllText(E().doc)); }

JFN(jstring, nMeta)(JNIEnv* env, jobject) {
    LOCK;
    Engine& e = E();
    Json j;
    j.Begin().Int("pages", e.doc.PageCount());
    for (const char* k : { "Title", "Author", "Subject", "Keywords", "Creator", "Producer" })
        j.Str(k, W2U(ops::GetMeta(e.doc, k)));
    j.Int("signatures", e.doc.Raw() ? FPDF_GetSignatureCount(e.doc.Raw()) : 0);
    return J(env, j.End().s);
}

JFN(jstring, nFormFields)(JNIEnv* env, jobject) {
    LOCK;
    auto fields = ops::ListFormFields(E().doc);
    std::string s = "[";
    for (size_t i = 0; i < fields.size(); ++i) {
        Json j;
        j.Begin().Int("page", fields[i].page).Str("name", W2U(fields[i].name)).Str("value", W2U(fields[i].value))
            .Int("type", fields[i].type).Rect("box", fields[i].box);
        s += (i ? "," : "") + j.End().s;
    }
    return J(env, s + "]");
}

// Sets a text field's value (first widget with that name on `page`).
JFN(jboolean, nSetFormValue)(JNIEnv* env, jobject, jint page, jstring jname, jstring jvalue) {
    LOCK;
    Engine& e = E();
    FPDF_PAGE p = e.doc.Page(page);
    if (!p || !e.doc.Form()) return false;
    std::wstring name = W(env, jname), value = W(env, jvalue);
    bool ok = false;
    for (int i = 0, n = FPDFPage_GetAnnotCount(p); i < n && !ok; ++i) {
        FPDF_ANNOTATION a = FPDFPage_GetAnnot(p, i);
        if (!a) continue;
        if (FPDFAnnot_GetSubtype(a) == FPDF_ANNOT_WIDGET) {
            std::wstring fname = ReadUtf16([&](unsigned short* buf, unsigned long len) {
                return FPDFAnnot_GetFormFieldName(e.doc.Form(), a, buf, len);
            });
            if (fname == name) ok = FPDFAnnot_SetStringValue(a, "V", WS(value)) != 0;
        }
        FPDFPage_CloseAnnot(a);
    }
    if (ok) {
        e.doc.VisualChanged(page);
        e.doc.modified = true;
        Commit("Fill form");
    }
    return ok;
}

JFN(jint, nSignatureCount)(JNIEnv*, jobject) {
    LOCK;
    return E().doc.Raw() ? FPDF_GetSignatureCount(E().doc.Raw()) : 0;
}
