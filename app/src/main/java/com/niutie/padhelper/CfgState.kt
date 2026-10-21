package com.niutie.padhelper

/**
 * 设置界面上三个开关的状态。
 *
 * 全部默认 `false`：模块装好但用户没动过任何开关时，等于「什么也不做」。
 */
data class CfgState(
    val skin: Boolean = false,
    val bgm: Boolean = false,
    val badge: Boolean = false,
    /** 徽章风险提示是否已被接受过；探针不解析这个 key，只有界面自己用。 */
    val badgeAck: Boolean = false,
)
