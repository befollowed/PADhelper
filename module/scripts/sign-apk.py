#!/usr/bin/env python3
"""给 APK 追加 JAR（v1）与 APK Signature Scheme v2 签名（纯 Python，不依赖 android.jar）。

背景：本机没有 android.jar，清单只能保持明文 XML，apksigner 无法解析，故自建签名器。

最终布局（v2 规范）：
    [条目数据][APK Signing Block][中央目录][EOCD]

难点与解法：
  * 签名块的长度取决于它自己的内容，而它里面包含的「条目偏移」又取决于块的长度；
  * 上一版实现直接把块长写死成常量，一旦真实长度不同（本机实测 1482），中央目录里
    所有偏移都会自相矛盾 → 安装时报 "Bad magic number for file header"。
  * 这里改用**两遍式求解**：
        偏移 = 固定项(条目的数据区偏移) + 块长
    反复用「上一轮块长」重算偏移并重建（确定性算法，结果只由输入决定），
    直到块长不再变化（实测 2 轮收敛）。
"""
import base64
import hashlib
import io
import os
import struct
import sys
import zlib

SIG_BLOCK_MAGIC = b"APK Sig Block 42"
V2_BLOCK_ID = 0x7109871A
CHUNK = 1024 * 1024
RSA_SHA256 = 0x0103

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ziputil  # noqa: E402


# --------------------------------------------------------------------------- #
# key
# --------------------------------------------------------------------------- #
def load_key(path, password):
    from cryptography.hazmat.primitives.serialization import Encoding, pkcs12
    data = open(path, "rb").read()
    key = cert = extra = None
    for pw in (password.encode() if password else None, None, b""):
        try:
            key, cert, extra = pkcs12.load_key_and_certificates(data, pw)
            break
        except Exception:
            continue
    if key is None or cert is None:
        raise SystemExit("keystore has no private key/certificate: " + path)
    chain = [cert.public_bytes(Encoding.DER)] + [c.public_bytes(Encoding.DER) for c in (extra or [])]
    return key, chain


# --------------------------------------------------------------------------- #
# v1（JAR 签名）
# --------------------------------------------------------------------------- #
def _wrap(line):
    out = [line[:70]]
    rest = line[70:]
    while rest:
        out.append(" " + rest[:69])
        rest = rest[69:]
    return "".join(p + "\r\n" for p in out)


def _attrs(pairs):
    return "".join(_wrap("%s: %s" % (k, v)) for k, v in pairs).encode("utf-8")


def _sha(data):
    return base64.b64encode(hashlib.sha256(data).digest()).decode()


def build_v1(entries, key, chain):
    mf = bytearray(b"Manifest-Version: 1.0\r\nCreated-By: PADHelper\r\n\r\n")
    sections = []
    for name, data in entries:
        sec = _attrs([("Name", name), ("SHA-256-Digest", _sha(data))])
        sections.append((name, sec))
        mf += sec + b"\r\n"
    sf = bytearray(b"Signature-Version: 1.0\r\nCreated-By: PADHelper\r\n")
    sf += _attrs([("SHA-256-Digest-Manifest", _sha(bytes(mf)))]) + b"\r\n"
    for name, sec in sections:
        sf += _attrs([("Name", name), ("SHA-256-Digest", _sha(sec))]) + b"\r\n"

    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.serialization import pkcs7
    cert = x509.load_der_x509_certificate(chain[0])
    p7 = (pkcs7.PKCS7SignatureBuilder()
          .set_data(bytes(sf))
          .add_signer(cert, key, hashes.SHA256())
          .sign(serialization.Encoding.DER,
                [pkcs7.PKCS7Options.DetachedSignature, pkcs7.PKCS7Options.Binary]))
    return [("META-INF/MANIFEST.MF", bytes(mf)),
            ("META-INF/CERT.SF", bytes(sf)),
            ("META-INF/CERT.RSA", p7)]


# --------------------------------------------------------------------------- #
# v2（签名块）
# --------------------------------------------------------------------------- #
def _seq(*chunks):
    body = b"".join(struct.pack("<I", len(c)) + c for c in chunks)
    return struct.pack("<I", len(body)) + body


def content_digest(content):
    digests = []
    for start in range(0, len(content), CHUNK):
        chunk = content[start:start + CHUNK]
        digests.append(hashlib.sha256(struct.pack("<BI", 0xA5, len(chunk)) + chunk).digest())
    return hashlib.sha256(struct.pack("<BI", 0x5A, len(digests)) + b"".join(digests)).digest()


def build_v2_block(key, chain, content):
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import padding

    digests = _seq(_seq(struct.pack("<I", RSA_SHA256), content_digest(content)))
    certs = _seq(*chain)
    signed_data = _seq(digests, certs, _seq())
    signature = key.sign(signed_data, padding.PKCS1v15(), hashes.SHA256())
    signatures = _seq(_seq(struct.pack("<I", RSA_SHA256), signature))
    pub = x509.load_der_x509_certificate(chain[0]).public_key().public_bytes(
        serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    signer = _seq(signed_data, signatures, pub)

    pair = struct.pack("<I", V2_BLOCK_ID) + _seq(signer)
    pairs = struct.pack("<Q", len(pair)) + pair
    size = 8 + len(pairs) + 8
    block = struct.pack("<Q", size) + pairs + struct.pack("<Q", size) + SIG_BLOCK_MAGIC
    assert len(block) == size + 16, (len(block), size)
    return block


# --------------------------------------------------------------------------- #
# 布局
# --------------------------------------------------------------------------- #
def write_entries(entries):
    """写出条目数据区，返回 (content, [条目信息])；条目信息里的 offset 是它自己的位置。"""
    body = io.BytesIO()
    infos = []
    for name, data in entries:
        comp = zlib.compress(data, 9)[2:-4]
        crc = zlib.crc32(data) & 0xFFFFFFFF
        nb = name.encode("utf-8")
        offset = body.tell()
        body.write(ziputil.local_header(nb, len(comp), len(data), crc))
        body.write(comp)
        infos.append({"name": nb, "csize": len(comp), "usize": len(data),
                      "crc": crc, "offset": offset})
    return body.getvalue(), infos


def assemble(content, infos, block):
    """条目数据 + 签名块 + 按真实位置重写的中央目录 + EOCD。"""
    shift = len(block)
    central = bytearray()
    for info in infos:
        central += ziputil.central_entry(info["name"], info["csize"], info["usize"],
                                         info["crc"], info["offset"] + shift)
    eocd = ziputil.eocd_record(len(infos), len(central), len(content) + shift)
    return b"".join((content, block, bytes(central), eocd))


def sign(src, dst, keystore, password):
    key, chain = load_key(keystore, password)

    src_entries = [(n, d) for n, d in ziputil.read_zip(src) if not n.startswith("META-INF/")]
    all_entries = src_entries + build_v1(src_entries, key, chain)
    content, infos = write_entries(all_entries)

    # 两遍式求解：块长 -> 偏移 -> 块长 … 直到块长不再变化
    block = build_v2_block(key, chain, content)
    for _ in range(8):
        trial = assemble(content, infos, block)
        new_block = build_v2_block(key, chain, content)
        if len(new_block) == len(block):
            blob = trial
            break
        block = new_block
    else:
        raise SystemExit("signing block length did not converge")

    ziputil.read_zip(blob)     # 结构自检（偏移、CRC、目录长度）
    with open(dst, "wb") as fh:
        fh.write(blob)


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        sys.exit(2)
    sign(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4])


if __name__ == "__main__":
    main()
