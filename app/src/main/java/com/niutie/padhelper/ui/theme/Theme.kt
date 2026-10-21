package com.niutie.padhelper.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

/** 深色背景的渐变两端。玻璃效果需要「有内容可折射」的背景，纯黑纯白都会让折射看不出来。 */
val BackgroundTop = Color(0xFF1A1A2E)
val BackgroundBottom = Color(0xFF16213E)

/** 徽章的警示色。 */
val Amber = Color(0xFFFFB74D)
val AmberRowHighlight = Color(0x14FFB74D)

private val DarkColors = darkColorScheme(
    primary = Amber,
    onPrimary = Color(0xFF2B1D00),
    background = BackgroundTop,
    onBackground = Color(0xFFEAEAF2),
    surface = BackgroundBottom,
    onSurface = Color(0xFFEAEAF2),
    surfaceVariant = Color(0xFF23233A),
    onSurfaceVariant = Color(0xFFA8A8BE),
    outline = Color(0xFF3A3A55),
)

@Composable
fun PadHelperTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = DarkColors,
        typography = Typography(),
        content = content,
    )
}
