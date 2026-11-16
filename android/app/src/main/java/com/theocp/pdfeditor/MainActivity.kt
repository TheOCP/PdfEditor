package com.theocp.pdfeditor

import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.ImageDecoder
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.OpenableColumns
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Surface
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.key
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.Density
import androidx.core.content.FileProvider
import androidx.documentfile.provider.DocumentFile
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

/** The editor lives as long as the process, like the native engine behind it. */
object App {
    @Volatile private var editor: Editor? = null
    fun editor(ctx: Context): Editor = editor ?: synchronized(this) {
        editor ?: Editor(ctx.applicationContext, Prefs(ctx.applicationContext)).also { editor = it }
    }
}

class MainActivity : ComponentActivity(), Host {
    private lateinit var ed: Editor

    private val openPdf = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) ed.guardUnsaved { openUri(uri, keep = true) }
    }
    private val importPdf = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) lifecycleScope.launch {
            val bytes = withContext(Dispatchers.IO) { readBytes(uri) }
            if (bytes != null) ed.importFile = displayName(uri) to bytes else ed.toast(tr("Could not open this PDF"), true)
        }
    }
    private val createPdf = registerForActivityResult(ActivityResultContracts.CreateDocument("application/pdf")) { uri ->
        if (uri == null) { ed.afterSave = null; return@registerForActivityResult }
        val bytes = Engine.nSave()
        if (bytes == null) { ed.toast(tr("Could not save"), true); return@registerForActivityResult }
        persist(uri, write = true)
        ed.savedAs(uri, displayName(uri), bytes)
    }
    private val pickFolder = registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri != null) exportTo(uri)
    }
    private val pickImg = registerForActivityResult(ActivityResultContracts.PickVisualMedia()) { uri ->
        if (uri == null) { ed.pendingImageAt = null; return@registerForActivityResult }
        lifecycleScope.launch {
            val bm = withContext(Dispatchers.IO) { decodeImage(uri) }
            if (bm != null) ed.placeImage(bm) else ed.toast(tr("Could not read this picture"), true)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.dark(C.Panel.toArgb()),
            navigationBarStyle = SystemBarStyle.dark(C.Panel.toArgb()),
        )
        super.onCreate(savedInstanceState)
        Engine.cacheDir = cacheDir.absolutePath
        ed = App.editor(this)
        ed.host = this
        ed.versionName = runCatching { packageManager.getPackageInfo(packageName, 0).versionName }.getOrNull() ?: ""
        ed.applyLanguage(ed.language)
        if (savedInstanceState == null) handleIntent(intent)

        setContent {
            val base = LocalDensity.current
            // Reading these here re-composes everything when they change.
            val lang = ed.language
            val scale = ed.uiScale
            CompositionLocalProvider(LocalDensity provides Density(base.density * scale, base.fontScale)) {
                AppTheme {
                    key(lang) {
                        Surface(color = C.Panel, contentColor = C.Text, modifier = Modifier.fillMaxSize()) {
                        Box(Modifier.fillMaxSize().safeDrawingPadding()) {
                            BackHandler(enabled = ed.isOpen) { ed.guardUnsaved { ed.close() } }
                            if (ed.isOpen) {
                                val vs = remember(ed.docId) { ViewState() }
                                EditorScreen(ed, vs)
                            } else {
                                val recent = remember(ed.isOpen, ed.tick) { recentFiles() }
                                StartScreen(ed, recent) { u -> openUri(Uri.parse(u), keep = true) }
                                Dialogs(ed)
                            }
                            if (ed.busy) Box(Modifier.fillMaxSize().background(Color(0x66000000)), contentAlignment = Alignment.Center) {
                                CircularProgressIndicator(color = C.Accent)
                            }
                        }
                        }
                    }
                }
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleIntent(intent)
    }

    private fun handleIntent(i: Intent?) {
        i ?: return
        val uri: Uri = when (i.action) {
            Intent.ACTION_VIEW, Intent.ACTION_EDIT -> i.data
            Intent.ACTION_SEND -> if (Build.VERSION.SDK_INT >= 33) i.getParcelableExtra(Intent.EXTRA_STREAM, Uri::class.java)
                                  else @Suppress("DEPRECATION") i.getParcelableExtra(Intent.EXTRA_STREAM)
            else -> null
        } ?: return
        val canWrite = i.action == Intent.ACTION_EDIT || (i.flags and Intent.FLAG_GRANT_WRITE_URI_PERMISSION) != 0
        ed.guardUnsaved { openUri(uri, keep = false, canWrite = canWrite) }
    }

    // ------------------------------------------------------------ files

    private fun displayName(uri: Uri): String {
        runCatching {
            contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
                if (c.moveToFirst()) c.getString(0)?.let { return it }
            }
        }
        return uri.lastPathSegment?.substringAfterLast('/') ?: "document.pdf"
    }

    private fun readBytes(uri: Uri): ByteArray? =
        runCatching { contentResolver.openInputStream(uri)?.use { it.readBytes() } }.getOrNull()

    /** Keeps access to a picked file across restarts. Returns true if it can be written. */
    private fun persist(uri: Uri, write: Boolean): Boolean {
        if (write) {
            val ok = runCatching {
                contentResolver.takePersistableUriPermission(uri,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
            }.isSuccess
            if (ok) return true
        }
        runCatching { contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION) }
        return false
    }

    private fun openUri(uri: Uri, keep: Boolean, canWrite: Boolean = false) {
        lifecycleScope.launch {
            ed.busy = true
            val writable = if (keep) persist(uri, write = true) else canWrite
            val name = displayName(uri)
            val bytes = withContext(Dispatchers.IO) { readBytes(uri) }
            ed.busy = false
            if (bytes == null) {
                ed.toast(tr("Could not open this PDF"), true)
                ed.prefs.recent = ed.prefs.recent.filter { it != uri.toString() }
                ed.tick++
                return@launch
            }
            ed.open(bytes, name, uri, writable)
            if (!keep) ed.prefs.recent = ed.prefs.recent.filter { it != uri.toString() }
        }
    }

    private fun recentFiles(): List<Pair<String, String>> {
        val held = contentResolver.persistedUriPermissions.map { it.uri.toString() }.toSet()
        return ed.prefs.recent.filter { it in held }.map { it to displayName(Uri.parse(it)) }
    }

    override fun write(uri: Uri, bytes: ByteArray): Boolean = runCatching {
        val out = runCatching { contentResolver.openOutputStream(uri, "wt") }.getOrNull()
            ?: contentResolver.openOutputStream(uri, "w")
        out!!.use { it.write(bytes) }
        true
    }.getOrDefault(false)

    private fun suggestedName(): String {
        val n = ed.name.ifEmpty { "document.pdf" }
        return if (n.endsWith(".pdf", true)) n else "$n.pdf"
    }

    override fun pickPdf() = openPdf.launch(arrayOf("application/pdf"))
    override fun pickImportPdf() = importPdf.launch(arrayOf("application/pdf"))
    override fun saveAs() = createPdf.launch(suggestedName())
    override fun exportPages() = pickFolder.launch(null)
    override fun pickImage() = pickImg.launch(PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly))

    override fun share() {
        val bytes = Engine.nSave() ?: run { ed.toast(tr("Could not save"), true); return }
        val dir = File(cacheDir, "share").apply { mkdirs() }
        dir.listFiles()?.forEach { it.delete() }
        val f = File(dir, suggestedName().replace(Regex("[\\\\/:*?\"<>|]"), "_"))
        f.writeBytes(bytes)
        val uri = FileProvider.getUriForFile(this, "$packageName.files", f)
        val send = Intent(Intent.ACTION_SEND).apply {
            type = "application/pdf"
            putExtra(Intent.EXTRA_STREAM, uri)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        startActivity(Intent.createChooser(send, tr("Share")))
    }

    /** Every page as a PNG (2x = 144 dpi) into the chosen folder. */
    private fun exportTo(tree: Uri) {
        val folder = DocumentFile.fromTreeUri(this, tree) ?: return
        val base = suggestedName().dropLast(4)
        val sizes = ed.pageSizes
        lifecycleScope.launch {
            ed.busy = true
            val n = withContext(Dispatchers.IO) {
                var done = 0
                for ((i, s) in sizes.withIndex()) {
                    val k = 2f.coerceAtMost(4000f / maxOf(s.first, s.second))
                    val w = (s.first * k).toInt().coerceAtLeast(1)
                    val h = (s.second * k).toInt().coerceAtLeast(1)
                    val bm = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
                    bm.eraseColor(android.graphics.Color.WHITE)
                    if (!Engine.nRender(i, k, 0, 0, w, h, bm)) continue
                    val file = folder.createFile("image/png", "$base-page-${i + 1}") ?: continue
                    val ok = runCatching {
                        contentResolver.openOutputStream(file.uri)?.use { bm.compress(Bitmap.CompressFormat.PNG, 100, it) }
                    }.getOrNull() == true
                    bm.recycle()
                    if (ok) done++
                }
                done
            }
            ed.busy = false
            ed.toast(trf("Exported %d page image(s)", n), n == 0)
        }
    }

    /** Reads a picture upright and at most ~2400 px, in normal memory (its pixels go to the engine). */
    private fun decodeImage(uri: Uri): Bitmap? = runCatching {
        if (Build.VERSION.SDK_INT >= 28) {
            ImageDecoder.decodeBitmap(ImageDecoder.createSource(contentResolver, uri)) { dec, info, _ ->
                dec.allocator = ImageDecoder.ALLOCATOR_SOFTWARE
                val big = maxOf(info.size.width, info.size.height)
                if (big > 2400) {
                    val k = 2400f / big
                    dec.setTargetSize((info.size.width * k).toInt().coerceAtLeast(1), (info.size.height * k).toInt().coerceAtLeast(1))
                }
            }
        } else {
            val o = BitmapFactory.Options().apply { inJustDecodeBounds = true }
            contentResolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it, null, o) }
            var sample = 1
            while (maxOf(o.outWidth, o.outHeight) / sample > 2400) sample *= 2
            contentResolver.openInputStream(uri)?.use {
                BitmapFactory.decodeStream(it, null, BitmapFactory.Options().apply { inSampleSize = sample })
            }
        }
    }.getOrNull()
}
