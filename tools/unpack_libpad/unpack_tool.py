#!/usr/bin/env python3
"""
unpack_tool.py —— libpad.so / lib__6dba__.so 加壳格式解析与 stage-1 提取

已完全验证的部分：
  1. 定位 payload（sh_type == 0x80000000 的节）
  2. 解密 8 字包头（自密钥）
  3. 解密并提取 stage-1 加载器（0x25970 字节）

尚未打通：
  4. payload 剩余 ~12.1MB 由 stage-1 内的 VM 解释器解密，格式未知

用法：
  python3 unpack_tool.py <目标.so> [-o 输出目录]
"""
import argparse
import hashlib
import os
import struct

MUL = 0xBF20165D           # 混淆乘法常量
MASK = 0xFFFFFFFF
SECTION_TYPE = 0x80000000  # LOUSER+0：加壳器私有节
PAYLOAD_SKIP = 0x23C       # 节起点到 payload 的固定间隔


def decrypt_words(key, data, nwords, start=0):
    """加壳器的流变换（对 32 位字逐字处理，位置相关）。

        p[i] = (c[i] + (i+3)*key) ^ ((i+1)*MUL)      (mod 2^32)

    注意：这不是可交换的简单异或，索引 i 参与运算，
    所以必须从流起点开始、按顺序逐字处理。
    """
    out = bytearray(data)
    for i in range(start, nwords):
        c = struct.unpack_from('<I', out, i * 4)[0]
        p = ((c + (((i + 3) * key) & MASK)) & MASK) ^ (((i + 1) * MUL) & MASK)
        struct.pack_into('<I', out, i * 4, p)
    return out


def load_elf(path):
    d = open(path, 'rb').read()
    if d[:4] != b'\x7fELF':
        raise SystemExit('不是 ELF 文件')
    e_shoff = struct.unpack_from('<Q', d, 0x28)[0]
    e_shentsize = struct.unpack_from('<H', d, 0x3a)[0]
    e_shnum = struct.unpack_from('<H', d, 0x3c)[0]
    e_phoff = struct.unpack_from('<Q', d, 0x20)[0]
    e_phnum = struct.unpack_from('<H', d, 0x38)[0]

    loads = []
    for i in range(e_phnum):
        o = e_phoff + i * 56
        typ, _flags, off, vaddr, _paddr, filesz, _memsz, _align = \
            struct.unpack_from('<IIQQQQQQ', d, o)
        if typ == 1:  # PT_LOAD
            loads.append((vaddr, vaddr + filesz, off - vaddr))

    secs = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        if o + 64 > len(d):
            break
        _name, typ, _flags, addr, off, size, _link, _info, _al, _ent = \
            struct.unpack_from('<IIQQQQIIQQ', d, o)
        secs.append({'idx': i, 'type': typ, 'addr': addr, 'off': off, 'size': size})
    return d, loads, secs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('target')
    ap.add_argument('-o', '--outdir', default=None)
    args = ap.parse_args()

    d, loads, secs = load_elf(args.target)
    name = os.path.basename(args.target)
    outdir = args.outdir or os.path.join(os.path.dirname(os.path.abspath(args.target)), 'unpacked')
    os.makedirs(outdir, exist_ok=True)

    print('== %s ==' % name)
    print('   文件大小 0x%x  sha256 %s' % (len(d), hashlib.sha256(d).hexdigest()[:16]))
    print('   PT_LOAD 数量 %d  末段结束 0x%x' % (len(loads), max(e for _s, e, _d in loads)))

    priv = [s for s in secs if s['type'] == SECTION_TYPE]
    if not priv:
        raise SystemExit('   未找到 0x80000000 私有节 —— 该文件可能未加壳')
    sec = priv[-1]
    payload_off = sec['off'] + PAYLOAD_SKIP
    payload_size = sec['size'] - PAYLOAD_SKIP
    print('   私有节 #%d  sh_offset=0x%x  sh_size=0x%x' % (sec['idx'], sec['off'], sec['size']))
    print('   payload     0x%x  大小 0x%x' % (payload_off, payload_size))

    # --- 解密 8 字包头 ---
    key = struct.unpack_from('<I', d, payload_off)[0]
    hdr = decrypt_words(key, d[payload_off:payload_off + 0x1000], 8, 0)
    struct.pack_into('<I', hdr, 0, key)  # 第 0 字被还原为密钥本身
    w = struct.unpack_from('<8I', hdr, 0)
    body_off, body_size, body_key, entry, mprot_len = w[2], w[3], w[4], w[5], w[6]

    print('   包头  key=0x%04x  body_off=0x%x  body_size=0x%x' % (key, body_off, body_size))
    print('         body_key=0x%04x  entry=0x%x  mprotect_len=0x%x' % (body_key, entry, mprot_len))
    print('   剩余加密区 0x%x 字节（VM 处理，未解）' % (payload_size - body_off - body_size))

    # --- 解密 stage-1 ---
    body = decrypt_words(body_key,
                         d[payload_off + body_off: payload_off + body_off + body_size],
                         body_size // 4, 0)
    p1 = os.path.join(outdir, name + '.stage1.bin')
    open(p1, 'wb').write(body)
    print('   -> stage-1  %s' % p1)

    # --- 包成可分析的 ELF ---
    eh = b'\x7fELF' + bytes([2, 1, 1, 0]) + b'\x00' * 8
    eh += struct.pack('<HHIQQQIHHHHHH', 3, 183, 1, entry, 64, 0, 0, 64, 56, 1, 64, 0, 0)
    ph = struct.pack('<IIQQQQQQ', 1, 5, 0x1000, 0, 0, len(body), len(body), 0x1000)
    pad = b'\x00' * (0x1000 - 64 - 56)
    p2 = os.path.join(outdir, name + '.stage1.elf')
    open(p2, 'wb').write(eh + ph + pad + body)
    print('   -> stage-1 ELF %s（vaddr 与 stage1.bin 偏移 1:1 对应）' % p2)


if __name__ == '__main__':
    main()
