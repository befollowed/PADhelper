package com.padhelper;

import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import de.robv.android.xposed.XposedHelpers;

/**
 * BGM 解锁。
 *
 * <p>逆向结论（1.4.x）：PAD 的音乐完全不走 Unity，而是 C++ 核心通过 JNI 回调
 * {@code AppDelegate._izSndAvpPreparePlay(String path, boolean loop, float vol, boolean isAsset)}
 * 请求播放 {@code assets/mon2/bgm_NNN.ogg}。是否"已解锁"由服务器在下发播放请求之前判定，
 * 客户端本地没有等价的门禁，因此只需要把被请求的资源换成目标资源即可。
 *
 * <p>BGM 播放器走 ExoPlayer（{@code qc.g == true}），音频来源是 {@code asset:///<path>}，
 * 真实读取发生在 ExoPlayer 内部，最终仍落到 {@link android.content.res.AssetManager#openFd}。
 * 所以这里做两层：
 * <ol>
 *   <li>在 {@code _izSndAvpPreparePlay} 处直接改 path（最干净，日志可读）；</li>
 *   <li>在 {@code AssetManager.openFd / openNonAssetFd} 处兜底替换（覆盖 C++ 直接 openFd 的情况）。</li>
 * </ol>
 *
 * <p>找不到替换资源时一律保持原样，避免把音乐"弄没"。
 */
final class BgmHooks {
    private final ClassLoader cl;
    private final HookEntry.Config cfg;

    BgmHooks(ClassLoader cl, HookEntry.Config cfg) {
        this.cl = cl;
        this.cfg = cfg;
    }

    void install() {
        if (!cfg.bgmEnabled) {
            HookEntry.log("bgm hooks disabled");
            return;
        }

        try {
            XposedHelpers.findAndHookMethod(HookEntry.APP_DELEGATE, cl, "_izSndAvpPreparePlay",
                    String.class, boolean.class, float.class, boolean.class, new XC_MethodHook() {
                        @Override
                        protected void beforeHookedMethod(MethodHookParam param) {
                            String requested = (String) param.args[0];
                            if (requested == null) {
                                return;
                            }
                            String replacement = resolve(requested);
                            if (replacement != null) {
                                param.args[0] = replacement;
                                HookEntry.log("BGM " + requested + " -> " + replacement);
                            }
                        }
                    });
            HookEntry.log("hooked _izSndAvpPreparePlay");
        } catch (Throwable t) {
            HookEntry.log("PreparePlay hook failed: " + t);
        }

        // ExoPlayer 的 data source 并不会调用 AssetManager.openFd（那是纯 Java 实现），
        // 但仍然挂一层，兼容 MediaPlayer 分支和 C++ 侧的 openFd。
        hookAssetManager();
    }

    private void hookAssetManager() {
        final XC_MethodHook hook = new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                String requested = (String) param.args[0];
                if (requested == null || !requested.contains("bgm")) {
                    return;
                }
                String replacement = resolve(requested);
                if (replacement != null) {
                    param.args[0] = replacement;
                }
            }
        };
        for (String m : new String[]{"openFd", "openNonAssetFd"}) {
            try {
                XposedHelpers.findAndHookMethod(android.content.res.AssetManager.class, m,
                        String.class, hook);
            } catch (Throwable t) {
                HookEntry.log(m + " hook failed: " + t);
            }
        }
    }

    /**
     * 决定某个 BGM 请求要被换成什么。
     *
     * @param requested C++ 传来的路径，例如 {@code mon2/bgm_012.ogg} 或 {@code bgm_012.ogg}
     * @return 新的 assets 路径，等价于不替换时返回 null
     */
    private String resolve(String requested) {
        // 1) 用户显式规则（config.properties 里的 bgm.swap.<原名> = <新名>）
        String explicit = cfg.bgmSwapFor(requested);
        if (explicit != null) {
            if (HookEntry.Util.assetExists(explicit)) {
                return explicit;
            }
            HookEntry.log("swap target missing: " + explicit + " (ignored)");
        }

        // 2) 外部目录无损替换：/sdcard/PADHelper/bgm/<basename>
        File ext = new File(cfg.externalBgmDir, HookEntry.Util.base(requested));
        if (ext.isFile() && ext.length() > 0) {
            String injected = inject(ext);
            if (injected != null) {
                return injected;
            }
        }
        return null;
    }

    /**
     * 把外部音频导入 assets 目录，返回可用的 assets 路径。
     *
     * <p>优先直接写 {@code files/mon2/}（assets 根即 files 目录，游戏普通应用权限即可写入）。
     * 同名文件绝不被覆盖（原地写入正在播放的 asset 可能让 ExoPlayer 读到半截数据），
     * 因此统一以 {@code bgm_ph_<hash>.ogg} 形式落盘。
     */
    private String inject(File src) {
        String name = "mon2/bgm_ph_" + Integer.toHexString((src.getAbsolutePath() + ":" + src.length()
                + ":" + src.lastModified()).hashCode()) + ".ogg";
        File dir = new File(HookEntry.Util.assetDir(), "mon2");
        File dst = new File(HookEntry.Util.assetDir(), name);
        if (dst.isFile() && dst.length() == src.length()) {
            return name;
        }
        if (!dir.isDirectory() && !dir.mkdirs()) {
            HookEntry.log("cannot create " + dir);
            return null;
        }
        if (dst.exists() && !dst.delete()) {
            // 正被播放的旧文件删不掉时换一个名字
            name = name.substring(0, name.length() - 4) + "_" + System.nanoTime() + ".ogg";
            dst = new File(HookEntry.Util.assetDir(), name);
        }
        try (FileInputStream in = new FileInputStream(src);
             FileOutputStream out = new FileOutputStream(dst)) {
            HookEntry.copy(in, out);
            HookEntry.log("injected " + src + " -> " + name);
            return name;
        } catch (IOException e) {
            HookEntry.log("inject failed: " + e);
            return null;
        }
    }
}
