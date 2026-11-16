package com.theocp.pdfeditor

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.graphics.Bitmap
import android.net.Uri
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.json.JSONObject

enum class Tool { Select, Text, Image, Rect, Cut, Redact, Pan }

/** Dialogs and sheets the editor can show (one at a time). */
sealed class Dlg {
    data class EditText(val text: String) : Dlg()
    data class NewText(val page: Int, val x: Float, val yTop: Float) : Dlg()
    data object Table : Dlg()
    data object Meta : Dlg()
    data object Import : Dlg()
    data class Extract(val text: String) : Dlg()
    data class Password(val bytes: ByteArray, val name: String, val uri: Uri?, val writable: Boolean, val wrong: Boolean) : Dlg()
    data object Settings : Dlg()
    data object Help : Dlg()
    data object About : Dlg()
    data object Forms : Dlg()
    data object Fonts : Dlg()
    data object MoveToPage : Dlg()
    data class Confirm(val then: () -> Unit) : Dlg()      // unsaved changes
}

/** What the activity does for the editor (pickers, files, clipboard). */
interface Host {
    fun pickPdf()
    fun pickImage()
    fun pickImportPdf()
    fun saveAs()
    fun exportPages()
    fun share()
    fun print()
    fun write(uri: Uri, bytes: ByteArray): Boolean
}

class Editor(private val ctx: Context, val prefs: Prefs) {
    lateinit var host: Host

    // ---- document
    var isOpen by mutableStateOf(false)
    var name by mutableStateOf("")
    var uri: Uri? = null
    var writable = false
    var pageCount by mutableIntStateOf(0)
    var pageSizes by mutableStateOf<List<Pair<Float, Float>>>(emptyList())
    var docId by mutableIntStateOf(0)
    var tick by mutableIntStateOf(0)                 // bumps after every change
    var modified by mutableStateOf(false)
    var canUndo by mutableStateOf(false)
    var canRedo by mutableStateOf(false)
    var currentPage by mutableIntStateOf(0)

    // ---- interaction
    var tool by mutableStateOf(Tool.Select)
    var sel by mutableStateOf(Selection())
    var dialog by mutableStateOf<Dlg?>(null)
    var message by mutableStateOf<Pair<String, Boolean>?>(null)   // text, error
    var showProps by mutableStateOf(false)            // properties sheet (phones)
    var showPages by mutableStateOf(false)            // pages sheet (phones)
    var scrollToPage by mutableStateOf(-1)
    var scrollToRect by mutableStateOf<SearchHit?>(null)
    var revealSel by mutableStateOf(false)            // scroll the selection above the properties sheet
    var pendingImageAt: Triple<Int, Float, Float>? = null
    var importFile by mutableStateOf<Pair<String, ByteArray>?>(null)   // chosen in the import dialog
    var afterSave: (() -> Unit)? = null                                  // runs once "Save as" succeeded
    var busy by mutableStateOf(false)

    // ---- app settings (recompose everything when they change)
    var language by mutableStateOf(prefs.language)        // "", "en", "de"
    var uiScale by mutableFloatStateOf(prefs.uiScale)
    var versionName = ""

    // ---- search
    var searching by mutableStateOf(false)
    var query by mutableStateOf("")
    var hits by mutableStateOf<List<SearchHit>>(emptyList())
    var hitIndex by mutableIntStateOf(-1)
    var matchCase by mutableStateOf(false)
    var wholeWord by mutableStateOf(false)

    // ---- style of new objects
    var matchFont by mutableStateOf(prefs.matchFont)
    var newTextSize by mutableFloatStateOf(12f)
    var newTextColor by mutableIntStateOf(0x1A1A1F)
    var fontIdx by mutableIntStateOf(0)
    var fillColor by mutableIntStateOf(0x1A1A1F)
    var strokeColor by mutableIntStateOf(0xD92626)
    var rectFilled by mutableStateOf(true)
    var rectStroked by mutableStateOf(false)
    var lineWidth by mutableFloatStateOf(1.5f)

    // ---- moving / resizing (remembered)
    var keepSpacing by mutableStateOf(prefs.keepSpacing)     // table resize: only the outer row / column changes
    var alignGuides by mutableStateOf(prefs.alignGuides)     // guide lines when text lines up with other text
    var alignSnap by mutableStateOf(prefs.alignSnap)         // ...and snap it into line
    var guidesAlways by mutableStateOf(prefs.guidesAlways)   // lines through selected text, the whole time

    fun toast(text: String, error: Boolean = false) { message = text to error }

    /** Re-reads everything that may have changed after an engine call. */
    fun refresh() {
        isOpen = Engine.nIsOpen()
        pageCount = if (isOpen) Engine.nPageCount() else 0
        pageSizes = (0 until pageCount).map { Engine.nPageSize(it).let { s -> s[0] to s[1] } }
        docId = Engine.nDocId()
        modified = isOpen && Engine.nModified()
        canUndo = isOpen && Engine.nCanUndo()
        canRedo = isOpen && Engine.nCanRedo()
        sel = Selection.parse(if (isOpen) Engine.nSelection() else null)
        if (currentPage >= pageCount) currentPage = maxOf(0, pageCount - 1)
        tick++
    }

    // ------------------------------------------------------------ files

    /** Opens PDF bytes. Returns false if a password is needed (dialog shown). */
    fun open(bytes: ByteArray, displayName: String, from: Uri?, canWrite: Boolean, password: String = ""): Boolean {
        when (Engine.nOpen(bytes, password, displayName)) {
            0 -> {
                name = displayName
                uri = from
                writable = canWrite
                tool = Tool.Select
                hits = emptyList(); hitIndex = -1; searching = false
                currentPage = 0
                scrollToPage = 0
                refresh()
                if (Engine.nSignatureCount() > 0)
                    toast(trf("Opened %s - note: it is digitally signed; any edit invalidates the signature", displayName))
                if (from != null) prefs.recent = listOf(from.toString()) + prefs.recent.filter { it != from.toString() }
                return true
            }
            1 -> { dialog = Dlg.Password(bytes, displayName, from, canWrite, password.isNotEmpty()); return false }
            else -> { toast(tr("Could not open this PDF"), true); return false }
        }
    }

    fun newDocument() {
        if (Engine.nNew()) {
            name = tr("Untitled.pdf"); uri = null; writable = false
            currentPage = 0; scrollToPage = 0
            refresh()
            toast(tr("New blank document"))
        }
    }

    fun close() {
        Engine.nClose()
        name = ""; uri = null
        hits = emptyList(); searching = false
        refresh()
    }

    /** Runs `then` now, or after asking about unsaved changes. */
    fun guardUnsaved(then: () -> Unit) {
        if (isOpen && Engine.nModified()) dialog = Dlg.Confirm(then) else then()
    }

    /** Saves to the opened file, or asks where to save. Runs `then` once saved. */
    fun save(then: (() -> Unit)? = null) {
        val u = uri
        if (u == null || !writable) { afterSave = then; host.saveAs(); return }
        val bytes = Engine.nSave() ?: run { toast(tr("Could not save"), true); return }
        if (host.write(u, bytes)) {
            Engine.nMarkSaved(); refresh(); toast(trf("Saved %s", name))
            then?.invoke()
        } else { afterSave = then; host.saveAs() }
    }

    fun applyLanguage(code: String) {
        language = code
        prefs.language = code
        L10n.german = when (code) {
            "de" -> true
            "en" -> false
            else -> java.util.Locale.getDefault().language == "de"
        }
        tick++
    }

    fun applyUiScale(s: Float) { uiScale = s; prefs.uiScale = s }

    fun savedAs(u: Uri, displayName: String, bytes: ByteArray): Boolean {
        if (!host.write(u, bytes)) { toast(tr("Could not save"), true); return false }
        uri = u; writable = true; name = displayName
        Engine.nMarkSaved()
        refresh()
        toast(trf("Saved %s", displayName))
        afterSave?.let { afterSave = null; it() }
        return true
    }

    // ------------------------------------------------------------ selection

    fun tap(page: Int, px: Float, py: Float, tolPt: Float, single: Boolean = false) {
        sel = Selection.parse(Engine.nTap(page, px, py, tolPt, single))
        currentPage = page
    }

    fun clearSelection() { Engine.nClearSelection(); sel = Selection() }

    fun selectArea(page: Int, r: PRect) {
        Engine.nSelectRect(page, r.l, r.b, r.r, r.t)
        refresh()
    }

    fun selectAll(page: Int, linesOnly: Boolean = false) {
        val n = Engine.nSelectAll(page, linesOnly)
        refresh()
        if (n == 0) toast(tr(if (linesOnly) "No lines on this page" else "Nothing on this page"), true)
        else toast(trf(if (linesOnly) "Selected %d line(s)" else "Selected %d object(s)", n))
    }

    fun delete() { Engine.nDelete(); refresh() }

    fun group() {
        val n = Engine.nGroup()
        refresh()
        if (n > 0) toast(trf("Grouped %d objects", n))
    }

    fun ungroup() { if (Engine.nUngroup()) { refresh(); toast(tr("Split into separate pieces")) } }

    fun split() {
        val n = Engine.nSplit()
        refresh()
        if (n > 0) toast(trf("Split into %d separate lines", n)) else toast(tr("This is already a single line"))
    }

    fun copy(): Boolean {
        val j = JSONObject(Engine.nCopy())
        if (j.has("error")) { toast(tr("Annotations can't be copied - right-click > Make this page's annotations editable"), true); return false }
        val n = j.optInt("count")
        val text = j.optString("text")
        if (text.isNotEmpty()) {
            val cm = ctx.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
            cm.setPrimaryClip(ClipData.newPlainText("PDF", text))
        }
        if (n == 0) toast(tr("Nothing in the selection can be copied"), true)
        else toast(trf(if (j.optInt("skipped") > 0) "Copied %d object(s) (some parts could not be copied)" else "Copied %d object(s)", n))
        return n > 0
    }

    fun cut() {
        val j = JSONObject(Engine.nCut())
        refresh()
        val n = j.optInt("count")
        if (n > 0) toast(trf("Cut %d object(s)", n))
    }

    fun hasClip() = Engine.nHasClip()

    /** Pastes copied objects; with no copied objects, text from the system clipboard. */
    fun paste(page: Int, x: Float, y: Float, atPoint: Boolean) {
        if (Engine.nHasClip()) {
            val n = Engine.nPaste(page, x, y, atPoint)
            refresh()
            if (n > 0) toast(trf("Pasted %d object(s) onto page %d", n, page + 1))
            else toast(tr("Paste failed: ") + tr(Engine.nLastError()), true)
            return
        }
        val cm = ctx.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        val text = cm.primaryClip?.takeIf { it.itemCount > 0 }?.getItemAt(0)?.coerceToText(ctx)?.toString().orEmpty()
        if (text.isBlank()) { toast(tr("Nothing to paste"), true); return }
        val lines = text.replace("\r", "").split('\n')
        var yy = y
        var made = 0
        for (ln in lines) {
            if (ln.isNotBlank()) {
                val r = EditResult.parse(Engine.nAddText(page, x, yy, ln, matchFont, newTextSize,
                    (newTextColor shr 16) and 255, (newTextColor shr 8) and 255, newTextColor and 255, fontIdx))
                if (r.ok) made++
            }
            yy -= newTextSize * 1.2f
        }
        refresh()
        if (made > 0) toast(trf(if (made == 1) "Pasted text (%d line) onto page %d" else "Pasted text (%d lines) onto page %d", made, page + 1))
    }

    fun moveToPage(target: Int) {
        val n = Engine.nMoveToPage(target)
        refresh()
        if (n > 0) { toast(trf("Moved %d object(s) to page %d", n, target + 1)); scrollToPage = target }
        else toast(tr("Could not move to that page"), true)
    }

    // ------------------------------------------------------------ text

    fun beginEditText() {
        val t = Engine.nEditText() ?: return
        dialog = Dlg.EditText(t)
    }

    fun commitText(text: String) {
        val r = EditResult.parse(Engine.nCommitText(text))
        refresh()
        if (!r.ok) toast(tr("Could not change this text"), true)
        else r.note()?.let { toast(it) }
    }

    fun addText(page: Int, x: Float, yTop: Float, text: String) {
        val r = EditResult.parse(Engine.nAddText(page, x, yTop, text, matchFont, newTextSize,
            (newTextColor shr 16) and 255, (newTextColor shr 8) and 255, newTextColor and 255, fontIdx))
        refresh()
        if (!r.ok) toast(tr("Could not add the text: ") + tr(Engine.nLastError()), true)
        else r.note()?.let { toast(it) }
    }

    fun setTextColor(rgb: Int) { Engine.nSetTextColor((rgb shr 16) and 255, (rgb shr 8) and 255, rgb and 255); refresh() }
    fun setTextSize(size: Float) { if (Engine.nSetTextSize(size)) refresh() }
    fun restyle(fontIdx: Int, size: Float) {
        if (Engine.nRestyle(fontIdx, size)) refresh()
        else toast(tr("Could not change the font: ") + tr(Engine.nLastError()), true)
    }
    fun fonts(): List<FontItem> = parseFonts(Engine.nFonts())

    // ------------------------------------------------------------ shapes / pictures

    /** Thickness of every selected line (one, several or a grid); commit=false while sliding. */
    fun setLineThickness(thickness: Float, commit: Boolean) {
        Engine.nSetLine(thickness.coerceIn(0.05f, 100f), -1, -1, -1, commit)
        if (commit) refresh() else tick++
    }

    fun setLineColor(rgb: Int) {
        Engine.nSetLine(-1f, (rgb shr 16) and 255, (rgb shr 8) and 255, rgb and 255, true)
        refresh()
    }

    /** Thickest selected line and the number of selected lines. */
    fun lineInfo(): Pair<Float, Int> = Engine.nLineInfo().let { it[0] to it[1].toInt() }

    fun addRect(page: Int, r: PRect) {
        if (r.w < 1 || r.h < 1) return
        Engine.nAddRect(page, r.l, r.b, r.r, r.t, rectFilled, (fillColor shr 16) and 255, (fillColor shr 8) and 255,
            fillColor and 255, rectStroked || !rectFilled, (strokeColor shr 16) and 255, (strokeColor shr 8) and 255,
            strokeColor and 255, lineWidth)
        refresh()
    }

    fun redact(page: Int, r: PRect) {
        if (r.w < 1 || r.h < 1) return
        val j = JSONObject(Engine.nRedact(page, r.l, r.b, r.r, r.t))
        refresh()
        toast(trf("Redacted: %d removed, %d text line(s) cut, %d picture(s) blacked out, %d annotation(s) removed",
            j.optInt("removed"), j.optInt("textSplit"), j.optInt("images"), j.optInt("annots")))
    }

    fun cutPiece(page: Int, r: PRect) {
        when (Engine.nCutPiece(page, r.l, r.b, r.r, r.t)) {
            0 -> { tool = Tool.Select; refresh(); toast(tr("Piece lifted out of the picture")) }
            1 -> toast(tr("There is no picture under that box."), true)
            else -> toast(tr("Could not cut: ") + tr(Engine.nLastError()), true)
        }
    }

    fun placeImage(bitmap: Bitmap) {
        val (page, cx, cy) = pendingImageAt ?: (currentPage to 0f).let { Triple(currentPage, pageSizes.getOrNull(currentPage)?.first?.div(2) ?: 300f, pageSizes.getOrNull(currentPage)?.second?.div(2) ?: 400f) }
        pendingImageAt = null
        val bm = if (bitmap.config == Bitmap.Config.ARGB_8888) bitmap else bitmap.copy(Bitmap.Config.ARGB_8888, false)
        val px = IntArray(bm.width * bm.height)
        bm.getPixels(px, 0, bm.width, 0, 0, bm.width, bm.height)
        val pageW = pageSizes.getOrNull(page)?.first ?: 595f
        val widthPt = minOf(pageW * 0.5f, bm.width * 0.75f)
        if (Engine.nAddImage(page, px, bm.width, bm.height, cx, cy, widthPt)) { tool = Tool.Select; refresh(); toast(tr("Image added")) }
        else toast(tr("Add image failed: ") + tr(Engine.nLastError()), true)
    }

    fun addTable(page: Int, cols: Int, rows: Int, header: Boolean, headerH: Float, margin: Float, lineW: Float) {
        val n = Engine.nAddTable(page, cols, rows, header, headerH, margin, lineW, 0, 0, 0)
        refresh()
        if (n > 0) toast(trf("Table added (%d editable lines)", n)) else toast(tr("Could not build the table (check the margin vs page size)"), true)
    }

    // ------------------------------------------------------------ undo / pages / doc

    fun undo() { if (Engine.nUndo()) { refresh(); toast(tr("Undone")) } }
    fun redo() { if (Engine.nRedo()) { refresh(); toast(tr("Redone")) } }
    fun commit(label: String) { Engine.nCommit(label); refresh() }

    fun insertPageAfter(i: Int) { if (Engine.nInsertBlank(i + 1)) { refresh(); scrollToPage = i + 1 } }
    fun duplicatePage(i: Int) { if (Engine.nDuplicatePage(i)) refresh() }
    fun deletePage(i: Int) { if (Engine.nDeletePage(i)) refresh() }
    fun rotatePage(i: Int, q: Int) { if (Engine.nRotatePage(i, q)) refresh() }
    fun movePage(from: Int, to: Int) { if (Engine.nMovePage(from, to)) { refresh(); scrollToPage = to } }

    fun importPages(bytes: ByteArray, password: String, range: String) {
        if (Engine.nImportPages(bytes, password, range, -1)) { refresh(); toast(tr("Pages imported")) }
        else toast(tr("Import failed: ") + tr(Engine.nLastError()), true)
    }

    fun flattenAll() {
        if (Engine.nFlattenAll()) { refresh(); toast(tr("Flattened annotations and form fields")) }
        else toast(tr("Nothing to flatten"), true)
    }

    fun flattenPage(i: Int) {
        if (Engine.nFlattenPage(i)) { refresh(); toast(trf("Page %d: annotations are now normal content - click them to edit, split or move", i + 1)) }
        else toast(tr("This page has no annotations to convert"), true)
    }

    fun runSearch() {
        if (query.isBlank()) { hits = emptyList(); hitIndex = -1; return }
        hits = parseHits(Engine.nSearch(query, matchCase, wholeWord))
        hitIndex = if (hits.isEmpty()) -1 else 0
        hits.firstOrNull()?.let { scrollToRect = it }
    }

    fun stepHit(d: Int) {
        if (hits.isEmpty()) return
        hitIndex = (hitIndex + d + hits.size) % hits.size
        scrollToRect = hits[hitIndex]
    }

    fun meta(): JSONObject = JSONObject(Engine.nMeta())
    fun formFields(): List<FormField> = parseFields(Engine.nFormFields())
    fun setFormValue(f: FormField, value: String) { if (Engine.nSetFormValue(f.page, f.name, value)) refresh() }
}
