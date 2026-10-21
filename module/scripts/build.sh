#!/usr/bin/env bash
# Build the PAD Helper LSPosed module without AGP / android.jar stubs.
#
# Pipeline: javac (against the device's own framework.jar + Xposed API 82) -> javac -> D8 -> aapt2 link -> zip assets -> apksigner.
# The signing keystore is generated on first run at module/keystore/padhelper.jks and is NOT committed.
set -euo pipefail

MODULE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# 工程整体搬进了 program/，而 work/（逆向产物、boot 框架包）留在仓库根，
# 所以仓库根是 MODULE_DIR 的上两级，不是上一级。
ROOT_DIR="$(cd "$MODULE_DIR/../.." && pwd)"

# ---- tool locations -------------------------------------------------------
JAVA_HOME="${JAVA_HOME:-$(/usr/libexec/java_home -v 21 2>/dev/null || echo "$HOME/.jdks/jbr-21")}"
JAVAC="$JAVA_HOME/bin/javac"
JAVA="$JAVA_HOME/bin/java"
KEYTOOL="$JAVA_HOME/bin/keytool"

SDK="${SDK:-${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}}}"
BT="${BT:-$SDK/build-tools/36.0.0}"
PLATFORM="${PLATFORM:-$SDK/platforms/android-36}"
AAPT2="${AAPT2:-$BT/aapt2}"; ZIPALIGN="$BT/zipalign"; D8TOOL="$BT/d8"
D8_JAR="$MODULE_DIR/src/lib/r8-8.7.18.jar"
APKSIGNER="${APKSIGNER:-$BT/apksigner}"
ZIP="${ZIP:-/usr/bin/zip}"; ZIPOPT="-j -q"

# framework jars pulled from the device (see README: ./gradlew-free path)
BOOT_DIR="${BOOT_DIR:-$ROOT_DIR/work/boot}"

OUT="$MODULE_DIR/build"
STAGE="$OUT/stage"
rm -rf "$OUT"
mkdir -p "$STAGE/java" "$STAGE/dex" "$STAGE/apk" "$OUT"

for f in "$JAVAC" "$JAVA" "$KEYTOOL" "$D8_JAR"; do
    [ -e "$f" ] || { echo "missing tool: $f" >&2; exit 1; }
done
for f in "$BOOT_DIR/android.test.runner.jar"; do
    [ -f "$f" ] || { echo "missing boot jar: $f (pull with adb as in README)" >&2; exit 1; }
done
[ -x "$AAPT2" ] || { echo "aapt2 not found (set AAPT2=...)" >&2; exit 1; }

BOOTCP="$BOOT_DIR/android.test.runner.jar"

# ---- 1. compile -----------------------------------------------------------
find "$MODULE_DIR/src" -name '*.java' > "$STAGE/sources.txt"
# android.* comes from the device's framework.jar, de.robv.android.xposed.* from API 82 (compile only).
if ! "$JAVAC" -nowarn -encoding UTF-8 -cp "$BOOTCP:$MODULE_DIR/src/lib/api-82.jar:$MODULE_DIR/src/lib/android-stubs-4.1.1.4.jar" -d "$STAGE/java" @"$STAGE/sources.txt"; then
    echo "javac failed" >&2
    exit 1
fi

if ! find "$STAGE/java" -name '*.class' | grep -q .; then
    echo "compile produced no classes" >&2
    exit 1
fi

# ---- 2. dex ---------------------------------------------------------------
"$JAVA" -cp "$D8_JAR" com.android.tools.r8.D8 \
    --min-api 26 --release --output "$STAGE/dex" \
    $(find "$STAGE/java" -name '*.class')

# ---- 3. resources（aapt2 + 真实 android.jar）-------------------------------
ANDROID_JAR="$PLATFORM/android.jar"
"$AAPT2" compile --dir "$MODULE_DIR/res" -o "$STAGE/res.zip"
"$AAPT2" link -o "$STAGE/base.apk" \
    -I "$ANDROID_JAR" \
    --manifest "$MODULE_DIR/AndroidManifest.xml" \
    --min-sdk-version 26 --target-sdk-version 34 \
    "$STAGE/res.zip"

# ---- 4. package -----------------------------------------------------------
cp "$STAGE/dex/classes.dex" "$STAGE/classes.dex"
for a in xposed_init xposed_scope padhelper.properties; do
    cp "$MODULE_DIR/assets/$a" "$STAGE/$a"
done
for a in xposed_init xposed_scope padhelper.properties; do
    (cd "$STAGE" && zip -qj "$STAGE/base.apk" "$a")
done
(cd "$STAGE" && zip -qj "$STAGE/base.apk" classes.dex)
"$ZIPALIGN" -f 4 "$STAGE/base.apk" "$STAGE/aligned.apk"

# ---- 5. sign --------------------------------------------------------------
KEYSTORE="$MODULE_DIR/keystore/padhelper.jks"
if [ ! -f "$KEYSTORE" ]; then
    mkdir -p "$(dirname "$KEYSTORE")"
    "$KEYTOOL" -genkeypair -keystore "$KEYSTORE" -alias padhelper -keyalg RSA -keysize 2048 \
        -validity 10000 -storepass padhelper -keypass padhelper \
        -dname "CN=PAD Helper, O=local" >/dev/null 2>&1
fi
"$APKSIGNER" sign --ks "$KEYSTORE" --ks-pass pass:padhelper --key-pass pass:padhelper \
    --min-sdk-version 26 \
    --out "$OUT/PADHelper.apk" "$STAGE/aligned.apk"
"$APKSIGNER" verify --verbose "$OUT/PADHelper.apk" | head -5

echo "done: $OUT/PADHelper.apk"
