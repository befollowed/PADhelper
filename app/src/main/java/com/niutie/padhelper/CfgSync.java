package com.niutie.padhelper;

import android.content.SharedPreferences;
import android.util.Log;

import io.github.libxposed.api.XposedModule;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

/**
 * 把设置界面写下的开关同步成游戏私有目录里的 {@code skin.cfg}。
 *
 * <p><b>为什么需要这一层</b>：设置界面是 {@code com.niutie.padhelper}，和游戏
 * {@code jp.gungho.pad} 不是同一个 UID，Android 上写不进对方的私有目录。
 * 所以界面把开关写进 LSPosed 框架的 remote preferences，这里在游戏进程内读出来，
 * 再落到原生探针认识的那个文件上 —— 探针侧完全不用改通信方式。
 *
 * <p><b>必须读-改-写</b>：同一个文件里还有 {@code fast} / {@code hold} /
 * {@code gotauto} / {@code pin ...} 这些手写调试项，只能替换自己管的四个 key，
 * 其余行（含顺序）原样保留。
 */
final class CfgSync {

    /** 与设置界面 {@code ConfigBridge.REMOTE_GROUP} 必须一致。 */
    static final String REMOTE_GROUP = "padhelper";

    static final String KEY_SKIN = "skin";
    static final String KEY_BGM = "bgm";
    static final String KEY_BADGE = "badge";
    static final String KEY_TIMERFIX = "timerfix";
    static final String KEY_BADGE_ACK = "badge_ack";

    private static final String[] MANAGED = {
            KEY_SKIN, KEY_BGM, KEY_BADGE, KEY_TIMERFIX, KEY_BADGE_ACK};

    /**
     * skin.cfg 的格式版本。探针侧 {@code CFG_VER_MIN} 必须与它一致。
     *
     * <p>为什么要有版本行：v2.1/v2.2 时代探针读的是同一个 {@code skin.cfg}，
     * 那份文件里可能残留手工或旧版写入的 {@code skin 1}/{@code bgm 1}。
     * 没有版本标记的话，升级后探针会把这些旧值当成「用户现在要开」，
     * 表现就是「三个开关怎么拨都不生效，皮肤和 BGM 恒解锁」。
     * 带上版本行之后，旧文件一律按全关处理，从根上消灭遗留状态。
     */
    private static final String KEY_CFG_VER = "cfgver";
    private static final String CFG_VER = "1";

    // 诊断键：只写不给探针看。界面读出来显示「链路断在哪一环」。
    static final String KEY_DIAG_CFG_TS = "diag_cfg_ts";
    static final String KEY_DIAG_PROBE_TS = "diag_probe_ts";
    static final String KEY_DIAG_PROBE_PID = "diag_probe_pid";
    static final String KEY_DIAG_CFG_PATH = "diag_cfg_path";

    private final XposedModule module;
    private final File cfgFile;
    private final File probeStatusFile;

    /** 上一次写下去的内容，用来避免每 50ms 无谓地重写文件。 */
    private String lastWritten;

    /** 上一次推送诊断状态的时间（秒），用来节流 binder 写。 */
    private long lastDiagTs;

    CfgSync(XposedModule module, String dataDir) {
        this.module = module;
        // 必须和原生探针的 BASE_DIR_INTERNAL 完全一致：
        // /data/user/0/jp.gungho.pad/files/padhelper/skin.cfg（注意中间的 files/）
        File dir = new File(new File(dataDir, "files"), "padhelper");
        this.cfgFile = new File(dir, "skin.cfg");
        this.probeStatusFile = new File(dir, "probe.status");
    }

    /**
     * 读一次 remote preferences，有变化就重写 skin.cfg。
     *
     * <p>由 {@code PadHelperModule} 的守护线程周期调用。
     *
     * @return true 表示这次成功读到了框架的配置（不论内容是否变化）；
     *         false 表示框架还没就绪，调用方应退避重试。
     */
    boolean syncOnce() {
        final SharedPreferences prefs;
        try {
            prefs = module.getRemotePreferences(REMOTE_GROUP);
        } catch (Throwable t) {
            return false;   // 框架不支持 / 还没就绪
        }
        if (prefs == null) {
            return false;
        }

        String skin = prefs.getBoolean(KEY_SKIN, false) ? "1" : "0";
        String bgm = prefs.getBoolean(KEY_BGM, false) ? "1" : "0";
        String badge = prefs.getBoolean(KEY_BADGE, false) ? "1" : "0";
        String timerfix = prefs.getBoolean(KEY_TIMERFIX, false) ? "1" : "0";
        String ack = prefs.getBoolean(KEY_BADGE_ACK, false) ? "1" : "0";

        String body = skin + "|" + bgm + "|" + badge + "|" + timerfix + "|" + ack;
        boolean wrote = false;
        if (!body.equals(lastWritten)) {
            try {
                writeCfg(skin, bgm, badge, timerfix, ack);
                lastWritten = body;
                wrote = true;
                module.log(Log.INFO, PadHelperModule.TAG,
                        "cfg: skin=" + skin + " bgm=" + bgm + " badge=" + badge
                                + " timerfix=" + timerfix + " ack=" + ack + " -> " + cfgFile);
            } catch (Throwable t) {
                module.log(Log.ERROR, PadHelperModule.TAG, "写 skin.cfg 失败: " + t);
            }
        }
        // 状态回显：把「这次写出去了没有 / 探针进程活着没有」写回框架侧。
        // 仅供日志排查；新版「模块状态」卡片只检测 LSPosed 是否启用本模块，
        // 不再读取这些时间戳。节流到 10 秒一次，避免频繁 binder 写。
        try {
            long now = System.currentTimeMillis() / 1000L;
            if (wrote || now - lastDiagTs >= 10L) {
                lastDiagTs = now;
                pushDiag(prefs, wrote ? now : readLastCfgTs(prefs));
            }
        } catch (Throwable t) {
            module.log(Log.WARN, PadHelperModule.TAG, "写诊断状态失败: " + t);
        }
        return true;
    }

    /** 上一次写出 skin.cfg 的时间（秒）。由 [pushDiag] 自己维护在 remote 里。 */
    private long readLastCfgTs(SharedPreferences prefs) {
        return prefs.getLong(KEY_DIAG_CFG_TS, 0L);
    }

    /**
     * 把诊断信息推进 remote preferences。
     *
     * <p>探针每 10 秒刷新 {@code probe.status}（pid / ts / cfgver / 三个开关），
     * 这里读它、解析、转发。读不到就写 0 —— 界面看到 0 就知道探针还没起来。
     */
    private void pushDiag(SharedPreferences prefs, long cfgTs) {
        int pid = 0;
        long probeTs = 0L;
        try {
            if (probeStatusFile.isFile()) {
                for (String line : Files.readAllLines(probeStatusFile.toPath(),
                        StandardCharsets.UTF_8)) {
                    String t = line.trim();
                    int sp = t.indexOf(' ');
                    if (sp <= 0) {
                        continue;
                    }
                    String k = t.substring(0, sp);
                    String v = t.substring(sp + 1).trim();
                    if ("pid".equals(k)) {
                        pid = parseIntSafe(v);
                    } else if ("ts".equals(k)) {
                        probeTs = parseLongSafe(v);
                    }
                }
            }
        } catch (Throwable ignored) {
            // 状态文件读失败不影响主流程
        }
        prefs.edit()
                .putLong(KEY_DIAG_CFG_TS, cfgTs)
                .putLong(KEY_DIAG_PROBE_TS, probeTs)
                .putInt(KEY_DIAG_PROBE_PID, pid)
                .putString(KEY_DIAG_CFG_PATH, cfgFile.getAbsolutePath())
                .apply();
    }

    private static int parseIntSafe(String s) {
        try {
            return Integer.parseInt(s);
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    private static long parseLongSafe(String s) {
        try {
            return Long.parseLong(s);
        } catch (NumberFormatException e) {
            return 0L;
        }
    }

    private void writeCfg(String skin, String bgm, String badge, String timerfix, String ack)
            throws IOException {
        List<String> lines = new ArrayList<>();
        if (cfgFile.isFile()) {
            lines.addAll(Files.readAllLines(cfgFile.toPath(), StandardCharsets.UTF_8));
        }

        // 先处理版本行：没有就补在第一行，有就改成当前版本。
        // 放在最前面是因为它是「这份文件可信」的声明，人看文件时也最先看到。
        int verIdx = -1;
        for (int j = 0; j < lines.size(); j++) {
            if (KEY_CFG_VER.equals(firstToken(lines.get(j)))) {
                verIdx = j;
                break;
            }
        }
        String verLine = KEY_CFG_VER + " " + CFG_VER;
        if (verIdx >= 0) {
            lines.set(verIdx, verLine);
        } else {
            lines.add(0, verLine);
        }

        String[] values = {skin, bgm, badge, timerfix, ack};
        for (int i = 0; i < MANAGED.length; i++) {
            String key = MANAGED[i];
            int idx = -1;
            for (int j = 0; j < lines.size(); j++) {
                if (key.equals(firstToken(lines.get(j)))) {
                    idx = j;
                    break;
                }
            }
            String line = key + " " + values[i];
            if (idx >= 0) {
                lines.set(idx, line);
            } else {
                lines.add(line);
            }
        }

        File dir = cfgFile.getParentFile();
        if (dir != null && !dir.isDirectory() && !dir.mkdirs()) {
            throw new IOException("无法创建 " + dir);
        }

        // 先写临时文件再改名：探针可能正在同一时刻 stat/fopen 这个文件
        File tmp = new File(dir, "skin.cfg.tmp");
        StringBuilder sb = new StringBuilder();
        for (String l : lines) {
            sb.append(l).append('\n');
        }
        Files.write(tmp.toPath(), sb.toString().getBytes(StandardCharsets.UTF_8));
        if (!tmp.renameTo(cfgFile)) {
            Files.write(cfgFile.toPath(), sb.toString().getBytes(StandardCharsets.UTF_8));
            //noinspection ResultOfMethodCallIgnored
            tmp.delete();
        }
    }

    private static String firstToken(String line) {
        String t = line.trim();
        if (t.isEmpty()) {
            return null;
        }
        int sp = -1;
        for (int i = 0; i < t.length(); i++) {
            if (Character.isWhitespace(t.charAt(i))) {
                sp = i;
                break;
            }
        }
        return sp < 0 ? t : t.substring(0, sp);
    }
}
