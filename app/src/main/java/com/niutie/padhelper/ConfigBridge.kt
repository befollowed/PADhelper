package com.niutie.padhelper

import android.content.Context
import android.content.SharedPreferences
import io.github.libxposed.service.XposedService
import io.github.libxposed.service.XposedServiceHelper
import java.util.concurrent.atomic.AtomicBoolean

/**
 * 设置界面与游戏进程之间的桥。
 *
 * **为什么不能直接写 skin.cfg**：`skin.cfg` 在 `/data/user/0/jp.gungho.pad/files/` 下，
 * 属于游戏的 UID。设置界面是另一个 UID，Android 上不可能写进去。
 *
 * 所以走 LSPosed 官方的 remote preferences：
 *  - 设置界面（模块自己的进程）通过 [XposedService.getRemotePreferences] **写**；
 *  - 游戏进程里的 [PadHelperModule] 通过 `XposedInterface.getRemotePreferences` **只读**，
 *    再把它同步成游戏私有目录下的 `skin.cfg`。
 *
 * 原生探针因此完全不用改通信方式，还是读文件 —— 只是这个文件由同进程的 Java 侧代写。
 *
 * 如果框架没连上（模块没启用 / 不是 LSPosed），退化成写本地 SharedPreferences，
 * 界面照样能用，只是设置传不到游戏。这种情况一定要让用户**看得见**，
 * 否则表现就是「开关怎么拨都没反应」，见 [readDiag]。
 */
class ConfigBridge(context: Context) {

    private val app = context.applicationContext
    private val local: SharedPreferences =
        app.getSharedPreferences(LOCAL_PREFS, Context.MODE_PRIVATE)

    private val _connected = AtomicBoolean(false)
    private var remote: SharedPreferences? = null

    /**
     * 服务还没绑上时收到的写入。绑上之后立刻补推。
     *
     * 不补推会怎样：用户在打开界面的头一两秒拨开关，[remote] 还是 null，
     * 写入只落到本地 —— 界面显示「已关闭」，游戏侧却还开着，
     * 而且之后再也不会自动纠正。这是「关了开关但功能没关」的另一条成因。
     */
    @Volatile
    private var pending: CfgState? = null

    val isConnected: Boolean get() = _connected.get()

    fun start(onChanged: () -> Unit) {
        runCatching {
            XposedServiceHelper.registerListener(object : XposedServiceHelper.OnServiceListener {
                override fun onServiceBind(service: XposedService) {
                    runCatching {
                        val rp = service.getRemotePreferences(REMOTE_GROUP)
                        remote = rp
                        val pendingState = pending
                        pending = null
                        // 上次进程里有过「没推到 remote」的写入，标记会留在本地。
                        // 进程被杀也不会丢，下次绑定时补推。
                        val dirty = local.getBoolean(KEY_PENDING_SYNC, false)
                        // 框架侧第一次连上时 remote 是空的。如果直接拿它当权威，
                        // 用户之前存在本地的开关会被「空值」覆盖成全关。
                        // 所以只在 remote 里**还没有任何开关键**时，把本地值灌进去一次。
                        //
                        // 判据用 contains(KEY_SKIN) 而不是 rp.all.isEmpty()：
                        // remote 里还会存诊断用的键（diag_*），那些键会让 all 非空，
                        // 但「开关从没设置过」这件事并没有改变。
                        if (!rp.contains(KEY_SKIN)) {
                            seedRemoteFromLocal(rp)
                            clearPendingSync()
                        } else if (dirty || pendingState != null) {
                            // 本地有更新但没能推到 remote：以本地为准补推
                            pushRemote(rp, pendingState ?: localState())
                            clearPendingSync()
                        } else {
                            // 没有任何待推的本地改动 → remote 才是游戏真正读到的值
                            // （比如重装应用后本地被清空，remote 里还是上次的设置），
                            // 把它抄回本地，界面才不会显示成「全关」而游戏其实是开的。
                            adoptRemoteToLocal(rp)
                            // 旧版本存下的 badge_ack 没有版本号，已经被判为无效。
                            // 这里顺手把「已失效的 ack」修正回 remote，
                            // 否则每次绑定都会重新判一次，永远处于半新半旧的状态。
                            if (rp.getBoolean(KEY_BADGE_ACK, false) != local.getBoolean(KEY_BADGE_ACK, false)) {
                                pushRemote(rp, localState())
                            }
                        }
                        _connected.set(true)
                        onChanged()
                    }
                }

                override fun onServiceDied(service: XposedService) {
                    remote = null
                    _connected.set(false)
                    onChanged()
                }
            })
        }
    }

    fun load(): CfgState = localState()

    /** 本地那份状态。界面启动时先拿它渲染，避免等 binder 时闪一下全关。 */
    private fun localState(): CfgState = CfgState(
        skin = local.getBoolean(KEY_SKIN, false),
        bgm = local.getBoolean(KEY_BGM, false),
        badge = local.getBoolean(KEY_BADGE, false),
        // 只有「确认过风险、且版本对得上」才算已接受。
        // v2.4 之前没有版本号这一说，那些 ack 一律作废 —— 用户上次是在
        // v2.1 的实验性版本里点的「接受」，不代表他知道现在这版也有风险。
        badgeAck = local.getBoolean(KEY_BADGE_ACK, false) && local.getInt(KEY_ACK_VER, 0) >= ACK_VER,
    )

    /** 把本地已有的开关值灌进一个刚建立、还空着的 remote preferences。 */
    private fun seedRemoteFromLocal(rp: SharedPreferences) {
        val ack = local.getBoolean(KEY_BADGE_ACK, false) && local.getInt(KEY_ACK_VER, 0) >= ACK_VER
        rp.edit()
            .putBoolean(KEY_SKIN, local.getBoolean(KEY_SKIN, false))
            .putBoolean(KEY_BGM, local.getBoolean(KEY_BGM, false))
            .putBoolean(KEY_BADGE, local.getBoolean(KEY_BADGE, false))
            .putBoolean(KEY_BADGE_ACK, ack)
            .putInt(KEY_ACK_VER, if (ack) ACK_VER else 0)
            .apply()
    }

    fun save(state: CfgState) {
        // 本地始终写一份，作为「界面自己记住」的兜底。
        // 用 commit 而不是 apply：紧接着的 seedRemoteFromLocal 要读回这些值，
        // apply 是异步落盘，读到的可能还是旧值。
        local.edit()
            .putBoolean(KEY_SKIN, state.skin)
            .putBoolean(KEY_BGM, state.bgm)
            .putBoolean(KEY_BADGE, state.badge)
            .putBoolean(KEY_BADGE_ACK, state.badgeAck)
            .putInt(KEY_ACK_VER, if (state.badgeAck) ACK_VER else 0)
            .commit()

        val rp = remote
        if (rp != null) {
            pushRemote(rp, state)
        } else {
            // 服务还没绑上：记住最后一次意图，绑上时补推。
            // 不这么做的话这次写入永远到不了游戏进程。
            pending = state
            // 同时落一个磁盘标记：进程被系统杀掉也不会丢，下次启动补推。
            local.edit().putBoolean(KEY_PENDING_SYNC, true).commit()
        }
    }

    private fun clearPendingSync() {
        if (local.getBoolean(KEY_PENDING_SYNC, false)) {
            local.edit().putBoolean(KEY_PENDING_SYNC, false).apply()
        }
    }

    /** 把 remote 的值抄回本地。remote 是游戏真正读到的，应以它为准。 */
    private fun adoptRemoteToLocal(rp: SharedPreferences) {
        val ack = rp.getBoolean(KEY_BADGE_ACK, false) && rp.getInt(KEY_ACK_VER, 0) >= ACK_VER
        local.edit()
            .putBoolean(KEY_SKIN, rp.getBoolean(KEY_SKIN, false))
            .putBoolean(KEY_BGM, rp.getBoolean(KEY_BGM, false))
            .putBoolean(KEY_BADGE, rp.getBoolean(KEY_BADGE, false))
            .putBoolean(KEY_BADGE_ACK, ack)
            .putInt(KEY_ACK_VER, if (ack) ACK_VER else 0)
            .commit()
        // 诊断信息只读，缓存到本地，断开连接后还能看到最后一次的状态。
        cacheDiag(rp)
    }

    /** 把一份状态推进 remote。remote 的 apply 是异步的，但内存立即可读。 */
    private fun pushRemote(rp: SharedPreferences, state: CfgState) {
        rp.edit()
            .putBoolean(KEY_SKIN, state.skin)
            .putBoolean(KEY_BGM, state.bgm)
            .putBoolean(KEY_BADGE, state.badge)
            .putBoolean(KEY_BADGE_ACK, state.badgeAck)
            .putInt(KEY_ACK_VER, if (state.badgeAck) ACK_VER else 0)
            .apply()
    }

    // ---------------------------------------------------------------- 诊断

    /**
     * 端到端状态，用来回答「我的开关到底有没有传到游戏里」。
     *
     * 三个时间戳来自不同环节，缺哪一环一眼就能看出来：
     *  - [bindOk]   界面 ↔ 框架：false 说明 LSPosed 没连上，设置根本发不出去；
     *  - [cfgTs]    框架 ↔ 游戏进程：`PadHelperModule` 成功写出 `skin.cfg` 的时间；
     *  - [probeTs]  游戏进程 ↔ 原生探针：探针 `probe.status` 的心跳时间。
     * 时间戳为 0 表示「这一环从来没成功过」。
     */
    data class Diag(
        val bindOk: Boolean,
        val cfgTs: Long,
        val probeTs: Long,
        val probePid: Int,
        val cfgPath: String,
    )

    fun readDiag(): Diag {
        val rp = remote
        if (rp != null) {
            runCatching { cacheDiag(rp) }
            return Diag(
                bindOk = _connected.get(),
                cfgTs = rp.getLong(KEY_DIAG_CFG_TS, 0L),
                probeTs = rp.getLong(KEY_DIAG_PROBE_TS, 0L),
                probePid = rp.getInt(KEY_DIAG_PROBE_PID, 0),
                cfgPath = rp.getString(KEY_DIAG_CFG_PATH, "") ?: "",
            )
        }
        return Diag(
            bindOk = false,
            cfgTs = local.getLong(KEY_DIAG_CFG_TS, 0L),
            probeTs = local.getLong(KEY_DIAG_PROBE_TS, 0L),
            probePid = local.getInt(KEY_DIAG_PROBE_PID, 0),
            cfgPath = local.getString(KEY_DIAG_CFG_PATH, "") ?: "",
        )
    }

    private fun cacheDiag(rp: SharedPreferences) {
        local.edit()
            .putLong(KEY_DIAG_CFG_TS, rp.getLong(KEY_DIAG_CFG_TS, 0L))
            .putLong(KEY_DIAG_PROBE_TS, rp.getLong(KEY_DIAG_PROBE_TS, 0L))
            .putInt(KEY_DIAG_PROBE_PID, rp.getInt(KEY_DIAG_PROBE_PID, 0))
            .putString(KEY_DIAG_CFG_PATH, rp.getString(KEY_DIAG_CFG_PATH, "") ?: "")
            .apply()
    }

    /**
     * 桌面图标开关。**只存本地**，绝不写 remote ——
     * remote 里的四个 key 会被 [CfgSync] 同步成 skin.cfg 交给游戏，
     * 多塞一个 key 进去只会让游戏侧读到无关内容。
     */
    fun loadShowIcon(): Boolean = local.getBoolean(KEY_SHOW_ICON, true)

    fun saveShowIcon(show: Boolean) {
        local.edit().putBoolean(KEY_SHOW_ICON, show).apply()
    }

    companion object {
        /** 与 [PadHelperModule] 里读的是同一个 group。 */
        const val REMOTE_GROUP = "padhelper"

        const val KEY_SKIN = "skin"
        const val KEY_BGM = "bgm"
        const val KEY_BADGE = "badge"
        const val KEY_BADGE_ACK = "badge_ack"

        /**
         * 「已接受徽章风险」这一确认的版本号。
         *
         * 为什么要它：用户以前可能在旧版本里点过「接受风险」，`badge_ack` 就一直是
         * true，于是再打开徽章开关**直接生效、再也不弹提示** —— 用户会以为弹窗坏了。
         * 而且这个 ack 存在 remote 里，会跨版本、跨重装被 [adoptRemoteToLocal] 抄回来，
         * 光清本地没用。
         *
         * 所以给 ack 绑定版本：版本对不上就当作"没接受过"。以后每次想让用户重新确认，
         * 只需要把这个数字 +1。
         */
        const val KEY_ACK_VER = "ack_ver"
        const val ACK_VER = 2

        /** 只存在本地：控制桌面图标是否显示。不参与 remote 同步。 */
        const val KEY_SHOW_ICON = "show_icon"

        /** 诊断：游戏进程最后一次成功写出 skin.cfg 的时间（秒）。 */
        private const val KEY_DIAG_CFG_TS = "diag_cfg_ts"

        /** 诊断：探针 probe.log 的最后更新时间（秒）。 */
        private const val KEY_DIAG_PROBE_TS = "diag_probe_ts"

        /** 诊断：探针所在进程 pid。 */
        private const val KEY_DIAG_PROBE_PID = "diag_probe_pid"

        /** 诊断：实际写入的 skin.cfg 路径。 */
        private const val KEY_DIAG_CFG_PATH = "diag_cfg_path"

        /** 只存在本地：有「没能推到 remote」的写入，等下次绑定时补推。 */
        private const val KEY_PENDING_SYNC = "__pending_sync"

        private const val LOCAL_PREFS = "padhelper_settings"
    }
}
