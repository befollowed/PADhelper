package com.niutie.padhelper;

import android.util.Log;

import io.github.libxposed.api.XposedModule;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.HashMap;
import java.util.Map;
import java.util.Properties;

/**
 * 模块配置。两处来源，后读的覆盖先读的：
 * <ol>
 *   <li>模块 APK 内 {@code assets/padhelper.properties}</li>
 *   <li>设备上 {@code /data/data/jp.gungho.pad/padhelper/config.properties}（需 root）</li>
 * </ol>
 *
 * <pre>
 * bgm.enabled = true
 * bgm.dir     = /sdcard/PADHelper/bgm
 * bgm.swap.bgm_012.ogg = bgm_101.ogg
 * </pre>
 */
final class Config {

    boolean bgmEnabled = true;
    String externalDir = "/sdcard/PADHelper/bgm";
    /** 游戏私有数据目录（onPackageReady 时由 ApplicationInfo 提供）。 */
    String dataDir = "/data/data/jp.gungho.pad";
    final Map<String, String> swaps = new HashMap<>();

    static Config load(XposedModule module, android.content.pm.ApplicationInfo appInfo) {
        Config c = new Config();
        c.dataDir = appInfo.dataDir;
        Properties p = new Properties();
        try {
            // 模块自身 assets：直接当 zip 读模块 APK，避免依赖 Context
            String moduleApk = module.getModuleApplicationInfo().sourceDir;
            try (java.util.zip.ZipFile zip = new java.util.zip.ZipFile(moduleApk)) {
                java.util.zip.ZipEntry e = zip.getEntry("assets/padhelper.properties");
                if (e != null) {
                    try (InputStream in = zip.getInputStream(e)) {
                        p.load(in);
                    }
                    module.log(Log.INFO, PadHelperModule.TAG, "读取模块内置配置");
                }
            }
        } catch (Throwable t) {
            module.log(Log.WARN, PadHelperModule.TAG, "无内置配置: " + t.getMessage());
        }
        File onDisk = new File(c.dataDir, "padhelper/config.properties");
        if (onDisk.isFile()) {
            try (InputStream in = new FileInputStream(onDisk)) {
                p.load(in);
                module.log(Log.INFO, PadHelperModule.TAG, "读取设备配置 " + onDisk);
            } catch (Throwable t) {
                module.log(Log.ERROR, PadHelperModule.TAG, "设备配置读取失败: " + t.getMessage());
            }
        } else {
            module.log(Log.INFO, PadHelperModule.TAG, "未找到 " + onDisk + "，仅使用内置/默认配置");
        }
        c.merge(p, module);
        return c;
    }

    private void merge(Properties p, XposedModule module) {
        bgmEnabled = truthy(p.getProperty("bgm.enabled"), bgmEnabled);
        String dir = p.getProperty("bgm.dir");
        if (dir != null && !dir.trim().isEmpty()) {
            externalDir = dir.trim();
        }
        for (String key : p.stringPropertyNames()) {
            if (!key.startsWith("bgm.swap.")) {
                continue;
            }
            String from = normalize(key.substring("bgm.swap.".length()));
            String to = normalize(p.getProperty(key).trim());
            if (!to.isEmpty()) {
                swaps.put(from, to);
                module.log(Log.INFO, PadHelperModule.TAG, "规则: " + from + " -> " + to);
            }
        }
    }

    private static boolean truthy(String v, boolean def) {
        if (v == null) {
            return def;
        }
        v = v.trim().toLowerCase();
        return v.equals("1") || v.equals("true") || v.equals("yes") || v.equals("on");
    }

    /** "bgm_012.ogg" -> "mon2/bgm_012.ogg"（ExoPlayer 的资产根是应用 files/ 目录）。 */
    static String normalize(String name) {
        String n = name.trim();
        if (n.isEmpty() || n.contains("/")) {
            return n;
        }
        return n.startsWith("bgm") ? "mon2/" + n : n;
    }

    static String baseName(String path) {
        int i = path.lastIndexOf('/');
        return i < 0 ? path : path.substring(i + 1);
    }

    // ---------------------------------------------------------------- resolve

    /**
     * 决定某个 BGM 请求要被换成什么。
     *
     * @param requested 原生侧传来的路径，如 {@code mon2/bgm_012.ogg} 或 {@code bgm_012.ogg}
     * @return 新的 assets 路径；不替换时返回 null
     */
    String resolve(XposedModule module, String requested) {
        if (requested == null) {
            return null;
        }
        String norm = normalize(requested);

        // 1) 显式规则
        String target = swaps.get(norm);
        if (target != null && assetExists(module, target)) {
            return target;
        }

        // 2) 外部目录同名文件
        File ext = new File(externalDir, baseName(norm));
        if (ext.isFile() && ext.length() > 0) {
            return inject(module, ext);
        }
        return null;
    }

    private boolean assetExists(XposedModule module, String asset) {
        // 游戏 assets 的根就是 files/ 目录，直接判文件更稳（此时还拿不到 Context）
        File f = new File(dataDir, "files/" + asset);
        if (f.isFile()) {
            return true;
        }
        module.log(Log.WARN, PadHelperModule.TAG, "目标资源不存在: " + asset);
        return false;
    }

    /**
     * 把外部音频导入应用 files/mon2/ 下（相同内容复用），返回可用的资产路径。
     * 永不覆盖正在播放的文件，统一用 bgm_ph_&lt;hash&gt;.ogg 命名。
     */
    private String inject(XposedModule module, File src) {
        String name = "mon2/bgm_ph_" + Integer.toHexString(
                (src.getAbsolutePath() + ":" + src.length() + ":" + src.lastModified()).hashCode()) + ".ogg";
        try {
            File dir = new File(dataDir, "files/mon2");
            File dst = new File(dataDir, "files/" + name);
            if (dst.isFile() && dst.length() == src.length()) {
                return name;
            }
            if (!dir.isDirectory() && !dir.mkdirs()) {
                module.log(Log.ERROR, PadHelperModule.TAG, "无法创建目录 " + dir);
                return null;
            }
            if (dst.exists() && dst.length() != src.length()) {
                name = name.substring(0, name.length() - 4) + "_" + System.nanoTime() + ".ogg";
                dst = new File(dataDir, "files/" + name);
            }
            try (FileInputStream in = new FileInputStream(src);
                 FileOutputStream out = new FileOutputStream(dst)) {
                byte[] buf = new byte[64 * 1024];
                int n;
                while ((n = in.read(buf)) > 0) {
                    out.write(buf, 0, n);
                }
            }
            module.log(Log.INFO, PadHelperModule.TAG, "注入 " + src + " -> " + name);
            return name;
        } catch (Throwable e) {
            module.log(Log.ERROR, PadHelperModule.TAG, "注入失败: " + e);
            return null;
        }
    }

}
