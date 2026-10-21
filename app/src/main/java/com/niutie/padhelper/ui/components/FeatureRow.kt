package com.niutie.padhelper.ui.components

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.unit.dp
import com.niutie.padhelper.ui.theme.Amber
import com.niutie.padhelper.ui.theme.AmberRowHighlight

/**
 * 一行功能开关：左边标题 + 说明，右边开关。
 *
 * 徽章行是特殊的一行：[warning] 为 true 时标题用琥珀色加警告图标，
 * 且开关打开后整行铺一层淡琥珀底色（设计文档要求的「开启即高亮」）。
 */
@Composable
fun FeatureRow(
    title: String,
    description: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
    modifier: Modifier = Modifier,
    warning: Boolean = false,
    warningIcon: ImageVector? = null,
    enabled: Boolean = true,
) {
    val highlight = warning && checked
    val rowShape = RoundedCornerShape(12.dp)
    Row(
        modifier = modifier
            .fillMaxWidth()
            .then(
                if (highlight) {
                    Modifier.background(AmberRowHighlight, rowShape)
                } else {
                    Modifier
                }
            )
            .padding(horizontal = 12.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(
            modifier = Modifier.weight(1f),
            verticalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (warning && warningIcon != null) {
                    Icon(
                        imageVector = warningIcon,
                        contentDescription = null,
                        tint = Amber,
                        modifier = Modifier.size(18.dp),
                    )
                    Spacer(Modifier.width(6.dp))
                }
                Text(
                    text = title,
                    style = MaterialTheme.typography.titleMedium,
                    color = if (warning) Amber else MaterialTheme.colorScheme.onSurface,
                )
            }
            Text(
                text = description,
                style = MaterialTheme.typography.bodySmall,
                color = if (warning) Amber.copy(alpha = 0.75f)
                else MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Spacer(Modifier.width(12.dp))
        Switch(
            checked = checked,
            onCheckedChange = onCheckedChange,
            enabled = enabled,
            colors = SwitchDefaults.colors(
                checkedThumbColor = if (warning) Color(0xFF2B1D00) else Color(0xFF1A1A2E),
                checkedTrackColor = if (warning) Amber else Color(0xFF8E8EFF),
            ),
        )
    }
}
