package com.padhelper;

import android.app.AndroidAppHelper;
import android.content.res.AssetManager;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Properties;
import java.util.TreeSet;

import de.robv.android.xposed.IXposedHookLoadPackage;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import de.robv.android.xposed.XposedHelpers;
import de.robv.android.xposed.callbacks.XC_LoadPackage;

/**
 * LSPosed 入口。类名在 assets/xposed_init 中登记。
 *
 * <p>作用域：jp.gungho.pad（《智龙迷城》）。
 * 所有 hook 都做了异常隔离：任何失败只写日志，不影响游戏原逻辑。
 */
public final class HookEntry implements IXposedHookLoadPackage {

    static final String TAG = "PADHelper";
    static final String PAD = "jp.gungho.pad";
    static final String APP_DELEGATE = "jp.gungho.pad.AppDelegate";

    @Override
    public void handleLoadPackage(XC_LoadPackage.LoadPackageParam p) {
        if (!PAD.equals(p.packageName)) {
            return;
        }
        final ClassLoader cl = p.classLoader;
        final Config cfg;
        try {
            cfg = Config.load(cl);
        } catch (Throwable t) {
            log("config load failed: " + Log.getStackTraceString(t));
            return;
        }
        log("loaded (bgm=" + cfg.bgmEnabled + ", skins=" + cfg.skinEnabled + ")");

        try {
            new BgmHooks(cl, cfg).install();
        } catch (Throwable t) {
            log("bgm hooks failed: " + Log.getStackTraceString(t));
        }
        if (cfg.skinEnabled) {
            try {
                new SkinHooks(cl, cfg).install();
            } catch (Throwable t) {
                log("skin hooks failed: " + Log.getStackTraceString(t));
            }
        }
        try {
            new AssetProbe(cl).install();
        } catch (Throwable t) {
            log("probe hook failed: " + t);
        }
    }

    static void log(String msg) {
        Log.i(TAG, msg);
        XposedBridge.log("[" + TAG + "] " + msg);
    }

    // =============================================================================================
    // Config
    // =============================================================================================

    /**
     * 配置来源（优先级从高到低）：
     * <ol>
     *   <li>{@code /data/data/jp.gungho.pad/padhelper/config.properties}（设备上有 root 时直接编辑）</li>
     *   <li>本模块 APK 内 {@code assets/padhelper.properties}（改完重新安装模块即可，无需 root）</li>
     * </ol>
     * 任一项都不存在时使用内置默认值。
     */
    static final class Config {
        boolean bgmEnabled = true;
        boolean skinEnabled = false;
        /** 无损：外部音乐目录，例如 /sdcard/PADHelper/bgm */
        String externalBgmDir = "/sdcard/PADHelper/bgm";
        /** 原始资源路径 -> 替换后的资源路径（仅在找到该资源时生效） */
        final List<String[]> bgmSwaps = new ArrayList<>();
        /** 皮肤资源名：原始名 -> 替换名 */
        final List<String[]> skinSwaps = new ArrayList<>();

        static Config load(ClassLoader cl) {
            Config c = new Config();
            Properties own = new Properties();
            AssetManager am = AndroidAppHelper.currentApplication().getAssets();
            try (InputStream in = am.open("padhelper.properties")) {
                own.load(in);
                log("config: module assets/padhelper.properties");
            } catch (Exception ignored) {
                // 未随模块打包，使用默认值
            }
            Properties disk = new Properties();
            File onDisk = new File(Util.dataDir(), "padhelper/config.properties");
            if (onDisk.isFile()) {
                try (FileInputStream in = new FileInputStream(onDisk)) {
                    disk.load(in);
                    log("config: " + onDisk);
                } catch (IOException e) {
                    log("config read failed: " + e);
                }
            }
            c.merge(own);
            c.merge(disk);
            return c;
        }

        private void merge(Properties p) {
            bgmEnabled = bool(p, "bgm.enabled", bgmEnabled);
            skinEnabled = bool(p, "skin.enabled", skinEnabled);
            externalBgmDir = p.getProperty("bgm.dir", externalBgmDir).trim();
            for (String k : p.stringPropertyNames()) {
                String v = p.getProperty(k).trim();
                if (v.isEmpty()) {
                    continue;
                }
                if (k.startsWith("bgm.swap.")) {
                    bgmSwaps.add(new String[]{k.substring("bgm.swap.".length()).trim(), v});
                } else if (k.startsWith("skin.swap.")) {
                    skinSwaps.add(new String[]{k.substring("skin.swap.".length()).trim(), v});
                }
            }
        }

        private static boolean bool(Properties p, String key, boolean def) {
            String v = p.getProperty(key);
            if (v == null) {
                return def;
            }
            v = v.trim().toLowerCase();
            return v.equals("1") || v.equals("true") || v.equals("yes") || v.equals("on");
        }

        /** 解析 BGM 替换：支持 "bgm_012.ogg" 或 "mon2/bgm_012.ogg" 两种写法，统一成 assets 路径。 */
        String bgmSwapFor(String requestedPath) {
            if (requestedPath == null) {
                return null;
            }
            String req = Util.normalizeAsset(requestedPath);
            for (String[] s : bgmSwaps) {
                if (Util.normalizeAsset(s[0]).equals(req)) {
                    return Util.normalizeAsset(s[1]);
                }
            }
            return null;
        }

        String skinSwapFor(String requestedAsset) {
            for (String[] s : skinSwaps) {
                if (s[0].equals(requestedAsset)) {
                    return s[1];
                }
            }
            return null;
        }
    }

    // =============================================================================================
    // Helpers
    // =============================================================================================

    static final class Util {
        private Util() {
        }

        static File dataDir() {
            return new File(AndroidAppHelper.currentApplication().getApplicationInfo().dataDir);
        }

        /** assets 目录（games 下载的资源都在这里：files/mon2/bgm_xxx.ogg 在 assets 中即 "mon2/bgm_xxx.ogg"）。 */
        static File assetDir() {
            return new File(dataDir(), "files");
        }

        /** "mon2/bgm_012.ogg" <-> "bgm_012.ogg" 统一为 assets 相对路径。 */
        static String normalizeAsset(String name) {
            String n = name.trim();
            if (n.startsWith("/")) {
                File f = new File(n);
                File dir = assetDir();
                String abs = f.getAbsolutePath();
                if (abs.startsWith(dir.getAbsolutePath() + "/")) {
                    return abs.substring(dir.getAbsolutePath().length() + 1);
                }
                return n;
            }
            if (n.indexOf('/') < 0 && n.startsWith("bgm_")) {
                return "mon2/" + n;
            }
            return n;
        }

        static boolean assetExists(String asset) {
            try {
                AndroidAppHelper.currentApplication().getAssets().openFd(asset).close();
                return true;
            } catch (Exception e) {
                return false;
            }
        }

        static String base(String path) {
            int i = path.lastIndexOf('/');
            return i < 0 ? path : path.substring(i + 1);
        }

        static TreeSet<String> lines(String text) {
            TreeSet<String> out = new TreeSet<>();
            if (text == null) {
                return out;
            }
            for (String l : text.split("\n")) {
                l = l.trim();
                if (!l.isEmpty() && !l.startsWith("#")) {
                    out.add(l);
                }
            }
            return out;
        }

    }

    /** 记录游戏中实际被打开的资源，帮助用户填写规则（日志中加 [PADHelper] 前缀）。 */
    static final class AssetProbe {
        private final ClassLoader cl;

        AssetProbe(ClassLoader cl) {
            this.cl = cl;
        }

        void install() {
            final TreeSet<String> seen = new TreeSet<>();
            try {
                XposedHelpers.findAndHookMethod(AssetManager.class, "openFd",
                        String.class, new XC_MethodHook() {
                            @Override
                            protected void beforeHookedMethod(MethodHookParam param) {
                                String name = (String) param.args[0];
                                if (name != null && (name.contains("bgm_") || name.contains("skci_")
                                        || name.contains("skse_") || name.contains("st_"))
                                        && seen.add(name)) {
                                    log("asset " + name);
                                }
                            }
                        });
            } catch (Throwable t) {
                log("openFd probe failed: " + t);
            }
            try {
                XposedHelpers.findAndHookMethod(AssetManager.class, "open",
                        String.class, new XC_MethodHook() {
                            @Override
                            protected void beforeHookedMethod(MethodHookParam param) {
                                String name = (String) param.args[0];
                                if (name != null && name.contains("sk") && seen.add(name)) {
                                    log("asset " + name);
                                }
                            }
                        });
            } catch (Throwable t) {
                log("open probe failed: " + t);
            }
        }
    }

    static void copy(InputStream in, FileOutputStream out) throws IOException {
        byte[] buf = new byte[64 * 1024];
        int n;
        while ((n = in.read(buf)) > 0) {
            out.write(buf, 0, n);
        }
    }
}
