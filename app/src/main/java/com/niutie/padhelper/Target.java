package com.niutie.padhelper;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;

/**
 * 适配版本闸门。
 *
 * <p>本模块的存档偏移、GOT 槽、请求格式全是照着 {@code jp.gungho.pad} v23.5.2 的
 * 二进制写死的：换一个版本轻则功能失效（偏移全错），重则写坏存档或触发服务端校验。
 * 所以策略是「只认这一个版本」，两条闸门：
 *
 * <ul>
 *   <li>游戏进程内：版本不符直接 {@code return}，cfg 同步、Java hook、原生探针
 *       一个都不加载 —— 宁可不生效，也不能带着错偏移去改内存。</li>
 *   <li>设置界面：版本不符只显示提示卡片，所有开关置灰只读，且
 *       {@code SettingsViewModel.commit()} 里再兜一层，绝不写配置。</li>
 * </ul>
 *
 * <p>判定只看 versionName：versionCode 会随渠道、重打包变化，内容一致就以版本名为准；
 * versionCode 仍会读出来放进 {@link Info}，仅用于诊断展示。
 */
public final class Target {

    /** 目标包名。 */
    public static final String PKG = "jp.gungho.pad";

    /** 唯一适配的版本。改成新版本前必须重新核对全部偏移。 */
    public static final String VERSION_NAME = "23.5.2";

    /** 同版本的 versionCode，仅用于日志/诊断。 */
    public static final long VERSION_CODE = 230503L;

    private Target() {
    }

    /** 版本名是否匹配。 */
    public static boolean matches(String versionName) {
        return VERSION_NAME.equals(versionName);
    }

    /**
     * 游戏进程内使用。
     *
     * <p>注意 {@link ApplicationInfo} 里<b>没有</b> versionName 字段（只有 sourceDir
     * 这类路径信息），而 onPackageReady 时机太早、拿不到 Context 去问 PackageManager。
     * 所以直接读本进程 APK 的二进制清单：{@link ApkManifest} 只解析出 versionName，
     * 不依赖隐藏 API，也不受包可见性限制。
     */
    public static boolean supported(ApplicationInfo info) {
        if (info == null || info.sourceDir == null) {
            return false;
        }
        return matches(ApkManifest.versionName(info.sourceDir));
    }

    /** 设置界面一次查询的结果。 */
    public static final class Info {
        /** 有没有装这个包（或者有没有权限看到它）。 */
        public final boolean installed;
        /** 版本是否匹配。 */
        public final boolean supported;
        /** 实际读到的 versionName，读不到时为 null。 */
        public final String versionName;
        /** 实际读到的 versionCode，读不到时为 0。 */
        public final long versionCode;

        Info(boolean installed, boolean supported, String versionName, long versionCode) {
            this.installed = installed;
            this.supported = supported;
            this.versionName = versionName;
            this.versionCode = versionCode;
        }
    }

    /**
     * 设置界面调用。
     *
     * <p><b>注意</b>：Android 11+ 的包可见性要求模块 manifest 里声明
     * {@code <queries><package android:name="jp.gungho.pad"/></queries>}，
     * 否则这里永远抛 {@link PackageManager.NameNotFoundException}，
     * 界面会误报「未检测到游戏」并把设置锁死。
     */
    public static Info query(Context context) {
        try {
            PackageManager pm = context.getPackageManager();
            PackageInfo pi = pm.getPackageInfo(PKG, 0);
            String name = pi.versionName;
            return new Info(true, matches(name), name, pi.getLongVersionCode());
        } catch (Throwable t) {
            return new Info(false, false, null, 0L);
        }
    }
}
