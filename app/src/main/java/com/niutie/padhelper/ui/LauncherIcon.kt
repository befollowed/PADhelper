package com.niutie.padhelper.ui

import android.content.ComponentName
import android.content.Context
import android.content.pm.PackageManager
import android.util.Log

/**
 * 桌面图标的显示/隐藏。
 *
 * 图标不是挂在 [MainActivity] 上，而是挂在 manifest 里的 `activity-alias`
 * （`.ui.LauncherAlias`）上，所以可以单独启停它而不影响界面本身。
 *
 * 用 [PackageManager.COMPONENT_ENABLED_STATE_ENABLED] / `DISABLED` +
 * [PackageManager.DONT_KILL_APP]：切换时不会杀掉当前进程，用户点完开关
 * 界面不会闪退。
 *
 * 关掉之后仍然有入口：manifest 里还有 `.ui.InfoAlias`（CATEGORY_INFO），
 * LSPosed 管理器解析模块入口时会退化到它，所以不会把自己锁在外面。
 */
object LauncherIcon {

    private const val TAG = "PADHelper"

    private fun alias(context: Context) =
        ComponentName(context.packageName, "${context.packageName}.ui.LauncherAlias")

    /** 读取系统里当前的实际状态（以系统为准，不信 SharedPreferences）。 */
    fun isEnabled(context: Context): Boolean = runCatching {
        val state = context.packageManager.getComponentEnabledSetting(alias(context))
        when (state) {
            PackageManager.COMPONENT_ENABLED_STATE_ENABLED -> true
            PackageManager.COMPONENT_ENABLED_STATE_DISABLED -> false
            // DEFAULT / 未设置：看 manifest 里写的值，当前是 enabled="true"
            else -> true
        }
    }.getOrDefault(true)

    fun apply(context: Context, show: Boolean) {
        runCatching {
            context.packageManager.setComponentEnabledSetting(
                alias(context),
                if (show) PackageManager.COMPONENT_ENABLED_STATE_ENABLED
                else PackageManager.COMPONENT_ENABLED_STATE_DISABLED,
                PackageManager.DONT_KILL_APP,
            )
        }.onFailure { Log.w(TAG, "setComponentEnabledSetting failed: $it") }
    }
}
