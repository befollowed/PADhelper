package com.padhelper;

import android.content.res.AssetManager;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;

import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedHelpers;

/**
 * 皮肤解锁（实验性，默认关闭：config.properties 里 skin.enabled=false）。
 *
 * <p>皮肤资源在 PAD 里是 {@code skci_N.bin}（卡片立绘图集）与 {@code skse_N.bin}（音效），
 * 配合 {@code st_N.txt} 描述。与 BGM 不同，皮肤"归属哪张卡"通常写在存档/服务器下发数据里，
 * 因此单纯替换资源只能让当前设备看到别的皮肤图案，不能改变服务端认定。
 * 更稳妥的用法是用 {@code skin.swap.<目标资源>=<你已有角色的资源>} 做"资源借用"：
 * 组合界面请求不到目标资源时，用已有角色的图集填充，从而在本地看到可用皮肤。
 *
 * <p>实现方式：exact -> 仅当检测到目标资源名被打开时，改写成替换资源名。
 */
final class SkinHooks {
    private final ClassLoader cl;
    private final HookEntry.Config cfg;

    SkinHooks(ClassLoader cl, HookEntry.Config cfg) {
        this.cl = cl;
        this.cfg = cfg;
    }

    void install() {
        if (cfg.skinSwaps.isEmpty()) {
            HookEntry.log("skin hooks enabled but no skin.swap.* rules");
        }
        final XC_MethodHook rewrite = new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                String requested = (String) param.args[0];
                if (requested == null) {
                    return;
                }
                String replacement = cfg.skinSwapFor(requested);
                if (replacement != null) {
                    param.args[0] = replacement;
                    HookEntry.log("SKIN " + requested + " -> " + replacement);
                }
            }
        };
        for (String m : new String[]{"openFd", "open", "openNonAssetFd"}) {
            try {
                if (m.equals("open")) {
                    XposedHelpers.findAndHookMethod(AssetManager.class, m, String.class, rewrite);
                } else {
                    XposedHelpers.findAndHookMethod(AssetManager.class, m, String.class, rewrite);
                }
            } catch (Throwable t) {
                HookEntry.log("skin " + m + " hook failed: " + t);
            }
        }

        // 备用：从外部目录注入自制皮肤图集（/sdcard/PADHelper/skin/<资源名>）
        injectExternalSkins();
    }

    private void injectExternalSkins() {
        File dir = new File("/sdcard/PADHelper/skin");
        File[] files = dir.listFiles();
        if (files == null) {
            return;
        }
        File assets = HookEntry.Util.assetDir();
        for (File f : files) {
            if (!f.isFile()) {
                continue;
            }
            File dst = new File(assets, "mon2/" + f.getName());
            if (dst.isFile()) {
                continue;
            }
            try (FileOutputStream out = new FileOutputStream(dst)) {
                java.io.FileInputStream in = new java.io.FileInputStream(f);
                try {
                    HookEntry.copy(in, out);
                } finally {
                    in.close();
                }
                HookEntry.log("skin injected " + f.getName());
            } catch (IOException e) {
                HookEntry.log("skin inject failed: " + e);
            }
        }
    }
}
