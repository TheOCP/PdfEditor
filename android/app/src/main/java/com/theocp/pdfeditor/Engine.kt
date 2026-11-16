package com.theocp.pdfeditor

import android.graphics.Bitmap

/**
 * The editing engine shared with the desktop version (C++ / PDFium), reached
 * through JNI. Every call is serialized natively; the UI calls rendering from
 * a background thread and edits from the main thread.
 */
object Engine {
    init {
        System.loadLibrary("pdfium")
        System.loadLibrary("pdfeditor")
        nInit()
    }

    /** App cache folder (set at start-up; used for temporary files). */
    @JvmField @Volatile var cacheDir: String = ""

    // ---- document
    external fun nInit()
    external fun nOpen(data: ByteArray, password: String, name: String): Int   // 0 ok, 1 password, 2 error
    external fun nNew(): Boolean
    external fun nClose()
    external fun nLastError(): String
    external fun nSave(): ByteArray?
    external fun nMarkSaved()
    external fun nModified(): Boolean
    external fun nIsOpen(): Boolean
    external fun nPageCount(): Int
    external fun nDocId(): Int
    external fun nPageVersion(page: Int): Int
    external fun nPageSize(page: Int): FloatArray
    external fun nCanUndo(): Boolean
    external fun nCanRedo(): Boolean
    external fun nUndo(): Boolean
    external fun nRedo(): Boolean
    external fun nRender(page: Int, scale: Float, x: Int, y: Int, w: Int, h: Int, bitmap: Bitmap): Boolean
    external fun nDevToPage(page: Int, scale: Float, dx: Float, dy: Float): FloatArray
    external fun nPageToDev(page: Int, scale: Float, px: Float, py: Float): FloatArray

    // ---- selection
    external fun nTap(page: Int, px: Float, py: Float, tol: Float, single: Boolean): String
    external fun nSelection(): String
    external fun nClearSelection()
    external fun nSelectRect(page: Int, l: Float, b: Float, r: Float, t: Float): Int
    external fun nSelectAll(page: Int, linesOnly: Boolean): Int
    external fun nMoveSelection(dx: Float, dy: Float)
    external fun nResizeSelection(l: Float, b: Float, r: Float, t: Float, keepSpacing: Boolean)
    external fun nSelBounds(): FloatArray
    // alignment while text is dragged
    external fun nAlignBegin(): Boolean
    external fun nAlignSnap(dx: Float, dy: Float, thr: Float, snap: Boolean): FloatArray   // [dx, dy, guideX, guideY]
    // guide lines through selected text: [left, right, baseline, top, hits, boxes (l,b,r,t)...], empty = none
    external fun nGuides(): FloatArray
    external fun nCommit(label: String)
    external fun nDelete()
    external fun nGroup(): Int
    external fun nUngroup(): Boolean
    external fun nSplit(): Int

    // ---- clipboard
    external fun nCopy(): String
    external fun nCut(): String
    external fun nHasClip(): Boolean
    external fun nPaste(page: Int, x: Float, y: Float, atPoint: Boolean): Int
    external fun nMoveToPage(page: Int): Int

    // ---- text
    external fun nEditText(): String?
    external fun nCommitText(text: String): String
    external fun nAddText(page: Int, x: Float, yTop: Float, text: String, matchNearby: Boolean, size: Float,
                          r: Int, g: Int, b: Int, fontIdx: Int): String
    external fun nSetTextColor(r: Int, g: Int, b: Int)
    external fun nSetTextSize(size: Float): Boolean
    external fun nFonts(): String
    external fun nRestyle(fontIdx: Int, size: Float): Boolean

    // ---- shapes / pictures
    external fun nSetLine(thickness: Float, r: Int, g: Int, b: Int, commit: Boolean)
    external fun nLineInfo(): FloatArray   // [thickest selected line, number of selected lines]
    external fun nAddRect(page: Int, l: Float, b: Float, r: Float, t: Float, filled: Boolean, fr: Int, fg: Int, fb: Int,
                          stroked: Boolean, sr: Int, sg: Int, sb: Int, width: Float): Boolean
    external fun nAddImage(page: Int, pixels: IntArray, w: Int, h: Int, cx: Float, cy: Float, widthPt: Float): Boolean
    external fun nCutPiece(page: Int, l: Float, b: Float, r: Float, t: Float): Int   // 0 ok, 1 no picture, 2 error
    external fun nRedact(page: Int, l: Float, b: Float, r: Float, t: Float): String
    external fun nAddTable(page: Int, cols: Int, rows: Int, header: Boolean, headerH: Float, margin: Float,
                           lineW: Float, r: Int, g: Int, b: Int): Int

    // ---- pages / document
    external fun nInsertBlank(at: Int): Boolean
    external fun nDuplicatePage(page: Int): Boolean
    external fun nDeletePage(page: Int): Boolean
    external fun nRotatePage(page: Int, quarters: Int): Boolean
    external fun nMovePage(from: Int, to: Int): Boolean
    external fun nImportPages(data: ByteArray, password: String, range: String, at: Int): Boolean
    external fun nFlattenAll(): Boolean
    external fun nFlattenPage(page: Int): Boolean
    external fun nSearch(query: String, matchCase: Boolean, whole: Boolean): String
    external fun nExtractText(): String
    external fun nMeta(): String
    external fun nFormFields(): String
    external fun nSetFormValue(page: Int, name: String, value: String): Boolean
    external fun nSignatureCount(): Int
}
