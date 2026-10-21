#!/usr/bin/env python3
"""手写 zip 封装：不依赖 Python 的 zipfile，避免平台差异导致的压缩流损坏。

zipfile 的 writestr 会走 zlib 压缩，不同平台/版本对块大小、拼接口的处理略有差异，
实测在 macOS 系统 Python 3.9 上出现过「中央目录长度与实际数据不一致 / 压缩流损坏」。
这里只调用 `zlib.compressobj(level, DEFLATED, -15)`（裸 deflate），再按 ZIP 规范逐字节
拼装 local header / 中央目录 / EOCD，完全确定、可自检。

核心难点是 **格式对齐**：
  local file header  : 30 + nlen + elen + csize
  central directory  : 46 + nlen + elen + clen
其中 csize/clen 记录的都是**包含自身字段在内的总长度**，所以填字段时要先把其余部分算好。
"""
import struct
import zlib

LOCAL_SIG = b"PK\x03\x04"
CENTRAL_SIG = b"PK\x01\x02"
EOCD_SIG = b"PK\x05\x06"
DOS_TIME = (0 << 11) | (0 << 5) | 0
DOS_DATE = ((2024 - 1980) << 9) | (1 << 5) | 1


def _deflate(data):
    """裸 deflate（wbits=-15）。用一次性 compress()，避免流对象状态的平台差异。"""
    return zlib.compress(bytes(data), 9)[2:-4]


def local_header(name_bytes, csize, usize, crc):
    """公开的 local header 生成函数（签名器逐步写出时用）。"""
    return _local_header(name_bytes, csize, usize, crc)


def _local_header(name_bytes, csize, usize, crc):
    return (LOCAL_SIG + struct.pack("<HHHHHIIIHH", 20, 0, 8, DOS_TIME, DOS_DATE,
                                    crc, csize, usize, len(name_bytes), 0) + name_bytes)


def _central_entry(name_bytes, csize, usize, crc, offset):
    """中央目录记录 = 46 字节固定头 + 文件名。

    固定头字段（ZIP APPNOTE 4.3.12）：
        signature(4) | version made by(2) | version needed(2) | flags(2) | method(2)
        | time(2) | date(2) | crc(4) | csize(4) | usize(4)
        | name len(2) | extra len(2) | comment len(2) | disk start(2)
        | internal attrs(2) | external attrs(4) | local header offset(4)
    共 1 + 6 + 3 + 6 fields = 17 个字段，格式串必须与之逐项对应。
    """
    fixed = struct.pack("<I", 0x02014B50)
    fixed += struct.pack("<HHHHHH", 20, 20, 0, 8, DOS_TIME, DOS_DATE)
    fixed += struct.pack("<III", crc, csize, usize)
    fixed += struct.pack("<HHHHH", len(name_bytes), 0, 0, 0, 0)
    fixed += struct.pack("<I", 0o644 << 16)
    fixed += struct.pack("<I", offset)
    assert len(fixed) == 46, len(fixed)
    return fixed + name_bytes


def central_entry(name_bytes, csize, usize, crc, offset):
    """单条中央目录记录（公开版本，签名器重建目录时用）。"""
    return _central_entry(name_bytes, csize, usize, crc, offset)


def eocd_record(count, cd_size, cd_offset):
    """EOCD 记录：sig | 盘号 | 中央目录起始盘号 | 本条记录数 | 总记录数 | 大小 | 偏移 | 注释长度。"""
    return EOCD_SIG + struct.pack("<HHHHIIH", 0, 0, count, count, cd_size, cd_offset, 0)


def entries_len(blob):
    """数据区长度：从 0 开始走 local header，走到中央目录起点。"""
    eocd = blob.rfind(EOCD_SIG)
    cd_off = struct.unpack_from("<I", blob, eocd + 16)[0]
    pos = 0
    while pos + 30 <= cd_off and blob[pos:pos + 4] == LOCAL_SIG:
        nlen, elen = struct.unpack_from("<HH", blob, pos + 26)
        csize = struct.unpack_from("<I", blob, pos + 18)[0]
        pos += 30 + nlen + elen + csize
    return pos


def local_info(blob, offset):
    """读出一个 local header 的关键字段（名字、压缩/解压大小、CRC、结束位置）。"""
    if blob[offset:offset + 4] != LOCAL_SIG:
        raise ValueError("偏移 %d 不是 local header" % offset)
    crc, csize, usize = struct.unpack_from("<III", blob, offset + 14)
    nlen, elen = struct.unpack_from("<HH", blob, offset + 26)
    name = blob[offset + 30:offset + 30 + nlen].decode("utf-8")
    return {"offset": offset, "name": name, "crc": crc, "csize": csize,
            "usize": usize, "end": offset + 30 + nlen + elen + csize}


def central_records(blob, cd_off=None):
    """返回 ([(记录偏移, local header 偏移字段)], EOCD 中中央目录偏移字段的位置)。

    cd_off 默认取 EOCD 里记录的值；处理「只是中央目录+EOCD 的片段」时需要外部传入（通常是 0）。
    """
    eocd = blob.rfind(EOCD_SIG)
    count, _cd_size, recorded_cd = struct.unpack_from("<HII", blob, eocd + 10)
    pos = recorded_cd if cd_off is None else cd_off
    out = []
    for _ in range(count):
        if blob[pos:pos + 4] != CENTRAL_SIG:
            raise ValueError("中央目录记录损坏（偏移 %d）" % pos)
        nlen, elen, clen = struct.unpack_from("<HHH", blob, pos + 28)
        out.append((pos, pos + 42))
        pos += 46 + nlen + elen + clen
    return out, eocd + 16


def write_zip(path_or_buffer, entries):
    """entries: [(name, bytes)]，按顺序写入；返回最终字节长度。

    先把每个条目（local header + 数据）拼好，同时记下它在文件中的偏移，
    最后一次性写出数据区 + 中央目录 + EOCD。
    """
    body = bytearray()
    central = bytearray()
    for name, data in entries:
        nb = name.encode("utf-8")
        comp = _deflate(data)
        crc = zlib.crc32(data) & 0xFFFFFFFF
        offset = len(body)
        body += _local_header(nb, len(comp), len(data), crc) + comp
        central += _central_entry(nb, len(comp), len(data), crc, offset)

    cd_offset = len(body)
    # EOCD: sig(4) | 盘号(2) | 中央目录起始盘号(2) | 本条记录数(2) | 总记录数(2)
    #       | 中央目录大小(4) | 中央目录偏移(4) | 注释长度(2)
    eocd = (EOCD_SIG + struct.pack("<HHHHIIH", 0, 0, len(entries), len(entries),
                                   len(central), cd_offset, 0))
    blob = bytes(body) + bytes(central) + eocd

    if hasattr(path_or_buffer, "write"):
        path_or_buffer.write(blob)
    else:
        with open(path_or_buffer, "wb") as fh:
            fh.write(blob)
    return blob


def read_zip(blob_or_path):
    """按中央目录顺序读回 [(name, data)]，只支持本模块会产生的 zip（不加密、deflate/store）。"""
    if isinstance(blob_or_path, (bytes, bytearray)):
        blob = bytes(blob_or_path)
    else:
        blob = open(blob_or_path, "rb").read()

    eocd = blob.rfind(EOCD_SIG)
    if eocd < 0:
        raise ValueError("找不到 EOCD")
    count, cd_size, cd_off = struct.unpack_from("<HII", blob, eocd + 10)

    out = []
    pos = cd_off
    for _ in range(count):
        if blob[pos:pos + 4] != CENTRAL_SIG:
            raise ValueError("中央目录记录损坏（偏移 %d）" % pos)
        (method,) = struct.unpack_from("<H", blob, pos + 10)
        crc, csize, usize = struct.unpack_from("<III", blob, pos + 16)
        nlen, elen, clen = struct.unpack_from("<HHH", blob, pos + 28)
        offset = struct.unpack_from("<I", blob, pos + 42)[0]
        name = blob[pos + 46:pos + 46 + nlen].decode("utf-8")

        if blob[offset:offset + 4] != LOCAL_SIG:
            raise ValueError("条目 %r 的偏移 %d 不是 local header" % (name, offset))
        if name != blob[offset + 30:offset + 30 + struct.unpack_from("<H", blob, offset + 26)[0]].decode():
            raise ValueError("条目 %r 的偏移 %d 指向了别的条目" % (name, offset))
        lnlen, lelen = struct.unpack_from("<HH", blob, offset + 26)
        start = offset + 30 + lnlen + lelen
        raw = blob[start:start + csize]
        data = zlib.decompress(raw, -15) if method == 8 else raw
        if (zlib.crc32(data) & 0xFFFFFFFF) != crc:
            raise ValueError("条目 %r CRC 校验失败" % name)
        out.append((name, data))
        pos += 46 + nlen + elen + clen

    if pos != eocd:
        raise ValueError("中央目录长度与实际不符（结束于 %d，EOCD 在 %d）" % (pos, eocd))
    if len(out) != count:
        raise ValueError("条目数量不符")
    return out
