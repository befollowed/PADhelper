package com.niutie.padhelper

/**
 * 设置界面上四个开关的状态。
 *
 * 全部默认 `false`：模块装好但用户没动过任何开关时，等于「什么也不做」。
 */
data class CfgState(
    val skin: Boolean = false,
    val bgm: Boolean = false,
    val badge: Boolean = false,
    /**
     * 计时本防退：计时关卡切后台再回前台后，倒计时会永久变成 `---.--`。
     * 打开后探针会把「计时暂停」标志位清掉，让倒计时继续走。
     * 见 `native_probe.c` 里 `timer_apply()` 的注释。
     */
    val timerfix: Boolean = false,
    /** 徽章风险提示是否已被接受过；探针不解析这个 key，只有界面自己用。 */
    val badgeAck: Boolean = false,
)
