package com.theocp.pdfeditor

import android.content.Context
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp

/** Same palette as the desktop version. */
object C {
    val Accent = Color(0xFF3B82F6)
    val AccentHover = Color(0xFF5B9BFF)
    val AccentDim = Color(0xFF2C64C4)
    val Bg = Color(0xFF16161A)
    val Panel = Color(0xFF1E1E23)
    val Raised = Color(0xFF26262C)
    val Control = Color(0xFF2B2B32)
    val ControlHot = Color(0xFF36363F)
    val Line = Color(0xFF33333C)
    val Text = Color(0xFFE8EAEF)
    val TextDim = Color(0xFF8B8F9C)
    val Good = Color(0xFF6BD199)
    val Bad = Color(0xFFFF7A7A)
    val CanvasBg = Color(0xFF26262C)
}

private val scheme = darkColorScheme(
    primary = C.Accent,
    onPrimary = Color.White,
    primaryContainer = C.AccentDim,
    onPrimaryContainer = Color.White,
    secondary = C.Accent,
    onSecondary = Color.White,
    secondaryContainer = C.Control,
    onSecondaryContainer = C.Text,
    background = C.Bg,
    onBackground = C.Text,
    surface = C.Panel,
    onSurface = C.Text,
    surfaceVariant = C.Raised,
    onSurfaceVariant = C.TextDim,
    surfaceContainer = C.Panel,
    surfaceContainerHigh = C.Raised,
    surfaceContainerHighest = C.Control,
    surfaceContainerLow = C.Panel,
    surfaceContainerLowest = C.Bg,
    outline = C.Line,
    outlineVariant = C.Line,
    error = C.Bad,
)

@Composable
fun AppTheme(content: @Composable () -> Unit) {
    val base = Typography()
    MaterialTheme(
        colorScheme = scheme,
        typography = base.copy(
            titleLarge = base.titleLarge.copy(fontWeight = FontWeight.SemiBold),
            titleMedium = base.titleMedium.copy(fontWeight = FontWeight.SemiBold),
            labelSmall = TextStyle(fontSize = 11.sp, fontWeight = FontWeight.Medium),
        ),
        content = content,
    )
}

/** Language, interface size and the new-text style, kept between starts. */
class Prefs(ctx: Context) {
    private val sp = ctx.getSharedPreferences("settings", Context.MODE_PRIVATE)
    var language: String
        get() = sp.getString("language", "") ?: ""
        set(v) = sp.edit().putString("language", v).apply()
    var uiScale: Float
        get() = sp.getFloat("uiscale", 1f)
        set(v) = sp.edit().putFloat("uiscale", v).apply()
    var matchFont: Boolean
        get() = sp.getBoolean("matchfont", true)
        set(v) = sp.edit().putBoolean("matchfont", v).apply()
    var keepSpacing: Boolean
        get() = sp.getBoolean("keepspacing", false)
        set(v) = sp.edit().putBoolean("keepspacing", v).apply()
    var alignGuides: Boolean
        get() = sp.getBoolean("alignguides", true)
        set(v) = sp.edit().putBoolean("alignguides", v).apply()
    var alignSnap: Boolean
        get() = sp.getBoolean("alignsnap", true)
        set(v) = sp.edit().putBoolean("alignsnap", v).apply()
    var guidesAlways: Boolean
        get() = sp.getBoolean("guidesalways", false)
        set(v) = sp.edit().putBoolean("guidesalways", v).apply()
    var recent: List<String>
        get() = (sp.getString("recent", "") ?: "").split('\n').filter { it.isNotBlank() }
        set(v) = sp.edit().putString("recent", v.take(8).joinToString("\n")).apply()
}
