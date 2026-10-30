package com.niutie.padhelper.ui

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.niutie.padhelper.CfgState
import com.niutie.padhelper.ConfigBridge
import com.niutie.padhelper.Target
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

/**
 * 开关状态机。
 *
 * 两条硬性约束：
 * 1. 写配置必须串行 —— 快速连续拨动开关时，多个协程并发「读-改-写」会互相覆盖。
 *    这里用一个 [Channel] 把写入排成队列，同一时刻只有一个写操作。
 * 2. 徽章「关 -> 想开」且 `badge_ack == 0` 时**不能**直接改状态：先弹窗，
 *    开关视觉保持「关」，等用户点「接受风险」才真正打开。
 */
class SettingsViewModel(app: Application) : AndroidViewModel(app) {

    private val bridge = ConfigBridge(app)

    private val _state = MutableStateFlow(bridge.load())
    val state: StateFlow<CfgState> = _state.asStateFlow()

    /**
     * 版本闸门。本模块只适配 23.6.0：版本不符时界面提示、开关只读，
     * [commit] 里再兜一层拒绝写入。见 [com.niutie.padhelper.Target]。
     */
    private val _gate = MutableStateFlow(Target.query(app))
    val gate: StateFlow<Target.Info> = _gate.asStateFlow()

    private val _badgeDialogVisible = MutableStateFlow(false)
    val badgeDialogVisible: StateFlow<Boolean> = _badgeDialogVisible.asStateFlow()

    private val _connected = MutableStateFlow(false)
    val connected: StateFlow<Boolean> = _connected.asStateFlow()

    /**
     * 链路诊断。开关「拨了没反应」时，这张卡片是唯一能定位断点的东西：
     * 界面↔框架 / 框架↔游戏进程 / 游戏进程↔探针，三段各自有时间戳。
     */
    private val _diag = MutableStateFlow(bridge.readDiag())
    val diag: StateFlow<ConfigBridge.Diag> = _diag.asStateFlow()

    /**
     * 桌面图标开关。**独立于 [CfgState]**：它只影响本机桌面，
     * 不参与 remote preferences，也不该被 CfgSync 同步给游戏。
     * 初值以系统的组件实际状态为准，不信 SharedPreferences
     * （用户可能在系统设置里手动停用过）。
     */
    private val _showIcon = MutableStateFlow(LauncherIcon.isEnabled(app))
    val showIcon: StateFlow<Boolean> = _showIcon.asStateFlow()

    private val writes = Channel<CfgState>(Channel.UNLIMITED)

    init {
        bridge.start {
            _connected.value = bridge.isConnected
            // 连上框架后以远端为准刷新一次界面
            if (bridge.isConnected) _state.value = bridge.load()
            _diag.value = bridge.readDiag()
        }
        viewModelScope.launch(Dispatchers.IO) {
            for (snapshot in writes) {
                runCatching { bridge.save(snapshot) }
                // 写完之后补一次诊断刷新：用户拨开关的下一个瞬间就能看到
                // 「cfg 写出时间」有没有前进，从而判断这次写入到没到游戏侧。
                _diag.value = bridge.readDiag()
            }
        }
    }

    /** 用户手动刷新诊断（下拉卡片上的"刷新"按钮）。 */
    fun refreshDiag() {
        _diag.value = bridge.readDiag()
        // 版本可能被用户在后台升级/降级，顺手重查一次；界面每 2 秒调一次本方法。
        _gate.value = Target.query(getApplication())
    }

    fun setSkin(enabled: Boolean) = commit(_state.value.copy(skin = enabled))

    fun setBgm(enabled: Boolean) = commit(_state.value.copy(bgm = enabled))

    /**
     * 计时本防退。没有风险提示：它只清游戏自己的「计时暂停」标志位，
     * 不改数值、不改代码，也不影响计分。
     */
    fun setTimerFix(enabled: Boolean) = commit(_state.value.copy(timerfix = enabled))

    /**
     * 「重新计时」子开关，只在「限时本防退」打开时有意义。
     *
     * 打开 = 重进限时本时倒计时从满值重新开始（旧行为，会白送时间）；
     * 关闭（默认）= 按墙钟自然流逝，退出多久就少多少，耗尽即判负。
     */
    fun setReTimer(enabled: Boolean) = commit(_state.value.copy(retimer = enabled))

    fun setShowIcon(show: Boolean) {
        _showIcon.value = show
        LauncherIcon.apply(getApplication<Application>(), show)
        bridge.saveShowIcon(show)
    }

    /**
     * 徽章开关的点击。关闭永远直接生效；打开**每次都先弹风险提示**。
     *
     * 为什么不复用「已接受过」的历史状态：v2.3/v2.4 用 `badge_ack`（以及后来的
     * `ack_ver`）记住「用户接受过风险」，本意是少打扰。但实际结果是 —— 只要这个
     * 标记曾经被写进 SharedPreferences 或 remote preferences，之后**再也看不到提示**，
     * 用户会认为弹窗坏了；而这份标记还会跨版本、跨重装被 remote 抄回来，清都清不干净。
     *
     * 徽章会改战斗数值、有封号风险，每次开启都确认一遍的成本远低于「以为没有风险」。
     * 所以 v2.5 起改成无条件提示：只有在弹窗里点「接受风险」才真正打开。
     * `badgeAck` 仍然照常写（留作记录），但不再参与「要不要弹窗」的决策。
     */
    fun onBadgeToggle(enabled: Boolean) {
        val current = _state.value
        if (!enabled) {
            commit(current.copy(badge = false))
            return
        }
        // 先不打开：等弹窗结果。开关视觉保持「关」，避免提示还没看清就生效。
        _badgeDialogVisible.value = true
    }

    fun acceptBadgeRisk() {
        _badgeDialogVisible.value = false
        commit(_state.value.copy(badge = true, badgeAck = true))
    }

    fun dismissBadgeDialog() {
        _badgeDialogVisible.value = false
    }

    private fun commit(next: CfgState) {
        // 版本不符：界面开关本来就是灰的，但状态也可能从远端被改过，
        // 这里再兜一层 —— 只要版本不对，就绝不写配置、也不改内存状态。
        if (!_gate.value.supported) return
        _state.value = next
        writes.trySend(next)
    }
}
