package com.niutie.padhelper.ui.components

import android.os.Build
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.kyant.backdrop.backdrops.LayerBackdrop
import com.kyant.backdrop.drawBackdrop
import com.kyant.backdrop.effects.blur
import com.kyant.backdrop.effects.lens
import com.kyant.backdrop.highlight.Highlight
import com.kyant.backdrop.shadow.Shadow

/** 液态玻璃是否可用。blur 需要 API 31，lens（折射）需要 API 33。 */
val liquidGlassSupported: Boolean get() = Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU

/**
 * 液态玻璃卡片。
 *
 * API 33+ 用 backdrop 库做真实的背景折射；低版本退回半透明纯色，
 * 保证不崩、内容照样可读（设计文档 8.1 的硬性要求）。
 *
 * [solidAlpha] 控制低版本回退时的不透明度；「关于」卡片比「功能」卡片更实，
 * 靠这个参数区分。
 */
@Composable
fun GlassCard(
    backdrop: LayerBackdrop,
    modifier: Modifier = Modifier,
    corner: Dp = 28.dp,
    solidAlpha: Float = 0.55f,
    content: @Composable BoxScope.() -> Unit,
) {
    val shape = RoundedCornerShape(corner)
    val glass = liquidGlassSupported
    val base = if (glass) {
        Modifier.drawBackdrop(
            backdrop = backdrop,
            shape = { shape },
            effects = {
                blur(radius = 24f)
                lens(refractionHeight = 28f, refractionAmount = 18f)
            },
            highlight = { Highlight.Ambient },
            shadow = { Shadow.Default },
        )
    } else {
        Modifier.background(Color(0xFF1E1E33).copy(alpha = solidAlpha), shape)
    }

    Box(
        modifier = modifier
            .clip(shape)
            .then(base)
            // 玻璃只负责折射，本身没有底色。压一层淡黑把背景压暗，
            // 否则背景色斑一浓，白字压在浅色玻璃上就看不清了。
            .background(
                Color(0xFF0E0E1A).copy(alpha = if (glass) 0.34f else 0f),
                shape,
            ),
        content = content,
    )
}
