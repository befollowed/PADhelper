package com.niutie.padhelper.ui.components

import androidx.compose.material3.AlertDialog
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import com.niutie.padhelper.ui.theme.Amber

/**
 * 徽章首次开启的风险提示。
 *
 * 刻意不用液态玻璃包弹窗：弹窗是系统层级的模态，玻璃效果会拖慢首帧，
 * 而且它的背景不是我们的渐变，折射出来只会是灰糊一片。
 */
@Composable
fun BadgeConfirmDialog(
    onConfirm: () -> Unit,
    onDismiss: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = {
            Text(text = "⚠️ 风险提示", color = Amber, style = MaterialTheme.typography.titleLarge)
        },
        text = {
            Text(
                text = "徽章会影响战斗数值，存在被检测封号的风险。谨慎开启。",
                style = MaterialTheme.typography.bodyMedium,
            )
        },
        confirmButton = {
            TextButton(onClick = onConfirm) {
                Text("接受风险", color = Amber)
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) {
                Text("取消", color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        },
        containerColor = Color(0xFF20203A),
        titleContentColor = Color(0xFFEAEAF2),
        textContentColor = Color(0xFFC9C9DB),
    )
}
