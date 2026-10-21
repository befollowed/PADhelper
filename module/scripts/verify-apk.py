#!/usr/bin/env python3
"""校验签名后的 APK：zip 结构、v1（JAR）清单摘要、v2（APK Signature Scheme）签名。

用法: verify-apk.py <apk>
退出码非 0 表示校验失败，可用于 CI/构建脚本。

v2 校验要点：
  1. EOCD 之前紧挨着 APK Signing Block，块两端 size 相同、结尾是固定 magic；
  2. 块内 0x7109871A 记录里的 signed_data 用证书公钥验证 RSA 签名；
  3. signed_data 里声明的分块 SHA-256 摘要与条目数据区重新计算的结果一致。
只要第 3 步通过，就说明「数据区没有被改动」，这等价于 Android 安装器的核心检查。
"""
import base64
import hashlib
import struct
import sys

sys_path_hint = "（需要与 sign-apk.py 放在同一目录）"


def find_eocd(blob):
    pos = blob.rfind(b"PK\x05\x06")
    if pos < 0:
        raise SystemExit("不是 zip：找不到 EOCD")
    return pos


def central_dir_info(blob):
    """返回 (eocd 偏移, 条目数, 中央目录大小, 中央目录偏移)。

    EOCD 布局：4 签名 + 4 磁盘号 + 4 中央目录起始盘号 + 2 本条记录 + 2 总记录
               + 4 大小 + 4 偏移；所以条目数在 +10，大小 +12，偏移 +16。
    """
    eocd = find_eocd(blob)
    count, cd_size, cd_off = struct.unpack_from("<HII", blob, eocd + 10)
    return eocd, count, cd_size, cd_off


def check_zip(blob):
    eocd, count, cd_size, cd_off = central_dir_info(blob)
    if cd_off + cd_size != eocd:
        raise SystemExit("中央目录长度与实际不符（%d + %d != %d）" % (cd_off, cd_size, eocd))
    pos = cd_off
    names = []
    for _ in range(count):
        if blob[pos:pos + 4] != b"PK\x01\x02":
            raise SystemExit("中央目录记录损坏")
        nlen, elen, clen = struct.unpack_from("<HHH", blob, pos + 28)
        off = struct.unpack_from("<I", blob, pos + 42)[0]
        names.append(blob[pos + 46:pos + 46 + nlen].decode())
        if blob[off:off + 4] != b"PK\x03\x04":
            raise SystemExit("条目 %s 的 local header 偏移错误（offset=%d, cd_off=%d, magic=%s）"
                             % (names[-1], off, cd_off, blob[off:off + 4].hex()))
        pos += 46 + nlen + elen + clen
    return names, cd_off


def read_entries(blob, names):
    """返回 {name: bytes}（只用于 v1 摘要校验）。"""
    import zlib
    eocd, count, cd_size, cd_off = central_dir_info(blob)
    out = {}
    pos = cd_off
    for _ in range(count):
        (method,) = struct.unpack_from("<H", blob, pos + 10)
        crc, csize, usize = struct.unpack_from("<III", blob, pos + 16)
        nlen, elen, clen = struct.unpack_from("<HHH", blob, pos + 28)
        off = struct.unpack_from("<I", blob, pos + 42)[0]
        name = blob[pos + 46:pos + 46 + nlen].decode()
        lnlen, lelen = struct.unpack_from("<HH", blob, off + 26)
        start = off + 30 + lnlen + lelen
        raw = blob[start:start + csize]
        data = zlib.decompress(raw, -15) if method == 8 else raw
        if (zlib.crc32(data) & 0xFFFFFFFF) != crc:
            raise SystemExit("条目 %s CRC 错误" % name)
        out[name] = data
        pos += 46 + nlen + elen + clen
    return out


def check_v1(entries):
    mf = entries.get("META-INF/MANIFEST.MF")
    sf = entries.get("META-INF/CERT.SF")
    if not mf or not sf or "META-INF/CERT.RSA" not in entries:
        raise SystemExit("缺少 JAR 签名文件（MANIFEST.MF/CERT.SF/CERT.RSA）")

    # 逐节核对：每个条目的 SHA-256 摘要
    detail = {}
    for section in mf.decode("utf-8").split("\r\n\r\n"):
        if not section.strip():
            continue
        fields = {}
        for line in section.replace("\r\n ", "").split("\r\n"):
            if ":" in line:
                k, v = line.split(":", 1)
                fields[k.strip()] = v.strip()
        if "Name" in fields and "SHA-256-Digest" in fields:
            detail[fields["Name"]] = fields["SHA-256-Digest"]
    for name, digest in detail.items():
        if name not in entries:
            raise SystemExit("清单列出的条目 %s 不存在" % name)
        if base64.b64encode(hashlib.sha256(entries[name]).digest()).decode() != digest:
            raise SystemExit("条目 %s 的摘要与 MANIFEST.MF 不符" % name)
    if not detail:
        raise SystemExit("MANIFEST.MF 里没有任何条目摘要")

    # CERT.SF 里记录整个清单的摘要
    sf_text = sf.decode("utf-8").replace("\r\n ", "")
    want = base64.b64encode(hashlib.sha256(mf).digest()).decode()
    if want not in sf_text:
        raise SystemExit("CERT.SF 的 SHA-256-Digest-Manifest 与 MANIFEST.MF 不符")
    return len(detail)


def read_seq(data, pos):
    """读 APK Signature Scheme 的 sequence，返回 (内容 bytes, 结束位置)。"""
    (length,) = struct.unpack_from("<I", data, pos)
    return data[pos + 4:pos + 4 + length], pos + 4 + length


def iter_seq(body):
    pos = 0
    while pos + 4 <= len(body):
        item, pos = read_seq(body, pos)
        yield item


def find_signing_block(blob, cd_off):
    """返回签名块的 pair 区域（不含两端 size 与 magic）。

    文件布局：[size:8][pair:8+value][size:8][magic:16]
    按规范，两个 size 字段的值都等于「从第一个 size 字段到 magic 起点」的字节数（含首尾 size）。
    """
    if blob[cd_off - 16:cd_off] != b"APK Sig Block 42":
        raise SystemExit("中央目录前没有 APK Signing Block")
    (size,) = struct.unpack_from("<Q", blob, cd_off - 24)
    # 块在文件里占 size + 16 字节（magic 额外 16，不计入 size）
    start = cd_off - size - 16
    if start < 0 or struct.unpack_from("<Q", blob, start)[0] != size:
        raise SystemExit("APK Signing Block 两端长度不一致")
    return blob[start + 8:cd_off - 24]


def check_v2(block, blob, cd_off):
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import padding

    signer = None
    pos = 0
    while pos + 12 <= len(block):
        (pair_len,) = struct.unpack_from("<Q", block, pos)
        (pair_id,) = struct.unpack_from("<I", block, pos + 8)
        value = block[pos + 12:pos + 8 + pair_len]
        if pair_id == 0x7109871A:
            signer = value
            break
        pos += 8 + pair_len
    if signer is None:
        raise SystemExit("签名块里没有 v2 签名记录")

    signers = list(iter_seq(signer))
    if not signers:
        raise SystemExit("v2 签名记录为空")
    parts = list(iter_seq(signers[0]))
    signed_data, signatures, public_key = parts[0], parts[1], parts[2]

    # 用证书公钥验证 signed_data
    signed_parts = list(iter_seq(signed_data))
    certs = list(iter_seq(signed_parts[1]))
    cert = x509.load_der_x509_certificate(certs[0])
    sig_entries = list(iter_seq(signatures))
    entry = sig_entries[0]
    (sig_alg,) = struct.unpack_from("<I", entry, 0)
    (sig_bytes,) = iter_seq(entry[4:]) if False else (None,)
    sig_bytes, _ = read_seq(entry, 4)
    if sig_alg != 0x0103:
        raise SystemExit("只支持 RSA PKCS#1 SHA-256（实际 0x%x）" % sig_alg)
    cert.public_key().verify(sig_bytes, signed_data, padding.PKCS1v15(), hashes.SHA256())

    # 重算数据区摘要并比对
    digests = list(iter_seq(signed_parts[0]))
    (alg,) = struct.unpack_from("<I", digests[0], 0)
    (expected,) = iter_seq(digests[0][4:]) if False else (None,)
    expected, _ = read_seq(digests[0], 4)
    content = blob[:find_signing_block_start(blob, cd_off)]
    if _content_digest(content) != expected:
        raise SystemExit("v2 内容摘要不匹配（数据区被改动）")
    return alg, len(public_key)


def _content_digest(content):
    digests = []
    for start in range(0, len(content), 1024 * 1024):
        chunk = content[start:start + 1024 * 1024]
        digests.append(hashlib.sha256(struct.pack("<BI", 0xA5, len(chunk)) + chunk).digest())
    return hashlib.sha256(struct.pack("<BI", 0x5A, len(digests)) + b"".join(digests)).digest()


def find_signing_block_start(blob, cd_off):
    (size,) = struct.unpack_from("<Q", blob, cd_off - 24)
    return cd_off - size - 16


def main():
    if len(sys.argv) not in (2, 3):
        print(__doc__ + sys_path_hint)
        sys.exit(2)
    blob = open(sys.argv[1], "rb").read()
    names, cd_off = check_zip(blob)
    if "--zip-only" in sys.argv:
        print("%s: zip OK (%d 条目)" % (sys.argv[1], len(names)))
        return
    entries = read_entries(blob, names)
    signed = check_v1(entries)
    alg, keylen = check_v2(find_signing_block(blob, cd_off), blob, cd_off)
    print("%s: OK (%d 条目, v1 摘要 %d 个, v2 算法 0x%x, 公钥 %d 字节)"
          % (sys.argv[1], len(names), signed, alg, keylen))


if __name__ == "__main__":
    main()
