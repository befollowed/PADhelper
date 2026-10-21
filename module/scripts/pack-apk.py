#!/usr/bin/env python3
"""把一个目录里的若干文件按给定顺序打包成 zip。

用法: pack-apk.py <输出apk> <源目录> <条目名>...
条目名同时作为 zip 内路径和源目录下的文件名。打包后立刻读回校验。

为什么不直接用 zip/zipfile：macOS 自带 Python 3.9 的 zipfile 与系统 zip 命令
在多文件追加时会产出「中央目录长度与实际数据不一致」的包，
安装时报 Bad magic number for file header；ziputil 是确定式实现，可自检。
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ziputil  # noqa: E402


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(2)
    out, stage, names = sys.argv[1], sys.argv[2], sys.argv[3:]
    entries = []
    for name in names:
        with open(os.path.join(stage, name), "rb") as fh:
            entries.append((name, fh.read()))
    ziputil.write_zip(out, entries)

    back = ziputil.read_zip(out)
    if [n for n, _ in back] != names:
        raise SystemExit("条目顺序不一致: %r" % ([n for n, _ in back],))
    for (name, data), (_, again) in zip(entries, back):
        if data != again:
            raise SystemExit("条目 %s 回读内容不一致" % name)
    print("%s: %d bytes, %d entries, roundtrip OK"
          % (os.path.basename(out), os.path.getsize(out), len(entries)))


if __name__ == "__main__":
    main()
