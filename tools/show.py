#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
打印源码（自动识别 GBK/UTF-8），带行号 —— 供审计时阅读 GBK 文件用。

read 工具只认 UTF-8，而本工程 User/ 下是 GBK；用这个脚本代替 cat。

用法:
    python tools/show.py User/BSP/flash.c                 # 全文
    python tools/show.py User/BSP/flash.c 100 160         # 只看 100~160 行
    python tools/show.py User/BSP/flash.c grep 某个关键字  # 只打含关键字的行
"""
import os
import sys


def load(path):
    raw = open(path, "rb").read()
    for enc in ("utf-8", "gbk"):
        try:
            return raw.decode(enc), enc
        except UnicodeDecodeError:
            continue
    return raw.decode("gbk", "replace"), "gbk(有损坏)"


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    path = sys.argv[1]
    if not os.path.isabs(path):
        path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), path)
    text, enc = load(path)
    lines = text.split("\n")

    if len(sys.argv) >= 4 and sys.argv[2] == "grep":
        kw = sys.argv[3]
        for i, l in enumerate(lines, 1):
            if kw in l:
                print("%4d| %s" % (i, l.rstrip("\r")))
        return 0

    if len(sys.argv) >= 4:
        a, b = int(sys.argv[2]), int(sys.argv[3])
    else:
        a, b = 1, len(lines)
    a = max(1, a)
    b = min(len(lines), b)
    print("# %s  [%s]  共 %d 行，显示 %d-%d" % (os.path.basename(path), enc, len(lines), a, b))
    for i in range(a, b + 1):
        print("%4d| %s" % (i, lines[i - 1].rstrip("\r")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
