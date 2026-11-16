package com.theocp.pdfeditor

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.material3.SnackbarDuration
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.snapshotFlow
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.launch
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.ui.draw.clip
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.Redo
import androidx.compose.material.icons.automirrored.outlined.Undo
import androidx.compose.material.icons.outlined.Close
import androidx.compose.material.icons.outlined.ContentCopy
import androidx.compose.material.icons.outlined.ContentCut
import androidx.compose.material.icons.outlined.Crop
import androidx.compose.material.icons.outlined.CropSquare
import androidx.compose.material.icons.outlined.Delete
import androidx.compose.material.icons.outlined.Description
import androidx.compose.material.icons.outlined.Edit
import androidx.compose.material.icons.outlined.FolderOpen
import androidx.compose.material.icons.outlined.GridOn
import androidx.compose.material.icons.outlined.Image
import androidx.compose.material.icons.outlined.KeyboardArrowDown
import androidx.compose.material.icons.outlined.KeyboardArrowUp
import androidx.compose.material.icons.outlined.MoreVert
import androidx.compose.material.icons.outlined.NoteAdd
import androidx.compose.material.icons.outlined.PanTool
import androidx.compose.material.icons.outlined.Save
import androidx.compose.material.icons.outlined.Search
import androidx.compose.material.icons.filled.Square
import androidx.compose.material.icons.outlined.TextFields
import androidx.compose.material.icons.outlined.TouchApp
import androidx.compose.material.icons.outlined.Tune
import androidx.compose.material.icons.outlined.ViewAgenda
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.IconButtonDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Snackbar
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.material3.VerticalDivider
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

// ------------------------------------------------------------------ start

@Composable
fun StartScreen(ed: Editor, recent: List<Pair<String, String>>, openRecent: (String) -> Unit) {
    Box(Modifier.fillMaxSize().background(C.CanvasBg), contentAlignment = Alignment.Center) {
        Column(
            Modifier.widthIn(max = 460.dp).fillMaxWidth().padding(20.dp)
                .background(C.Panel, RoundedCornerShape(18.dp))
                .border(1.dp, C.Line, RoundedCornerShape(18.dp))
                .padding(horizontal = 24.dp, vertical = 32.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Icon(Icons.Outlined.Description, null, tint = C.Accent, modifier = Modifier.size(52.dp))
            Spacer(Modifier.height(12.dp))
            Text(tr("PDF Editor"), style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.SemiBold)
            Text(tr("by TheOCProductions & ShortGG"), color = C.TextDim, style = MaterialTheme.typography.bodyMedium)
            Spacer(Modifier.height(28.dp))
            Button(onClick = { ed.host.pickPdf() }, modifier = Modifier.fillMaxWidth().height(52.dp),
                shape = RoundedCornerShape(12.dp)) {
                Icon(Icons.Outlined.FolderOpen, null)
                Spacer(Modifier.width(10.dp))
                Text(tr("Open PDF"), fontSize = 16.sp)
            }
            Spacer(Modifier.height(10.dp))
            FilledTonalButton(onClick = { ed.newDocument() }, modifier = Modifier.fillMaxWidth().height(52.dp),
                shape = RoundedCornerShape(12.dp)) {
                Icon(Icons.Outlined.NoteAdd, null)
                Spacer(Modifier.width(10.dp))
                Text(tr("New document"), fontSize = 16.sp)
            }
            if (recent.isNotEmpty()) {
                Spacer(Modifier.height(24.dp))
                Text(tr("Recent: tap to open"), color = C.TextDim, style = MaterialTheme.typography.labelMedium,
                    modifier = Modifier.fillMaxWidth())
                Spacer(Modifier.height(6.dp))
                for ((uri, name) in recent.take(5)) {
                    TextButton(onClick = { openRecent(uri) }, modifier = Modifier.fillMaxWidth()) {
                        Icon(Icons.Outlined.Description, null, tint = C.TextDim, modifier = Modifier.size(18.dp))
                        Spacer(Modifier.width(10.dp))
                        Text(name, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f), color = C.Text)
                    }
                }
            }
        }
    }
}

// ------------------------------------------------------------------ editor

private data class ToolDef(val tool: Tool, val icon: ImageVector, val name: String, val hint: String)

private val toolDefs = listOf(
    ToolDef(Tool.Select, Icons.Outlined.TouchApp, "Select", "Select, move and resize"),
    ToolDef(Tool.Text, Icons.Outlined.TextFields, "Text", "Edit or add text"),
    ToolDef(Tool.Image, Icons.Outlined.Image, "Image", "Place a picture"),
    ToolDef(Tool.Rect, Icons.Outlined.CropSquare, "Rectangle", "Draw a rectangle"),
    ToolDef(Tool.Cut, Icons.Outlined.Crop, "Cut piece", "Lift part of a picture out"),
    ToolDef(Tool.Redact, Icons.Filled.Square, "Redact", "Remove content permanently"),
    ToolDef(Tool.Pan, Icons.Outlined.PanTool, "Pan", "Scroll the page"),
)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun EditorScreen(ed: Editor, vs: ViewState) {
    val snack = remember { SnackbarHostState() }
    val scope = rememberCoroutineScope()
    // Each message replaces the one on screen (collecting must not restart when it's cleared).
    LaunchedEffect(Unit) {
        snapshotFlow { ed.message }.filterNotNull().collect { m ->
            ed.message = null
            snack.currentSnackbarData?.dismiss()
            scope.launch { snack.showSnackbar(m.first, withDismissAction = m.second, duration = SnackbarDuration.Short) }
        }
    }
    BoxWithConstraints(Modifier.fillMaxSize()) {
        val compact = maxWidth < 600.dp
        val expanded = maxWidth >= 900.dp
        Scaffold(
            containerColor = C.Bg,
            topBar = { EditorTopBar(ed, compact) },
            bottomBar = { if (compact) BottomTools(ed) },
            snackbarHost = {
                SnackbarHost(snack, Modifier.padding(bottom = if (compact && ed.sel.has) 64.dp else 0.dp)) { data ->
                    Snackbar(data, containerColor = C.Raised, contentColor = C.Text, actionColor = C.Accent,
                        dismissActionContentColor = C.TextDim, shape = RoundedCornerShape(12.dp))
                }
            },
        ) { pad ->
            Row(Modifier.padding(pad).fillMaxSize()) {
                if (!compact) {
                    ToolRail(ed)
                    VerticalDivider(color = C.Line)
                }
                if (expanded) {
                    Surface(color = C.Panel, modifier = Modifier.width(190.dp).fillMaxHeight()) { PagesPanel(ed, vs, columns = 1) }
                    VerticalDivider(color = C.Line)
                }
                Box(Modifier.weight(1f).fillMaxHeight()) {
                    PageView(ed, vs, Modifier.fillMaxSize())
                    if (ed.searching) SearchBar(ed, Modifier.align(Alignment.TopCenter))
                    if (compact && ed.sel.has) SelectionBar(ed, Modifier.align(Alignment.BottomCenter))
                    PageBadge(ed, vs, Modifier.align(Alignment.TopEnd).padding(top = if (ed.searching) 64.dp else 8.dp, end = 8.dp))
                }
                if (!compact) {
                    VerticalDivider(color = C.Line)
                    Surface(color = C.Panel, modifier = Modifier.width(if (expanded) 340.dp else 300.dp).fillMaxHeight()) {
                        PropertiesPanel(ed)
                    }
                }
            }
        }
        LaunchedEffect(ed.showProps, compact) { if (compact && ed.showProps && ed.sel.has) ed.revealSel = true }
        if (compact && ed.showProps) {
            // Light scrim: the page stays visible while the thickness / colour changes.
            ModalBottomSheet(onDismissRequest = { ed.showProps = false }, containerColor = C.Panel,
                scrimColor = Color(0x22000000),
                sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = false)) {
                PropertiesPanel(ed, Modifier.padding(bottom = 24.dp))
            }
        }
        if (!expanded && ed.showPages) {
            ModalBottomSheet(onDismissRequest = { ed.showPages = false }, containerColor = C.Panel,
                sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)) {
                PagesPanel(ed, vs, columns = if (compact) 3 else 4, Modifier.fillMaxHeight(0.85f)) { ed.showPages = false }
            }
        }
    }
    Dialogs(ed)
}

/** "Page x of y" - shown while scrolling, then fades out so it never covers the page. */
@Composable
private fun PageBadge(ed: Editor, vs: ViewState, modifier: Modifier) {
    if (ed.pageCount <= 0) return
    var visible by remember { mutableStateOf(true) }
    LaunchedEffect((vs.scrollY / 40).toInt(), ed.currentPage, ed.pageCount) {
        visible = true
        delay(1600)
        visible = false
    }
    AnimatedVisibility(visible, modifier, enter = fadeIn(), exit = fadeOut()) { BadgeText(ed) }
}

@Composable
private fun BadgeText(ed: Editor, modifier: Modifier = Modifier) {
    Text(trf("Page %d of %d", ed.currentPage + 1, ed.pageCount), color = Color.White, fontSize = 12.sp,
        modifier = modifier.clip(RoundedCornerShape(10.dp)).background(Color(0x99000000)).clickable { ed.showPages = true }
            .padding(horizontal = 10.dp, vertical = 5.dp))
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun EditorTopBar(ed: Editor, compact: Boolean) {
    var menu by remember { mutableStateOf(false) }
    TopAppBar(
        colors = TopAppBarDefaults.topAppBarColors(containerColor = C.Panel, titleContentColor = C.Text),
        navigationIcon = {
            IconButton(onClick = { ed.guardUnsaved { ed.close() } }) { Icon(Icons.Outlined.Close, tr("Close document")) }
        },
        title = {
            Column {
                Text(ed.name, maxLines = 1, overflow = TextOverflow.Ellipsis, fontSize = 16.sp)
                if (ed.modified) Text(tr("(unsaved)"), color = C.TextDim, fontSize = 11.sp, maxLines = 1)
            }
        },
        actions = {
            IconButton(onClick = { ed.undo() }, enabled = ed.canUndo) { Icon(Icons.AutoMirrored.Outlined.Undo, tr("Undo")) }
            IconButton(onClick = { ed.redo() }, enabled = ed.canRedo) { Icon(Icons.AutoMirrored.Outlined.Redo, tr("Redo")) }
            if (!compact) IconButton(onClick = { ed.searching = !ed.searching; if (!ed.searching) ed.hits = emptyList() }) {
                Icon(Icons.Outlined.Search, tr("Search"))
            }
            IconButton(onClick = { ed.save() }) { Icon(Icons.Outlined.Save, tr("Save")) }
            Box {
                IconButton(onClick = { menu = true }) { Icon(Icons.Outlined.MoreVert, tr("More")) }
                DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                    @Composable fun item(label: String, action: () -> Unit) =
                        DropdownMenuItem(text = { Text(label) }, onClick = { menu = false; action() })
                    if (compact) {
                        item(tr("Search")) { ed.searching = true }
                        item(tr("Text & shape style")) { ed.showProps = true }
                    }
                    item(tr("Save a copy...")) { ed.host.saveAs() }
                    item(tr("Print")) { ed.host.print() }
                    item(tr("Share")) { ed.host.share() }
                    HorizontalDivider(color = C.Line)
                    item(tr("Pages")) { ed.showPages = true }
                    item(tr("Table / form...")) { ed.dialog = Dlg.Table }
                    item(tr("Import pages...")) { ed.dialog = Dlg.Import }
                    item(tr("Export pages as PNG...")) { ed.host.exportPages() }
                    item(tr("Extract all text...")) { ed.dialog = Dlg.Extract(Engine.nExtractText()) }
                    item(tr("Form fields")) { ed.dialog = Dlg.Forms }
                    item(tr("Flatten all forms & annotations")) { ed.flattenAll() }
                    item(tr("Properties / metadata")) { ed.dialog = Dlg.Meta }
                    HorizontalDivider(color = C.Line)
                    item(tr("Settings")) { ed.dialog = Dlg.Settings }
                    item(tr("Help")) { ed.dialog = Dlg.Help }
                    item(tr("About")) { ed.dialog = Dlg.About }
                }
            }
        },
    )
}

/** Phone: the tools along the bottom, spread evenly (scrolls only if they don't fit). */
@Composable
private fun BottomTools(ed: Editor) {
    val tools = toolDefs.filter { it.tool != Tool.Pan }     // dragging an empty spot scrolls anyway
    Surface(color = C.Panel) {
        Column {
            HorizontalDivider(color = C.Line)
            BoxWithConstraints(Modifier.fillMaxWidth().padding(horizontal = 4.dp, vertical = 6.dp)) {
                val fits = maxWidth / tools.size >= 60.dp
                Row(if (fits) Modifier.fillMaxWidth() else Modifier.horizontalScroll(rememberScrollState()),
                    verticalAlignment = Alignment.CenterVertically) {
                    for (d in tools)
                        ToolButton(d, ed.tool == d.tool, if (fits) Modifier.weight(1f) else Modifier.width(68.dp)) {
                            ed.tool = d.tool; toolHint(ed, d.tool)
                        }
                }
            }
        }
    }
}

private fun toolHint(ed: Editor, t: Tool) {
    when (t) {
        Tool.Text -> ed.toast(tr("Tap where the text should go."))
        Tool.Image -> ed.toast(tr("Tap where the picture should go."))
        Tool.Rect, Tool.Cut, Tool.Redact -> ed.toast(tr("Drag a box on the page."))
        else -> {}
    }
}

@Composable
private fun ToolButton(d: ToolDef, active: Boolean, modifier: Modifier = Modifier.widthIn(min = 64.dp), onClick: () -> Unit) {
    val bg = if (active) C.Accent else Color.Transparent
    val fg = if (active) Color.White else C.Text
    Column(
        modifier.padding(horizontal = 2.dp).clip(RoundedCornerShape(12.dp)).background(bg)
            .clickable(onClick = onClick).padding(vertical = 8.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Icon(d.icon, null, tint = fg, modifier = Modifier.size(24.dp))
        Spacer(Modifier.height(4.dp))
        Text(tr(d.name), color = fg, fontSize = 11.sp, maxLines = 1, overflow = TextOverflow.Ellipsis,
            textAlign = TextAlign.Center, modifier = Modifier.padding(horizontal = 2.dp))
    }
}

/** Tablets / landscape: the tools in a column on the left. */
@Composable
private fun ToolRail(ed: Editor) {
    Surface(color = C.Panel, modifier = Modifier.fillMaxHeight()) {
        Column(
            Modifier.width(84.dp).verticalScroll(rememberScrollState()).padding(vertical = 8.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            for (d in toolDefs) ToolButton(d, ed.tool == d.tool) { ed.tool = d.tool; toolHint(ed, d.tool) }
            HorizontalDivider(color = C.Line, modifier = Modifier.padding(vertical = 6.dp, horizontal = 12.dp))
            ToolButton(ToolDef(Tool.Select, Icons.Outlined.ViewAgenda, "Pages", ""), false) { ed.showPages = true }
        }
    }
}

/** Quick actions for the selection (phones). */
@Composable
private fun SelectionBar(ed: Editor, modifier: Modifier) {
    val s = ed.sel
    Row(
        modifier.padding(10.dp).background(C.Raised, RoundedCornerShape(16.dp)).border(1.dp, C.Line, RoundedCornerShape(16.dp))
            .horizontalScroll(rememberScrollState()).padding(horizontal = 4.dp, vertical = 2.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        @Composable fun act(icon: ImageVector, label: String, onClick: () -> Unit) =
            IconButton(onClick = onClick) { Icon(icon, label, tint = C.Text) }
        if (s.canEditText) act(Icons.Outlined.Edit, tr("Edit text")) { ed.beginEditText() }
        if (!s.annot && (s.isText || s.texts > 0))
            IconButton(onClick = { ed.guidesAlways = !ed.guidesAlways; ed.prefs.guidesAlways = ed.guidesAlways }) {
                Icon(Icons.Outlined.GridOn, tr("Always show guide lines"), tint = if (ed.guidesAlways) C.Accent else C.Text)
            }
        act(Icons.Outlined.ContentCopy, tr("Copy")) { ed.copy() }
        act(Icons.Outlined.ContentCut, tr("Cut")) { ed.cut() }
        act(Icons.Outlined.Delete, tr("Delete")) { ed.delete() }
        act(Icons.Outlined.Tune, tr("Properties")) { ed.showProps = true }
    }
}

@Composable
private fun SearchBar(ed: Editor, modifier: Modifier) {
    Row(
        modifier.fillMaxWidth().padding(8.dp).background(C.Raised, RoundedCornerShape(14.dp))
            .border(1.dp, C.Line, RoundedCornerShape(14.dp)).padding(horizontal = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        OutlinedTextField(
            value = ed.query, onValueChange = { ed.query = it }, singleLine = true,
            placeholder = { Text(tr("Search")) },
            keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
            keyboardActions = KeyboardActions(onSearch = { ed.runSearch() }),
            modifier = Modifier.weight(1f).padding(vertical = 4.dp),
        )
        Text(if (ed.hits.isEmpty()) (if (ed.query.isNotEmpty() && ed.hitIndex == -1) "" else "") else "${ed.hitIndex + 1} / ${ed.hits.size}",
            color = C.TextDim, fontSize = 12.sp, modifier = Modifier.padding(horizontal = 6.dp))
        IconButton(onClick = { ed.stepHit(-1) }, enabled = ed.hits.isNotEmpty()) { Icon(Icons.Outlined.KeyboardArrowUp, tr("Prev")) }
        IconButton(onClick = { ed.stepHit(1) }, enabled = ed.hits.isNotEmpty()) { Icon(Icons.Outlined.KeyboardArrowDown, tr("Next")) }
        IconButton(onClick = { ed.searching = false; ed.hits = emptyList(); ed.query = "" }) { Icon(Icons.Outlined.Close, tr("Close")) }
    }
}

@Composable
fun PrimaryButton(text: String, onClick: () -> Unit, modifier: Modifier = Modifier, enabled: Boolean = true) {
    Button(onClick = onClick, enabled = enabled, modifier = modifier, shape = RoundedCornerShape(10.dp),
        colors = ButtonDefaults.buttonColors(containerColor = C.Accent, contentColor = Color.White)) { Text(text) }
}
