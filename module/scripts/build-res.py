#!/usr/bin/env python3
"""在没有 Android SDK (aapt2 / android.jar) 的机器上完成模块的资源打包。

做三件事：
  1. 把清单里的资源引用（@string/app_name 等）换成硬编码值，生成可用的 AndroidManifest.xml；
  2. 生成最小但合法的 resources.arsc（模块用到的字符串 + xposedscope 数组）；
  3. 产出只含 AndroidManifest.xml + resources.arsc 的 base.apk，供 build.sh 继续追加 dex/assets。

resources.arsc 结构参考 frameworks/base/libs/androidfw/include/androidfw/ResourceTypes.h，
这里只写「单一默认配置 + 单个包」的最简形态。

用法: build-res.py <stage目录> <待改写的AndroidManifest.xml路径>
"""
import os
import struct
import sys
import zipfile

PACKAGE = "com.padhelper"
PACKAGE_ID = 0x7F

STRINGS = {
    "app_name": "PAD Helper",
    "xposed_description": "让未持有角色也能使用其解锁的背景音乐/皮肤（实验性，配合 LSPosed 使用）",
}
ARRAYS = {"xposedscope": ["jp.gungho.pad"]}

RES_STRING_POOL_TYPE = 0x0001
RES_TABLE_TYPE = 0x0002
RES_TABLE_PACKAGE_TYPE = 0x0200
RES_TABLE_TYPE_TYPE = 0x0201
RES_TABLE_TYPE_SPEC_TYPE = 0x0202

TYPE_STRING = 0x03

# meta-data 的 value 只能用字符串，没有资源 ID 时用 valueType="string" 强制成字符串
MANIFEST_SUBSTITUTIONS = {
    "@string/app_name": "PAD Helper",
    "@string/xposed_description": STRINGS["xposed_description"],
    "@array/xposedscope": "jp.gungho.pad",
}


def _chunk(kind, header_size, body, extra=b""):
    size = header_size + len(extra) + len(body)
    return struct.pack("<HHII", kind, header_size, size, 0) + extra + body


def _utf8_len(n):
    if n < 0x80:
        return bytes([n])
    return bytes([0x80 | (n >> 8), n & 0xFF])


def string_pool(strings):
    """RES_STRING_POOL_TYPE，UTF-8 编码（aapt2 的默认输出格式）。"""
    data = b""
    offsets = []
    for s in strings:
        offsets.append(len(data))
        raw = s.encode("utf-8")
        data += _utf8_len(len(s.encode("utf-16-le")) // 2) + _utf8_len(len(raw)) + raw + b"\x00"
    if len(data) % 4:
        data += b"\x00" * (4 - len(data) % 4)
    header_size = 28
    strings_start = header_size + 4 * len(strings)
    body = b"".join(struct.pack("<I", strings_start + o) for o in offsets) + data
    extra = struct.pack("<IIIII", len(strings), 0, 0x0001 | 0x0100, strings_start, len(data))
    return _chunk(RES_STRING_POOL_TYPE, header_size, body, extra)


def type_spec(type_id, entry_count):
    return _chunk(RES_TABLE_TYPE_SPEC_TYPE, 16, b"\x00" * (4 * entry_count),
                  struct.pack("<BBH", type_id, 0, 0))


def type_chunk(type_id, entries):
    """entries: [(key_index, simple_data, complex_entry_bytes_or_None)]，按 entry 下标排列。"""
    entry_count = len(entries)
    offsets_size = 2 * entry_count
    blob = b""
    offsets = []
    for _key, data, complex_ in entries:
        offsets.append(offsets_size + len(blob))
        blob += complex_ if complex_ else struct.pack("<HHI", 8, 0, data & 0xFFFFFFFF)
    if len(blob) % 4:
        blob += b"\x00" * (4 - len(blob) % 4)
    config = b"\x00" * 48  # ResTable_config：全 0 表示默认配置
    header_size = 20 + len(config)
    body = b"".join(struct.pack("<H", o) for o in offsets) + blob
    extra = struct.pack("<II", entry_count, 0) + config
    return _chunk(RES_TABLE_TYPE_TYPE, header_size, body, extra)


def complex_array(value_indices):
    body = struct.pack("<I", len(value_indices)) + b"".join(
        struct.pack("<HBBI", 8, 0, TYPE_STRING, i) for i in value_indices)
    return struct.pack("<HHI", 16, 0, 0) + body  # ResTable_map_entry


def build_arsc():
    values = [STRINGS[n] for n in sorted(STRINGS)]
    for n in sorted(ARRAYS):
        values.extend(ARRAYS[n])
    value_index = {v: i for i, v in enumerate(values)}

    keys = sorted(list(STRINGS) + list(ARRAYS))
    key_index = {k: i for i, k in enumerate(keys)}

    array_entries = [(key_index[n], 0, complex_array([value_index[v] for v in ARRAYS[n]]))
                     for n in keys if n in ARRAYS]
    string_entries = [(key_index[n], value_index[STRINGS[n]], None)
                      for n in keys if n in STRINGS]

    # typeId 从 1 开始连续分配：array=0x01, string=0x02
    type_pool = string_pool(["array", "string"])
    key_pool = string_pool(keys)
    types = b"".join([
        type_spec(0x01, len(array_entries)), type_chunk(0x01, array_entries),
        type_spec(0x02, len(string_entries)), type_chunk(0x02, string_entries),
    ])

    header_size = 288
    type_strings_off = header_size
    key_strings_off = type_strings_off + len(type_pool)
    name = PACKAGE.encode("utf-8") + b"\x00"
    name += b"\x00" * ((4 - len(name) % 4) % 4)
    # id(4) + name[128] + typeStrings(4) + lastPublicType(4) + keyStrings(4) +
    # lastPublicKey(4) + typeIdOffset(4)  ->  aapt2 会把头部补到 288 字节
    header = struct.pack("<I", PACKAGE_ID) + name + b"\x00" * (128 - len(name))
    header += struct.pack("<IIII", type_strings_off, 0x02, key_strings_off, 0)
    header += struct.pack("<I", 0)  # typeIdOffset
    header += b"\x00" * (header_size - len(header))

    pkg_body = type_pool + key_pool + types
    pkg = struct.pack("<HHII", RES_TABLE_PACKAGE_TYPE, header_size,
                      header_size + len(pkg_body), PACKAGE_ID) + header + pkg_body

    globals_pool = string_pool(values)
    body = globals_pool + pkg
    return struct.pack("<HHII", RES_TABLE_TYPE, 12, 12 + len(body), 1) + body


def rewrite_manifest(path):
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    for ref, value in MANIFEST_SUBSTITUTIONS.items():
        text = text.replace('"%s"' % ref, '"%s" valueType="string"' % value)
    if "@" in text.split("<application")[0].split("<manifest")[1]:
        raise SystemExit("manifest references an unknown resource")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    stage, manifest = sys.argv[1], sys.argv[2]
    rewrite_manifest(manifest)
    arsc = build_arsc()
    # 用自带的确定式 zip 封装，避免平台 zipfile 压缩流差异（见 scripts/ziputil.py）
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import ziputil
    with open(manifest, "rb") as fh:
        manifest_bytes = fh.read()
    # 落盘一份 resources.arsc 供打包脚本使用；base.apk 保留作中间产物便于排查
    with open(os.path.join(stage, "resources.arsc"), "wb") as fh:
        fh.write(arsc)
    ziputil.write_zip(os.path.join(stage, "base.apk"),
                      [("AndroidManifest.xml", manifest_bytes), ("resources.arsc", arsc)])
    print("base.apk: resources.arsc=%d bytes" % len(arsc))


if __name__ == "__main__":
    main()
