#!/usr/bin/env bash
# 用 aapt2 + d8 + apksigner 直接构建 libxposed API 102 模块（不依赖 Gradle/AGP）。
#
# 为什么不用 Gradle：本机离线缓存里没有 AGP 的 plugin marker，wrapper 又要联网下载发行包；
# 而 aapt2/d8/apksigner 都在 SDK 里，直接调它们更可靠、更快。
#
# 依赖：JDK 11+、Android SDK（build-tools 36.0.0 + platform android-36）、
#       libxposed api-102.0.0.aar（Gradle 缓存或手动指定 LX_AAR）。
#
# 可用环境变量覆盖：SDK、JAVA_HOME、LX_AAR、KEYSTORE
#   SDK=<sdk路径> LX_AAR=<api-102.0.0.aar 路径> bash app/build.sh
#
# 产物：build-output/PADHelper-v<ver>.apk
set -euo pipefail

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$APP_DIR/.." && pwd)"

JAVAC="${JAVAC:-${JAVA_HOME:-/usr/lib/jvm/default-java}/bin/javac}"
KEYTOOL="${KEYTOOL:-$(dirname "$JAVAC")/keytool}"
[ -x "$JAVAC" ] || JAVAC=javac
[ -x "$KEYTOOL" ] || KEYTOOL=keytool

SDK="${SDK:-${ANDROID_HOME:-$HOME/Android/Sdk}}"
BT="$SDK/build-tools/36.0.0"
PLATFORM="${PLATFORM:-$SDK/platforms/android-36}"
AAPT2="$BT/aapt2"
ZIPALIGN="$BT/zipalign"
APKSIGNER="$BT/apksigner"
ANDROID_JAR="$PLATFORM/android.jar"

# libxposed API 102（从 Gradle 缓存里取，避免联网）
LX_AAR="${LX_AAR:-$(find "$HOME/.gradle/caches/modules-2/files-2.1/io.github.libxposed" \
    -name 'api-102.0.0.aar' 2>/dev/null | head -1)}"

OUT="$APP_DIR/build"
STAGE="$OUT/stage"
rm -rf "$OUT"
mkdir -p "$STAGE/classes" "$STAGE/dex" "$APP_DIR/build-output"

[ -f "$ANDROID_JAR" ] || { echo "missing android.jar: $ANDROID_JAR" >&2; exit 1; }
[ -f "$LX_AAR" ] || { echo "missing libxposed aar; 设置 LX_AAR=..." >&2; exit 1; }

# ---- 1. 依赖 jar ----------------------------------------------------------
mkdir -p "$STAGE/lib"
( cd "$STAGE/lib" && unzip -o -q "$LX_AAR" classes.jar && mv classes.jar libxposed-api-102.jar )

# ---- 2. 编译 --------------------------------------------------------------
find "$APP_DIR/src/main/java" -name '*.java' > "$STAGE/sources.txt"
"$JAVAC" -nowarn -encoding UTF-8 --release 11 \
    -cp "$ANDROID_JAR:$STAGE/lib/libxposed-api-102.jar" \
    -d "$STAGE/classes" @"$STAGE/sources.txt"

# ---- 3. dex ---------------------------------------------------------------
# 注意：API jar 只能用于编译（compileOnly），绝不能作为 d8 输入，
# 否则 libxposed 的类会被打进 dex，LSPosed 会以
# "The Xposed API classes are compiled into the module's APK" 拒绝加载模块。
"$BT/d8" --min-api 26 --release --lib "$ANDROID_JAR" \
    --classpath "$STAGE/lib/libxposed-api-102.jar" \
    --output "$STAGE/dex" \
    $(find "$STAGE/classes" -name '*.class')

# ---- 4. 资源 + 清单 -------------------------------------------------------
"$AAPT2" compile --dir "$APP_DIR/src/main/res" -o "$STAGE/res.zip"
"$AAPT2" link -o "$STAGE/base.apk" \
    -I "$ANDROID_JAR" \
    --manifest "$APP_DIR/src/main/AndroidManifest.xml" \
    --min-sdk-version 26 --target-sdk-version 35 \
    "$STAGE/res.zip"

# ---- 5. 追加 dex / assets / META-INF/xposed -------------------------------
cp "$STAGE/dex/classes.dex" "$STAGE/classes.dex"
mkdir -p "$STAGE/meta/META-INF/xposed"
cp "$APP_DIR/src/main/resources/META-INF/xposed/"* "$STAGE/meta/META-INF/xposed/"

# assets 必须保持 assets/ 前缀（getAssets().open("x") 就等价于打开 assets/x），
# 所以这里不能用 -j，直接把整个 assets 目录按相对路径塞进去。
rm -rf "$STAGE/assets" && cp -R "$APP_DIR/src/main/assets" "$STAGE/assets"

( cd "$STAGE" && zip -qj base.apk classes.dex >/dev/null )
( cd "$STAGE" && zip -qr base.apk assets >/dev/null )
( cd "$STAGE/meta" && zip -qr "$STAGE/base.apk" META-INF/xposed )

"$ZIPALIGN" -f 4 "$STAGE/base.apk" "$STAGE/aligned.apk"

# ---- 6. 签名 --------------------------------------------------------------
KEYSTORE="${KEYSTORE:-$ROOT_DIR/keystore/debug.jks}"
if [ ! -f "$KEYSTORE" ]; then
    mkdir -p "$(dirname "$KEYSTORE")"
    "$KEYTOOL" -genkeypair -keystore "$KEYSTORE" -alias padhelper -keyalg RSA -keysize 2048 \
        -validity 10000 -storepass padhelper -keypass padhelper \
        -dname "CN=PAD Helper, O=local" >/dev/null 2>&1
fi
VERSION="1.0.0"
FINAL="$APP_DIR/build-output/PADHelper-v${VERSION}.apk"
"$APKSIGNER" sign --ks "$KEYSTORE" --ks-pass pass:padhelper --key-pass pass:padhelper \
    --min-sdk-version 26 --out "$FINAL" "$STAGE/aligned.apk"
"$APKSIGNER" verify --verbose "$FINAL" | head -4

echo "done: $FINAL"
