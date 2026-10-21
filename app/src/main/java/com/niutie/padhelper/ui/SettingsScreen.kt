package com.niutie.padhelper.ui

import android.content.Intent
import android.net.Uri
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.expandVertically
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import com.kyant.backdrop.backdrops.layerBackdrop
import com.kyant.backdrop.backdrops.rememberLayerBackdrop
import com.niutie.padhelper.BuildConfig
import com.niutie.padhelper.ui.components.BadgeConfirmDialog
import com.niutie.padhelper.ui.components.FeatureRow
import com.niutie.padhelper.ui.components.GlassCard
import com.niutie.padhelper.ui.theme.Amber
import com.niutie.padhelper.ui.theme.BackgroundBottom
import com.niutie.padhelper.ui.theme.BackgroundTop

private const val PROJECT_URL = "https://github.com/befollowed/PADhelper"

@Composable
fun SettingsScreen(vm: SettingsViewModel = viewModel()) {
    val state by vm.state.collectAsStateWithLifecycle()
    val gate by vm.gate.collectAsStateWithLifecycle()
    val showBadgeDialog by vm.badgeDialogVisible.collectAsStateWithLifecycle()
    val showIcon by vm.showIcon.collectAsStateWithLifecycle()
    val diag by vm.diag.collectAsStateWithLifecycle()

    // 模块状态卡片只看「模块有没有在 LSPosed 里启用」。
    // 页面可见时每 2 秒重读一次，切回前台就能看到最新状态，不用手动刷新。
    androidx.compose.runtime.LaunchedEffect(Unit) {
        while (true) {
            vm.refreshDiag()
            kotlinx.coroutines.delay(2000)
        }
    }

    // 整个屏幕的「背景层」：玻璃卡片从这一层取像素做折射
    val backdrop = rememberLayerBackdrop()
    val scroll = rememberScrollState()
    val context = LocalContext.current

    Box(
        Modifier
            .fillMaxSize()
            .background(Brush.verticalGradient(listOf(BackgroundTop, BackgroundBottom)))
    ) {
        // 背景层必须挂 layerBackdrop：backdrop 库靠这个 Modifier 把这一段内容
        // 录进 GraphicsLayer，玻璃卡片才拿得到像素做模糊/折射。
        // 少了它，drawBackdrop 拿不到 layerCoordinates 就直接 return，
        // 卡片只会画个阴影 —— 也就是「完全没有液态玻璃效果」。
        Box(
            Modifier
                .fillMaxSize()
                .layerBackdrop(backdrop)
        ) {
            // 玻璃要有东西可折射才看得出来。纯深色渐变折射后几乎没差别，
            // 所以这里放几团高饱和、边缘明显的色斑 + 一道斜向亮带。
            Box(
                Modifier
                    .fillMaxSize()
                    .background(
                        Brush.radialGradient(
                            colors = listOf(Color(0x7A2E5BFF), Color(0x00000000)),
                            center = androidx.compose.ui.geometry.Offset(160f, 200f),
                            radius = 700f,
                        )
                    )
            )
            Box(
                Modifier
                    .fillMaxSize()
                    .background(
                        Brush.radialGradient(
                            colors = listOf(Color(0x6E7C4DFF), Color(0x00000000)),
                            center = androidx.compose.ui.geometry.Offset(920f, 1120f),
                            radius = 680f,
                        )
                    )
            )
            Box(
                Modifier
                    .fillMaxSize()
                    .background(
                        Brush.radialGradient(
                            colors = listOf(Color(0x5EFFB74D), Color(0x00000000)),
                            center = androidx.compose.ui.geometry.Offset(560f, 660f),
                            radius = 520f,
                        )
                    )
            )
            Box(
                Modifier
                    .fillMaxSize()
                    .background(
                        Brush.radialGradient(
                            colors = listOf(Color(0x4A00D4FF), Color(0x00000000)),
                            center = androidx.compose.ui.geometry.Offset(760f, 1720f),
                            radius = 560f,
                        )
                    )
            )
            // 斜向亮带：给 lens 折射提供一条锐利边界，卡片压上去能看到明显的位移
            Box(
                Modifier
                    .fillMaxSize()
                    .background(
                        Brush.linearGradient(
                            colors = listOf(
                                Color(0x00FFFFFF),
                                Color(0x1FFFFFFF),
                                Color(0x00FFFFFF),
                            ),
                            start = androidx.compose.ui.geometry.Offset(0f, 420f),
                            end = androidx.compose.ui.geometry.Offset(1080f, 900f),
                        )
                    )
            )
        }

        Scaffold(containerColor = Color.Transparent) { inner ->
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(inner)
                    .verticalScroll(scroll)
                    .padding(horizontal = 20.dp, vertical = 24.dp),
                verticalArrangement = Arrangement.spacedBy(16.dp),
            ) {
                StatusCard(
                    backdrop = backdrop,
                    diag = diag,
                )
                if (!gate.supported) {
                    VersionBlockCard(backdrop = backdrop, gate = gate)
                }
                FeatureCard(
                    backdrop = backdrop,
                    state = state,
                    enabled = gate.supported,
                    onSkin = vm::setSkin,
                    onBgm = vm::setBgm,
                    onBadge = vm::onBadgeToggle,
                )
                AppearanceCard(
                    backdrop = backdrop,
                    showIcon = showIcon,
                    onShowIcon = vm::setShowIcon,
                )
                AboutCard(
                    backdrop = backdrop,
                    onOpenProject = {
                        runCatching {
                            context.startActivity(
                                Intent(Intent.ACTION_VIEW, Uri.parse(PROJECT_URL))
                                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                            )
                        }
                    },
                )
                Spacer(Modifier.height(8.dp))
            }
        }
    }

    if (showBadgeDialog) {
        BadgeConfirmDialog(
            onConfirm = vm::acceptBadgeRisk,
            onDismiss = vm::dismissBadgeDialog,
        )
    }
}

@Composable
private fun FeatureCard(
    backdrop: com.kyant.backdrop.backdrops.LayerBackdrop,
    state: com.niutie.padhelper.CfgState,
    enabled: Boolean,
    onSkin: (Boolean) -> Unit,
    onBgm: (Boolean) -> Unit,
    onBadge: (Boolean) -> Unit,
) {
    GlassCard(backdrop = backdrop, modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(20.dp)) {
            SectionTitle("功能")
            Spacer(Modifier.height(12.dp))
            FeatureRow(
                title = "珠子皮肤",
                description = "本地解锁所有珠子皮肤",
                checked = state.skin,
                onCheckedChange = onSkin,
                enabled = enabled,
            )
            Spacer(Modifier.height(12.dp))
            FeatureRow(
                title = "背景音乐",
                description = "本地解锁所有BGM",
                checked = state.bgm,
                onCheckedChange = onBgm,
                enabled = enabled,
            )
            Spacer(Modifier.height(12.dp))
            FeatureRow(
                title = "队伍徽章",
                description = "解锁所有徽章，影响战斗数值，存在封号风险",
                checked = state.badge,
                onCheckedChange = onBadge,
                enabled = enabled,
                warning = true,
                warningIcon = Icons.Filled.Warning,
            )
        }
    }
}

/**
 * 版本不匹配时的提示卡。只在 [com.niutie.padhelper.Target.Info.supported] 为 false 时出现，
 * 同时所有功能开关置灰只读 —— 本模块的偏移全是照着 23.5.2 写死的，别的版本拨了也没用，
 * 更糟的是会让人以为「功能开着」。
 */
@Composable
private fun VersionBlockCard(
    backdrop: com.kyant.backdrop.backdrops.LayerBackdrop,
    gate: com.niutie.padhelper.Target.Info,
) {
    GlassCard(backdrop = backdrop, modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(20.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(
                    imageVector = Icons.Filled.Warning,
                    contentDescription = null,
                    tint = Amber,
                    modifier = Modifier.size(18.dp),
                )
                Spacer(Modifier.width(6.dp))
                Text(
                    text = "版本不匹配",
                    style = MaterialTheme.typography.titleMedium,
                    color = Amber,
                )
            }
            Spacer(Modifier.height(8.dp))
            Text(
                text = if (!gate.installed) {
                    "未检测到《智龙迷城》，无法确认版本。设置已锁定。"
                } else {
                    "当前游戏版本 ${gate.versionName ?: "未知"}，" +
                        "本模块只适配 ${com.niutie.padhelper.Target.VERSION_NAME}。\n" +
                        "设置已锁定；进入游戏也不会加载任何功能。"
                },
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun AppearanceCard(
    backdrop: com.kyant.backdrop.backdrops.LayerBackdrop,
    showIcon: Boolean,
    onShowIcon: (Boolean) -> Unit,
) {
    GlassCard(backdrop = backdrop, modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(20.dp)) {
            SectionTitle("界面")
            Spacer(Modifier.height(12.dp))
            FeatureRow(
                title = "显示模块图标",
                description = "在桌面显示本应用的启动图标",
                checked = showIcon,
                onCheckedChange = onShowIcon,
            )
        }
    }
}

@Composable
private fun AboutCard(
    backdrop: com.kyant.backdrop.backdrops.LayerBackdrop,
    onOpenProject: () -> Unit,
) {
    // 比「功能」卡片更实一点，压住长文本，避免文字在玻璃上发虚
    GlassCard(backdrop = backdrop, modifier = Modifier.fillMaxWidth(), solidAlpha = 0.78f) {
        Column(Modifier.padding(20.dp)) {
            SectionTitle("关于")
            Spacer(Modifier.height(10.dp))
            HorizontalDivider(color = MaterialTheme.colorScheme.outline.copy(alpha = 0.5f))
            Spacer(Modifier.height(12.dp))

            // 使用方法默认折叠，点标题展开（长文本不该一进来就占满屏）
            var expanded by rememberSaveable { mutableStateOf(false) }
            val arrowAngle by animateFloatAsState(
                targetValue = if (expanded) 180f else 0f,
                label = "usageArrow",
            )
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .clickable { expanded = !expanded }
                    .padding(vertical = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    text = "使用方法",
                    style = MaterialTheme.typography.titleSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.weight(1f),
                )
                Icon(
                    imageVector = Icons.Filled.KeyboardArrowDown,
                    contentDescription = if (expanded) "收起" else "展开",
                    tint = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier
                        .size(22.dp)
                        .rotate(arrowAngle),
                )
            }

            AnimatedVisibility(
                visible = expanded,
                enter = fadeIn() + expandVertically(),
                exit = fadeOut() + shrinkVertically(),
            ) {
                Column {
                    Spacer(Modifier.height(8.dp))
                    val steps = listOf(
                        "安装模块并在 LSPosed 中启用",
                        "勾选作用域 jp.gungho.pad（适配版本：23.5.2）",
                        "启动游戏，等待探针加载（约 15 秒）",
                        "在编成中选择目标皮肤 / BGM / 徽章",
                        "进入关卡即生效",
                    )
                    steps.forEachIndexed { i, text ->
                        Row(Modifier.padding(vertical = 3.dp)) {
                            Text(
                                text = "${i + 1}.",
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                modifier = Modifier.width(22.dp),
                            )
                            Text(
                                text = text,
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onSurface,
                            )
                        }
                    }

                    Spacer(Modifier.height(14.dp))
                    Text(
                        text = "关闭功能后建议重启游戏，确保完全恢复。",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }

            Spacer(Modifier.height(14.dp))
            HorizontalDivider(color = MaterialTheme.colorScheme.outline.copy(alpha = 0.5f))
            Spacer(Modifier.height(12.dp))
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .clickable(onClick = onOpenProject)
                    .padding(vertical = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(Modifier.weight(1f)) {
                    Text(
                        text = "项目地址",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurface,
                    )
                    Text(
                        text = PROJECT_URL.removePrefix("https://"),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Text(
                    text = "↗",
                    style = MaterialTheme.typography.bodyMedium,
                    color = Amber,
                )
            }

            Spacer(Modifier.height(10.dp))
            Text(
                text = "版本 ${BuildConfig.VERSION_NAME} · MIT License",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun SectionTitle(text: String) {
    Text(
        text = text,
        style = MaterialTheme.typography.titleSmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
}

@Composable
private fun StatusCard(
    backdrop: com.kyant.backdrop.backdrops.LayerBackdrop,
    diag: com.niutie.padhelper.ConfigBridge.Diag,
) {
    // 只看模块在 LSPosed 里有没有启用：bindOk=false 说明 LSPosed 没有把模块
    // 服务交过来（模块被禁用 / 未安装 / 框架未运行）。
    val active = diag.bindOk
    val title = if (active) "模块已激活" else "模块未激活"
    val statusColor = if (active) Color(0xFF66BB6A) else Amber

    GlassCard(backdrop = backdrop, modifier = Modifier.fillMaxWidth(), solidAlpha = 0.78f) {
        Column(Modifier.padding(20.dp)) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                SectionTitle("模块状态")
                Spacer(Modifier.weight(1f))
                Box(
                    modifier = Modifier
                        .size(8.dp)
                        .background(color = statusColor, shape = androidx.compose.foundation.shape.CircleShape),
                )
                Spacer(Modifier.width(8.dp))
                Text(
                    text = title,
                    style = MaterialTheme.typography.labelLarge,
                    color = statusColor,
                )
            }
            if (!active) {
                Spacer(Modifier.height(10.dp))
                Text(
                    text = "请进入 LSPosed 启用本模块",
                    style = MaterialTheme.typography.bodySmall,
                    color = Amber,
                )
            }
        }
    }
}
