package com.theocp.pdfeditor

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties

/** A dialog card that fits any screen: max 560 dp wide, scrolls when tall. */
@Composable
fun Card(
    title: String,
    onDismiss: () -> Unit,
    wide: Boolean = false,
    scroll: Boolean = true,
    buttons: @Composable () -> Unit = {},
    content: @Composable ColumnScope.() -> Unit,
) {
    Dialog(onDismissRequest = onDismiss, properties = DialogProperties(usePlatformDefaultWidth = false)) {
        BoxWithConstraints(Modifier.imePadding().padding(16.dp), contentAlignment = Alignment.Center) {
            Surface(
                color = C.Panel, shape = RoundedCornerShape(18.dp),
                modifier = Modifier.widthIn(max = if (wide) 640.dp else 520.dp).fillMaxWidth()
                    .heightIn(max = maxHeight).border(1.dp, C.Line, RoundedCornerShape(18.dp)),
            ) {
                Column(Modifier.padding(top = 20.dp, bottom = 12.dp)) {
                    Text(title, style = MaterialTheme.typography.titleLarge, modifier = Modifier.padding(horizontal = 22.dp))
                    Spacer(Modifier.height(14.dp))
                    val body = Modifier.padding(horizontal = 22.dp).weight(1f, fill = false)
                    Column(if (scroll) body.verticalScroll(rememberScrollState()) else body, content = content)
                    Spacer(Modifier.height(8.dp))
                    FlowRow(Modifier.fillMaxWidth().padding(horizontal = 14.dp), buttons)
                }
            }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun FlowRow(modifier: Modifier, content: @Composable () -> Unit) {
    androidx.compose.foundation.layout.FlowRow(
        modifier, horizontalArrangement = Arrangement.spacedBy(4.dp, Alignment.End),
        verticalArrangement = Arrangement.spacedBy(4.dp),
    ) { content() }
}

@Composable
private fun Primary(text: String, enabled: Boolean = true, onClick: () -> Unit) =
    Button(onClick = onClick, enabled = enabled, shape = RoundedCornerShape(10.dp),
        colors = ButtonDefaults.buttonColors(containerColor = C.Accent, contentColor = Color.White)) { Text(text) }

@Composable
private fun Plain(text: String, color: Color = C.Text, onClick: () -> Unit) =
    TextButton(onClick = onClick) { Text(text, color = color) }

@Composable
fun Dialogs(ed: Editor) {
    val close = { ed.dialog = null }
    when (val d = ed.dialog) {
        null -> {}
        is Dlg.EditText -> TextDialog(tr("Edit text"), d.text, tr("Apply"), close) { ed.commitText(it) }
        is Dlg.NewText -> TextDialog(tr("Add text"), "", tr("Add"), close) { if (it.isNotBlank()) ed.addText(d.page, d.x, d.yTop, it) }
        Dlg.Table -> TableDialog(ed, close)
        Dlg.Meta -> MetaDialog(ed, close)
        Dlg.Import -> ImportDialog(ed, close)
        is Dlg.Extract -> ExtractDialog(d.text, close)
        is Dlg.Password -> PasswordDialog(ed, d, close)
        Dlg.Settings -> SettingsDialog(ed, close)
        Dlg.Help -> HelpDialog(close)
        Dlg.About -> AboutDialog(ed, close)
        Dlg.Forms -> FormsDialog(ed, close)
        Dlg.Fonts -> FontsDialog(ed, close)
        Dlg.MoveToPage -> MoveToPageDialog(ed, close)
        is Dlg.Confirm -> Card(tr("Unsaved changes"), close, buttons = {
            Plain(tr("Cancel"), onClick = close)
            Plain(tr("Don't save"), C.Bad) { close(); d.then() }
            Primary(tr("Save")) { close(); ed.save(d.then) }
        }) { Text(tr("Save the changes first?")) }
    }
}

@Composable
private fun TextDialog(title: String, initial: String, okText: String, close: () -> Unit, onOk: (String) -> Unit) {
    var value by remember { mutableStateOf(TextFieldValue(initial, TextRange(initial.length))) }
    val focus = remember { FocusRequester() }
    LaunchedEffect(Unit) { focus.requestFocus() }
    Card(title, close, buttons = {
        Plain(tr("Cancel"), onClick = close)
        Primary(okText) { close(); onOk(value.text) }
    }) {
        OutlinedTextField(value, { value = it }, modifier = Modifier.fillMaxWidth().focusRequester(focus), minLines = 1, maxLines = 6)
    }
}

@Composable
private fun IntField(label: String, value: Int, range: IntRange, modifier: Modifier = Modifier, onChange: (Int) -> Unit) {
    var text by remember { mutableStateOf(value.toString()) }
    OutlinedTextField(text, {
        text = it.filter(Char::isDigit).take(4)
        text.toIntOrNull()?.let { v -> onChange(v.coerceIn(range)) }
    }, label = { Text(label) }, singleLine = true, modifier = modifier,
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number))
}

@Composable
private fun FloatField(label: String, value: Float, modifier: Modifier = Modifier, onChange: (Float) -> Unit) {
    var text by remember { mutableStateOf(value.toString().removeSuffix(".0")) }
    OutlinedTextField(text, {
        text = it
        it.replace(',', '.').toFloatOrNull()?.let(onChange)
    }, label = { Text(label) }, singleLine = true, modifier = modifier, suffix = { Text("pt", color = C.TextDim) },
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal))
}

@Composable
private fun TableDialog(ed: Editor, close: () -> Unit) {
    var cols by remember { mutableIntStateOf(4) }
    var rows by remember { mutableIntStateOf(8) }
    var header by remember { mutableStateOf(true) }
    var headerH by remember { mutableStateOf(24f) }
    var margin by remember { mutableStateOf(40f) }
    var lineW by remember { mutableStateOf(0.5f) }
    Card(tr("Table / form"), close, buttons = {
        Plain(tr("Cancel"), onClick = close)
        Primary(trf("Add to page %d", ed.currentPage + 1)) {
            close(); ed.addTable(ed.currentPage, cols, rows, header, headerH, margin, lineW)
        }
    }) {
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            IntField(tr("Columns"), cols, 1..40, Modifier.weight(1f)) { cols = it }
            IntField(tr("Rows"), rows, 1..200, Modifier.weight(1f)) { rows = it }
        }
        Spacer(Modifier.height(6.dp))
        CheckRow(tr("Header row"), header) { header = it }
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            if (header) FloatField(tr("Header height"), headerH, Modifier.weight(1f)) { headerH = it }
            FloatField(tr("Margin"), margin, Modifier.weight(1f)) { margin = it }
        }
        Spacer(Modifier.height(6.dp))
        FloatField(tr("Line width"), lineW, Modifier.fillMaxWidth()) { lineW = it }
        Spacer(Modifier.height(8.dp))
        Dim(tr("Every line of the table stays a separate, editable line."))
    }
}

@Composable
private fun MetaDialog(ed: Editor, close: () -> Unit) {
    val m = remember { ed.meta() }
    Card(tr("Properties / metadata"), close, buttons = { Primary(tr("Close"), onClick = close) }) {
        @Composable fun row(k: String, v: String) {
            Text(k, color = C.TextDim, fontSize = 12.sp)
            SelectionContainer { Text(v.ifEmpty { "-" }, fontSize = 15.sp) }
            Spacer(Modifier.height(10.dp))
        }
        row(tr("File"), ed.name)
        row(tr("Pages"), m.optInt("pages").toString())
        for (k in listOf("Title", "Author", "Subject", "Keywords", "Creator", "Producer")) row(tr(k), m.optString(k))
        if (m.optInt("signatures") > 0) Text(trf("Digital signatures: %d", m.optInt("signatures")), color = C.Bad)
    }
}

@Composable
private fun ImportDialog(ed: Editor, close: () -> Unit) {
    var range by remember { mutableStateOf("") }
    var password by remember { mutableStateOf("") }
    val f = ed.importFile
    Card(tr("Import pages"), { ed.importFile = null; close() }, buttons = {
        Plain(tr("Cancel")) { ed.importFile = null; close() }
        Primary(tr("Import"), enabled = f != null) {
            close(); ed.importPages(f!!.second, password, range); ed.importFile = null
        }
    }) {
        OutlinedButton(onClick = { ed.host.pickImportPdf() }, modifier = Modifier.fillMaxWidth()) {
            Text(f?.first ?: tr("Choose PDF"), maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        Spacer(Modifier.height(10.dp))
        OutlinedTextField(range, { range = it }, label = { Text(tr("Page range (empty = all)")) }, placeholder = { Text("1-3, 5") },
            singleLine = true, modifier = Modifier.fillMaxWidth())
        Spacer(Modifier.height(6.dp))
        OutlinedTextField(password, { password = it }, label = { Text(tr("Password")) }, singleLine = true,
            visualTransformation = PasswordVisualTransformation(), modifier = Modifier.fillMaxWidth())
        Spacer(Modifier.height(6.dp))
        Dim(tr("The pages are added at the end."))
    }
}

@Composable
private fun ExtractDialog(text: String, close: () -> Unit) {
    val ctx = LocalContext.current
    Card(tr("All text"), close, wide = true, buttons = {
        Plain(tr("Close"), onClick = close)
        Primary(tr("Copy text")) {
            (ctx.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager).setPrimaryClip(ClipData.newPlainText("PDF", text))
        }
    }) {
        SelectionContainer {
            Text(text.ifEmpty { tr("This PDF has no text (it may be scanned images).") }, fontFamily = FontFamily.Monospace, fontSize = 12.sp)
        }
    }
}

@Composable
private fun PasswordDialog(ed: Editor, d: Dlg.Password, close: () -> Unit) {
    var pw by remember { mutableStateOf("") }
    val focus = remember { FocusRequester() }
    LaunchedEffect(Unit) { focus.requestFocus() }
    Card(tr("Password"), close, buttons = {
        Plain(tr("Cancel"), onClick = close)
        Primary(tr("Open")) { close(); ed.open(d.bytes, d.name, d.uri, d.writable, pw) }
    }) {
        Text(d.name, color = C.TextDim, maxLines = 1, overflow = TextOverflow.Ellipsis)
        Spacer(Modifier.height(8.dp))
        if (d.wrong) { Text(tr("Wrong password, try again."), color = C.Bad); Spacer(Modifier.height(6.dp)) }
        OutlinedTextField(pw, { pw = it }, singleLine = true, visualTransformation = PasswordVisualTransformation(),
            modifier = Modifier.fillMaxWidth().focusRequester(focus))
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun Choice(options: List<Pair<String, String>>, selected: String, onPick: (String) -> Unit) {
    androidx.compose.foundation.layout.FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        for ((key, label) in options) {
            val on = key == selected
            Text(label, color = if (on) Color.White else C.Text, fontSize = 14.sp,
                modifier = Modifier.clip(RoundedCornerShape(10.dp)).background(if (on) C.Accent else C.Control)
                    .clickable { onPick(key) }.padding(horizontal = 16.dp, vertical = 10.dp))
        }
    }
}

@Composable
private fun SettingsDialog(ed: Editor, close: () -> Unit) {
    Card(tr("Settings"), close, buttons = { Primary(tr("Done"), onClick = close) }) {
        SectionTitle(tr("Language"))
        Choice(listOf("" to tr("System"), "en" to "English", "de" to "Deutsch"), ed.language) { ed.applyLanguage(it) }
        Spacer(Modifier.height(18.dp))
        SectionTitle(tr("Interface size"))
        Choice(listOf("0.85" to "85 %", "1.0" to "100 %", "1.15" to "115 %", "1.3" to "130 %"), ed.uiScale.toString()) {
            ed.applyUiScale(it.toFloat())
        }
        Spacer(Modifier.height(18.dp))
        SectionTitle(tr("Editing"))
        CheckRow(tr("New text matches the PDF's font"), ed.matchFont) { ed.matchFont = it; ed.prefs.matchFont = it }
    }
}

@Composable
private fun HelpDialog(close: () -> Unit) {
    Card(tr("Help"), close, wide = true, buttons = { Primary(tr("Close"), onClick = close) }) {
        SectionTitle(tr("Gestures"))
        val gestures = listOf(
            "Tap" to "select a line, text or picture",
            "Double-tap text" to "type into it (font and size stay the same)",
            "Drag the selection" to "move it",
            "Drag a handle" to "resize it",
            "Pinch" to "zoom in and out",
            "Drag the page" to "scroll",
            "Long-press, then drag" to "select everything in the box",
            "Long-press an object" to "copy, cut, delete and more",
        )
        for ((a, b) in gestures) {
            Row(Modifier.padding(vertical = 5.dp)) {
                Text(tr(a), fontWeight = FontWeight.SemiBold, fontSize = 14.sp, modifier = Modifier.weight(0.45f))
                Text(tr(b), color = C.TextDim, fontSize = 14.sp, modifier = Modifier.weight(0.55f))
            }
        }
        HorizontalDivider(color = C.Line, modifier = Modifier.padding(vertical = 12.dp))
        SectionTitle(tr("Tools"))
        Text(tr("Tap a tool, then tap or drag on the page."), fontSize = 14.sp)
        Spacer(Modifier.height(8.dp))
        val tools = listOf(
            "Text" to "Edit or add text",
            "Image" to "Place a picture",
            "Rectangle" to "Draw a rectangle",
            "Cut piece" to "Lift part of a picture out",
            "Redact" to "Remove content permanently",
        )
        for ((a, b) in tools) {
            Row(Modifier.padding(vertical = 5.dp)) {
                Text(tr(a), fontWeight = FontWeight.SemiBold, fontSize = 14.sp, modifier = Modifier.weight(0.45f))
                Text(tr(b), color = C.TextDim, fontSize = 14.sp, modifier = Modifier.weight(0.55f))
            }
        }
    }
}

@Composable
private fun AboutDialog(ed: Editor, close: () -> Unit) {
    Card(tr("About"), close, buttons = { Primary(tr("Close"), onClick = close) }) {
        Text(tr("PDF Editor"), style = MaterialTheme.typography.titleMedium)
        Text(trf("Version %s", ed.versionName), color = C.TextDim)
        Spacer(Modifier.height(10.dp))
        Text(tr("by TheOCProductions & ShortGG"))
        Spacer(Modifier.height(10.dp))
        Dim("PDFium (BSD-3-Clause)")
    }
}

@Composable
private fun FormsDialog(ed: Editor, close: () -> Unit) {
    val fields = remember(ed.tick) { ed.formFields().filter { it.name.isNotEmpty() } }
    Card(tr("Form fields"), close, wide = true, scroll = false, buttons = { Primary(tr("Done"), onClick = close) }) {
        if (fields.isEmpty()) { Text(tr("No form fields in this PDF.")); return@Card }
        LazyColumn(Modifier.heightIn(max = 520.dp)) {
            itemsIndexed(fields) { _, f ->
                var v by remember(f.page, f.name, f.value) { mutableStateOf(f.value) }
                Column(Modifier.padding(vertical = 6.dp)) {
                    Text(f.name + "  ·  " + trf("Page %d", f.page + 1), color = C.TextDim, fontSize = 12.sp,
                        maxLines = 1, overflow = TextOverflow.Ellipsis)
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        OutlinedTextField(v, { v = it }, singleLine = true, modifier = Modifier.weight(1f))
                        Spacer(Modifier.width(8.dp))
                        TextButton(onClick = { ed.setFormValue(f, v) }, enabled = v != f.value) { Text(tr("Apply")) }
                    }
                }
            }
        }
    }
}

@Composable
private fun FontsDialog(ed: Editor, close: () -> Unit) {
    val fonts = remember { ed.fonts() }
    val size = ed.sel.size
    Card(tr("Change font"), close, wide = true, scroll = false, buttons = { Plain(tr("Cancel"), onClick = close) }) {
        LazyColumn(Modifier.heightIn(max = 560.dp)) {
            itemsIndexed(fonts) { i, f ->
                val heading = when (f.kind) {
                    "doc", "docref" -> tr("Font of this PDF")
                    "std" -> tr("Standard PDF font")
                    else -> tr("Font on this phone")
                }
                val showHeading = i == 0 || heading != when (fonts[i - 1].kind) {
                    "doc", "docref" -> tr("Font of this PDF")
                    "std" -> tr("Standard PDF font")
                    else -> tr("Font on this phone")
                }
                if (showHeading) SectionTitle(heading, Modifier.padding(top = 10.dp))
                Text(f.label, fontSize = 15.sp, maxLines = 1, overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.fillMaxWidth().clip(RoundedCornerShape(8.dp))
                        .clickable { close(); ed.restyle(i, size) }.padding(horizontal = 10.dp, vertical = 12.dp))
            }
        }
    }
}

@Composable
private fun MoveToPageDialog(ed: Editor, close: () -> Unit) {
    var target by remember { mutableIntStateOf(if (ed.sel.page + 1 < ed.pageCount) ed.sel.page + 2 else 1) }
    Card(tr("Move to page"), close, buttons = {
        Plain(tr("Cancel"), onClick = close)
        Primary(tr("Move")) { close(); ed.moveToPage(target - 1) }
    }) {
        IntField(trf("Page (1 - %d)", ed.pageCount), target, 1..ed.pageCount, Modifier.fillMaxWidth()) { target = it }
    }
}
