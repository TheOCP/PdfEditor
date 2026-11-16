package com.theocp.pdfeditor

import android.graphics.Bitmap
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.itemsIndexed
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Add
import androidx.compose.material.icons.outlined.Remove
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CheckboxDefaults
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.FilledTonalIconButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.util.Locale
import kotlin.math.ln
import kotlin.math.exp
import kotlin.math.roundToInt

// ------------------------------------------------------------------ small parts

@Composable
fun SectionTitle(text: String, modifier: Modifier = Modifier) {
    Text(text, color = C.TextDim, style = MaterialTheme.typography.labelLarge, modifier = modifier.padding(top = 4.dp, bottom = 6.dp))
}

@Composable
fun Dim(text: String) = Text(text, color = C.TextDim, fontSize = 13.sp)

private fun fmt(v: Float, digits: Int = 2) = String.format(if (L10n.german) Locale.GERMANY else Locale.US, "%.${digits}f", v)
private fun parseNum(s: String): Float? = s.replace(',', '.').replace("pt", "").trim().toFloatOrNull()

/** A number field that applies its value when Done is pressed or focus leaves. */
@Composable
fun NumberField(value: Float, suffix: String, modifier: Modifier = Modifier, digits: Int = 2, onCommit: (Float) -> Unit) {
    var text by remember(value) { mutableStateOf(fmt(value, digits)) }
    var focused by remember { mutableStateOf(false) }
    OutlinedTextField(
        value = text,
        onValueChange = { text = it },
        singleLine = true,
        suffix = { Text(suffix, color = C.TextDim) },
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
        keyboardActions = androidx.compose.foundation.text.KeyboardActions(onDone = { parseNum(text)?.let(onCommit) }),
        modifier = modifier.onFocusChanged {
            if (focused && !it.isFocused) parseNum(text)?.let { v -> if (v != value) onCommit(v) }
            focused = it.isFocused
        },
    )
}

private val swatches = listOf(
    0x000000, 0x1A1A1F, 0x5A5F6B, 0xA0A4AE, 0xFFFFFF,
    0xD92626, 0xF07A1A, 0xE6B800, 0x2E9E4F, 0x1F7AE0, 0x3B4CCA, 0x8E44AD,
)

/** Colour swatches; the current colour is ringed. */
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun ColorRow(current: Int, onPick: (Int) -> Unit) {
    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        for (c in swatches) {
            val sel = current >= 0 && (current and 0xFFFFFF) == c
            Box(
                Modifier.size(32.dp).clip(CircleShape).background(Color(0xFF000000.toInt() or c))
                    .border(if (sel) 3.dp else 1.dp, if (sel) C.Accent else C.Line, CircleShape)
                    .clickable { onPick(c) },
            )
        }
    }
}

@Composable
fun CheckRow(text: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth().clip(RoundedCornerShape(8.dp)).clickable { onChange(!checked) }.padding(vertical = 2.dp),
        verticalAlignment = Alignment.CenterVertically) {
        Checkbox(checked, onChange, colors = CheckboxDefaults.colors(checkedColor = C.Accent))
        Text(text, fontSize = 14.sp, modifier = Modifier.weight(1f))
    }
}

// ------------------------------------------------------------------ line thickness

private const val MinW = 0.1f
private const val MaxW = 20f
private fun toSlider(w: Float) = ((ln(w.coerceIn(MinW, MaxW)) - ln(MinW)) / (ln(MaxW) - ln(MinW)))
private fun fromSlider(s: Float) = exp(ln(MinW) + s * (ln(MaxW) - ln(MinW)))

/** Slider (logarithmic) + exact value with -/+ for every selected line. */
@Composable
private fun LineThickness(ed: Editor) {
    val (thick, n) = remember(ed.tick, ed.sel) { ed.lineInfo() }
    if (n == 0) return
    var live by remember(ed.sel, thick) { mutableFloatStateOf(thick) }
    HorizontalDivider(color = C.Line, modifier = Modifier.padding(vertical = 10.dp))
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(tr("Line thickness"), fontSize = 14.sp)
        if (n > 1) { Spacer(Modifier.width(6.dp)); Dim(trf("(%d lines)", n)) }
        Spacer(Modifier.weight(1f))
        Text(fmt(live) + " pt", color = C.Accent, fontSize = 14.sp)
    }
    Slider(
        value = toSlider(live),
        onValueChange = { live = fromSlider(it); ed.setLineThickness(live, false) },
        onValueChangeFinished = { ed.setLineThickness(live, true) },
        colors = SliderDefaults.colors(thumbColor = C.Accent, activeTrackColor = C.Accent, inactiveTrackColor = C.Control),
    )
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        FilledTonalIconButton(onClick = { val v = (live - 0.25f).coerceAtLeast(MinW); live = v; ed.setLineThickness(v, true) }) {
            Icon(Icons.Outlined.Remove, tr("Thinner"))
        }
        NumberField(live, "pt", Modifier.weight(1f)) { v -> val c = v.coerceIn(0.05f, 100f); live = c; ed.setLineThickness(c, true) }
        FilledTonalIconButton(onClick = { val v = live + 0.25f; live = v; ed.setLineThickness(v, true) }) {
            Icon(Icons.Outlined.Add, tr("Thicker"))
        }
    }
}

// ------------------------------------------------------------------ properties

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun PropertiesPanel(ed: Editor, modifier: Modifier = Modifier) {
    Column(modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 12.dp)) {
        if (!ed.isOpen) return@Column
        val s = ed.sel
        SectionTitle(tr("Selection"))
        if (!s.has) {
            Dim(tr("Nothing selected"))
        } else if (s.annot) {
            Text(trf("Annotation  -  page %d", s.page + 1))
            Spacer(Modifier.height(8.dp))
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                FilledTonalButton(onClick = { ed.flattenPage(s.page) }) { Text(tr("Make this page's annotations editable")) }
                OutlinedButton(onClick = { ed.delete() }) { Text(tr("Delete")) }
            }
        } else {
            val kind = when {
                s.multi -> trf("%d selected", s.count)
                s.type == ObjType.TEXT -> tr("Text")
                s.type == ObjType.PATH -> tr("Line / shape")
                s.type == ObjType.IMAGE -> tr("Image")
                s.type == ObjType.FORM -> tr("Group")
                else -> tr("Object")
            }
            Text("$kind  -  " + trf("Page %d", s.page + 1), fontSize = 15.sp)
            s.bounds?.let { b ->
                Spacer(Modifier.height(8.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    NumberField(b.l, "X", Modifier.weight(1f), 1) { v -> Engine.nMoveSelection(v - b.l, 0f); ed.commit("Move") }
                    NumberField(b.b, "Y", Modifier.weight(1f), 1) { v -> Engine.nMoveSelection(0f, v - b.b); ed.commit("Move") }
                }
                Spacer(Modifier.height(4.dp))
                Dim(trf("Size: %.1f x %.1f pt", b.w, b.h).replace(" x ", " × "))
            }

            if (s.canEditText && !s.isText) {
                HorizontalDivider(color = C.Line, modifier = Modifier.padding(vertical = 10.dp))
                FilledTonalButton(onClick = { ed.beginEditText() }, modifier = Modifier.fillMaxWidth()) { Text(tr("Edit text")) }
            }
            if (s.isText) {
                HorizontalDivider(color = C.Line, modifier = Modifier.padding(vertical = 10.dp))
                FilledTonalButton(onClick = { ed.beginEditText() }, modifier = Modifier.fillMaxWidth()) { Text(tr("Edit text")) }
                Spacer(Modifier.height(10.dp))
                if (s.font.isNotEmpty())
                    Text(s.font + if (s.embedded) "" else "  (" + tr("not embedded") + ")", color = C.TextDim, fontSize = 13.sp,
                        maxLines = 2, overflow = TextOverflow.Ellipsis)
                Spacer(Modifier.height(6.dp))
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(tr("Size"), fontSize = 14.sp, modifier = Modifier.width(72.dp))
                    NumberField(s.size, "pt", Modifier.weight(1f), 1) { v -> if (v > 0.5f) ed.setTextSize(v) }
                }
                Spacer(Modifier.height(8.dp))
                OutlinedButton(onClick = { ed.dialog = Dlg.Fonts }, modifier = Modifier.fillMaxWidth()) { Text(tr("Change font")) }
                Spacer(Modifier.height(10.dp))
                Text(tr("Colour"), fontSize = 14.sp)
                Spacer(Modifier.height(6.dp))
                ColorRow(s.color) { ed.setTextColor(it) }
            }

            // Thickness and colour for one line, several lines or a whole table grid.
            LineThickness(ed)
            val lines = remember(ed.tick, s) { ed.lineInfo().second }
            if (lines > 0 && (s.multi || s.lineColor)) {
                Spacer(Modifier.height(10.dp))
                Text(tr("Line colour"), fontSize = 14.sp)
                Spacer(Modifier.height(6.dp))
                ColorRow(if (s.lineColor) s.color else -1) { ed.setLineColor(it) }
            }
            if (s.isPath && s.parts > 1) {
                Spacer(Modifier.height(10.dp))
                OutlinedButton(onClick = { ed.split() }, modifier = Modifier.fillMaxWidth()) {
                    Text(trf("Split into %d separate lines", s.parts))
                }
            }

            // Resizing a table: keep the gaps between its lines.
            if (s.multi && s.lines > 0) {
                Spacer(Modifier.height(6.dp))
                CheckRow(tr("Keep line spacing when resizing"), ed.keepSpacing) { ed.keepSpacing = it; ed.prefs.keepSpacing = it }
                Dim(tr("Only the outer row / column changes size"))
            }
            // Moving text: guide lines and snapping to other text.
            if (s.canEditText || s.texts > 0) {
                HorizontalDivider(color = C.Line, modifier = Modifier.padding(vertical = 10.dp))
                CheckRow(tr("Show alignment lines when moving"), ed.alignGuides) { ed.alignGuides = it; ed.prefs.alignGuides = it }
                CheckRow(tr("Snap into line with other text"), ed.alignSnap) { ed.alignSnap = it; ed.prefs.alignSnap = it }
            }

            HorizontalDivider(color = C.Line, modifier = Modifier.padding(vertical = 10.dp))
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(onClick = { ed.copy() }) { Text(tr("Copy")) }
                OutlinedButton(onClick = { ed.cut() }) { Text(tr("Cut")) }
                if (s.multi && s.group == 0) OutlinedButton(onClick = { ed.group() }) { Text(tr("Group")) }
                if (s.group != 0 || s.type == ObjType.FORM) OutlinedButton(onClick = { ed.ungroup() }) { Text(tr("Ungroup")) }
                if (ed.pageCount > 1) OutlinedButton(onClick = { ed.dialog = Dlg.MoveToPage }) { Text(tr("Move to page...")) }
                OutlinedButton(onClick = { ed.delete() }) { Text(tr("Delete"), color = C.Bad) }
            }
        }
        Spacer(Modifier.height(8.dp))
        OutlinedButton(onClick = {
            val p = ed.currentPage
            val sz = ed.pageSizes.getOrNull(p) ?: (595f to 842f)
            ed.paste(p, sz.first / 2, sz.second / 2, false)
        }) { Text(tr("Paste")) }

        Spacer(Modifier.height(18.dp))
        SectionTitle(tr("New text & shapes"))
        CheckRow(tr("New text matches the PDF's font"), ed.matchFont) { ed.matchFont = it; ed.prefs.matchFont = it }
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(tr("Text size"), fontSize = 14.sp, modifier = Modifier.weight(1f))
            NumberField(ed.newTextSize, "pt", Modifier.width(120.dp), 1) { v -> ed.newTextSize = v.coerceIn(1f, 400f) }
        }
        Spacer(Modifier.height(8.dp))
        Text(tr("Text colour"), fontSize = 14.sp)
        Spacer(Modifier.height(6.dp))
        ColorRow(ed.newTextColor) { ed.newTextColor = it }
        Spacer(Modifier.height(12.dp))
        CheckRow(tr("Filled"), ed.rectFilled) { ed.rectFilled = it }
        if (ed.rectFilled) {
            ColorRow(ed.fillColor) { ed.fillColor = it }
            Spacer(Modifier.height(8.dp))
        }
        CheckRow(tr("Outline"), ed.rectStroked) { ed.rectStroked = it }
        if (ed.rectStroked || !ed.rectFilled) {
            ColorRow(ed.strokeColor) { ed.strokeColor = it }
            Spacer(Modifier.height(8.dp))
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(tr("Line width"), fontSize = 14.sp, modifier = Modifier.weight(1f))
                NumberField(ed.lineWidth, "pt", Modifier.width(120.dp)) { v -> ed.lineWidth = v.coerceIn(0.1f, 50f) }
            }
        }
    }
}

// ------------------------------------------------------------------ pages

@Composable
private fun Thumb(ed: Editor, page: Int, widthPx: Int) {
    val size = ed.pageSizes.getOrNull(page) ?: return
    val version = remember(ed.tick, page) { Engine.nPageVersion(page) }
    val bmp by produceState<Bitmap?>(null, page, version, ed.docId, widthPx) {
        value = withContext(Dispatchers.Default) {
            val w = widthPx.coerceIn(32, 400)
            val s = w / size.first
            val h = (size.second * s).roundToInt().coerceAtLeast(16)
            val bm = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
            if (Engine.nRender(page, s, 0, 0, w, h, bm)) bm else null
        }
    }
    Box(Modifier.fillMaxWidth().aspectRatio(size.first / size.second).background(Color.White)) {
        bmp?.let { Image(it.asImageBitmap(), null, Modifier.fillMaxSize()) }
    }
}

@Composable
fun PagesPanel(ed: Editor, vs: ViewState, columns: Int, modifier: Modifier = Modifier, onPicked: () -> Unit = {}) {
    Column(modifier.fillMaxWidth()) {
        Text(tr("Pages"), style = MaterialTheme.typography.titleMedium, modifier = Modifier.padding(16.dp, 12.dp, 16.dp, 8.dp))
        LazyVerticalGrid(
            columns = GridCells.Fixed(columns),
            contentPadding = PaddingValues(12.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
            modifier = Modifier.fillMaxWidth().weight(1f, fill = false),
        ) {
            itemsIndexed(ed.pageSizes) { i, _ ->
                var menu by remember { mutableStateOf(false) }
                val current = i == ed.currentPage
                Column(
                    Modifier.clip(RoundedCornerShape(10.dp))
                        .background(if (current) C.Raised else Color.Transparent)
                        .border(if (current) 2.dp else 1.dp, if (current) C.Accent else C.Line, RoundedCornerShape(10.dp))
                        .clickable { ed.scrollToPage = i; ed.currentPage = i; onPicked() }
                        .padding(8.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    Thumb(ed, i, 240)
                    Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
                        Text(trf("Page %d", i + 1), fontSize = 12.sp, color = C.TextDim, modifier = Modifier.weight(1f).padding(start = 2.dp))
                        Box {
                            Text("•••", color = C.TextDim, fontSize = 14.sp,
                                modifier = Modifier.clip(RoundedCornerShape(6.dp)).clickable { menu = true }.padding(horizontal = 8.dp, vertical = 4.dp))
                            DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                                @Composable fun item(label: String, action: () -> Unit) =
                                    DropdownMenuItem(text = { Text(label) }, onClick = { menu = false; action() })
                                item(tr("Insert page after")) { ed.insertPageAfter(i) }
                                item(tr("Duplicate page")) { ed.duplicatePage(i) }
                                item(tr("Rotate right")) { ed.rotatePage(i, 1) }
                                item(tr("Rotate left")) { ed.rotatePage(i, 3) }
                                if (i > 0) item(tr("Move up")) { ed.movePage(i, i - 1) }
                                if (i < ed.pageCount - 1) item(tr("Move down")) { ed.movePage(i, i + 1) }
                                item(tr("Paste")) {
                                    val sz = ed.pageSizes[i]
                                    ed.paste(i, sz.first / 2, sz.second / 2, false)
                                }
                                item(tr("Select all on this page")) { ed.selectAll(i) }
                                item(tr("Make this page's annotations editable")) { ed.flattenPage(i) }
                                if (ed.pageCount > 1) item(tr("Delete page")) { ed.deletePage(i) }
                            }
                        }
                    }
                }
            }
        }
        // Side column (narrow): buttons stacked; sheet: side by side.
        @Composable fun AddBtn(m: Modifier) = FilledTonalButton(onClick = { ed.insertPageAfter(ed.pageCount - 1) }, modifier = m) {
            Text(tr("Add page"), maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        @Composable fun ImportBtn(m: Modifier) = FilledTonalButton(onClick = { ed.dialog = Dlg.Import }, modifier = m) {
            Text(tr("Import pages..."), maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        if (columns == 1) {
            Column(Modifier.fillMaxWidth().padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                AddBtn(Modifier.fillMaxWidth())
                ImportBtn(Modifier.fillMaxWidth())
            }
        } else {
            Row(Modifier.fillMaxWidth().padding(12.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                AddBtn(Modifier.weight(1f))
                ImportBtn(Modifier.weight(1f))
            }
        }
    }
}
