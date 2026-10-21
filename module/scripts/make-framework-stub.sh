#!/usr/bin/env bash
# 用 aapt2 生成 link 需要的 framework-res.apk（没有真机、取不到系统 framework-res 时使用）。
# 覆盖 PAD 会用到的资源类型；其它类型的 id 可能对不上，只用于本地冒烟测试。
set -euo pipefail

MODULE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT_DIR="$(cd "$MODULE_DIR/../.." && pwd)"   # 仓库根（program/ 的上一级）；work/ 在那里
OUT="${1:-$ROOT_DIR/work/boot/framework-res.apk}"
SDK="${SDK:-${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}}}"
BT="${BT:-$SDK/build-tools/36.0.0}"
AAPT2="$BT/aapt2"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/AndroidManifest.xml" <<'XML'
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="android" >
    <!-- @id @uses-sdk @application -->
    <application />
</manifest>
XML

mkdir -p "$TMP/res/values"
cat > "$TMP/res/values/attrs.xml" <<'XML'
<resources>
    <attr name="id" format="reference" />
    <attr name="name" format="string" />
    <attr name="label" format="reference|string" />
    <attr name="icon" format="reference" />
    <attr name="value" format="reference|string|boolean|integer|float|color" />
    <attr name="resource" format="reference" />
    <attr name="theme" format="reference" />
    <attr name="enabled" format="boolean" />
    <attr name="exported" format="boolean" />
    <attr name="allowBackup" format="boolean" />
    <attr name="configChanges" format="flags" />
    <attr name="required" format="boolean" />
    <attr name="minSdkVersion" format="integer" />
    <attr name="targetSdkVersion" format="integer" />
    <attr name="compileSdkVersion" format="integer" />
    <attr name="compileSdkVersionCodename" format="string" />
    <attr name="screenOrientation" format="integer" />
    <attr name="stateNotNeeded" format="boolean" />
    <attr name="writable" format="boolean" />
    <attr name="authorities" format="string" />
    <attr name="valueType" format="string" />
</resources>
XML
cat > "$TMP/res/values/public.xml" <<'XML'
<resources>
    <public type="attr" name="id" id="0x010100d0" />
    <public type="attr" name="name" id="0x01010003" />
    <public type="attr" name="label" id="0x01010001" />
    <public type="attr" name="icon" id="0x01010002" />
    <public type="attr" name="value" id="0x01010024" />
    <public type="attr" name="resource" id="0x01010025" />
    <public type="attr" name="theme" id="0x01010000" />
    <public type="attr" name="enabled" id="0x0101000e" />
    <public type="attr" name="exported" id="0x01010010" />
    <public type="attr" name="allowBackup" id="0x01010280" />
    <public type="attr" name="configChanges" id="0x0101001f" />
    <public type="attr" name="required" id="0x0101028e" />
    <public type="attr" name="minSdkVersion" id="0x0101020c" />
    <public type="attr" name="targetSdkVersion" id="0x01010270" />
    <public type="attr" name="compileSdkVersion" id="0x01010572" />
    <public type="attr" name="compileSdkVersionCodename" id="0x01010573" />
    <public type="attr" name="screenOrientation" id="0x0101001e" />
    <public type="attr" name="stateNotNeeded" id="0x0101021a" />
    <public type="attr" name="writable" id="0x01010064" />
    <public type="attr" name="authorities" id="0x01010018" />
    <!-- 以下是清单里非 framework 的纯名字属性，随便给 id，只为让 aapt2 认识 -->
    <public type="attr" name="package" id="0x7f999001" />
    <public type="attr" name="valueType" id="0x7f999002" />
    <public type="style" name="Theme" id="0x01030000" />
    <public type="style" name="Theme.DeviceDefault" id="0x0103000b" />
    <public type="style" name="Theme.DeviceDefault.NoActionBar" id="0x0103000c" />
    <public type="style" name="Theme.DeviceDefault.Light" id="0x0103000d" />
    <public type="style" name="Theme.DeviceDefault.Light.NoActionBar" id="0x0103000e" />
    <public type="style" name="Theme.Material" id="0x01030010" />
    <public type="style" name="Theme.Material.NoActionBar" id="0x01030011" />
    <public type="style" name="Theme.Material.Light" id="0x01030012" />
    <public type="style" name="Theme.Material.Light.NoActionBar" id="0x01030013" />
    <public type="style" name="Theme.Black.NoTitleBar" id="0x01030008" />
</resources>
XML
cat > "$TMP/res/values/styles.xml" <<'XML'
<resources>
    <style name="Theme" />
    <style name="Theme.DeviceDefault" parent="Theme" />
    <style name="Theme.DeviceDefault.NoActionBar" parent="Theme.DeviceDefault" />
    <style name="Theme.DeviceDefault.Light" parent="Theme.DeviceDefault" />
    <style name="Theme.DeviceDefault.Light.NoActionBar" parent="Theme.DeviceDefault.Light" />
    <style name="Theme.Material" parent="Theme" />
    <style name="Theme.Material.NoActionBar" parent="Theme.Material" />
    <style name="Theme.Material.Light" parent="Theme.Material" />
    <style name="Theme.Material.Light.NoActionBar" parent="Theme.Material.Light" />
    <style name="Theme.Black.NoTitleBar" parent="Theme" />
</resources>
XML
# 常用颜色（游戏代码里 getColor 0x01060000 起，这一批只保证不会找不到符号）
mkdir -p "$TMP/res/values"
{
    echo '<resources>'
    for i in $(seq 0 0x30); do printf '  <color name="color_%d">#ff000000</color>\n' "$i"; done
    echo '</resources>'
} > "$TMP/res/values/colors.xml"

"$AAPT2" compile --dir "$TMP/res" -o "$TMP/res.zip"
"$AAPT2" link -o "$OUT" -I "$TMP/res.zip" \
    --manifest "$TMP/AndroidManifest.xml" \
    --no-static-lib-packages
echo "framework-res: $OUT"
