package com.padhelper;

import android.util.Log;

import io.github.libxposed.api.XposedInterface;
import io.github.libxposed.api.XposedModule;
import io.github.libxposed.api.XposedModuleInterface.ModuleLoadedParam;
import io.github.libxposed.api.XposedModuleInterface.PackageReadyParam;

import java.lang.reflect.Method;

/**
 * libxposed API 102 入口模块（新版格式，LSPosed 2.x 通过 META-INF/xposed/ 识别）。
 *
 * <p>目标：《智龙迷城》jp.gungho.pad。游戏逻辑在原生库里，但 BGM 播放要经 Java 侧
 * {@code AppDelegate._izSndAvpPreparePlay(String path, boolean loop, float vol, boolean asset)}，
 * ExoPlayer 最终按 {@code asset:///<path>} 读取 {@code files/} 下的资源。
 * 这里改写请求路径，把「未解锁的 BGM」换成目标音频。
 */
public final class PadHelperModule extends XposedModule {

    static final String TAG = "PADHelper";
    static final String TARGET_PKG = "jp.gungho.pad";
    static final String APP_DELEGATE = "jp.gungho.pad.AppDelegate";

    private volatile boolean hooksInstalled;

    @Override
    public void onModuleLoaded(ModuleLoadedParam param) {
        if (param.isSystemServer()) {
            detach();
            return;
        }
        log(Log.INFO, TAG, "loaded process=" + param.getProcessName()
                + " framework=" + getFrameworkName() + ' ' + getFrameworkVersion()
                + " api=" + getApiVersion());
    }

    @Override
    public void onPackageReady(PackageReadyParam param) {
        if (!param.isFirstPackage() || hooksInstalled) {
            return;
        }
        String pkg = param.getPackageName();
        if (!TARGET_PKG.equals(pkg)) {
            return;
        }
        hooksInstalled = true;
        log(Log.INFO, TAG, "target=" + pkg + "，开始安装 hook");
        try {
            installHooks(this, param.getClassLoader(), param.getApplicationInfo());
            loadNativeProbe(this, param.getApplicationInfo());
            // 触发方式只剩「体力变化」一种，由原生探针 sta_watch() 自行处理，
            // 不再需要音量键/触摸坐标这类 Java 侧触发器。
            log(Log.INFO, TAG, "hook 安装完成");
        } catch (Throwable t) {
            log(Log.ERROR, TAG, "hook 安装失败", t);
        }
    }

    // ------------------------------------------------------------- native probe

    /**
     * 把模块 assets 里的原生库拷进游戏私有目录并加载。
     *
     * <p>目的：验证「能在游戏进程内跑原生代码 + 能解析 libpad.so 的内存布局」，
     * 这是后续对原生判定函数做 inline hook 的前提。当前这个 .so 只读不写，
     * 只打印 libpad.so 的基址与可执行段范围。
     */
    private static void loadNativeProbe(XposedModule module,
                                        android.content.pm.ApplicationInfo appInfo) {
        try {
            // 模块 APK 路径：直接当 zip 读，不依赖 Context（onPackageReady 时 Application 还没创建）
            String moduleApk = module.getModuleApplicationInfo().sourceDir;
            java.io.File dir = new java.io.File(appInfo.dataDir, "files/padhelper");
            if (!dir.isDirectory() && !dir.mkdirs()) {
                module.log(Log.ERROR, TAG, "无法创建 " + dir);
                return;
            }
            java.io.File so = new java.io.File(dir, "libpadhelper-probe.so");
            try (java.util.zip.ZipFile zip = new java.util.zip.ZipFile(moduleApk)) {
                java.util.zip.ZipEntry entry = zip.getEntry("assets/libpadhelper-probe.so");
                if (entry == null) {
                    module.log(Log.ERROR, TAG, "模块 APK 里没有 assets/libpadhelper-probe.so");
                    return;
                }
                try (java.io.InputStream in = zip.getInputStream(entry);
                     java.io.FileOutputStream out = new java.io.FileOutputStream(so)) {
                    byte[] buf = new byte[64 * 1024];
                    int n;
                    while ((n = in.read(buf)) > 0) {
                        out.write(buf, 0, n);
                    }
                }
            }
            System.load(so.getAbsolutePath());
            module.log(Log.INFO, TAG, "原生探针已加载 " + so);
        } catch (Throwable t) {
            module.log(Log.ERROR, TAG, "原生探针加载失败: " + t);
        }
    }

    // ------------------------------------------------------------------ hooks

    private static void installHooks(XposedModule module, ClassLoader cl,
                                     android.content.pm.ApplicationInfo appInfo) {
        Config cfg = Config.load(module, appInfo);
        if (!cfg.bgmEnabled) {
            module.log(Log.INFO, TAG, "BGM 功能已关闭（config: bgm.enabled=false）");
            return;
        }

        // 1) 主入口：C++ 核心请求播放 BGM 时改道
        hookPreparePlay(module, cl, cfg);

        // 2) 兜底：兼容 MediaPlayer 分支 / 原生侧直接 openFd 的情况
        hookAssetManager(module, cl, cfg);
    }

    private static void hookPreparePlay(XposedModule module, ClassLoader cl, Config cfg) {
        try {
            Class<?> cls = Class.forName(APP_DELEGATE, false, cl);
            int count = 0;
            for (Method m : cls.getDeclaredMethods()) {
                if (!"_izSndAvpPreparePlay".equals(m.getName())) {
                    continue;
                }
                module.hook(m)
                        .setPriority(XposedInterface.PRIORITY_HIGHEST)
                        .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                        .setId(TAG + ".preparePlay." + count)
                        .intercept(chain -> {
                            Object[] args = chain.getArgs().toArray();
                            if (args.length > 0 && args[0] instanceof String) {
                                String requested = (String) args[0];
                                String replacement = cfg.resolve(module, requested);
                                if (replacement != null) {
                                    args[0] = replacement;
                                    module.log(Log.INFO, TAG, "BGM " + requested + " -> " + replacement);
                                }
                            }
                            return chain.proceed(args);
                        });
                count++;
            }
            module.log(Log.INFO, TAG, "hooked _izSndAvpPreparePlay（" + count + " 个重载）");
        } catch (Throwable t) {
            module.log(Log.ERROR, TAG, "找不到 " + APP_DELEGATE + ": " + t);
        }
    }

    private static void hookAssetManager(XposedModule module, ClassLoader cl, Config cfg) {
        try {
            Class<?> am = Class.forName("android.content.res.AssetManager", false, cl);
            int count = 0;
            for (String name : new String[]{"openFd", "openNonAssetFd", "open"}) {
                try {
                    Method m = am.getMethod(name, String.class);
                    module.hook(m)
                            .setPriority(XposedInterface.PRIORITY_DEFAULT)
                            .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                            .setId(TAG + ".asset." + name)
                            .intercept(chain -> {
                                Object arg = chain.getArg(0);
                                if (arg instanceof String && ((String) arg).contains("bgm")) {
                                    String replacement = cfg.resolve(module, (String) arg);
                                    if (replacement != null) {
                                        return chain.proceedWith(replacement);
                                    }
                                }
                                return chain.proceed();
                            });
                    count++;
                } catch (NoSuchMethodException ignored) {
                    // 该 ROM 没有这个方法，跳过
                }
            }
            module.log(Log.INFO, TAG, "hooked AssetManager（" + count + " 个方法）");
        } catch (Throwable t) {
            module.log(Log.ERROR, TAG, "AssetManager hook 失败: " + t);
        }
    }
}
