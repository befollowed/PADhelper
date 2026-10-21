# libxposed 通过 java_init.list 里的类名反射加载模块入口，
# R8 看不到这条引用，必须显式保留，否则模块装上后「启用但无任何效果」。
-keep class com.niutie.padhelper.PadHelperModule { *; }

# 模块里用到的 libxposed API 基类与回调接口
-keep class io.github.libxposed.api.** { *; }
-keepclassmembers class * implements io.github.libxposed.api.XposedInterface$Hooker {
    <methods>;
}

# CfgSync 通过反射拿 XposedModule.getRemotePreferences / log，
# 且游戏进程里的 RemotePreferences 来自框架、不在本 APK 内
-keep class com.niutie.padhelper.CfgSync { *; }
-keepclassmembers class * {
    @io.github.libxposed.api.annotations.* <methods>;
}

# 保留行号，崩溃日志可读
-keepattributes SourceFile,LineNumberTable
-renamesourcefileattribute SourceFile
