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
    /**
     * 「重新计时」子开关，只在 [timerfix] 打开时有意义。
     *
     * - `true`：杀进程重进限时本时，倒计时从满值重新开始（v2.9.5 及以前的行为）。
     * - `false`（默认）：按墙钟自然流逝 —— 剩余 = 上限 − (现在 − 进入关卡的时刻)。
     *   中途退出多久就少多少，真的耗尽就判负结束。
     *
     * 见 `native_probe.c` 里 `timer_apply()` 的注释。
     */
    val retimer: Boolean = false,
    /** 徽章风险提示是否已被接受过；探针不解析这个 key，只有界面自己用。 */
    val badgeAck: Boolean = false,
)
