package com.theocp.pdfeditor

import android.graphics.Bitmap
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.calculateCentroid
import androidx.compose.foundation.gestures.calculateZoom
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.runtime.withFrameMillis
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.positionChange
import androidx.compose.ui.input.pointer.util.VelocityTracker
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.DpOffset
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.conflate
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/**
 * Scroll / zoom state and page layout of the continuous page view.
 * Content coordinates are pixels; zoom 1 fits the widest page to the width.
 */
class ViewState {
    var zoom by mutableFloatStateOf(1f)
    var scrollX by mutableFloatStateOf(0f)
    var scrollY by mutableFloatStateOf(0f)
    var viewW by mutableIntStateOf(1)
    var viewH by mutableIntStateOf(1)
    var interacting by mutableStateOf(false)
    var margin = 16f
    var gap = 16f

    var scale = 1f; private set                 // device px per PDF point
    var contentW = 1f; private set
    var contentH = 1f; private set
    var tops = FloatArray(0); private set
    var lefts = FloatArray(0); private set
    var widths = FloatArray(0); private set
    var heights = FloatArray(0); private set

    fun layout(sizes: List<Pair<Float, Float>>) {
        val maxW = sizes.maxOfOrNull { it.first } ?: 1f
        scale = max(0.01f, (viewW - 2 * margin) / maxW) * zoom
        val n = sizes.size
        tops = FloatArray(n); lefts = FloatArray(n); widths = FloatArray(n); heights = FloatArray(n)
        contentW = max(viewW.toFloat(), maxW * scale + 2 * margin)
        var y = margin
        for (i in 0 until n) {
            widths[i] = sizes[i].first * scale
            heights[i] = sizes[i].second * scale
            lefts[i] = (contentW - widths[i]) / 2
            tops[i] = y
            y += heights[i] + gap
        }
        contentH = y + margin - gap
        clamp()
    }

    fun clamp() {
        scrollX = scrollX.coerceIn(0f, max(0f, contentW - viewW))
        scrollY = scrollY.coerceIn(0f, max(0f, contentH - viewH))
    }

    /** Page under a screen point, and that point in page device pixels. */
    fun pageAt(x: Float, y: Float): Triple<Int, Float, Float>? {
        val cx = x + scrollX
        val cy = y + scrollY
        for (i in tops.indices) {
            if (cy >= tops[i] - gap / 2 && cy <= tops[i] + heights[i] + gap / 2 && cx >= lefts[i] - margin && cx <= lefts[i] + widths[i] + margin)
                return Triple(i, (cx - lefts[i]).coerceIn(0f, widths[i]), (cy - tops[i]).coerceIn(0f, heights[i]))
        }
        return null
    }

    fun mostVisible(): Int {
        var best = 0
        var bestH = -1f
        for (i in tops.indices) {
            val h = min(tops[i] + heights[i], scrollY + viewH) - max(tops[i], scrollY)
            if (h > bestH) { bestH = h; best = i }
        }
        return best
    }

    /** Zoom by `factor` keeping the content point under (fx, fy) in place. */
    fun zoomAround(factor: Float, fx: Float, fy: Float, sizes: List<Pair<Float, Float>>) {
        val oldScale = scale
        val oldLeft = if (lefts.isNotEmpty()) lefts.min() else margin
        val px = (scrollX + fx - oldLeft) / oldScale
        val py = (scrollY + fy) / oldScale
        zoom = (zoom * factor).coerceIn(0.5f, 16f)
        layout(sizes)
        val newLeft = if (lefts.isNotEmpty()) lefts.min() else margin
        scrollX = px * scale + newLeft - fx
        scrollY = py * scale - fy
        clamp()
    }
}

private class Rendered(val bitmap: Bitmap, val version: Int, val docId: Int, val width: Int)
private class Tile(val bitmap: Bitmap, val version: Int, val docId: Int, val scale: Float, val x: Int, val y: Int)

/** Page bitmaps (limited number) plus sharp tiles of the visible part when zoomed in. */
private class Renderer {
    val pages = LinkedHashMap<Int, Rendered>(16, 0.75f, true)
    val tiles = HashMap<Int, Tile>()

    fun trim(keep: Set<Int>) {
        val it = pages.entries.iterator()
        while (pages.size > 10 && it.hasNext()) {
            val e = it.next()
            if (e.key !in keep) it.remove()
        }
        tiles.keys.retainAll(keep)
    }
}

private enum class Drag { None, Pan, Move, Resize, Marquee, DrawBox, Pinch }

/** Renders what is visible now; called with the newest state, never cancelled midway. */
private suspend fun renderPass(ed: Editor, vs: ViewState, renderer: Renderer, onFrame: () -> Unit) {
    val sizes = ed.pageSizes
    if (!ed.isOpen || sizes.isEmpty() || vs.tops.size != sizes.size) return
    val visible = sizes.indices.filter { i ->
        vs.tops[i] + vs.heights[i] > vs.scrollY - vs.viewH * 0.5f && vs.tops[i] < vs.scrollY + vs.viewH * 1.5f
    }
    val docId = ed.docId
    val interacting = vs.interacting
    val scale = vs.scale
    val tops = vs.tops.copyOf(); val lefts = vs.lefts.copyOf(); val widths = vs.widths.copyOf(); val heights = vs.heights.copyOf()
    val sx = vs.scrollX; val sy = vs.scrollY; val vw = vs.viewW; val vh = vs.viewH
    withContext(Dispatchers.Default) {
        for (i in visible) {
            val version = Engine.nPageVersion(i)
            val want = min(widths[i].roundToInt(), 1500).coerceAtLeast(16)
            val have = renderer.pages[i]
            if (have != null && have.version == version && have.docId == docId && (interacting || abs(have.width - want) < want / 3)) continue
            val s = want / sizes[i].first
            val h = (sizes[i].second * s).roundToInt().coerceAtLeast(16)
            val bm = Bitmap.createBitmap(want, h, Bitmap.Config.ARGB_8888)
            if (Engine.nRender(i, s, 0, 0, want, h, bm)) {
                renderer.pages[i] = Rendered(bm, version, docId, want)
                withContext(Dispatchers.Main) { onFrame() }
            }
        }
        if (!interacting) {
            for (i in visible) {
                if (widths[i] <= 1600) { renderer.tiles.remove(i); continue }
                val version = Engine.nPageVersion(i)
                val x0 = max(0f, sx - lefts[i]).toInt()
                val y0 = max(0f, sy - tops[i]).toInt()
                val x1 = min(widths[i], sx + vw - lefts[i]).toInt()
                val y1 = min(heights[i], sy + vh - tops[i]).toInt()
                if (x1 - x0 < 8 || y1 - y0 < 8) continue
                val tile = renderer.tiles[i]
                if (tile != null && tile.version == version && tile.docId == docId && tile.scale == scale &&
                    tile.x <= x0 && tile.y <= y0 && tile.x + tile.bitmap.width >= x1 && tile.y + tile.bitmap.height >= y1) continue
                val bm = Bitmap.createBitmap(x1 - x0, y1 - y0, Bitmap.Config.ARGB_8888)
                if (Engine.nRender(i, scale, x0, y0, x1 - x0, y1 - y0, bm)) {
                    renderer.tiles[i] = Tile(bm, version, docId, scale, x0, y0)
                    withContext(Dispatchers.Main) { onFrame() }
                }
            }
        }
        renderer.trim(visible.toSet())
    }
}

@Composable
fun PageView(ed: Editor, vs: ViewState, modifier: Modifier = Modifier) {
    val density = LocalDensity.current
    val dp = density.density
    val scope = rememberCoroutineScope()
    val renderer = remember { Renderer() }
    var frame by remember { mutableIntStateOf(0) }                 // bumps when bitmaps arrive
    var overlayRect by remember { mutableStateOf<Rect?>(null) }    // marquee / drawn box (screen)
    var menuAt by remember { mutableStateOf<Offset?>(null) }       // long-press menu
    var menuPage by remember { mutableStateOf(Triple(-1, 0f, 0f)) }
    var guideX by remember { mutableFloatStateOf(Float.NaN) }     // alignment guides (page points)
    var guideY by remember { mutableFloatStateOf(Float.NaN) }

    vs.margin = 12f * dp
    vs.gap = 12f * dp
    val sizes = ed.pageSizes
    vs.layout(sizes)

    fun toPage(page: Int, dx: Float, dy: Float) = Engine.nDevToPage(page, vs.scale, dx, dy)
    fun pageRectOnScreen(page: Int, r: PRect): Rect {
        val a = Engine.nPageToDev(page, vs.scale, r.l, r.t)
        val b = Engine.nPageToDev(page, vs.scale, r.r, r.b)
        val ox = vs.lefts[page] - vs.scrollX
        val oy = vs.tops[page] - vs.scrollY
        return Rect(min(a[0], b[0]) + ox, min(a[1], b[1]) + oy, max(a[0], b[0]) + ox, max(a[1], b[1]) + oy)
    }
    fun handles(r: Rect): List<Offset> = listOf(
        Offset(r.left, r.top), Offset(r.center.x, r.top), Offset(r.right, r.top),
        Offset(r.left, r.center.y), Offset(r.right, r.center.y),
        Offset(r.left, r.bottom), Offset(r.center.x, r.bottom), Offset(r.right, r.bottom),
    )

    // Scroll requests (page jumps, search hits).
    LaunchedEffect(ed.scrollToPage, ed.tick, vs.viewH) {
        val p = ed.scrollToPage
        if (p >= 0 && p < vs.tops.size && vs.viewH > 1) {
            vs.scrollY = vs.tops[p] - vs.margin
            vs.clamp()
            ed.scrollToPage = -1
        }
    }
    LaunchedEffect(ed.scrollToRect) {
        val h = ed.scrollToRect ?: return@LaunchedEffect
        if (h.page < vs.tops.size) {
            val r = pageRectOnScreen(h.page, h.box)
            vs.scrollY += r.center.y - vs.viewH / 2f
            vs.scrollX += r.center.x - vs.viewW / 2f
            vs.clamp()
        }
        ed.scrollToRect = null
    }
    LaunchedEffect(ed.revealSel) {
        if (!ed.revealSel) return@LaunchedEffect
        val s = ed.sel
        if (s.has && s.bounds != null && s.page in vs.tops.indices) {
            val r = pageRectOnScreen(s.page, s.bounds)
            if (r.center.y > vs.viewH * 0.4f || r.center.y < 0) {
                vs.scrollY += r.center.y - vs.viewH * 0.22f
                vs.clamp()
            }
        }
        ed.revealSel = false
    }
    LaunchedEffect(Unit) {
        snapshotFlow { listOf(ed.tick, ed.docId, vs.zoom, vs.scrollX, vs.scrollY, vs.viewW, vs.viewH, vs.interacting, ed.isOpen) }
            .conflate()
            .collect {
                renderPass(ed, vs, renderer) { frame++ }
                ed.currentPage = vs.mostVisible()
                delay(8)
            }
    }

    val touchSlop = 8f * dp
    val handleRadius = 22f * dp

    Box(
        modifier
            .onSizeChanged { sz -> vs.viewW = max(1, sz.width); vs.viewH = max(1, sz.height); vs.layout(ed.pageSizes) }
            .pointerInput(ed.isOpen) {
                var lastTapTime = 0L
                var lastTapPos = Offset.Zero
                awaitEachGesture {
                    val down = awaitFirstDown(requireUnconsumed = false)
                    val start = down.position
                    var mode = Drag.None
                    var longPressed = false
                    val velocity = VelocityTracker()
                    velocity.addPosition(down.uptimeMillis, down.position)
                    val startHit = vs.pageAt(start.x, start.y)
                    var lastPagePt: FloatArray? = startHit?.let { toPage(it.first, it.second, it.third) }
                    var handle = -1
                    var startBounds: PRect? = null
                    var dragTotal = Offset.Zero
                    var alignOn = false
                    val startPagePt = lastPagePt
                    var appliedX = 0f
                    var appliedY = 0f

                    while (true) {
                        val event = if (mode == Drag.None && !longPressed)
                            withTimeoutOrNull(viewConfiguration.longPressTimeoutMillis) { awaitPointerEvent() }
                        else awaitPointerEvent()
                        if (event == null) { longPressed = true; continue }
                        val pressed = event.changes.filter { it.pressed }
                        if (pressed.isEmpty()) break
                        if (pressed.size >= 2) {
                            if (mode != Drag.Pinch) { mode = Drag.Pinch; vs.interacting = true; overlayRect = null }
                            val zoomChange = event.calculateZoom()
                            val c1 = event.calculateCentroid(useCurrent = true)
                            val c0 = event.calculateCentroid(useCurrent = false)
                            if (zoomChange != 1f && c1 != Offset.Unspecified) vs.zoomAround(zoomChange, c1.x, c1.y, sizes)
                            if (c1 != Offset.Unspecified && c0 != Offset.Unspecified) {
                                vs.scrollX -= c1.x - c0.x
                                vs.scrollY -= c1.y - c0.y
                                vs.clamp()
                            }
                            event.changes.forEach { it.consume() }
                            continue
                        }
                        val ch = pressed.first()
                        velocity.addPosition(ch.uptimeMillis, ch.position)
                        if (mode == Drag.None) {
                            if ((ch.position - start).getDistance() < touchSlop) continue
                            val sel = ed.sel
                            val selRect = if (sel.has && sel.bounds != null && sel.page in sizes.indices)
                                pageRectOnScreen(sel.page, sel.bounds) else null
                            if (selRect != null && !sel.annot) {
                                // Small things (a word, a thin line): a drag on them moves them;
                                // their handles are reached from just outside.
                                val thinY = selRect.height < 2 * handleRadius
                                val thinX = selRect.width < 2 * handleRadius
                                val small = selRect.width < 3 * handleRadius || selRect.height < 3 * handleRadius
                                if (!(small && selRect.contains(start))) {
                                    var best = handleRadius
                                    handles(selRect).forEachIndexed { k, p ->
                                        if ((k == 1 || k == 6) && thinY) return@forEachIndexed
                                        if ((k == 3 || k == 4) && thinX) return@forEachIndexed
                                        val dd = (p - start).getDistance()
                                        if (dd <= best) { best = dd; handle = k }
                                    }
                                }
                            }
                            mode = when {
                                ed.tool == Tool.Pan -> Drag.Pan
                                longPressed && ed.tool == Tool.Select -> Drag.Marquee
                                handle >= 0 && ed.tool == Tool.Select -> Drag.Resize
                                selRect != null && ed.tool == Tool.Select && startHit?.first == sel.page &&
                                    selRect.inflate(handleRadius * 0.5f).contains(start) -> Drag.Move
                                ed.tool in setOf(Tool.Rect, Tool.Redact, Tool.Cut) && startHit != null -> Drag.DrawBox
                                else -> Drag.Pan
                            }
                            if (mode == Drag.Resize) startBounds = sel.bounds
                            if (mode == Drag.Move) alignOn = Engine.nAlignBegin()
                            vs.interacting = mode == Drag.Pan || mode == Drag.Move || mode == Drag.Resize
                        }
                        val delta = ch.positionChange()
                        when (mode) {
                            Drag.Pan -> { vs.scrollX -= delta.x; vs.scrollY -= delta.y; vs.clamp() }
                            Drag.Move -> {
                                val p = ed.sel.page
                                val cur = toPage(p, ch.position.x + vs.scrollX - vs.lefts[p], ch.position.y + vs.scrollY - vs.tops[p])
                                val st = startPagePt
                                if (st != null) {
                                    // total move since the start, lined up with other text if close
                                    var tx = cur[0] - st[0]
                                    var ty = cur[1] - st[1]
                                    guideX = Float.NaN; guideY = Float.NaN
                                    if (alignOn && (ed.alignGuides || ed.alignSnap)) {
                                        val r = Engine.nAlignSnap(tx, ty, 8f * dp / vs.scale, ed.alignSnap)
                                        tx = r[0]; ty = r[1]
                                        if (ed.alignGuides) { guideX = r[2]; guideY = r[3] }
                                    }
                                    if (tx != appliedX || ty != appliedY) {
                                        Engine.nMoveSelection(tx - appliedX, ty - appliedY)
                                        appliedX = tx; appliedY = ty
                                        ed.sel = Selection.parse(Engine.nSelection())
                                        ed.tick++
                                    }
                                }
                                lastPagePt = cur
                            }
                            Drag.Resize -> {
                                val b0 = startBounds
                                dragTotal += delta
                                if (b0 != null) {
                                    val dxp = dragTotal.x / vs.scale
                                    val dyp = -dragTotal.y / vs.scale
                                    var l = b0.l; var r = b0.r; var b = b0.b; var t = b0.t
                                    when (handle) { 0, 3, 5 -> l += dxp; 2, 4, 7 -> r += dxp }
                                    when (handle) { 0, 1, 2 -> t += dyp; 5, 6, 7 -> b += dyp }
                                    if (r - l > 1f && t - b > 1f) {
                                        Engine.nResizeSelection(l, b, r, t, ed.keepSpacing)
                                        ed.sel = Selection.parse(Engine.nSelection())
                                        ed.tick++
                                    }
                                }
                            }
                            Drag.Marquee, Drag.DrawBox -> overlayRect = Rect(
                                min(start.x, ch.position.x), min(start.y, ch.position.y),
                                max(start.x, ch.position.x), max(start.y, ch.position.y))
                            else -> {}
                        }
                        ch.consume()
                    }

                    // ---- gesture ended
                    vs.interacting = false
                    guideX = Float.NaN; guideY = Float.NaN
                    when (mode) {
                        Drag.None -> {
                            val hit = startHit
                            if (hit != null) {
                                val pt = toPage(hit.first, hit.second, hit.third)
                                val tol = 10f * dp / vs.scale
                                if (longPressed) {
                                    if (ed.tool == Tool.Select && !(ed.sel.has && ed.sel.page == hit.first &&
                                            ed.sel.bounds?.contains(pt[0], pt[1]) == true))
                                        ed.tap(hit.first, pt[0], pt[1], tol)
                                    menuPage = Triple(hit.first, pt[0], pt[1])
                                    menuAt = start
                                } else {
                                    val now = System.currentTimeMillis()
                                    val isDouble = now - lastTapTime < 350 && (start - lastTapPos).getDistance() < touchSlop * 3
                                    lastTapTime = if (isDouble) 0L else now
                                    lastTapPos = start
                                    handleTap(ed, hit.first, pt[0], pt[1], tol, isDouble)
                                }
                            }
                        }
                        Drag.Pan -> {
                            val v = velocity.calculateVelocity()
                            scope.launch {
                                var vx = -v.x
                                var vy = -v.y
                                var lastT = 0L
                                while (isActive && (abs(vx) > 40 || abs(vy) > 40)) {
                                    withFrameMillis { tms ->
                                        val dt = if (lastT == 0L) 16f else (tms - lastT).toFloat().coerceIn(1f, 64f)
                                        lastT = tms
                                        vs.scrollX += vx * dt / 1000f
                                        vs.scrollY += vy * dt / 1000f
                                        vs.clamp()
                                        val decay = Math.pow(0.08, dt / 1000.0).toFloat()
                                        vx *= decay
                                        vy *= decay
                                    }
                                }
                            }
                        }
                        Drag.Move -> ed.commit(if (ed.sel.count > 1) "Move group" else "Move")
                        Drag.Resize -> ed.commit("Resize")
                        Drag.Marquee, Drag.DrawBox -> {
                            val r = overlayRect
                            overlayRect = null
                            val hit = startHit
                            if (r != null && hit != null) {
                                val p = hit.first
                                val a = toPage(p, r.left + vs.scrollX - vs.lefts[p], r.top + vs.scrollY - vs.tops[p])
                                val b = toPage(p, r.right + vs.scrollX - vs.lefts[p], r.bottom + vs.scrollY - vs.tops[p])
                                val box = PRect.points(a[0], a[1], b[0], b[1])
                                if (mode == Drag.Marquee) ed.selectArea(p, box)
                                else when (ed.tool) {
                                    Tool.Rect -> ed.addRect(p, box)
                                    Tool.Redact -> ed.redact(p, box)
                                    Tool.Cut -> ed.cutPiece(p, box)
                                    else -> {}
                                }
                            }
                        }
                        Drag.Pinch -> {}
                    }
                }
            }
    ) {
        Canvas(Modifier.fillMaxSize()) {
            drawRect(C.CanvasBg)
            if (!ed.isOpen) return@Canvas
            @Suppress("UNUSED_VARIABLE") val f = frame + ed.tick
            for (i in sizes.indices) {
                if (i >= vs.tops.size) break
                val top = vs.tops[i] - vs.scrollY
                val left = vs.lefts[i] - vs.scrollX
                if (top > size.height || top + vs.heights[i] < 0) continue
                drawRect(Color(0x55000000), Offset(left + 2 * dp, top + 3 * dp), Size(vs.widths[i], vs.heights[i]))
                drawRect(Color.White, Offset(left, top), Size(vs.widths[i], vs.heights[i]))
                renderer.pages[i]?.let { rp ->
                    drawImage(rp.bitmap.asImageBitmap(), srcOffset = IntOffset.Zero,
                        srcSize = IntSize(rp.bitmap.width, rp.bitmap.height),
                        dstOffset = IntOffset(left.roundToInt(), top.roundToInt()),
                        dstSize = IntSize(vs.widths[i].roundToInt(), vs.heights[i].roundToInt()))
                }
                renderer.tiles[i]?.let { tl ->
                    if (tl.scale == vs.scale) drawImage(tl.bitmap.asImageBitmap(),
                        dstOffset = IntOffset((left + tl.x).roundToInt(), (top + tl.y).roundToInt()),
                        dstSize = IntSize(tl.bitmap.width, tl.bitmap.height))
                }
            }
            for ((k, h) in ed.hits.withIndex()) {
                if (h.page !in sizes.indices) continue
                val r = pageRectOnScreen(h.page, h.box)
                if (r.bottom < 0 || r.top > size.height) continue
                drawRect(if (k == ed.hitIndex) Color(0x88FF9800) else Color(0x55FFEB3B), r.topLeft, r.size)
            }
            val sel = ed.sel
            if (sel.has && sel.bounds != null && sel.page in sizes.indices) {
                val r = pageRectOnScreen(sel.page, sel.bounds)
                val pad = 3f * dp
                drawRect(C.Accent, Offset(r.left - pad, r.top - pad), Size(r.width + 2 * pad, r.height + 2 * pad),
                    style = Stroke(width = 2f * dp))
                if (!sel.annot && ed.tool == Tool.Select) {
                    for (p in handles(r)) {
                        drawCircle(Color.White, 6f * dp, p)
                        drawCircle(C.Accent, 6f * dp, p, style = Stroke(width = 2f * dp))
                    }
                }
            }
            // alignment guides while text is dragged
            if (sel.has && sel.page in sizes.indices && (!guideX.isNaN() || !guideY.isNaN())) {
                val pg = sel.page
                val top = vs.tops[pg] - vs.scrollY
                val left = vs.lefts[pg] - vs.scrollX
                val gc = Color(0xEBFF40AA)
                if (!guideX.isNaN()) {
                    val x = Engine.nPageToDev(pg, vs.scale, guideX, 0f)[0] + left
                    drawLine(gc, Offset(x, top), Offset(x, top + vs.heights[pg]), strokeWidth = 1.5f * dp)
                }
                if (!guideY.isNaN()) {
                    val y = Engine.nPageToDev(pg, vs.scale, 0f, guideY)[1] + top
                    drawLine(gc, Offset(left, y), Offset(left + vs.widths[pg], y), strokeWidth = 1.5f * dp)
                }
            }
            overlayRect?.let { r ->
                val col = if (ed.tool == Tool.Redact) Color(0x66000000) else Color(0x333B82F6)
                drawRect(col, r.topLeft, r.size)
                drawRect(C.Accent, r.topLeft, r.size, style = Stroke(width = 1.5f * dp))
            }
        }

        val at = menuAt
        if (at != null) {
            val (pg, px, py) = menuPage
            val off = with(density) { DpOffset(at.x.toDp(), (at.y - vs.viewH).toDp()) }
            DropdownMenu(expanded = true, onDismissRequest = { menuAt = null }, offset = off) {
                val s = ed.sel
                @Composable fun item(label: String, action: () -> Unit) =
                    DropdownMenuItem(text = { Text(label) }, onClick = { menuAt = null; action() })
                if (s.has && !s.annot) {
                    if (s.canEditText) item(tr("Edit text")) { ed.beginEditText() }
                    item(tr("Copy")) { ed.copy() }
                    item(tr("Cut")) { ed.cut() }
                    if (s.multi && s.group == 0) item(tr("Group")) { ed.group() }
                    if (s.group != 0 || s.type == ObjType.FORM) item(tr("Ungroup")) { ed.ungroup() }
                    if (s.isPath && s.parts > 1) item(trf("Split into %d separate lines", s.parts)) { ed.split() }
                    if (ed.pageCount > 1) item(tr("Move to page...")) { ed.dialog = Dlg.MoveToPage }
                    item(tr("Properties")) { ed.showProps = true }
                    item(tr("Delete")) { ed.delete() }
                } else if (s.annot) {
                    item(tr("Delete annotation")) { ed.delete() }
                    item(tr("Make this page's annotations editable")) { ed.flattenPage(s.page) }
                }
                if (pg >= 0) {
                    item(tr("Paste here")) { ed.paste(pg, px, py, true) }
                    item(tr("Select all on this page")) { ed.selectAll(pg) }
                    item(tr("Select all lines on this page")) { ed.selectAll(pg, true) }
                }
            }
        }
    }
}

/** Taps: select / edit text / place text or a picture, depending on the tool. */
private fun handleTap(ed: Editor, page: Int, px: Float, py: Float, tol: Float, isDouble: Boolean) {
    when (ed.tool) {
        Tool.Select, Tool.Pan -> {
            if (isDouble && ed.sel.canEditText) { ed.beginEditText(); return }
            ed.tap(page, px, py, tol, single = isDouble)
            if (isDouble && ed.sel.canEditText) ed.beginEditText()
        }
        Tool.Text -> {
            ed.tap(page, px, py, tol, single = true)
            if (ed.sel.canEditText) ed.beginEditText()
            else { ed.clearSelection(); ed.dialog = Dlg.NewText(page, px, py) }
        }
        Tool.Image -> {
            ed.pendingImageAt = Triple(page, px, py)
            ed.host.pickImage()
        }
        else -> {}
    }
}
