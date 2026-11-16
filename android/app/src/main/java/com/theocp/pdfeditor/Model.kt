package com.theocp.pdfeditor

import org.json.JSONArray
import org.json.JSONObject

/** A rectangle in PDF page space (points, y up). */
data class PRect(val l: Float, val b: Float, val r: Float, val t: Float) {
    val w get() = r - l
    val h get() = t - b
    fun contains(x: Float, y: Float) = x in l..r && y in b..t
    companion object {
        fun of(a: JSONArray?) = if (a == null || a.length() < 4) null
        else PRect(a.getDouble(0).toFloat(), a.getDouble(1).toFloat(), a.getDouble(2).toFloat(), a.getDouble(3).toFloat())
        fun points(x0: Float, y0: Float, x1: Float, y1: Float) =
            PRect(minOf(x0, x1), minOf(y0, y1), maxOf(x0, x1), maxOf(y0, y1))
    }
}

object ObjType { const val TEXT = 1; const val PATH = 2; const val IMAGE = 3; const val SHADING = 4; const val FORM = 5 }

data class Selection(
    val has: Boolean = false,
    val annot: Boolean = false,
    val page: Int = -1,
    val count: Int = 0,
    val group: Int = 0,
    val type: Int = 0,
    val bounds: PRect? = null,
    val font: String = "",
    val embedded: Boolean = false,
    val size: Float = 0f,
    val color: Int = 0,
    val thickness: Float = 0f,
    val parts: Int = 0,
    val textEdit: Boolean = false,
    val line: Boolean = false,          // a line of any kind (stroke, bar, picture)
    val lineColor: Boolean = false,     // its colour can be changed
    val lines: Int = 0,                 // several selected: how many are lines
    val texts: Int = 0,                 // ...and how many are text
) {
    val isText get() = has && !annot && count <= 1 && type == ObjType.TEXT
    val isPath get() = has && !annot && count <= 1 && type == ObjType.PATH
    val isImage get() = has && !annot && count <= 1 && type == ObjType.IMAGE
    val multi get() = has && count > 1
    /** Can be typed into (one text, or the pieces of an earlier edit). */
    val canEditText get() = has && !annot && textEdit

    companion object {
        fun parse(s: String?): Selection {
            if (s.isNullOrEmpty()) return Selection()
            val j = JSONObject(s)
            if (!j.optBoolean("has")) return Selection()
            val rgb = (j.optInt("r") shl 16) or (j.optInt("g") shl 8) or j.optInt("b")
            return Selection(
                has = true,
                annot = j.optBoolean("annot"),
                page = j.optInt("page", -1),
                count = j.optInt("count"),
                group = j.optInt("group"),
                type = j.optInt("type"),
                bounds = PRect.of(j.optJSONArray("bounds")),
                font = j.optString("font"),
                embedded = j.optBoolean("embedded"),
                size = j.optDouble("size", 0.0).toFloat(),
                color = rgb,
                thickness = j.optDouble("thickness", 0.0).toFloat(),
                parts = j.optInt("parts"),
                textEdit = j.optBoolean("textEdit"),
                line = j.optBoolean("line"),
                lineColor = j.optBoolean("lineColor"),
                lines = j.optInt("lines"),
                texts = j.optInt("texts"),
            )
        }
    }
}

data class EditResult(val ok: Boolean, val missing: String, val font: String, val standIn: String, val pieces: Int) {
    companion object {
        fun parse(s: String) = JSONObject(s).let {
            EditResult(it.optBoolean("ok"), it.optString("missing"), it.optString("font"), it.optString("standIn"),
                it.optInt("pieces"))
        }
    }

    /** Short note when a character the PDF's font lacks was drawn with a stand-in. */
    fun note(): String? {
        if (standIn.isEmpty() || missing.isEmpty()) return null
        val chars = missing.map { "\"$it\"" }.joinToString(" ")
        val f = font.ifEmpty { tr("The PDF's font") }
        return trf("%s is not in the font %s - drawn with %s instead", chars, f, standIn)
    }
}

data class SearchHit(val page: Int, val box: PRect)

fun parseHits(s: String): List<SearchHit> {
    val a = JSONArray(s)
    return (0 until a.length()).map {
        val h = a.getJSONArray(it)
        SearchHit(h.getInt(0), PRect(h.getDouble(1).toFloat(), h.getDouble(2).toFloat(), h.getDouble(3).toFloat(),
            h.getDouble(4).toFloat()))
    }
}

data class FontItem(val label: String, val kind: String)

fun parseFonts(s: String): List<FontItem> {
    val a = JSONArray(s)
    return (0 until a.length()).map { a.getJSONObject(it).let { o -> FontItem(o.optString("label"), o.optString("kind")) } }
}

data class FormField(val page: Int, val name: String, val value: String, val type: Int)

fun parseFields(s: String): List<FormField> {
    val a = JSONArray(s)
    return (0 until a.length()).map {
        a.getJSONObject(it).let { o -> FormField(o.optInt("page"), o.optString("name"), o.optString("value"), o.optInt("type")) }
    }
}
